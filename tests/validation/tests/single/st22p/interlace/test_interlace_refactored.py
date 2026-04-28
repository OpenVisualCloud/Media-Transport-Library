# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_interlace


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    indirect=["media_file"],
    ids=list(yuv_files_interlace.keys()),
)
@pytest.mark.refactored
def test_interlace_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    media_file,
    application,
):
    """Refactored test for interlace.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param media: Path to the media files directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # JPEG-XS plugin init adds 3-10s on top of MTL init.
    test_time = max(test_time, 90)

    application.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality="speed",
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        codec_threads=2,
        interlaced=True,
        test_time=test_time,
    )
    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
