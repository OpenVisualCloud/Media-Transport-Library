# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format, test_time_mutlipler, media_file",
    [
        ("i1080p25", 2, yuv_files["i1080p25"]),
        ("i1080p30", 2, yuv_files["i1080p30"]),
        ("i1080p60", 4, yuv_files["i1080p60"]),
        ("i2160p30", 4, yuv_files["i2160p30"]),
        ("i2160p60", 6, yuv_files["i2160p60"]),
    ],
    indirect=["media_file"],
    ids=["i1080p25", "i1080p30", "i1080p60", "i2160p30", "i2160p60"],
)
def test_rx_ffmpeg_tx_ffmpeg_rgb24(
    hosts,
    test_time,
    build,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_mutlipler,
    test_config,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.execute_test_rgb24(
        test_time=test_time * test_time_mutlipler,
        build=build,
        host=host,
        type_="frame",
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        nic_port_list=interfaces_list,
    )
