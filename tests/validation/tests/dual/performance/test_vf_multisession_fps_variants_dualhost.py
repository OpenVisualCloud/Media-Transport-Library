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

Network Configuration (generic - independent of hostname):
- tx_host: Source IP 192.168.4.150
- rx_host: Destination IP 192.168.4.206

Host Role Assignment:
- By default, hosts[0] = TX, hosts[1] = RX (for TX tests) or vice versa (for RX tests)
- Can be overridden via test_config["host_roles"]["tx"] and test_config["host_roles"]["rx"]
"""

import json
import logging
import time

import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.dsa import get_dsa_info, get_host_dsa_config, setup_host_dsa
from mtl_engine.execute import log_fail, run
from mtl_engine.performance_monitoring import (
    FPS_TOLERANCE_PCT,
    FPS_WARMUP_SECONDS,
    _display_throughput_summary,
    get_companion_log_summary,
    monitor_dev_rate,
    monitor_rx_fps,
    monitor_rx_frames_simple,
    monitor_rx_throughput,
    monitor_tx_fps,
    monitor_tx_frames,
    monitor_tx_throughput,
)
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


# Media file configurations for different FPS values
# Using RFC4175 format files to avoid TX-side pixel format conversion bottleneck
# Files in /mnt/media or copied to media_path
MEDIA_CONFIGS = {
    25: {
        # RFC4175 format - no TX conversion needed
        "filename": "HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv",
        "file_format": "YUV_422_10bit",
        "format": "YUV_422_10bit",
        "pixel_format": "YUV422RFC4175PG2BE10",  # Native RFC4175 format - no conversion
        "width": 1920,
        "height": 1080,
        "fps": 25,
    },
    30: {
        # RFC4175 format - using 60fps file at 30fps
        "filename": "CSGObuymenu_1080p_60fps_120frames_yuv422rfc4175be10.yuv",
        "file_format": "YUV_422_10bit",
        "format": "YUV_422_10bit",
        "pixel_format": "YUV422RFC4175PG2BE10",
        "width": 1920,
        "height": 1080,
        "fps": 30,
    },
    50: {
        # RFC4175 format - 60fps file works for 50fps
        "filename": "CSGObuymenu_1080p_60fps_120frames_yuv422rfc4175be10.yuv",
        "file_format": "YUV_422_10bit",
        "format": "YUV_422_10bit",
        "pixel_format": "YUV422RFC4175PG2BE10",
        "width": 1920,
        "height": 1080,
        "fps": 50,
    },
    59: {
        # RFC4175 format - native 60fps file, no conversion needed
        "filename": "CSGObuymenu_1080p_60fps_120frames_yuv422rfc4175be10.yuv",
        "file_format": "YUV_422_10bit",
        "format": "YUV_422_10bit",
        "pixel_format": "YUV422RFC4175PG2BE10",
        "width": 1920,
        "height": 1080,
        "fps": 59,
    },
}

# Scheduler session quota for single-core tests
# This forces all sessions to run on a single core
# For multi-core tests, this should be None (not set)
SCH_SESSION_QUOTA_SINGLE_CORE = 60


def get_host_media_path(host, default_media: str, test_config: dict = None) -> str:
    """
    Get the media path for a specific host from test_config.

    Args:
        host: Host object
        default_media: Default media path to use as fallback
        test_config: Optional test configuration dict containing host_media_paths

    Returns:
        Media path for the host
    """
    # Try to get from test_config's host_media_paths
    if test_config and "host_media_paths" in test_config:
        host_path = test_config["host_media_paths"].get(host.name)
        if host_path:
            logger.debug(f"Using media path from test_config for {host.name}: {host_path}")
            return host_path

    # Fall back to default media path
    logger.debug(f"Using default media path for {host.name}: {default_media}")
    return default_media


def get_tx_rx_hosts(hosts: dict, test_type: str, test_config: dict = None):
    """
    Get TX and RX hosts generically, independent of hostname.

    Host assignment priority:
    1. test_config["host_roles"]["tx"] and test_config["host_roles"]["rx"] (by name)
    2. Default positional assignment:
       - For "tx_test": hosts[0] = TX (measured), hosts[1] = RX (companion)
       - For "rx_test": hosts[0] = RX (measured), hosts[1] = TX (companion)

    Args:
        hosts: Dictionary of host objects from topology
        test_type: "tx_test" or "rx_test" - determines which host is measured
        test_config: Optional test configuration dict containing host_roles

    Returns:
        Tuple of (tx_host, rx_host) objects

    Example test_config:
        host_roles:
          tx: "host_150"  # Name of host to use for TX
          rx: "host_206"  # Name of host to use for RX
    """
    host_list = list(hosts.values())

    if len(host_list) < 2:
        raise ValueError("At least 2 hosts required for dual-host testing")

    # Try to get from test_config's host_roles first
    if test_config and "host_roles" in test_config:
        roles = test_config["host_roles"]
        tx_name = roles.get("tx")
        rx_name = roles.get("rx")

        if tx_name and rx_name:
            tx_host = hosts.get(tx_name)
            rx_host = hosts.get(rx_name)

            if tx_host and rx_host:
                logger.info(f"Using host roles from test_config: TX={tx_name}, RX={rx_name}")
                return tx_host, rx_host
            else:
                available = list(hosts.keys())
                logger.warning(
                    f"Host roles from test_config not found: TX={tx_name}, RX={rx_name}. "
                    f"Available hosts: {available}. Falling back to positional assignment."
                )

    # Default positional assignment based on test type
    if test_type == "tx_test":
        # TX test: host[0] = TX (measured), host[1] = RX (companion)
        tx_host = host_list[0]
        rx_host = host_list[1]
    else:  # rx_test
        # RX test: host[0] = RX (measured), host[1] = TX (companion)
        rx_host = host_list[0]
        tx_host = host_list[1]

    logger.info(f"Using positional host assignment ({test_type}): TX={tx_host.name}, RX={rx_host.name}")
    return tx_host, rx_host


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
    # Try to get from conftest extracted extra config (build_path in host section)
    try:
        from conftest import get_host_extra_config
        extra_config = get_host_extra_config(host.name)
        if "build_path" in extra_config:
            build_path = extra_config["build_path"]
            logger.debug(f"Using build_path from host config for {host.name}: {build_path}")
            return build_path
    except ImportError:
        pass

    # Try to get from test_config's host_mtl_paths
    if test_config and host and "host_mtl_paths" in test_config:
        host_path = test_config["host_mtl_paths"].get(host.name)
        if host_path:
            logger.debug(f"Using MTL path from test_config for {host.name}: {host_path}")
            return host_path

    # Fall back to default build path
    logger.debug(f"Using default build path for {host.name}: {default_build}")
    return default_build


def display_test_config_summary(
    direction: str,
    test_label: str,
    measured_host,
    companion_host,
    measured_vf: str,
    companion_vf: str,
    num_sessions: int,
    fps: float,
    media_config: dict,
    use_dsa: bool,
    dsa_device: str = None,
    dsa_info: dict = None,
    measured_vf_r: str = None,
    companion_vf_r: str = None,
    redundant: bool = False,
    multi_core: bool = False,
) -> None:
    """
    Display a formatted configuration summary box before FPS results.

    Args:
        direction: "TX" or "RX" (which side is being measured)
        test_label: Short test variant label (e.g., "Single-Core", "Multi-Core", "REDUNDANT")
        measured_host: Host object for the measured side
        companion_host: Host object for the companion side
        measured_vf: VF PCI address used on measured host
        companion_vf: VF PCI address used on companion host
        num_sessions: Number of concurrent sessions
        fps: Target frame rate
        media_config: Media configuration dict
        use_dsa: Whether DSA is enabled
        dsa_device: DSA PCI address if enabled
        dsa_info: Optional DSA info dict from get_dsa_info()
        measured_vf_r: Redundant VF on measured host (if redundant mode)
        companion_vf_r: Redundant VF on companion host (if redundant mode)
        redundant: Whether running in redundant mode
        multi_core: Whether running in multi-core mode
    """
    companion_dir = "RX" if direction == "TX" else "TX"
    mode = "REDUNDANT " if redundant else ""
    core_mode = "Multi-Core" if multi_core else "Single-Core"
    dsa_status = f"✓ ENABLED ({dsa_device})" if use_dsa else "✗ DISABLED"

    # Build NUMA info from dsa_info if available
    nic_numa = dsa_info.get("nic_numa", "?") if dsa_info else "?"
    dsa_numa = dsa_info.get("dsa_numa", "?") if dsa_info else "N/A"
    numa_match = dsa_info.get("numa_match", "N/A") if dsa_info else "N/A"

    logger.info(
        f"\n"
        f"╔══════════════════════════════════════════════════════════════════════════════╗\n"
        f"║  TEST CONFIGURATION SUMMARY                                                ║\n"
        f"╠══════════════════════════════════════════════════════════════════════════════╣\n"
        f"║  Test:          {mode}{direction} {core_mode:<58} ║\n"
        f"║  Sessions:      {num_sessions:<58} ║\n"
        f"║  Target FPS:    {fps:<58} ║\n"
        f"║  Media:         {media_config['filename'][:58]:<58} ║\n"
        f"║  Format:        {media_config['format']:<58} ║\n"
        f"║  Resolution:    {media_config['width']}x{media_config['height']:<50} ║\n"
        f"╠══════════════════════════════════════════════════════════════════════════════╣\n"
        f"║  MEASURED HOST                                                             ║\n"
        f"║  Host:          {measured_host.name:<58} ║\n"
        f"║  Role:          {direction:<58} ║\n"
        f"║  NIC (primary): {measured_vf:<58} ║\n"
        + (
            f"║  NIC (redund.): {measured_vf_r:<58} ║\n"
            if redundant and measured_vf_r
            else ""
        )
        + f"║  NIC NUMA:      {nic_numa:<58} ║\n"
        f"╠══════════════════════════════════════════════════════════════════════════════╣\n"
        f"║  COMPANION HOST                                                            ║\n"
        f"║  Host:          {companion_host.name:<58} ║\n"
        f"║  Role:          {companion_dir:<58} ║\n"
        f"║  NIC (primary): {companion_vf:<58} ║\n"
        + (
            f"║  NIC (redund.): {companion_vf_r:<58} ║\n"
            if redundant and companion_vf_r
            else ""
        )
        + f"╠══════════════════════════════════════════════════════════════════════════════╣\n"
        f"║  DSA:           {dsa_status:<58} ║\n"
        + (
            f"║  DSA NUMA:      {dsa_numa:<58} ║\n"
            f"║  NUMA Match:    {numa_match:<58} ║\n"
            if use_dsa
            else ""
        )
        + f"╚══════════════════════════════════════════════════════════════════════════════╝"
    )


def display_session_results(
    direction: str,
    dsa_label: str,
    num_sessions: int,
    fps: float,
    fps_details: dict,
    tx_frame_counts: dict,
    rx_frame_counts: dict,
    dsa_info: dict = None,
    throughput_details: dict = None,
    dev_rate: dict = None,
    companion_throughput_details: dict = None,
    companion_dev_rate: dict = None,
) -> None:
    """
    Display FPS, frame count, and throughput results for all sessions.

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
        dsa_info: Optional DSA configuration info dict from get_dsa_info()
        throughput_details: Throughput data from measured app (from monitor_tx_throughput/monitor_rx_throughput)
        dev_rate: Device rate data from measured app (from monitor_dev_rate)
        companion_throughput_details: Throughput data from companion app
        companion_dev_rate: Device rate data from companion app
    """
    successful_count = len(fps_details.get("successful_sessions", []))
    min_required = fps_details.get("min_required_fps", fps * FPS_TOLERANCE_PCT)

    logger.info("=" * 80)
    logger.info(
        f"{direction} {'FPS Results' if direction == 'TX' else 'Results'}{dsa_label}: "
        f"{successful_count}/{num_sessions} sessions at target {fps} fps "
        f"(min required: {min_required:.1f} fps, after {FPS_WARMUP_SECONDS}s warmup)"
    )
    
    # Display DSA configuration info if available
    if dsa_info:
        logger.info("-" * 80)
        logger.info(
            f"  DSA Config: Device={dsa_info.get('dsa_device', 'N/A')} (NUMA {dsa_info.get('dsa_numa', '?')}) | "
            f"NIC={dsa_info.get('nic_address', 'N/A')} (NUMA {dsa_info.get('nic_numa', '?')}) | "
            f"Match: {dsa_info.get('numa_match', 'N/A')}"
        )
    
    logger.info("=" * 80)

    for session_id in range(num_sessions):
        fps_history = fps_details.get("session_fps_history", {}).get(session_id, [])
        tx_frames = tx_frame_counts.get(session_id, 0)
        rx_frames = rx_frame_counts.get(session_id, 0)

        # Calculate success percentage with defensive checks
        if tx_frames > 0:
            success_pct = rx_frames / tx_frames * 100
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

    # Display throughput information (informational only, no pass/fail impact)
    _display_throughput_summary(
        direction, num_sessions, throughput_details, dev_rate,
        companion_throughput_details, companion_dev_rate,
    )

    logger.info("=" * 80)


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess"],
)
def test_dualhost_vf_tx_fps_variants_single_core(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
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

    # Get TX and RX hosts generically (independent of hostname)
    tx_host, rx_host = get_tx_rx_hosts(hosts, "tx_test", test_config)

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host ({tx_host.name})")

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host ({rx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    # Get TX host's media path (may be different from default, e.g., ramdisk)
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    # Check if both hosts point to the same machine (loopback mode)
    same_host = tx_host.connection.ip == rx_host.connection.ip

    if same_host:
        # Loopback mode: use different VFs on the same host
        if len(tx_host.vfs) < 2:
            pytest.skip("Loopback mode requires at least 2 VFs on the host")
        tx_vf = tx_host.vfs[0]  # TX VF
        rx_vf = tx_host.vfs[1]  # RX VF (different from TX)
        logger.info(f"Running in LOOPBACK mode on same host: TX={tx_vf}, RX={rx_vf}")
    else:
        tx_vf = tx_host.vfs[0]  # TX VF on measured host
        rx_vf = rx_host.vfs[0]  # RX VF on companion host

    # Configure DSA if enabled (only for measured host)
    # Uses mtl_engine.dsa for detection, validation, and NUMA alignment
    if use_dsa:
        dsa_config = get_host_dsa_config(tx_host)  # Get from topology config
        dsa_device = setup_host_dsa(tx_host, tx_vf, dsa_config, role="TX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({tx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{tx_host.name} (measured) TX={tx_vf}, {rx_host.name} (companion) RX={rx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_tx_host = get_host_build_path(tx_host, build, test_config)
    build_rx_host = get_host_build_path(rx_host, build, test_config)

    # Start companion RX on rx_host
    rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")

    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        source_ip="192.168.4.150",
        destination_ip="192.168.4.206",
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        measure_latency=True,
        replicas=num_sessions,
        sch_session_quota=SCH_SESSION_QUOTA_SINGLE_CORE,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = f"{build_rx_host}/tests/validation/logs/performance/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_rx_companion.log"
    logger.info(f"Starting companion RX on {rx_host.name}, logs: {companion_log}")
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")

    # Write RX config to rx_host before starting
    rx_app.prepare_execution(build=build_rx_host, host=rx_host)

    rx_process = rx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {rx_app.command} > {companion_log} 2>&1", cwd=build_rx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA on tx_host
        tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")

        tx_kwargs = {
            "session_type": "st20p",
            "direction": "tx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "input_file": media_file_path,
            "nic_port": tx_vf,
            "source_ip": "192.168.4.150",
            "destination_ip": "192.168.4.206",
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "replicas": num_sessions,
            "sch_session_quota": SCH_SESSION_QUOTA_SINGLE_CORE,
            "rx_queues_cnt": 64,
            "test_time": test_time,
            "tsc": True,  # Force TSC pacing (workaround for hosts without Kahawai ice driver)
        }

        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)

        # Write TX config to tx_host before running
        tx_app.prepare_execution(build=build_tx_host, host=tx_host)

        logger.info(f"Running TX on {tx_host.name}{dsa_label}")

        result = run(tx_app.command, cwd=build_tx_host, timeout=test_time + 60, testcmd=True, host=tx_host)

        # Log TX output to the main test log
        logger.info("=" * 80)
        logger.info(f"TX Process Output ({tx_host.name})")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch"]):
                    logger.info(f"TX: {line}")
        else:
            logger.warning("No TX output captured!")
        logger.info("=" * 80)

        # Retrieve companion RX logs from rx_host before validation
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs ({rx_host.name})")
        logger.info("=" * 80)
        get_companion_log_summary(rx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process{dsa_label} on {tx_host.name} failed with exit code {result.return_code}")
            pytest.fail(f"TX process{dsa_label} on {tx_host.name} failed with exit code {result.return_code}")

        # Validate TX FPS achievement from TX logs (skip first warmup period)
        success, successful_count, fps_details = monitor_tx_fps(result.stdout_text.splitlines(), fps, num_sessions)

        # Get TX frame counts from TX logs (measure from beginning)
        tx_frame_counts = monitor_tx_frames(result.stdout_text.splitlines(), num_sessions)

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get RX frame counts from companion RX log on rx_host (measure from beginning)
        companion_lines = []
        try:
            companion_result = rx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            rx_frame_counts = monitor_rx_frames_simple(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log from {rx_host.name} for frame counts: {e}")
            rx_frame_counts = {}

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_tx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_rx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        # Display session results with DSA info if enabled
        dsa_info = get_dsa_info(tx_host, role="TX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(tx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(tx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="TX", test_label="Single-Core",
            measured_host=tx_host, companion_host=rx_host,
            measured_vf=tx_vf, companion_vf=rx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
        )

        display_session_results(
            "TX", dsa_label, num_sessions, fps, fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

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
            logger.warning(f"Error stopping companion RX on {rx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48, 64],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess", "64sess"],
)
def test_dualhost_vf_rx_fps_variants_single_core(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
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

    # Get TX and RX hosts generically (independent of hostname)
    tx_host, rx_host = get_tx_rx_hosts(hosts, "rx_test", test_config)

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host ({rx_host.name})")

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host ({tx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    # Get TX host's media path (may be different from default, e.g., ramdisk)
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    rx_vf = rx_host.vfs[0]  # RX VF on measured host
    tx_vf = tx_host.vfs[0]  # TX VF on companion host

    # Configure DSA if enabled (only for measured host)
    # Uses mtl_engine.dsa for detection, validation, and NUMA alignment
    if use_dsa:
        dsa_config = get_host_dsa_config(rx_host)  # Get from topology config
        dsa_device = setup_host_dsa(rx_host, rx_vf, dsa_config, role="RX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({rx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX FPS Variant{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{rx_host.name} (measured) RX={rx_vf}, {tx_host.name} (companion) TX={tx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_rx_host = get_host_build_path(rx_host, build, test_config)
    build_tx_host = get_host_build_path(tx_host, build, test_config)

    # Start companion TX on tx_host
    tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")

    tx_app.create_command(
        session_type="st20p",
        direction="tx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        input_file=media_file_path,
        nic_port=tx_vf,
        source_ip="192.168.4.150",
        destination_ip="192.168.4.206",
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        replicas=num_sessions,
        sch_session_quota=SCH_SESSION_QUOTA_SINGLE_CORE,
        rx_queues_cnt=64,
        test_time=test_time + 10,
        pacing_way="tsc",  # Force TSC pacing - TM crashes on VFs with DPDK 25
    )

    companion_log = f"{build_tx_host}/tests/validation/logs/performance/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_tx_companion.log"
    logger.info(f"Starting companion TX on {tx_host.name}, logs: {companion_log}")
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")

    # Write TX config to tx_host before starting
    tx_app.prepare_execution(build=build_tx_host, host=tx_host)

    # Get RX VF MAC address dynamically from rx_host
    rx_vf_mac = rx_vf.replace("0000:", "").replace(":", "/").replace("/", ":")  # PCI to interface name
    try:
        mac_result = rx_host.connection.execute_command(
            f"cat /sys/bus/pci/devices/{rx_vf}/net/*/address 2>/dev/null || echo ''", shell=True
        )
        rx_mac = mac_result.stdout.strip() if mac_result.stdout else None
        if rx_mac:
            tx_app.command += f" --p_tx_dst_mac {rx_mac}"
            logger.info(f"Using RX VF MAC: {rx_mac} for TX destination")
    except Exception as e:
        logger.warning(f"Could not get RX VF MAC, relying on ARP: {e}")

    tx_process = tx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {tx_app.command} > {companion_log} 2>&1", cwd=build_tx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA on rx_host
        rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")

        rx_kwargs = {
            "session_type": "st20p",
            "direction": "rx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "output_file": "/dev/null",
            "nic_port": rx_vf,
            "source_ip": "192.168.4.150",
            "destination_ip": "192.168.4.206",
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "measure_latency": True,
            "replicas": num_sessions,
            "sch_session_quota": SCH_SESSION_QUOTA_SINGLE_CORE,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)

        # Write RX config to rx_host before running
        rx_app.prepare_execution(build=build_rx_host, host=rx_host)

        logger.info(f"Running RX on {rx_host.name}{dsa_label}")

        result = run(rx_app.command, cwd=build_rx_host, timeout=test_time + 60, testcmd=True, host=rx_host)

        # Log RX output to the main test log
        logger.info("=" * 80)
        logger.info(f"RX Process Output ({rx_host.name})")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(
                    keyword in line.lower()
                    for keyword in ["frame", "session", "error", "fail", "warn", "dma", "mismatch"]
                ):
                    logger.info(f"RX: {line}")
        else:
            logger.warning("No RX output captured!")
        logger.info("=" * 80)

        # Retrieve companion TX logs from tx_host before validation
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs ({tx_host.name})")
        logger.info("=" * 80)
        get_companion_log_summary(tx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process{dsa_label} on {rx_host.name} failed with exit code {result.return_code}")
            pytest.fail(f"RX process{dsa_label} on {rx_host.name} failed with exit code {result.return_code}")

        # Validate RX FPS achievement from RX logs (skip first warmup period)
        rx_fps_success, rx_fps_successful_count, rx_fps_details = monitor_rx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )

        # Get RX frame counts from RX logs (measure from beginning)
        rx_frame_counts = monitor_rx_frames_simple(result.stdout_text.splitlines(), num_sessions)

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get TX frame counts from companion TX log on tx_host (measure from beginning)
        tx_frame_counts = {}
        companion_lines = []
        try:
            companion_result = tx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []

            # Get TX frame counts (from beginning)
            tx_frame_counts = monitor_tx_frames(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log from {tx_host.name} for frame counts: {e}")

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_rx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_tx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        # Display session results with DSA info if enabled
        dsa_info = get_dsa_info(rx_host, role="RX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(rx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(rx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="RX", test_label="Single-Core",
            measured_host=rx_host, companion_host=tx_host,
            measured_vf=rx_vf, companion_vf=tx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
        )

        display_session_results(
            "RX", dsa_label, num_sessions, fps, rx_fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

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
            logger.warning(f"Error stopping companion TX on {tx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess"],
)
def test_dualhost_vf_tx_fps_variants_multi_core(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
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

    # Get TX and RX hosts generically (independent of hostname)
    tx_host, rx_host = get_tx_rx_hosts(hosts, "tx_test", test_config)

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host ({tx_host.name})")

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host ({rx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    # Get TX host's media path (may be different from default, e.g., ramdisk)
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    tx_vf = tx_host.vfs[0]  # TX VF on measured host
    rx_vf = rx_host.vfs[0]  # RX VF on companion host

    # Configure DSA if enabled (only for measured host)
    # Uses mtl_engine.dsa for detection, validation, and NUMA alignment
    if use_dsa:
        dsa_config = get_host_dsa_config(tx_host)  # Get from topology config
        dsa_device = setup_host_dsa(tx_host, tx_vf, dsa_config, role="TX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({tx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{tx_host.name} (measured) TX={tx_vf}, {rx_host.name} (companion) RX={rx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_tx_host = get_host_build_path(tx_host, build, test_config)
    build_rx_host = get_host_build_path(rx_host, build, test_config)

    # Start companion RX on rx_host
    rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")

    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        source_ip="192.168.4.150",
        destination_ip="192.168.4.206",
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        measure_latency=True,
        replicas=num_sessions,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = (
        f"{build_rx_host}/tests/validation/logs/performance/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_rx_companion.log"
    )
    logger.info(f"Starting companion RX on {rx_host.name}, logs: {companion_log}")
    logger.info(f"Companion RX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")

    # Write RX config to rx_host before starting
    rx_app.prepare_execution(build=build_rx_host, host=rx_host)

    rx_process = rx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {rx_app.command} > {companion_log} 2>&1", cwd=build_rx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX with optional DSA on tx_host (no sch_session_quota for multi-core)
        tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")

        tx_kwargs = {
            "session_type": "st20p",
            "direction": "tx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "input_file": media_file_path,
            "nic_port": tx_vf,
            "source_ip": "192.168.4.150",
            "destination_ip": "192.168.4.206",
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "replicas": num_sessions,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }

        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)

        # Write TX config to tx_host before running
        tx_app.prepare_execution(build=build_tx_host, host=tx_host)

        logger.info(f"Running TX on {tx_host.name} (Multi-Core){dsa_label}")

        result = run(tx_app.command, cwd=build_tx_host, timeout=test_time + 60, testcmd=True, host=tx_host)

        # Log TX output to the main test log
        logger.info("=" * 80)
        logger.info(f"TX Process Output ({tx_host.name})")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch"]):
                    logger.info(f"TX: {line}")
        else:
            logger.warning("No TX output captured!")
        logger.info("=" * 80)

        # Retrieve companion RX logs from rx_host before validation
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs ({rx_host.name})")
        logger.info("=" * 80)
        get_companion_log_summary(rx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process (Multi-Core){dsa_label} on {tx_host.name} failed with exit code {result.return_code}")
            pytest.fail(
                f"TX process (Multi-Core){dsa_label} on {tx_host.name} failed with exit code {result.return_code}"
            )

        # Validate TX FPS achievement from TX logs (skip first warmup period)
        success, successful_count, fps_details = monitor_tx_fps(result.stdout_text.splitlines(), fps, num_sessions)

        # Get TX frame counts from TX logs (measure from beginning)
        tx_frame_counts = monitor_tx_frames(result.stdout_text.splitlines(), num_sessions)

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get RX frame counts from companion RX log on rx_host (measure from beginning)
        companion_lines = []
        try:
            companion_result = rx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            rx_frame_counts = monitor_rx_frames_simple(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log from {rx_host.name} for frame counts: {e}")
            rx_frame_counts = {}

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_tx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_rx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        # Display session results with DSA info if enabled
        dsa_info = get_dsa_info(tx_host, role="TX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(tx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(tx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="TX", test_label="Multi-Core",
            measured_host=tx_host, companion_host=rx_host,
            measured_vf=tx_vf, companion_vf=rx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            multi_core=True,
        )

        display_session_results(
            "TX", dsa_label, num_sessions, fps, fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (min {fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(
            TEST_PASS,
            f"Dual-host TX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps",
        )

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX on {rx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48, 64],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess", "64sess"],
)
def test_dualhost_vf_rx_fps_variants_multi_core(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
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

    # Get TX and RX hosts generically (independent of hostname)
    tx_host, rx_host = get_tx_rx_hosts(hosts, "rx_test", test_config)

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host ({rx_host.name})")

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host ({tx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    # Get TX host's media path (may be different from default, e.g., ramdisk)
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    rx_vf = rx_host.vfs[0]  # RX VF on measured host
    tx_vf = tx_host.vfs[0]  # TX VF on companion host

    # Configure DSA if enabled (only for measured host)
    # Uses mtl_engine.dsa for detection, validation, and NUMA alignment
    if use_dsa:
        dsa_config = get_host_dsa_config(rx_host)  # Get from topology config
        dsa_device = setup_host_dsa(rx_host, rx_vf, dsa_config, role="RX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({rx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX FPS Variant (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{rx_host.name} (measured) RX={rx_vf}, {tx_host.name} (companion) TX={tx_vf}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    # Get build paths from topology config for each host
    build_rx_host = get_host_build_path(rx_host, build, test_config)
    build_tx_host = get_host_build_path(tx_host, build, test_config)

    # Start companion TX on tx_host
    tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")

    tx_app.create_command(
        session_type="st20p",
        direction="tx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        input_file=media_file_path,
        nic_port=tx_vf,
        source_ip="192.168.4.150",
        destination_ip="192.168.4.206",
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        replicas=num_sessions,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = (
        f"{build_tx_host}/tests/validation/logs/performance/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_multicore_tx_companion.log"
    )
    logger.info(f"Starting companion TX on {tx_host.name}, logs: {companion_log}")
    logger.info(f"Companion TX will run for {test_time + 10} seconds (test_time={test_time} + 10s buffer)")

    # Write TX config to tx_host before starting
    tx_app.prepare_execution(build=build_tx_host, host=tx_host)

    tx_process = tx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {tx_app.command} > {companion_log} 2>&1", cwd=build_tx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX with optional DSA on rx_host (no sch_session_quota for multi-core)
        rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")

        rx_kwargs = {
            "session_type": "st20p",
            "direction": "rx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "output_file": "/dev/null",
            "nic_port": rx_vf,
            "source_ip": "192.168.4.150",
            "destination_ip": "192.168.4.206",
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "measure_latency": True,
            "replicas": num_sessions,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }

        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)

        # Write RX config to rx_host before running
        rx_app.prepare_execution(build=build_rx_host, host=rx_host)

        logger.info(f"Running RX on {rx_host.name} (Multi-Core){dsa_label}")

        result = run(rx_app.command, cwd=build_rx_host, timeout=test_time + 60, testcmd=True, host=rx_host)

        # Log RX output to the main test log
        logger.info("=" * 80)
        logger.info(f"RX Process Output ({rx_host.name})")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(
                    keyword in line.lower()
                    for keyword in ["frame", "session", "error", "fail", "warn", "dma", "mismatch"]
                ):
                    logger.info(f"RX: {line}")
        else:
            logger.warning("No RX output captured!")
        logger.info("=" * 80)

        # Retrieve companion TX logs from tx_host before validation
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs ({tx_host.name})")
        logger.info("=" * 80)
        get_companion_log_summary(tx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process (Multi-Core){dsa_label} on {rx_host.name} failed with exit code {result.return_code}")
            pytest.fail(
                f"RX process (Multi-Core){dsa_label} on {rx_host.name} failed with exit code {result.return_code}"
            )

        # Validate RX FPS achievement from RX logs (skip first warmup period)
        rx_fps_success, rx_fps_successful_count, rx_fps_details = monitor_rx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )

        # Get RX frame counts from RX logs (measure from beginning)
        rx_frame_counts = monitor_rx_frames_simple(result.stdout_text.splitlines(), num_sessions)

        # Wait for companion to finish writing (it runs test_time+10, so should be done)
        time.sleep(5)

        # Get TX frame counts from companion TX log on tx_host (measure from beginning)
        tx_frame_counts = {}
        companion_lines = []
        try:
            companion_result = tx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []

            # Get TX frame counts (from beginning)
            tx_frame_counts = monitor_tx_frames(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log from {tx_host.name} for frame counts: {e}")

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_rx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_tx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        # Display session results with DSA info if enabled
        dsa_info = get_dsa_info(rx_host, role="RX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(rx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(rx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="RX", test_label="Multi-Core",
            measured_host=rx_host, companion_host=tx_host,
            measured_vf=rx_vf, companion_vf=tx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            multi_core=True,
        )

        display_session_results(
            "RX", dsa_label, num_sessions, fps, rx_fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        # Test passes/fails based on RX FPS achievement
        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (min {rx_fps_details['min_required_fps']:.1f} fps) (Multi-Core){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(
            TEST_PASS,
            f"Dual-host RX FPS variant test (Multi-Core){dsa_label} passed: {num_sessions} sessions @ {fps} fps",
        )

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX on {tx_host.name}: {e}")


# ============================================================================
# Redundant Mode (ST2022-7) Tests
# Uses both E810 ports per host for dual-path redundancy
# Primary path:   localhost port 0 <-> host_183 port 1 (192.168.4.x subnet)
# Redundant path: localhost port 1 <-> host_183 port 0 (192.168.5.x subnet)
# ============================================================================

# Redundant mode IP addressing
REDUNDANT_IP_PRIMARY_TX = "192.168.4.150"
REDUNDANT_IP_PRIMARY_RX = "192.168.4.206"
REDUNDANT_IP_SECONDARY_TX = "192.168.5.150"
REDUNDANT_IP_SECONDARY_RX = "192.168.5.206"


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess"],
)
def test_dualhost_vf_tx_fps_variants_single_core_redundant(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
    """
    Test TX multi-session performance in REDUNDANT mode (single-core pinning).

    Uses both E810 ports per host for ST2022-7 dual-path redundancy.
    Each session transmits on both ports simultaneously.
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    tx_host, rx_host = get_tx_rx_hosts(hosts, "tx_test", test_config)

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 0 ({tx_host.name})")
    if not hasattr(tx_host, "vfs_r") or len(tx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 1 for redundant ({tx_host.name})")
    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 0 ({rx_host.name})")
    if not hasattr(rx_host, "vfs_r") or len(rx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 1 for redundant ({rx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    tx_vf = tx_host.vfs[0]     # Primary TX VF (port 0)
    tx_vf_r = tx_host.vfs_r[0] # Redundant TX VF (port 1)
    rx_vf = rx_host.vfs[0]     # Primary RX VF
    rx_vf_r = rx_host.vfs_r[0] # Redundant RX VF

    # Configure DSA if enabled
    if use_dsa:
        dsa_config = get_host_dsa_config(tx_host)
        dsa_device = setup_host_dsa(tx_host, tx_vf, dsa_config, role="TX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({tx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX REDUNDANT{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{tx_host.name} TX: primary={tx_vf}, redundant={tx_vf_r}")
    logger.info(f"{rx_host.name} RX: primary={rx_vf}, redundant={rx_vf_r}")
    logger.info(f"Media: {media_config['filename']}")
    logger.info("=" * 80)

    build_tx_host = get_host_build_path(tx_host, build, test_config)
    build_rx_host = get_host_build_path(rx_host, build, test_config)

    # Start companion RX in redundant mode
    rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")
    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        nic_port_r=rx_vf_r,
        redundant=True,
        source_ip=REDUNDANT_IP_PRIMARY_TX,
        destination_ip=REDUNDANT_IP_PRIMARY_RX,
        source_ip_r=REDUNDANT_IP_SECONDARY_TX,
        destination_ip_r=REDUNDANT_IP_SECONDARY_RX,
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        measure_latency=True,
        replicas=num_sessions,
        sch_session_quota=SCH_SESSION_QUOTA_SINGLE_CORE,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = f"{build_rx_host}/tests/validation/logs/performance/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_redundant_rx_companion.log"
    logger.info(f"Starting companion RX (redundant) on {rx_host.name}")
    rx_app.prepare_execution(build=build_rx_host, host=rx_host)
    rx_process = rx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {rx_app.command} > {companion_log} 2>&1", cwd=build_rx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX in redundant mode
        tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")
        tx_kwargs = {
            "session_type": "st20p",
            "direction": "tx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "input_file": media_file_path,
            "nic_port": tx_vf,
            "nic_port_r": tx_vf_r,
            "redundant": True,
            "source_ip": REDUNDANT_IP_PRIMARY_TX,
            "destination_ip": REDUNDANT_IP_PRIMARY_RX,
            "source_ip_r": REDUNDANT_IP_SECONDARY_TX,
            "destination_ip_r": REDUNDANT_IP_SECONDARY_RX,
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "replicas": num_sessions,
            "sch_session_quota": SCH_SESSION_QUOTA_SINGLE_CORE,
            "rx_queues_cnt": 64,
            "test_time": test_time,
            "tsc": True,
        }
        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)
        tx_app.prepare_execution(build=build_tx_host, host=tx_host)

        logger.info(f"Running TX (redundant) on {tx_host.name}{dsa_label}")
        result = run(tx_app.command, cwd=build_tx_host, timeout=test_time + 60, testcmd=True, host=tx_host)

        # Log TX output
        logger.info("=" * 80)
        logger.info(f"TX Process Output ({tx_host.name}) - REDUNDANT")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "mismatch", "redundant"]):
                    logger.info(f"TX: {line}")
        logger.info("=" * 80)

        # Retrieve companion RX logs
        logger.info("=" * 80)
        logger.info(f"Companion RX Process Logs ({rx_host.name}) - REDUNDANT")
        logger.info("=" * 80)
        get_companion_log_summary(rx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"TX process (Redundant){dsa_label} on {tx_host.name} failed with exit code {result.return_code}")
            pytest.fail(f"TX process (Redundant){dsa_label} on {tx_host.name} failed with exit code {result.return_code}")

        success, successful_count, fps_details = monitor_tx_fps(result.stdout_text.splitlines(), fps, num_sessions)
        tx_frame_counts = monitor_tx_frames(result.stdout_text.splitlines(), num_sessions)

        time.sleep(5)
        companion_lines = []
        try:
            companion_result = rx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            rx_frame_counts = monitor_rx_frames_simple(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log: {e}")
            rx_frame_counts = {}

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_tx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_rx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        dsa_info = get_dsa_info(tx_host, role="TX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(tx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(tx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="TX", test_label="REDUNDANT Single-Core",
            measured_host=tx_host, companion_host=rx_host,
            measured_vf=tx_vf, companion_vf=rx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            measured_vf_r=tx_vf_r, companion_vf_r=rx_vf_r, redundant=True,
        )

        display_session_results(
            "TX", f" REDUNDANT{dsa_label}", num_sessions, fps, fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        if not success:
            failure_msg = f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (Redundant){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host TX REDUNDANT test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX on {rx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48, 64],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess", "64sess"],
)
def test_dualhost_vf_rx_fps_variants_single_core_redundant(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
    """
    Test RX multi-session performance in REDUNDANT mode (single-core pinning).

    Uses both E810 ports per host for ST2022-7 dual-path redundancy.
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    tx_host, rx_host = get_tx_rx_hosts(hosts, "rx_test", test_config)

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 0 ({rx_host.name})")
    if not hasattr(rx_host, "vfs_r") or len(rx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 1 for redundant ({rx_host.name})")
    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 0 ({tx_host.name})")
    if not hasattr(tx_host, "vfs_r") or len(tx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 1 for redundant ({tx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    rx_vf = rx_host.vfs[0]
    rx_vf_r = rx_host.vfs_r[0]
    tx_vf = tx_host.vfs[0]
    tx_vf_r = tx_host.vfs_r[0]

    if use_dsa:
        dsa_config = get_host_dsa_config(rx_host)
        dsa_device = setup_host_dsa(rx_host, rx_vf, dsa_config, role="RX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({rx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX REDUNDANT{dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{rx_host.name} RX: primary={rx_vf}, redundant={rx_vf_r}")
    logger.info(f"{tx_host.name} TX: primary={tx_vf}, redundant={tx_vf_r}")
    logger.info("=" * 80)

    build_rx_host = get_host_build_path(rx_host, build, test_config)
    build_tx_host = get_host_build_path(tx_host, build, test_config)

    # Start companion TX in redundant mode
    tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")
    tx_app.create_command(
        session_type="st20p",
        direction="tx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        input_file=media_file_path,
        nic_port=tx_vf,
        nic_port_r=tx_vf_r,
        redundant=True,
        source_ip=REDUNDANT_IP_PRIMARY_TX,
        destination_ip=REDUNDANT_IP_PRIMARY_RX,
        source_ip_r=REDUNDANT_IP_SECONDARY_TX,
        destination_ip_r=REDUNDANT_IP_SECONDARY_RX,
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        replicas=num_sessions,
        rx_queues_cnt=64,
        test_time=test_time + 10,
        pacing_way="tsc",
    )

    companion_log = f"{build_tx_host}/tests/validation/logs/performance/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_redundant_tx_companion.log"
    logger.info(f"Starting companion TX (redundant) on {tx_host.name}")
    tx_app.prepare_execution(build=build_tx_host, host=tx_host)
    tx_process = tx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {tx_app.command} > {companion_log} 2>&1", cwd=build_tx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX in redundant mode
        rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")
        rx_kwargs = {
            "session_type": "st20p",
            "direction": "rx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "output_file": "/dev/null",
            "nic_port": rx_vf,
            "nic_port_r": rx_vf_r,
            "redundant": True,
            "source_ip": REDUNDANT_IP_PRIMARY_TX,
            "destination_ip": REDUNDANT_IP_PRIMARY_RX,
            "source_ip_r": REDUNDANT_IP_SECONDARY_TX,
            "destination_ip_r": REDUNDANT_IP_SECONDARY_RX,
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "measure_latency": True,
            "replicas": num_sessions,
            "sch_session_quota": SCH_SESSION_QUOTA_SINGLE_CORE,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }
        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)
        rx_app.prepare_execution(build=build_rx_host, host=rx_host)

        logger.info(f"Running RX (redundant) on {rx_host.name}{dsa_label}")
        result = run(rx_app.command, cwd=build_rx_host, timeout=test_time + 60, testcmd=True, host=rx_host)

        # Log RX output
        logger.info("=" * 80)
        logger.info(f"RX Process Output ({rx_host.name}) - REDUNDANT")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["frame", "session", "error", "fail", "warn", "redundant"]):
                    logger.info(f"RX: {line}")
        logger.info("=" * 80)

        # Retrieve companion TX logs
        logger.info("=" * 80)
        logger.info(f"Companion TX Process Logs ({tx_host.name}) - REDUNDANT")
        logger.info("=" * 80)
        get_companion_log_summary(tx_host, companion_log, max_lines=50)
        logger.info("=" * 80)

        if result.return_code != 0:
            log_fail(f"RX process (Redundant){dsa_label} on {rx_host.name} failed with exit code {result.return_code}")
            pytest.fail(f"RX process (Redundant){dsa_label} on {rx_host.name} failed with exit code {result.return_code}")

        rx_fps_success, rx_fps_successful_count, rx_fps_details = monitor_rx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )
        rx_frame_counts = monitor_rx_frames_simple(result.stdout_text.splitlines(), num_sessions)

        time.sleep(5)
        tx_frame_counts = {}
        companion_lines = []
        try:
            companion_result = tx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            tx_frame_counts = monitor_tx_frames(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log: {e}")

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_rx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_tx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        dsa_info = get_dsa_info(rx_host, role="RX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(rx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(rx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="RX", test_label="REDUNDANT Single-Core",
            measured_host=rx_host, companion_host=tx_host,
            measured_vf=rx_vf, companion_vf=tx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            measured_vf_r=rx_vf_r, companion_vf_r=tx_vf_r, redundant=True,
        )

        display_session_results(
            "RX", f" REDUNDANT{dsa_label}", num_sessions, fps, rx_fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        if not rx_fps_success:
            failure_msg = f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (Redundant){dsa_label}"
            log_fail(failure_msg)
            pytest.fail(failure_msg)

        logger.log(TEST_PASS, f"Dual-host RX REDUNDANT test{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX on {tx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess"],
)
def test_dualhost_vf_tx_fps_variants_multi_core_redundant(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
    """
    Test TX multi-session performance in REDUNDANT mode (multi-core distribution).

    Uses both E810 ports per host for ST2022-7 dual-path redundancy.
    Sessions distributed across multiple cores.
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    tx_host, rx_host = get_tx_rx_hosts(hosts, "tx_test", test_config)

    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 0 ({tx_host.name})")
    if not hasattr(tx_host, "vfs_r") or len(tx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 1 for redundant ({tx_host.name})")
    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 0 ({rx_host.name})")
    if not hasattr(rx_host, "vfs_r") or len(rx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 1 for redundant ({rx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    tx_vf = tx_host.vfs[0]
    tx_vf_r = tx_host.vfs_r[0]
    rx_vf = rx_host.vfs[0]
    rx_vf_r = rx_host.vfs_r[0]

    if use_dsa:
        dsa_config = get_host_dsa_config(tx_host)
        dsa_device = setup_host_dsa(tx_host, tx_vf, dsa_config, role="TX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({tx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF TX REDUNDANT (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{tx_host.name} TX: primary={tx_vf}, redundant={tx_vf_r}")
    logger.info(f"{rx_host.name} RX: primary={rx_vf}, redundant={rx_vf_r}")
    logger.info("=" * 80)

    build_tx_host = get_host_build_path(tx_host, build, test_config)
    build_rx_host = get_host_build_path(rx_host, build, test_config)

    # Start companion RX in redundant mode
    rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")
    rx_app.create_command(
        session_type="st20p",
        direction="rx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        output_file="/dev/null",
        nic_port=rx_vf,
        nic_port_r=rx_vf_r,
        redundant=True,
        source_ip=REDUNDANT_IP_PRIMARY_TX,
        destination_ip=REDUNDANT_IP_PRIMARY_RX,
        source_ip_r=REDUNDANT_IP_SECONDARY_TX,
        destination_ip_r=REDUNDANT_IP_SECONDARY_RX,
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        measure_latency=True,
        replicas=num_sessions,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = f"{build_rx_host}/tests/validation/logs/performance/dualhost_vf_tx_fps{fps}_{num_sessions}s{dsa_suffix}_redundant_multicore_rx_companion.log"
    logger.info(f"Starting companion RX (redundant) on {rx_host.name}")
    rx_app.prepare_execution(build=build_rx_host, host=rx_host)
    rx_process = rx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {rx_app.command} > {companion_log} 2>&1", cwd=build_rx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure TX in redundant mode (no sch_session_quota for multi-core)
        tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")
        tx_kwargs = {
            "session_type": "st20p",
            "direction": "tx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "input_file": media_file_path,
            "nic_port": tx_vf,
            "nic_port_r": tx_vf_r,
            "redundant": True,
            "source_ip": REDUNDANT_IP_PRIMARY_TX,
            "destination_ip": REDUNDANT_IP_PRIMARY_RX,
            "source_ip_r": REDUNDANT_IP_SECONDARY_TX,
            "destination_ip_r": REDUNDANT_IP_SECONDARY_RX,
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "replicas": num_sessions,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }
        if use_dsa:
            tx_kwargs["dma_dev"] = dsa_device

        tx_app.create_command(**tx_kwargs)
        tx_app.prepare_execution(build=build_tx_host, host=tx_host)

        logger.info(f"Running TX (redundant, Multi-Core) on {tx_host.name}{dsa_label}")
        result = run(tx_app.command, cwd=build_tx_host, timeout=test_time + 60, testcmd=True, host=tx_host)

        # Log TX output
        logger.info("=" * 80)
        logger.info(f"TX Process Output ({tx_host.name}) - REDUNDANT Multi-Core")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["fps", "session", "error", "fail", "warn", "redundant"]):
                    logger.info(f"TX: {line}")
        logger.info("=" * 80)

        get_companion_log_summary(rx_host, companion_log, max_lines=50)

        if result.return_code != 0:
            pytest.fail(f"TX process (Redundant Multi-Core){dsa_label} failed with exit code {result.return_code}")

        success, successful_count, fps_details = monitor_tx_fps(result.stdout_text.splitlines(), fps, num_sessions)
        tx_frame_counts = monitor_tx_frames(result.stdout_text.splitlines(), num_sessions)

        time.sleep(5)
        companion_lines = []
        try:
            companion_result = rx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            rx_frame_counts = monitor_rx_frames_simple(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log: {e}")
            rx_frame_counts = {}

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_tx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_rx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        dsa_info = get_dsa_info(tx_host, role="TX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(tx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(tx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="TX", test_label="REDUNDANT Multi-Core",
            measured_host=tx_host, companion_host=rx_host,
            measured_vf=tx_vf, companion_vf=rx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            measured_vf_r=tx_vf_r, companion_vf_r=rx_vf_r, redundant=True, multi_core=True,
        )

        display_session_results(
            "TX", f" REDUNDANT Multi-Core{dsa_label}", num_sessions, fps, fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        if not success:
            pytest.fail(f"Only {successful_count}/{num_sessions} sessions reached target {fps} fps (Redundant Multi-Core){dsa_label}")

        logger.log(TEST_PASS, f"Dual-host TX REDUNDANT Multi-Core{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            rx_process.stop()
            if rx_process.running:
                time.sleep(2)
                rx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion RX on {rx_host.name}: {e}")


@pytest.mark.nightly
@pytest.mark.performance
@pytest.mark.parametrize("use_dsa", [False, True], ids=["no-dsa", "dsa"])
@pytest.mark.parametrize("fps", [25, 30, 50, 59], ids=["25fps", "30fps", "50fps", "59fps"])
@pytest.mark.parametrize(
    "num_sessions",
    [1, 2, 4, 8, 14, 15, 16, 17, 20, 24, 32, 33, 34, 36, 42, 48, 64],
    ids=["1sess", "2sess", "4sess", "8sess", "14sess", "15sess", "16sess", "17sess", "20sess", "24sess", "32sess", "33sess", "34sess", "36sess", "42sess", "48sess", "64sess"],
)
def test_dualhost_vf_rx_fps_variants_multi_core_redundant(
    hosts, build, media, test_time, nic_port_list, fps, num_sessions, use_dsa, test_config
) -> None:
    """
    Test RX multi-session performance in REDUNDANT mode (multi-core distribution).

    Uses both E810 ports per host for ST2022-7 dual-path redundancy.
    Sessions distributed across multiple cores.
    """
    if len(hosts) < 2:
        pytest.skip("Test requires at least 2 hosts for dual-host testing")

    tx_host, rx_host = get_tx_rx_hosts(hosts, "rx_test", test_config)

    if not hasattr(rx_host, "vfs") or len(rx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 0 ({rx_host.name})")
    if not hasattr(rx_host, "vfs_r") or len(rx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on RX host port 1 for redundant ({rx_host.name})")
    if not hasattr(tx_host, "vfs") or len(tx_host.vfs) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 0 ({tx_host.name})")
    if not hasattr(tx_host, "vfs_r") or len(tx_host.vfs_r) < 1:
        pytest.skip(f"Test requires at least 1 VF on TX host port 1 for redundant ({tx_host.name})")

    media_config = MEDIA_CONFIGS[fps]
    tx_media_path = get_host_media_path(tx_host, media, test_config)
    media_file_path = f"{tx_media_path}/{media_config['filename']}"

    rx_vf = rx_host.vfs[0]
    rx_vf_r = rx_host.vfs_r[0]
    tx_vf = tx_host.vfs[0]
    tx_vf_r = tx_host.vfs_r[0]

    if use_dsa:
        dsa_config = get_host_dsa_config(rx_host)
        dsa_device = setup_host_dsa(rx_host, rx_vf, dsa_config, role="RX")
        if dsa_device is None:
            pytest.skip(f"DSA device not available on measured host ({rx_host.name})")
    else:
        dsa_device = None
    dsa_suffix = "_dsa" if use_dsa else ""
    dsa_label = f" with DSA ({dsa_device})" if use_dsa else ""

    logger.info("=" * 80)
    logger.info(f"Dual-Host VF RX REDUNDANT (Multi-Core){dsa_label}: {num_sessions} sessions @ {fps} fps")
    logger.info(f"{rx_host.name} RX: primary={rx_vf}, redundant={rx_vf_r}")
    logger.info(f"{tx_host.name} TX: primary={tx_vf}, redundant={tx_vf_r}")
    logger.info("=" * 80)

    build_rx_host = get_host_build_path(rx_host, build, test_config)
    build_tx_host = get_host_build_path(tx_host, build, test_config)

    # Start companion TX in redundant mode
    tx_app = RxTxApp(app_path=f"{build_tx_host}/tests/tools/RxTxApp/build")
    tx_app.create_command(
        session_type="st20p",
        direction="tx",
        test_mode="unicast",
        transport_format=media_config["format"],
        pixel_format=media_config["pixel_format"],
        width=media_config["width"],
        height=media_config["height"],
        framerate=f"p{fps}",
        input_file=media_file_path,
        nic_port=tx_vf,
        nic_port_r=tx_vf_r,
        redundant=True,
        source_ip=REDUNDANT_IP_PRIMARY_TX,
        destination_ip=REDUNDANT_IP_PRIMARY_RX,
        source_ip_r=REDUNDANT_IP_SECONDARY_TX,
        destination_ip_r=REDUNDANT_IP_SECONDARY_RX,
        port=5004,
        payload_type=96,
        pacing="narrow",
        packing="GPM",
        replicas=num_sessions,
        rx_queues_cnt=64,
        test_time=test_time + 10,
    )

    companion_log = f"{build_tx_host}/tests/validation/logs/performance/dualhost_vf_rx_fps{fps}_{num_sessions}s{dsa_suffix}_redundant_multicore_tx_companion.log"
    logger.info(f"Starting companion TX (redundant) on {tx_host.name}")
    tx_app.prepare_execution(build=build_tx_host, host=tx_host)
    tx_process = tx_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && {tx_app.command} > {companion_log} 2>&1", cwd=build_tx_host, shell=True
    )
    time.sleep(10)

    try:
        # Configure RX in redundant mode (no sch_session_quota for multi-core)
        rx_app = RxTxApp(app_path=f"{build_rx_host}/tests/tools/RxTxApp/build")
        rx_kwargs = {
            "session_type": "st20p",
            "direction": "rx",
            "test_mode": "unicast",
            "transport_format": media_config["format"],
            "pixel_format": media_config["pixel_format"],
            "width": media_config["width"],
            "height": media_config["height"],
            "framerate": f"p{fps}",
            "output_file": "/dev/null",
            "nic_port": rx_vf,
            "nic_port_r": rx_vf_r,
            "redundant": True,
            "source_ip": REDUNDANT_IP_PRIMARY_TX,
            "destination_ip": REDUNDANT_IP_PRIMARY_RX,
            "source_ip_r": REDUNDANT_IP_SECONDARY_TX,
            "destination_ip_r": REDUNDANT_IP_SECONDARY_RX,
            "port": 5004,
            "payload_type": 96,
            "pacing": "narrow",
            "packing": "GPM",
            "measure_latency": True,
            "replicas": num_sessions,
            "rx_queues_cnt": 64,
            "test_time": test_time,
        }
        if use_dsa:
            rx_kwargs["dma_dev"] = dsa_device

        rx_app.create_command(**rx_kwargs)
        rx_app.prepare_execution(build=build_rx_host, host=rx_host)

        logger.info(f"Running RX (redundant, Multi-Core) on {rx_host.name}{dsa_label}")
        result = run(rx_app.command, cwd=build_rx_host, timeout=test_time + 60, testcmd=True, host=rx_host)

        # Log RX output
        logger.info("=" * 80)
        logger.info(f"RX Process Output ({rx_host.name}) - REDUNDANT Multi-Core")
        logger.info("=" * 80)
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                if any(keyword in line.lower() for keyword in ["frame", "session", "error", "fail", "warn", "redundant"]):
                    logger.info(f"RX: {line}")
        logger.info("=" * 80)

        get_companion_log_summary(tx_host, companion_log, max_lines=50)

        if result.return_code != 0:
            pytest.fail(f"RX process (Redundant Multi-Core){dsa_label} failed with exit code {result.return_code}")

        rx_fps_success, rx_fps_successful_count, rx_fps_details = monitor_rx_fps(
            result.stdout_text.splitlines(), fps, num_sessions
        )
        rx_frame_counts = monitor_rx_frames_simple(result.stdout_text.splitlines(), num_sessions)

        time.sleep(5)
        tx_frame_counts = {}
        companion_lines = []
        try:
            companion_result = tx_host.connection.execute_command(f"cat {companion_log}", shell=True)
            companion_lines = companion_result.stdout.splitlines() if companion_result.stdout else []
            tx_frame_counts = monitor_tx_frames(companion_lines, num_sessions)
        except Exception as e:
            logger.warning(f"Could not read companion log: {e}")

        # Monitor throughput (informational only, no pass/fail impact)
        measured_lines = result.stdout_text.splitlines()
        measured_throughput = monitor_rx_throughput(measured_lines, num_sessions)
        measured_dev_rate = monitor_dev_rate(measured_lines)
        companion_throughput = monitor_tx_throughput(companion_lines, num_sessions)
        companion_dev_rate_data = monitor_dev_rate(companion_lines)

        dsa_info = get_dsa_info(rx_host, role="RX") if use_dsa else None

        # Print command and config before summary
        logger.info("=" * 80)
        logger.info("MEASURED APP COMMAND:")
        logger.info(rx_app.command)
        logger.info("-" * 80)
        logger.info("MEASURED APP CONFIG:")
        logger.info(json.dumps(rx_app.config, indent=2))
        logger.info("=" * 80)

        display_test_config_summary(
            direction="RX", test_label="REDUNDANT Multi-Core",
            measured_host=rx_host, companion_host=tx_host,
            measured_vf=rx_vf, companion_vf=tx_vf,
            num_sessions=num_sessions, fps=fps, media_config=media_config,
            use_dsa=use_dsa, dsa_device=dsa_device, dsa_info=dsa_info,
            measured_vf_r=rx_vf_r, companion_vf_r=tx_vf_r, redundant=True, multi_core=True,
        )

        display_session_results(
            "RX", f" REDUNDANT Multi-Core{dsa_label}", num_sessions, fps, rx_fps_details, tx_frame_counts, rx_frame_counts,
            dsa_info=dsa_info, throughput_details=measured_throughput, dev_rate=measured_dev_rate,
            companion_throughput_details=companion_throughput, companion_dev_rate=companion_dev_rate_data,
        )

        if not rx_fps_success:
            pytest.fail(f"Only {rx_fps_successful_count}/{num_sessions} sessions reached target {fps} fps (Redundant Multi-Core){dsa_label}")

        logger.log(TEST_PASS, f"Dual-host RX REDUNDANT Multi-Core{dsa_label} passed: {num_sessions} sessions @ {fps} fps")

    finally:
        try:
            tx_process.stop()
            if tx_process.running:
                time.sleep(2)
                tx_process.kill()
        except Exception as e:
            logger.warning(f"Error stopping companion TX on {tx_host.name}: {e}")