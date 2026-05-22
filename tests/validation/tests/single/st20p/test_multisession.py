# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine.media_files import yuv_files

pytestmark = pytest.mark.verified

MULTISESSION_MEDIA = [
    ("i1080p25", yuv_files["i1080p25"]),
    ("i1080p30", yuv_files["i1080p30"]),
]


@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize(
    "output_format",
    [
        "yuv",
        pytest.param(
            "h264", marks=pytest.mark.skip(reason="h264 multi-session not validated")
        ),
    ],
)
@pytest.mark.parametrize(
    "video_format, media_file",
    MULTISESSION_MEDIA,
    ids=[fmt[0] for fmt in MULTISESSION_MEDIA],
    indirect=["media_file"],
)
def test_multisession(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    video_format,
    output_format,
):
    """Test multiple simultaneous ST2110-20 sessions.

    Validates that the framework can handle two concurrent TX/RX sessions
    on the same interface pair.
    """
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
        output_format=output_format,
        multiple_sessions=True,
        test_time=test_time * 3,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time * 3,
        host=host,
    )
    assert result, f"Multi-session test failed for {video_format} ({application})"
