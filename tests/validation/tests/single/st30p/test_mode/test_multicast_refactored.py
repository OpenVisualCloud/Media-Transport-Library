# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import os

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.execute import LOG_FOLDER
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
@pytest.mark.refactored
def test_multicast_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    # Ensure the output directory exists.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.wav")

    rxtxapp.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
