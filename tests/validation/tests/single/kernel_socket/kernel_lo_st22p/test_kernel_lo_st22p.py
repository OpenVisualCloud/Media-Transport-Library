# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files_422rfc10


@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize("file", ["Penguin_1080p"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st22p_video_format(
    build, media, test_time, test_mode, file, replicas
):
    st22p_file = yuv_files_422rfc10[file]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=st22p_file["width"],
        height=st22p_file["height"],
        fps="p30",
        codec="JPEG-XS",
        quality="speed",
        pack_type="codestream",
        input_format=st22p_file["file_format"],
        output_format=st22p_file["file_format"],
        codec_thread_count=2,
        st22p_url=os.path.join(media, st22p_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st22p", replicas=replicas
    )
    rxtxapp.execute_test(config=config, build=build, test_time=test_time * replicas)
