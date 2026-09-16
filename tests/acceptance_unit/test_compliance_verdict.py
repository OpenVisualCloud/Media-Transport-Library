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
EBU_USER = "ebu"
PCAP_ID = "0e5b5089"
PCAP_FILE = "/mnt/ramdisk/pcap/capture.pcap"
PASSWORD = "s3cret"


def _intent(framerate=None):
    """A ``CaptureIntent`` carrying only what the checks under test read."""
    return CaptureIntent(dst_ips=("239.0.0.1",), capture_time=1, framerate=framerate)


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
            ebu_server={"ebu_ip": EBU_IP, "user": EBU_USER, "password": PASSWORD},
            mtl_path="/repo",
            node_id="test_compliance_verdict",
        )

    def _serve(self, reports):
        self.analyser = _Analyser(reports)
        patch.object(
            pcap_compliance, "PcapComplianceClient", lambda **kwargs: self.analyser
        ).start()

    def fetch(self, *reports, framerate="p60"):
        """Fetch under *framerate*, by default one EBU LIST can name exactly.

        The rate matters to the verdict now: EBU LIST cannot express every rate
        MTL sends, and a verdict it derived from a rate it could not express is
        withheld rather than enforced (see :class:`AnalyserRateLimitTests`). A
        rate it can express keeps every case here on the enforcing path.
        """
        self._serve(reports)
        report, _ = self.session._fetch_report(True, _intent(framerate))
        return report

    def verdict(self, *reports, framerate="p60", allow_wide=False):
        """Drive the whole verdict: the fetch above plus the checks after it.

        The two halves have to be exercised together. Whether a verdict was
        withheld is decided in the fetch and acted on in the checks -- which
        assertion replaces it, which gate it disarms, and what the report row
        ends up saying -- so a case that stops at the fetch cannot see any of it.
        """
        self._serve(reports)
        self.session._verdict(
            _intent(framerate), allow_wide=allow_wide, fail_on_error=True
        )

    def commands(self):
        return [call.args[0] for call in self.connection.execute_command.call_args_list]

    def cell(self):
        """The Compliance cell this verdict wrote to the CSV report."""
        return self.recorded.call_args.args[1]


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
            ("Pass (2110-21 verdict withheld)", False),
            (None, False),
        ):
            with self.subTest(recorded=recorded):
                self.assertIs(_conftest_compliance_failed(recorded), failed)


# media_specific.rate and the analyses.inter_frame_rtp_ts_delta range EBU LIST
# 2.2.2 measured off eleven nightly captures, one per rate in the st20p matrix,
# each downloaded back from the analyser. p119 is the only row where the rate it
# reported is not the rate MTL sent: its table holds 24000/1001, 30000/1001 and
# 60000/1001 but no 120000/1001, so it substituted 180000/1501 (119.9201 fps)
# for 119.8801 and called the timing not_compliant on that basis -- while its own
# tick measurement, 750..751, is exactly right for 750.75 ticks per frame.
MEASURED = {
    "p23": ("24000/1001", 3753, 3754),
    "p24": (24, 3750, 3750),
    "p25": (25, 3600, 3600),
    "p29": ("30000/1001", 3003, 3003),
    "p30": (30, 3000, 3000),
    "p50": (50, 1800, 1800),
    "p59": ("60000/1001", 1501, 1502),
    "p60": (60, 1500, 1500),
    "p100": (100, 900, 900),
    "p119": ("180000/1501", 750, 751),
    "p120": (120, 750, 750),
}


def _report(
    rate,
    tick_min,
    tick_max,
    *,
    compliance="narrow",
    not_compliant=0,
    failed=(),
    dropped=0,
):
    """One analysed video stream. *failed* names the analyses EBU LIST did not pass.

    Per-analysis results are not decoration: ``not_compliant_streams`` counts
    streams and never says which analysis failed, so these are the only field
    that distinguishes a failure the substitute rate explains from one it does
    not.
    """

    def result(name):
        return "not_compliant" if name in failed else "compliant"

    return {
        "analyzed": True,
        "not_compliant_streams": not_compliant,
        "streams": [
            {
                "media_type": "video",
                "media_specific": {"rate": rate, "scan_type": "progressive"},
                "global_video_analysis": {"compliance": compliance},
                "analyses": {
                    "inter_frame_rtp_ts_delta": {
                        "details": {"range": {"min": tick_min, "max": tick_max}},
                        "result": result("inter_frame_rtp_ts_delta"),
                    },
                    "2110_21_vrx": {"result": result("2110_21_vrx")},
                    "rtp_sequence": {"result": result("rtp_sequence")},
                    "packet_ts_vs_rtp_ts": {"result": result("packet_ts_vs_rtp_ts")},
                },
                "statistics": {
                    "dropped_packet_count": dropped,
                    "packet_count": 16460,
                },
            }
        ],
    }


# The p119 capture exactly as it came back: analysed, lossless, its tick
# measurement right, and failed anyway -- on VRX alone, a curve computed at
# 119.9201 fps. Seven of the twelve lossless p119 captures on the analyser read
# exactly like this; the other five also failed inter_frame_rtp_ts_delta, whose
# limit comes from the same substituted rate.
P119_REPORT = _report(
    "180000/1501",
    750,
    751,
    compliance="not_compliant",
    not_compliant=1,
    failed=("2110_21_vrx",),
)


class RateAgreementTests(unittest.TestCase):
    """No rate in the matrix may read as a mismatch, and neighbours still must."""

    def test_every_measured_capture_agrees_with_its_configured_rate(self):
        # The false-positive guard, over real data: if either rate check fires
        # on any of these the nightly fails a conformant transmitter.
        for framerate, (rate, low, high) in MEASURED.items():
            with self.subTest(framerate=framerate):
                report = _report(rate, low, high)
                self.assertEqual(
                    pcap_compliance._framerate_mismatch_streams(report, framerate), []
                )
                self.assertEqual(
                    pcap_compliance._frame_period_mismatch_streams(report, framerate),
                    [],
                )

    def test_the_integer_neighbour_of_an_ntsc_rate_is_still_caught(self):
        # The slack that admits EBU LIST's approximation must not admit the
        # error it exists to catch: an NTSC rate sent at its integer neighbour
        # (119.88 -> 120) is 1/1001 away, the approximation only 3.33e-4.
        for framerate, neighbour, ticks in (
            ("p23", 24, 3750),
            ("p29", 30, 3000),
            ("p59", 60, 1500),
            ("p119", 120, 750),
        ):
            with self.subTest(framerate=framerate):
                report = _report(neighbour, ticks, ticks)
                self.assertTrue(
                    pcap_compliance._framerate_mismatch_streams(report, framerate),
                    "an NTSC rate sent at its integer neighbour must fail",
                )

    def test_a_transmitted_period_outside_the_bracketing_ticks_fails(self):
        # The period check is what carries the rate assertion where EBU LIST
        # could not name the rate, so it has to bite on its own.
        for framerate, low, high in (
            ("p119", 1500, 1500),  # 60 fps sent for 119.88
            ("p59", 1500, 1500),  # 60 fps sent for 59.94
            ("p60", 1501, 1502),  # 59.94 fps sent for 60
            ("p29", 3000, 3000),  # 30 fps sent for 29.97
        ):
            with self.subTest(framerate=framerate):
                report = _report(MEASURED[framerate][0], low, high)
                self.assertTrue(
                    pcap_compliance._frame_period_mismatch_streams(report, framerate),
                    "a frame period that contradicts the configured rate must fail",
                )

    def test_a_window_holding_only_the_upper_tick_is_not_a_mismatch(self):
        # 119.88 fps is 750.75 ticks, which MTL rounds into the repeating cycle
        # 751,751,750,751 -- so a short capture can legitimately observe 751
        # alone. Three of the twelve lossless p119 captures on the analyser do.
        # Requiring both bracketing ticks to appear would fail a quarter of them.
        report = _report("180000/1501", 751, 751)
        self.assertEqual(
            pcap_compliance._frame_period_mismatch_streams(report, "p119"), []
        )

    def test_a_capture_with_no_measured_period_is_inconclusive(self):
        # Missing data is not a defect, as everywhere else in the module.
        report = _report(120, 750, 750)
        del report["streams"][0]["analyses"]["inter_frame_rtp_ts_delta"]
        self.assertEqual(
            pcap_compliance._frame_period_mismatch_streams(report, "p120"), []
        )

    def test_a_framerate_that_is_not_a_rate_label_resolves_to_nothing(self):
        # This repo builds "p11988/100" from a media table whose fps field holds
        # "11988/100". Reading the digits out of that yields 11988100, an
        # integer rate -- which every check here would then enforce against the
        # stream, failing a conformant 119.88 fps transmitter on a rate nothing
        # configured. Unresolvable is the only honest answer.
        for framerate in ("p11988/100", "p59.94", "1080p", "", None, "p0"):
            with self.subTest(framerate=framerate):
                self.assertIsNone(pcap_compliance._expected_rate(framerate))

    def test_every_rate_label_in_the_matrix_still_resolves(self):
        # The negative control: strictness must not silence the checks outright.
        for framerate in MEASURED:
            with self.subTest(framerate=framerate):
                self.assertIsNotNone(pcap_compliance._expected_rate(framerate))
        self.assertEqual(pcap_compliance._expected_rate("i50"), 50.0)


class AnalyserRateLimitTests(_FetchHarness, unittest.TestCase):
    """A verdict the analyser could not compute is withheld, and only that one.

    EBU LIST 2.2.2 cannot express 120000/1001, so at 119.88 fps it builds its ST
    2110-21 model on a substitute rate and reports not_compliant for a stream
    whose RTP timestamps it measured as correct. Every p119 rxtxapp case in the
    nightly failed that way, on every runner and NIC family -- so the verdict
    has to be withheld, without loosening anything that still holds.
    """

    def test_a_verdict_built_on_a_substitute_rate_is_withheld(self):
        self.assertIs(self.fetch(P119_REPORT, framerate="p119"), P119_REPORT)

    def test_the_same_report_still_fails_at_a_rate_the_analyser_can_name(self):
        # The negative control for the case above: it is the unnameable rate
        # that withholds the verdict, never the report saying not_compliant.
        # Without this, "withheld" would be indistinguishable from "ignored".
        with self.assertRaises(AssertionError) as raised:
            self.fetch(P119_REPORT, framerate="p120")
        self.assertIn("non-compliance", str(raised.exception))

    def test_a_non_compliance_at_every_nameable_rate_still_fails(self):
        for framerate, (rate, low, high) in MEASURED.items():
            if framerate == "p119":
                continue
            with self.subTest(framerate=framerate):
                report = _report(
                    rate, low, high, compliance="not_compliant", not_compliant=1
                )
                with self.assertRaises(AssertionError):
                    self.fetch(report, framerate=framerate)

    def test_an_unclassifiable_stream_is_not_absorbed_by_the_withholding(self):
        # A stream EBU LIST could not classify as video means a truncated or
        # malformed capture; the rate gate must not launder that into a pass.
        report = {
            **P119_REPORT,
            "streams": P119_REPORT["streams"] + [{"media_type": "unknown"}],
        }
        with self.assertRaises(AssertionError):
            self.fetch(report, framerate="p119")

    def test_an_empty_report_is_not_absorbed_by_the_withholding(self):
        # No streams is no verdict, which is its own fault and still terminal
        # after the re-analysis attempts.
        with self.assertRaises(AssertionError) as raised:
            self.fetch(*[EMPTY_REPORT] * _ANALYSIS_ATTEMPTS, framerate="p119")
        self.assertIn("no ST 2110 streams", str(raised.exception))

    def test_a_lossy_capture_is_blamed_on_the_capture_not_the_period(self):
        # Verbatim from a 2160p119 capture on the analyser: 12% of the stream
        # missing from the file, so whole frames are absent and the measured
        # period doubles to 1501 -- which is a hole in the pcap, not a frame MTL
        # failed to send. Capture loss is judged first and has to stay that way,
        # or the period check invented here would blame the transmitter. Every
        # p119 capture that measured outside [750..751] was lossy like this one;
        # none of the twelve lossless ones was.
        report = _report(
            "180000/1501",
            751,
            1501,
            compliance="not_compliant",
            not_compliant=1,
            failed=("2110_21_vrx", "inter_frame_rtp_ts_delta", "rtp_sequence"),
            dropped=8774,
        )
        report["streams"][0]["statistics"]["packet_count"] = 65832
        with self.assertRaises(AssertionError):
            self.fetch(report, framerate="p119")
        self.assertEqual(self.cell(), "Fail (capture lost 11.8% of packets)")

    def test_a_failure_the_substitute_rate_cannot_explain_still_fails(self):
        # The withholding reads a flag that counts streams, not analyses, so on
        # its own it cannot tell the failure the substitute rate causes from any
        # other. Here a lossless p119 capture fails packet_ts_vs_rtp_ts, which
        # is measured against the packet timestamps rather than the nominal rate
        # -- one real capture in the survey did exactly this. Withholding it
        # would be laundering, not withholding.
        report = _report(
            "180000/1501",
            750,
            751,
            compliance="not_compliant",
            not_compliant=1,
            failed=("2110_21_vrx", "packet_ts_vs_rtp_ts"),
        )
        with self.assertRaises(AssertionError) as raised:
            self.fetch(report, framerate="p119")
        self.assertIn("non-compliance", str(raised.exception))

    def test_a_result_that_is_not_a_pass_is_not_read_as_one(self):
        # Attribution turns on which analyses passed, and EBU LIST prints only
        # "compliant" or "not_compliant" today. Should a later version print a
        # third value on an analysis outside the rate-derived set, reading it as
        # a pass is the one misreading with no symptom: the verdict is withheld
        # and the row says Pass. So anything but "compliant" has to count as not
        # passed, which withholds nothing until the value is understood.
        report = _report(
            "180000/1501",
            750,
            751,
            compliance="not_compliant",
            not_compliant=1,
            failed=("2110_21_vrx",),
        )
        report["streams"][0]["analyses"]["packet_ts_vs_rtp_ts"]["result"] = "error"
        with self.assertRaises(AssertionError) as raised:
            self.fetch(report, framerate="p119")
        self.assertIn("non-compliance", str(raised.exception))


class WithheldVerdictChecksTests(_FetchHarness, unittest.TestCase):
    """What replaces the withheld verdict, and what it must not take with it.

    These drive the whole verdict, not just the fetch: withholding is only sound
    because an independent assertion survives it and the report says a verdict
    was not taken. Both live past the fetch.
    """

    def test_the_row_says_no_timing_verdict_was_taken(self):
        # Not "Pass": the nightly artifact would then claim this capture was
        # compliance-verified when nothing verified its timing. Only a
        # Fail-prefixed cell reads back as a failure (see the truth table in
        # LossyCaptureTests), so this is a pass that declares what it omits.
        self.verdict(P119_REPORT, framerate="p119")
        self.assertEqual(self.cell(), "Pass (2110-21 verdict withheld)")

    def test_the_transmitted_period_is_still_asserted(self):
        # The assertion that makes withholding sound rather than blind. Same
        # withheld verdict, but MTL sent 60 fps where 119.88 was configured, and
        # nothing else in the run would catch it: the rate check sees EBU LIST's
        # own approximation and the timing verdict is gone.
        report = _report(
            "180000/1501",
            1500,
            1500,
            compliance="not_compliant",
            not_compliant=1,
            failed=("2110_21_vrx",),
        )
        with self.assertRaises(AssertionError) as raised:
            self.verdict(report, framerate="p119")
        self.assertIn("[750..751]", str(raised.exception))
        self.assertTrue(self.cell().startswith("Fail"), self.cell())

    def test_the_wide_tier_is_not_enforced_on_a_substitute_rate(self):
        # "wide" is the worse of the same VRX/Cinst sub-verdicts the withheld
        # verdict came from, so at p119 it is the identical false failure in a
        # different shape -- and one that tells the author to go and mark the
        # test allow_wide_compliance.
        self.verdict(
            _report("180000/1501", 750, 751, compliance="wide"), framerate="p119"
        )
        self.assertEqual(self.cell(), "Pass (2110-21 verdict withheld)")

    def test_the_wide_tier_is_still_enforced_at_a_nameable_rate(self):
        # The negative control for the case above.
        with self.assertRaises(AssertionError) as raised:
            self.verdict(_report(120, 750, 750, compliance="wide"), framerate="p120")
        self.assertIn("wide", str(raised.exception))

    def test_a_capture_the_analyser_could_name_records_an_ordinary_pass(self):
        # Withholding must stay confined to the rates that earn it.
        self.verdict(_report(120, 750, 750), framerate="p120")
        self.assertEqual(self.cell(), "Pass")


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
