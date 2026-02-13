# SPDX-License-Identifier: BSD-3-Clause

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_unicast_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="st22p",
        test_mode="unicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality="speed",
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        codec_threads=2,
        test_time=test_time,
    )
    rxtxapp.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
