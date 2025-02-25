# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import yuv_files_interlace


@pytest.mark.parametrize("format", yuv_files_interlace.keys())
def test_interlace(build, media, nic_port_list, test_time, format):
    st22p_file = yuv_files_interlace[format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        width=st22p_file["width"],
        height=st22p_file["height"],
        fps="p25",
        codec="JPEG-XS",
        quality="speed",
        input_format=st22p_file["file_format"],
        output_format=st22p_file["file_format"],
        st22p_url=os.path.join(media, st22p_file["filename"]),
        codec_thread_count=2,
        interlaced=True,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
