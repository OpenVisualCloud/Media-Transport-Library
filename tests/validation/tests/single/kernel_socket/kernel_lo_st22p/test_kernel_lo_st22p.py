# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize("file", ["Penguin_1080p"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st22p_video_format(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    file,
    replicas,
    prepare_ramdisk,
):
    st22p_file = yuv_files_422rfc10[file]
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=[
            "kernel:lo",
            "kernel:lo",
        ],  # Note: keeping hardcoded for kernel loopback test
        test_mode=test_mode,
        width=st22p_file["width"],
        height=st22p_file["height"],
        fps=parse_fps_to_pformat(st22p_file["fps"]),
        codec="JPEG-XS",
        quality="speed",
        input_format=st22p_file["file_format"],
        output_format=st22p_file["file_format"],
        codec_thread_count=2,
        st22p_url=os.path.join(media, st22p_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st22p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * replicas,
        host=host,
    )
