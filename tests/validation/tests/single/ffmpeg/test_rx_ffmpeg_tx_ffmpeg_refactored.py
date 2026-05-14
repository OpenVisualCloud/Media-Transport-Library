# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored FFmpeg ↔ FFmpeg ST2110-20 streaming tests.

Mirrors :mod:`tests.single.ffmpeg.test_rx_ffmpeg_tx_ffmpeg` but uses the
unified :class:`mtl_engine.ffmpeg.FFmpeg` ``Application`` adapter and the
shared ``ffmpeg_app`` fixture, matching the RxTxApp refactor pattern.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format, test_time_multipler, media_file",
    [
        ("i1080p25", 2, yuv_files["i1080p25"]),
        ("i1080p50", 2, yuv_files["i1080p50"]),
        pytest.param("i1080p60", 4, yuv_files["i1080p60"], marks=pytest.mark.smoke),
        ("i2160p60", 6, yuv_files["i2160p60"]),
    ],
    indirect=["media_file"],
    ids=["i1080p25", "i1080p50", "i1080p60", "i2160p60"],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.refactored
def test_rx_ffmpeg_tx_ffmpeg_refactored(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_multipler,
    output_format,
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
        output_format=output_format,
        mode="yuv_h264",
        tx_is_ffmpeg=True,
    )

    ffmpeg_app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_multipler,
        host=host,
    )
