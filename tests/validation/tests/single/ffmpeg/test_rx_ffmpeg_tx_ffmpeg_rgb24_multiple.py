# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "video_format_1, video_format_2, test_time_mutlipler, media_file",
    [
        ("i1080p25", "i1080p25", 4, [yuv_files["i1080p25"], yuv_files["i1080p25"]]),
        ("i1080p30", "i1080p30", 4, [yuv_files["i1080p30"], yuv_files["i1080p30"]]),
        ("i1080p60", "i1080p60", 8, [yuv_files["i1080p60"], yuv_files["i1080p60"]]),
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
def test_rx_ffmpeg_tx_ffmpeg_rgb24_multiple(
    hosts,
    test_time,
    build,
    setup_interfaces: InterfaceSetup,
    video_format_1,
    video_format_2,
    test_time_mutlipler,
    test_config,
    media_file,
):
    """Test multiple simultaneous FFmpeg RGB24 video streams over ST2110-20.

    Validates concurrent transmission of two RGB24 video streams using FFmpeg MTL
    plugin and reception with RxTxApp. Tests multi-stream ST2110-20 transport
    with YUV to RGB conversion.

    :param hosts: Dictionary of test hosts
    :type hosts: dict
    :param test_time: Base test duration in seconds
    :type test_time: int
    :param build: Build directory path
    :type build: str
    :param setup_interfaces: Network interface setup fixture (requires 4 VFs)
    :type setup_interfaces: InterfaceSetup
    :param video_format_1: First video stream format (e.g., 'i1080p60')
    :type video_format_1: str
    :param video_format_2: Second video stream format (e.g., 'i1080p50')
    :type video_format_2: str
    :param test_time_mutlipler: Multiplier for test duration
    :type test_time_mutlipler: int
    :param test_config: Test configuration dictionary
    :type test_config: dict
    :param media_file: List of two media file dicts with 'filename' and 'format'
    :type media_file: list

    :raises AssertionError: If any RGB24 video stream fails transmission or reception

    .. note::
        - Requires 4 VFs: 2 for RX, 2 for TX (2VFxPF configuration)
        - Two FFmpeg TX processes (independent streams)
        - Single RxTxApp RX process (dual stream reception)
        - Each stream: YUV422p10be → RGB24 conversion
        - Supports mixed format streaming (e.g., 1080p60 + 1080p50)

    .. seealso::
        :func:`test_rx_ffmpeg_tx_ffmpeg_rgb24` for single stream tests
    """
    host = list(hosts.values())[0]
    # This test requires 4 VFs (2 for RX, 2 for TX), created as 2 VFs per PF across 2 PFs
    interfaces_list = setup_interfaces.get_interfaces_list_single("2VFxPF", count=4)

    # Get media files from media_path
    media_path = test_config.get("media_path", "/mnt/ramdisk/media")

    ffmpeg_app.execute_test_rgb24_multiple(
        test_time=test_time * test_time_mutlipler,
        build=build,
        nic_port_list=interfaces_list,
        type_="frame",
        video_format_list=[video_format_1, video_format_2],
        pg_format=media_file[0]["format"],
        video_url_list=[
            str(host.connection.path(media_path) / media_file[0]["filename"]),
            str(host.connection.path(media_path) / media_file[1]["filename"]),
        ],
        host=host,
    )
