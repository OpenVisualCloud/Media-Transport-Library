# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.dual
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
def test_rx_ffmpeg_tx_rxtxapp_dual(
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
    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    video_file = yuv_files[video_format]

    ffmpeg_app.execute_dual_test(
        test_time=test_time * test_time_multipler,
        build=build,
        tx_host=tx_host,
        rx_host=rx_host,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
        output_format=output_format,
        multiple_sessions=multiple_sessions,
        tx_is_ffmpeg=False,  # This test uses RxTxApp for TX
    )
