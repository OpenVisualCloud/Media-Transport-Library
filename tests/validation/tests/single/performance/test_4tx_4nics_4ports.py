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
def test_perf_4tx_4nics_4ports(
    hosts,
    mtl_path,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    video_format,
    test_config,
    prepare_ramdisk,
):
    # For 4 NICs init time of the app is increased
    test_time = 60

    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VFxPF"), count=4
    )

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[0],
        ip="192.168.17.101",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[1],
        ip="192.168.17.102",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[2],
        ip="192.168.17.103",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[3],
        ip="192.168.17.104",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    # upper bound
    replicas_b = 1

    # find upper bound
    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_b, rx=False
        )
        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=mtl_path,
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
            config=config, session_type="video", replicas=replicas_midpoint, rx=False
        )

        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=mtl_path,
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
