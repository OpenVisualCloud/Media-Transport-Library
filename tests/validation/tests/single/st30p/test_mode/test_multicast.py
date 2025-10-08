# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.media_files import audio_files


@pytest.mark.nightly
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
def test_multicast(
    hosts,
    build,
    media,
    nic_port_list,
    test_config,
    test_time,
    pcap_capture,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=media_file_path,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
        ptp=test_config.get("ptp", False),
    )
