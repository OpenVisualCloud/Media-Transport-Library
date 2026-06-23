# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored hybrid PMD (TX) + kernel-socket (RX) ST20P video test.

Uses the unified ``application`` fixture and forwards ``setup_interfaces`` to
``execute_test`` so that kernel-socket OS IPs are configured and registered
for cleanup, mirroring the legacy behaviour.
"""
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize("test_mode", ["multicast"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files["i1080p59"]],
    indirect=["media_file"],
    ids=["i1080p59"],
)
# Note: i2160p59 excluded - kernel socket cannot handle 4K@59fps bandwidth (10.4Gbps)
@pytest.mark.refactored
@pytest.mark.parametrize("replicas", [1, 2])
def test_pmd_kernel_video_format_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_mode,
    replicas,
    test_config,
    media_file,
    application,
):
    """Refactored test for pmd kernel video format.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_mode: Transport mode parameter (e.g. ``unicast``, ``multicast``, ``kernel``).
    :param replicas: Number of session replicas to spawn.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_pmd_kernel_interfaces(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=parse_fps_to_pformat(media_file_info["fps"]),
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        replicas=replicas,
        test_time=test_time * 2,
    )

    application.execute_test(
        build=mtl_path,
        test_time=test_time * 2,
        host=host,
        interface_setup=setup_interfaces,
    )
