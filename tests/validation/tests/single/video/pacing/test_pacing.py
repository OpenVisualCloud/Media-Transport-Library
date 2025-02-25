# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("pacing", ["narrow", "wide", "linear"])
@pytest.mark.parametrize("video_format", ["i720p50", "i1080p30", "i2160p60"])
def test_pacing(
    build,
    media,
    nic_port_list,
    test_time,
    video_format,
    pacing,
):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, "test_8k.yuv"),
    )
    config = rxtxapp.change_pacing_video(content=config, pacing=pacing)

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
