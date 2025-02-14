# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import audio_files
from tests.xfail import SDBQ1001_audio_channel_check


@pytest.mark.parametrize(
    "audio_channel", ["M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP"]
)
@pytest.mark.parametrize("audio_format", ["PCM8", "PCM16", "PCM24"])
def test_audio_channel(
    build, media, nic_port_list, test_time, audio_format, audio_channel, request
):
    # The max frame size that could be sent in MTL is:
    # ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr) = 1440 bytes
    # "222" represents "22.2" which requires 24 channels
    # with 24 channel size, 1ms ptime and audio_format PCM16 or PCM24 the packet size is too big (~2100 - ~2800 bytes)
    # to send the packet with this size ptime should be changed to the lower value (0.33 ms)
    audio_ptime = "1"  # 1ms
    if audio_channel == "222" and (audio_format == "PCM16" or audio_format == "PCM24"):
        audio_ptime = "0.33"  # 333 us

    SDBQ1001_audio_channel_check(audio_channel, audio_format, request)

    audio_file = audio_files[audio_format]
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_audio_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        type_="frame",
        audio_format=audio_format,
        audio_channel=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime=audio_ptime,
        audio_url=os.path.join(media, audio_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
