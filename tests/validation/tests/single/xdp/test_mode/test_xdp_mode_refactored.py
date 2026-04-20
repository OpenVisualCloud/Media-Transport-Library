# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: native_af_xdp mixed media (st20p + st30p + ancillary).

Mirrors ``test_xdp_mode.py`` using the multi-session ``sessions=[...]`` API.
"""
import os

import pytest
from mtl_engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.refactored
@pytest.mark.parametrize("test_mode", ["multicast", "unicast"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_xdp_mode_refactored(
    hosts,
    mtl_path,
    media,
    test_time,
    test_mode,
    video_format,
    replicas,
    prepare_ramdisk,
    rxtxapp,
):
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    interfaces_list = ["native_af_xdp:eth2", "native_af_xdp:eth3"]

    rxtxapp.create_command(
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        replicas=replicas,
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
