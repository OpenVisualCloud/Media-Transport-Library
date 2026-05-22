# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation

"""Multi-stream RGB24 streaming tests (FFmpeg TX×2 + RxTxApp RX).

Requires 4 VFs (2VFxPF configuration): 2 for TX, 2 for RX.
"""

import pytest
from mtl_engine.media_files import yuv_files


RGB24_MULTI_CASES = [
    ("i1080p25", "i1080p25", 4, yuv_files["i1080p25"], yuv_files["i1080p25"]),
    ("i1080p30", "i1080p30", 4, yuv_files["i1080p30"], yuv_files["i1080p30"]),
    ("i1080p60", "i1080p60", 8, yuv_files["i1080p60"], yuv_files["i1080p60"]),
    ("i1080p60", "i1080p50", 8, yuv_files["i1080p60"], yuv_files["i1080p50"]),
    ("i1080p50", "i1080p30", 6, yuv_files["i1080p50"], yuv_files["i1080p30"]),
    ("i1080p25", "i1080p50", 6, yuv_files["i1080p25"], yuv_files["i1080p50"]),
    ("i1080p25", "i1080p60", 6, yuv_files["i1080p25"], yuv_files["i1080p60"]),
]


@pytest.mark.parametrize("application", [
    "ffmpeg",
    pytest.param("rxtxapp", marks=pytest.mark.skip(
        reason="Multi-stream RGB24 requires FFmpeg TX plugin"
    )),
])
@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_multiplier, media_1, media_2",
    RGB24_MULTI_CASES,
    ids=[
        "i1080p25_i1080p25",
        "i1080p30_i1080p30",
        "i1080p60_i1080p60",
        "i1080p60_i1080p50",
        "i1080p50_i1080p30",
        "i1080p25_i1080p50",
        "i1080p25_i1080p60",
    ],
)
def test_rgb24_multiple(
    application,
    app_factory,
    hosts,
    test_time,
    mtl_path,
    setup_interfaces,
    video_format_1,
    video_format_2,
    test_time_multiplier,
    media_1,
    media_2,
    test_config,
):
    """Test multiple simultaneous FFmpeg RGB24 video streams over ST2110-20.

    Two independent FFmpeg TX processes convert YUV422p10be to RGB24 and transmit
    over ST2110-20. A single RxTxApp RX process validates dual-stream reception.
    Requires 4 VFs (2VFxPF configuration).
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single("2VFxPF", count=4)

    media_path = test_config.get("media_path", "/mnt/ramdisk/media")
    video_url_1 = str(host.connection.path(media_path) / media_1["filename"])
    video_url_2 = str(host.connection.path(media_path) / media_2["filename"])

    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        video_format=video_format_1,
        pg_format=media_1["format"],
        video_url=video_url_1,
        mode="rgb24_multiple",
        video_format_list=[video_format_1, video_format_2],
        video_url_list=[video_url_1, video_url_2],
        test_time=test_time * test_time_multiplier,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_multiplier,
        host=host,
    )
    assert result, f"RGB24 multiple test failed for {video_format_1} + {video_format_2}"
