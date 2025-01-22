# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_info, log_result_note
from tests.Engine.media_files import yuv_files


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
def test_perf_2tx_2rx_4nics(build, media, nic_port_list, test_time, video_format):
    # Increase time for 4k and 8k streams
    if "2160" in video_format:
        test_time = 60
    elif "4320" in video_format:
        test_time = 120

    video_file = yuv_files[video_format]
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
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[2],
        ip="192.168.17.103",
        sip="239.168.48.9",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[3],
        ip="192.168.17.104",
        sip="239.168.48.9",
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
        passed = rxtxapp.execute_perf_test(
            config=config, build=build, test_time=test_time, fail_on_error=False
        )

        if passed:
            log_info(f"{__name__} {video_format} passed with {replicas_b} replicas")
            replicas_b *= 2
        else:
            log_info(f"{__name__} {video_format} failed with {replicas_b} replicas")
            break

    # lower bound
    replicas_a = round(replicas_b / 2)

    # find maximum number of replicas
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)

        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            log_info(f"{__name__} {video_format} finished with {replicas_a} replicas")
            log_result_note(f"{replicas_a} replicas")
            break

        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_midpoint, rx=False
        )
        passed = rxtxapp.execute_perf_test(
            config=config, build=build, test_time=test_time, fail_on_error=False
        )

        if passed:
            log_info(
                f"{__name__} {video_format} passed with {replicas_midpoint} replicas"
            )
            replicas_a = replicas_midpoint
        else:
            log_info(
                f"{__name__} {video_format} failed with {replicas_midpoint} replicas"
            )
            replicas_b = replicas_midpoint
