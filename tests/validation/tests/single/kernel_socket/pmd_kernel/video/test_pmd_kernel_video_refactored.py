# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Refactored hybrid PMD (TX) + kernel-socket (RX) ST20P video test.

Uses the unified ``rxtxapp`` fixture and forwards ``setup_interfaces`` to
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
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    interfaces_list = setup_interfaces.get_pmd_kernel_interfaces(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
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

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time * 2,
        host=host,
        interface_setup=setup_interfaces,
    )
