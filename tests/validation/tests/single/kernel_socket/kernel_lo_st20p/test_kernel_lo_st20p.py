# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize("file", ["Penguin_1080p"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st20p_video_format(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    file,
    replicas,
    test_config,
    prepare_ramdisk,
):
    st20p_file = yuv_files_422p10le[file]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=f"p{st20p_file['fps']}",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    host = list(hosts.values())[0]
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * replicas,
        host=host,
    )
