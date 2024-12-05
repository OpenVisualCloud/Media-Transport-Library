# INTEL CONFIDENTIAL
# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.

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
