# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files, yuv_files_422rfc10


@pytest.mark.parametrize("standard_mode", ["st20p", "st22p"])
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 2])
def test_xdp_standard(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    video_format,
    replicas,
    standard_mode,
    prepare_ramdisk,
):
    video_file = yuv_files[video_format]

    st22p_file = yuv_files_422rfc10["Crosswalk_1080p"]
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    if standard_mode == "st20p":
        config = rxtxapp.add_st20p_sessions(
            config=config,
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            type_="frame",
            video_format=video_format,
            pg_format=video_file["format"],
            video_url=os.path.join(media, video_file["filename"]),
        )
        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas
        )
    elif standard_mode == "st22p":
        config = rxtxapp.add_st22p_sessions(
            config=config,
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode=test_mode,
            width=st22p_file["width"],
            height=st22p_file["height"],
            fps=f"p{st22p_file['fps']}",
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
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
