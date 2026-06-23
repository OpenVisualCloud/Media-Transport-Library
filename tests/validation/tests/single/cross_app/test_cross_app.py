# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation

"""Cross-framework streaming tests (TX app != RX app).

Validates ST2110-20 transport where TX and RX use different frameworks
(e.g., RxTxApp transmitting, FFmpeg receiving).
"""

import pytest
from mtl_engine.media_files import yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

CROSS_APP_MEDIA = [
    ("i1080p25", 1, yuv_files["i1080p25"]),
    ("i1080p30", 1, yuv_files["i1080p30"]),
    ("i1080p60", 2, yuv_files["i1080p60"]),
    ("i2160p25", 2, yuv_files["i2160p25"]),
    ("i2160p30", 2, yuv_files["i2160p30"]),
    ("i2160p60", 2, yuv_files["i2160p60"]),
]


@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(
                reason="Cross-app test uses RxTxApp TX + FFmpeg RX; only FFmpeg adapter supports this mode"
            ),
        ),
    ],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.parametrize(
    "video_format, test_time_multiplier, media_file",
    CROSS_APP_MEDIA,
    ids=[m[0] for m in CROSS_APP_MEDIA],
    indirect=["media_file"],
)
def test_cross_app(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    video_format,
    test_time_multiplier,
    output_format,
    test_config,
    media_file,
):
    """Test RxTxApp TX -> FFmpeg RX for ST2110-20 video streams.

    Validates FFmpeg MTL plugin receiving ST2110-20 video streams transmitted by
    RxTxApp. Tests various resolutions and output formats.
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
        tx_is_ffmpeg=False,
        mode="yuv_h264",
        test_time=test_time * test_time_multiplier,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_multiplier,
        host=host,
    )
    assert result, f"Cross-app test failed for {video_format} ({output_format})"


CROSS_APP_MULTI_MEDIA = [
    ("i1080p25", 3, yuv_files["i1080p25"]),
    ("i1080p30", 3, yuv_files["i1080p30"]),
]


@pytest.mark.parametrize(
    "application",
    [
        "ffmpeg",
        pytest.param(
            "rxtxapp",
            marks=pytest.mark.skip(
                reason="Cross-app multi-session uses FFmpeg adapter with tx_is_ffmpeg=False"
            ),
        ),
    ],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.parametrize(
    "video_format, test_time_multiplier, media_file",
    CROSS_APP_MULTI_MEDIA,
    ids=[f"{m[0]}_multi" for m in CROSS_APP_MULTI_MEDIA],
    indirect=["media_file"],
)
def test_cross_app_multisession(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    video_format,
    test_time_multiplier,
    output_format,
    test_config,
    media_file,
):
    """Test RxTxApp TX -> FFmpeg RX with multiple simultaneous sessions."""
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
        tx_is_ffmpeg=False,
        multiple_sessions=True,
        mode="yuv_h264",
        test_time=test_time * test_time_multiplier,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_multiplier,
        host=host,
    )
    assert (
        result
    ), f"Cross-app multi-session test failed for {video_format} ({output_format})"
