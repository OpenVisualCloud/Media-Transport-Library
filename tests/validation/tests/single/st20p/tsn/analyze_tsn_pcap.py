# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
"""Pcap analyzer for TSN wire-precision tests.

Parses a pcap file (libpcap or pcapng via dpkt), extracts HW timestamps
for UDP packets on a given port, and computes scheduling-quality metrics:
R², inter-arrival percentiles, and frame-boundary gap count.

This module is intentionally dependency-light (dpkt + numpy) so it can
run inside the pytest venv without pulling in heavy frameworks.
"""

import logging
import struct
from typing import List, Optional

import dpkt
import numpy as np

logger = logging.getLogger(__name__)

# Header sizes
_ETH_HDR = 14
_IP_HDR = 20
_UDP_HDR = 8


def parse_pcap(path: str, udp_port: int = 20000) -> List[float]:
    """Extract HW-timestamp arrivals (ns) for UDP packets on *udp_port*.

    Args:
        path: Path to pcap / pcapng file.
        udp_port: UDP destination port to filter on.

    Returns:
        Sorted list of arrival timestamps in nanoseconds.
    """
    arrivals: List[float] = []

    with open(path, "rb") as fh:
        try:
            pcap = dpkt.pcap.Reader(fh)
        except ValueError:
            fh.seek(0)
            pcap = dpkt.pcapng.Reader(fh)

        for ts, buf in pcap:
            if len(buf) < _ETH_HDR + _IP_HDR + _UDP_HDR:
                continue
            eth_type = struct.unpack("!H", buf[12:14])[0]
            if eth_type != 0x0800:  # IPv4
                continue
            proto = buf[_ETH_HDR + 9]
            if proto != 17:  # UDP
                continue
            dst_port = struct.unpack(
                "!H", buf[_ETH_HDR + _IP_HDR + 2 : _ETH_HDR + _IP_HDR + 4]
            )[0]
            if dst_port != udp_port:
                continue
            arrivals.append(float(ts) * 1e9)

    arrivals.sort()
    return arrivals


def compute_metrics(
    timestamps_ns: List[float],
    expected_step_ns: Optional[float] = None,
) -> dict:
    """Compute scheduling-quality metrics from ordered HW timestamps.

    Args:
        timestamps_ns: Ordered arrival timestamps in nanoseconds.
        expected_step_ns: If ``None``, derived as median inter-arrival gap.

    Returns:
        Dictionary with keys:
            r_squared       – R² of linear fit (1.0 = perfect linearity)
            slope_ns        – fitted ns-per-packet slope
            intra_mean_ns   – mean intra-frame gap
            intra_p50_ns    – median intra-frame gap
            intra_p99_ns    – 99th-pctl intra-frame gap
            intra_std_ns    – std-dev of intra-frame gaps
            intra_max_ns    – max intra-frame gap
            frame_gap_count – number of frame-boundary gaps detected
            n_packets       – total packets analysed
            pct_on_target   – % of intra-frame gaps within 50-200% of step
            pct_burst       – % of intra-frame gaps < 10% of step (wire-speed)
    """
    arr = np.asarray(timestamps_ns, dtype=np.float64)
    n = len(arr)
    if n < 10:
        raise ValueError(f"Too few packets ({n}) for meaningful analysis")

    rel = arr - arr[0]
    indices = np.arange(n, dtype=np.float64)

    # Linear fit
    slope, intercept = np.polyfit(indices, rel, 1)
    predicted = intercept + slope * indices
    residuals = rel - predicted
    ss_res = np.sum(residuals**2)
    ss_tot = np.sum((rel - np.mean(rel)) ** 2)
    r_squared = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0

    # Inter-arrival gaps
    gaps = np.diff(rel)
    step = expected_step_ns if expected_step_ns else float(np.median(gaps))

    # Separate frame-boundary gaps (> 10× step)
    threshold = step * 10
    intra = gaps[gaps < threshold]
    frame_gaps = gaps[gaps >= threshold]

    if len(intra) == 0:
        intra = gaps  # fallback

    pct_on_target = float(
        np.sum((intra >= step * 0.5) & (intra < step * 2.0)) / len(intra) * 100
    )
    pct_burst = float(np.sum(intra < step * 0.1) / len(intra) * 100)

    return {
        "r_squared": float(r_squared),
        "slope_ns": float(slope),
        "intra_mean_ns": float(np.mean(intra)),
        "intra_p50_ns": float(np.median(intra)),
        "intra_p99_ns": float(np.percentile(intra, 99)),
        "intra_std_ns": float(np.std(intra)),
        "intra_max_ns": float(np.max(intra)),
        "frame_gap_count": int(len(frame_gaps)),
        "n_packets": n,
        "pct_on_target": pct_on_target,
        "pct_burst": pct_burst,
    }


def format_metrics_report(m: dict) -> str:
    """Format metrics dict as human-readable text for pytest log output."""
    lines = [
        f"Packets: {m['n_packets']}",
        f"R²:      {m['r_squared']:.6f}",
        f"Slope:   {m['slope_ns']:.1f} ns/pkt",
        f"Intra-frame: mean={m['intra_mean_ns']:.0f}ns  "
        f"p50={m['intra_p50_ns']:.0f}ns  p99={m['intra_p99_ns']:.0f}ns  "
        f"std={m['intra_std_ns']:.0f}ns",
        f"Frame boundaries: {m['frame_gap_count']}",
        f"On-target: {m['pct_on_target']:.1f}%  Burst: {m['pct_burst']:.1f}%",
    ]
    return "\n".join(lines)
