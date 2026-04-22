# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Refactored PTP + interface-mix ST20P test (new RxTxApp API).

Single-session ST2110-20 video with PTP enabled, optional pcap capture, and
an out-of-band ``FileVideoIntegrityRunner`` post-check (kept identical to the
legacy test).
"""
import logging
import os

import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.media_files import yuv_files_422rfc10

logger = logging.getLogger(__name__)


def _is_supported_runner() -> bool:
    """Skip on e810-dell where ptp4l sync is unstable."""
    workflow = os.environ.get("MTL_GITHUB_WORKFLOW", "")
    if not workflow:
        return True
    return workflow.endswith(":e810") or workflow.endswith(":e830")


@pytest.mark.nightly
@pytest.mark.ptp
@pytest.mark.skipif(
    not _is_supported_runner(),
    reason="This test is not supported on e810-dell, "
    "because of problems with ptp4l synchronization.",
)
@pytest.mark.parametrize(
    "interface_profile",
    [
        pytest.param({"mode": "vf_only"}, id="vf_only"),
        pytest.param(
            {"mode": "mixed", "tx_type": "PF", "rx_type": "VF"},
            id="pf_tx_vf_rx",
            marks=pytest.mark.skip(reason="pf_tx_vf_rx work but are not stable yet"),
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
    prepare_ramdisk,
    pcap_capture,
    media_file,
    output_files,
    rxtxapp,
):
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

    rxtxapp.create_command(
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

    rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        netsniff=pcap_capture,
    )

    if test_config.get("integrity_check", True):
        logger.info("Running video integrity check...")
        resolution = f"{media_file_info['width']}x{media_file_info['height']}"
        video_integrity = FileVideoIntegrityRunner(
            host=host,
            test_repo_path=mtl_path,
            src_url=media_file_path,
            out_name=os.path.basename(video_out_url),
            resolution=resolution,
            file_format=media_file_info["file_format"],
            out_path=os.path.dirname(video_out_url),
            integrity_path=os.path.join(
                mtl_path, "tests", "validation", "common", "integrity"
            ),
        )
        if not video_integrity.run():
            log_fail("Video integrity check failed")
