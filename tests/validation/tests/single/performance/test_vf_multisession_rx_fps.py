# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VF Multi-Session RX FPS Performance Tests

Tests RX performance using Virtual Functions (VFs) on a single NIC.
- RX sessions run on VF0 with companion TX on VF1 (same physical NIC)
- Tests multiple sessions (1, 2, 4, 8, 16) with sch_session_quota=60
- Performance validated by checking received frames and frame rate
- Test fails if RX sessions don't receive expected traffic
"""

import logging
import os
import re
import time

import pytest
from mfd_common_libs.log_levels import TEST_FAIL, TEST_PASS
from mtl_engine.execute import log_fail
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


def monitor_rx_performance(
    log_lines: list, num_sessions: int, expected_frames_min: int = 100
) -> tuple:
    """Monitor RX performance from RxTxApp log output."""
    rx_frames_pattern = re.compile(
        r"app_rx_st20p_result\((\d+)\),\s+OK,\s+fps\s+[\d.]+,\s+(\d+)\s+frame received"
    )

    session_frames = {}
    for line in log_lines:
        match = rx_frames_pattern.search(line)
        if match:
            session_id = int(match.group(1))
            frames_received = int(match.group(2))
            if (
                session_id not in session_frames
                or frames_received > session_frames[session_id]
            ):
                session_frames[session_id] = frames_received

    successful_sessions = {
        sid for sid, frames in session_frames.items() if frames >= expected_frames_min
    }

    details = {
        "successful_count": len(successful_sessions),
        "total_sessions": num_sessions,
        "successful_sessions": sorted(list(successful_sessions)),
        "session_frames": session_frames,
        "expected_frames_min": expected_frames_min,
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
def test_vf_multisession_rx_fps(
    hosts,
    build,
    nic_port_list,
    test_time,
    media_file,
    num_sessions,
):
    """Test RX multi-session performance using VFs."""
    host = list(hosts.values())[0]
    media_file_info, media_file_path = media_file
    expected_fps = int(media_file_info["fps"])

    if num_sessions <= 2:
        tolerance = 0.85
    elif num_sessions <= 4:
        tolerance = 0.80
    elif num_sessions <= 8:
        tolerance = 0.70
    else:
        tolerance = 0.50

    expected_frames_min = int(test_time * expected_fps * tolerance)

    if not hasattr(host, "vfs") or len(host.vfs) < 2:
        pytest.skip("Test requires at least 2 VFs on host")

    rx_vf = host.vfs[0]
    tx_vf = host.vfs[1]

    logger.info("=" * 80)
    logger.info(f"VF Multi-Session RX: {num_sessions} sessions, RX={rx_vf}, TX={tx_vf}")
    logger.info(f"Target: {expected_fps} fps, Min frames: {expected_frames_min}")
    logger.info("=" * 80)

    tx_config_path = f"{build}/tests/vf_rx_companion_tx_{num_sessions}s_config.json"
    tx_app = RxTxApp(
        app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=tx_config_path
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
        source_ip="192.168.31.101",
        destination_ip="192.168.31.102",
        port=20000,
        replicas=num_sessions,
        sch_session_quota=60,
        test_time=test_time + 10,
    )

    companion_log_path = f"{build}/tests/vf_rx_companion_tx_{num_sessions}s.log"
    logger.info(f"Starting companion TX: {companion_log_path}")
    tx_process = host.connection.start_process(
        f"{tx_app.command} > {companion_log_path} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)
    logger.info("Companion TX started")

    try:
        rx_config_path = f"{build}/tests/vf_rx_{num_sessions}s_config.json"
        rx_app = RxTxApp(
            app_path=f"{build}/tests/tools/RxTxApp/build",
            config_file_path=rx_config_path,
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
            source_ip="192.168.31.101",
            destination_ip="192.168.31.102",
            port=20000,
            replicas=num_sessions,
            sch_session_quota=60,
            test_time=test_time,
        )

        rx_log_path = f"{build}/tests/vf_rx_{num_sessions}s.log"
        logger.info(f"Running RX: {rx_log_path}")

        from mtl_engine.execute import run

        result = run(
            rx_app.command,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
        )

        with open(rx_log_path, "w") as f:
            f.write(result.stdout_text)

        for line in result.stdout_text.splitlines():
            logger.debug(f"[RX] {line}")

        if result.return_code != 0:
            log_fail(f"RX process failed with exit code {result.return_code}")
            pytest.fail(f"RX process failed with exit code {result.return_code}")

        success, successful_count, rx_details = monitor_rx_performance(
            result.stdout_text.splitlines(), num_sessions, expected_frames_min
        )

        logger.info("=" * 80)
        logger.info(
            f"RX Results: {successful_count}/{num_sessions} sessions successful"
        )
        for session_id in range(num_sessions):
            frames = rx_details["session_frames"].get(session_id, 0)
            status = "✓" if session_id in rx_details["successful_sessions"] else "✗"
            logger.info(f"  Session {session_id}: {frames} frames {status}")
        logger.info("=" * 80)

        if not success:
            failure_msg = (
                f"Only {successful_count}/{num_sessions} sessions "
                f"received sufficient frames (min {expected_frames_min})"
            )
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"RX test passed: {num_sessions} sessions")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX: {e}")
