# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


def test_rx_timing_video_replicas(
    build,
    media,
    nic_port_list,
    test_time,
):
    video_file = yuv_files["i1080p60"]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_="frame",
        video_format="i1080p60",
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(config=config, session_type="video", replicas=2)

    rxtxapp.execute_test(
        config=config, build=build, test_time=test_time, rx_timing_parser=True
    )
