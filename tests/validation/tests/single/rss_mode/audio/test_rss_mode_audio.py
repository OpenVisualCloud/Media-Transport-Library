# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import FileAudioIntegrityRunner
from mtl_engine.execute import log_fail
from mtl_engine.media_files import audio_files


logger = logging.getLogger(__name__)


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
@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
def test_rss_mode_audio(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    rss_mode,
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
        f"test_rss_mode_audio_{media_file_info['format']}_{rss_mode}"
    )

    out_file_url = host.connection.path(media_file_path).parent / "out.wav"

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st30p_sessions(
        config=config,
        nic_port_list=host.vfs,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=media_file_path,
        out_url=str(out_file_url),
    )
    config = rxtxapp.change_rss_mode(content=config, rss_mode=rss_mode)

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        capture_cfg=capture_cfg,
    )
    
    if test_config.get("integrity_check", True):
        logger.info("Running audio integrity check...")
        integrity = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=build,
            src_url=media_file_path,
            out_name=out_file_url.name,
            out_path=str(out_file_url.parent)
        )
        result = integrity.run()
        if not result:
            log_fail("Audio integrity check failed")
