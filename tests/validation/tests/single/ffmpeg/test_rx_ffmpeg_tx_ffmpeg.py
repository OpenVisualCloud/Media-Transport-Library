# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
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
def test_rx_ffmpeg_tx_ffmpeg(
    hosts,
    test_time,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    video_format,
    test_time_multipler,
    output_format,
    test_config,
    media_file,
):
    """Test FFmpeg-to-FFmpeg ST2110-20 video streaming.

    Validates bidirectional FFmpeg MTL plugin operation with both TX and RX
    using the ST2110-20 uncompressed video standard.

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
    :param test_time_multipler: Multiplier for test duration
    :type test_time_multipler: int
    :param output_format: Output file format ('yuv' or 'h264')
    :type output_format: str
    :param test_config: Test configuration dictionary
    :type test_config: dict
    :param media_file: Tuple of (media_file_info, media_file_path)
    :type media_file: tuple

    :raises AssertionError: If video transmission or reception fails

    .. note::
        - Both TX and RX use FFmpeg MTL plugin
        - Tests ST2110-20 uncompressed video transport
        - Validates received video quality
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.execute_test(
        test_time=test_time * test_time_multipler,
        build=mtl_path,
        host=host,
        nic_port_list=interfaces_list,
        type_="frame",
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        output_format=output_format,
    )
