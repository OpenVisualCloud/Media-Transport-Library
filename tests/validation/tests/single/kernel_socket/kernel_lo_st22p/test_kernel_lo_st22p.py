# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Kernel loopback tests for ST2110-22 JPEG XS compressed video.

Tests ST22P (JPEG XS pipeline mode) compressed video streams over kernel socket
loopback interface.

Test Purpose
------------
Validate ST2110-22 JPEG XS encoding/decoding with kernel socket backend using
loopback interface for local testing without physical NICs.

Methodology
-----------
Tests use kernel:lo for both TX and RX with JPEG XS compression. Validates
codec functionality and concurrent session handling with multiple replicas.

Topology Requirements
---------------------
* Single host with loopback interface
* No physical network interfaces required
* Ramdisk for media files (optional but recommended)
* JPEG XS codec support in MTL build
"""
import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files_422rfc10


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st22p_video_format(
    hosts,
    mtl_path,
    test_time,
    test_mode,
    replicas,
    media_file,
):
    """Test ST2110-22 JPEG XS compressed video over kernel loopback.

    Validates ST22P (JPEG XS pipeline mode) video compression and decompression
    using kernel socket backend over loopback interface.

    :param hosts: Host objects from topology configuration
    :param build: Path to MTL build directory
    :param test_time: Test duration in seconds
    :param test_mode: Backend mode (kernel)
    :param replicas: Number of session replicas (1 or 4)
    :param media_file: Media file fixture (video file info and path)
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st22p_sessions(
        config=config,
        nic_port_list=[
            "kernel:lo",
            "kernel:lo",
        ],  # Note: keeping hardcoded for kernel loopback test
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=parse_fps_to_pformat(media_file_info["fps"]),
        codec="JPEG-XS",
        quality="speed",
        input_format=media_file_info["file_format"],
        output_format=media_file_info["file_format"],
        codec_thread_count=2,
        st22p_url=media_file_path,
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st22p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
