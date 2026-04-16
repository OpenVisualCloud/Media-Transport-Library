# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
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
def test_rx_ffmpeg_tx_ffmpeg_rgb24(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_mutlipler,
    test_config,
    media_file,
):
    """Test FFmpeg RGB24 format video streaming over ST2110-20.

    Validates FFmpeg MTL plugin transmitting RGB24 format video and RxTxApp
    receiving it. Tests YUV422p10be to RGB24 conversion and ST2110-20 transport.

    :param hosts: Dictionary of test hosts
    :type hosts: dict
    :param test_time: Base test duration in seconds
    :type test_time: int
    :param build: Build directory path
    :type build: str
    :param setup_interfaces: Network interface setup fixture
    :type setup_interfaces: InterfaceSetup
    :param video_format: Video format identifier (e.g., 'i1080p60', 'i2160p60')
    :type video_format: str
    :param test_time_mutlipler: Multiplier for test duration
    :type test_time_mutlipler: int
    :param test_config: Test configuration dictionary
    :type test_config: dict
    :param media_file: Tuple of (media_file_info, media_file_path)
    :type media_file: tuple

    :raises AssertionError: If RGB24 video transmission or reception fails

    .. note::
        - FFmpeg TX reads YUV422p10be and converts to RGB24
        - RxTxApp RX validates RGB24 ST2110-20 reception
        - Tests pixel format conversion pipeline
        - Supports resolutions from 1080p to 4K60
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.execute_test_rgb24(
        test_time=test_time * test_time_mutlipler,
        build=mtl_path,
        host=host,
        type_="frame",
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        nic_port_list=interfaces_list,
    )
