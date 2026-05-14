# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored multi-stream FFmpeg RGB24 ST2110-20 test.

Two FFmpeg TX processes (rgb24) + a single RxTxApp RX process receiving
both streams. Requires 4 VFs (2 RX + 2 TX) created as 2 VFs per PF on 2
PFs (``2VFxPF``). Mirrors the legacy ``test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple``.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler, media_file",
    [
        ("i1080p25", "i1080p25", 4, [yuv_files["i1080p25"], yuv_files["i1080p25"]]),
        ("i1080p30", "i1080p30", 4, [yuv_files["i1080p30"], yuv_files["i1080p30"]]),
        (
            "i1080p60",
            "i1080p60",
            8,
            [yuv_files["i1080p60"], yuv_files["i1080p60"]],
        ),
        ("i1080p60", "i1080p50", 8, [yuv_files["i1080p60"], yuv_files["i1080p50"]]),
        ("i1080p50", "i1080p30", 6, [yuv_files["i1080p50"], yuv_files["i1080p30"]]),
        ("i1080p25", "i1080p50", 6, [yuv_files["i1080p25"], yuv_files["i1080p50"]]),
        ("i1080p25", "i1080p60", 6, [yuv_files["i1080p25"], yuv_files["i1080p60"]]),
    ],
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
@pytest.mark.refactored
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple_refactored(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
    test_config,
    media_file,
    ffmpeg_app,
):
    host = list(hosts.values())[0]
    # 4 VFs (2 RX + 2 TX) across 2 PFs.
    interfaces_list = setup_interfaces.get_interfaces_list_single("2VFxPF", count=4)

    media_path = test_config.get("media_path", "/mnt/ramdisk/media")
    video_url_list = [
        str(host.connection.path(media_path) / media_file[0]["filename"]),
        str(host.connection.path(media_path) / media_file[1]["filename"]),
    ]

    ffmpeg_app.create_command(
        nic_port_list=interfaces_list,
        video_format=video_format_1,
        pg_format=media_file[0]["format"],
        video_url=video_url_list[0],
        mode="rgb24_multiple",
        video_format_list=[video_format_1, video_format_2],
        video_url_list=video_url_list,
    )

    ffmpeg_app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_mutlipler,
        host=host,
    )
