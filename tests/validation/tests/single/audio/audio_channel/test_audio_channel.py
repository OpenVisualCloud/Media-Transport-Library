# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
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
