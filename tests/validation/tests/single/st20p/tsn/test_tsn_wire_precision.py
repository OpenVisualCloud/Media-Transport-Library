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

import logging

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


def _assert_tsn_metrics(pcap_path: str, udp_port: int = ST20_PORT) -> dict:
    """Parse pcap, compute metrics, assert TSN quality, return metrics dict."""
    timestamps = parse_pcap(pcap_path, udp_port=udp_port)
    assert len(timestamps) >= MIN_PACKETS, (
        f"Capture too small: {len(timestamps)} packets (need ≥{MIN_PACKETS}). "
        f"Check that capture_cfg is enabled and the stream ran long enough."
    )

    metrics = compute_metrics(timestamps)
    report = format_metrics_report(metrics)
    logger.info("TSN wire-precision metrics:\n%s", report)

    assert metrics["r_squared"] >= R2_THRESHOLD, (
        f"R²={metrics['r_squared']:.6f} < {R2_THRESHOLD} — "
        f"packets are not uniformly spaced. TSN pacing may not be active.\n"
        f"{report}"
    )

    assert metrics["pct_burst"] <= MAX_BURST_PCT, (
        f"Burst={metrics['pct_burst']:.1f}% > {MAX_BURST_PCT}% — "
        f"too many wire-speed gaps; packets are being sent in bursts.\n"
        f"{report}"
    )

    return metrics


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
    pcap_capture,
    media_file,
    application,
):
    """TSN pacing at 1080p50: assert R² ≥ 0.99 on captured packets."""
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # TX on PF (index 0) for TXTIME support, RX on VF (index 1)
    interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
        tx_interface_type="PF",
        rx_interface_type="VF",
        tx_index=0,
        rx_index=1,
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
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
        "pacing_way": "tsn",
        "test_time": test_time,
    }

    actual_test_time = max(test_time, 10)

    application.create_command(**config_params)
    # fail_on_error=False: TSN warmup may cause fps to dip below the
    # standard ST2110 tolerance; the pcap-based R²/burst assertions below
    # are the actual pass criteria for wire-precision tests.
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=pcap_capture,
        fail_on_error=False,
    )

    _assert_tsn_metrics(pcap_capture.pcap_file, udp_port=ST20_PORT)


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
    pcap_capture,
    media_file,
    application,
):
    """TSN pacing at 720p30: low-res regression test (vrx underflow fix)."""
    if pcap_capture is None:
        pytest.skip("capture_cfg not enabled in test_config.yaml")

    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # TX on PF (index 0) for TXTIME support, RX on VF (index 1)
    interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
        tx_interface_type="PF",
        rx_interface_type="VF",
        tx_index=0,
        rx_index=1,
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
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
        "pacing_way": "tsn",
        "test_time": test_time,
    }

    actual_test_time = max(test_time, 10)

    application.create_command(**config_params)
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=pcap_capture,
        fail_on_error=False,
    )

    _assert_tsn_metrics(pcap_capture.pcap_file, udp_port=ST20_PORT)


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

    # TX on PF (index 0) for TXTIME support, RX on VF (index 1)
    interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
        tx_interface_type="PF",
        rx_interface_type="VF",
        tx_index=0,
        rx_index=1,
    )

    base_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
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
        "test_time": test_time,
    }

    actual_test_time = max(test_time, 10)

    # --- Run TSN ---
    tsn_params = {**base_params, "pacing_way": "tsn"}
    application.create_command(**tsn_params)
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=pcap_capture,
        fail_on_error=False,
    )

    tsn_timestamps = parse_pcap(pcap_capture.pcap_file, udp_port=ST20_PORT)
    assert len(tsn_timestamps) >= MIN_PACKETS, (
        f"TSN capture too small: {len(tsn_timestamps)} packets"
    )
    tsn_metrics = compute_metrics(tsn_timestamps)
    logger.info(
        "TSN metrics:\n%s", format_metrics_report(tsn_metrics)
    )

    # TSN must meet baseline quality on its own
    assert tsn_metrics["r_squared"] >= R2_THRESHOLD, (
        f"TSN R²={tsn_metrics['r_squared']:.6f} below threshold"
    )

    # --- Run TSC (no pcap_capture — we start our own) ---
    # Import NetsniffRecorder to create a second capture session
    from create_pcap_file.netsniff import NetsniffRecorder

    tsc_recorder = NetsniffRecorder(
        host=host,
        test_name="tsn_vs_tsc__tsc_run",
        pcap_dir="/tmp",
        interface=pcap_capture.interface,
        capture_time=actual_test_time,
    )

    tsc_params = {**base_params, "pacing_way": "tsc"}
    application.create_command(**tsc_params)
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=tsc_recorder,
        fail_on_error=False,
    )

    tsc_timestamps = parse_pcap(tsc_recorder.pcap_file, udp_port=ST20_PORT)
    assert len(tsc_timestamps) >= MIN_PACKETS, (
        f"TSC capture too small: {len(tsc_timestamps)} packets"
    )
    tsc_metrics = compute_metrics(tsc_timestamps)
    logger.info(
        "TSC metrics:\n%s", format_metrics_report(tsc_metrics)
    )

    # --- Compare ---
    tsn_std = tsn_metrics["intra_std_ns"]
    tsc_std = tsc_metrics["intra_std_ns"]

    # TSN should have at least 5× lower jitter than TSC
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
