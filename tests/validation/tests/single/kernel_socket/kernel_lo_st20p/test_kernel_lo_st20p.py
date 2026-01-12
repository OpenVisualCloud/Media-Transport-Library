# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Kernel loopback tests for ST2110-20 pipeline mode.

Tests ST20P (pipeline mode) video streams over kernel socket loopback interface.

Test Purpose
------------
Validate ST2110-20 pipeline mode with kernel socket backend using loopback
interface for local testing without physical NICs.

Methodology
-----------
Tests use kernel:lo for both TX and RX with 4:2:2 10-bit video formats.
Multiple replica configurations validate concurrent session handling.

Topology Requirements
---------------------
* Single host with loopback interface
* No physical network interfaces required
* Ramdisk for media files (optional but recommended)
"""
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import yuv_files_422p10le


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize("file", ["Penguin_1080p"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_kernello_st20p_video_format(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    file,
    replicas,
    prepare_ramdisk,
):
    """Test ST2110-20 pipeline video over kernel loopback.

    Validates ST20P (pipeline mode) video transmission and reception using
    kernel socket backend over loopback interface.

    :param hosts: Host objects from topology configuration
    :param build: Path to MTL build directory
    :param media: Path to media files directory
    :param test_time: Test duration in seconds
    :param test_mode: Transport mode (multicast)
    :param file: Video file identifier (Penguin_1080p)
    :param replicas: Number of session replicas (1 or 4)
    :param prepare_ramdisk: Ramdisk setup fixture
    """
    st20p_file = yuv_files_422p10le[file]
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=[
            "kernel:lo",
            "kernel:lo",
        ],  # Note: keeping hardcoded for kernel loopback test
        test_mode=test_mode,
        width=st20p_file["width"],
        height=st20p_file["height"],
        fps=f"p{st20p_file['fps']}",
        input_format=st20p_file["file_format"],
        transport_format=st20p_file["format"],
        output_format=st20p_file["file_format"],
        st20p_url=os.path.join(media, st20p_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
