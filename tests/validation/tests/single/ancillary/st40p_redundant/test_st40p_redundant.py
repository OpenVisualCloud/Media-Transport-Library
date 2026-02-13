# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""RxTxApp-based ST40P pipeline redundancy tests (ST 2022-7).

Single-host loopback validation of ST 2022-7 redundant ancillary (ST40P)
streams using 4 VFs on one PF — two for TX (primary + redundant) and two
for RX (primary + redundant).  Originally ported from the GStreamer
``test_st40p_redundant_progressive*`` family in
``tests/validation/tests/single/gstreamer/anc_format/test_anc_format.py``,
the module has since grown to cover additional scenarios.

Test coverage:
  - **Baseline** (``test_st40p_redundant_progressive``):
    Multicast TX/RX at p29/p50/p59 — no faults injected.
  - **Seq-gap injection** (``test_st40p_redundant_progressive_gap``):
    ``test_mode="seq-gap"`` drops packets on alternating ports; RX dedup
    recovers from the gapped path via the redundant stream.
  - **Gap + inter-path latency** (``test_st40p_redundant_progressive_gap_latency``):
    Seq-gap plus a 7 ms busy-wait delay on port R
    (``redundant_delay_ns=7000000``) with a 15 ms RX reorder window
    (``reorder_window_ns=15000000``).  Stresses the ST 2022-7 dejitter
    buffer under path asymmetry.
  - **Synthetic source** (``test_st40p_redundant_progressive_synthetic``):
    Empty ``ancillary_url`` triggers the library's built-in pattern — no
    file I/O dependency.
  - **Scan mode** (``test_st40p_redundant_scan_mode``):
    Parametrised progressive vs interlaced cadence.
  - **Unicast** (``test_st40p_redundant_unicast``):
    Unicast addressing instead of multicast (control case).

Key differences from the GStreamer variant:
  - RxTxApp does not expose ``frame-info-path`` logging, so per-frame
    ``pkts_recv_p`` / ``pkts_recv_r`` assertions are replaced by
    coarse-grained stdout validation (``app_rx_st40p_result … OK``).
  - Gap injection and delay are configured via JSON fields
    (``"test_mode"``, ``"redundant_delay_ns"``, ``"reorder_window_ns"``)
    instead of GObject properties.

Usage::

    pytest tests/validation/tests/single/ancillary/st40p_redundant/ \\
           -k st40p_redundant \\
           --topology_config=configs/topology_config.yaml \\
           --test_config=configs/test_config.yaml -v
"""

import logging
import re

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import anc_files

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _check_dedup_activity(output: list[str]) -> dict:
    """Parse RxTxApp stdout for ST 2022-7 dedup indicators.

    Returns a dict with ``primary_pkts``, ``redundant_pkts``, and
    ``dedup_drops`` scraped from the library's ``ST40`` / ``ST40P`` log
    lines.  All counters default to ``-1`` when no matching line is found.
    """
    stats: dict = {
        "primary_pkts": -1,
        "redundant_pkts": -1,
        "dedup_drops": -1,
    }

    # The library logs lines such as:
    #   "RX_ANC_SESSION(0,0): dedup primary <N> redundant <M> drop <D>"
    dedup_re = re.compile(
        r"dedup\s+primary\s+(\d+)\s+redundant\s+(\d+)\s+drop\s+(\d+)"
    )
    for line in output:
        m = dedup_re.search(line)
        if m:
            stats["primary_pkts"] = int(m.group(1))
            stats["redundant_pkts"] = int(m.group(2))
            stats["dedup_drops"] = int(m.group(3))
            break  # first match is sufficient

    return stats


def _assert_rx_fps(output: list[str], expected_fps: float, tolerance: float = 0.10):
    """Verify that the measured RX FPS is within *tolerance* of *expected_fps*.

    Parses ``app_rx_st40p_result(0), OK, fps <float>`` lines from stdout.
    """
    fps_re = re.compile(r"app_rx_st40p_result\(\d+\).*fps\s+([\d.]+)")
    measured = []
    for line in output:
        m = fps_re.search(line)
        if m:
            measured.append(float(m.group(1)))

    assert measured, "No app_rx_st40p_result lines found in output"

    for fps in measured:
        lo = expected_fps * (1.0 - tolerance)
        hi = expected_fps * (1.0 + tolerance)
        assert lo <= fps <= hi, (
            f"Measured FPS {fps:.2f} outside [{lo:.2f}, {hi:.2f}] "
            f"(expected ~{expected_fps:.2f})"
        )


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


_FPS_LOOKUP: dict[str, float] = {
    "p24": 24.0,
    "p25": 25.0,
    "p29": 29.97,
    "p30": 30.0,
    "p50": 50.0,
    "p59": 59.94,
    "p60": 60.0,
}


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
def test_st40p_redundant_progressive(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Basic redundant ST40P progressive – no gap injection.

    Validates that RxTxApp can transmit and receive st40p ancillary data
    over two multicast paths (ST 2022-7) and the RX side reports OK.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]
    expected_fps = _FPS_LOOKUP.get(fps_tag, 59.94)

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        ancillary_fps=fps_tag,
        ancillary_url=media_file_path,
    )

    logger.info(
        "Running redundant ST40P progressive test – fps=%s, file=%s",
        fps_tag,
        media_file_path,
    )

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, "Redundant ST40P progressive test reported FAILED"


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
def test_st40p_redundant_progressive_gap(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Redundant ST40P with deliberate RTP sequence-number gaps.

    Mirrors the GStreamer ``test_st40p_redundant_progressive_gap`` test.
    Sets ``test_mode="seq-gap"`` on the TX st40p session so the library
    injects RTP sequence discontinuities (via ``ST40_TX_TEST_SEQ_GAP``).
    The RX-side ST 2022-7 dedup must recover from the missing packets on
    the gapped path by falling back to the redundant stream.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]
    expected_fps = _FPS_LOOKUP.get(fps_tag, 59.94)

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        ancillary_fps=fps_tag,
        ancillary_url=media_file_path,
        tx_test_mode="seq-gap",
        tx_test_pkt_count=200,
        tx_test_frame_count=65535,
    )

    logger.info(
        "Running redundant ST40P gap test – fps=%s, file=%s, test_mode=seq-gap",
        fps_tag,
        media_file_path,
    )

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, "Redundant ST40P gap test reported FAILED"


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p29"],
        anc_files["text_p50"],
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p29",
        "text_p50",
        "text_p59",
    ],
)
def test_st40p_redundant_progressive_gap_latency(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Redundant ST40P with seq-gap injection **and** 7 ms inter-path delay.

    Combines ``ST40_TX_TEST_SEQ_GAP`` (RTP gap injection) with a 7 ms
    busy-wait delay before sending on port R (``redundant_delay_ns``).
    This stresses the RX-side ST 2022-7 reorder/dejitter buffer: the
    redundant stream arrives ~7 ms after the primary, so the dedup must
    wait for the late packets within the configurable reorder window
    before committing frames.

    The RX reorder window is bumped to 15 ms (``reorder_window_ns``) to
    comfortably accommodate the 7 ms path asymmetry with margin for
    scheduling jitter.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]
    expected_fps = _FPS_LOOKUP.get(fps_tag, 59.94)

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        ancillary_fps=fps_tag,
        ancillary_url=media_file_path,
        tx_test_mode="seq-gap",
        tx_test_pkt_count=200,
        tx_test_frame_count=65535,
        redundant_delay_ns=7_000_000,   # 7 ms
        reorder_window_ns=15_000_000,   # 15 ms
    )

    logger.info(
        "Running redundant ST40P gap+latency test – fps=%s, file=%s, "
        "delay=7ms, reorder_window=15ms",
        fps_tag,
        media_file_path,
    )

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, "Redundant ST40P gap+latency test reported FAILED"


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p59",
    ],
)
def test_st40p_redundant_progressive_synthetic(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Redundant ST40P progressive with synthetic TX data (no file).

    Exercises the library's built-in synthetic TX path (incrementing
    pattern).  Dedup is exercised because packets traverse two distinct
    multicast groups to two RX VFs and the receiver must reconcile
    duplicates.  For deliberate RTP gap injection see
    ``test_st40p_redundant_progressive_gap``.
    """
    media_file_info, _ = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]
    expected_fps = _FPS_LOOKUP.get(fps_tag, 59.94)

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        ancillary_fps=fps_tag,
        # Empty URL triggers synthetic TX data inside RxTxApp
        ancillary_url="",
    )

    logger.info(
        "Running redundant ST40P synthetic test – fps=%s, 4 VFs, multicast",
        fps_tag,
    )

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, "Redundant ST40P synthetic test reported FAILED"


@pytest.mark.nightly
@pytest.mark.parametrize("interlaced", [False, True], ids=["progressive", "interlaced"])
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p59",
    ],
)
def test_st40p_redundant_scan_mode(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
    interlaced,
):
    """Redundant ST40P with interlaced / progressive parametrisation."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        ancillary_fps=fps_tag,
        ancillary_url=media_file_path,
        interlaced=interlaced,
    )

    label = "interlaced" if interlaced else "progressive"
    logger.info("Running redundant ST40P %s test – fps=%s", label, fps_tag)

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, f"Redundant ST40P {label} test reported FAILED"


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        anc_files["text_p59"],
    ],
    indirect=["media_file"],
    ids=[
        "text_p59",
    ],
)
def test_st40p_redundant_unicast(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """Redundant ST40P over unicast paths (non-multicast control case)."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=4
    )
    if len(interfaces_list) < 4:
        pytest.skip("Redundant ST40P requires 4 interfaces (2 TX + 2 RX)")
    if len(ip_pools.tx) < 2 or len(ip_pools.rx) < 2:
        pytest.skip("IP pools not initialised with redundant addresses")

    fps_tag = media_file_info["fps"]

    config = rxtxapp.create_empty_redundant_config()
    config = rxtxapp.add_st40p_redundant_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        ancillary_fps=fps_tag,
        ancillary_url=media_file_path,
    )

    logger.info("Running redundant ST40P unicast test – fps=%s", fps_tag)

    passed = rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
    assert passed, "Redundant ST40P unicast test reported FAILED"
