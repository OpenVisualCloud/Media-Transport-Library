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
