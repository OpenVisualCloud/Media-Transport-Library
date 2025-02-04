# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.GstreamerApp as gstreamerapp
import tests.Engine.media_creator as media_create
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("file", yuv_files.keys())
def test_video_resolutions(
    build,
    media,
    nic_port_list,
    file,
):
    video_file = yuv_files[file]
    video_file["format"] = "v210"

    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=gstreamerapp.video_format_change(video_file["format"]),
        media_path=media,
    )

    tx_config = gstreamerapp.setup_gstreamer_st20p_tx_pipeline(
        build=build,
        nic_port_list=nic_port_list[0],
        input_path=input_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=gstreamerapp.video_format_change(video_file["format"]),
        tx_payload_type=112,
        tx_queues=4,
    )

    rx_config = gstreamerapp.setup_gstreamer_st20p_rx_pipeline(
        build=build,
        nic_port_list=nic_port_list[1],
        output_path=os.path.join(media, "output_video.yuv"),
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=gstreamerapp.video_format_change(video_file["format"]),
        rx_payload_type=112,
        rx_queues=4,
    )

    try:
        gstreamerapp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_video.yuv"),
            type="st20",
            tx_first=False,
            sleep_interval=1,
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path)
        media_create.remove_file(os.path.join(media, "output_video.yuv"))
