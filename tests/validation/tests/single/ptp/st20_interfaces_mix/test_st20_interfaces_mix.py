# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import logging
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.integrity.integrity_runner import FileVideoIntegrityRunner
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_fail
from mtl_engine.media_files import yuv_files_422rfc10

logger = logging.getLogger(__name__)


def _is_supported_runner() -> bool:
    """Check if the test is running on a supported runner (e810 or e830)."""
    workflow = os.environ.get("MTL_GITHUB_WORKFLOW", "")
    # MTL_GITHUB_WORKFLOW format is "nightly-pytest:e810" or "nightly-pytest:e810-dell" etc.
    # Return True if not in CI (no workflow set) or if running on e810 or e830 (not e810-dell)
    if not workflow:
        return True  # Not in CI, allow test to run
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
        pytest.param(
            {"mode": "vf_only"},
            id="vf_only",
        ),
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
def test_st20_interfaces_mix(
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
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
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

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
        out_url=video_out_url,
    )

    rxtxapp.execute_test(
        config=config,
        build=mtl_path,
        test_time=test_time,
        ptp=True,
        rx_max_file_size=5 * 1024 * 1024 * 1024,  # 5 GB limit for rx file size
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
