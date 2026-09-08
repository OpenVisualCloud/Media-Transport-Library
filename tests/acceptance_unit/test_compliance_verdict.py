# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""How a capture EBU LIST dropped is told apart from one that held no streams.

The acceptance tree imports five packages only its venv installs. Stand them
in, since none is reached by the code under test, and import the real modules:
a stub of the code itself would not notice a rename or a changed signature.
"""

import logging
import sys
import types
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.append(str(Path(__file__).resolve().parents[1] / "acceptance"))
for _name in (
    "mfd_common_libs",
    "mfd_common_libs.log_levels",
    "mfd_connect",
    "mfd_connect.exceptions",
    "pytest",
    "pytest_check",
    "requests",
):
    sys.modules.setdefault(_name, types.ModuleType(_name))
# The only two attributes reached at import time. The exception class has to be
# a real one: pcap_compliance catches it around the pcap removal.
sys.modules["mfd_common_libs.log_levels"].TEST_FAIL = 41
sys.modules["mfd_connect.exceptions"].ConnectionCalledProcessError = OSError
sys.modules["pytest_check"].check = Mock()

from compliance.compliance_client import (  # noqa: E402
    REANALYSIS_TIMEOUT,
    PcapComplianceClient,
    no_verdict_reason,
)
from mtl_engine import pcap_compliance  # noqa: E402
from mtl_engine.pcap_compliance import (  # noqa: E402
    _ANALYSIS_ATTEMPTS,
    ComplianceSession,
)

# The code under test warns about the very reports these cases feed it. Without
# a handler those records reach logging.lastResort, so a passing run prints
# "found no streams"/"non-compliance" into the CI log.
for _module in (pcap_compliance, sys.modules["compliance.compliance_client"]):
    _module.logger.addHandler(logging.NullHandler())

EBU_IP = "10.0.0.1"
PCAP_ID = "0e5b5089"
PCAP_FILE = "/mnt/ramdisk/pcap/capture.pcap"
PASSWORD = "s3cret"

# An EBU LIST report for a capture it analyzed but found nothing in. Note
# not_compliant_streams == 0: read as an ordinary verdict this says "compliant".
EMPTY_REPORT = {"analyzed": True, "not_compliant_streams": 0, "streams": []}
COMPLIANT_REPORT = {
    "analyzed": True,
    "not_compliant_streams": 0,
    "streams": [{"media_type": "video"}],
}


class VerdictTests(unittest.TestCase):
    def verdict(self, report):
        return PcapComplianceClient.check_compliance(None, report)[0]

    def test_report_states_are_distinct(self):
        self.assertIsNone(self.verdict(False), "unavailable report has no verdict")
        self.assertIsNone(
            self.verdict(EMPTY_REPORT), "no streams is no verdict, not a compliant one"
        )
        self.assertIs(self.verdict(COMPLIANT_REPORT), True)
        self.assertIs(
            self.verdict({**COMPLIANT_REPORT, "not_compliant_streams": 1}), False
        )

    def test_the_two_no_verdict_faults_read_differently(self):
        empty = no_verdict_reason(EMPTY_REPORT)
        self.assertIn("no ST 2110 streams", empty)
        self.assertNotEqual(empty, no_verdict_reason(False))
        # A report omitting the key entirely, not just one with an empty list.
        self.assertEqual(no_verdict_reason({"analyzed": True}), empty)
        self.assertIsNone(no_verdict_reason(COMPLIANT_REPORT))


class ReanalyzeWaitTests(unittest.TestCase):
    """The re-analysis has to wait for its own report, not the one it replaces."""

    # A dropped ingest leaves capture_date at 0; a real analysis fills it in.
    DROPPED = {"analyzed": True, "capture_date": 0, "streams": []}
    FRESH = {"analyzed": True, "capture_date": 1788796269053, "streams": [{}]}

    def setUp(self):
        self.slept = patch("time.sleep").start()
        self.addCleanup(patch.stopall)

    def reanalyze(self, *polls):
        polls = list(polls)
        self.client = object.__new__(PcapComplianceClient)
        self.client.__dict__.update(
            pcap_id=PCAP_ID,
            ebu_ip=EBU_IP,
            token="t",
            proxies={},
            session=Mock(),
            download_report=lambda: polls.pop(0),
        )
        return self.client.reanalyze(self.DROPPED)

    def test_the_stale_report_is_not_mistaken_for_the_new_one(self):
        # The pre-processor had not cleared `analyzed` yet, so the first poll
        # still answers with the report being replaced.
        self.assertIs(
            self.reanalyze(self.DROPPED, self.DROPPED, self.FRESH), self.FRESH
        )
        self.assertEqual(self.slept.call_count, 2)

    def test_a_report_that_is_already_fresh_is_not_waited_on(self):
        self.assertIs(self.reanalyze(self.FRESH), self.FRESH)
        self.slept.assert_not_called()

    def test_the_capture_is_re_analysed_in_place(self):
        # The whole point is that the analyser already holds the bytes: a fresh
        # ingest would transfer a multi-gigabyte capture a second time.
        self.reanalyze(self.FRESH)
        self.client.session.put.assert_called_once_with(
            f"http://{EBU_IP}/api/pcap/{PCAP_ID}/reanalyze",
            headers={"Authorization": "Bearer t"},
            verify=False,
            proxies={},
        )
        self.client.session.post.assert_not_called()

    def test_a_poll_that_fails_mid_wait_ends_the_wait(self):
        # download_report() answers False when the analyser is unreachable;
        # treating that as a report would raise AttributeError on .get().
        self.assertIs(self.reanalyze(self.DROPPED, False), False)

    def test_a_capture_that_stays_empty_returns_the_empty_report(self):
        # A genuinely empty capture never gets a capture_date, and the empty
        # report is the right answer -- waiting must not turn it into "no
        # report", which reads as a down analyser instead.
        self.assertIs(
            self.reanalyze(*[self.DROPPED] * (REANALYSIS_TIMEOUT + 1)), self.DROPPED
        )
        self.assertEqual(self.slept.call_count, REANALYSIS_TIMEOUT)


class _Analyser:
    """Stand in for the EBU LIST client, serving queued reports in order."""

    check_compliance = PcapComplianceClient.check_compliance

    def __init__(self, reports):
        self.reports = list(reports)
        self.reanalyses = 0

    def download_report(self):
        return self.reports.pop(0)

    def reanalyze(self, previous=None):
        self.reanalyses += 1
        return self.download_report()


class FetchReportTests(unittest.TestCase):
    def setUp(self):
        # _fail() reports through pytest_check and the CSV report; pytest_check
        # would turn every failure these cases expect into a real one.
        patch.object(pcap_compliance, "log_fail", Mock()).start()
        patch.object(pcap_compliance, "update_compliance_result", Mock()).start()
        self.addCleanup(patch.stopall)
        self.connection = Mock()
        self.connection.execute_command.return_value = SimpleNamespace(
            return_code=0, stdout=f">>>UUID: {PCAP_ID}\n", stderr=""
        )
        self.session = object.__new__(ComplianceSession)
        self.session.__dict__.update(
            _recorder=SimpleNamespace(
                pcap_file=PCAP_FILE,
                host=SimpleNamespace(connection=self.connection),
            ),
            ebu_server={"ebu_ip": EBU_IP, "user": "gta", "password": PASSWORD},
            mtl_path="/repo",
            node_id="test_compliance_verdict",
        )

    def fetch(self, *reports):
        self.analyser = _Analyser(reports)
        patch.object(
            pcap_compliance, "PcapComplianceClient", lambda **kwargs: self.analyser
        ).start()
        return self.session._fetch_report(True)

    def commands(self):
        return [call.args[0] for call in self.connection.execute_command.call_args_list]

    def test_a_dropped_ingest_is_recovered_by_re_analysing_it(self):
        # The empty first report was the analyser losing the capture, not the
        # capture being empty, so the file it already holds analyses fine.
        self.assertIs(self.fetch(EMPTY_REPORT, COMPLIANT_REPORT), COMPLIANT_REPORT)
        self.assertEqual(self.analyser.reanalyses, 1)
        # The recovery must not re-transfer the capture: these run over SSH and
        # a 4K st20p pcap is gigabytes.
        self.assertEqual(sum("upload_pcap.py" in c for c in self.commands()), 1)

    def test_a_compliant_first_analysis_is_not_repeated(self):
        self.assertIs(self.fetch(COMPLIANT_REPORT), COMPLIANT_REPORT)
        self.assertEqual(self.analyser.reanalyses, 0)

    def test_a_genuinely_empty_capture_is_named_as_the_cause(self):
        with self.assertRaises(AssertionError) as raised:
            self.fetch(*[EMPTY_REPORT] * _ANALYSIS_ATTEMPTS)
        self.assertIn("no ST 2110 streams", str(raised.exception))
        self.assertNotIn("non-compliance", str(raised.exception))
        self.assertEqual(self.analyser.reanalyses, _ANALYSIS_ATTEMPTS - 1)

    def test_an_unavailable_report_is_not_re_analysed(self):
        # Nothing was analyzed at all, so re-analysing the same file cannot
        # help -- unlike the analyzed-but-empty report above.
        with self.assertRaisesRegex(AssertionError, PCAP_ID):
            self.fetch(*[False] * _ANALYSIS_ATTEMPTS)
        self.assertEqual(self.analyser.reanalyses, 0)

    def test_the_root_owned_pcap_is_removed_after_a_failed_verdict(self):
        with self.assertRaises(AssertionError):
            self.fetch(*[EMPTY_REPORT] * _ANALYSIS_ATTEMPTS)
        self.assertIn(f"sudo rm -f '{PCAP_FILE}'", self.commands())

    def test_the_password_stays_out_of_the_argument_list(self):
        # mfd_connect logs every command at CMD level, and those logs are the
        # pytest artifact of a public CI run.
        self.fetch(COMPLIANT_REPORT)
        self.assertEqual([c for c in self.commands() if PASSWORD in c], [])
        upload = self.connection.execute_command.call_args_list[0]
        self.assertIn("--password-stdin", upload.args[0])
        self.assertEqual(upload.kwargs["input_data"], f"{PASSWORD}\n")


if __name__ == "__main__":
    unittest.main()
