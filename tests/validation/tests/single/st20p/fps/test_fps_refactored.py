# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        "p25",
        pytest.param("p29", marks=pytest.mark.smoke),
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
)
@pytest.mark.refactored
def test_fps_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fps,
    pcap_capture,
    media_file,
    application,
):
    """Test different frame rates.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param fps: Parametrized frame rate (e.g. ``p25``, ``p50``, ``p59``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "test_mode": "multicast",
        "destination_ip": ip_pools.rx_multicast[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": fps,
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_time": test_time,
    }

    if fps in ["p30", "p50", "p59", "p60"]:
        config_params.update({"pacing": "gap", "tx_no_chain": True})
    elif fps in ["p100", "p119", "p120"]:
        config_params.update({"pacing": "linear", "tx_no_chain": True})

    application.create_command(**config_params)

    actual_test_time = test_time
    if fps in ["p30", "p50", "p59", "p60"]:
        actual_test_time = max(test_time, 15)
    elif fps in ["p100", "p119", "p120"]:
        actual_test_time = max(test_time, 10)

    application.execute_test(
        build=mtl_path, test_time=actual_test_time, host=host, netsniff=pcap_capture
    )
