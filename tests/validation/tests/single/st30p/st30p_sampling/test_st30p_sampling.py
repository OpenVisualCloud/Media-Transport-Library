# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import audio_files


@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM16"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=[
        "PCM8",
        "PCM16",
        "PCM24",
    ],
)
@pytest.mark.parametrize("audio_sampling", ["48kHz", "96kHz"])
def test_st30p_sampling(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    audio_sampling,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_st30p_sampling_{media_file_info['format']}"  # Set a unique pcap file name
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channel=["U02"],
        audio_sampling=audio_sampling,
        audio_ptime="1",
        filename=media_file_path,
        out_url=media_file_path,  # TODO: Fix path
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
