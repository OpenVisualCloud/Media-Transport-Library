# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp


@pytest.mark.dual
@pytest.mark.parametrize("audio_format", ["S8", "S16BE", "S24BE"])
@pytest.mark.parametrize("audio_channel", [1, 2])
@pytest.mark.parametrize("audio_rate", [44100, 48000, 96000])
def test_audio_format_dual(
    hosts,
    mtl_path,
    media,
    nic_port_list,
    audio_format,
    audio_channel,
    audio_rate,
    test_time,
    test_config,
    prepare_ramdisk,
):
    """Test GStreamer ST30 audio format in dual host configuration."""
    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Create input file on TX host
    input_file_path = os.path.join(media, "test_audio.pcm")

    # media_create.create_audio_file_sox(
    #     sample_rate=audio_rate,
    #     channels=audio_channel,
    #     bit_depth=GstreamerApp.audio_format_change(audio_format),
    #     duration=10,
    #     frequency=440,
    #     output_path=input_file_path,
    #     host=tx_host,
    # )

    # Create output file path for RX host
    output_file_path = os.path.join(
        media, f"output_audio_dual_{audio_format}_{audio_channel}_{audio_rate}.pcm"
    )

    # Setup TX pipeline using existing function
    tx_config = GstreamerApp.setup_gstreamer_st30_tx_pipeline(
        build=mtl_path,
        nic_port_list=tx_host.vfs[0],
        input_path=input_file_path,
        tx_payload_type=111,
        tx_queues=4,
        audio_format=audio_format,
        channels=audio_channel,
        sampling=audio_rate,
    )

    # Setup RX pipeline using existing function
    rx_config = GstreamerApp.setup_gstreamer_st30_rx_pipeline(
        build=mtl_path,
        nic_port_list=rx_host.vfs[0],
        output_path=output_file_path,
        rx_payload_type=111,
        rx_queues=4,
        rx_audio_format=GstreamerApp.audio_format_change(audio_format, rx_side=True),
        rx_channels=audio_channel,
        rx_sampling=audio_rate,
    )

    try:
        result = GstreamerApp.execute_test(
            build=mtl_path,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=output_file_path,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            tx_first=False,
            sleep_interval=1,
        )

        assert (
            result
        ), f"GStreamer dual audio format test failed for format {audio_format}"

    finally:
        # Remove the output file on RX host
        # media_create.remove_file(input_file_path, host=tx_host)
        media_create.remove_file(output_file_path, host=rx_host)
