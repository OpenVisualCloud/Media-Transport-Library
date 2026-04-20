# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


def test_rx_timing_video_replicas_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    rxtxapp,
):
    video_file = yuv_files["i1080p60"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=video_file["width"],
        height=video_file["height"],
        framerate=f"p{video_file['fps']}",
        pixel_format=video_file["file_format"],
        transport_format=video_file["format"],
        input_file=os.path.join(media, video_file["filename"]),
        replicas=2,
        rx_timing_parser=True,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
