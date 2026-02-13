# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ffmpeg_app
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
def test_rx_ffmpeg_tx_rxtxapp(
    hosts,
    test_time,
    build,
    setup_interfaces: InterfaceSetup,
    video_format,
    multiple_sessions,
    test_time_multipler,
    output_format,
    test_config,
    media_file,
):
    """Test FFmpeg RX with RxTxApp TX for ST2110-20 video streams.

    Validates FFmpeg MTL plugin receiving ST2110-20 video streams transmitted by
    RxTxApp. Tests single and multiple session configurations with various
    resolutions and output formats.

    :param hosts: Dictionary of test hosts
    :type hosts: dict
    :param test_time: Base test duration in seconds
    :type test_time: int
    :param build: Build directory path
    :type build: str
    :param setup_interfaces: Network interface setup fixture
    :type setup_interfaces: InterfaceSetup
    :param video_format: Video format identifier (e.g., 'i1080p60', 'i2160p30')
    :type video_format: str
    :param multiple_sessions: Whether to test multiple simultaneous sessions
    :type multiple_sessions: bool
    :param test_time_multipler: Multiplier for test duration
    :type test_time_multipler: int
    :param output_format: Output file format ('yuv' or 'h264')
    :type output_format: str
    :param test_config: Test configuration dictionary
    :type test_config: dict
    :param media_file: Tuple of (media_file_info, media_file_path)
    :type media_file: tuple

    :raises AssertionError: If video reception or validation fails

    .. note::
        - RxTxApp acts as TX (ST2110-20 transmitter)
        - FFmpeg MTL plugin acts as RX (receiver)
        - Validates received video against source file
        - Supports both YUV and H.264 output formats
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    ffmpeg_app.execute_test(
        test_time=test_time * test_time_multipler,
        build=build,
        host=host,
        nic_port_list=interfaces_list,
        type_="frame",
        video_format=video_format,
        pg_format=media_file_info["format"],
        video_url=media_file_path,
        output_format=output_format,
        multiple_sessions=multiple_sessions,
        tx_is_ffmpeg=False,
    )
