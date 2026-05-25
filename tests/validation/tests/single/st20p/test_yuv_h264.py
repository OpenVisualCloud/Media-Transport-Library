# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

YUV_H264_MEDIA = [
    ("Penguin_1080p", "H264_CBR", yuv_files_422p10le["Penguin_1080p"]),
    ("Penguin_1080p", "JPEG-XS", yuv_files_422p10le["Penguin_1080p"]),
]


@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize(
    "media_key, codec, media_file",
    YUV_H264_MEDIA,
    ids=[f"{k}_{c}" for k, c in [m[:2] for m in YUV_H264_MEDIA]],
    indirect=["media_file"],
)
def test_yuv_h264(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    media_key,
    codec,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    test_time = max(test_time, 90)
    app = app_factory(application)
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec=codec,
        quality="speed",
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        codec_threads=2,
        test_time=test_time,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
    assert result, f"YUV/H264 test failed for {media_key} with codec {codec}"
