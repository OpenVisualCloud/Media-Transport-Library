#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
MCP Server for preparing a host to run MTL's tests/acceptance/ pytest suite.

Split out from mtl_mcp_server.py (the system-wide/gtest server) so that any
caller's tool-schema footprint for pytest-environment prep only covers the
handful of tools it actually needs, instead of a `mtl-system-setup/*`
wildcard pulling in ~30 tools' worth of unrelated (VF/driver/gtest) schemas
on every turn. Shared build/summarization logic lives in mtl_setup_common.py
so neither server duplicates it.

tests/acceptance/mtl_engine/const.py hardcodes PREFIX = ".local_install" —
every app path the pytest framework invokes (RxTxApp, MtlManager, ffmpeg,
gstreamer) resolves under <repo>/.local_install/{mtl,ffmpeg,gstreamer}/...,
a tree entirely separate from the system-wide build/ + /usr/local install
mtl_mcp_server.py's build_mtl/dpdk_build produce for gtest/KahawaiTest.

Usage:
    pip install -r requirements.txt
    python mtl_acceptance_mcp_server.py
"""

from __future__ import annotations

import textwrap

from mcp.server.fastmcp import FastMCP
from mtl_setup_common import REPO_ROOT, _run_rc, _summarize_output

ACCEPTANCE_SETUP_BASE_SH = (
    REPO_ROOT / ".github" / "scripts" / "acceptance_setup_base.sh"
)
ACCEPTANCE_SETUP_SH = REPO_ROOT / ".github" / "scripts" / "acceptance_setup.sh"
ACCEPTANCE_DISCOVER_LIB = (
    REPO_ROOT / ".github" / "scripts" / "lib" / "mtl_acceptance_discover.sh"
)

mcp = FastMCP(
    "mtl-acceptance-setup",
    instructions=textwrap.dedent(
        """\
        MTL Acceptance Tests Setup MCP Server — takes a host to "ready to run
        tests/acceptance/tests/single/ pytest".

        All setup logic lives in standalone, human-runnable scripts under
        .github/scripts/ (acceptance_setup.sh, acceptance_setup_base.sh,
        lib/mtl_acceptance_discover.sh, lib/mtl_host_common.sh) — this
        server is a thin wrapper so a human can do the same setup by hand,
        with no AI/MCP involved, via `.github/scripts/acceptance_setup.sh`.

        Common workflow:
        • Discover host state first: discover_status() (full report) or
          discover_pfs() (NIC PF table only) — read-only, no changes made.
        • Clean or partially-prepared host: setup_acceptance_tests_full(nfs_source=...,
          pf_bdf=...) — one-shot broad host setup (apt/DPDK/ICE/MTL/hugepages/
          CPU governor/plugins into .local_install) + pytest-specific setup
          (NFS media/localhost-root-SSH/venv/configs).
        • Re-run either phase alone: setup_acceptance_tests_base / setup_acceptance_tests_pytest.
        • EBU LIST pcap compliance checking (optional, ask the human first):
          pass ebu_ip/ebu_user/ebu_password + capture_pci_device (a 2nd NIC
          PF) to setup_acceptance_tests_full / setup_acceptance_tests_pytest. Without
          these, test_config.yaml's `compliance` stays false and the
          `pcap_capture` fixture only skips (no capture, no EBU upload).
        """
    ),
)


@mcp.tool()
def discover_status() -> str:
    """
    Read-only discovery report: kernel/CPU, hugepages, CPU governor, ICE
    driver, candidate NIC PFs, .local_install build artifacts, NFS media,
    and pytest venv/configs.

    Thin wrapper around `acceptance_setup.sh status` (same report a human
    gets running that script by hand, or `bash
    .github/scripts/lib/mtl_acceptance_discover.sh`). Call this first —
    before `setup_acceptance_tests_base`/`setup_acceptance_tests_pytest` — to decide
    what's actually missing, and to pick a `pf_bdf` for
    `setup_acceptance_tests_pytest`/`setup_acceptance_tests_full`.
    """
    rc, out = _run_rc(f"bash {ACCEPTANCE_SETUP_SH} status")
    return out if rc == 0 else f"Error (exit {rc}):\n{out}"


@mcp.tool()
def discover_pfs() -> str:
    """
    List candidate Intel E810/E830/E825/E835 NIC PFs for `pf_bdf`/
    `capture_pci_device` (BDF, interface name, driver, NUMA node, SR-IOV
    VF counts). Ask the human which one to use whenever more than one is
    listed — never guess.
    """
    rc, out = _run_rc(f"source {ACCEPTANCE_DISCOVER_LIB} && vd_report_pfs")
    return out if rc == 0 else f"Error (exit {rc}):\n{out}"


@mcp.tool()
def setup_acceptance_tests_base(
    nr_hugepages: int = 2048,
    build_mode: str = "release",
    include_ffmpeg_plugin: bool = False,
    include_gstreamer_plugin: bool = False,
) -> str:
    """
    One-shot broad host setup for acceptance_tests environments.

    IMPORTANT: tests/acceptance/mtl_engine/const.py hardcodes
    PREFIX = ".local_install" — every app path (RxTxApp, MtlManager, ffmpeg,
    gstreamer) the pytest framework invokes is resolved under
    ``<repo>/.local_install/{mtl,ffmpeg,gstreamer}/...``. This is a SEPARATE,
    parallel install tree from the system-wide one used by gtest/KahawaiTest
    (top-level ``build/`` + ``/usr/local``, produced by the `mtl-system-setup`
    server's `build_mtl` / `dpdk_build` tools). Building system-wide only is
    NOT sufficient for pytest — MtlManager/RxTxApp/ffmpeg will be reported
    "not found" even though `build_mtl`/`dpdk_build` succeeded. This tool
    builds the `.local_install` tree; it does not touch or replace the
    system-wide one.

    This covers non-pytest-specific responsibilities and is intended to replace
    generic setup logic in setup_acceptance.sh:
    - apt dependencies
    - DPDK build/install (into .local_install/dpdk)
    - ICE driver status/rebuild (system-wide — it's a kernel module, no prefix)
    - MTL build (into .local_install/mtl)
    - hugepages
    - CPU governor performance + confirmation
    - optional FFmpeg/GStreamer plugin builds (into .local_install/{ffmpeg,gstreamer})

    Pytest-specific setup (NFS media mount, localhost root SSH, acceptance_tests venv,
    topology/test config generation) remains in setup_acceptance.sh.

    Args:
        nr_hugepages: Number of 2MB hugepages (default 2048)
        build_mode: release/debug (default release; "debugonly" is treated as
            "debug" here — the ASAN/no-ASAN distinction only matters for
            gtest's MTL_SIMULATE_PACKET_DROPS, not for pytest app behavior)
        include_ffmpeg_plugin: Build FFmpeg plugin (default False)
        include_gstreamer_plugin: Build GStreamer plugin (default False)
    """
    if build_mode not in ("release", "debug", "debugonly"):
        return (
            "Error: build_mode must be one of release/debug/debugonly. "
            f"Got '{build_mode}'."
        )

    local_prefix = REPO_ROOT / ".local_install" / "mtl"
    # DPDK (2-4 min) + MTL (1-3 min); +ffmpeg (~5-10 min) / +gstreamer if enabled;
    # +apt/ICE rebuild if needed.
    build_timeout = (
        1800
        + (900 if include_ffmpeg_plugin else 0)
        + (600 if include_gstreamer_plugin else 0)
    )
    env = {
        "NR_HUGEPAGES": str(nr_hugepages),
        "BUILD_MODE": build_mode,
        "INCLUDE_FFMPEG_PLUGIN": "1" if include_ffmpeg_plugin else "0",
        "INCLUDE_GSTREAMER_PLUGIN": "1" if include_gstreamer_plugin else "0",
        "LOCAL_PREFIX": str(local_prefix),
    }
    # This tool is now a thin wrapper around acceptance_setup_base.sh, the
    # standalone script a human can run by hand for the same broad-setup
    # phase (apt/ICE/DPDK/MTL/hugepages/governor) — see that script's header
    # for the stage-by-stage breakdown. Single implementation, two callers.
    rc, out = _run_rc(
        f"bash {ACCEPTANCE_SETUP_BASE_SH}",
        env=env,
        timeout=build_timeout,
    )
    manager_ok = (local_prefix / "bin" / "MtlManager").is_file()
    rxtxapp_ok = (local_prefix / "bin" / "RxTxApp").is_file()
    # The artifact check is the real signal here (not the script's exit code
    # alone) — a build stage can be merely skipped, but if the binaries
    # pytest needs aren't there, this step failed for our purposes.
    build_rc = rc if (manager_ok and rxtxapp_ok) else 1

    result = (
        f"## Broad Acceptance Tests Host Setup\n{_summarize_output('setup_acceptance_tests_base', out, rc=build_rc)}"
        f"\n\n### Artifacts\n- {local_prefix}/bin/MtlManager: "
        + ("OK" if manager_ok else "MISSING")
        + f"\n- {local_prefix}/bin/RxTxApp: "
        + ("OK" if rxtxapp_ok else "MISSING")
        + "\n\n## Next Step\n"
        "Run `setup_acceptance_tests_pytest` (NFS/SSH/venv/config stages), or call "
        "`setup_acceptance_tests_full` next time to do both in one shot."
    )
    return result


@mcp.tool()
def setup_acceptance_tests_pytest(
    nfs_source: str = "",
    pf_bdf: str = "",
    test_time: int = 30,
    nfs_persist: bool = False,
    ebu_ip: str = "",
    ebu_user: str = "",
    ebu_password: str = "",
    capture_pci_device: str = "",
) -> str:
    """
    Pytest-specific acceptance_tests setup: NFS media, localhost root SSH, venv, configs.

    Thin wrapper around `.github/scripts/setup_acceptance.sh` (idempotent —
    safe to re-run). Does NOT build anything — call `setup_acceptance_tests_base`
    (or `setup_acceptance_tests_full`) first if `.local_install/mtl/bin/{MtlManager,
    RxTxApp}` don't exist yet.

    Args:
        nfs_source: `host:/export` for the media NFS share, e.g.
            '10.123.232.121:/mnt/NFS/mtl_assets/media'. REQUIRED unless
            /mnt/media is already mounted or already contains local files —
            the caller must ask the human for this value; never guess or
            assume a default (every host has a different storage server).
        pf_bdf: NIC PF BDF for the generated topology config, e.g.
            '0000:c9:00.0'. Auto-picked (first Intel E810/E830/E835 PF) if
            empty and there is only one candidate; if multiple PFs are
            present, ask the human which one to use. May be a comma-
            separated list of multiple DUT PF candidates (e.g.
            '0000:c9:00.0,0000:c9:00.1' — both ports of a second card) when
            a PF-mode test needs more than one PF candidate that avoids the
            capture NIC's IOMMU group; see capture_pci_device.
        test_time: test_config.yaml::test_time in seconds (default 30).
        nfs_persist: When True, also add an /etc/fstab entry so the NFS
            mount survives a reboot.
        ebu_ip: EBU LIST server IP for PCAP compliance analysis (the
            `pcap_capture` fixture's teardown upload/verdict step). OPTIONAL
            — only pass this if the human explicitly asked for compliance/
            EBU checking; never assume or guess an EBU server, ask via
            askQuestions. Requires ebu_user and ebu_password too.
        ebu_user: EBU LIST server username. Required together with ebu_ip.
        ebu_password: EBU LIST server password. Required together with
            ebu_ip. Passed through the subprocess environment, never placed
            on a command line, so it won't leak into `ps` output.
        capture_pci_device: a SECOND NIC PF BDF, different from `pf_bdf`
            (e.g. '0000:15:00.1' when pf_bdf is '0000:15:00.0' — two ports
            of the same or a different card), used for netsniff-ng packet
            capture. Compliance checking needs this in addition to
            ebu_ip/ebu_user/ebu_password — without a second PF there's no
            wire capture, and "compliance" in test_config.yaml stays false
            even with valid EBU credentials. Ask the human for this only
            when they want compliance checking; probe available PFs first
            (`nic_discover_pfs`/`system_status`) so you can suggest one.
            Kept as a dedicated BDF separate from `pf_bdf`'s DUT PF
            candidate(s) — a PF-mode DUT test that also wants compliance
            needs a PF candidate not sharing an IOMMU group/physical card
            with this value; if the host has two 2-port cards, prefer
            passing both ports of one card as `pf_bdf` and one port of the
            other card as `capture_pci_device` so PF-mode tests requiring 2
            isolated DUT PFs (e.g. `get_test_interfaces("PF", count=2)`)
            have a candidate pair to pick from.
    """
    env = {
        "TEST_TIME": str(test_time),
        "NFS_PERSIST": "1" if nfs_persist else "0",
    }
    if nfs_source:
        env["NFS_SOURCE"] = nfs_source
    if pf_bdf:
        env["PCI_DEVICE_BDF"] = pf_bdf
    if ebu_ip:
        env["EBU_IP"] = ebu_ip
        env["EBU_USER"] = ebu_user
        env["EBU_PASSWORD"] = ebu_password
    if capture_pci_device:
        env["CAPTURE_PCI_DEVICE"] = capture_pci_device
    # Pass secrets/inputs through the subprocess environment (not an inline
    # "VAR=val bash script.sh" command string) so EBU_PASSWORD never appears
    # in the executed command line / process listing.
    rc, out = _run_rc(
        "bash .github/scripts/setup_acceptance.sh",
        env=env,
        timeout=300,
    )
    summary = _summarize_output(
        "setup_acceptance_tests_pytest", out, tail_lines=60, rc=rc
    )
    return f"## Pytest Acceptance Tests Setup\n{summary}"


@mcp.tool()
def setup_acceptance_tests_full(
    nfs_source: str = "",
    pf_bdf: str = "",
    nr_hugepages: int = 2048,
    build_mode: str = "release",
    include_ffmpeg_plugin: bool = True,
    include_gstreamer_plugin: bool = False,
    test_time: int = 30,
    ebu_ip: str = "",
    ebu_user: str = "",
    ebu_password: str = "",
    capture_pci_device: str = "",
) -> str:
    """
    One-shot: take a clean host to "ready to run tests/acceptance/ pytest".

    Runs `setup_acceptance_tests_base` (apt, DPDK, ICE, MTL, hugepages, CPU governor
    — all into .local_install/mtl, plus optional ffmpeg/gstreamer plugins)
    then `setup_acceptance_tests_pytest` (NFS, SSH, venv, configs). Idempotent —
    safe to re-run on an already-prepared host (fast no-op on each stage).

    `include_ffmpeg_plugin` defaults to True here (unlike `setup_acceptance_tests_base`)
    because most test_single/st20p/... tests parametrize over both
    application=rxtxapp and application=ffmpeg.

    Args:
        nfs_source: `host:/export` for the media NFS share. REQUIRED unless
            /mnt/media is already mounted — ASK the human for this, never
            assume a default.
        pf_bdf: NIC PF BDF for the topology config. Ask the human if there
            are multiple candidate PFs.
        nr_hugepages: Number of 2MB hugepages (default 2048).
        build_mode: release/debug/debugonly (default release).
        include_ffmpeg_plugin: Build FFmpeg plugin into .local_install (default True).
        include_gstreamer_plugin: Build GStreamer plugin into .local_install (default False).
        test_time: test_config.yaml::test_time in seconds (default 30).
        ebu_ip: EBU LIST server IP for PCAP compliance analysis. OPTIONAL —
            only pass this if the human explicitly asked for compliance/EBU
            checking; never assume or guess a server, ask via askQuestions.
            Requires ebu_user and ebu_password too.
        ebu_user: EBU LIST server username. Required together with ebu_ip.
        ebu_password: EBU LIST server password. Required together with
            ebu_ip. Passed through the subprocess environment, never a
            command line, so it won't leak into `ps` output.
        capture_pci_device: a SECOND NIC PF BDF (different physical PF than
            pf_bdf) used for netsniff-ng packet capture. Compliance checking
            needs this in addition to the ebu_* args — without it,
            "compliance" stays false even with valid EBU credentials. See
            `setup_acceptance_tests_pytest`'s docstring for the multi-PF-DUT +
            dedicated-capture-NIC topology this enables.
    """
    results = [
        "## Phase 1/2: Broad host setup\n"
        + setup_acceptance_tests_base(
            nr_hugepages=nr_hugepages,
            build_mode=build_mode,
            include_ffmpeg_plugin=include_ffmpeg_plugin,
            include_gstreamer_plugin=include_gstreamer_plugin,
        )
    ]
    results.append(
        "## Phase 2/2: Pytest-specific setup\n"
        + setup_acceptance_tests_pytest(
            nfs_source=nfs_source,
            pf_bdf=pf_bdf,
            test_time=test_time,
            ebu_ip=ebu_ip,
            ebu_user=ebu_user,
            ebu_password=ebu_password,
            capture_pci_device=capture_pci_device,
        )
    )
    results.append(
        "## Ready\n"
        "Run e.g.:\n```\ncd tests/acceptance && sudo -E ./venv/bin/python3 -m pytest "
        "--topology_config=configs/topology_config.yaml "
        "--test_config=configs/test_config.yaml "
        "tests/single/st20p/test_input_formats.py --tb=short -v\n```"
    )
    return "\n\n---\n\n".join(results)


@mcp.tool()
def build_ffmpeg_plugin() -> str:
    """Build the MTL FFmpeg plugin (ecosystem/ffmpeg_plugin) into .local_install."""
    build_sh = REPO_ROOT / "ecosystem/ffmpeg_plugin/build.sh"
    if not build_sh.is_file():
        return "Error: ecosystem/ffmpeg_plugin/build.sh not found"

    # Without MTL_INSTALL_PREFIX, setup_environment.sh falls back to
    # `sudo make install` (system-wide), leaving .local_install/ffmpeg --
    # the binary pytest actually invokes -- stale.
    local_prefix = REPO_ROOT / ".local_install" / "mtl"
    rc, out = _run_rc(
        "ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN=1 "
        "SETUP_ENVIRONMENT=0 SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 MTL_BUILD_AND_INSTALL=0 "
        f"MTL_INSTALL_PREFIX={local_prefix} "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    ffmpeg_ok = (REPO_ROOT / ".local_install/ffmpeg/bin/ffmpeg").is_file()
    build_rc = rc if ffmpeg_ok else 1
    return f"## FFmpeg Plugin Build\n{_summarize_output('ffmpeg_plugin_build', out, rc=build_rc)}"


@mcp.tool()
def build_gstreamer_plugin() -> str:
    """Build the MTL GStreamer plugin (ecosystem/gstreamer_plugin) into .local_install."""
    build_sh = REPO_ROOT / "ecosystem/gstreamer_plugin/build.sh"
    if not build_sh.is_file():
        return "Error: ecosystem/gstreamer_plugin/build.sh not found"

    # See build_ffmpeg_plugin() above -- same MTL_INSTALL_PREFIX requirement.
    local_prefix = REPO_ROOT / ".local_install" / "mtl"
    rc, out = _run_rc(
        "ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=1 "
        "SETUP_ENVIRONMENT=0 SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 MTL_BUILD_AND_INSTALL=0 "
        f"MTL_INSTALL_PREFIX={local_prefix} "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    return f"## GStreamer Plugin Build\n{_summarize_output('gstreamer_plugin_build', out, rc=rc)}"


if __name__ == "__main__":
    mcp.run(transport="stdio")
