# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
"""Refactored RSS-mode performance binary-search test (uses fail_on_error=False)."""
import logging

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_result_note
from mtl_engine.media_files import yuv_files

logger = logging.getLogger(__name__)


def _try_replicas(rxtxapp, mtl_path, host, test_time, replicas: int) -> bool:
    """Re-issue create_command with new replicas count and run; return passed bool."""
    rxtxapp.params["replicas"] = replicas
    # Re-build command/config so the new replicas value lands in the JSON
    rxtxapp.command, rxtxapp.config = rxtxapp._create_command_and_config()
    return rxtxapp.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        fail_on_error=False,
    )


@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files["i1080p60"],
        yuv_files["i2160p60"],
    ],
    indirect=["media_file"],
    ids=["i1080p60", "i2160p60"],
)
@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
def test_rss_mode_video_performance_refactored(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    rss_mode,
    test_config,
    prepare_ramdisk,
    media_file,
    rxtxapp,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    rxtxapp.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        rss_mode=rss_mode,
        replicas=1,
        test_time=test_time,
    )

    # Find upper bound: keep doubling until failure
    replicas_b = 1
    while True:
        passed = _try_replicas(rxtxapp, mtl_path, host, test_time, replicas_b)
        if passed:
            logger.info(
                f"test_rss_mode_video_performance_refactored passed with {replicas_b} replicas"
            )
            replicas_b *= 2
        else:
            logger.info(
                f"test_rss_mode_video_performance_refactored failed with {replicas_b} replicas"
            )
            break

    # Binary search between replicas_a and replicas_b
    replicas_a = round(replicas_b / 2)
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)
        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            logger.info(
                f"test_rss_mode_video_performance_refactored finished with {replicas_a} replicas"
            )
            log_result_note(f"{replicas_a} replicas")
            break
        passed = _try_replicas(rxtxapp, mtl_path, host, test_time, replicas_midpoint)
        if passed:
            logger.info(
                f"test_rss_mode_video_performance_refactored passed with {replicas_midpoint} replicas"
            )
            replicas_a = replicas_midpoint
        else:
            logger.info(
                f"test_rss_mode_video_performance_refactored failed with {replicas_midpoint} replicas"
            )
            replicas_b = replicas_midpoint
