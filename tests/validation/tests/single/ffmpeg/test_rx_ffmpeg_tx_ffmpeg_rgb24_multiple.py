# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler, media_file_1, media_file_2",
    [
        ("i1080p25", "i1080p25", 4, yuv_files["i1080p25"], yuv_files["i1080p25"]),
        ("i1080p30", "i1080p30", 4, yuv_files["i1080p30"], yuv_files["i1080p30"]),
        ("i1080p60", "i1080p60", 8, yuv_files["i1080p60"], yuv_files["i1080p60"]),
        ("i1080p60", "i1080p50", 8, yuv_files["i1080p60"], yuv_files["i1080p50"]),
        ("i1080p50", "i1080p30", 6, yuv_files["i1080p50"], yuv_files["i1080p30"]),
        ("i1080p25", "i1080p50", 6, yuv_files["i1080p25"], yuv_files["i1080p50"]),
        ("i1080p25", "i1080p60", 6, yuv_files["i1080p25"], yuv_files["i1080p60"]),
    ],
    indirect=["media_file_1", "media_file_2"],
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
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple(
    hosts,
    test_time,
    build,
    setup_interfaces: InterfaceSetup,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
    test_config,
    media_file_1,
    media_file_2,
):
    host = list(hosts.values())[0]
    # This test requires 4 VFs (2 for RX, 2 for TX), created as 2 VFs per PF across 2 PFs
    interfaces_list = setup_interfaces.get_interfaces_list_single("2VFxPF", count=4)

    media_file_1_info, media_file_1_path = media_file_1
    media_file_2_info, media_file_2_path = media_file_2

    ffmpeg_app.execute_test_rgb24_multiple(
        test_time=test_time * test_time_mutlipler,
        build=build,
        nic_port_list=interfaces_list,
        type_="frame",
        video_format_list=[video_format_1, video_format_2],
        pg_format=media_file_1_info["format"],
        video_url_list=[
            media_file_1_path,
            media_file_2_path,
        ],
        host=host,
    )
