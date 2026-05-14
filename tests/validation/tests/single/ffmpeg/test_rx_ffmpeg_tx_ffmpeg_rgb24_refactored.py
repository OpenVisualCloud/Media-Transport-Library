# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored FFmpeg RGB24 single-stream ST2110-20 test.

FFmpeg TX (yuv422p10be → rgb24) + RxTxApp RX. Mirrors the legacy
``test_rx_ffmpeg_tx_ffmpeg_rgb24`` but built on the unified
:class:`mtl_engine.ffmpeg.FFmpeg` adapter.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format, test_time_mutlipler, media_file",
    [
        ("i1080p25", 1, yuv_files["i1080p25"]),
        ("i1080p30", 1, yuv_files["i1080p30"]),
        ("i1080p60", 1, yuv_files["i1080p60"]),
        ("i2160p30", 1, yuv_files["i2160p30"]),
        ("i2160p60", 1, yuv_files["i2160p60"]),
    ],
    indirect=["media_file"],
    ids=["i1080p25", "i1080p30", "i1080p60", "i2160p30", "i2160p60"],
)
@pytest.mark.refactored
def test_rx_ffmpeg_tx_ffmpeg_rgb24_refactored(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_mutlipler,
    test_config,
    media_file,
    ffmpeg_app,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.create_command(
        nic_port_list=interfaces_list,
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        mode="rgb24",
    )

    ffmpeg_app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_mutlipler,
        host=host,
    )
