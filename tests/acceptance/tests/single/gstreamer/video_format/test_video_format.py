# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""GStreamer ST20P video format validation.

Generates synthetic clips and exercises ST20P TX/RX pipelines across the
catalog of supported GStreamer video formats to ensure negotiation, pacing,
and capture work for each advertised format.
"""

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp
from mtl_engine.media_files import gstreamer_formats


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(gstreamer_formats.values()),
    indirect=["media_file"],
    ids=list(gstreamer_formats.keys()),
)
@pytest.mark.parametrize("file", gstreamer_formats.keys())
def test_video_format(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    file,
    test_time,
    test_config,
    media_file,
    prepare_ramdisk,
):
    video_file, media_file_path = media_file

    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    media_dir = host.connection.path(media_file_path).parent
    media_dir = str(media_dir)
    output_file_path = host.connection.path(media_dir, "output_video.yuv")
    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        media_path=media_dir,
        duration=3,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=mtl_path,
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
        build=mtl_path,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=GstreamerApp.video_format_change(video_file["format"]),
        rx_payload_type=112,
        rx_queues=4,
    )

    try:
        GstreamerApp.execute_test(
            build=mtl_path,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=4,
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
