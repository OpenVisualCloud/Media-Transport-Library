# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("video_format", ["i1080p59", "i2160p59"])
@pytest.mark.parametrize("replicas", [1, 2])
def test_pmd_kernel_video_format(
    build, media, test_time, test_mode, video_format, replicas
):

    video_file = yuv_files[video_format]

    # rxtxapp.check_and_bind_interface(["0000:38:00.0","0000:38:00.1"], "pmd")

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=["0000:4b:00.0", "kernel:eth2"],
        test_mode=test_mode,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    # rxtxapp.check_and_set_ip('eth2')
    config = rxtxapp.change_replicas(
        config=config, session_type="video", replicas=replicas
    )
    rxtxapp.execute_test(config=config, build=build, test_time=test_time * 2)
