# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: rx_timing/mixed (st20p + st30p + ancillary with rx_timing_parser).

Mirrors ``test_mode.py`` using the multi-session ``sessions=[...]`` API.
"""
import os

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
    prepare_ramdisk,
    rxtxapp,
):
    video_file = yuv_files["i1080p50"]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
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
                "input_file": os.path.join(media, video_file["filename"]),
                "output_file": os.path.join(media, video_file["filename"]),
            },
            {
                "session_type": "st30p",
                "audio_format": "PCM24",
                "audio_channels": ["U02"],
                "audio_sampling": "48kHz",
                "audio_ptime": "1",
                "input_file": os.path.join(media, audio_file["filename"]),
                "output_file": os.path.join(media, audio_file["filename"]),
            },
            {
                "session_type": "ancillary",
                "type_mode": "frame",
                "ancillary_format": "closed_caption",
                "ancillary_fps": ancillary_file["fps"],
                "ancillary_url": os.path.join(media, ancillary_file["filename"]),
            },
        ],
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
