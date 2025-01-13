import os

import pytest
import tests.Engine.GstreamerApp as gstreamerapp
import tests.Engine.media_creator as media_create
from tests.Engine.media_files import gstreamer_formats


@pytest.mark.parametrize("file", gstreamer_formats.keys())
def test_video_format(
    build,
    media,
    nic_port_list,
    file,
):
    video_file = gstreamer_formats[file]

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
        tx_fps=video_file["fps"],
    )

    rx_config = gstreamerapp.setup_gstreamer_st20p_rx_pipeline(
        build=build,
        nic_port_list=nic_port_list[1],
        output_path=os.path.join(media, "output.yuv"),
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=gstreamerapp.video_format_change(video_file["format"]),
        rx_payload_type=112,
        rx_queues=4,
        rx_fps=video_file["fps"],
    )

    try:
        gstreamerapp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            fps=video_file["fps"],
            input_file=input_file_path,
            output_file=os.path.join(media, "output.yuv"),
            type="st20",
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path)
        media_create.remove_file(os.path.join(media, "output.yuv"))
