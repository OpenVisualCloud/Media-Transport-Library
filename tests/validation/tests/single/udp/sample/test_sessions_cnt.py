# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import pytest
from tests.Engine import udp_app


@pytest.mark.parametrize("sessions_cnt", [1, 2, 5, 7])
def test_udp_sessions_cnt(build, nic_port_list, test_time, sessions_cnt):
    udp_app.execute_test_sample(
        build=build,
        nic_port_list=nic_port_list,
        test_time=test_time,
        sessions_cnt=sessions_cnt,
    )
