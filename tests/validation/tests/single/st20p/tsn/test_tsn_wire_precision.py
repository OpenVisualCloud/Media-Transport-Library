# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
"""TSN/TXTIME wire-precision tests for ST 2110-20.

Verify that packets transmitted with ``--pacing_way tsn`` arrive on the wire
with near-perfect uniform spacing (R² ≥ 0.99 from a linear fit of HW
timestamps vs packet index).

Prerequisites:
  - E810/E830 NIC with TSN (TXTIME / LaunchTime) support on PF
  - Two network_interfaces in topology_config.yaml (TX PF on index 0, RX VF on index 1)
  - ``capture_cfg.enable: true`` in test_config.yaml
  - Media files on NFS / ramdisk (standard validation setup)

Note: TXTIME is only supported on the PF (ice driver), not on VFs (iavf).
      TX uses the PF bound to DPDK, RX uses a VF on the second port.
      Multicast addressing is used to avoid ARP issues between different
      physical ports.
"""

import ipaddress
import logging
import os
import re
from pathlib import Path

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10

from .analyze_tsn_pcap import compute_metrics, format_metrics_report, parse_pcap

logger = logging.getLogger(__name__)

# --- Thresholds -----------------------------------------------------------
# R² of arrival-time vs packet-index linear fit.  TSN/TXTIME on E830 PF
# produces ≥0.99 in lab; 0.99 gives margin for thermal/load/cross-port jitter.
R2_THRESHOLD = 0.99

# Maximum tolerable burst percentage (wire-speed gaps < 10% of median step).
# TSN should pace every packet; >30% burst means pacing is not working.
MAX_BURST_PCT = 30.0

# Minimum packets to consider the capture valid.
MIN_PACKETS = 1000

# UDP port used by ST 2110-20 sessions
ST20_PORT = 20000


# --- Helpers --------------------------------------------------------------


def _run_pacing_test(
    setup_interfaces,
    application,
    mtl_path,
    host,
    recorder,
    test_config,
    media_file_info,
    media_file_path,
    pacing_way,
    test_time,
):
    """Run a single pacing test and return pcap metrics.

    Shared boilerplate for all pacing tests: configure interfaces, build
    RxTxApp config, execute, capture pcap, compute R²/burst metrics.

    Returns:
        dict with keys r_squared, pct_burst, intra_std_ns, n_packets, etc.
    """
    interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
        tx_interface_type="PF",
        rx_interface_type="VF",
        tx_index=0,
        rx_index=1,
    )
    ptp_source_ip = _ptp_source_ip(test_config)

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ptp_source_ip,
        "destination_ip": "239.168.85.20",
        "port": ST20_PORT,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "multicast",
        "pacing": "narrow",
        "pacing_way": pacing_way,
        "test_time": test_time,
        # Enable MTL's built-in PTP. MTL is a PTP slave; ptp4l on the
        # capture interface participates in the lab PTP domain and lets
        # both TX and capture clocks track the network grandmaster.
        # The framework also waits ptp_sync_time (default 50 s) before
        # starting capture, which gives TSN time to leave warmup.
        "enable_ptp": True,
    }

    actual_test_time = max(test_time, 10)

    _create_command_with_ptp_source_ip(application, config_params, ptp_source_ip)
    # fail_on_error=False: warmup may cause fps to dip below the standard
    # ST2110 tolerance; pcap-based assertions are the real pass criteria.
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=recorder,
        fail_on_error=False,
    )
    _save_mtl_log(application, pacing_way)
    _assert_mtl_ptp_connected(application.last_output)

    timestamps = parse_pcap(recorder.pcap_file, udp_port=ST20_PORT)
    assert len(timestamps) >= MIN_PACKETS, (
        f"Capture too small: {len(timestamps)} packets (need ≥{MIN_PACKETS}). "
        f"Check that capture_cfg is enabled and the stream ran long enough."
    )

    metrics = compute_metrics(timestamps)
    report = format_metrics_report(metrics)
    logger.info(
        "%s pacing (%s) wire-precision metrics:\n%s",
        pacing_way.upper(),
        f"{media_file_info['width']}x{media_file_info['height']}p{media_file_info['fps']}",
        report,
    )
    return metrics


def _ptp_source_ip(test_config):
    capture_cfg = test_config.get("capture_cfg", {}) if test_config else {}
    sniff_interface_ip = capture_cfg.get("sniff_interface_ip")
    if not sniff_interface_ip:
        return ip_pools.tx[0]

    try:
        capture_ip = ipaddress.ip_interface(sniff_interface_ip)
    except ValueError:
        logger.warning("Invalid sniff_interface_ip=%s", sniff_interface_ip)
        return ip_pools.tx[0]

    network = capture_ip.network
    candidate = network.network_address + 8
    if candidate == capture_ip.ip:
        candidate = network.network_address + 9
    if candidate not in network or candidate in (
        network.network_address,
        network.broadcast_address,
    ):
        return ip_pools.tx[0]
    return str(candidate)


def _create_command_with_ptp_source_ip(application, config_params, ptp_source_ip):
    original_tx_ip = ip_pools.tx[0]
    ip_pools.tx[0] = ptp_source_ip
    try:
        application.create_command(**config_params)
    finally:
        ip_pools.tx[0] = original_tx_ip

    logger.info("Using MTL TX PTP source IP %s", ptp_source_ip)


def _save_mtl_log(application, pacing_way):
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "tsn_wire_precision")
    case_id = case_id.rsplit(" (", 1)[0]
    log_folder = os.environ.get("MTL_LOG_FOLDER", "logs")
    log_path = Path(log_folder) / "latest" / f"{case_id}.{pacing_way}.mtl.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    output = application.last_output or ""
    with log_path.open("w", encoding="utf-8") as log_file:
        log_file.write(f"RxTxApp/MTL command: {application.command or '<unknown>'}\n")
        log_file.write(f"Return code: {application.last_return_code}\n")
        log_file.write("\n")
        log_file.write(output)
        if output and not output.endswith("\n"):
            log_file.write("\n")

    logger.info("Saved RxTxApp/MTL log to %s", log_path)


def _assert_mtl_ptp_connected(output):
    output = output or ""
    lines = output.splitlines()
    ptp_lines = [(idx, line) for idx, line in enumerate(lines) if "PTP(0):" in line]
    delta_lines = [
        (idx, line) for idx, line in ptp_lines if "PTP(0): delta avg" in line
    ]
    not_connected_lines = [
        (idx, line) for idx, line in ptp_lines if "PTP(0): not connected" in line
    ]
    stats_lines = [
        (idx, line)
        for idx, line in ptp_lines
        if "t2_t1_delta" in line and "t4_t3_delta" in line
    ]

    last_delta_idx = delta_lines[-1][0] if delta_lines else -1
    last_not_connected_idx = not_connected_lines[-1][0] if not_connected_lines else -1
    last_stats_idx = stats_lines[-1][0] if stats_lines else -1
    last_stats_line = stats_lines[-1][1] if stats_lines else ""
    last_stats_zero = False
    stats_match = re.search(
        r"sync cnt (\d+).*t2_t1_delta (-?\d+) t4_t3_delta (-?\d+)",
        last_stats_line,
    )
    if stats_match:
        last_stats_zero = stats_match.group(2) == "0" and stats_match.group(3) == "0"

    if (
        delta_lines
        and last_not_connected_idx <= last_delta_idx
        and not (last_stats_zero and last_stats_idx > last_delta_idx)
    ):
        return

    announce_count = output.count("ptp_parse_announce")
    txtime_delay_req_count = output.count("TXTIME t3")
    recent_ptp = (
        "\n".join(line for _, line in ptp_lines[-8:])
        if ptp_lines
        else "<no PTP(0) lines>"
    )
    last_delta = delta_lines[-1][1] if delta_lines else "<no PTP(0): delta avg line>"
    last_not_connected = (
        not_connected_lines[-1][1]
        if not_connected_lines
        else "<no PTP(0): not connected line>"
    )
    last_stats = last_stats_line or "<no PTP stats line>"

    pytest.fail(
        "MTL PTP did not stay synchronized. Expected a recent PTP(0): delta avg "
        "line and nonzero t2/t1 + t4/t3 deltas.\n"
        f"PTP announce lines: {announce_count}\n"
        f"TXTIME Delay_Req lines: {txtime_delay_req_count}\n"
        f"Last delta line: {last_delta}\n"
        f"Last not-connected line: {last_not_connected}\n"
        f"Last stats line: {last_stats}\n"
        f"Recent MTL PTP lines:\n{recent_ptp}"
    )


def _assert_pacing_quality(metrics, label=""):
    """Assert R² ≥ threshold and burst ≤ threshold."""
    prefix = f"{label}: " if label else ""
    assert metrics["r_squared"] >= R2_THRESHOLD, (
        f"{prefix}R²={metrics['r_squared']:.6f} < {R2_THRESHOLD} — "
        f"packets are not uniformly spaced."
    )
    assert metrics["pct_burst"] <= MAX_BURST_PCT, (
        f"{prefix}Burst={metrics['pct_burst']:.1f}% > {MAX_BURST_PCT}% — "
        f"too many wire-speed gaps."
    )


# ---------------------------------------------------------------------------
# Test: 1080p @ 50 fps — standard HD resolution
# ---------------------------------------------------------------------------
@pytest.mark.tsn
@pytest.mark.ptp
@pytest.mark.smoke
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
def test_tsn_wire_precision_1080p(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """TSN pacing at 1080p50: assert R² ≥ 0.99 on captured packets."""
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    metrics = _run_pacing_test(
        setup_interfaces,
        application,
        mtl_path,
        host,
        pcap_capture,
        test_config,
        media_file_info,
        media_file_path,
        "tsn",
        test_time,
    )
    _assert_pacing_quality(metrics, "TSN 1080p50")


# ---------------------------------------------------------------------------
# Test: 720p @ 30 fps — low-resolution regression (vrx underflow bug)
# ---------------------------------------------------------------------------
@pytest.mark.tsn
@pytest.mark.ptp
@pytest.mark.smoke
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Pedestrian_720p"]],
    indirect=["media_file"],
    ids=["Pedestrian_720p"],
)
def test_tsn_wire_precision_720p(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """TSN pacing at 720p30: low-res regression test (vrx underflow fix)."""
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    metrics = _run_pacing_test(
        setup_interfaces,
        application,
        mtl_path,
        host,
        pcap_capture,
        test_config,
        media_file_info,
        media_file_path,
        "tsn",
        test_time,
    )
    _assert_pacing_quality(metrics, "TSN 720p30")


# ---------------------------------------------------------------------------
# Test: 4K @ 60 fps — assert-negative: TSN cannot sustain wire-precision
# ---------------------------------------------------------------------------
@pytest.mark.tsn
@pytest.mark.ptp
@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Crosswalk_4K"]],
    indirect=["media_file"],
    ids=["Crosswalk_4K"],
)
def test_tsn_wire_precision_4k60(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """Negative test: TSN pacing at 4K60 (~12 Gbps) degrades.

    At 4K 60fps the per-packet pacing interval (~3.3 µs for ~300K pkt/s)
    is too tight for the TXTIME doorbell latency.  Assert that R² drops
    below the wire-precision threshold OR burst rises above the limit.

    If this test *fails* (i.e. TSN passes at 4K60), it means HW/FW improved
    and the negative assertion should be re-evaluated.
    """
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    metrics = _run_pacing_test(
        setup_interfaces,
        application,
        mtl_path,
        host,
        pcap_capture,
        test_config,
        media_file_info,
        media_file_path,
        "tsn",
        test_time,
    )

    r2 = metrics["r_squared"]
    burst = metrics["pct_burst"]
    degraded = r2 < R2_THRESHOLD or burst > MAX_BURST_PCT
    logger.info(
        "4K60 TSN degradation check: R²=%.6f burst=%.1f%% → %s",
        r2,
        burst,
        "DEGRADED (expected)" if degraded else "PASSED (unexpected!)",
    )
    assert degraded, (
        f"TSN unexpectedly meets wire-precision at 4K60: "
        f"R²={r2:.6f} ≥ {R2_THRESHOLD} AND burst={burst:.1f}% ≤ {MAX_BURST_PCT}%. "
        f"If HW/FW improved, update this negative test."
    )


# ---------------------------------------------------------------------------
# Test: TSN vs TSC jitter differential — TSN must be meaningfully better
# ---------------------------------------------------------------------------
@pytest.mark.tsn
@pytest.mark.ptp
@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
def test_tsn_vs_tsc_jitter(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """Back-to-back TSN vs TSC: TSN must have lower jitter (std) by ≥5×.

    This test runs TSN first (with pcap capture), then TSC (with a second
    capture), and compares intra-frame jitter standard deviations.
    """
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # --- Run TSN ---
    tsn_metrics = _run_pacing_test(
        setup_interfaces,
        application,
        mtl_path,
        host,
        pcap_capture,
        test_config,
        media_file_info,
        media_file_path,
        "tsn",
        test_time,
    )
    _assert_pacing_quality(tsn_metrics, "TSN baseline")

    # --- Run TSC (separate capture) ---
    from create_pcap_file.netsniff import NetsniffRecorder

    tsc_recorder = NetsniffRecorder(
        host=host,
        test_name="tsn_vs_tsc__tsc_run",
        pcap_dir="/tmp",
        interface=pcap_capture.interface,
        capture_time=max(test_time, 10),
    )

    tsc_metrics = _run_pacing_test(
        setup_interfaces,
        application,
        mtl_path,
        host,
        tsc_recorder,
        test_config,
        media_file_info,
        media_file_path,
        "tsc",
        test_time,
    )

    # --- Compare ---
    tsn_std = tsn_metrics["intra_std_ns"]
    tsc_std = tsc_metrics["intra_std_ns"]

    jitter_ratio = tsc_std / tsn_std if tsn_std > 0 else float("inf")
    logger.info(
        "Jitter ratio (TSC/TSN): %.1f×  (TSN=%.0fns, TSC=%.0fns)",
        jitter_ratio,
        tsn_std,
        tsc_std,
    )
    assert jitter_ratio >= 5.0, (
        f"TSN jitter not significantly better than TSC: "
        f"ratio={jitter_ratio:.1f}× (need ≥5×). "
        f"TSN std={tsn_std:.0f}ns, TSC std={tsc_std:.0f}ns"
    )
