# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("packing", ["GPM_SL", "GPM"])
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
def test_packing_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    packing,
    prepare_ramdisk,
    media_file,
    pcap_capture,
    rxtxapp,
):
    """Test different packing modes (GPM_SL, GPM)"""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

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
        "packing": packing,
        "test_time": test_time,
    }

    height = media_file_info.get("height", 0)
    if height >= 2160:
        if packing == "GPM_SL":
            config_params.update({"tx_no_chain": True, "pacing": "linear"})
        else:
            config_params.update({"tx_no_chain": False, "pacing": "wide"})
        actual_test_time = max(test_time, 12)
    else:
        config_params["pacing"] = "linear" if packing == "GPM_SL" else "narrow"
        actual_test_time = max(test_time, 8)

    rxtxapp.create_command(**config_params)
    rxtxapp.execute_test(
        build=mtl_path, test_time=actual_test_time, host=host, netsniff=pcap_capture
    )
