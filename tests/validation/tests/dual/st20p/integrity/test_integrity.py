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


@pytest.mark.dual
@pytest.mark.parametrize(
    "st20p_file, fps",
    [
        (yuv_files_422rfc10["Penguin_720p"], "p25"),
        (yuv_files_422rfc10["Penguin_1080p"], "p25"),
        (yuv_files_422p10le["Penguin_720p"], "p25"),
        (yuv_files_422p10le["Penguin_1080p"], "p25"),
    ],
)
def test_integrity_dual(
    hosts, mtl_path, media, nic_port_list, test_time, st20p_file, fps
):
    st20p_file_url = os.path.join(media, st20p_file["filename"])

    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Ensure the output directory exists for the integrity test output file.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.yuv")

    config = rxtxapp.create_empty_dual_config()
    config = rxtxapp.add_st20p_dual_sessions(
        config=config,
        tx_nic_port_list=tx_host.vfs,
        rx_nic_port_list=rx_host.vfs,
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

    rxtxapp.execute_dual_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
    )

    frame_size = calculate_yuv_frame_size(
        st20p_file["width"], st20p_file["height"], st20p_file["file_format"]
    )
    result = check_st20p_integrity(
        src_url=st20p_file_url, out_url=out_file_url, frame_size=frame_size
    )

    if result:
        logger.log(TEST_PASS, "INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
