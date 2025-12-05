# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.execute import log_result_note
from mtl_engine.media_files import yuv_files

logger = logging.getLogger(__name__)


@pytest.mark.parametrize(
    "video_format",
    [
        "i1080p29",
        "i1080p50",
        "i1080p59",
        "i2160p29",
        "i2160p50",
        "i2160p59",
        "i4320p29",
        "i4320p50",
        "i4320p59",
    ],
)
def test_perf_1tx_1nic_1port(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    video_format,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"), count=1
    )

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_st20p_session_tx(
        config=config,
        nic_port=interfaces_list[0],
        ip="192.168.17.101",
        dip="239.168.48.9",
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )

    # upper bound
    replicas_b = 1

    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas_b
        )
        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=build,
                test_time=test_time,
                host=host,
                fail_on_error=False,
            )
        except Exception as e:
            logger.info(
                f"Exception occurred during performance test with {replicas_b} replicas: {e}"
            )
            passed = False

        if passed:
            logger.info(f"{__name__} {video_format} passed with {replicas_b} replicas")
            replicas_b *= 2
        else:
            logger.info(f"{__name__} {video_format} failed with {replicas_b} replicas")
            break

    # lower bound
    replicas_a = round(replicas_b / 2)
    if replicas_a == 0:
        # If we only tested 1 replica and it failed, log and exit
        logger.info(
            f"{__name__} {video_format} finished with 0 replicas (no successful runs)"
        )
        log_result_note("0 replicas")
        return
    # find maximum number of replicas
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)

        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            logger.info(
                f"{__name__} {video_format} finished with {replicas_a} replicas"
            )
            log_result_note(f"{replicas_a} replicas")
            break

        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas_midpoint
        )

        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=build,
                test_time=test_time,
                host=host,
                fail_on_error=False,
            )
        except Exception as e:
            logger.info(
                f"Exception occurred during binary search with {replicas_midpoint} replicas: {e}"
            )
            passed = False

        if passed:
            logger.info(
                f"{__name__} {video_format} passed with {replicas_midpoint} replicas"
            )
            replicas_a = replicas_midpoint
        else:
            logger.info(
                f"{__name__} {video_format} failed with {replicas_midpoint} replicas"
            )
            replicas_b = replicas_midpoint
