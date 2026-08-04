# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files["i1080p25"],
        yuv_files["i1080p30"],
        yuv_files["i1080p50"],
        yuv_files["i1080p60"],
        yuv_files["i1080p100"],
        yuv_files["i1080p120"],
        yuv_files["i2160p60"],
    ],
    indirect=["media_file"],
    ids=[
        "i1080p25",
        "i1080p30",
        "i1080p50",
        "i1080p60",
        "i1080p100",
        "i1080p120",
        "i2160p60",
    ],
)
@pytest.mark.refactored
def test_rx_timing_video_video_format_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    application,
    media_file,
):
    """Refactored test for rx timing video video format.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    video_file, video_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=video_file["width"],
        height=video_file["height"],
        framerate=f"p{video_file['fps']}",
        pixel_format=video_file["file_format"],
        transport_format=video_file["format"],
        input_file=video_path,
        rx_timing_parser=True,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, compliance=pcap_capture
    )
