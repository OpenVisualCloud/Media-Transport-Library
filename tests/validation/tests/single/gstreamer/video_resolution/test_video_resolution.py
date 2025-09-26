# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp
from mtl_engine.media_files import yuv_files
from tests.xfail import SDBQ1971_conversion_v210_720p_error


@pytest.mark.parametrize("file", yuv_files.keys())
def test_video_resolutions(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file,
    request,
    test_time,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files[file]
    video_file["format"] = "v210"

    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    SDBQ1971_conversion_v210_720p_error(
        video_format=video_file["format"],
        resolution_width=video_file["height"],
        request=request,
    )

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
        nic_port_list=interfaces_list[0],
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
        nic_port_list=interfaces_list[1],
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
            sleep_interval=2,
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(os.path.join(media, "output_video.yuv"), host=host)
