# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""Which captures may yield an ST 2110-21 verdict, and what a failure blames.

Three states have to stay distinct: a capture EBU LIST dropped on ingest, one
it analysed but found no streams in, and one it analysed that is missing
packets of the stream it claims to describe.

The acceptance tree imports five packages only its venv installs. Stand them
in, since none is reached by the code under test, and import the real modules:
a stub of the code itself would not notice a rename or a changed signature.
"""

import ast
import logging
import sys
import types
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
CONFTEST = ROOT / "tests/acceptance/conftest.py"
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
    CaptureIntent,
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

# Both verbatim from the nightly under analysis: a 2160p119 capture EBU LIST
# could not classify at all -- no timing analysis, only the dropped-packet
# counts -- and one it did reach a "narrow" verdict on with 435 packets of the
# stream missing from the file it read them off.
LOSSY_UNCLASSIFIED_REPORT = {
    "analyzed": True,
    "not_compliant_streams": 1,
    "streams": [
        {
            "media_type": "unknown",
            "statistics": {"dropped_packet_count": 47801, "packet_count": 65832},
        }
    ],
}
LOSSY_NARROW_REPORT = {
    "analyzed": True,
    "not_compliant_streams": 0,
    "streams": [
        {
            "media_type": "video",
            "statistics": {"dropped_packet_count": 435, "packet_count": 65832},
        }
    ],
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


class _FetchHarness:
    """Drives ``ComplianceSession._fetch_report`` against queued reports.

    Not a TestCase itself, so the cases below inherit the harness without
    unittest collecting -- and re-running -- each other's tests.
    """

    def setUp(self):
        # _fail() reports through pytest_check and the CSV report; pytest_check
        # would turn every failure these cases expect into a real one.
        patch.object(pcap_compliance, "log_fail", Mock()).start()
        self.recorded = patch.object(
            pcap_compliance, "update_compliance_result", Mock()
        ).start()
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


class FetchReportTests(_FetchHarness, unittest.TestCase):
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


def _conftest_compliance_failed(recorded):
    """Run conftest's own ``compliance_failed`` against a recorded cell value.

    It is a closure inside the ``log_case`` fixture and conftest imports pytest,
    so exec just that function with its two free names supplied. Reproducing the
    predicate here instead would test this file against itself.
    """
    node = next(
        n
        for n in ast.walk(ast.parse(CONFTEST.read_text()))
        if isinstance(n, ast.FunctionDef) and n.name == "compliance_failed"
    )
    scope = {"case_id": "node_id", "get_compliance_result": lambda _case: recorded}
    exec(
        compile(ast.Module(body=[node], type_ignores=[]), str(CONFTEST), "exec"), scope
    )
    return scope["compliance_failed"]()


class LossyCaptureTests(_FetchHarness, unittest.TestCase):
    """A capture missing packets of its own fails, as the capture's failure.

    VRX and Cinst are computed from the intervals between the packets in the
    pcap, so a file missing some of them measures a stream that was never sent.
    Six 2160p119 cases in one nightly were failed on captures missing 42-50% of
    their packets while MTL's own RX counters and frame integrity were clean,
    and reporting that as a stream defect sent three rounds of diagnosis after
    a transmitter that was working.
    """

    def message(self, *reports):
        with self.assertRaises(AssertionError) as raised:
            self.fetch(*reports)
        return str(raised.exception)

    def cell(self):
        """The Compliance cell this verdict wrote to the CSV report."""
        return self.recorded.call_args.args[1]

    def test_a_lossy_capture_fails_and_the_row_names_the_capture(self):
        # "Fail" alone reads as a stream defect, which is the misattribution
        # this gate exists to prevent; the percentage redirects to the pcap.
        self.assertIn("42.1%", self.message(LOSSY_UNCLASSIFIED_REPORT))
        self.assertEqual(self.cell(), "Fail (capture lost 42.1% of packets)")

    def test_a_lossy_capture_fails_even_when_ebu_reached_a_verdict(self):
        # "narrow" is not evidence the loss was harmless: it is a measurement
        # of a capture missing part of the stream it claims to describe, so it
        # is no more trustworthy than "not compliant" would be.
        self.message(LOSSY_NARROW_REPORT)
        self.assertEqual(self.cell(), "Fail (capture lost 0.7% of packets)")

    def test_a_lossless_non_compliance_still_fails_as_itself(self):
        # The loss gate must not swallow the verdict it precedes, or a real
        # pacing regression on a clean capture stops failing.
        self.message({**COMPLIANT_REPORT, "not_compliant_streams": 1})
        self.assertEqual(self.cell(), "Fail")

    def test_conftest_reads_the_recorded_row_back_as_a_failure(self):
        # The cell is not just display: conftest reads it back to choose the
        # failure wording, and on a soft fail -- where the call phase itself
        # passed -- to decide whether the case is reported failed at all. That
        # test was ``== "Fail"``, so naming the capture in the cell reported a
        # soft capture-loss failure as a pass. The prefix match must not widen
        # into "anything non-empty fails" either: the rest mean the opposite.
        for recorded, failed in (
            ("Fail", True),
            ("Fail (capture lost 42.1% of packets)", True),
            ("Pass", False),
            ("Pass (wide)", False),
            (None, False),
        ):
            with self.subTest(recorded=recorded):
                self.assertIs(_conftest_compliance_failed(recorded), failed)


class _Clock:
    """``time`` with no wall clock: only the sleeps under test move it."""

    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class ArmLivenessTests(unittest.TestCase):
    """arm() must not sniff a transmitter that is already gone.

    ``--pacing_way tsn`` fails mtl_init on the RX VF, so RxTxApp was gone at
    t+24s with return code 244; arming regardless spent 2m52s per case and then
    reported the empty capture as the headline instead of the return code.
    """

    def setUp(self):
        self.clock = _Clock()
        patch.object(pcap_compliance, "time", self.clock).start()
        self.addCleanup(patch.stopall)
        self.recorder = Mock(packets_capture=None)
        self.session = object.__new__(ComplianceSession)
        self.session.__dict__.update(
            _recorder=self.recorder, _skip_reason=None, _evaluated=False
        )
        # The production budget: ptp_wait 50s + settle 12s before 70s of capture.
        self.intent = CaptureIntent(
            dst_ips=("239.168.85.20",), capture_time=70, ptp_wait=50
        )

    def test_a_dead_process_is_not_captured(self):
        self.session.arm(self.intent, exit_code=lambda: 244)
        self.recorder.capture.assert_not_called()
        self.assertFalse(self.session.enabled)
        self.assertIn("244", self.session._skip_reason)
        self.assertEqual(self.clock.sleeps, [])

    def test_a_process_that_dies_mid_wait_ends_the_wait(self):
        codes = [None, None, 244]
        self.session.arm(self.intent, exit_code=lambda: codes.pop(0))
        self.recorder.capture.assert_not_called()
        self.assertLessEqual(
            sum(self.clock.sleeps), 2 * pcap_compliance._LIVENESS_POLL_INTERVAL
        )

    def test_a_live_process_still_gets_its_whole_budget(self):
        # No return code means "still running": a process slow to reach steady
        # state must not be mistaken for a dead one.
        self.session.arm(self.intent, exit_code=lambda: None)
        self.recorder.capture.assert_called_once_with(capture_time=70)
        self.assertEqual(sum(self.clock.sleeps), 62)
        self.assertLessEqual(
            max(self.clock.sleeps), pcap_compliance._LIVENESS_POLL_INTERVAL
        )

    def test_a_caller_without_a_handle_is_unaffected(self):
        # ffmpeg.py and gstreamer.py arm from after_last_start with no handle.
        self.session.arm(self.intent)
        self.recorder.capture.assert_called_once_with(capture_time=70)
        self.assertEqual(self.clock.sleeps, [50, 12])


if __name__ == "__main__":
    unittest.main()
