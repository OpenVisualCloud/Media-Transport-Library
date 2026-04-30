# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Refactored PTP + interface-mix ST20P test (new RxTxApp API).

Single-session ST2110-20 video with PTP enabled, optional pcap capture, and
an out-of-band ``FileVideoIntegrityRunner`` post-check (kept identical to the
legacy test).
"""
import logging

import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.media_files import yuv_files_422rfc10

logger = logging.getLogger(__name__)


@pytest.mark.nightly
@pytest.mark.ptp
@pytest.mark.parametrize(
    "interface_profile",
    [
        pytest.param({"mode": "vf_only"}, id="vf_only"),
        pytest.param(
            {"mode": "mixed", "tx_type": "PF", "rx_type": "VF"},
            id="pf_tx_vf_rx",
            marks=pytest.mark.skip(reason="pf_tx_vf_rx works but is not stable yet"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["Crosswalk_720p"],
        yuv_files_422rfc10["ParkJoy_1080p"],
        yuv_files_422rfc10["Pedestrian_4K"],
    ],
    indirect=["media_file"],
    ids=["Crosswalk_720p", "ParkJoy_1080p", "Pedestrian_4K"],
)
@pytest.mark.refactored
def test_st20_interfaces_mix_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    interface_profile,
    test_config,
    pcap_capture,
    media_file,
    output_files,
    application,
):
    """Refactored test for st20 interfaces mix.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param setup_interfaces: Interface setup helper for NIC / VF configuration.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param interface_profile: Parametrized NIC profile (``vf_only`` or ``pf_tx_vf_rx``).
    :param test_config: Test configuration dictionary loaded from ``test_config.yaml``.
    :param pcap_capture: Pcap capture fixture for EBU ST 2110-21 compliance check.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param output_files: Helper that registers RX-output files for cleanup.
    :param application: Media application driver fixture (currently ``RxTxApp``).
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    # PTP sync (+10s in RxTxApp), pcap capture, and large RX file caps
    # collectively need more headroom than the 60s default.
    test_time = max(test_time, 90)

    if interface_profile["mode"] == "vf_only":
        interfaces_list = setup_interfaces.get_interfaces_list_single("VF")
    else:
        tx_index = test_config.get("tx_interface_index", 0)
        rx_index = test_config.get("rx_interface_index", 1)
        interfaces_list = setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type=interface_profile["tx_type"],
            rx_interface_type=interface_profile["rx_type"],
            tx_index=tx_index,
            rx_index=rx_index,
        )

    video_out_url = output_files.register(
        str(
            host.connection.path(media_file_path).parent
            / f"{media_file_info['filename']}.out"
        )
    )

    application.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=video_out_url,
        enable_ptp=True,
        rx_max_file_size=5 * 1024 * 1024 * 1024,  # 5 GB cap
        test_time=test_time,
    )

    application.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running video integrity check...")
        resolution = f"{media_file_info['width']}x{media_file_info['height']}"
        out_path = host.connection.path(video_out_url)
        video_integrity = FileVideoIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=out_path.name,
            resolution=resolution,
            file_format=media_file_info["file_format"],
            out_path=str(out_path.parent),
            integrity_path=str(
                host.connection.path(
                    mtl_path, "tests", "validation", "common", "integrity"
                )
            ),
        )
        if not video_integrity.run():
            log_fail("Video integrity check failed")
