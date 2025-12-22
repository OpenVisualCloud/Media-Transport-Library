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

Single-Core Tests: All sessions pinned to single core using sch_session_quota
Multi-Core Tests: Sessions distributed across multiple cores (no sch_session_quota)
"""

import logging
import time

import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.config.app_mappings import DSA_DEVICES
from mtl_engine.execute import log_fail, run
from mtl_engine.performance_monitoring import (
    FPS_TOLERANCE_PCT,
    FPS_WARMUP_SECONDS,
    display_session_results,
    get_companion_log_summary,
    monitor_rx_fps,
    monitor_rx_frames_simple,
    monitor_tx_fps,
    monitor_tx_frames,
)
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


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


def get_dsa_device_for_nic(nic_pci_address: str, host=None) -> str:
    """
    Get the appropriate DSA device for a given NIC based on NUMA node.

    Reads DSA device from host's topology config (if available), otherwise falls back
    to default DSA_DEVICES from app_mappings.py.

    Args:
        nic_pci_address: PCI address of the NIC (e.g., "0000:18:01.0" or "0000:af:01.0")
        host: Optional Host object with topology config containing dsa_devices

    Returns:
        DSA device PCI address for the appropriate NUMA node
    """
    try:
        bus = int(nic_pci_address.split(":")[1], 16)
        numa_node = "numa0" if bus < 0x80 else "numa1"
        
        # Try to get DSA from host's topology config first
        if host and hasattr(host, 'topology') and hasattr(host.topology, 'dsa_devices'):
            dsa_devices = host.topology.dsa_devices
            if dsa_devices and numa_node in dsa_devices:
                logger.info(f"Using DSA device from topology config: {dsa_devices[numa_node]} for {numa_node}")
                return dsa_devices[numa_node]
        
        # Fall back to default DSA_DEVICES from app_mappings.py
        logger.info(f"Using default DSA device from app_mappings: {DSA_DEVICES[numa_node]} for {numa_node}")
        return DSA_DEVICES[numa_node]
    except (ValueError, IndexError):
        logger.warning(f"Could not determine NUMA node for {nic_pci_address}, defaulting to NUMA 0")
        return DSA_DEVICES.get("numa0", "0000:6a:01.0")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_vf_tx_fps_variants_single_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test TX multi-session performance with different FPS values (single-core pinning).

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
    dsa_device = get_dsa_device_for_nic(tx_vf, host=host) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF TX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"TX={tx_vf}, RX={rx_vf}, Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Start companion RX
    rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        display_session_results("TX", dsa_label, num_sessions, fps, fps_details,
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
def test_vf_rx_fps_variants_single_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test RX multi-session performance with different FPS values (single-core pinning).

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
    dsa_device = get_dsa_device_for_nic(rx_vf, host=host) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF RX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"RX={rx_vf}, TX={tx_vf}, Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Start companion TX
    tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        display_session_results("RX", dsa_label, num_sessions, fps, rx_fps_details,
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


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_vf_tx_fps_variants_multi_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test TX multi-session performance with different FPS values (multi-core distribution).

    Sessions are distributed across multiple cores for parallel processing.

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
    dsa_device = get_dsa_device_for_nic(tx_vf, host=host) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF TX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"TX={tx_vf}, RX={rx_vf}, Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Start companion RX
    rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        test_time=test_time + 10,
    )

    companion_log = f"{build}/tests/vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_rx_companion.log"
    logger.info(f"Starting companion RX, logs will be saved to: {companion_log}")
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
    rx_process = host.connection.start_process(
        f"{rx_app.command} > {companion_log} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA (no sch_session_quota for multi-core)
        tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
            "test_time": test_time,
        }

        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)

        logger.info(f"Running TX (Multi-Core){dsa_label}")

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
            log_fail(f"TX process (Multi-Core){dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"TX process (Multi-Core){dsa_label} failed with exit code {result.return_code}")

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
        display_session_results("TX", dsa_label, num_sessions, fps, fps_details,
                                tx_frame_counts, rx_frame_counts)

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (min {fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"TX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps")

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
def test_vf_rx_fps_variants_multi_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa) -> None:
    """
    Test RX multi-session performance with different FPS values (multi-core distribution).

    Sessions are distributed across multiple cores for parallel processing.

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
    dsa_device = get_dsa_device_for_nic(rx_vf, host=host) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"VF RX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"RX={rx_vf}, TX={tx_vf}, Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Start companion TX
    tx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
        test_time=test_time + 10,
    )

    companion_log = f"{build}/tests/vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_tx_companion.log"
    logger.info(f"Starting companion TX, logs will be saved to: {companion_log}")
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
    tx_process = host.connection.start_process(
        f"{tx_app.command} > {companion_log} 2>&1", cwd=build, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA (no sch_session_quota for multi-core)
        rx_app = RxTxApp(app_path=f"{build}/tests/tools/RxTxApp/build")

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
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)

        logger.info(f"Running RX (Multi-Core){dsa_label}")

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
            log_fail(f"RX process (Multi-Core){dsa_label} failed with exit code {result.return_code}")
            pytest.fail(f"RX process (Multi-Core){dsa_label} failed with exit code {result.return_code}")

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
        display_session_results("RX", dsa_label, num_sessions, fps, rx_fps_details,
                                tx_frame_counts, rx_frame_counts)

        # Test passes/fails based on RX FPS achievement
        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (min {rx_fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"RX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX: {e}")
