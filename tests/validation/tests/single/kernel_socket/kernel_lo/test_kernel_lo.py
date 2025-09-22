# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import anc_files, audio_files


@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize("video_format", ["i1080p59"])
@pytest.mark.parametrize("replicas", [1, 3])
def test_kernello_mixed_format(
    hosts,
    build,
    media,
    test_time,
    test_mode,
    video_format,
    replicas,
    media_file,
):
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]
    host = list(hosts.values())[0]
    media_file_info, media_file_path = media_file
    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=os.path.join(media, media_file_info["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st20p", replicas=replicas
    )
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
        out_url=os.path.join(media, audio_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="st30p", replicas=replicas
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )
    config = rxtxapp.change_replicas(
        config=config, session_type="ancillary", replicas=replicas
    )
    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time * replicas * 3,
        host=host,
        netsniff=None,
    )
