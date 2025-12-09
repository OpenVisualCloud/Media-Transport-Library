# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_result_note
from mtl_engine.media_files import yuv_files

logger = logging.getLogger(__name__)


@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files["i1080p60"],
        yuv_files["i2160p60"],
    ],
    indirect=["media_file"],
    ids=[
        "i1080p60",
        "i2160p60",
    ],
)
@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
def test_rss_mode_video_performance(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    rss_mode,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )
    config = rxtxapp.change_rss_mode(content=config, rss_mode=rss_mode)

    # upper bound
    replicas_b = 1

    # find upper bound
    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas_b
        )
        passed = rxtxapp.execute_test(
            config=config,
            build=build,
            test_time=test_time,
            fail_on_error=False,
            host=host,
        )

        if passed:
            logger.info(
                f"test_rss_mode_video_performance passed with {replicas_b} replicas"
            )
            replicas_b *= 2
        else:
            logger.info(
                f"test_rss_mode_video_performance failed with {replicas_b} replicas"
            )
            break

    # lower bound
    replicas_a = round(replicas_b / 2)

    # find maximum number of replicas
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)

        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            logger.info(
                f"test_rss_mode_video_performance finished with {replicas_a} replicas"
            )
            log_result_note(f"{replicas_a} replicas")
            break

        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas_midpoint
        )
        passed = rxtxapp.execute_test(
            config=config,
            build=build,
            test_time=test_time,
            fail_on_error=False,
            host=host,
        )

        if passed:
            logger.info(
                f"test_rss_mode_video_performance passed with {replicas_midpoint} replicas"
            )
            replicas_a = replicas_midpoint
        else:
            logger.info(
                f"test_rss_mode_video_performance failed with {replicas_midpoint} replicas"
            )
            replicas_b = replicas_midpoint
