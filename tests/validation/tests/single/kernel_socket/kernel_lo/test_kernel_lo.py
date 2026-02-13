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
@pytest.mark.parametrize(
    "media_file",
    [yuv_files["i1080p59"]],
    indirect=["media_file"],
    ids=["i1080p59"],
)
@pytest.mark.parametrize("replicas", [1, 3])
def test_kernello_mixed_format(
    hosts,
    mtl_path,
    test_time,
    test_mode,
    replicas,
    media_file,
    media,
    setup_interfaces,
):
    """Test mixed media streams over kernel loopback interface.

    Validates simultaneous ST2110-20 (video), ST2110-30 (audio), and ST2110-40
    (ancillary) streams using kernel socket over loopback interface.

    :param hosts: Host objects from topology configuration
    :param build: Path to MTL build directory
    :param test_time: Test duration in seconds
    :param test_mode: Backend mode (kernel)
    :param replicas: Number of session replicas (1 or 3)
    :param media_file: Media file fixture (video file info and path)
    :param media: Source media directory path
    :param setup_interfaces: Interface setup helper for cleanup
    """
    media_file_info, media_file_path = media_file
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=[
            "kernel:lo",
            "kernel:lo",
        ],
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=parse_fps_to_pformat(media_file_info["fps"]),
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
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
        filename=str(host.connection.path(media) / audio_file["filename"]),
        out_url=str(
            host.connection.path(media_file_path).parent
            / ("out_" + audio_file["filename"])
        ),
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
        ancillary_url=str(host.connection.path(media) / ancillary_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="ancillary", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
        interface_setup=setup_interfaces,
    )
