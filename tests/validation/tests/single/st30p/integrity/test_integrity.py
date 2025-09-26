# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_st30p_framebuff_size, check_st30p_integrity
from mtl_engine.media_files import audio_files

logger = logging.getLogger(__name__)


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        audio_files["PCM8"],
        audio_files["PCM24"],
    ],
    indirect=["media_file"],
    ids=[
        "PCM8",
        "PCM24",
    ],
)
def test_integrity(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    media_file,
):
    media_file_info, media_file_path = media_file

    # Ensure the output directory exists.
    host = list(hosts.values())[0]
    out_file_url = host.connection.path(media_file_path).parent / "out.pcm"

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
        out_url=out_file_url,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
        netsniff=None,
    )

    size = calculate_st30p_framebuff_size(
        format=media_file_info["format"], ptime="1", sampling="48kHz", channel="U02"
    )
    result = check_st30p_integrity(
        src_url=media_file_path, out_url=out_file_url, size=size
    )

    if result:
        logger.log(TEST_PASS, "INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
