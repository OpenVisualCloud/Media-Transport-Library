# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_info
from mtl_engine.integrity import calculate_yuv_frame_size, check_st20p_integrity
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10


@pytest.mark.parametrize(
    "st20p_file, fps",
    [
        (yuv_files_422rfc10["Penguin_720p"], "p25"),
        (yuv_files_422rfc10["Penguin_1080p"], "p25"),
        (yuv_files_422p10le["Penguin_720p"], "p25"),
        (yuv_files_422p10le["Penguin_1080p"], "p25"),
    ],
)
def test_integrity(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    st20p_file,
    fps,
    test_config,
    prepare_ramdisk,
):
    st20p_file_url = os.path.join(media, st20p_file["filename"])

    # Ensure the output directory exists for the integrity test output file.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.yuv")
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_integrity_{os.path.splitext(os.path.basename(st20p_file['filename']))[0]}_{fps}"  # Set a unique pcap file name
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=host.vfs,
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

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )

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
