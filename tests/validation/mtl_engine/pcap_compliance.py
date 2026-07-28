# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""EBU LIST pcap-compliance capture + upload/poll/verdict, owned by one
``ComplianceSession`` object.

The ``pcap_capture`` fixture (tests/validation/conftest.py) is the single
entry point: it builds the underlying ``NetsniffRecorder`` and wraps it in a
``ComplianceSession``, which then owns the whole lifecycle -- arming the
capture, running the EBU verdict, and enforcing that a test which requested
compliance checking cannot silently skip it. Application adapters
(application_base.py) never touch ``NetsniffRecorder`` or the EBU client
directly; they only supply a ``CaptureIntent`` and call
``arm()``/``evaluate()`` on whatever the fixture handed them. Tests that
don't want compliance checking get ``NO_COMPLIANCE``, a no-op stand-in with
the same surface.
"""

import logging
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING, Optional, Protocol

from compliance.compliance_client import PcapComplianceClient
from mfd_connect.exceptions import ConnectionCalledProcessError

from .csv_report import update_compliance_result
from .execute import log_fail

logger = logging.getLogger(__name__)

# Seconds to let a stream reach steady state before arming the pcap capture.
# ST 2110-21 conformance is a steady-state measurement, and MTL session/queue
# init plus PTP epoch alignment take several seconds before the first RTP
# packet -- arming capture too early pulls startup transients into the
# compliance window and causes spurious VRX failures.
CAPTURE_SETTLE_TIME = 12


def _video_streams(report: dict) -> list[dict]:
    """Return every ``media_type == "video"`` stream in an EBU LIST report."""
    return [s for s in (report.get("streams") or []) if s.get("media_type") == "video"]


def _wide_video_streams(report: dict) -> list[dict]:
    """Return video streams whose ST 2110-21 VRX/Cinst compliance tier is "wide".

    EBU LIST classifies each video stream's overall timing compliance as
    ``narrow_linear``, ``narrow``, or ``wide`` in
    ``stream.global_video_analysis.compliance`` (the worse of the per-stream
    ``cinst``/``vrx`` sub-verdicts). This is independent of
    ``media_specific.schedule`` ('linear'/'gapped') -- a stream can be
    schedule=linear and still only be "wide" compliant, so schedule must not
    be used as a proxy for this tier.
    """
    return [
        s
        for s in _video_streams(report)
        if s.get("global_video_analysis", {}).get("compliance") == "wide"
    ]


# EBU LIST's own packing_mode enum (pi-list cpp/libs/st2110/lib/include/ebu/
# list/st2110/d20/video_description.h: `enum class packing_mode_t { unknown,
# general, block }`, serialized to JSON as the plain underlying int -- 0, 1, 2
# respectively) is UNRELATED to MTL's ST20_PACKING_{BPM,GPM,GPM_SL} enum
# ordering (BPM=0, GPM=1, GPM_SL=2 in include/st20_api.h) despite the
# superficial name overlap. EBU LIST's packing_mode_analyzer
# (cpp/libs/st2110/lib/src/ebu/list/st2110/d20/packing_mode_analyzer.cpp)
# defaults to `block` and only flips to `general` when it sees an SRD
# (payload) length, other than the marker packet, that is not a multiple of
# 180 bytes -- it cannot distinguish MTL's GPM from GPM_SL (both are "general"
# to an outside observer), so both map to the same expected value here.
_PACKING_TO_EBU_MODE = {
    "BPM": 2,  # packing_mode_t::block
    "GPM": 1,  # packing_mode_t::general
    "GPM_SL": 1,  # packing_mode_t::general (EBU LIST has no separate single-line value)
}


def _resolution_mismatch_streams(
    report: dict, expected_width, expected_height
) -> list[dict]:
    """Return video streams whose EBU LIST width/height disagree with the expected resolution.

    *expected_width*/*expected_height* are the MTL ``width``/``height`` config
    values, checked independently -- a stream where only one dimension was
    analyzed (the other missing/``None`` in the report) is inconclusive for
    that dimension, not a mismatch, mirroring every other check in this
    module's "missing data is not a defect" convention.
    """
    if not expected_width and not expected_height:
        return []
    mismatches = []
    for s in _video_streams(report):
        media_specific = s.get("media_specific", {})
        width = media_specific.get("width")
        height = media_specific.get("height")
        width_mismatch = (
            expected_width and width is not None and width != expected_width
        )
        height_mismatch = (
            expected_height and height is not None and height != expected_height
        )
        if width_mismatch or height_mismatch:
            mismatches.append(s)
    return mismatches


# MTL transport_format -> EBU LIST's own (media_specific.sampling,
# media_specific.color_depth). This is a closed, explicit table over the
# transport_format values mtl_engine.media_files/rxtxapp_config actually
# produce, rather than parsing the string (e.g. splitting on "_" and
# stripping a "bit" suffix) -- "v210" and "I422_10LE" don't decompose into a
# "{SAMPLING}_{DEPTH}bit" shape at all, so a generic parser silently (and
# incorrectly) treats them as unrecognized and skips the check. Sampling
# strings verified against pi-list cpp/libs/core/lib/src/ebu/list/core/
# media/video/sampling.cpp's video::to_string(video_sampling).
_TRANSPORT_FORMAT_TO_EBU_SAMPLING = {
    "YUV_420_8bit": ("YCbCr-4:2:0", 8),
    "YUV_422_8bit": ("YCbCr-4:2:2", 8),
    "YUV_422_10bit": ("YCbCr-4:2:2", 10),
    "YUV_422_12bit": ("YCbCr-4:2:2", 12),
    "YUV_444_10bit": ("YCbCr-4:4:4", 10),
    "YUV_444_12bit": ("YCbCr-4:4:4", 12),
    "RGB_10bit": ("RGB", 10),
    "RGB_12bit": ("RGB", 12),
    "I422_10LE": ("YCbCr-4:2:2", 10),  # planar 10-bit 4:2:2
    "v210": ("YCbCr-4:2:2", 10),  # packed 10-bit 4:2:2 (v210 fourcc)
}


def _sampling_mismatch_streams(report: dict, expected_transport_format) -> list[dict]:
    """Return video streams whose EBU LIST sampling/color_depth disagree with *expected_transport_format*.

    *expected_transport_format* is the MTL ``transport_format`` config value
    (e.g. ``"YUV_422_10bit"``, ``"v210"``), looked up in
    ``_TRANSPORT_FORMAT_TO_EBU_SAMPLING``. Returns an empty list (with a
    warning logged) when the value has no known mapping -- an unmappable
    format is a harness gap, not a passing test, so it must not disappear
    silently.
    """
    if not expected_transport_format:
        return []
    expected = _TRANSPORT_FORMAT_TO_EBU_SAMPLING.get(expected_transport_format)
    if expected is None:
        logger.warning(
            "transport_format=%r has no known EBU LIST sampling/color_depth "
            "mapping -- sampling/color_depth compliance check skipped for "
            "this test (add it to _TRANSPORT_FORMAT_TO_EBU_SAMPLING)",
            expected_transport_format,
        )
        return []
    expected_sampling, expected_depth = expected
    mismatches = []
    for s in _video_streams(report):
        media_specific = s.get("media_specific", {})
        sampling = media_specific.get("sampling")
        depth = media_specific.get("color_depth")
        if sampling not in (None, expected_sampling) or depth not in (
            None,
            expected_depth,
        ):
            mismatches.append(s)
    return mismatches


# NTSC frame rates are truncated for their MTL ``pXX``/``iXX`` label (e.g.
# 59.94 -> "p59", never "p59.94" -- see media_files.py's fps_to_framerate_field)
# while EBU LIST reports the true rate as an exact rational (verified against
# pi-list cpp/libs/core/lib/src/ebu/list/core/math/fraction.h's
# to_string(fraction_t): integer rates serialize as a bare int string,
# fractional ones as "num/den", e.g. "60000/1001" for 59.94). Comparing the
# truncated label against the exact rate directly would flag every NTSC
# stream as a false mismatch.
_NTSC_TRUNCATED_TO_EXACT_RATE = {
    23: 24000 / 1001,
    29: 30000 / 1001,
    59: 60000 / 1001,
    119: 120000 / 1001,
}

_FRAMERATE_TOLERANCE = 0.01


def _parse_ebu_rate(rate) -> Optional[float]:
    """Parse EBU LIST's ``media_specific.rate`` (int, bare-int string, or "num/den" string)."""
    if isinstance(rate, (int, float)):
        return float(rate)
    text = str(rate)
    numerator, sep, denominator = text.partition("/")
    return float(numerator) / float(denominator) if sep else float(numerator)


def _framerate_mismatch_streams(report: dict, expected_framerate) -> list[dict]:
    """Return video streams whose EBU LIST ``rate`` disagrees with *expected_framerate*.

    *expected_framerate* is the MTL ``framerate`` config value (e.g.
    ``"p25"``/``"i50"``/``"p59"``); only the numeric part is compared against
    EBU LIST's ``media_specific.rate``, since EBU LIST reports scan type
    separately (``media_specific.scan_type``). NTSC labels are mapped to
    their exact rational rate via ``_NTSC_TRUNCATED_TO_EXACT_RATE`` before
    comparing (with ``_FRAMERATE_TOLERANCE`` slack for floating-point
    rounding). Returns an empty list when no numeric part can be parsed
    (nothing to check against).
    """
    if not expected_framerate:
        return []
    digits = "".join(c for c in str(expected_framerate) if c.isdigit())
    if not digits:
        return []
    expected_int = int(digits)
    expected_rate = _NTSC_TRUNCATED_TO_EXACT_RATE.get(expected_int, float(expected_int))
    mismatches = []
    for s in _video_streams(report):
        rate = s.get("media_specific", {}).get("rate")
        if rate is None:
            continue
        try:
            observed_rate = _parse_ebu_rate(rate)
        except (ValueError, ZeroDivisionError):
            continue
        if abs(observed_rate - expected_rate) > _FRAMERATE_TOLERANCE:
            mismatches.append(s)
    return mismatches


def _packing_mismatch_streams(report: dict, expected_packing) -> list[dict]:
    """Return video streams whose EBU LIST packing_mode disagrees with *expected_packing*.

    *expected_packing* is the MTL ``packing`` config value ("BPM"/"GPM"/
    "GPM_SL"); see ``_PACKING_TO_EBU_MODE`` for the verified mapping to EBU
    LIST's own ``packing_mode`` values. Streams with no ``packing_mode`` in
    the report (analysis inconclusive) are not treated as a mismatch -- only
    a definite disagreement is. Returns an empty list when *expected_packing*
    isn't a recognized value (nothing to check against).
    """
    expected_mode = _PACKING_TO_EBU_MODE.get(expected_packing)
    if expected_mode is None:
        return []
    return [
        s
        for s in _video_streams(report)
        if s.get("media_specific", {}).get("packing_mode") not in (None, expected_mode)
    ]


@dataclass
class CaptureIntent:
    """Everything a :class:`ComplianceSession` needs from a running Application.

    Built by ``Application.capture_intent()`` from ``self.params`` so this
    module never reaches into Application internals directly -- the
    Application/session boundary is exactly this dataclass.
    """

    dst_ip: Optional[str]
    capture_time: int
    settle_time: int = CAPTURE_SETTLE_TIME
    ptp_wait: int = 0
    packing: Optional[str] = None
    pacing: Optional[str] = None
    width: Optional[int] = None
    height: Optional[int] = None
    transport_format: Optional[str] = None
    framerate: Optional[str] = None


class ComplianceCheck(Protocol):
    """Shared surface of :class:`ComplianceSession` and its null-object stand-in.

    Enforces, at type-check time, that ``_NullComplianceSession`` cannot
    silently drift out of parity with the real session as the surface grows.
    """

    enabled: bool

    def skip(self, reason: str) -> None: ...

    def arm(self, intent: CaptureIntent) -> None: ...

    def evaluate(self, intent: CaptureIntent, fail_on_error: bool = True) -> bool: ...

    def close(self) -> None: ...


class ComplianceSession:
    """Owns one capture and its EBU LIST compliance verdict for one test.

    Created only by the ``pcap_capture`` fixture (tests/validation/conftest.py),
    which builds the underlying ``NetsniffRecorder`` and this wrapper
    together. Tests that don't want compliance checking get ``NO_COMPLIANCE``
    instead, a no-op stand-in sharing this class's public surface
    (``enabled``/``skip``/``arm``/``evaluate``/``close``).
    """

    def __init__(
        self,
        recorder,
        ebu_server: dict,
        mtl_path,
        node_id: str,
        allow_wide: bool = False,
    ):
        self._recorder = recorder
        self.ebu_server = ebu_server
        self.mtl_path = mtl_path
        self.node_id = node_id
        self.allow_wide = allow_wide
        self._skip_reason: Optional[str] = None
        self._evaluated = False

    @property
    def enabled(self) -> bool:
        """False once :meth:`skip` has been called; capture/verdict become no-ops."""
        return self._skip_reason is None

    def skip(self, reason: str) -> None:
        """Opt out of compliance checking for this test at runtime.

        Must be called before ``execute_test()``. Stops any capture in
        progress and satisfies the evaluated-exactly-once invariant, since a
        skipped test has nothing left to evaluate. Raises ``RuntimeError`` if
        called after the verdict already ran -- a silent no-op there would
        look like it worked while leaving the real verdict unaffected.
        """
        if self._evaluated:
            raise RuntimeError(
                "pcap_capture.skip() called after the compliance verdict "
                "already ran -- call skip() before execute_test()."
            )
        self._skip_reason = reason
        self._evaluated = True
        self._recorder.stop()
        logger.info("Compliance capture skipped: %s", reason)

    def arm(self, intent: CaptureIntent) -> None:
        """Wait for steady state, then start the capture. No-op when skipped."""
        if not self.enabled:
            return
        try:
            if intent.ptp_wait:
                logger.info(
                    "Waiting %ds for PTP sync before netsniff capture",
                    intent.ptp_wait,
                )
                time.sleep(intent.ptp_wait)
            # ST 2110-21 is a steady-state conformance measurement. The first
            # frames of a session carry startup transients (MTL session/
            # framebuffer init, first-touch page faults on the source file,
            # producer thread placement) that are not representative and
            # would otherwise dominate a short capture.
            if intent.settle_time:
                logger.info(
                    "Waiting %ds for stream to settle before capture",
                    intent.settle_time,
                )
                time.sleep(intent.settle_time)
            if not intent.dst_ip:
                logger.warning("No destination IP available for netsniff capture")
                return
            self._recorder.update_filter(dst_ip=intent.dst_ip)
            self._recorder.capture(capture_time=intent.capture_time)
            logger.info(
                "Started netsniff-ng capture for destination IP %s", intent.dst_ip
            )
        except Exception as e:
            logger.warning("netsniff capture setup failed: %s", e)

    def evaluate(self, intent: CaptureIntent, fail_on_error: bool = True) -> bool:
        """Run (or skip) the EBU compliance verdict for this session.

        A test that requests the ``pcap_capture`` fixture REQUIRES a real
        compliance verdict unless it explicitly opted out via :meth:`skip`
        or ``capture_cfg.enable: false`` -- a missing ``ebu_server`` or a
        capture that failed to produce a pcap file are hard compliance
        failures, never a silent pass. Returns True when compliant, skipped,
        or not applicable. Returns False only when non-compliant/
        unconfigured and ``fail_on_error`` is False. Raises
        ``AssertionError`` when non-compliant/unconfigured and
        ``fail_on_error`` is True.
        """
        self._evaluated = True
        if not self.enabled:
            return True
        try:
            if not self.ebu_server:
                self._fail(
                    "Compliance check required (test uses the pcap_capture "
                    "fixture and did not opt out) but ebu_server is not "
                    "configured in test_config.yaml -- cannot verify EBU "
                    "compliance for this test. Configure capture_cfg (a 2nd "
                    "NIC PF for netsniff-ng) and ebu_server, or set "
                    "capture_cfg.enable: false / call pcap_capture.skip(...) "
                    "to explicitly opt out.",
                    fail_on_error,
                )
            if not self._recorder.pcap_file:
                self._fail(
                    "Compliance check required but PCAP capture failed to "
                    "produce a file (netsniff-ng did not start) -- cannot "
                    "verify EBU compliance for this test.",
                    fail_on_error,
                )
            # pacing="wide" (ST21_PACING_WIDE) deliberately widens MTL's
            # VRX/Cinst tolerance, so EBU LIST legitimately reports "wide"
            # (not narrow/narrow_linear) compliance for these streams -- that
            # is the requested behavior, not a defect. Correlate
            # automatically here so no test needs to set allow_wide by hand;
            # ``@pytest.mark.allow_wide_compliance`` (read by the
            # ``pcap_capture`` fixture) stays available for other legitimate
            # wide cases.
            allow_wide = self.allow_wide or intent.pacing == "wide"
            self._verdict(intent, allow_wide=allow_wide, fail_on_error=fail_on_error)
            return True
        except AssertionError:
            if fail_on_error:
                raise
            logger.info("Compliance check failed (fail_on_error=False); continuing")
            return False

    def close(self) -> None:
        """Stop the capture and enforce the evaluated-exactly-once invariant.

        Called from the ``pcap_capture`` fixture's teardown. The real
        upload/poll/verdict runs during the call phase via :meth:`evaluate`;
        this only catches a test that requested the fixture but never called
        it at all -- a required compliance check must not be silently
        skippable that way either.
        """
        self._recorder.stop()
        if not self._evaluated:
            log_fail(
                "Compliance check required (test uses the pcap_capture "
                "fixture) but execute_test() never dispatched it -- ensure "
                "the test calls execute_test(compliance=pcap_capture, ...) "
                "or pcap_capture.skip(...)."
            )

    def _fail(self, msg: str, fail_on_error: bool) -> None:
        update_compliance_result(self.node_id, "Fail")
        if fail_on_error:
            log_fail(msg)
        else:
            logger.info("Compliance soft-fail (fail_on_error=False): %s", msg)
        raise AssertionError(msg)

    def _verdict(
        self, intent: CaptureIntent, *, allow_wide: bool, fail_on_error: bool
    ) -> None:
        """Upload ``self._recorder.pcap_file`` to the EBU LIST analyser and verify compliance.

        Raises ``AssertionError`` when the capture is not compliant, when a
        video stream is only ST 2110-21 "wide" compliant (not narrow/
        narrow_linear) and ``allow_wide`` is False, or when a video stream's
        EBU LIST ``packing_mode``/resolution/sampling+color_depth/framerate
        disagrees with the configured MTL values (all always checked, no
        opt-out marker, since a mismatch means the stream isn't actually
        using the requested wire format). Narrow (or narrow linear) is the
        expected default for MTL; a "wide" verdict most often means the
        capture PF and the primary/TX PF are not properly PTP-synchronized
        (sync the system clock to the capturing NIC's PHC via phc2sys before
        capture).

        When ``fail_on_error`` is True, also records a hard pytest failure
        via ``log_fail``; when False, only logs at INFO so soft-fail callers
        (binary-search/performance loops) can continue without a forced
        abort. Removes the pcap file after upload regardless of the verdict.
        """
        report = self._fetch_report(fail_on_error)
        self._apply_checks(report, intent, allow_wide, fail_on_error)

    def _fetch_report(self, fail_on_error: bool) -> dict:
        """Upload ``self._recorder.pcap_file`` and return its EBU LIST report.

        Raises ``AssertionError`` (via :meth:`_fail`) on any transport
        failure -- the upload command failing, its output not containing the
        expected UUID marker, or the EBU LIST client itself reporting
        non-compliant with no usable report -- so a broken upload/poll path
        is always a hard compliance failure, never a silent pass. Removes the
        pcap file afterward regardless of outcome.
        """
        capturer = self._recorder
        ebu_ip = self.ebu_server.get("ebu_ip", None)
        ebu_login = self.ebu_server.get("user", None)
        ebu_passwd = self.ebu_server.get("password", None)
        ebu_proxy = self.ebu_server.get("proxy", None)
        proxy_cmd = f" --proxy {ebu_proxy}" if ebu_proxy else ""
        try:
            compliance_upl = capturer.host.connection.execute_command(
                "python3 ./tests/validation/compliance/upload_pcap.py"
                f" --ip {ebu_ip}"
                f" --user {ebu_login}"
                f" --password {ebu_passwd}"
                f" --pcap '{capturer.pcap_file}'{proxy_cmd}",
                cwd=f"{str(self.mtl_path)}",
            )
            if compliance_upl.return_code != 0:
                self._fail(
                    f"PCAP upload to EBU LIST failed: {compliance_upl.stderr}",
                    fail_on_error,
                )
            try:
                uuid = compliance_upl.stdout.split(">>>UUID: ")[1].strip()
            except IndexError:
                self._fail(
                    "PCAP upload to EBU LIST succeeded but its output did not "
                    f"contain a UUID marker: {compliance_upl.stdout!r}",
                    fail_on_error,
                )
            logger.debug(f"PCAP successfully uploaded to EBU LIST with UUID: {uuid}")
            uploader = PcapComplianceClient(
                ebu_ip=ebu_ip,
                user=ebu_login,
                password=ebu_passwd,
                pcap_id=uuid,
                proxies={"http": ebu_proxy, "https": ebu_proxy},
            )
            result, report = uploader.check_compliance()
            if not result:
                update_compliance_result(self.node_id, "Fail")
                logger.info(f"Compliance report: {report}")
                self._fail("PCAP compliance check failed", fail_on_error)
            return report or {}
        finally:
            try:
                capturer.host.connection.execute_command(
                    f"rm -f '{capturer.pcap_file}'"
                )
                logger.debug(f"Removed pcap file: {capturer.pcap_file}")
            except ConnectionCalledProcessError as e:
                logger.warning(f"Failed to remove pcap file: {e}")

    def _apply_checks(
        self, report: dict, intent: CaptureIntent, allow_wide: bool, fail_on_error: bool
    ) -> None:
        """Compare *report* against *intent*'s expected wire format; raises on the first mismatch.

        A pure function of ``(report, intent)`` plus the ``allow_wide``/
        ``fail_on_error`` policy knobs -- no network I/O, so it is directly
        unit-testable against a saved EBU LIST report.
        """
        node_id = self.node_id
        wide_streams = _wide_video_streams(report)
        if wide_streams and not allow_wide:
            msg = (
                f"PCAP compliance check failed: {len(wide_streams)} video "
                "stream(s) are only ST 2110-21 'wide' compliant (not "
                "narrow/narrow_linear). This usually means the capture PF "
                "and the primary/TX PF are not properly PTP-synchronized -- "
                "sync the system clock to the capturing NIC's PHC (phc2sys) "
                'before capture. If pacing="wide" wasn\'t configured for '
                "this test but wide compliance is still expected/acceptable, "
                "mark it with @pytest.mark.allow_wide_compliance."
            )
            self._fail(msg, fail_on_error)

        packing_mismatch = _packing_mismatch_streams(report, intent.packing)
        if packing_mismatch:
            observed = packing_mismatch[0].get("media_specific", {}).get("packing_mode")
            msg = (
                f"PCAP compliance check failed: {len(packing_mismatch)} video "
                f"stream(s) report an EBU LIST packing_mode that disagrees with "
                f"the configured packing={intent.packing!r} (observed packing_mode="
                f"{observed!r}) -- the stream is not actually using the requested "
                "packing mode on the wire."
            )
            self._fail(msg, fail_on_error)

        resolution_mismatch = _resolution_mismatch_streams(
            report, intent.width, intent.height
        )
        if resolution_mismatch:
            observed_specific = resolution_mismatch[0].get("media_specific", {})
            observed = (
                f"{observed_specific.get('width')}x{observed_specific.get('height')}"
            )
            msg = (
                f"PCAP compliance check failed: {len(resolution_mismatch)} video "
                f"stream(s) report a resolution that disagrees with the configured "
                f"{intent.width}x{intent.height} (observed {observed}) -- the stream "
                "is not actually transporting the requested resolution on the wire."
            )
            self._fail(msg, fail_on_error)

        sampling_mismatch = _sampling_mismatch_streams(report, intent.transport_format)
        if sampling_mismatch:
            observed_specific = sampling_mismatch[0].get("media_specific", {})
            observed = (
                f"sampling={observed_specific.get('sampling')!r} "
                f"color_depth={observed_specific.get('color_depth')!r}"
            )
            msg = (
                f"PCAP compliance check failed: {len(sampling_mismatch)} video "
                f"stream(s) report a sampling/color_depth that disagrees with the "
                f"configured transport_format={intent.transport_format!r} (observed "
                f"{observed}) -- the stream is not actually using the requested "
                "pixel format on the wire."
            )
            self._fail(msg, fail_on_error)

        framerate_mismatch = _framerate_mismatch_streams(report, intent.framerate)
        if framerate_mismatch:
            observed = framerate_mismatch[0].get("media_specific", {}).get("rate")
            msg = (
                f"PCAP compliance check failed: {len(framerate_mismatch)} video "
                f"stream(s) report a frame rate that disagrees with the configured "
                f"framerate={intent.framerate!r} (observed rate={observed!r}) -- the "
                "stream is not actually running at the requested frame rate on the "
                "wire."
            )
            self._fail(msg, fail_on_error)

        if wide_streams:
            update_compliance_result(node_id, "Pass (wide)")
            logger.warning(
                "PCAP compliance check passed with wide compliance on "
                "%d video stream(s) (allowed via pacing='wide' or "
                "@pytest.mark.allow_wide_compliance)",
                len(wide_streams),
            )
        else:
            update_compliance_result(node_id, "Pass")
            logger.info("PCAP compliance check passed (narrow/narrow_linear)")


class _NullComplianceSession:
    """No-op stand-in used when capture is disabled (config opt-out or 8K skip)."""

    enabled = False

    def skip(self, reason: str) -> None:
        pass

    def arm(self, intent: CaptureIntent) -> None:
        pass

    def evaluate(self, intent: CaptureIntent, fail_on_error: bool = True) -> bool:
        return True

    def close(self) -> None:
        pass


if TYPE_CHECKING:
    # Static-only proof that both classes implement ComplianceCheck's full
    # surface -- a mypy/pyright run fails here (not just at a call site) the
    # moment either class drifts out of parity. No runtime cost/effect.
    _null_check: ComplianceCheck = _NullComplianceSession()
    _session_check: ComplianceCheck = ComplianceSession(None, {}, None, "")

NO_COMPLIANCE = _NullComplianceSession()
