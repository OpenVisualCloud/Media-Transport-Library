# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.

import os
import pytest

import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.execute import log_info, log_result_note
from tests.Engine.media_files import yuv_files


@pytest.mark.parametrize(
    "video_format",
    ["i1080p29", "i1080p50", "i1080p59", "i2160p29", "i2160p50", "i2160p59", "i4320p29", "i4320p50", "i4320p59"],
)
def test_perf_2tx_2rx_4nics(build, media, nic_port_list, test_time, video_format):
    # For 4 NICs init time of the app is increased
    test_time = 60

    video_file = yuv_files[video_format]

    config = rxtxapp.create_empty_performance_config()
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[0],  # from NIC 0 to NIC 1
        ip="192.168.17.101",
        dip="192.168.17.105",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[1],  # from NIC 1 to NIC 2
        ip="192.168.17.102",
        dip="192.168.17.106",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[2],  # from NIC 2 to NIC 3
        ip="192.168.17.103",
        dip="192.168.17.107",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_tx(
        config=config,
        nic_port=nic_port_list[3],  # from NIC 3 to NIC 0
        ip="192.168.17.104",
        dip="192.168.17.108",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[4],
        ip="192.168.17.105",
        sip="192.168.17.101",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[5],
        ip="192.168.17.106",
        sip="192.168.17.102",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[6],
        ip="192.168.17.107",
        sip="192.168.17.103",
        video_format=video_format,
        pg_format=video_file["format"],
    )
    config = rxtxapp.add_perf_video_session_rx(
        config=config,
        nic_port=nic_port_list[7],
        ip="192.168.17.108",
        sip="192.168.17.104",
        video_format=video_format,
        pg_format=video_file["format"],
    )

    # upper bound
    replicas_b = 1

    # find upper bound
    while True:
        config = rxtxapp.change_replicas(config=config, session_type="video", replicas=replicas_b, rx=False)
        passed = rxtxapp.execute_perf_test(config=config, build=build, test_time=test_time, fail_on_error=False)

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

        config = rxtxapp.change_replicas(config=config, session_type="video", replicas=replicas_midpoint, rx=False)
        passed = rxtxapp.execute_perf_test(config=config, build=build, test_time=test_time, fail_on_error=False)

        if passed:
            log_info(f"{__name__} {video_format} passed with {replicas_midpoint} replicas")
            replicas_a = replicas_midpoint
        else:
            log_info(f"{__name__} {video_format} failed with {replicas_midpoint} replicas")
            replicas_b = replicas_midpoint
