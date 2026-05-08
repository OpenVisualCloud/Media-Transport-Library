# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging

import pytest
from mtl_engine.execute import log_fail
from tests.single.st20p.tsn.analyze_tsn_pcap import (
    compute_metrics,
    format_metrics_report,
    parse_pcap,
)

logger = logging.getLogger(__name__)

R2_THRESHOLD = 0.99
MAX_BURST_PCT = 30.0
MIN_PACKETS = 1000
ST20_PORT = 20000


@pytest.fixture(scope="function")
def wire_precision_check(pcap_capture):
    """Soft-log wire pacing metrics for tests that already capture a pcap.

    This is intentionally not an assertion fixture. It gives RL/default pacing
    tests the same R2/burst visibility as TSN tests without changing their pass
    criteria.
    """
    yield

    if pcap_capture is None:
        logger.warning("Wire precision check skipped: pcap_capture is disabled")
        return

    pcap_file = getattr(pcap_capture, "pcap_file", None)
    if not pcap_file:
        logger.warning("Wire precision check skipped: no pcap file was captured")
        return

    try:
        timestamps = parse_pcap(pcap_file, udp_port=ST20_PORT)
    except Exception as e:
        logger.warning(
            "Wire precision check skipped: failed to parse %s: %s", pcap_file, e
        )
        return

    if len(timestamps) < MIN_PACKETS:
        logger.warning(
            "Wire precision check skipped: capture has %d packets, need at least %d",
            len(timestamps),
            MIN_PACKETS,
        )
        return

    metrics = compute_metrics(timestamps)
    logger.info(
        "Wire precision soft-check metrics:\n%s", format_metrics_report(metrics)
    )

    if metrics["r_squared"] < R2_THRESHOLD or metrics["pct_burst"] > MAX_BURST_PCT:
        log_fail(
            "Wire precision soft-check failed: "
            f"R2={metrics['r_squared']:.6f} threshold={R2_THRESHOLD}, "
            f"burst={metrics['pct_burst']:.1f}% threshold={MAX_BURST_PCT}%"
        )
    else:
        logger.info(
            "Wire precision soft-check passed: R2=%.6f burst=%.1f%%",
            metrics["r_squared"],
            metrics["pct_burst"],
        )
