# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VF Multi-Session FPS Variants Performance Tests

Tests TX/RX performance using Virtual Functions (VFs) with different FPS values.
- Tests FPS: 25, 30, 50, 59
- Tests multiple sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32) to validate performance scaling
- Validates both TX FPS achievement and RX frame reception
- Supports optional DSA (Data Streaming Accelerator) for DMA offload
- Uses same physical NIC with VF0 for profiled side and VF1 for companion
- Skips first 30 seconds of measurements to exclude unstable initialization period
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

# Configuration: FPS measurement warmup period (seconds)
# This warmup period is excluded from FPS measurements to avoid unstable initialization
FPS_WARMUP_SECONDS = 30
FPS_TOLERANCE_PCT = 0.99  # 99% of requested FPS required for pass


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


def _monitor_fps_generic(log_lines: list, expected_fps: float, num_sessions: int,
                         session_pattern: str, fps_tolerance_pct: float = FPS_TOLERANCE_PCT,
                         warmup_seconds: int = FPS_WARMUP_SECONDS) -> tuple:
    """
    Generic FPS monitoring from RxTxApp log output (used by both TX and RX).

    Args:
        log_lines: List of log output lines
        expected_fps: Target FPS value
        num_sessions: Number of sessions to validate
        session_pattern: Regex pattern for matching session FPS logs (TX_VIDEO_SESSION or RX_VIDEO_SESSION)
        fps_tolerance_pct: Required percentage of target FPS
        warmup_seconds: Number of seconds to skip at start for initialization

    Returns:
        Tuple of (all_successful: bool, successful_count: int, details: dict)
    """
    from datetime import datetime

    fps_pattern = re.compile(session_pattern)
    timestamp_pattern = re.compile(r"MTL:\s+(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})")

    successful_sessions = set()
    session_fps_history = {}
    start_timestamp = None
    min_required_fps = expected_fps * fps_tolerance_pct

    for line in log_lines:
        # Track timestamps to identify warmup period
        ts_match = timestamp_pattern.search(line)
        if ts_match and start_timestamp is None:
            start_timestamp = datetime.strptime(ts_match.group(1), "%Y-%m-%d %H:%M:%S")

        match = fps_pattern.search(line)
        if match:
            session_id = int(match.group(2))
            actual_fps = float(match.group(3))

            # Calculate elapsed time from start if timestamp exists on this line
            if ts_match and start_timestamp:
                current_timestamp = datetime.strptime(ts_match.group(1), "%Y-%m-%d %H:%M:%S")
                elapsed_seconds = (current_timestamp - start_timestamp).total_seconds()

                # Skip measurements during warmup period
                if elapsed_seconds < warmup_seconds:
                    continue
            elif start_timestamp is None:
                # No timestamp found yet, skip this measurement (still in early startup)
                continue

            if session_id not in session_fps_history:
                session_fps_history[session_id] = []
            session_fps_history[session_id].append(actual_fps)

    # Determine success based on average FPS >= tolerance of requested
    for session_id, fps_history in session_fps_history.items():
        if fps_history:
            avg_fps = sum(fps_history) / len(fps_history)
            if avg_fps >= min_required_fps:
                successful_sessions.add(session_id)

    details = {
        "successful_count": len(successful_sessions),
        "successful_sessions": sorted(list(successful_sessions)),
        "session_fps_history": session_fps_history,
        "min_required_fps": min_required_fps,
    }

    return len(successful_sessions) == num_sessions, len(successful_sessions), details


def monitor_tx_fps(log_lines: list, expected_fps: float, num_sessions: int,
                   fps_tolerance_pct: float = FPS_TOLERANCE_PCT,
                   warmup_seconds: int = FPS_WARMUP_SECONDS) -> tuple:
    """
    Monitor TX FPS from RxTxApp log output.

    Args:
        log_lines: List of log output lines from RxTxApp
        expected_fps: Target FPS value for validation
        num_sessions: Number of sessions to validate
        fps_tolerance_pct: Required percentage of target FPS (default: 99%)
        warmup_seconds: Warmup period to skip (default: 30s)

    Returns:
        Tuple of (all_successful: bool, successful_count: int, details: dict)
    """
    return _monitor_fps_generic(
        log_lines, expected_fps, num_sessions,
        r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)",
        fps_tolerance_pct, warmup_seconds
    )


def monitor_rx_fps(log_lines: list, expected_fps: float, num_sessions: int,
                   fps_tolerance_pct: float = FPS_TOLERANCE_PCT,
                   warmup_seconds: int = FPS_WARMUP_SECONDS) -> tuple:
    """
    Monitor RX FPS from RxTxApp log output.

    Args:
        log_lines: List of log output lines from RxTxApp
        expected_fps: Target FPS value for validation
        num_sessions: Number of sessions to validate
        fps_tolerance_pct: Required percentage of target FPS (default: 99%)
        warmup_seconds: Warmup period to skip (default: 30s)

    Returns:
        Tuple of (all_successful: bool, successful_count: int, details: dict)
    """
    return _monitor_fps_generic(
        log_lines, expected_fps, num_sessions,
        r"RX_VIDEO_SESSION\(\d+,(\d+):app_rx_st20p_(\d+)\):\s+fps\s+([\d.]+)",
        fps_tolerance_pct, warmup_seconds
    )


def monitor_tx_frames(log_lines: list, num_sessions: int) -> dict:
    """
    Extract TX frame counts from RxTxApp log output.

    Args:
        log_lines: List of log output lines
        num_sessions: Number of sessions

    Returns:
        Dictionary mapping session_id -> transmitted frames (raw count from logs)
    """
    tx_frames_pattern = re.compile(
        r"TX_VIDEO_SESSION\(\d+,\d+:app_tx_st20p_(\d+)\):\s+fps\s+[\d.]+\s+frames\s+(\d+)"
    )

    session_tx_frames = {}
    for line in log_lines:
        match = tx_frames_pattern.search(line)
        if match:
            session_id = int(match.group(1))
            frames_transmitted = int(match.group(2))

            # Keep the highest frame count seen (last report)
            if session_id not in session_tx_frames or frames_transmitted > session_tx_frames[session_id]:
                session_tx_frames[session_id] = frames_transmitted

    return session_tx_frames


def monitor_rx_frames_simple(log_lines: list, num_sessions: int) -> dict:
    """
    Extract RX frame counts from RxTxApp log output.

    Args:
        log_lines: List of log output lines
        num_sessions: Number of sessions

    Returns:
        Dictionary mapping session_id -> received frames (raw count from logs)
    """
    # Pattern for periodic RX logs: RX_VIDEO_SESSION(0,0:app_rx_st20p_0): fps 59.899856 frames 599
    rx_frames_pattern_periodic = re.compile(
        r"RX_VIDEO_SESSION\(\d+,\d+:app_rx_st20p_(\d+)\):\s+fps\s+[\d.]+\s+frames\s+(\d+)"
    )
    # Pattern for final result: app_rx_st20p_result(0), OK, fps 59.12, 1180 frame received
    rx_frames_pattern_result = re.compile(
        r"app_rx_st20p_result\((\d+)\),\s+OK,\s+fps\s+[\d.]+,\s+(\d+)\s+frame received"
    )

    session_rx_frames = {}
    for line in log_lines:
        # Try periodic pattern first
        match = rx_frames_pattern_periodic.search(line)
        if match:
            session_id = int(match.group(1))
            frames_received = int(match.group(2))

            # Keep the highest frame count seen (last report)
            if session_id not in session_rx_frames or frames_received > session_rx_frames[session_id]:
                session_rx_frames[session_id] = frames_received
        else:
            # Try result pattern
            match = rx_frames_pattern_result.search(line)
            if match:
                session_id = int(match.group(1))
                frames_received = int(match.group(2))

                # Keep the highest frame count seen (last report)
                if session_id not in session_rx_frames or frames_received > session_rx_frames[session_id]:
                    session_rx_frames[session_id] = frames_received

    return session_rx_frames


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
    Get the appropriate DSA device for a given NIC based on NUMA node.

    Args:
        nic_pci_address: PCI address of the NIC (e.g., "0000:18:01.0" or "0000:af:01.0")

    Returns:
        DSA device PCI address for the appropriate NUMA node
    """
    try:
        bus = int(nic_pci_address.split(":")[1], 16)
        return DSA_DEVICES["numa0"] if bus < 0x80 else DSA_DEVICES["numa1"]
    except (ValueError, IndexError):
        logger.warning(f"Could not determine NUMA node for {nic_pci_address}, defaulting to NUMA 0")
        return DSA_DEVICES["numa0"]


def _display_session_results(direction: str, dsa_label: str, num_sessions: int, fps: float,
                             fps_details: dict, tx_frame_counts: dict, rx_frame_counts: dict) -> None:
    """
    Display FPS and frame count results for all sessions.

    Args:
        direction: "TX" or "RX" (which side is being tested)
        dsa_label: Label with DSA information (e.g., " with DSA (0000:6d:01.0)")
        num_sessions: Total number of sessions
        fps: Requested frame rate
        fps_details: FPS monitoring results containing:
            - successful_sessions: List of session IDs that met FPS requirements
            - session_fps_history: Dict mapping session_id -> list of FPS values
            - min_required_fps: Minimum FPS threshold for pass
        tx_frame_counts: TX frame counts per session (dict: session_id -> count)
        rx_frame_counts: RX frame counts per session (dict: session_id -> count)
    """
    successful_count = len(fps_details.get("successful_sessions", []))
    min_required = fps_details.get("min_required_fps", fps * FPS_TOLERANCE_PCT)

    logger.info("=" * 80)
    logger.info(
        f"{direction} {'FPS Results' if direction == 'TX' else 'Results'}{dsa_label}: "
        f"{successful_count}/{num_sessions} sessions at target {fps} fps "
        f"(min required: {min_required:.1f} fps, after {FPS_WARMUP_SECONDS}s warmup)"
    )
    logger.info("=" * 80)

    for session_id in range(num_sessions):
        fps_history = fps_details.get("session_fps_history", {}).get(session_id, [])
        tx_frames = tx_frame_counts.get(session_id, 0)
        rx_frames = rx_frame_counts.get(session_id, 0)

        # Calculate success percentage with defensive checks
        if tx_frames > 0:
            success_pct = (rx_frames / tx_frames * 100)
            if success_pct > 100.0:
                logger.warning(
                    f"Session {session_id}: RX frames ({rx_frames}) > TX frames ({tx_frames}) "
                    "- companion may have terminated early"
                )
                success_pct_display = f"{success_pct:.1f}% (>100%!)"
            else:
                success_pct_display = f"{success_pct:.1f}%"
        else:
            success_pct_display = "N/A (no TX data)"

        successful_session_ids = fps_details.get("successful_sessions", [])

        if fps_history:
            avg_fps = sum(fps_history) / len(fps_history)
            min_fps = min(fps_history)
            max_fps = max(fps_history)
            fps_status = "✓" if session_id in successful_session_ids else "✗"
            logger.info(
                f"  Session {session_id}: FPS: requested={fps}, avg={avg_fps:.1f}, "
                f"min={min_fps:.1f}, max={max_fps:.1f} {fps_status} | "
                f"Frames: TX={tx_frames}, RX={rx_frames}, Success={success_pct_display}"
            )
        else:
            logger.info(
                f"  Session {session_id}: FPS: requested={fps}, No data ✗ | "
                f"Frames: TX={tx_frames}, RX={rx_frames}, Success={success_pct_display}"
            )
    logger.info("=" * 80)


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_vf_tx_fps_variants(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test TX multi-session performance with different FPS values.

    Args:
        hosts: Host fixtures
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
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
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
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

        # Retrieve companion RX logs before validation
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs")
        logger.info("=" * 80)
        get_companion_log_summary(host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process{dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"TX process{dsa_label} failed with exit code {result.return_code}")

        # Validate TX FPS achievement from TX logs (skip first warmup period)
        success, successful_count, fps_details = monitor_tx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )

        # Get TX frame counts from TX logs (measure from beginning)
        tx_frame_counts = monitor_tx_frames(
            result.stdout_text.splitlines(), num_sessions
        )

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get RX frame counts from companion RX log (measure from beginning)
        try:
            companion_result = host.connection.execute_command(f"cat {companion_log}", shell=True)
            rx_frame_counts = monitor_rx_frames_simple(
                companion_result.stdout.splitlines() if companion_result.stdout else [],
                num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log for frame counts: {e}")
            rx_frame_counts = {}

        # Display session results
        _display_session_results("TX", dsa_label, num_sessions, fps, fps_details,
                                tx_frame_counts, rx_frame_counts)

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (min {fps_details['min_required_fps']:.1f} fps){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"TX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
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
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_vf_rx_fps_variants(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test RX multi-session performance with different FPS values.

    Args:
        hosts: Host fixtures
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
        use_dsa: Whether to use DSA for DMA operations
    """
    host = list(hosts.values())[0]

    if not hasattr(host, "vfs") or len(host.vfs) < 2:
        pytest.skip("Test requires at least 2 VFs on host")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    rx_vf = host.vfs[0]
    tx_vf = host.vfs[1]

    # Configure DSA if enabled
    dsa_device = get_dsa_device_for_nic(rx_vf) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF RX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"RX={rx_vf}, TX={tx_vf}, Media: {media_config['filename']}")
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
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
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

        # Retrieve companion TX logs before validation
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs")
        logger.info("=" * 80)
        get_companion_log_summary(host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process{dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"RX process{dsa_label} failed with exit code {result.return_code}")

        # Validate RX FPS achievement from RX logs (skip first warmup period)
        rx_fps_success, rx_fps_successful_count, rx_fps_details = monitor_rx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )

        # Get RX frame counts from RX logs (measure from beginning)
        rx_frame_counts = monitor_rx_frames_simple(
            result.stdout_text.splitlines(), num_sessions
        )

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get TX frame counts from companion TX log (measure from beginning)
        tx_frame_counts = {}
        try:
            companion_result = host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []

            # Get TX frame counts (from beginning)
            tx_frame_counts = monitor_tx_frames(
                companion_lines, num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log for frame counts: {e}")

        # Display session results
        _display_session_results("RX", dsa_label, num_sessions, fps, rx_fps_details,
                                tx_frame_counts, rx_frame_counts)

        # Test passes/fails based on RX FPS achievement
        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (min {rx_fps_details['min_required_fps']:.1f} fps){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"RX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX: {e}")
