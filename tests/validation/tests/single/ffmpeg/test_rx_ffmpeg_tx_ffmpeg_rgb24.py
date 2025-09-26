# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, test_time_mutlipler",
    [
        ("i1080p25", 2),
        ("i1080p30", 2),
        ("i1080p60", 4),
        ("i2160p30", 4),
        ("i2160p60", 6),
    ],
)
def test_rx_ffmpeg_tx_ffmpeg_rgb24(
    hosts,
    test_time,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_mutlipler,
    test_config,
    prepare_ramdisk,
):
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    video_file = yuv_files[video_format]

    ffmpeg_app.execute_test_rgb24(
        test_time=test_time * test_time_mutlipler,
        build=build,
        host=host,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
        nic_port_list=interfaces_list,
    )
