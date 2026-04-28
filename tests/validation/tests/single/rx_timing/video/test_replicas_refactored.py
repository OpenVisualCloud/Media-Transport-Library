# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files


@pytest.mark.refactored
def test_rx_timing_video_replicas_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    pcap_capture,
    application,
):
    """Refactored test for rx timing video replicas.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param media: Path to the media files directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    video_file = yuv_files["i1080p60"]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # Multi-replica + rx_timing parser needs headroom for the parser to
    # collect enough samples to classify narrow/wide.
    test_time = max(test_time, 90)

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=video_file["width"],
        height=video_file["height"],
        framerate=f"p{video_file['fps']}",
        pixel_format=video_file["file_format"],
        transport_format=video_file["format"],
        input_file=str(host.connection.path(media, video_file["filename"])),
        replicas=2,
        rx_timing_parser=True,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
    )
