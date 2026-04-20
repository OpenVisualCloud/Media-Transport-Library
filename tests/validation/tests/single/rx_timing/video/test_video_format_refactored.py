# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format",
    [
        "i1080p25",
        "i1080p30",
        "i1080p50",
        "i1080p60",
        "i1080p100",
        "i1080p120",
        "i2160p60",
    ],
)
def test_rx_timing_video_video_format_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    video_format,
    test_config,
    prepare_ramdisk,
    rxtxapp,
):
    video_file = yuv_files[video_format]
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
        rx_timing_parser=True,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
