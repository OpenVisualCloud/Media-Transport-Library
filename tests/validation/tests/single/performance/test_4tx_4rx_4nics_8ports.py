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
def test_perf_4tx_4rx_4nics_8ports(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    nic_port_list,
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
        test_config.get("interface_type", "2VFxPF"), count=8
    )
    # interface_list contains 8 addresses of vfs for 4 pfs in order:
    # first 2 addresses - nic0, second 2 addresses nic1 etc.

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[0],  # from NIC 0 to NIC 1
        ip="192.168.17.101",
        dip="192.168.17.105",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[2],  # from NIC 1 to NIC 2
        ip="192.168.17.102",
        dip="192.168.17.106",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[4],  # from NIC 2 to NIC 3
        ip="192.168.17.103",
        dip="192.168.17.107",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=interfaces_list[6],  # from NIC 3 to NIC 0
        ip="192.168.17.104",
        dip="192.168.17.108",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=interfaces_list[3],  # NIC 1 Rx
        ip="192.168.17.105",
        sip="192.168.17.101",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=interfaces_list[5],  # NIC 2 Rx
        ip="192.168.17.106",
        sip="192.168.17.102",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=interfaces_list[7],  # NIC 3 Rx
        ip="192.168.17.107",
        sip="192.168.17.103",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=interfaces_list[1],  # NIC 0 Rx
        ip="192.168.17.108",
        sip="192.168.17.104",
        video_format=video_format,
        pg_format=video_file["format"],
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
