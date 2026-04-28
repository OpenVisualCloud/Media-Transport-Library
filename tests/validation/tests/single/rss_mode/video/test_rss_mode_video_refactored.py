# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files["i1080p60"],
        yuv_files["i2160p60"],
    ],
    indirect=["media_file"],
    ids=[
        "i1080p60",
        "i2160p60",
    ],
)
@pytest.mark.refactored
@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
def test_rss_mode_video_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    rss_mode,
    test_config,
    media_file,
    pcap_capture,
    application,
):
    """Refactored test for rss mode video.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param rss_mode: Parametrized RSS mode (``hash``, ``none`` ...).
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        rss_mode=rss_mode,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
