# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Shared fixtures for native_af_xdp tests.

native_af_xdp tests need real kernel network interfaces (eth2/eth3 by default)
that are administratively up and have IPv4 addresses configured.  Many CI
runners lack these, in which case MTL aborts with
``mt_socket_get_if_ip SIOCGIFADDR fail`` and the test produces an opaque rc=244
failure.  Skip cleanly instead.
"""
from __future__ import annotations

import pytest

XDP_INTERFACES = ("eth2", "eth3")


@pytest.fixture(scope="module", autouse=True)
def require_xdp_interfaces(hosts):
    host = list(hosts.values())[0]
    missing = []
    for ifname in XDP_INTERFACES:
        result = host.connection.execute_command(
            f"ip -o -4 addr show dev {ifname}",
            shell=True,
            expected_return_codes=None,
        )
        if result.return_code != 0 or not result.stdout.strip():
            missing.append(ifname)
    if missing:
        pytest.skip(
            f"native_af_xdp interfaces unavailable on host: {', '.join(missing)} "
            "(no IPv4 address or interface not present)"
        )
