# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import FileAudioIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.integrity import get_sample_size
from mtl_engine.media_files import audio_files

logger = logging.getLogger(__name__)


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
@pytest.mark.parametrize("audio_ptime", ["1", "0.12", "0.25", "0.33", "4"])
def test_st30p_ptime(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    audio_ptime,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime=audio_ptime,
        filename=media_file_path,
        out_url=str(out_file_url),
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        host=host,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_file_url.name,
            sample_size=get_sample_size(media_file_info["format"]),
            out_path=str(out_file_url.parent),
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")
