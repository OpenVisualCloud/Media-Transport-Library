# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_interlace
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    indirect=["media_file"],
    ids=list(yuv_files_interlace.keys()),
)
def test_interlace_refactored(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    prepare_ramdisk,
    media_file,
    pcap_capture,
):
    """Test interlaced video transmission"""
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = RxTxApp(f"{build}/tests/tools/RxTxApp/build")

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "source_ip": "192.168.17.101",
        "destination_ip": "192.168.17.102",
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "test_mode": "unicast",
        "interlaced": True,
        "pacing": "linear",
        "tx_no_chain": False,
        "test_time": test_time,
    }

    app.create_command(**config_params)
    actual_test_time = max(test_time, 10)
    app.execute_test(build=build, test_time=actual_test_time, host=host, netsniff=pcap_capture)
