# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Kernel loopback tests for mixed media streams.

Tests MTL's kernel socket backend using the loopback interface for local
transmit and receive without physical network interfaces.

Test Purpose
------------
Validate kernel socket functionality with mixed ST2110-20/30/40 streams over
the loopback interface, testing video, audio, and ancillary data simultaneously.

Methodology
-----------
Tests use kernel:lo interface for both TX and RX, enabling validation without
physical NICs. Each test varies replica counts and formats.

Topology Requirements
---------------------
* Single host with loopback interface
* No physical network interfaces required
* Ramdisk for media files (optional but recommended)
"""
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import (
    anc_files,
    audio_files,
    parse_fps_to_pformat,
    yuv_files,
)


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 3])
def test_kernello_mixed_format(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    video_format,
    replicas,
    test_config,
    prepare_ramdisk,
):
    """Test mixed media streams over kernel loopback interface.

    Validates simultaneous ST2110-20 (video), ST2110-30 (audio), and ST2110-40
    (ancillary) streams using kernel socket over loopback interface.

    :param hosts: Host objects from topology configuration
    :param build: Path to MTL build directory
    :param media: Path to media files directory
    :param test_time: Test duration in seconds
    :param test_mode: Backend mode (kernel)
    :param video_format: Video format specification (i1080p59)
    :param replicas: Number of session replicas (1 or 3)
    :param prepare_ramdisk: Ramdisk setup fixture
    """
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]

    # Get ramdisk mountpoint for output files (read-only media path can't be used for output)
    ramdisk_mountpoint = (
        test_config.get("ramdisk", {})
        .get("media", {})
        .get("mountpoint", "/mnt/ramdisk/media")
    )
    audio_out_path = str(
        host.connection.path(ramdisk_mountpoint) / ("out_" + audio_file["filename"])
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=[
            "kernel:lo",
            "kernel:lo",
        ],  # Note: keeping hardcoded for kernel loopback test
        test_mode=test_mode,
        width=video_file["width"],
        height=video_file["height"],
        fps=parse_fps_to_pformat(video_file["fps"]),
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        output_format=video_file["file_format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
        out_url=audio_out_path,
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st30p", replicas=replicas
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="ancillary", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * replicas * 3,
        host=host,
    )
