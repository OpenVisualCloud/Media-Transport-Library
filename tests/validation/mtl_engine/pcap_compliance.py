# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""EBU LIST pcap-compliance upload/poll/verdict, run from the test call phase."""

import logging

from compliance.compliance_client import PcapComplianceClient
from mfd_connect.exceptions import ConnectionCalledProcessError

from .csv_report import update_compliance_result
from .execute import log_fail

logger = logging.getLogger(__name__)


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
    streams = report.get("streams") or []
    return [
        s
        for s in streams
        if s.get("media_type") == "video"
        and s.get("global_video_analysis", {}).get("compliance") == "wide"
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
    streams = report.get("streams") or []
    return [
        s
        for s in streams
        if s.get("media_type") == "video"
        and s.get("media_specific", {}).get("packing_mode") not in (None, expected_mode)
    ]


def check_pcap_compliance(
    capturer,
    ebu_server: dict,
    mtl_path,
    node_id: str,
    fail_on_error: bool = True,
    allow_wide: bool = False,
    expected_packing=None,
) -> None:
    """Upload ``capturer.pcap_file`` to the EBU LIST analyser and verify compliance.

    Raises ``AssertionError`` when the capture is not compliant, when a video
    stream is only ST 2110-21 "wide" compliant (not narrow/narrow_linear) and
    ``allow_wide`` is False, or when a video stream's EBU LIST ``packing_mode``
    disagrees with *expected_packing* (the configured MTL packing mode --
    always checked, no opt-out marker, since a mismatch means the stream
    isn't using the packing mode the test actually asked for). Narrow (or
    narrow linear) is the expected default for MTL; a "wide" verdict most
    often means the capture PF and the primary/TX PF are not properly
    PTP-synchronized (sync the system clock to the capturing NIC's PHC via
    phc2sys before capture). ``ApplicationBase._dispatch_compliance_check``
    sets ``allow_wide`` automatically whenever the test configured
    ``pacing="wide"`` (ST21_PACING_WIDE deliberately widens VRX/Cinst
    tolerance, so a wide verdict there is the requested behavior, not a
    defect); tests where wide compliance is expected for some other,
    non-obvious reason should opt in explicitly with
    ``@pytest.mark.allow_wide_compliance`` instead.

    When ``fail_on_error`` is True, also records a hard pytest failure via
    ``log_fail``; when False, only logs at INFO so soft-fail callers
    (binary-search/performance loops) can continue without a forced abort.
    Removes the pcap file after upload regardless of the verdict.
    """
    ebu_ip = ebu_server.get("ebu_ip", None)
    ebu_login = ebu_server.get("user", None)
    ebu_passwd = ebu_server.get("password", None)
    ebu_proxy = ebu_server.get("proxy", None)
    proxy_cmd = f" --proxy {ebu_proxy}" if ebu_proxy else ""
    try:
        compliance_upl = capturer.host.connection.execute_command(
            "python3 ./tests/validation/compliance/upload_pcap.py"
            f" --ip {ebu_ip}"
            f" --user {ebu_login}"
            f" --password {ebu_passwd}"
            f" --pcap '{capturer.pcap_file}'{proxy_cmd}",
            cwd=f"{str(mtl_path)}",
        )
        if compliance_upl.return_code != 0:
            logger.error(f"PCAP upload failed: {compliance_upl.stderr}")
            return
        uuid = compliance_upl.stdout.split(">>>UUID: ")[1].strip()
        logger.debug(f"PCAP successfully uploaded to EBU LIST with UUID: {uuid}")
        uploader = PcapComplianceClient(
            ebu_ip=ebu_ip,
            user=ebu_login,
            password=ebu_passwd,
            pcap_id=uuid,
            proxies={"http": ebu_proxy, "https": ebu_proxy},
        )
        result, report = uploader.check_compliance()
        if result:
            wide_streams = _wide_video_streams(report or {})
            if wide_streams and not allow_wide:
                update_compliance_result(node_id, "Fail")
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
                if fail_on_error:
                    log_fail(msg)
                else:
                    logger.info(
                        "PCAP compliance soft-fail (fail_on_error=False): %s", msg
                    )
                raise AssertionError(msg)
            packing_mismatch = _packing_mismatch_streams(report or {}, expected_packing)
            if packing_mismatch:
                update_compliance_result(node_id, "Fail")
                msg = (
                    f"PCAP compliance check failed: {len(packing_mismatch)} video "
                    f"stream(s) report an EBU LIST packing_mode that disagrees with "
                    f"the configured packing={expected_packing!r} -- the stream is not "
                    "actually using the requested packing mode on the wire."
                )
                if fail_on_error:
                    log_fail(msg)
                else:
                    logger.info(
                        "PCAP compliance soft-fail (fail_on_error=False): %s", msg
                    )
                raise AssertionError(msg)
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
            return
        update_compliance_result(node_id, "Fail")
        logger.info(f"Compliance report: {report}")
        msg = "PCAP compliance check failed"
        if fail_on_error:
            log_fail(msg)
        else:
            logger.info("PCAP compliance soft-fail (fail_on_error=False): %s", msg)
        raise AssertionError(msg)
    finally:
        try:
            capturer.host.connection.execute_command(f"rm -f '{capturer.pcap_file}'")
            logger.debug(f"Removed pcap file: {capturer.pcap_file}")
        except ConnectionCalledProcessError as e:
            logger.warning(f"Failed to remove pcap file: {e}")
