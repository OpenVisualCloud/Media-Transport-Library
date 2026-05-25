# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = [pytest.mark.verified, pytest.mark.nightly]


FORMAT_CASES = [
    ("i1080p25", "p25", yuv_files_422rfc10["Penguin_1080p"]),
    ("i1080p30", "p30", yuv_files_422rfc10["Penguin_1080p"]),
    ("i1080p60", "p60", yuv_files_422rfc10["Crosswalk_1080p"]),
    ("i2160p30", "p30", yuv_files_422rfc10["Crosswalk_4K"]),
    ("i2160p60", "p60", yuv_files_422rfc10["Crosswalk_4K"]),
]


@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.parametrize(
    "video_format, fps, media_file",
    FORMAT_CASES,
    ids=[c[0] for c in FORMAT_CASES],
    indirect=["media_file"],
)
def test_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    video_format,
    fps,
    output_format,
):
    if output_format == "h264" and application == "rxtxapp":
        pytest.skip("RxTxApp does not support h264 output format")
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=fps,
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_format=output_format,
        test_time=test_time,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
    assert result, f"Format test failed for {video_format} ({output_format})"
