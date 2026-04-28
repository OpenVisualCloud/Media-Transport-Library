# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
from pathlib import Path

import pytest
from common.nicctl import InterfaceSetup
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine import ip_pools
from mtl_engine.const import LOG_FOLDER
from mtl_engine.execute import log_fail
from mtl_engine.integrity import calculate_yuv_frame_size, check_st20p_integrity
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

logger = logging.getLogger(__name__)


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Penguin_720p"],
        yuv_files_422rfc10["Penguin_1080p"],
        yuv_files_422p10le["Penguin_720p"],
        yuv_files_422p10le["Penguin_1080p"],
    ],
    indirect=["media_file"],
    ids=[
        "Penguin_720p_422rfc10",
        "Penguin_1080p_422rfc10",
        "Penguin_720p_422p10le",
        "Penguin_1080p_422p10le",
    ],
)
@pytest.mark.refactored
def test_integrity_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    media_file,
    application,
):
    """Test video integrity by comparing input and output files.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file

    log_dir = Path.cwd() / LOG_FOLDER / "latest"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_file_url = str(log_dir / "out.yuv")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate="p25",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=out_file_url,
        test_mode="unicast",
        pacing="linear",
        test_time=test_time,
    )

    actual_test_time = max(test_time, 8)
    application.execute_test(
        build=mtl_path, test_time=actual_test_time, host=host, netsniff=pcap_capture
    )

    frame_size = calculate_yuv_frame_size(
        media_file_info["width"],
        media_file_info["height"],
        media_file_info["file_format"],
    )
    result = check_st20p_integrity(
        src_url=media_file_path, out_url=out_file_url, frame_size=frame_size
    )

    if result:
        logger.log(TEST_PASS, "INTEGRITY PASS")
    else:
        log_fail("INTEGRITY FAIL")
        raise AssertionError(
            "st20p integrity test failed content integrity comparison."
        )
