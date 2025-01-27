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
def test_st30p_channel(
    build, media, nic_port_list, test_time, audio_format, audio_channel, request
):
    SDBQ1001_audio_channel_check(audio_channel, audio_format, request)

    audio_file = audio_files[audio_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="multicast",
        audio_format=audio_format,
        audio_channel=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
