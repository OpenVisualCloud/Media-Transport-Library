# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VF Multi-Session TX FPS Performance Tests

Tests TX performance using Virtual Functions (VFs) on a single NIC.
- TX sessions run on VF0 with companion RX on VF1 (same physical NIC)
- Tests multiple sessions (1, 2, 4, 8, 16) with sch_session_quota=60
- Performance validated by checking FPS in live logs
- Test fails if FPS drops below target (with 2 fps tolerance)
"""

import logging
import os
import re
import threading
import time

import pytest
from mfd_common_libs.log_levels import TEST_FAIL, TEST_PASS
from mtl_engine.execute import log_fail
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


def monitor_fps_in_logs(
    log_lines: list, expected_fps: float, num_sessions: int, fps_tolerance: float = 2.0
) -> tuple:
    """Monitor FPS from RxTxApp log output."""
    fps_pattern = re.compile(
        r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)"
    )

    successful_sessions = set()
    session_fps_history = {}

    for line in log_lines:
        match = fps_pattern.search(line)
        if match:
            session_id = int(match.group(2))
            actual_fps = float(match.group(3))

            if session_id not in session_fps_history:
                session_fps_history[session_id] = []
            session_fps_history[session_id].append(actual_fps)

            if abs(actual_fps - expected_fps) <= fps_tolerance:
                successful_sessions.add(session_id)

    details = {
        "successful_count": len(successful_sessions),
        "total_sessions": num_sessions,
        "successful_sessions": sorted(list(successful_sessions)),
        "session_fps_history": session_fps_history,
    }

    return len(successful_sessions) == num_sessions, len(successful_sessions), details


@pytest.mark.smoke
@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 16],
    ids=["1sess", "2sess", "4sess", "8sess", "16sess"],
)
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["1080p"],
)
def test_vf_multisession_tx_fps(
    hosts,
    build,
    nic_port_list,
    test_time,
    media_file,
    num_sessions,
):
    """Test TX multi-session performance using VFs with FPS validation."""
    host = list(hosts.values())[0]
    media_file_info, media_file_path = media_file
    expected_fps = int(media_file_info["fps"])

    if not hasattr(host, "vfs") or len(host.vfs) < 2:
        pytest.skip("Test requires at least 2 VFs on host")

    tx_vf = host.vfs[0]
    rx_vf = host.vfs[1]

    logger.info("=" * 80)
    logger.info(f"VF Multi-Session TX: {num_sessions} sessions, TX={tx_vf}, RX={rx_vf}")
    logger.info(f"Target: {expected_fps} fps")
    logger.info("=" * 80)

    rx_config_path = f"{build}/tests/vf_tx_companion_rx_{num_sessions}s_config.json"
    rx_app = RxTxApp(
        app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=rx_config_path
    )

    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_file_info["format"],
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{expected_fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        source_ip="192.168.30.101",
        destination_ip="192.168.30.102",
        port=20000,
        replicas=num_sessions,
        sch_session_quota=60,
        test_time=test_time + 10,
    )

    companion_log_path = f"{build}/tests/vf_tx_companion_rx_{num_sessions}s.log"
    logger.info(f"Starting companion RX: {companion_log_path}")
    rx_process = host.connection.start_process(
        f"{rx_app.command} > {companion_log_path} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)
    logger.info("Companion RX started")

    try:
        tx_config_path = f"{build}/tests/vf_tx_{num_sessions}s_config.json"
        tx_app = RxTxApp(
            app_path=f"{build}/tests/tools/RxTxApp/build",
            config_file_path=tx_config_path,
        )

        tx_app.create_command(
            session_type="st20p",
            direction="tx",
            test_mode="unicast",
            transport_format=media_file_info["format"],
            width=media_file_info["width"],
            height=media_file_info["height"],
            framerate=f"p{expected_fps}",
            input_file=media_file_path,
            nic_port=tx_vf,
            source_ip="192.168.30.101",
            destination_ip="192.168.30.102",
            port=20000,
            replicas=num_sessions,
            sch_session_quota=60,
            test_time=test_time,
        )

        tx_log_path = f"{build}/tests/vf_tx_{num_sessions}s.log"
        logger.info(f"Running TX: {tx_log_path}")

        from mtl_engine.execute import run

        result = run(
            tx_app.command,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
        )

        with open(tx_log_path, "w") as f:
            f.write(result.stdout_text)

        for line in result.stdout_text.splitlines():
            logger.debug(f"[TX] {line}")

        if result.return_code != 0:
            log_fail(f"TX process failed with exit code {result.return_code}")
            pytest.fail(f"TX process failed with exit code {result.return_code}")

        success, successful_count, fps_details = monitor_fps_in_logs(
            result.stdout_text.splitlines(), expected_fps, num_sessions
        )

        logger.info("=" * 80)
        logger.info(
            f"TX FPS Results: {successful_count}/{num_sessions} sessions at target"
        )
        for session_id, fps_history in fps_details["session_fps_history"].items():
            if fps_history:
                avg_fps = sum(fps_history) / len(fps_history)
                min_fps = min(fps_history)
                max_fps = max(fps_history)
                status = (
                    "✓" if session_id in fps_details["successful_sessions"] else "✗"
                )
                logger.info(
                    f"  Session {session_id}: avg={avg_fps:.1f}, "
                    f"min={min_fps:.1f}, max={max_fps:.1f} fps {status}"
                )
        logger.info("=" * 80)

        if not success:
            failure_msg = (
                f"Only {successful_count}/{num_sessions} sessions "
                f"reached target {expected_fps} fps (±2 tolerance)"
            )
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(
            TEST_PASS, f"TX test passed: {num_sessions} sessions @ {expected_fps} fps"
        )

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX: {e}")
