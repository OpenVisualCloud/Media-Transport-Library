# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import FileAudioIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.integrity import get_channel_number, get_sample_size
from mtl_engine.media_files import audio_files

logger = logging.getLogger(__name__)


_AUDIO_FORMATS = ["PCM8", "PCM16", "PCM24"]
_AUDIO_CHANNELS = ["M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP"]
_SMOKE_CASE = ("PCM16", "M")


@pytest.mark.nightly
@pytest.mark.parametrize(
    ("media_file", "audio_channel"),
    [
        pytest.param(
            audio_files[fmt],
            ch,
            marks=[pytest.mark.smoke] if (fmt, ch) == _SMOKE_CASE else [],
            id=f"{fmt}-{ch}",
        )
        for fmt in _AUDIO_FORMATS
        for ch in _AUDIO_CHANNELS
    ],
    indirect=["media_file"],
)
def test_st30p_channel(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    audio_channel,
    request,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    media_file_info, media_file_path = media_file

    if media_file_info["format"] in ["PCM16", "PCM24"] and audio_channel == "222":
        pytest.skip("Unsupported parameter combination")

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=interfaces_list,
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
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
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
