# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.GstreamerApp as gstreamerapp
import tests.Engine.media_creator as media_create


@pytest.mark.parametrize("audio_format", ["s8", "s16le", "s24le"])
@pytest.mark.parametrize("audio_channel", [1, 2])
@pytest.mark.parametrize("audio_rate", [44100, 48000, 96000])
def test_audio_format(
    build, media, nic_port_list, audio_format, audio_channel, audio_rate
):
    input_file_path = os.path.join(media, "test_audio.pcm")

    media_create.create_audio_file_sox(
        sample_rate=audio_rate,
        channels=audio_channel,
        bit_depth=gstreamerapp.audio_format_change(audio_format),
        duration=10,
        frequency=440,
        output_path=input_file_path,
    )

    tx_config = gstreamerapp.setup_gstreamer_st30_tx_pipeline(
        build=build,
        nic_port_list=nic_port_list[0],
        input_path=input_file_path,
        tx_payload_type=111,
        tx_queues=4,
        audio_format=audio_format,
        channels=audio_channel,
        sampling=audio_rate,
    )

    rx_config = gstreamerapp.setup_gstreamer_st30_rx_pipeline(
        build=build,
        nic_port_list=nic_port_list[1],
        output_path=os.path.join(media, "output_audio.pcm"),
        rx_payload_type=111,
        rx_queues=4,
        rx_audio_format=gstreamerapp.audio_format_change(audio_format, rx_side=True),
        rx_channels=audio_channel,
        rx_sampling=audio_rate,
    )

    try:
        gstreamerapp.execute_test(
            build=build,
            tx_command=tx_config,
            rx_command=rx_config,
            input_file=input_file_path,
            output_file=os.path.join(media, "output_audio.pcm"),
            type="st30",
            tx_first=True,
            sleep_interval=0,
        )
    finally:
        media_create.remove_file(input_file_path)
        media_create.remove_file(os.path.join(media, "output_audio.pcm"))
