# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import audio_files


@pytest.mark.nightly
@pytest.mark.parametrize("audio_format", ["PCM8", "PCM16", "PCM24"])
@pytest.mark.smoke
def test_st30p_format(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    audio_format,
    test_config,
    prepare_ramdisk,
):

    audio_file = audio_files[audio_format]
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_st30p_format_{audio_format}"  # Set a unique pcap file name
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        audio_format=audio_format,
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=os.path.join(media, audio_file["filename"]),
        out_url=os.path.join(media, audio_file["filename"]),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
