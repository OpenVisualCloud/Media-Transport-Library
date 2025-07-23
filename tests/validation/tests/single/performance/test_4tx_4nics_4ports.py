# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import mtl_engine.RxTxApp as rxtxapp
import pytest
from mtl_engine.execute import log_info, log_result_note
from mtl_engine.media_files import yuv_files


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
    build,
    media,
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

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[0],
        ip="192.168.17.101",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[1],
        ip="192.168.17.102",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[2],
        ip="192.168.17.103",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[3],
        ip="192.168.17.104",
        dip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )

    # Initialize logging for the test
    rxtxapp.init_test_logging()
    rxtxapp.log_to_file(
        f"Starting 4TX performance test for {video_format}", host, build
    )

    # upper bound
    replicas_b = 1

    # find upper bound
    rxtxapp.log_to_file("Finding upper bound - starting replica testing", host, build)
    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_b, rx=False
        )
        rxtxapp.log_to_file(
            f"Testing {video_format} with {replicas_b} replicas", host, build
        )

        capture_cfg = dict(test_config.get("capture_cfg", {}))
        capture_cfg["test_name"] = (
            f"test_perf_4tx_4nics_4ports_upper_{video_format}_{replicas_b}"
        )
        log_info(f"capture_cfg for upper bound: {capture_cfg}")

        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=build,
                test_time=test_time,
                host=host,
                fail_on_error=False,
                capture_cfg=capture_cfg,
            )
        except Exception as e:
            log_info(
                f"Exception occurred during performance test with {replicas_b} replicas: {e}"
            )
            rxtxapp.log_to_file(
                f"Exception occurred during performance test with {replicas_b} replicas: {e}",
                host,
                build,
            )
            passed = False

        if passed:
            log_info(f"{__name__} {video_format} passed with {replicas_b} replicas")
            rxtxapp.log_to_file(
                f"{video_format} passed with {replicas_b} replicas", host, build
            )
            replicas_b *= 2
        else:
            log_info(f"{__name__} {video_format} failed with {replicas_b} replicas")
            rxtxapp.log_to_file(
                f"{video_format} failed with {replicas_b} replicas - found upper bound",
                host,
                build,
            )
            rxtxapp.log_to_file(
                "Failure reason: Test returned False, check RxTxApp output above for details",
                host,
                build,
            )
            break

    # lower bound
    replicas_a = round(replicas_b / 2)
    if replicas_a == 0:
        log_info(
            f"{__name__} {video_format} finished with 0 replicas (no successful runs)"
        )
        log_result_note("0 replicas")
        rxtxapp.log_to_file(
            f"Performance test completed: {video_format} finished with 0 replicas",
            host,
            build,
        )
        return

    rxtxapp.log_to_file(
        f"Starting binary search between {replicas_a} and {replicas_b}", host, build
    )

    # find maximum number of replicas
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)

        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            log_info(f"{__name__} {video_format} finished with {replicas_a} replicas")
            log_result_note(f"{replicas_a} replicas")
            rxtxapp.log_to_file(
                f"Performance test completed: {video_format} finished with {replicas_a} replicas",
                host,
                build,
            )
            break

        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_midpoint, rx=False
        )
        rxtxapp.log_to_file(
            f"Binary search: testing {video_format} with {replicas_midpoint} replicas",
            host,
            build,
        )

        capture_cfg = dict(test_config.get("capture_cfg", {}))
        capture_cfg["test_name"] = (
            f"test_perf_4tx_4nics_4ports_search_{video_format}_{replicas_midpoint}"
        )
        log_info(f"capture_cfg for binary search: {capture_cfg}")

        try:
            passed = rxtxapp.execute_perf_test(
                config=config,
                build=build,
                test_time=test_time,
                host=host,
                fail_on_error=False,
                capture_cfg=capture_cfg,
            )
        except Exception as e:
            log_info(
                f"Exception occurred during binary search with {replicas_midpoint} replicas: {e}"
            )
            rxtxapp.log_to_file(
                f"Exception occurred during binary search with {replicas_midpoint} replicas: {e}",
                host,
                build,
            )
            passed = False

        if passed:
            log_info(
                f"{__name__} {video_format} passed with {replicas_midpoint} replicas"
            )
            rxtxapp.log_to_file(
                f"{video_format} passed with {replicas_midpoint} replicas", host, build
            )
            replicas_a = replicas_midpoint
        else:
            log_info(
                f"{__name__} {video_format} failed with {replicas_midpoint} replicas"
            )
            rxtxapp.log_to_file(
                f"{video_format} failed with {replicas_midpoint} replicas", host, build
            )
            rxtxapp.log_to_file(
                "Binary search failure reason: Test returned False, check RxTxApp output above for details",
                host,
                build,
            )
            replicas_b = replicas_midpoint
