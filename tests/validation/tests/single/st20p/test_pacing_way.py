# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Verify every RxTxApp TX pacing_way (auto, rl, tsc, tsc_narrow, ptp, be, tsn)
works with the TX side on both a VF and a PF interface. "PF" cases bind only
TX to the PF (pacing_way is TX-only) and keep RX on a VF, so a PF is always
left in kernel mode for netsniff-ng capture.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.const import E810_DEVICE_ID
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = pytest.mark.verified


def _is_e810(host) -> bool:
    """True if the host's first NIC is an Intel E810 Series Ethernet Adapter.

    E810 lacks the TxPP launch-time HW engine and never advertises
    RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP, so tsn pacing is rejected at
    mtl_init() time on E810 even on a PF (see doc/design.md#4.3.3).
    """
    device_id = str(host.network_interfaces[0].pci_device.device_id).lower()
    return device_id == E810_DEVICE_ID


# tsn launch-time pacing compares the packet timestamp against the NIC's
# hardware PHC, which is only exposed on a PF (see doc/design.md#4.3.3); a VF
# port is rejected at mtl_init() time (lib/src/dev/mt_dev.c
# dev_if_init_pacing()), so there is no positive VF-tsn case to run.
# "PF" here means the TX interface only (see module docstring); RX always
# uses a VF.
PACING_WAY_CASES = [
    pytest.param("VF", "auto", id="VF-auto"),
    pytest.param("PF", "auto", id="PF-auto"),
    pytest.param("VF", "rl", id="VF-rl"),
    pytest.param("PF", "rl", id="PF-rl"),
    pytest.param("VF", "tsc", id="VF-tsc"),
    pytest.param("PF", "tsc", id="PF-tsc"),
    pytest.param("VF", "tsc_narrow", id="VF-tsc_narrow"),
    pytest.param("PF", "tsc_narrow", id="PF-tsc_narrow"),
    pytest.param("VF", "ptp", id="VF-ptp"),
    pytest.param("PF", "ptp", id="PF-ptp"),
    pytest.param("VF", "be", id="VF-be"),
    pytest.param("PF", "be", id="PF-be"),
    pytest.param(
        "VF",
        "tsn",
        marks=pytest.mark.skip(
            reason="tsn launch-time pacing requires a PF; VF has no HW PHC"
        ),
        id="VF-tsn",
    ),
    pytest.param("PF", "tsn", id="PF-tsn"),
]


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support pacing_way selection"
            ),
        ),
    ],
)
@pytest.mark.parametrize("interface_type,pacing_way", PACING_WAY_CASES)
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
def test_st20p_pacing_way(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    interface_type,
    pacing_way,
    pcap_capture,
    media_file,
):
    """Test RxTxApp TX pacing_way on a VF or PF interface."""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    if interface_type == "PF":
        # pacing_way only affects the TX side, so only TX needs to bind to
        # the PF; RX stays on a VF, leaving the other PF free (kernel-mode)
        # for netsniff-ng to capture on.
        interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type="PF", rx_interface_type="VF"
        )
    else:
        interfaces_list = setup_interfaces.get_interfaces_list_single(interface_type)

    # ptp and tsn pace against the ptp-synced phc; give PTP convergence
    # (handled internally via enable_ptp) more headroom than the 30s default.
    needs_ptp = pacing_way in ("ptp", "tsn")
    actual_test_time = max(test_time, 60) if needs_ptp else test_time

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": ip_pools.tx[0],
        "destination_ip": ip_pools.rx[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "pacing_way": pacing_way,
        "enable_ptp": needs_ptp,
        "test_time": actual_test_time,
    }

    app = app_factory(application)
    app.create_command(**config_params)

    if pacing_way == "tsn" and _is_e810(host):
        # No TxPP launch-time HW on E810: expect mtl_init() to reject tsn
        # pacing and the app to fail to start, rather than pass.
        assert not app.execute_test(
            build=mtl_path,
            test_time=actual_test_time,
            host=host,
            netsniff=pcap_capture,
            fail_on_error=False,
        ), "tsn pacing unexpectedly succeeded on E810 (no TxPP launch-time HW)"
        return

    app.execute_test(
        build=mtl_path, test_time=actual_test_time, host=host, netsniff=pcap_capture
    )
