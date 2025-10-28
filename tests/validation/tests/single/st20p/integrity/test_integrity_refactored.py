# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os

import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_yuv_frame_size, check_st20p_integrity
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Penguin_720p"],
        yuv_files_422rfc10["Penguin_1080p"],
        pytest.param(yuv_files_422p10le["Penguin_720p"], marks=pytest.mark.nightly),
        yuv_files_422p10le["Penguin_1080p"],
    ],
    indirect=["media_file"],
    ids=[
        "Penguin_720p_422rfc10",
        "Penguin_1080p_422rfc10",
        "Penguin_720p_422p10le",
        "Penguin_1080p_422p10le",
    ],
)
def test_integrity_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    prepare_ramdisk,
    media_file,
):
    """Test video integrity by comparing input and output files"""
    media_file_info, media_file_path = media_file

    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.yuv")
    host = list(hosts.values())[0]

    app = RxTxApp(f"{build}/tests/tools/RxTxApp/build")

    app.create_command(
        session_type="st20p",
        nic_port=host.vfs[0] if host.vfs else "0000:31:01.0",
        nic_port_list=host.vfs,
        source_ip="192.168.17.101",
        destination_ip="192.168.17.102",
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p25",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=out_file_url,
        test_mode="unicast",
        pacing="linear",
        test_time=test_time,
    )

    actual_test_time = max(test_time, 8)
    app.execute_test(build=build, test_time=actual_test_time, host=host)

    frame_size = calculate_yuv_frame_size(
        media_file_info["width"],
        media_file_info["height"],
        media_file_info["file_format"],
    )
    result = check_st20p_integrity(
        src_url=media_file_path, out_url=out_file_url, frame_size=frame_size
    )

    if result:
        logger.log(TEST_PASS, "INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
        raise AssertionError(
            "st20p integrity test failed content integrity comparison."
        )
