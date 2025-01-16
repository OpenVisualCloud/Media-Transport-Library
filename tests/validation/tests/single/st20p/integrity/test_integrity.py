# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_info
from tests.Engine.integrity import calculate_yuv_frame_size, check_st20p_integrity
from tests.Engine.logging import LOG_FOLDER
from tests.Engine.media_files import yuv_files_422p10le, yuv_files_422rfc10


@pytest.mark.parametrize(
    "st20p_file, fps",
    [
        (yuv_files_422rfc10["Penguin_720p"], "p25"),
        (yuv_files_422rfc10["Penguin_1080p"], "p25"),
        (yuv_files_422p10le["Penguin_720p"], "p25"),
        (yuv_files_422p10le["Penguin_1080p"], "p25"),
    ],
)
def test_integrity(build, media, nic_port_list, test_time, st20p_file, fps):
    st20p_file_url = os.path.join(media, st20p_file["filename"])

    out_file_url = os.path.join(os.getcwd(), LOG_FOLDER, "latest", "out.yuv")

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        height=st20p_file["height"],
        width=st20p_file["width"],
        fps=fps,
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=st20p_file_url,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)

    frame_size = calculate_yuv_frame_size(
        st20p_file["width"], st20p_file["height"], st20p_file["file_format"]
    )
    result = check_st20p_integrity(
        src_url=st20p_file_url, out_url=out_file_url, frame_size=frame_size
    )

    if result:
        log_info("INTEGRITY PASS")
    else:
        log_info("INTEGRITY FAIL")
