# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp
from mtl_engine.media_files import gstreamer_formats


@pytest.mark.parametrize("file", gstreamer_formats.keys())
def test_video_format(
    hosts,
    build,
    media,
    nic_port_list,
    file,
    test_time,
    test_config,
    prepare_ramdisk,
):
    video_file = gstreamer_formats[file]

    # Get the first host for remote execution
    host = list(hosts.values())[0]

    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        media_path=media,
        duration=3,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=build,
        nic_port_list=host.vfs[0],
        input_path=input_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        tx_payload_type=112,
        tx_queues=4,
    )

    rx_config = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=build,
        nic_port_list=host.vfs[0],
        output_path=os.path.join(media, "output_video.yuv"),
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        rx_payload_type=112,
        rx_queues=4,
    )

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_video.yuv"),
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=4,
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(os.path.join(media, "output_video.yuv"), host=host)
