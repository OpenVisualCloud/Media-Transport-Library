# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, replicas",
    [
        ("i1080p60", 1),
        ("i1080p60", 3),
        ("i1080p60", 30),
        ("i2160p60", 1),
        ("i2160p60", 3),
        ("i2160p60", 9),
    ],
)
def test_virtio_user(build, media, nic_port_list, test_time, video_format, replicas):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="video", replicas=replicas
    )

    rxtxapp.execute_test(
        config=config, build=build, test_time=test_time, virtio_user=True
    )
