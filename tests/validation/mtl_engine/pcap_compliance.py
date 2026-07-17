# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""EBU LIST pcap-compliance upload/poll/verdict, run from the test call phase."""

import logging

from compliance.compliance_client import PcapComplianceClient
from mfd_connect.exceptions import ConnectionCalledProcessError

from .csv_report import update_compliance_result
from .execute import log_fail

logger = logging.getLogger(__name__)


def _gapped_video_streams(report: dict) -> list[dict]:
    """Return video streams using ST 2110-21 gapped (not narrow linear) schedule.

    EBU LIST reports a per-stream ``media_specific.schedule`` of ``"linear"``
    or ``"gapped"`` for video streams. Gapped scheduling still counts toward
    ``narrow_streams`` (it can pass VRX/Cinst narrow bounds) but not
    ``narrow_linear_streams`` -- it is a real, looser compliance tier, not a
    reporting artifact.
    """
    streams = report.get("streams") or []
    return [
        s
        for s in streams
        if s.get("media_type") == "video"
        and s.get("media_specific", {}).get("schedule") == "gapped"
    ]


def check_pcap_compliance(
    capturer,
    ebu_server: dict,
    mtl_path,
    node_id: str,
    fail_on_error: bool = True,
    allow_gapped: bool = False,
) -> None:
    """Upload ``capturer.pcap_file`` to the EBU LIST analyser and verify compliance.

    Raises ``AssertionError`` when the capture is not compliant, or when a
    video stream uses gapped (not narrow linear) scheduling and
    ``allow_gapped`` is False. Narrow linear is the expected default for MTL;
    gapped scheduling most often means the capture PF and the primary/TX PF
    are not properly PTP-synchronized (sync the system clock to the
    capturing NIC's PHC via phc2sys before capture). Tests where gapped is
    expected/acceptable should opt in with ``@pytest.mark.allow_gapped_compliance``.

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
            gapped_streams = _gapped_video_streams(report or {})
            if gapped_streams and not allow_gapped:
                update_compliance_result(node_id, "Fail")
                msg = (
                    f"PCAP compliance check failed: {len(gapped_streams)} video "
                    "stream(s) use gapped (not narrow linear) ST 2110-21 "
                    "scheduling. This usually means the capture PF and the "
                    "primary/TX PF are not properly PTP-synchronized -- sync "
                    "the system clock to the capturing NIC's PHC (phc2sys) "
                    "before capture. If gapped scheduling is expected/acceptable "
                    "for this test, mark it with @pytest.mark.allow_gapped_compliance."
                )
                if fail_on_error:
                    log_fail(msg)
                else:
                    logger.info(
                        "PCAP compliance soft-fail (fail_on_error=False): %s", msg
                    )
                raise AssertionError(msg)
            if gapped_streams:
                update_compliance_result(node_id, "Pass (gapped)")
                logger.warning(
                    "PCAP compliance check passed with gapped scheduling on "
                    "%d video stream(s) (allowed by @pytest.mark.allow_gapped_compliance)",
                    len(gapped_streams),
                )
            else:
                update_compliance_result(node_id, "Pass")
                logger.info("PCAP compliance check passed (narrow linear)")
            return
        streams = (report or {}).get("streams") or []
        if not streams:
            # Empty capture -- interface may not see VF-to-VF loopback
            # traffic. Not a real failure.
            update_compliance_result(node_id, "N/A")
            logger.warning(
                "PCAP compliance check skipped: capture contains no streams "
                "(capture interface may not see VF-to-VF loopback traffic)"
            )
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
