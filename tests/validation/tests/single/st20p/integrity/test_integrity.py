# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_yuv_frame_size, check_st20p_integrity
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

logger = logging.getLogger(__name__)


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
def test_integrity(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file

    # Ensure the output directory exists for the integrity test output file.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.yuv")
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # Collect packet capture configuration and assign test_name
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    # Set a unique pcap file name
    capture_cfg["test_name"] = f"test_integrity_{media_file_info['filename']}"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        height=media_file_info["height"],
        width=media_file_info["width"],
        fps="p25",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )

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
