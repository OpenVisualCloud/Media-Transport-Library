# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("type_mode", ["rtp", "slice"])
@pytest.mark.parametrize(
    "video_format",
    ["i720p50", "i1080p30", "i2160p60", "i4320p30", "i480i59", "i576i50", "i1080i59"],
)
def test_type_mode(
    build,
    media,
    nic_port_list,
    test_time,
    video_format,
    type_mode,
):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_=type_mode,
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
