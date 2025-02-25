# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
@pytest.mark.parametrize("video_format", ["i1080p60", "i2160p60"])
def test_rss_mode_video(build, media, nic_port_list, test_time, video_format, rss_mode):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_rss_mode(content=config, rss_mode=rss_mode)

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
