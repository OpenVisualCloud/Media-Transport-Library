# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: rx_timing/mixed (st20p + st30p + ancillary with rx_timing_parser).

Mirrors ``test_mode.py`` using the multi-session ``sessions=[...]`` API.
"""
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.refactored
@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
def test_rx_timing_mode_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    test_config,
    pcap_capture,
    application,
):
    """Refactored test for rx timing mode.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param media: Path to the media files directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_mode: Transport mode parameter (e.g. ``unicast``, ``multicast``, ``kernel``).
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    video_file = yuv_files["i1080p50"]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        rx_timing_parser=True,
        test_time=test_time,
        sessions=[
            {
                "session_type": "st20p",
                "width": video_file["width"],
                "height": video_file["height"],
                "framerate": f"p{video_file['fps']}",
                "pixel_format": video_file["file_format"],
                "transport_format": video_file["format"],
                "input_file": str(host.connection.path(media, video_file["filename"])),
                "output_file": str(host.connection.path(media, video_file["filename"])),
            },
            {
                "session_type": "st30p",
                "audio_format": "PCM24",
                "audio_channels": ["U02"],
                "audio_sampling": "48kHz",
                "audio_ptime": "1",
                "input_file": str(host.connection.path(media, audio_file["filename"])),
                "output_file": str(host.connection.path(media, audio_file["filename"])),
            },
            {
                "session_type": "ancillary",
                "type_mode": "frame",
                "ancillary_format": "closed_caption",
                "ancillary_fps": ancillary_file["fps"],
                "ancillary_url": str(
                    host.connection.path(media, ancillary_file["filename"])
                ),
            },
        ],
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
