# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.dual
@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler",
    [
        ("i1080p25", "i1080p25", 2),
        ("i1080p30", "i1080p30", 2),
        ("i1080p60", "i1080p60", 4),
        ("i1080p60", "i1080p50", 4),
        ("i1080p50", "i1080p30", 3),
        ("i1080p25", "i1080p50", 3),
        ("i1080p25", "i1080p60", 3),
    ],
)
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple_dual(
    hosts,
    test_time,
    build,
    media,
    nic_port_list,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
    test_config,
    prepare_ramdisk,
):
    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    video_file_1 = yuv_files[video_format_1]
    video_file_2 = yuv_files[video_format_2]

    ffmpeg_app.execute_dual_test_rgb24_multiple(
        test_time=test_time * test_time_mutlipler,
        build=build,
        tx_host=tx_host,
        rx_host=rx_host,
        type_="frame",
        video_format_list=[video_format_1, video_format_2],
        pg_format=video_file_1["format"],
        video_url_list=[
            os.path.join(media, video_file_1["filename"]),
            os.path.join(media, video_file_2["filename"]),
        ],
    )
