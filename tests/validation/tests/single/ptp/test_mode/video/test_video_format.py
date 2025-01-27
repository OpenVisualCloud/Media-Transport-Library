# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize(
    "video_format",
    ["i1080p30", "i1080p50", "i1080p59", "i2160p30", "i2160p50", "i2160p59"],
)
@pytest.mark.parametrize("replicas", [1, 3, 7, 10, 14, 18])
def test_ptp_video_format(
    build, media, nic_port_list, test_time, test_mode, video_format, replicas
):
    if "i2160" in video_format and replicas > 3:
        pytest.skip("Skipping 4k tests with more than 3 replicas")
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode=test_mode,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="video", replicas=replicas
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, ptp=True)
