# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VF Multi-Session FPS Variants Dual-Host Performance Tests

Tests TX/RX performance using Virtual Functions (VFs) across two separate hosts.
- Host 1: Runs the measured application (TX or RX) with performance profiling
- Host 2: Runs the companion application (RX or TX)
- Tests FPS: 25, 30, 50, 59
- Tests multiple sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32) to validate performance scaling
- Validates both TX FPS achievement and RX frame reception
- Supports optional DSA (Data Streaming Accelerator) for DMA offload
- Skips first 30 seconds of measurements to exclude unstable initialization period

Single-Core Tests: All sessions pinned to single core using sch_session_quota
Multi-Core Tests: Sessions distributed across multiple cores (no sch_session_quota)

Network Configuration:
- remote_183 (measured): 192.168.30.101  
- localhost (companion): 192.168.30.102
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


def get_dsa_device_for_nic(nic_pci_address: str, host=None, test_config: dict = None) -> str:
    """
    Get the appropriate DSA device for a given NIC based on NUMA node.

    Reads DSA device from test_config's host_dsa_devices (if available), otherwise falls back
    to default DSA_DEVICES from app_mappings.py.
    
    For hosts with single NUMA node in config, uses that device regardless of NIC bus.
    For hosts with multiple NUMA nodes, calculates based on PCI bus number.

    Args:
        nic_pci_address: PCI address of the NIC (e.g., "0000:18:01.0" or "0000:af:01.0")
        host: Optional Host object
        test_config: Optional test configuration dict containing host_dsa_devices

    Returns:
        DSA device PCI address for the appropriate NUMA node
    """
    try:
        # Try to get DSA from test_config's host_dsa_devices first
        if test_config and host and "host_dsa_devices" in test_config:
            host_dsa = test_config["host_dsa_devices"].get(host.name, {})
            if host_dsa:
                # If only one NUMA node configured, use it regardless of NIC bus
                if len(host_dsa) == 1:
                    numa_node = list(host_dsa.keys())[0]
                    dsa_device = host_dsa[numa_node]
                    logger.info(f"Using single DSA device from test_config for {host.name}: {dsa_device} ({numa_node})")
                    return dsa_device
                
                # Multiple NUMA nodes: calculate based on PCI bus
                bus = int(nic_pci_address.split(":")[1], 16)
                numa_node = "numa0" if bus < 0x80 else "numa1"
                if numa_node in host_dsa:
                    logger.info(f"Using DSA device from test_config for {host.name}: {host_dsa[numa_node]} for {numa_node}")
                    return host_dsa[numa_node]
        
        # Fall back to default DSA_DEVICES from app_mappings.py
        bus = int(nic_pci_address.split(":")[1], 16)
        numa_node = "numa0" if bus < 0x80 else "numa1"
        logger.info(f"Using default DSA device from app_mappings: {DSA_DEVICES[numa_node]} for {numa_node}")
        return DSA_DEVICES[numa_node]
    except (ValueError, IndexError):
        logger.warning(f"Could not determine NUMA node for {nic_pci_address}, defaulting to NUMA 0")
        # Get from test_config if available, otherwise use fallback
        if test_config and host and "host_dsa_devices" in test_config:
            host_dsa = test_config["host_dsa_devices"].get(host.name, {})
            # Return first available NUMA node or numa0 fallback
            if host_dsa:
                return list(host_dsa.values())[0]
            return host_dsa.get("numa0", "0000:6a:01.0")
        return DSA_DEVICES.get("numa0", "0000:6a:01.0")


def get_host_build_path(host, default_build: str, test_config: dict = None) -> str:
    """
    Get the MTL build path for a host from test_config.
    
    Args:
        host: Host object
        default_build: Default build path to use as fallback
        test_config: Optional test configuration dict containing host_mtl_paths
        
    Returns:
        MTL build path for the host
    """
    # Try to get from test_config's host_mtl_paths
    if test_config and host and "host_mtl_paths" in test_config:
        host_path = test_config["host_mtl_paths"].get(host.name)
        if host_path:
            logger.debug(f"Using MTL path from test_config for {host.name}: {host_path}")
            return host_path
    
    # Fall back to default build path
    logger.debug(f"Using default build path for {host.name}: {default_build}")
    return default_build


def display_session_results(direction: str, dsa_label: str, num_sessions: int, fps: float,
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
def test_dualhost_vf_tx_fps_variants_single_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config) -> None:
    """
    Test TX multi-session performance with different FPS values across two hosts (single-core pinning).

    Host 1 (measured): Runs TX with performance profiling
    Host 2 (companion): Runs RX to receive frames

    Args:
        hosts: Host fixtures (requires at least 2 hosts)
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
        use_dsa: Whether to use DSA for DMA operations
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    # Host order matches topology_config: [0]=localhost (companion), [1]=remote_183 (measured)
    remote_183 = list(hosts.values())[1]  # Measured TX host
    localhost = list(hosts.values())[0]  # Companion RX host

    if not hasattr(remote_183, "vfs") or len(remote_183.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on remote_183")
    
    if not hasattr(localhost, "vfs") or len(localhost.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on localhost")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    tx_vf = remote_183.vfs[0]  # TX VF on measured host
    rx_vf = localhost.vfs[0]  # RX VF on companion host

    # Configure DSA if enabled (only for measured host)
    dsa_device = get_dsa_device_for_nic(tx_vf, host=remote_183, test_config=test_config) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"remote_183 (measured) TX={tx_vf}, localhost (companion) RX={rx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_remote_183 = get_host_build_path(remote_183, build, test_config)
    build_localhost = get_host_build_path(localhost, build, test_config)

    # Start companion RX on localhost
    rx_app = RxTxApp(app_path=f"{build_localhost}/tests/tools/RxTxApp/build")

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

    companion_log = f"{build_localhost}/tests/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_rx_companion.log"
    logger.info(f"Starting companion RX on localhost, logs: {companion_log}")
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")

    # Write RX config to localhost before starting
    rx_app.prepare_execution(build=build_localhost, host=localhost)

    rx_process = localhost.connection.start_process(
        f"{rx_app.command} > {companion_log} 2>&1", cwd=build_localhost, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA on remote_183
        tx_app = RxTxApp(app_path=f"{build_remote_183}/tests/tools/RxTxApp/build")

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

        # Write TX config to remote_183 before running
        tx_app.prepare_execution(build=build_remote_183, host=remote_183)


        logger.info(f"Running TX on remote_183{dsa_label}")

        result = run(tx_app.command, cwd=build_remote_183, timeout=test_time + 60, testcmd=True, host=remote_183)

        # Log TX output to the main test log
        logger.info("=" * 80)
        logger.info(f"TX Process Output (Host1)")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch"]):
                    logger.info(f"TX: {line}")
        else:
            logger.warning("No TX output captured!")
        logger.info("=" * 80)

        # Retrieve companion RX logs from localhost before validation
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs (Host2)")
        logger.info("=" * 80)
        get_companion_log_summary(localhost, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process{dsa_label} on remote_183 failed with exit code {result.return_code}")
            pytest.fail(f"TX process{dsa_label} on remote_183 failed with exit code {result.return_code}")

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

        # Get RX frame counts from companion RX log on localhost (measure from beginning)
        try:
            companion_result = localhost.connection.execute_command(f"cat {companion_log}", shell=True)
            rx_frame_counts = monitor_rx_frames_simple(
                companion_result.stdout.splitlines() if companion_result.stdout else [],
                num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log from localhost for frame counts: {e}")
            rx_frame_counts = {}

        # Display session results
        display_session_results("TX", dsa_label, num_sessions, fps, fps_details,
                                tx_frame_counts, rx_frame_counts)

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (min {fps_details['min_required_fps']:.1f} fps){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host TX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX on localhost: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_dualhost_vf_rx_fps_variants_single_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config) -> None:
    """
    Test RX multi-session performance with different FPS values across two hosts (single-core pinning).

    Host 1 (measured): Runs RX with performance profiling
    Host 2 (companion): Runs TX to send frames

    Args:
        hosts: Host fixtures (requires at least 2 hosts)
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
        use_dsa: Whether to use DSA for DMA operations
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    # Host order matches topology_config: [0]=localhost (companion), [1]=remote_183 (measured)
    remote_183 = list(hosts.values())[1]  # Measured RX host
    localhost = list(hosts.values())[0]  # Companion TX host

    if not hasattr(remote_183, "vfs") or len(remote_183.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on remote_183")
    
    if not hasattr(localhost, "vfs") or len(localhost.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on localhost")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    rx_vf = remote_183.vfs[0]  # RX VF on measured host
    tx_vf = localhost.vfs[0]  # TX VF on companion host

    # Configure DSA if enabled (only for measured host)
    dsa_device = get_dsa_device_for_nic(rx_vf, host=remote_183, test_config=test_config) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"remote_183 (measured) RX={rx_vf}, localhost (companion) TX={tx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_remote_183 = get_host_build_path(remote_183, build, test_config)
    build_localhost = get_host_build_path(localhost, build, test_config)

    # Start companion TX on localhost
    tx_app = RxTxApp(app_path=f"{build_localhost}/tests/tools/RxTxApp/build")

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
        source_ip="192.168.30.101",
        destination_ip="192.168.30.102",
        port=20000,
        replicas=num_sessions,
        sch_session_quota=60,
        test_time=test_time + 10,
    )

    companion_log = f"{build_localhost}/tests/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_tx_companion.log"
    logger.info(f"Starting companion TX on localhost, logs: {companion_log}")
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
    
    # Write TX config to localhost before starting
    tx_app.prepare_execution(build=build_localhost, host=localhost)
    
    # Get RX VF MAC address dynamically from remote_183
    rx_vf_mac = rx_vf.replace("0000:", "").replace(":", "/").replace("/", ":")  # PCI to interface name
    try:
        mac_result = remote_183.connection.execute_command(
            f"cat /sys/bus/pci/devices/{rx_vf}/net/*/address 2>/dev/null || echo ''",
            shell=True
        )
        rx_mac = mac_result.stdout.strip() if mac_result.stdout else None
        if rx_mac:
            tx_app.command += f" --p_tx_dst_mac {rx_mac}"
            logger.info(f"Using RX VF MAC: {rx_mac} for TX destination")
    except Exception as e:
        logger.warning(f"Could not get RX VF MAC, relying on ARP: {e}")
    
    tx_process = localhost.connection.start_process(
        f"{tx_app.command} > {companion_log} 2>&1", cwd=build_localhost, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA on remote_183
        rx_app = RxTxApp(app_path=f"{build_remote_183}/tests/tools/RxTxApp/build")

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
            "source_ip": "192.168.30.101",
            "destination_ip": "192.168.30.102",
            "port": 20000,
            "replicas": num_sessions,
            "sch_session_quota": 60,
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)
        
        # Write RX config to remote_183 before running
        rx_app.prepare_execution(build=build_remote_183, host=remote_183)

        logger.info(f"Running RX on remote_183{dsa_label}")

        result = run(rx_app.command, cwd=build_remote_183, timeout=test_time + 60, testcmd=True, host=remote_183)

        # Log RX output to the main test log
        logger.info("=" * 80)
        logger.info(f"RX Process Output (Host1)")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["frame", "session", "error", "fail", "warn", "dma", "mismatch"]):
                    logger.info(f"RX: {line}")
        else:
            logger.warning("No RX output captured!")
        logger.info("=" * 80)

        # Retrieve companion TX logs from localhost before validation
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs (Host2)")
        logger.info("=" * 80)
        get_companion_log_summary(localhost, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process{dsa_label} on remote_183 failed with exit code {result.return_code}")
            pytest.fail(f"RX process{dsa_label} on remote_183 failed with exit code {result.return_code}")

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

        # Get TX frame counts from companion TX log on localhost (measure from beginning)
        tx_frame_counts = {}
        try:
            companion_result = localhost.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []

            # Get TX frame counts (from beginning)
            tx_frame_counts = monitor_tx_frames(
                companion_lines, num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log from localhost for frame counts: {e}")

        # Display session results
        display_session_results("RX", dsa_label, num_sessions, fps, rx_fps_details,
                                tx_frame_counts, rx_frame_counts)

        # Test passes/fails based on RX FPS achievement
        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (min {rx_fps_details['min_required_fps']:.1f} fps){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host RX FPS variant test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX on localhost: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_dualhost_vf_tx_fps_variants_multi_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config) -> None:
    """
    Test TX multi-session performance with different FPS values across two hosts (multi-core distribution).

    Sessions are distributed across multiple cores for parallel processing.
    Host 1 (measured): Runs TX with performance profiling
    Host 2 (companion): Runs RX to receive frames

    Args:
        hosts: Host fixtures (requires at least 2 hosts)
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
        use_dsa: Whether to use DSA for DMA operations
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    # Host order matches topology_config: [0]=localhost (companion), [1]=remote_183 (measured)
    remote_183 = list(hosts.values())[1]  # Measured TX host
    localhost = list(hosts.values())[0]  # Companion RX host

    if not hasattr(remote_183, "vfs") or len(remote_183.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on remote_183")
    
    if not hasattr(localhost, "vfs") or len(localhost.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on localhost")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    tx_vf = remote_183.vfs[0]  # TX VF on measured host
    rx_vf = localhost.vfs[0]  # RX VF on companion host

    # Configure DSA if enabled (only for measured host)
    dsa_device = get_dsa_device_for_nic(tx_vf, host=remote_183, test_config=test_config) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"remote_183 (measured) TX={tx_vf}, localhost (companion) RX={rx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_remote_183 = get_host_build_path(remote_183, build, test_config)
    build_localhost = get_host_build_path(localhost, build, test_config)

    # Start companion RX on localhost
    rx_app = RxTxApp(app_path=f"{build_localhost}/tests/tools/RxTxApp/build")

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

    companion_log = f"{build_localhost}/tests/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_rx_companion.log"
    logger.info(f"Starting companion RX on localhost, logs: {companion_log}")
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
    
    # Write RX config to localhost before starting
    rx_app.prepare_execution(build=build_localhost, host=localhost)
    
    rx_process = localhost.connection.start_process(
        f"{rx_app.command} > {companion_log} 2>&1", cwd=build_localhost, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA on remote_183 (no sch_session_quota for multi-core)
        tx_app = RxTxApp(app_path=f"{build_remote_183}/tests/tools/RxTxApp/build")

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
        
        # Write TX config to remote_183 before running
        tx_app.prepare_execution(build=build_remote_183, host=remote_183)

        logger.info(f"Running TX on remote_183 (Multi-Core){dsa_label}")

        result = run(tx_app.command, cwd=build_remote_183, timeout=test_time + 60, testcmd=True, host=remote_183)

        # Log TX output to the main test log
        logger.info("=" * 80)
        logger.info(f"TX Process Output (Host1)")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch"]):
                    logger.info(f"TX: {line}")
        else:
            logger.warning("No TX output captured!")
        logger.info("=" * 80)

        # Retrieve companion RX logs from localhost before validation
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs (Host2)")
        logger.info("=" * 80)
        get_companion_log_summary(localhost, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process (Multi-Core){dsa_label} on remote_183 failed with exit code {result.return_code}")
            pytest.fail(f"TX process (Multi-Core){dsa_label} on remote_183 failed with exit code {result.return_code}")

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

        # Get RX frame counts from companion RX log on localhost (measure from beginning)
        try:
            companion_result = localhost.connection.execute_command(f"cat {companion_log}", shell=True)
            rx_frame_counts = monitor_rx_frames_simple(
                companion_result.stdout.splitlines() if companion_result.stdout else [],
                num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log from localhost for frame counts: {e}")
            rx_frame_counts = {}

        # Display session results
        display_session_results("TX", dsa_label, num_sessions, fps, fps_details,
                                tx_frame_counts, rx_frame_counts)

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (min {fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host TX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX on localhost: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize("num_sessions", [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32], ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess"])
def test_dualhost_vf_rx_fps_variants_multi_core(hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config) -> None:
    """
    Test RX multi-session performance with different FPS values across two hosts (multi-core distribution).

    Sessions are distributed across multiple cores for parallel processing.
    Host 1 (measured): Runs RX with performance profiling
    Host 2 (companion): Runs TX to send frames

    Args:
        hosts: Host fixtures (requires at least 2 hosts)
        build: Build directory path
        media: Media directory path
        test_time: Test duration in seconds
        nic_port_list: List of available NIC ports (triggers VF creation)
        fps: Frame rate to test (25, 30, 50, or 59)
        num_sessions: Number of concurrent sessions (1, 2, 4, 8, 14, 15, 16, 17, 20, 24, or 32)
        use_dsa: Whether to use DSA for DMA operations
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    # Host order matches topology_config: [0]=localhost (companion), [1]=remote_183 (measured)
    remote_183 = list(hosts.values())[1]  # Measured RX host
    localhost = list(hosts.values())[0]  # Companion TX host

    if not hasattr(remote_183, "vfs") or len(remote_183.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on remote_183")
    
    if not hasattr(localhost, "vfs") or len(localhost.vfs) < 1:
        pytest.skip("Test requires at least 1 VF on localhost")

    media_config = MEDIA_CONFIGS[fps]
    media_file_path = f"{media}/{media_config['filename']}"

    rx_vf = remote_183.vfs[0]  # RX VF on measured host
    tx_vf = localhost.vfs[0]  # TX VF on companion host

    # Configure DSA if enabled (only for measured host)
    dsa_device = get_dsa_device_for_nic(rx_vf, host=remote_183, test_config=test_config) if use_dsa else None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"remote_183 (measured) RX={rx_vf}, localhost (companion) TX={tx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_remote_183 = get_host_build_path(remote_183, build, test_config)
    build_localhost = get_host_build_path(localhost, build, test_config)

    # Start companion TX on localhost
    tx_app = RxTxApp(app_path=f"{build_localhost}/tests/tools/RxTxApp/build")

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
        source_ip="192.168.30.101",
        destination_ip="192.168.30.102",
        port=20000,
        replicas=num_sessions,
        test_time=test_time + 10,
    )

    companion_log = f"{build_localhost}/tests/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_tx_companion.log"
    logger.info(f"Starting companion TX on localhost, logs: {companion_log}")
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")
    
    # Write TX config to localhost before starting
    tx_app.prepare_execution(build=build_localhost, host=localhost)
    
    tx_process = localhost.connection.start_process(
        f"{tx_app.command} > {companion_log} 2>&1", cwd=build_localhost, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA on remote_183 (no sch_session_quota for multi-core)
        rx_app = RxTxApp(app_path=f"{build_remote_183}/tests/tools/RxTxApp/build")

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
            "source_ip": "192.168.30.101",
            "destination_ip": "192.168.30.102",
            "port": 20000,
            "replicas": num_sessions,
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)
        
        # Write RX config to remote_183 before running
        rx_app.prepare_execution(build=build_remote_183, host=remote_183)

        logger.info(f"Running RX on remote_183 (Multi-Core){dsa_label}")

        result = run(rx_app.command, cwd=build_remote_183, timeout=test_time + 60, testcmd=True, host=remote_183)

        # Log RX output to the main test log
        logger.info("=" * 80)
        logger.info(f"RX Process Output (Host1)")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["frame", "session", "error", "fail", "warn", "dma", "mismatch"]):
                    logger.info(f"RX: {line}")
        else:
            logger.warning("No RX output captured!")
        logger.info("=" * 80)

        # Retrieve companion TX logs from localhost before validation
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs (Host2)")
        logger.info("=" * 80)
        get_companion_log_summary(localhost, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process (Multi-Core){dsa_label} on remote_183 failed with exit code {result.return_code}")
            pytest.fail(f"RX process (Multi-Core){dsa_label} on remote_183 failed with exit code {result.return_code}")

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

        # Get TX frame counts from companion TX log on localhost (measure from beginning)
        tx_frame_counts = {}
        try:
            companion_result = localhost.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []

            # Get TX frame counts (from beginning)
            tx_frame_counts = monitor_tx_frames(
                companion_lines, num_sessions
            )
        except Exception as e:
            logger.warning(f"Could not read companion log from localhost for frame counts: {e}")

        # Display session results
        display_session_results("RX", dsa_label, num_sessions, fps, rx_fps_details,
                                tx_frame_counts, rx_frame_counts)

        # Test passes/fails based on RX FPS achievement
        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (min {rx_fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host RX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX on localhost: {e}")
