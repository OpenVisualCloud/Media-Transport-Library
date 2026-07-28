# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp
from mtl_engine.media_files import gstreamer_formats


@pytest.mark.dual
@pytest.mark.parametrize("file", gstreamer_formats.keys())
def test_video_format_dual(
    hosts,
    mtl_path,
    media,
    nic_port_list,
    file,
    test_time,
    test_config,
    prepare_ramdisk,
):
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
        duration=3,
        media_path=media,
        host=tx_host,
    )

    # Create output file path for RX host
    output_file_path = os.path.join(media, f"output_video_dual_{file}.yuv")

    # Setup TX pipeline using existing function
    tx_config = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=mtl_path,
        nic_port_list=tx_host.vfs[0],
        input_path=input_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        tx_payload_type=112,
        tx_queues=4,
    )

    # Setup RX pipeline using existing function
    rx_config = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=mtl_path,
        nic_port_list=rx_host.vfs[0],
        output_path=output_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        rx_payload_type=112,
        rx_queues=4,
    )

    try:
        # Use the unified execute_test function for dual host execution
        result = GstreamerApp.execute_test(
            build=mtl_path,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            tx_first=False,
        )

        assert result, f"GStreamer dual video format test failed for format {file}"

    finally:
        # Remove the input file on TX host and output file on RX host
        media_create.remove_file(input_file_path, host=tx_host)
        media_create.remove_file(output_file_path, host=rx_host)
