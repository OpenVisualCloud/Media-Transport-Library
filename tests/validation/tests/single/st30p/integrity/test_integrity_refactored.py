# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
from pathlib import Path

import pytest
from common.nicctl import InterfaceSetup
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine.const import LOG_FOLDER
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
@pytest.mark.refactored
def test_integrity_refactored(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    pcap_capture,
    application,
):
    """Refactored test for integrity.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param media: Path to the media files directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    """
    media_file_info, media_file_path = media_file

    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # Ensure the output directory exists.
    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.wav")
    host = list(hosts.values())[0]

    application.create_command(
        session_type="st30p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        audio_format=media_file_info["format"],
        audio_channels=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        input_file=media_file_path,
        output_file=out_file_url,
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path, test_time=test_time, host=host, netsniff=pcap_capture
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
