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
def test_perf_1tx_1nic_1port(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    video_format,
    test_config,
    prepare_ramdisk,
):
    video_file = yuv_files[video_format]
    host = list(hosts.values())[0]

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_st20p_session_tx(
        config=config,
        nic_port=host.vfs[0],
        ip="192.168.17.101",
        dip="239.168.48.9",
        width=video_file["width"],
        height=video_file["height"],
        fps=f"p{video_file['fps']}",
        input_format=video_file["file_format"],
        transport_format=video_file["format"],
        st20p_url=os.path.join(media, video_file["filename"]),
    )

    # Initialize logging for the test
    rxtxapp.init_test_logging()
    rxtxapp.log_to_file(f"Starting performance test for {video_format}", host, build)

    # upper bound
    replicas_b = 1

    # find upper bound
    rxtxapp.log_to_file("Finding upper bound - starting replica testing", host, build)
    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="st20p", replicas=replicas_b
        )
        rxtxapp.log_to_file(
            f"Testing {video_format} with {replicas_b} replicas", host, build
        )

        capture_cfg = dict(test_config.get("capture_cfg", {}))
        capture_cfg["test_name"] = (
            f"test_perf_1tx_1nic_1port_upper_{video_format}_{replicas_b}"
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
        # If we only tested 1 replica and it failed, log and exit
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
            config=config, session_type="st20p", replicas=replicas_midpoint
        )
        rxtxapp.log_to_file(
            f"Binary search: testing {video_format} with {replicas_midpoint} replicas",
            host,
            build,
        )

        capture_cfg = dict(test_config.get("capture_cfg", {})) if test_config else {}
        capture_cfg["test_name"] = (
            f"test_perf_1tx_1nic_1port_search_{video_format}_{replicas_midpoint}"
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
