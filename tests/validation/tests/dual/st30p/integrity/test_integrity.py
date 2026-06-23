# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_st30p_framebuff_size, check_st30p_integrity
from mtl_engine.media_files import audio_files

logger = logging.getLogger(__name__)


@pytest.mark.dual
@pytest.mark.parametrize("audio_format", ["PCM8", "PCM24"])
def test_integrity_dual(
    hosts,
    mtl_path,
    media,
    nic_port_list,
    test_time,
    audio_format,
):
    st30p_file = audio_files[audio_format]
    st30p_file_url = os.path.join(media, st30p_file["filename"])

    # Get TX and RX hosts
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual tests require at least 2 hosts")

    tx_host = host_list[0]
    rx_host = host_list[1]

    # Ensure the output directory exists for the integrity test output file.
    log_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")
    os.makedirs(log_dir, exist_ok=True)
    out_file_url = os.path.join(log_dir, "out.wav")

    config = rxtxapp.create_empty_dual_config()
    config = rxtxapp.add_st30p_dual_sessions(
        config=config,
        tx_nic_port_list=tx_host.vfs,
        rx_nic_port_list=rx_host.vfs,
        test_mode="unicast",
        audio_format=audio_format,
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        filename=st30p_file_url,
        out_url=out_file_url,
    )

    rxtxapp.execute_dual_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
    )

    size = calculate_st30p_framebuff_size(
        format=audio_format, ptime="1", sampling="48kHz", channel="U02"
    )
    result = check_st30p_integrity(
        src_url=st30p_file_url, out_url=out_file_url, size=size
    )

    if result:
        logger.log(TEST_PASS, "INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
