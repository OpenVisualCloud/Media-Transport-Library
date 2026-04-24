# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: kernel loopback mixed media (st20p + st30p + ancillary).

Mirrors ``test_kernel_lo.py`` but uses the unified ``rxtxapp`` fixture
with the multi-session ``sessions=[...]`` API added to ``RxTxApp``.

Pass criterion: process rc==0 AND ``check_rx_output`` passes for *every*
populated session type (st20p OK + converters; st30p OK; anc OK).
"""
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
@pytest.mark.refactored
@pytest.mark.parametrize("replicas", [1, 3])
def test_kernello_mixed_format_refactored(
    hosts,
    mtl_path,
    test_time,
    test_mode,
    replicas,
    media_file,
    media,
    setup_interfaces,
    rxtxapp,
):
    """Mixed media streams over kernel loopback (refactored)."""
    media_file_info, media_file_path = media_file
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    # Kernel-socket loopback + multi-session init is slower than VF.
    test_time = max(test_time, 90)

    audio_in = str(host.connection.path(media) / audio_file["filename"])
    audio_out = str(
        host.connection.path(media_file_path).parent
        / ("out_" + audio_file["filename"])
    )
    anc_path = str(host.connection.path(media) / ancillary_file["filename"])

    rxtxapp.create_command(
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        replicas=replicas,
        test_time=test_time,
        sessions=[
            {
                "session_type": "st20p",
                "width": media_file_info["width"],
                "height": media_file_info["height"],
                "framerate": parse_fps_to_pformat(media_file_info["fps"]),
                "pixel_format": media_file_info["file_format"],
                "transport_format": media_file_info["format"],
                "input_file": media_file_path,
                "output_file": media_file_path,
            },
            {
                "session_type": "st30p",
                "audio_format": "PCM24",
                "audio_channels": ["U02"],
                "audio_sampling": "48kHz",
                "audio_ptime": "1",
                "input_file": audio_in,
                "output_file": audio_out,
            },
            {
                "session_type": "ancillary",
                "type_mode": "frame",
                "ancillary_format": "closed_caption",
                "ancillary_fps": ancillary_file["fps"],
                "ancillary_url": anc_path,
            },
        ],
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        interface_setup=setup_interfaces,
    )
