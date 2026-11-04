# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Performance-test specific fixtures.

CPU governor, turbo and isolation checks live here so they only run for tests
marked ``performance``, not for the broader nightly suite that also
depends on ``nic_port_list``.
"""

import pytest
from common.host_setup import (
    check_cpu_isolation,
    check_cpu_performance_governor,
    check_turbo_boost_enabled,
)


@pytest.fixture(scope="session", autouse=True)
def performance_host_setup(hosts: dict) -> None:
    """Check the host settings that performance tests depend on."""
    for host in hosts.values():
        check_cpu_performance_governor(host)
        check_turbo_boost_enabled(host)
        check_cpu_isolation(host)
