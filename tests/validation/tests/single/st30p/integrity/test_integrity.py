# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.execute import LOG_FOLDER, log_info
from mtl_engine.integrity import calculate_st30p_framebuff_size, check_st30p_integrity
from mtl_engine.media_files import audio_files


@pytest.mark.parametrize("audio_format", ["PCM8", "PCM24"])
def test_integrity(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    audio_format,
    test_config,
    prepare_ramdisk,
):
    st30p_file = audio_files[audio_format]
    st30p_file_url = os.path.join(media, st30p_file["filename"])

    # Ensure the output directory exists.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.wav")
    host = list(hosts.values())[0]

    # Get capture configuration from test_config.yaml
    # This controls whether tcpdump capture is enabled, where to store the pcap, etc.
    capture_cfg = dict(test_config.get("capture_cfg", {}))
    capture_cfg["test_name"] = (
        f"test_integrity_{audio_format}"  # Set a unique pcap file name
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
        filename=st30p_file_url,
        out_url=out_file_url,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )

    size = calculate_st30p_framebuff_size(
        format=audio_format, ptime="1", sampling="48kHz", channel="U02"
    )
    result = check_st30p_integrity(
        src_url=st30p_file_url, out_url=out_file_url, size=size
    )

    if result:
        log_info("INTEGRITY PASS")
    else:
        log_info("INTEGRITY FAIL")
