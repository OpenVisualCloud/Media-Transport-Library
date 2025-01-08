# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import audio_files


@pytest.mark.parametrize("type_mode", ["rtp", "frame"])
@pytest.mark.parametrize("audio_format", ["PCM8", "PCM16", "PCM24"])
def test_type_mode(build, media, nic_port_list, test_time, audio_format, type_mode):
    audio_file = audio_files[audio_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_audio_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_=type_mode,
        audio_format=audio_format,
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        audio_url=os.path.join(media, audio_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
