# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Performance-test specific fixtures.

CPU governor and isolation checks live here so they only run for tests
marked ``performance``, not for the broader nightly suite that also
depends on ``nic_port_list``.
"""

import pytest
from common.host_setup import check_cpu_isolation, ensure_cpu_performance_governor


@pytest.fixture(scope="session", autouse=True)
def performance_host_setup(hosts: dict) -> None:
    """One-time host tuning for performance tests only."""
    for host in hosts.values():
        ensure_cpu_performance_governor(host)
        check_cpu_isolation(host)
