# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""GStreamer ST20P video resolution validation.

Sweeps ST20P pipelines across the catalog of YUV sample resolutions, including
format fallbacks for widths not divisible by six, to verify configuration,
pacing, and basic receive stability at varied sizes.
"""

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp
from mtl_engine.media_files import yuv_files
from tests.xfail import SDBQ1971_conversion_v210_720p_error


@pytest.mark.nightly
@pytest.mark.parametrize("application", ["gstreamer"])
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files.values()),
    indirect=["media_file"],
    ids=list(yuv_files.keys()),
)
def test_video_resolutions(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    request,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    video_file, media_file_path = media_file

    # The st20 plugin can only produce v210 when the width is divisible by 6
    # (pixel groups are 6 pixels wide). Fall back to I422_10LE otherwise so
    # 1280-wide sources succeed instead of tripping the converter.
    gst_format = (
        "v210"
        if video_file["width"] % 6 == 0
        else GstreamerApp.video_format_change(video_file["format"])
    )

    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if gst_format == "v210":
        SDBQ1971_conversion_v210_720p_error(
            video_format=gst_format,
            resolution_height=video_file["height"],
            request=request,
        )

    media_dir = host.connection.path(media_file_path).parent
    media_dir = str(media_dir)
    input_file_path = media_create.create_video_file(
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        format=gst_format,
        media_path=media_dir,
        duration=3,
        host=host,
    )
    # media_dir is an absolute path on the (local) target host, so os.path.join
    # is byte-identical to the prior host.connection.path(...) form here.
    output_file_path = os.path.join(media_dir, "output_video.yuv")

    app = app_factory(application)
    app.create_command(
        build=mtl_path,
        session_type="st20p",
        nic_port_list=interfaces_list,
        input_file=input_file_path,
        output_file=output_file_path,
        width=video_file["width"],
        height=video_file["height"],
        framerate=video_file["fps"],
        gst_format=gst_format,
    )
    try:
        app.execute_test(
            build=mtl_path,
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=2,
        )
    finally:
        # Remove the video file after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
