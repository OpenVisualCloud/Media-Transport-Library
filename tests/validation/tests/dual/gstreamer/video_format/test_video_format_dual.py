# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp
from mtl_engine.media_files import gstreamer_formats


@pytest.mark.parametrize("file", gstreamer_formats.keys())
def test_video_format_dual(
    hosts,
    build,
    media,
    nic_port_list,
    file,
    test_time,
    test_config,
    prepare_ramdisk,
):
    """
    Test GStreamer ST20P video format in dual host configuration using single host setup functions.
    This test now reuses single host pipeline setup functions with dual host networking updates.
    """
    video_file = gstreamer_formats[file]

    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Create input file on TX host
    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        media_path=media,
        host=tx_host,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {})) if test_config else {}
    capture_cfg["test_name"] = f"test_video_format_dual_{file}"

    try:
        result = GstreamerApp.execute_dual_st20p_test(
            build=build,
            tx_nic_port=tx_host.vfs[0],
            rx_nic_port=rx_host.vfs[0],
            input_path=input_file_path,
            width=video_file["width"],
            height=video_file["height"],
            framerate=video_file["fps"],
            format=GstreamerApp.video_format_change(video_file["format"]),
            payload_type=112,
            queues=4,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            capture_cfg=capture_cfg,
        )

        assert result, f"GStreamer dual video format test failed for format {file}"

    finally:
        # Remove the input file on TX host
        media_create.remove_file(input_file_path, host=tx_host)
