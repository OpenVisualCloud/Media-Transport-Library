# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import FileAudioIntegrityRunner
from mtl_engine.execute import log_fail
from mtl_engine.integrity import get_channel_number, get_sample_size
from mtl_engine.media_files import audio_files
from tests.xfail import SDBQ1001_audio_channel_check

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
@pytest.mark.parametrize(
    "audio_channel", ["M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP"]
)
def test_st30p_channel(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    audio_channel,
    request,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file

    SDBQ1001_audio_channel_check(audio_channel, media_file_info["format"], request)

    host = list(hosts.values())[0]
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="multicast",
        audio_format=media_file_info["format"],
        audio_channel=[audio_channel],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=media_file_path,
        out_url=str(out_file_url),
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=build,
            src_url=media_file_path,
            out_name=out_file_url.name,
            channel_num=get_channel_number(audio_channel),
            sample_size=get_sample_size(media_file_info["format"]),
            out_path=str(out_file_url.parent),
            delete_file=True,
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")
