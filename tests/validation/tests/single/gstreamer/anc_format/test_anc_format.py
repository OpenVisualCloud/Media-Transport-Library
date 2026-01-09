# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp


# helper function to setup input and output file paths for ancillary files
def setup_paths(media_file, host):
    media_file_info, media_file_path = media_file
    if not media_file_path:
        raise ValueError("ramdisk was not setup correctly for media_file fixture")

    input_file_path = os.path.join(media_file_path, "input_anc.txt")
    output_file_path = os.path.join(media_file_path, "output_anc.txt")
    return input_file_path, output_file_path


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=15,
    )

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=5,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [60])
@pytest.mark.parametrize("file_size_kb", [100])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_framebuff(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    # Base the timeout on parameter to make sure the amount of time between RX and TX
    # is less than the timeout period
    timeout_period = 20

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=timeout_period + 10,
    )

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=timeout_period,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)

    """ Validate ST40p integrity using GStreamer RFC8331 pipelines.
        A pseudo RFC8331 input file, generated via ancgenerator, carries fixed
        ancillary frames, output is compared against simplified Python output to
        verify metadata consistency.
        This verifies Ancillary integrity in complex scenarios for MTL library.
        GStreamer ancillary pipelines with rfc8331 format and simplified rf8331 format.
    """


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_format_8331(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # Based on this parameters
    timeout_period = 15

    input_file_path, output_file_path = setup_paths(media_file)

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=timeout_period + 10,
        capture_metadata=True,
    )

    try:
        GstreamerApp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            host=host,
            tx_first=False,
            sleep_interval=timeout_period,
        )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
