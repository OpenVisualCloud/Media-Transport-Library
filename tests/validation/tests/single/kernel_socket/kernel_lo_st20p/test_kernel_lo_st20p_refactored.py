# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Refactored kernel loopback ST20P test (new RxTxApp create_command/execute_test API).

Validates ST2110-20 pipeline mode video transmission and reception over the
kernel-socket loopback interface using the unified ``rxtxapp`` fixture.
"""
import pytest
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.refactored
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st20p_video_format_refactored(
    hosts,
    mtl_path,
    test_time,
    test_mode,
    replicas,
    media_file,
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    rxtxapp.create_command(
        session_type="st20p",
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        replicas=replicas,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
