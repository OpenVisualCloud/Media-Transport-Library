# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""EBU LIST pcap-compliance capture + upload/poll/verdict, owned by one
``ComplianceSession`` object.

The ``pcap_capture`` fixture (tests/acceptance/conftest.py) is the single
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
import math
import re
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING, Callable, Optional, Protocol

from compliance.compliance_client import PcapComplianceClient, no_verdict_reason
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

# How many times to analyse an uploaded capture before concluding that it really
# holds no stream. EBU LIST occasionally drops an ingest: its pre-processor logs
# "Processing time: 0.000 s" and "Added 0 new streams", then marks the pcap
# analyzed, so the empty report is terminal and polling longer cannot recover it.
# Re-analysing the capture already on the analyser recovers it -- observed to fix
# every dropped ingest seen so far -- so retry that before failing the test.
_ANALYSIS_ATTEMPTS = 3

# Slice of :meth:`ComplianceSession.arm`'s minute-plus wait between checks that
# the process under test is still alive.
_LIVENESS_POLL_INTERVAL = 2

# Returns the process under test's return code, None while it is still running.
_ExitCode = Optional[Callable[[], Optional[int]]]


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


def _capture_loss(report: dict) -> Optional[tuple[int, int]]:
    """``(missing, present)`` packet counts EBU LIST reports, None if lossless.

    ``statistics.dropped_packet_count`` is derived from RTP sequence gaps in the
    uploaded pcap, over every stream it found -- a report it could not classify
    as video carries the counts but no timing verdict at all.
    """
    missing = 0
    present = 0
    for stream in report.get("streams") or []:
        statistics = stream.get("statistics") or {}
        missing += statistics.get("dropped_packet_count") or 0
        present += statistics.get("packet_count") or 0
    return (missing, present) if missing else None


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

# The only fractional frame rates EBU LIST 2.2.2 can name. Verified in the
# analyser itself: `strings /app/bin/st2110_extractor` carries the literals
# "24000/1001", "30000/1001" and "60000/1001" beside its rate parser
# "^(\d+)(\/(\d+))?$" and its "Unknown rate: {}" error, and no "120000/1001".
# Handed a rate outside this set the analyser substitutes the nearest rational
# it can express -- 119.88 fps is reported as 180000/1501 or 90000/751 -- and
# then derives media_specific.rate and the whole ST 2110-21 VRX/Cinst model
# from that substitute. See :func:`_rate_is_beyond_analyser`.
_EBU_FRACTIONAL_RATES = (24000 / 1001, 30000 / 1001, 60000 / 1001)

_MEDIA_CLOCK_HZ = 90000  # ST 2110-20 video RTP timestamp clock

# Relative slack when comparing EBU LIST's reported rate to the configured one.
# Its rate is its own best rational approximation, so it is a measurement, not a
# label: for 119.88 fps -- which it cannot name, see _EBU_FRACTIONAL_RATES -- it
# substitutes 180000/1501 or 90000/751, both 3.33e-4 off. The error this must
# still catch is a stream running at an NTSC rate's integer neighbour (120 fps
# for 119.88), and every 1000/1001 rate sits 1/1001 = 1.0e-3 from its
# neighbour, so this sits between the two with margin either way.
_ANALYSER_RATE_TOLERANCE = 6e-4


# An MTL ``framerate`` config value: an optional scan-type letter and a whole
# number, nothing else. Matching strictly matters because this repo also builds
# tokens this cannot mean -- ``f"p{fps}"`` over a media table whose ``fps`` holds
# "11988/100" yields "p11988/100" -- and reading the digits out of one of those
# would silently resolve 119.88 fps to 11988100, an integer rate the checks
# below would then enforce against the stream.
_FRAMERATE_LABEL_RE = re.compile(r"^[pi]?(\d+)$")


def _expected_rate(expected_framerate) -> Optional[float]:
    """The exact rate an MTL ``framerate`` config value ("p59", "i50") asks for.

    The scan-type letter is dropped; MTL spells scan type into the same string
    while EBU LIST reports it separately (``media_specific.scan_type``). NTSC
    labels are truncations, so they map through
    ``_NTSC_TRUNCATED_TO_EXACT_RATE`` to the exact rational. None when the value
    is not a rate label at all, so every caller treats it as unresolvable rather
    than checking against a number it invented -- and warns, unless there was no
    value to begin with: a capture of a stream that has no frame rate (audio,
    ancillary) has nothing to warn about.
    """
    if not expected_framerate:
        return None
    match = _FRAMERATE_LABEL_RE.match(str(expected_framerate))
    rate = int(match.group(1)) if match else 0
    if rate <= 0:
        logger.warning(
            "Framerate %r is not a rate label; no rate to check against",
            expected_framerate,
        )
        return None
    return _NTSC_TRUNCATED_TO_EXACT_RATE.get(rate, float(rate))


def _rate_is_beyond_analyser(expected_framerate) -> bool:
    """True when EBU LIST cannot name *expected_framerate* exactly.

    Its rate-derived output then describes its own approximation rather than
    the stream, so neither ``media_specific.rate`` nor the ST 2110-21
    VRX/Cinst tier computed from it can stand as a verdict on MTL. Integer
    rates are always expressible; see :data:`_EBU_FRACTIONAL_RATES` for the
    fractional ones.
    """
    rate = _expected_rate(expected_framerate)
    if rate is None:
        return False
    return not (rate.is_integer() or rate in _EBU_FRACTIONAL_RATES)


def _frame_period_ticks(expected_framerate) -> Optional[tuple]:
    """``(exact, low, high)`` RTP ticks per frame, or None if the rate is unresolvable.

    ``low``/``high`` are the only two integer periods a conformant sender may
    emit for a non-integer ``exact``, and collapse onto it when it is whole.
    """
    rate = _expected_rate(expected_framerate)
    if rate is None:
        return None
    exact = _MEDIA_CLOCK_HZ / rate
    return exact, math.floor(exact), math.ceil(exact)


def _frame_period_mismatch_streams(report: dict, expected_framerate) -> list[dict]:
    """Video streams whose measured frame period contradicts *expected_framerate*.

    Judges the period MTL actually put on the wire, straight from the capture's
    RTP timestamps: EBU LIST measures
    ``analyses.inter_frame_rtp_ts_delta.details.range`` from those alone, so
    that range holds even where the analyser could not name the nominal rate
    and its own ``limit``/``result`` beside it were computed against a
    substitute (:func:`_rate_is_beyond_analyser`) -- which is why this reads the
    measurement and applies its own limit.

    At the 90 kHz video clock the exact period is ``90000 / rate`` ticks, and the
    only periods a conformant sender may emit are the two integers bracketing it
    -- 119.88 fps is 750.75 ticks, so every delta is 750 or 751. Which of them a
    given capture holds is not fixed, so this bounds the measured range rather
    than demanding both appear: MTL's cycle at 119.88 is 751,751,750,751, and a
    window landing inside a run of 751 is conformant. An integer period (30000/
    1001 fps is exactly 3003 ticks, as is every whole rate) collapses both bounds
    onto one value. Streams carrying no measured range are inconclusive, not a
    mismatch, as elsewhere in this module.
    """
    ticks = _frame_period_ticks(expected_framerate)
    if ticks is None:
        return []
    _, low, high = ticks
    mismatches = []
    for s in _video_streams(report):
        measured = (
            s.get("analyses", {})
            .get("inter_frame_rtp_ts_delta", {})
            .get("details", {})
            .get("range", {})
        )
        minimum, maximum = measured.get("min"), measured.get("max")
        if minimum is None or maximum is None:
            continue
        if minimum < low or maximum > high:
            mismatches.append(s)
    return mismatches


def _non_compliant_analyses(report: dict) -> set:
    """Names of the per-stream analyses EBU LIST did not pass, across *report*.

    Anything other than ``"compliant"`` counts as not passed -- the two values
    it emits are that and ``"not_compliant"``, and reading an unrecognized
    third as a pass is the one error with a silent outcome: its only caller
    withholds a verdict when this set is empty.
    """
    failed = set()
    for s in report.get("streams") or []:
        for name, analysis in (s.get("analyses") or {}).items():
            if isinstance(analysis, dict) and analysis.get("result") != "compliant":
                failed.add(name)
    return failed


# The analyses EBU LIST computes from the nominal frame rate, and so the only
# ones a rate it cannot name invalidates. Its ST 2110-21 model is VRX plus
# Cinst; ``inter_frame_rtp_ts_delta`` compares the measured period against a
# limit derived from the same rate (this module reads its measurement instead,
# see :func:`_frame_period_mismatch_streams`).
_RATE_DERIVED_ANALYSES = frozenset(
    {"2110_21_vrx", "2110_21_cinst", "inter_frame_rtp_ts_delta"}
)


def _verdict_withheld_reason(report: dict, expected_framerate) -> Optional[str]:
    """Why EBU LIST's ST 2110-21 verdict cannot hold here, or None if it holds.

    Two conditions must both hold. The analyser could not name the configured
    frame rate, so what it derived from that rate was computed against one the
    stream never used (:func:`_rate_is_beyond_analyser`); and nothing it failed
    lies outside :data:`_RATE_DERIVED_ANALYSES`. The second test is what makes
    the first safe: the verdict this withholds is a single flag,
    ``not_compliant_streams``, which counts streams and never says which
    analysis failed -- so on its own a rate the analyser cannot name would
    suppress every unrelated failure alongside the one it explains.

    That the two are separable is not an assumption: across the 33 p119 streams
    in the nightly captures surveyed for this check, failing only rate-derived
    analyses and losing no packets were the same 12 captures, exactly. Every
    other p119 failure also failed ``rtp_sequence`` -- capture loss, which
    :func:`_capture_loss` blames on the capture ahead of any of this.

    A report with no verdict at all, and one holding a stream the analyser could
    not classify as video, are capture defects that this must not absorb either.
    """
    if not _rate_is_beyond_analyser(expected_framerate):
        return None
    if no_verdict_reason(report):
        return None
    if any(s.get("media_type") == "unknown" for s in report.get("streams") or []):
        return None
    unexplained = _non_compliant_analyses(report) - _RATE_DERIVED_ANALYSES
    if unexplained:
        return None
    detected = ", ".join(
        str(s.get("media_specific", {}).get("rate")) for s in _video_streams(report)
    )
    return (
        f"EBU LIST cannot represent the configured framerate "
        f"{expected_framerate!r} exactly (it analysed the capture as "
        f"rate={detected}), so the ST 2110-21 VRX/Cinst verdict it reported was "
        "computed against a nominal rate the stream never used. That verdict is "
        "withheld; the frame period MTL transmitted is asserted from the "
        "capture's own RTP timestamps instead."
    )


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
    ``"p25"``/``"i50"``/``"p59"``), resolved by :func:`_expected_rate` and
    compared against EBU LIST's ``media_specific.rate`` within
    :data:`_ANALYSER_RATE_TOLERANCE`. Returns an empty list when no rate can be
    resolved (nothing to check against).
    """
    expected_rate = _expected_rate(expected_framerate)
    if expected_rate is None:
        return []
    allowed_error = expected_rate * _ANALYSER_RATE_TOLERANCE
    mismatches = []
    for s in _video_streams(report):
        rate = s.get("media_specific", {}).get("rate")
        if rate is None:
            continue
        try:
            observed_rate = _parse_ebu_rate(rate)
        except (ValueError, ZeroDivisionError):
            continue
        if abs(observed_rate - expected_rate) > allowed_error:
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

    dst_ips: tuple[str, ...]
    capture_time: int
    settle_time: int = CAPTURE_SETTLE_TIME
    ptp_wait: int = 0
    packing: Optional[str] = None
    pacing: Optional[str] = None
    width: Optional[int] = None
    height: Optional[int] = None
    transport_format: Optional[str] = None
    framerate: Optional[str] = None
    expected_video_streams: int = 1


class ComplianceCheck(Protocol):
    """Shared surface of :class:`ComplianceSession` and its null-object stand-in.

    Enforces, at type-check time, that ``_NullComplianceSession`` cannot
    silently drift out of parity with the real session as the surface grows.
    """

    enabled: bool

    def skip(self, reason: str) -> None: ...

    def arm(self, intent: CaptureIntent, *, exit_code: _ExitCode = None) -> None: ...

    def evaluate(self, intent: CaptureIntent, fail_on_error: bool = True) -> bool: ...

    def close(self, enforce_dispatch: bool = True) -> None: ...


class ComplianceSession:
    """Owns one capture and its EBU LIST compliance verdict for one test.

    Created only by the ``pcap_capture`` fixture (tests/acceptance/conftest.py),
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

        Must be called before the verdict runs: a test opting out calls it
        before ``execute_test()``, :meth:`arm` mid-run when the process under
        test dies before the capture window. Stops any capture in
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

    def arm(self, intent: CaptureIntent, *, exit_code: _ExitCode = None) -> None:
        """Wait for steady state, then start the capture. No-op when skipped.

        *exit_code* is ``Application._safe_return_code``: a process that died
        during init transmits nothing, so do not wait it out. Left None, the
        whole budget is waited out as before.
        """
        if not self.enabled:
            return
        try:
            if intent.ptp_wait:
                logger.info(
                    "Waiting %ds for PTP sync before netsniff capture",
                    intent.ptp_wait,
                )
                if not self._sleep_while_alive(intent.ptp_wait, exit_code):
                    return
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
                if not self._sleep_while_alive(intent.settle_time, exit_code):
                    return
            if not intent.dst_ips:
                logger.warning("No destination IP available for netsniff capture")
                return
            # One pcap holds every stream, so the packet budget -- sized for
            # one stream's frame count -- has to cover all replicas too.
            if self._recorder.packets_capture is not None:
                self._recorder.packets_capture *= max(1, intent.expected_video_streams)
            self._recorder.update_filter(dst_ip=intent.dst_ips)
            self._recorder.capture(capture_time=intent.capture_time)
            logger.info(
                "Started netsniff-ng capture for destination IP %s",
                ", ".join(intent.dst_ips),
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
        or not applicable. A failure -- non-compliant, unconfigured, or
        analyzed with no verdict at all -- returns False when
        ``fail_on_error`` is False and raises ``AssertionError`` when it is
        True.
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

    def close(self, enforce_dispatch: bool = True) -> None:
        """Stop the capture and enforce the evaluated-exactly-once invariant.

        Called from the ``pcap_capture`` fixture's teardown. The real
        upload/poll/verdict runs during the call phase via :meth:`evaluate`;
        this only catches a test that requested the fixture but never called
        it at all -- a required compliance check must not be silently
        skippable that way either.

        *enforce_dispatch* is False when the test already failed before the
        verdict could run: the check legitimately never got its turn, and
        reporting it as "never dispatched" would bury the real failure under
        a second, misleading one.
        """
        self._recorder.stop()
        if not self._evaluated and enforce_dispatch:
            log_fail(
                "Compliance check required (test uses the pcap_capture "
                "fixture) but execute_test() never dispatched it -- ensure "
                "the test calls execute_test(compliance=pcap_capture, ...) "
                "or pcap_capture.skip(...)."
            )

    def _sleep_while_alive(self, seconds: int, exit_code: _ExitCode) -> bool:
        """Sleep *seconds* in slices while the process under test is running.

        True when the whole budget elapsed (or *exit_code* is None), False when
        the process is gone -- the capture is then already skipped and the
        caller must not capture.
        """
        if exit_code is None:
            time.sleep(seconds)
            return True
        deadline = time.monotonic() + seconds
        while True:
            code = exit_code()
            if code is not None:
                self.skip(
                    f"the process under test exited with return code {code} "
                    "before the capture window opened, so it transmitted "
                    "nothing to analyse"
                )
                return False
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return True
            time.sleep(min(_LIVENESS_POLL_INTERVAL, remaining))

    def _fail(self, msg: str, fail_on_error: bool, *, result: str = "Fail") -> None:
        """Record a compliance failure and raise it.

        *result* is the CSV report cell for this test; a caller overrides the
        bare ``"Fail"`` when that alone would misattribute the failure.
        """
        update_compliance_result(self.node_id, result)
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
        disagrees with the configured MTL values, or when the frame period it
        transmitted does not match the configured rate (all always checked, no
        opt-out marker, since a mismatch means the stream isn't actually
        using the requested wire format). Narrow (or narrow linear) is the
        expected default for MTL. Raises ahead of any of that when the capture
        itself lost packets: the analysed capture has to be the same stream
        that was transmitted (see :meth:`_fetch_report`). Where EBU LIST cannot
        name the configured rate, what it derived from that rate is withheld --
        its ST 2110-21 verdict and the narrow/wide tier alike
        (:func:`_verdict_withheld_reason`); every other check here still runs,
        the rate one against its approximation of the rate and the period one
        against the frame period measured off the capture's RTP timestamps.

        When ``fail_on_error`` is True, also records a hard pytest failure
        via ``log_fail``; when False, only logs at INFO so soft-fail callers
        (binary-search/performance loops) can continue without a forced
        abort. Removes the pcap file after upload regardless of the verdict.
        """
        report, withheld = self._fetch_report(fail_on_error, intent)
        self._apply_checks(report, intent, allow_wide, fail_on_error, withheld)

    def _fetch_report(
        self, fail_on_error: bool, intent: CaptureIntent
    ) -> tuple[dict, Optional[str]]:
        """Upload ``self._recorder.pcap_file``; return its report and any withheld reason.

        Raises ``AssertionError`` (via :meth:`_fail`) on any transport
        failure -- the upload command failing, its output not containing the
        expected UUID marker, or the report remaining unavailable/not analyzed
        after polling. Also raises on both analyzed outcomes that are not a
        pass: a non-compliance verdict, and a report carrying no verdict at
        all because the capture held no streams (see
        :func:`no_verdict_reason`) -- the latter only after re-analysing the
        capture, see :data:`_ANALYSIS_ATTEMPTS`. Raises too, ahead of either
        verdict, when the capture lost packets of its own. The one
        non-compliance verdict it does not raise on is one that cannot hold in
        the first place; the second element of the return value is that reason,
        or None (:func:`_verdict_withheld_reason`). Removes the pcap file
        afterward regardless of outcome.
        """
        capturer = self._recorder
        ebu_ip = self.ebu_server.get("ebu_ip", None)
        ebu_login = self.ebu_server.get("user", None)
        ebu_passwd = self.ebu_server.get("password", None)
        ebu_proxy = self.ebu_server.get("proxy", None)
        proxy_cmd = f" --proxy {ebu_proxy}" if ebu_proxy else ""
        try:
            # The password goes over stdin, not in the argument list: mfd_connect
            # logs every command it runs at CMD level, and these logs are the
            # pytest artifact of a public CI run.
            compliance_upl = capturer.host.connection.execute_command(
                "python3 ./tests/acceptance/compliance/upload_pcap.py"
                f" --ip {ebu_ip}"
                f" --user {ebu_login}"
                " --password-stdin"
                f" --pcap '{capturer.pcap_file}'{proxy_cmd}",
                cwd=f"{str(self.mtl_path)}",
                input_data=f"{ebu_passwd}\n",
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
            report = uploader.download_report()
            for attempt in range(2, _ANALYSIS_ATTEMPTS + 1):
                # Only the analyzed-but-empty report is the dropped ingest this
                # recovers. An unavailable one means nothing was analyzed at all
                # -- a down or backlogged analyser -- which re-analysing the
                # same file cannot change.
                if not report or report.get("streams"):
                    break
                logger.warning(
                    "EBU LIST found no streams in PCAP UUID %s; re-analysing the "
                    "capture already on the analyser (attempt %d/%d)",
                    uuid,
                    attempt,
                    _ANALYSIS_ATTEMPTS,
                )
                report = uploader.reanalyze(report)
            if not report:
                # Only this site knows the UUID; no_verdict_reason cannot cite it.
                self._fail(
                    "EBU LIST report unavailable or not analyzed after polling "
                    f"for PCAP UUID {uuid}; compliance was not evaluated",
                    fail_on_error,
                )
            result, report = uploader.check_compliance(report)
            # Ahead of the verdict and whatever it was: a capture with RTP
            # sequence gaps is not the stream that was transmitted, so a
            # "narrow" read off it is no more trustworthy than a "not
            # compliant" one.
            loss = _capture_loss(report)
            if loss:
                missing, present = loss
                percent = 100.0 * missing / (missing + present)
                self._fail(
                    "PCAP compliance check failed: the capture is not a "
                    "faithful copy of the transmitted stream, so no ST "
                    "2110-21 verdict can be drawn from it. EBU LIST counted "
                    f"{missing} packet(s) missing against {present} present, "
                    f"{percent:.1f}% of the sequence span.",
                    fail_on_error,
                    result=f"Fail (capture lost {percent:.1f}% of packets)",
                )
            # Resolved here, once, for both halves of the verdict: it decides
            # whether a non-compliance below can be reported as one, and
            # _apply_checks needs the same answer for the tier drawn from the
            # same rate. Unconditional because a report that passed still has a
            # tier that cannot hold.
            withheld = _verdict_withheld_reason(report, intent.framerate)
            if not result and not withheld:
                logger.info(f"Compliance report: {report}")
                self._fail(
                    no_verdict_reason(report)
                    or "EBU LIST analyzed the PCAP and reported non-compliance",
                    fail_on_error,
                )
            if withheld:
                logger.warning("ST 2110-21 verdict withheld: %s", withheld)
            return report, withheld
        finally:
            try:
                # netsniff-ng captures under sudo, so the pcap belongs to root.
                # The default pcap_dir is /tmp, which is sticky, and there the
                # unprivileged test account cannot unlink a root-owned file --
                # every run would leave its capture behind and eventually fill
                # the filesystem with multi-gigabyte pcaps.
                capturer.host.connection.execute_command(
                    f"sudo rm -f '{capturer.pcap_file}'"
                )
                logger.debug(f"Removed pcap file: {capturer.pcap_file}")
            except ConnectionCalledProcessError as e:
                logger.warning(f"Failed to remove pcap file: {e}")

    def _apply_checks(
        self,
        report: dict,
        intent: CaptureIntent,
        allow_wide: bool,
        fail_on_error: bool,
        withheld: Optional[str],
    ) -> None:
        """Compare *report* against *intent*'s expected wire format; raises on the first mismatch.

        A pure function of ``(report, intent)`` plus the ``allow_wide``/
        ``fail_on_error`` policy knobs and *withheld*, the reason EBU LIST's ST
        2110-21 verdict cannot hold (:func:`_verdict_withheld_reason`) -- no
        network I/O, so it is directly unit-testable against a saved report.
        """
        node_id = self.node_id
        video_streams = _video_streams(report)
        logger.info(
            "EBU LIST analysed %d video stream(s); expected %d across %d destination(s)",
            len(video_streams),
            intent.expected_video_streams,
            len(intent.dst_ips),
        )
        if len(video_streams) != intent.expected_video_streams:
            self._fail(
                "PCAP compliance check failed: EBU LIST analysed "
                f"{len(video_streams)} video stream(s) for "
                f"{intent.expected_video_streams} expected stream(s) across "
                f"destination(s) {', '.join(intent.dst_ips)}",
                fail_on_error,
            )
        wide_streams = _wide_video_streams(report)
        # "wide" is the worse of the VRX/Cinst sub-verdicts, so where those were
        # computed against a rate the stream never used this tier cannot pick out
        # a failure any more than it could confirm a pass. Enforcing it there
        # would reproduce, in the tier, the exact false failure being withheld.
        if wide_streams and not allow_wide and not withheld:
            msg = (
                f"PCAP compliance check failed: {len(wide_streams)} video "
                "stream(s) are only ST 2110-21 'wide' compliant (not "
                'narrow/narrow_linear). If pacing="wide" wasn\'t configured '
                "for this test but wide compliance is still expected/"
                "acceptable, mark it with @pytest.mark.allow_wide_compliance."
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

        period_mismatch = _frame_period_mismatch_streams(report, intent.framerate)
        if period_mismatch:
            measured = (
                period_mismatch[0]
                .get("analyses", {})
                .get("inter_frame_rtp_ts_delta", {})
                .get("details", {})
                .get("range", {})
            )
            exact, low, high = _frame_period_ticks(intent.framerate)
            msg = (
                f"PCAP compliance check failed: {len(period_mismatch)} video "
                "stream(s) transmitted a frame period that disagrees with the "
                f"configured framerate={intent.framerate!r}. At {_MEDIA_CLOCK_HZ} Hz "
                f"that rate is {exact:g} RTP ticks per frame, so every "
                f"inter-frame delta must fall in [{low}..{high}]; the capture "
                f"measured [{measured.get('min')}..{measured.get('max')}]."
            )
            self._fail(msg, fail_on_error)

        if withheld:
            update_compliance_result(node_id, "Pass (2110-21 verdict withheld)")
            logger.warning(
                "PCAP checks passed and the transmitted frame period matches, but "
                "no ST 2110-21 timing verdict was recorded: %s",
                withheld,
            )
        elif wide_streams:
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

    def arm(self, intent: CaptureIntent, *, exit_code: _ExitCode = None) -> None:
        pass

    def evaluate(self, intent: CaptureIntent, fail_on_error: bool = True) -> bool:
        return True

    def close(self, enforce_dispatch: bool = True) -> None:
        pass


if TYPE_CHECKING:
    # Static-only proof that both classes implement ComplianceCheck's full
    # surface -- a mypy/pyright run fails here (not just at a call site) the
    # moment either class drifts out of parity. No runtime cost/effect.
    _null_check: ComplianceCheck = _NullComplianceSession()
    _session_check: ComplianceCheck = ComplianceSession(None, {}, None, "")

NO_COMPLIANCE = _NullComplianceSession()
