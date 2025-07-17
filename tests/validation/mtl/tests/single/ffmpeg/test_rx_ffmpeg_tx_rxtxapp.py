# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, multiple_sessions, test_time_multipler",
    [
        ("i1080p25", False, 1),
        ("i1080p30", False, 1),
        ("i1080p60", False, 2),
        ("i2160p25", False, 2),
        ("i2160p30", False, 2),
        ("i2160p60", False, 2),
        ("i1080p25", True, 3),
        ("i1080p30", True, 3),
    ],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
def test_rx_ffmpeg_tx_rxtxapp(
    hosts,
    test_time,
    build,
    media,
    nic_port_list,
    video_format,
    multiple_sessions,
    test_time_multipler,
    output_format,
    test_config,
    prepare_ramdisk,
):
    host = list(hosts.values())[0]
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = f"test_rx_ffmpeg_tx_rxtxapp_{video_format}_{output_format}_{multiple_sessions}_{test_time_multipler}"

    video_file = yuv_files[video_format]

    ffmpeg_app.execute_test(
        test_time=test_time * test_time_multipler,
        build=build,
        host=host,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
        output_format=output_format,
        multiple_sessions=multiple_sessions,
        tx_is_ffmpeg=False,
        capture_cfg=capture_cfg,
    )
