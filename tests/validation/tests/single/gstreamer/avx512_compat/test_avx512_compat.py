# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer plugin AVX-512 compatibility.

Regression test for the crash described in ``.github/avx512_crash.md``:
DPDK built with an ISA baseline wider than the target CPU bakes AVX-512
instructions into ELF constructors (e.g. ``dummy_queue_init`` in
``librte_ethdev.so``) that run unconditionally at ``dlopen()`` time,
crashing with SIGILL on hosts/VMs that lack AVX-512 -- with no way for
MTL's own runtime SIMD dispatch to intervene, since the crash happens
before any MTL code runs.

The non-AVX-512 host is emulated locally with ``qemu-x86_64-static`` using
``QEMU_CPU=Skylake-Client`` (AVX2, no AVX-512; also satisfies glibc's
x86-64-v2 baseline, unlike older QEMU CPU models such as ``qemu64``).
"""

import pytest
from mtl_engine.execute import log_fail

GSTREAMER_PLUGINS = [
    "libgstmtl_st20p_rx.so",
    "libgstmtl_st20p_tx.so",
    "libgstmtl_st30p_rx.so",
    "libgstmtl_st30p_tx.so",
    "libgstmtl_st40p_rx.so",
    "libgstmtl_st40p_tx.so",
    "libgstmtl_st40_rx.so",
]


@pytest.mark.smoke
@pytest.mark.parametrize("plugin", GSTREAMER_PLUGINS)
def test_gstreamer_plugin_avx512_compat(hosts, mtl_path, qemu_x86_64_static, plugin):
    host = list(hosts.values())[0]
    plugin_path = f"{mtl_path}/.local_install/gstreamer/gstreamer-1.0/{plugin}"
    ld_library_path = (
        f"{mtl_path}/.local_install/mtl/lib64:" f"{mtl_path}/.local_install/dpdk/lib64"
    )

    if (
        host.connection.execute_command(
            f"test -f {plugin_path}", shell=True, expected_return_codes=None
        ).return_code
        != 0
    ):
        pytest.skip(f"GStreamer plugin not built: {plugin_path}")

    native = host.connection.execute_command(
        f"LD_LIBRARY_PATH={ld_library_path} gst-inspect-1.0 {plugin_path}",
        shell=True,
        expected_return_codes=None,
    )
    if native.return_code != 0:
        log_fail(f"Native gst-inspect-1.0 failed for {plugin}: {native.stdout}")
    assert native.return_code == 0, f"Native load of {plugin} failed unexpectedly"

    emulated = host.connection.execute_command(
        f"LD_LIBRARY_PATH={ld_library_path} QEMU_CPU=Skylake-Client "
        f"{qemu_x86_64_static} $(command -v gst-inspect-1.0) {plugin_path}",
        shell=True,
        expected_return_codes=None,
    )
    if emulated.return_code != 0:
        log_fail(
            f"{plugin} crashed under emulated non-AVX-512 CPU "
            f"(rc={emulated.return_code}): {emulated.stdout}"
        )
    assert emulated.return_code == 0, (
        f"{plugin} must load on a non-AVX-512 CPU without SIGILL "
        f"(rc={emulated.return_code}, expected 132 before the DPDK ISA-baseline fix)"
    )
