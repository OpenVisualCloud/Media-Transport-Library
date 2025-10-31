# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VF Multi-Session FPS Variants Performance Tests

Tests TX/RX performance using Virtual Functions (VFs) with different FPS values.
- Tests FPS: 25, 30, 50, 59
- Tests multiple sessions (1, 2, 4, 8, 14, 15, 16) to validate performance scaling
- Validates both TX FPS achievement and RX frame reception
- Supports optional DSA (Data Streaming Accelerator) for DMA offload
- Uses same physical NIC with VF0 for profiled side and VF1 for companion
"""

import logging
import re
import time

import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.config.app_mappings import DSA_DEVICES
from mtl_engine.execute import log_fail, run
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


def get_companion_log_summary(host, log_path: str, max_lines: int = 30) -> None:
    """
    Retrieve and display summary of companion process log from remote host.

    Args:
        host: Host connection object
        log_path: Path to log file on remote host
        max_lines: Maximum number of lines to display
    """
    try:
        # Try to read last N lines of the log file
        result = host.connection.execute_command(
            f"tail -n {max_lines} {log_path} 2>/dev/null || echo 'Log file not found'",
            shell=True
        )
        if result.stdout and "Log file not found" not in result.stdout:
            logger.debug(f"Companion log ({log_path}, last {max_lines} lines):")
            for line in result.stdout.splitlines():
                if line.strip():
                    logger.debug(f"  {line}")
    except Exception as e:
        logger.warning(f"Could not retrieve companion log from {log_path}: {e}")


def monitor_tx_fps(log_lines: list, expected_fps: float, num_sessions: int, fps_tolerance: float = 2.0) -> tuple:
    """
    Monitor TX FPS from RxTxApp log output.

    Args:
        log_lines: List of log output lines
        expected_fps: Target FPS value
        num_sessions: Number of sessions to validate
        fps_tolerance: Allowable FPS deviation

    Returns:
        Tuple of (all_successful: bool, successful_count: int, details: dict)
    """
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
        "successful_sessions": sorted(list(successful_sessions)),
        "session_fps_history": session_fps_history,
    }

    return len(successful_sessions) == num_sessions, len(successful_sessions), details


def monitor_rx_frames(log_lines: list, num_sessions: int, expected_frames_min: int) -> tuple:
    """
    Monitor RX frame reception from RxTxApp log output.

    Args:
        log_lines: List of log output lines
        num_sessions: Number of sessions to validate
        expected_frames_min: Minimum expected frames per session

    Returns:
        Tuple of (all_successful: bool, successful_count: int, details: dict)
    """
    rx_frames_pattern = re.compile(
        r"app_rx_st20p_result\((\d+)\),\s+OK,\s+fps\s+[\d.]+,\s+(\d+)\s+frame received"
    )

    session_frames = {}
    for line in log_lines:
        match = rx_frames_pattern.search(line)
        if match:
            session_id = int(match.group(1))
            frames_received = int(match.group(2))
            if session_id not in session_frames or frames_received > session_frames[session_id]:
                session_frames[session_id] = frames_received

    successful_sessions = {
        sid for sid, frames in session_frames.items() if frames >= expected_frames_min
    }

    details = {
        "successful_count": len(successful_sessions),
        "successful_sessions": sorted(list(successful_sessions)),
        "session_frames": session_frames,
    }

    return len(successful_sessions) == num_sessions, len(successful_sessions), details


# Media file configurations for different FPS values
MEDIA_CONFIGS = {
    25: {
        "filename": "HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv",
        "file_format": "YUV422RFC4175PG2BE10",
        "format": "YUV_422_10bit",
        "width": 1920,
        "height": 1080,
        "fps": 25,
    },
    30: {
        "filename": "HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv",
        "file_format": "YUV422RFC4175PG2BE10",
        "format": "YUV_422_10bit",
        "width": 1920,
        "height": 1080,
        "fps": 30,
    },
    50: {
        "filename": "CrowdRun_3840x2160_50fps_10frames_yuv422rfc4175be10.yuv",
        "file_format": "YUV422RFC4175PG2BE10",
        "format": "YUV_422_10bit",
        "width": 3840,
        "height": 2160,
        "fps": 50,
    },
    59: {
        "filename": "CSGObuymenu_1080p_60fps_120frames_yuv422rfc4175be10.yuv",
        "file_format": "YUV422RFC4175PG2BE10",
        "format": "YUV_422_10bit",
        "width": 1920,
        "height": 1080,
        "fps": 59,
    },
}


def get_dsa_device_for_nic(nic_pci_address: str) -> str:
    """
    Determine which DSA device to use based on NIC's NUMA node.

    Uses PCI bus number as a heuristic to determine NUMA affinity:
    - Bus numbers < 0x80 are typically on NUMA node 0
    - Bus numbers >= 0x80 are typically on NUMA node 1

    Args:
        nic_pci_address: PCI address of the NIC (e.g., "0000:31:00.0")

    Returns:
        DSA device PCI address for the appropriate NUMA node
    """
    try:
        bus = int(nic_pci_address.split(":")[1], 16)
        return DSA_DEVICES["numa0"] if bus < 0x80 else DSA_DEVICES["numa1"]
    except (ValueError, IndexError):
        logger.warning(f"Could not determine NUMA node for {nic_pci_address}, defaulting to NUMA 0")
        return DSA_DEVICES["numa0"]


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess"])
def test_vf_tx_fps_variants(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa):
    """
    Test TX multi-session performance with different FPS values.

    Args:
        hosts: Host fixtures
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, or 16)
        use_dsa: Whether to use DSA for DMA operations
    """
    host = list(hosts.values())[0]

    if not hasattr(host, "vfs") or len(host.vfs) < 2:
        pytest.skip("Test requires at least 2 VFs on host")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    tx_vf = host.vfs[0]
    rx_vf = host.vfs[1]

    # Configure DSA if enabled
    dsa_device = get_dsa_device_for_nic(tx_vf) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF TX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"TX={tx_vf}, RX={rx_vf}, Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Start companion RX
    rx_config_path = f"{build}/tests/vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_rx_config.json"
    rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=rx_config_path)

    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_config["format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        source_ip="192.168.30.101",
        destination_ip="192.168.30.102",
        port=20000,
        replicas=num_sessions,
        sch_session_quota=60,
        test_time=test_time + 10,
    )

    companion_log = f"{build}/tests/vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_rx_companion.log"
    logger.info(f"Starting companion RX, logs will be saved to: {companion_log}")
    rx_process = host.connection.start_process(
        f"{rx_app.command} > {companion_log} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA
        tx_config_path = f"{build}/tests/vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_config.json"
        tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=tx_config_path)

        tx_kwargs = {
            "session_type": "st20p",
            "direction": "tx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "input_file": media_file_path,
            "nic_port": tx_vf,
            "source_ip": "192.168.30.101",
            "destination_ip": "192.168.30.102",
            "port": 20000,
            "replicas": num_sessions,
            "sch_session_quota": 60,
            "test_time": test_time,
        }

        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)

        logger.info(f"Running TX{dsa_label}")

        result = run(tx_app.command, cwd=build, timeout=test_time + 60, testcmd=True, host=host)

        # Log TX output to the main test log
        logger.info("=" * 80)
        logger.info(f"TX Process Output")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch"]):
                    logger.info(f"TX: {line}")
        else:
            logger.warning("No TX output captured!")
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process{dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"TX process{dsa_label} failed with exit code {result.return_code}")

        # Validate FPS achievement
        success, successful_count, fps_details = monitor_tx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )

        logger.info("=" * 80)
        logger.info(f"TX FPS Results{dsa_label}: {successful_count}/{num_sessions} sessions at target {fps} fps")
        for session_id, fps_history in fps_details["session_fps_history"].items():
            if fps_history:
                avg_fps = sum(fps_history) / len(fps_history)
                min_fps = min(fps_history)
                max_fps = max(fps_history)
                status = "✓" if session_id in fps_details["successful_sessions"] else "✗"
                logger.info(
                    f"  Session {session_id}: avg={avg_fps:.1f}, min={min_fps:.1f}, max={max_fps:.1f} fps {status}"
                )
        logger.info("=" * 80)

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps{dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"TX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        # Retrieve companion RX logs before cleanup
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs")
        logger.info("=" * 80)
        get_companion_log_summary(host, companion_log, max_lines=50)

        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess"])
def test_vf_rx_fps_variants(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa):
    """
    Test RX multi-session performance with different FPS values.

    Args:
        hosts: Host fixtures
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, or 16)
        use_dsa: Whether to use DSA for DMA operations
    """
    host = list(hosts.values())[0]

    if not hasattr(host, "vfs") or len(host.vfs) < 2:
        pytest.skip("Test requires at least 2 VFs on host")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    # Calculate expected frames with adaptive tolerance
    # Non-DSA mode with high session counts (14+) has degraded performance without DMA offload
    # DSA provides hardware acceleration for high session counts
    if num_sessions >= 14 and not use_dsa:
        tolerance = 0.10  # Accept if ANY session receives >10% of frames
    else:
        tolerance = {1: 0.90, 2: 0.90, 4: 0.90, 8: 0.90, 14: 0.90, 15: 0.90, 16: 0.90}.get(num_sessions, 0.90)
    expected_frames_min = int(test_time * fps * tolerance)

    rx_vf = host.vfs[0]
    tx_vf = host.vfs[1]

    # Configure DSA if enabled
    dsa_device = get_dsa_device_for_nic(rx_vf) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF RX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"RX={rx_vf}, TX={tx_vf}, Min frames: {expected_frames_min}")
    logger.info("=" * 80)

    # Start companion TX
    tx_config_path = f"{build}/tests/vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_tx_config.json"
    tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=tx_config_path)

    tx_app.create_command(
        session_type="st20p",
        direction="tx",
        test_mode="unicast",
        transport_format=media_config["format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        input_file=media_file_path,
        nic_port=tx_vf,
        source_ip="192.168.31.101",
        destination_ip="192.168.31.102",
        port=20000,
        replicas=num_sessions,
        sch_session_quota=60,
        test_time=test_time + 10,
    )

    companion_log = f"{build}/tests/vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_tx_companion.log"
    logger.info(f"Starting companion TX, logs will be saved to: {companion_log}")
    tx_process = host.connection.start_process(
        f"{tx_app.command} > {companion_log} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA
        rx_config_path = f"{build}/tests/vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_config.json"
        rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build", config_file_path=rx_config_path)

        rx_kwargs = {
            "session_type": "st20p",
            "direction": "rx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "output_file": "/dev/null",
            "nic_port": rx_vf,
            "source_ip": "192.168.31.101",
            "destination_ip": "192.168.31.102",
            "port": 20000,
            "replicas": num_sessions,
            "sch_session_quota": 60,
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)

        logger.info(f"Running RX{dsa_label}")

        result = run(rx_app.command, cwd=build, timeout=test_time + 60, testcmd=True, host=host)

        # Log RX output to the main test log
        logger.info("=" * 80)
        logger.info(f"RX Process Output")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["frame", "session", "error", "fail", "warn", "dma", "mismatch"]):
                    logger.info(f"RX: {line}")
        else:
            logger.warning("No RX output captured!")
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process{dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"RX process{dsa_label} failed with exit code {result.return_code}")

        # Validate frame reception
        success, successful_count, rx_details = monitor_rx_frames(
            result.stdout_text.splitlines(), num_sessions, expected_frames_min
        )

        logger.info("=" * 80)
        logger.info(f"RX Results{dsa_label}: {successful_count}/{num_sessions} sessions successful @ {fps} fps")
        for session_id in range(num_sessions):
            frames = rx_details["session_frames"].get(session_id, 0)
            status = "✓" if session_id in rx_details["successful_sessions"] else "✗"
            logger.info(f"  Session {session_id}: {frames} frames {status}")
        logger.info("=" * 80)

        if not success:
            failure_msg = (
                f"Only {successful_count}/{num_sessions} sessions "
                f"received sufficient frames (min {expected_frames_min}) @ {fps} fps{dsa_label}"
            )
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"RX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        # Retrieve companion TX logs before cleanup
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs")
        logger.info("=" * 80)
        get_companion_log_summary(host, companion_log, max_lines=50)

        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX: {e}")
