# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Shared fixtures for avx512_compat tests.

These tests emulate a non-AVX-512 host via ``qemu-x86_64-static`` to catch
init-time SIGILL crashes caused by DPDK being built with an ISA baseline
that is wider than the CPU it eventually runs on (see
``.github/avx512_crash.md``). qemu-user is an optional resource: skip
cleanly instead of failing the suite when it cannot be made available.
"""
from __future__ import annotations

import pytest

QEMU_CACHE_PATH = "/tmp/mtl_validation_qemu-x86_64-static"
QEMU_DOWNLOAD_URL = (
    "https://github.com/multiarch/qemu-user-static/releases/download/"
    "v7.2.0-1/qemu-x86_64-static"
)


@pytest.fixture(scope="session")
def qemu_x86_64_static(hosts):
    """Return a path to a runnable ``qemu-x86_64-static`` binary, or skip."""
    host = list(hosts.values())[0]

    found = host.connection.execute_command(
        "command -v qemu-x86_64-static", shell=True, expected_return_codes=None
    )
    if found.return_code == 0 and found.stdout.strip():
        return found.stdout.strip()

    cached = host.connection.execute_command(
        f"test -x {QEMU_CACHE_PATH}", shell=True, expected_return_codes=None
    )
    if cached.return_code == 0:
        return QEMU_CACHE_PATH

    downloaded = host.connection.execute_command(
        f"curl -fsSL -o {QEMU_CACHE_PATH} {QEMU_DOWNLOAD_URL} "
        f"&& chmod +x {QEMU_CACHE_PATH}",
        shell=True,
        expected_return_codes=None,
    )
    if downloaded.return_code == 0:
        return QEMU_CACHE_PATH

    pytest.skip(
        "qemu-x86_64-static unavailable (not on PATH and download failed) - "
        "install the 'qemu-user-static' package or see .github/avx512_crash.md "
        "for a manual download command"
    )
