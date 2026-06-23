# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

RGB24_MEDIA = [
    ("i1080p25", yuv_files["i1080p25"]),
    ("i1080p30", yuv_files["i1080p30"]),
    ("i1080p60", yuv_files["i1080p60"]),
    ("i2160p30", yuv_files["i2160p30"]),
    ("i2160p60", yuv_files["i2160p60"]),
]


@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(reason="RGB24 mode requires FFmpeg TX plugin"),
        ),
    ],
)
@pytest.mark.parametrize(
    "video_format, media_file",
    RGB24_MEDIA,
    ids=[fmt[0] for fmt in RGB24_MEDIA],
    indirect=["media_file"],
)
def test_rgb24(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    video_format,
    test_config,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        mode="rgb24",
        test_time=test_time,
    )
    passed = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        fail_on_error=False,
    )
    assert passed, f"RGB24 test failed for {video_format}"
