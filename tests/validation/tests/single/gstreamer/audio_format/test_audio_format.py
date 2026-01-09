# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp

""" Checks various audio format support in GStreamer ST30 pipelines.
    Validates end-to-end transmission and reception of audio streams with
    different audio formats, channels, and sampling rates.
    No integrity check is performed, only format handling is verified.
"""


@pytest.mark.nightly
@pytest.mark.parametrize("audio_format", ["S8", "S16BE", "S24BE"])
@pytest.mark.parametrize("audio_channel", [1, 2, 6, 8])
@pytest.mark.parametrize("audio_rate", [44100, 48000, 96000])
def test_audio_format(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    audio_format,
    audio_channel,
    audio_rate,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    if audio_rate == 96000 and (
        audio_channel == 8
        and (audio_format == "S16BE" or audio_format == "S24BE")
        or audio_channel == 6
        and audio_format == "S24BE"
    ):
        pytest.skip(
            f"Audio, {audio_format} with {audio_channel} audio channel invalid pkt_len skipped"
        )

    media_file_info, media_file_path = media_file
    if not media_file_path:
        raise ValueError("ramdisk was not setup correctly for media_file fixture")

    input_file_path = os.path.join(media_file_path, "input_test_audio.pcm")
    output_file_path = os.path.join(media_file_path, "output_test_audio.pcm")

    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    # media_create.create_audio_file_sox(
    #     sample_rate=audio_rate,
    #     channels=audio_channel,
    #     bit_depth=GstreamerApp.audio_format_change(audio_format),
    #     duration=10,
    #     frequency=440,
    #     output_path=input_file_path,
    #     host=host,
    # )

    # input path doesn't matter we are generating audio inside gstreamer pipeline
    tx_config = GstreamerApp.setup_gstreamer_st30_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=111,
        tx_queues=4,
        audio_format=audio_format,
        channels=audio_channel,
        sampling=audio_rate,
    )

    rx_config = GstreamerApp.setup_gstreamer_st30_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=111,
        rx_queues=4,
        rx_audio_format=GstreamerApp.audio_format_change(audio_format, rx_side=True),
        rx_channels=audio_channel,
        rx_sampling=audio_rate,
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
            sleep_interval=1,
        )
    finally:
        pass
        # media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
