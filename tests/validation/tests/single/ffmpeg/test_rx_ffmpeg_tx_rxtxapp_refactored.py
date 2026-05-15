# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored FFmpeg RX + RxTxApp TX ST2110-20 streaming test.

FFmpeg MTL plugin RX validates streams transmitted by RxTxApp. Mirrors the
legacy ``test_rx_ffmpeg_tx_rxtxapp`` but built on the unified
:class:`mtl_engine.ffmpeg.FFmpeg` adapter.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format, multiple_sessions, test_time_multipler, media_file",
    [
        ("i1080p25", False, 1, yuv_files["i1080p25"]),
        ("i1080p30", False, 1, yuv_files["i1080p30"]),
        ("i1080p60", False, 2, yuv_files["i1080p60"]),
        ("i2160p25", False, 2, yuv_files["i2160p25"]),
        ("i2160p30", False, 2, yuv_files["i2160p30"]),
        ("i2160p60", False, 2, yuv_files["i2160p60"]),
        ("i1080p25", True, 3, yuv_files["i1080p25"]),
        ("i1080p30", True, 3, yuv_files["i1080p30"]),
    ],
    indirect=["media_file"],
    ids=[
        "i1080p25",
        "i1080p30",
        "i1080p60",
        "i2160p25",
        "i2160p30",
        "i2160p60",
        "i1080p25_multi",
        "i1080p30_multi",
    ],
)
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.refactored
def test_rx_ffmpeg_tx_rxtxapp_refactored(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    multiple_sessions,
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
        tx_is_ffmpeg=False,
        multiple_sessions=multiple_sessions,
    )

    ffmpeg_app.execute_test(
        build=mtl_path,
        test_time=test_time * test_time_multipler,
        host=host,
    )
