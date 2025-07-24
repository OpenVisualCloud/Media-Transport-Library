# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files

@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler",
    [
        ("i1080p25", "i1080p25", 4),
        ("i1080p30", "i1080p30", 4),
        ("i1080p60", "i1080p60", 8),
        ("i1080p60", "i1080p50", 8),
        ("i1080p50", "i1080p30", 6),
        ("i1080p25", "i1080p50", 6),
        ("i1080p25", "i1080p60", 6),
    ],
)
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple(
    hosts,
    test_time,
    build,
    media,
    nic_port_list,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
    test_config,
    prepare_ramdisk,
):
    host = list(hosts.values())[0]
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple_{video_format_1}_{video_format_2}"
    )

    video_file_1 = yuv_files[video_format_1]
    video_file_2 = yuv_files[video_format_2]

    ffmpeg_app.execute_test_rgb24_multiple(
        test_time=test_time * test_time_mutlipler,
        build=build,
        nic_port_list=host.vfs,
        type_="frame",
        video_format_list=[video_format_1, video_format_2],
        pg_format=video_file_1["format"],
        video_url_list=[
            os.path.join(media, video_file_1["filename"]),
            os.path.join(media, video_file_2["filename"]),
        ],
        host=host,
        capture_cfg=capture_cfg,
    )
