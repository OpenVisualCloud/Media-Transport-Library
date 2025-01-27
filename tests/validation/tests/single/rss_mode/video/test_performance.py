# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_info, log_result_note
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize("rss_mode", ["l3_l4", "l3", "none"])
@pytest.mark.parametrize("video_format", ["i1080p60", "i2160p60"])
def test_rss_mode_video_performance(
    build, media, nic_port_list, test_time, rss_mode, video_format
):
    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode="unicast",
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.change_rss_mode(content=config, rss_mode=rss_mode)

    # upper bound
    replicas_b = 1

    # find upper bound
    while True:
        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_b
        )
        passed = rxtxapp.execute_test(
            config=config, build=build, test_time=test_time, fail_on_error=False
        )

        if passed:
            log_info(
                f"test_rss_mode_video_performance passed with {replicas_b} replicas"
            )
            replicas_b *= 2
        else:
            log_info(
                f"test_rss_mode_video_performance failed with {replicas_b} replicas"
            )
            break

    # lower bound
    replicas_a = round(replicas_b / 2)

    # find maximum number of replicas
    while True:
        replicas_midpoint = round((replicas_a + replicas_b) / 2)

        if replicas_midpoint == replicas_a or replicas_midpoint == replicas_b:
            log_info(
                f"test_rss_mode_video_performance finished with {replicas_a} replicas"
            )
            log_result_note(f"{replicas_a} replicas")
            break

        config = rxtxapp.change_replicas(
            config=config, session_type="video", replicas=replicas_midpoint
        )
        passed = rxtxapp.execute_test(
            config=config, build=build, test_time=test_time, fail_on_error=False
        )

        if passed:
            log_info(
                f"test_rss_mode_video_performance passed with {replicas_midpoint} replicas"
            )
            replicas_a = replicas_midpoint
        else:
            log_info(
                f"test_rss_mode_video_performance failed with {replicas_midpoint} replicas"
            )
            replicas_b = replicas_midpoint
