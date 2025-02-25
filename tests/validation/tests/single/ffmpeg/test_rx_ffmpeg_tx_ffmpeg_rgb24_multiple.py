# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from tests.Engine import ffmpeg_app
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler",
    [
        ("i1080p25", "i1080p25", 4),
        ("i1080p30", "i1080p30", 4),
        ("i1080p60", "i1080p60", 8),
        ("i1080p60", "i1080p50", 8),
        ("i1080p50", "i1080p30", 6),
        ("i1080p25", "i1080p50", 6),
        ("i1080p25", "i1080p60", 6),
    ],
)
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple(
    test_time,
    build,
    media,
    nic_port_list,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
):
    video_file_1 = yuv_files[video_format_1]
    video_file_2 = yuv_files[video_format_2]

    ffmpeg_app.execute_test_rgb24_multiple(
        test_time=test_time * test_time_mutlipler,
        build=build,
        nic_port_list=nic_port_list,
        type_="frame",
        video_format_list=[video_format_1, video_format_2],
        pg_format=video_file_1["format"],
        video_url_list=[
            os.path.join(media, video_file_1["filename"]),
            os.path.join(media, video_file_2["filename"]),
        ],
    )
