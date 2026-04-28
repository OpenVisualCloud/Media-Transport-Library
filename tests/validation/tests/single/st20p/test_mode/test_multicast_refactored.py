# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Crosswalk_720p"],
        yuv_files_422rfc10["ParkJoy_1080p"],
        yuv_files_422rfc10["Pedestrian_4K"],
    ],
    indirect=["media_file"],
    ids=["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"],
)
@pytest.mark.refactored
def test_multicast_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    media_file,
    application,
):
    """Test multicast transmission mode.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param test_time: Duration to run the streaming pipeline, in seconds.
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
        "destination_ip": ip_pools.rx_multicast[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "multicast",
        "test_time": test_time,
    }

    height = media_file_info.get("height", 0)
    if height >= 2160:
        config_params.update(
            {"pacing": "linear", "packing": "GPM_SL", "tx_no_chain": True}
        )
        actual_test_time = max(test_time, 15)
    elif height >= 1080:
        config_params.update({"pacing": "wide", "packing": "GPM", "tx_no_chain": False})
        actual_test_time = max(test_time, 10)
    else:
        config_params.update(
            {"pacing": "narrow", "packing": "GPM", "tx_no_chain": False}
        )
        actual_test_time = max(test_time, 8)

    application.create_command(**config_params)
    application.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        netsniff=pcap_capture,
    )
