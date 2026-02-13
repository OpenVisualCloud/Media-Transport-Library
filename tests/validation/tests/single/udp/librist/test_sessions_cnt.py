# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import udp_app


@pytest.mark.parametrize(
    "sleep_us, sleep_step, sessions_cnt",
    [
        (1, 3, 1),
        (1, 3, 2),
        (1, 3, 5),
        (1, 3, 7),
        (1, 3, 8),
        (10, 1, 9),
        (10, 1, 15),
        (10, 1, 20),
        (10, 1, 25),
        (10, 1, 28),
    ],
)
def test_udp_sessions_cnt(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    sleep_us,
    sleep_step,
    sessions_cnt,
    test_config,
    prepare_ramdisk,
):
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    udp_app.execute_test_librist(
        build=mtl_path,
        nic_port_list=interfaces_list,
        test_time=test_time,
        sleep_us=sleep_us,
        sleep_step=sleep_step,
        sessions_cnt=sessions_cnt,
        host=host,
    )
