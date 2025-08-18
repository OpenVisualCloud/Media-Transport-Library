# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.media_creator as media_create
import pytest
from mtl_engine import GstreamerApp


@pytest.mark.parametrize("audio_format", ["s8", "s16le", "s24le"])
@pytest.mark.parametrize("audio_channel", [1, 2])
@pytest.mark.parametrize("audio_rate", [44100, 48000, 96000])
def test_audio_format_dual(
    hosts,
    build,
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

    media_create.create_audio_file_sox(
        sample_rate=audio_rate,
        channels=audio_channel,
        bit_depth=GstreamerApp.audio_format_change(audio_format),
        duration=10,
        frequency=440,
        output_path=input_file_path,
        host=tx_host,
    )

    capture_cfg = dict(test_config.get("capture_cfg", {})) if test_config else {}
    capture_cfg["test_name"] = (
        f"test_audio_format_dual_{audio_format}_{audio_channel}_{audio_rate}"
    )

    try:
        result = GstreamerApp.execute_dual_st30_test(
            build=build,
            tx_nic_port=tx_host.vfs[0],
            rx_nic_port=rx_host.vfs[0],
            input_path=input_file_path,
            payload_type=111,
            queues=4,
            audio_format=audio_format,
            channels=audio_channel,
            sampling=audio_rate,
            test_time=test_time,
            tx_host=tx_host,
            rx_host=rx_host,
            capture_cfg=capture_cfg,
        )

        assert result, f"GStreamer dual audio format test failed for format {audio_format}"

    finally:
        # Remove the input file on TX host
        media_create.remove_file(input_file_path, host=tx_host)
