# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import mtl_engine.RxTxApp as rxtxapp
from mtl_engine.media_files import audio_files
from tests.xfail import SDBQ1001_audio_channel_check

@pytest.mark.parametrize(
    "audio_channel", ["M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP"]
)
@pytest.mark.parametrize("audio_format", ["PCM8", "PCM16", "PCM24"])
def test_st30p_channel_dual(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    audio_format,
    audio_channel,
    request,
):
    SDBQ1001_audio_channel_check(audio_channel, audio_format, request)

    audio_file = audio_files[audio_format]
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    config = rxtxapp.create_empty_dual_config()
    config = rxtxapp.add_st30p_dual_sessions(
        config=config,
        tx_nic_port_list=tx_host.vfs,
        rx_nic_port_list=rx_host.vfs,
        test_mode="multicast",
        audio_format=audio_format,
        audio_channel=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
    )

    rxtxapp.execute_dual_test(
        config=config,
        build=build,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
    )
