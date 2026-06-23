# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored: PMD-TX + kernel-RX hybrid mixed media (st20p + st30p + ancillary).

Mirrors ``test_pmd_kernel_mixed.py`` using the multi-session
``sessions=[...]`` API of the refactored ``RxTxApp`` class.
"""
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import (
    anc_files,
    audio_files,
    parse_fps_to_pformat,
    yuv_files,
)


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files["i1080p59"]],
    indirect=["media_file"],
    ids=["i1080p59"],
)
@pytest.mark.refactored
@pytest.mark.parametrize("replicas", [1, 4])
def test_pmd_kernel_mixed_format_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    replicas,
    test_config,
    media_file,
    application,
):
    """Refactored test for pmd kernel mixed format.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param media: Path to the media files directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_mode: Transport mode parameter (e.g. ``unicast``, ``multicast``, ``kernel``).
    :param replicas: Number of session replicas to spawn.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    # PMD-TX + kernel-RX hybrid + multi-session init is slower than VF.
    test_time = max(test_time, 90)

    interfaces_list = setup_interfaces.get_pmd_kernel_interfaces(
        test_config.get("interface_type", "VF")
    )

    audio_in = str(host.connection.path(media) / audio_file["filename"])
    audio_out = str(
        host.connection.path(media_file_path).parent / ("out_" + audio_file["filename"])
    )
    anc_path = str(host.connection.path(media) / ancillary_file["filename"])

    application.create_command(
        nic_port_list=interfaces_list,
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

    application.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        interface_setup=setup_interfaces,
    )
