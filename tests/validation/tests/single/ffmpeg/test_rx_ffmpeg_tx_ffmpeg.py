# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from tests.Engine import ffmpeg_app
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, test_time_multipler,",
    [
        ("i1080p25", 2),
        ("i1080p50", 2),
        ("i1080p60", 4),
        ("i2160p60", 6),
    ],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
def test_rx_ffmpeg_tx_ffmpeg(
    test_time,
    build,
    media,
    nic_port_list,
    video_format,
    test_time_multipler,
    output_format,
):
    video_file = yuv_files[video_format]

    ffmpeg_app.execute_test(
        test_time=test_time * test_time_multipler,
        build=build,
        nic_port_list=nic_port_list,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
        output_format=output_format,
    )
