# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.parametrize("test_mode", ["multicast", "unicast"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 4])
def test_xdp_mode(build, media, test_time, test_mode, video_format, replicas):
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
        test_mode=test_mode,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="video", replicas=replicas
    )
    config = rxtxapp.add_audio_sessions(
        config=config,
        nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
        test_mode=test_mode,
        type_="frame",
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        audio_url=os.path.join(media, audio_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="audio", replicas=replicas
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="ancillary", replicas=replicas
    )
    rxtxapp.execute_test(config=config, build=build, test_time=test_time)
