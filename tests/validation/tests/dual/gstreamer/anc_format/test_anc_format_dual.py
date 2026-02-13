# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp

TMP_INPUT_FILE = "/tmp/test_anc.txt"
TMP_OUTPUT_FILE = "/tmp/output_anc_dual.txt"


@pytest.mark.dual
def test_st40p_dual_host_basic(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Dual-host sanity with one VF per host to validate cross-host ANC delivery.
    """
    if len(hosts) < 2:
        pytest.skip("Dual-host topology not available")

    tx_host, rx_host = list(hosts.values())[:2]

    tx_interfaces = setup_interfaces.get_test_interfaces(
        test_config.get("interface_type", "VF"), count=1, host=tx_host
    )[tx_host.name]
    rx_interfaces = setup_interfaces.get_test_interfaces(
        test_config.get("interface_type", "VF"), count=1, host=rx_host
    )[rx_host.name]

    if not tx_interfaces or not rx_interfaces:
        pytest.skip("Insufficient interfaces for dual-host test")

    media_file_info, media_root = media_file
    if not media_root:
        raise ValueError("ramdisk was not setup correctly for media_file fixture")

    input_file_path = os.path.join(media_root, "input_anc.txt")
    output_file_path = os.path.join(media_root, "output_anc.txt")

    input_file_path = media_create.create_text_file(
        size_kb=10,
        output_path=input_file_path,
        host=tx_host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=tx_interfaces[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=50,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=rx_interfaces[0],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=15,
    )

    expectation = "Dual-host ST40P ANC text payload survives cross-host path"

    try:
        assert GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=8,
            tx_host=tx_host,
            rx_host=rx_host,
            tx_first=True,
            sleep_interval=4,
            log_frame_info=True,
        ), expectation
    finally:
        media_create.remove_file(input_file_path, host=tx_host)
        media_create.remove_file(output_file_path, host=rx_host)


@pytest.mark.dual
@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size_dual(
    hosts,
    build,
    media,
    nic_port_list,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
):
    """Test GStreamer ST40P ANC format in dual host configuration."""
    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Create input file on TX host
    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=TMP_INPUT_FILE,
        host=tx_host,
    )

    # Create output file path for RX host
    output_file_path = TMP_OUTPUT_FILE

    # Setup TX pipeline using existing function
    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=tx_host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    # Setup RX pipeline using existing function
    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=rx_host.vfs[0],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=15,
    )

    try:
        result = GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            sleep_interval=3,
            tx_first=False,
            log_frame_info=True,
        )

        assert (
            result
        ), f"GStreamer dual ST40P test failed for fps={fps}, size={file_size_kb}KB"

    finally:
        # Remove the input file on TX host and output file on RX host
        media_create.remove_file(input_file_path, host=tx_host)
        media_create.remove_file(output_file_path, host=rx_host)
