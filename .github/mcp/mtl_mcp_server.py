#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
MCP Server for Media Transport Library (MTL) system setup and management.

Provides tools for configuring DPDK, hugepages, NIC binding, VFs, IOMMU,
ICE driver, MTL builds, and runtime management (MtlManager, PTP).

Usage:
    pip install -r requirements.txt
    python mtl_mcp_server.py
"""

from __future__ import annotations

import os
import re
import textwrap
import time
from pathlib import Path

from mcp.server.fastmcp import FastMCP
from mtl_setup_common import (
    REPO_ROOT,
    _load_versions,
    _run,
    _run_output,
    _run_rc,
    _save_test_log,
    _summarize_output,
)
from mtl_setup_common import (
    cpu_governor_set_and_confirm_performance as _cpu_governor_set_and_confirm_performance,
)
from mtl_setup_common import hugepages_get as _hugepages_get_impl
from mtl_setup_common import hugepages_set as _hugepages_set_impl
from mtl_setup_common import ice_driver_rebuild as _ice_driver_rebuild_impl
from mtl_setup_common import ice_driver_status as _ice_driver_status_impl
from mtl_setup_common import install_dependencies as _install_dependencies_impl

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
NICCTL = REPO_ROOT / "script" / "nicctl.sh"

mcp = FastMCP(
    "mtl-system-setup",
    instructions=textwrap.dedent(
        """\
        MTL System Setup MCP Server — tools for preparing a Linux host to run
        the Media Transport Library (SMPTE ST 2110 over DPDK).

        Common workflows:
        • First-time setup:  system_status → iommu_status → hugepages_set →
                             nic_create_vf → build_mtl → manager_start
        • After reboot:      setup_after_reboot_auto (single step — auto-
                             rebuilds the ICE driver first if needed, e.g.
                             after a kernel upgrade, then does hugepages,
                             VFs, and MtlManager. No separate ice_driver_
                             rebuild call needed in the common case.)
        • Force an ICE driver rebuild without touching hugepages/VFs:
                             ice_driver_rebuild → setup_after_reboot_auto
                             (VFs are destroyed by driver reload!)
        • Run tests:         run_gtest (auto-discovers ports)
        • Build from clean:  mtl_clean_rebuild
        • Switch PF→VF:      nic_bind_kernel → nic_create_vf
        • Switch VF→PF:      nic_destroy_vf → nic_bind_pmd
        • Kernel socket mode: nic_bind_kernel (leave NIC in kernel)
        • Debug crash:        dmesg_tail → ice_driver_status → dpdk_status →
                             log_tail

        Preparing tests/validation/ pytest (a separate .local_install tree)?
        Use the sibling `mtl-validation-setup` MCP server instead.
    """
    ),
)


# ---------------------------------------------------------------------------
# Helpers local to this server (NIC/VF discovery — not needed by the
# validation server, so not shared via mtl_setup_common)
# ---------------------------------------------------------------------------
def _validate_bdf(bdf: str) -> str | None:
    """Return error string if BDF is invalid, else None."""
    if not re.match(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d$", bdf):
        return f"Error: invalid BDF format '{bdf}'. Expected format: 0000:af:00.0"
    return None


def _int_or_default(text: str, default: int = 0) -> int:
    """Parse int from text with a safe default."""
    try:
        return int(text.strip())
    except Exception:
        return default


def _discover_sriov_intel_pfs() -> list[dict[str, str]]:
    """Discover Intel Ethernet PFs that expose SR-IOV controls."""
    pfs: list[dict[str, str]] = []
    for dev in sorted(Path("/sys/bus/pci/devices").glob("*:*:*.*")):
        vendor = (
            (dev / "vendor").read_text().strip() if (dev / "vendor").is_file() else ""
        )
        class_code = (
            (dev / "class").read_text().strip() if (dev / "class").is_file() else ""
        )
        if vendor.lower() != "0x8086" or not class_code.lower().startswith("0x02"):
            continue
        if not (dev / "sriov_totalvfs").is_file():
            continue

        bdf = dev.name
        driver = "N/A"
        if (dev / "driver").exists():
            driver = os.path.basename(os.path.realpath(dev / "driver"))

        iface = "N/A"
        net_dir = dev / "net"
        if net_dir.is_dir():
            names = sorted([n.name for n in net_dir.iterdir()])
            if names:
                iface = names[0]

        total_vfs = "0"
        if (dev / "sriov_totalvfs").is_file():
            total_vfs = (dev / "sriov_totalvfs").read_text().strip()
        num_vfs = "0"
        if (dev / "sriov_numvfs").is_file():
            num_vfs = (dev / "sriov_numvfs").read_text().strip()

        pfs.append(
            {
                "bdf": bdf,
                "driver": driver,
                "iface": iface,
                "sriov_totalvfs": total_vfs,
                "sriov_numvfs": num_vfs,
            }
        )
    return pfs


# ===================================================================
# TOOLS — Status & Diagnostics
# ===================================================================


@mcp.tool()
def system_status() -> str:
    """
    Comprehensive system status report for MTL readiness.

    Checks: IOMMU, hugepages, NICs, drivers, DPDK, MTL, kernel, CPU, VFIO.
    Run this first to understand the current system state.
    """
    sections: list[str] = []

    # Kernel
    uname = _run_output("uname -r")
    cmdline = _run_output("cat /proc/cmdline")
    sections.append(f"## Kernel\n- Version: {uname}\n- Cmdline: {cmdline}")

    # CPU
    nproc = _run_output("nproc")
    numa = _run_output("lscpu | grep 'NUMA node(s)'")
    sections.append(f"## CPU\n- Cores: {nproc}\n- {numa}")

    # IOMMU
    iommu_groups = _run_output(
        "find /sys/kernel/iommu_groups/ -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l"
    )
    iommu_ok = int(iommu_groups) > 0 if iommu_groups.isdigit() else False
    sections.append(
        f"## IOMMU\n- Groups: {iommu_groups}\n- Status: {'ENABLED' if iommu_ok else 'DISABLED — see iommu_status tool'}"
    )

    # Hugepages
    hp_info = _run_output("grep -i huge /proc/meminfo")
    sections.append(f"## Hugepages\n```\n{hp_info}\n```")

    # NICs — lshw for full bus/interface/driver picture
    lshw = _run_output(
        "sudo lshw -c network -businfo 2>/dev/null || echo 'lshw not available'"
    )
    sections.append(f"## Network Devices (lshw)\n```\n{lshw}\n```")

    # dpdk-devbind status — shows driver bindings (vfio-pci / kernel / unbound)
    devbind = _run_output(
        "dpdk-devbind.py -s 2>/dev/null || dpdk-devbind -s 2>/dev/null || echo 'dpdk-devbind not found'"
    )
    sections.append(f"## DPDK Device Bindings (dpdk-devbind)\n```\n{devbind}\n```")

    # ICE driver
    ice_ver = _run_output(
        "modinfo ice 2>/dev/null | grep '^version:' | head -1 || echo 'not loaded'"
    )
    ice_path = _run_output("modinfo -n ice 2>/dev/null || echo 'not found'")
    sections.append(f"## ICE Driver\n- {ice_ver}\n- Path: {ice_path}")

    # DPDK
    dpdk_ver = _run_output(
        "pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'"
    )
    sections.append(f"## DPDK\n- Version: {dpdk_ver}")

    # MTL
    mtl_installed = _run_output(
        "ldconfig -p 2>/dev/null | grep libmtl || echo 'not installed'"
    )
    rxtxapp = (
        "OK"
        if (REPO_ROOT / "tests/tools/RxTxApp/build/RxTxApp").is_file()
        else "NOT BUILT"
    )
    manager = (
        "OK" if (REPO_ROOT / "build/manager/MtlManager").is_file() else "NOT BUILT"
    )
    sections.append(
        f"## MTL\n- libmtl: {mtl_installed}\n- RxTxApp: {rxtxapp}\n- MtlManager: {manager}"
    )

    # VFIO devices
    vfio = _run_output(
        "ls -l /dev/vfio/ 2>/dev/null | head -20 || echo 'no VFIO devices'"
    )
    sections.append(f"## VFIO Devices\n```\n{vfio}\n```")

    # Versions from versions.env
    vers = _load_versions()
    ver_lines = "\n".join(
        f"- {k}: {v}" for k, v in vers.items() if not k.endswith("_REPO")
    )
    sections.append(f"## Expected Versions (versions.env)\n{ver_lines}")

    return "\n\n".join(sections)


# ===================================================================
# TOOLS — IOMMU
# ===================================================================


@mcp.tool()
def iommu_status() -> str:
    """
    Check IOMMU (VT-d) status: kernel cmdline, IOMMU groups, CPU VMX flags.

    If IOMMU is disabled, provides instructions to enable it in BIOS and GRUB.
    """
    sections: list[str] = []

    cmdline = _run_output("cat /proc/cmdline")
    has_iommu = "intel_iommu=on" in cmdline and "iommu=pt" in cmdline
    sections.append(
        f"## Kernel Cmdline\n```\n{cmdline}\n```\n"
        f"- intel_iommu=on: {'YES' if 'intel_iommu=on' in cmdline else 'NO'}\n"
        f"- iommu=pt: {'YES' if 'iommu=pt' in cmdline else 'NO'}"
    )

    groups = _run_output(
        "find /sys/kernel/iommu_groups/ -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l"
    )
    sections.append(f"## IOMMU Groups: {groups}")

    vmx = _run_output("lscpu | grep -i vmx || echo 'VMX not found'")
    sections.append(f"## CPU VMX\n{vmx}")

    if not has_iommu or groups == "0":
        sections.append(
            "## Action Required\n"
            "1. Enable VT-d and VT-x in BIOS\n"
            "2. Add to /etc/default/grub GRUB_CMDLINE_LINUX_DEFAULT:\n"
            "   `intel_iommu=on iommu=pt`\n"
            "3. Run: `sudo update-grub && sudo reboot`"
        )

    return "\n\n".join(sections)


# ===================================================================
# TOOLS — Hugepages
# ===================================================================


@mcp.tool()
def hugepages_get() -> str:
    """Get current hugepage status (total, free, size)."""
    return _hugepages_get_impl()


@mcp.tool()
def hugepages_set(nr_hugepages: int = 2048, size_kb: int = 2048) -> str:
    """
    Configure hugepages. Default: 2048 x 2MB = 4GB.

    Args:
        nr_hugepages: Number of hugepages to allocate (default 2048).
        size_kb: Hugepage size in KB. Use 2048 for 2MB (default) or 1048576 for 1GB pages.
    """
    return _hugepages_set_impl(nr_hugepages, size_kb)


# ===================================================================
# TOOLS — NIC Management
# ===================================================================


@mcp.tool()
def nic_list() -> str:
    """
    List all network devices with driver, NUMA, IOMMU group, and interface name.
    Shows both PFs and any existing VFs.
    """
    # Use nicctl.sh list if available
    if NICCTL.is_file():
        out = _run_output(f"sudo bash {NICCTL} list all", timeout=15)
        if out:
            return f"## NIC List\n```\n{out}\n```"

    # Fallback to dpdk-devbind
    return dpdk_devbind_status()


@mcp.tool()
def nic_discover_pfs() -> str:
    """
    Discover SR-IOV-capable Intel Ethernet PFs suitable for VF creation.

    This is useful after reboot, especially if PFs are bound to vfio-pci
    and normal netdev-based tools show little context.
    """
    pfs = _discover_sriov_intel_pfs()
    if not pfs:
        return "No SR-IOV-capable Intel Ethernet PFs found."

    lines = ["BDF          Driver       IF Name    sriov_totalvfs  sriov_numvfs"]
    lines.append("------------ ------------ ---------- --------------- -------------")
    for pf in pfs:
        lines.append(
            f"{pf['bdf']:<12} {pf['driver']:<12} {pf['iface']:<10} {pf['sriov_totalvfs']:<15} {pf['sriov_numvfs']:<13}"
        )
    return "## Discovered PFs\n```\n" + "\n".join(lines) + "\n```"


@mcp.tool()
def nic_bind_pmd(bdf: str) -> str:
    """
    Bind a NIC (PF) directly to DPDK PMD (vfio-pci).

    Use this for non-E800 series NICs or when you want PF-mode DPDK.
    For E810/E830, prefer nic_create_vf instead.

    Args:
        bdf: PCI Bus:Device.Function, e.g. '0000:af:00.0'
    """
    err = _validate_bdf(bdf)
    if err:
        return err

    out = _run_output(f"sudo bash {NICCTL} bind_pmd {bdf}")
    return f"## Bind PF to DPDK PMD\n{out}"


@mcp.tool()
def nic_bind_kernel(bdf: str) -> str:
    """
    Bind a NIC back to kernel driver (ice/i40e/etc).

    Use this to return a DPDK-bound port to kernel control,
    or for kernel socket transport mode.

    Args:
        bdf: PCI Bus:Device.Function, e.g. '0000:af:00.0'
    """
    err = _validate_bdf(bdf)
    if err:
        return err

    out = _run_output(f"sudo bash {NICCTL} bind_kernel {bdf}")
    return f"## Bind to Kernel Driver\n{out}"


def _ensure_vfio_permissions() -> str:
    """Ensure udev rule + group exist and fix /dev/vfio/* permissions."""
    user = os.environ.get("SUDO_USER", os.environ.get("USER", "root"))
    notes: list[str] = []

    # Ensure 'vfio' group exists
    r = _run("getent group 2110", check=False)
    if r.returncode != 0:
        _run(["sudo", "groupadd", "-g", "2110", "vfio"])
        notes.append("Created group 'vfio'")

    # Add user to group
    _run(["sudo", "usermod", "-aG", "vfio", user], check=False)

    # Ensure udev rule for future devices
    udev_rule = 'SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"'
    udev_path = Path("/etc/udev/rules.d/10-vfio.rules")
    if not udev_path.is_file() or udev_rule not in udev_path.read_text():
        _run(f"echo '{udev_rule}' | sudo tee {udev_path}", sudo=False)
        _run(["sudo", "udevadm", "control", "--reload-rules"])
        _run(["sudo", "udevadm", "trigger"])
        notes.append("Installed udev rule")

    # Fix permissions on already-existing /dev/vfio/* devices
    _run(
        "sudo chgrp vfio /dev/vfio/* 2>/dev/null;"
        " sudo chmod 0660 /dev/vfio/* 2>/dev/null",
        sudo=False,
        check=False,
    )

    # Check if current process has vfio group active
    active_gids = os.getgroups()
    r = _run("getent group vfio", check=False)
    if r.returncode == 0:
        vfio_gid = int(r.stdout.strip().split(":")[2])
        if vfio_gid not in active_gids:
            notes.append(
                "ACTION REQUIRED: Re-login to activate the 'vfio' group. "
                "1) Run `pkill -f vscode-server` from an external SSH session. "
                "2) Reconnect VS Code Remote-SSH. "
                "This is needed because VS Code's remote server inherits "
                "group membership at startup — 'Reload Window' is not enough"
            )

    return "; ".join(notes) if notes else ""


@mcp.tool()
def nic_create_vf(bdf: str, trusted: bool = False, vf_count: int = 6) -> str:
    """
    Create VFs from an Intel E800 series PF and bind them to vfio-pci.

    This is the standard way to use E810/E830 NICs with MTL.
    Creates 6 VFs by default. After reboot, this must be run again.

    Args:
        bdf: PF BDF, e.g. '0000:af:00.0'
        trusted: Create trusted VFs (required for some privileged operations)
        vf_count: Number of VFs to create (default 6)
    """
    err = _validate_bdf(bdf)
    if err:
        return err
    if vf_count <= 0:
        return f"Error: vf_count must be > 0. Got {vf_count}."

    cmd_name = "create_tvf" if trusted else "create_vf"
    out = _run_output(f"sudo bash {NICCTL} {cmd_name} {bdf} {vf_count}")

    # List resulting VFs
    vf_list = _run_output(f"sudo bash {NICCTL} list {bdf}")

    # Ensure VFIO devices are accessible to the current user
    perm_notes = _ensure_vfio_permissions()

    # Verify permissions
    vfio_perms = _run_output("ls -l /dev/vfio/ 2>/dev/null | head -10")

    return (
        f"## Create {'Trusted ' if trusted else ''}VFs on {bdf}\n{out}\n\n"
        f"## VF BDFs\n```\n{vf_list}\n```\n\n"
        f"## VFIO Devices\n```\n{vfio_perms}\n```\n\n"
        + (f"## VFIO Permissions\n{perm_notes}\n\n" if perm_notes else "")
        + "**Remember these VF BDFs** — use them as interface names in JSON configs."
    )


@mcp.tool()
def nic_destroy_vf(bdf: str) -> str:
    """
    Destroy (disable) all VFs on a PF.

    Args:
        bdf: PF BDF, e.g. '0000:af:00.0'
    """
    err = _validate_bdf(bdf)
    if err:
        return err

    out = _run_output(f"sudo bash {NICCTL} disable_vf {bdf}")
    return f"## Disable VFs on {bdf}\n{out}"


@mcp.tool()
def nic_create_kernel_vf(bdf: str, vf_count: int = 6) -> str:
    """
    Create VFs bound to the kernel driver (not DPDK).

    Useful for kernel-based networking or testing without DPDK PMD.

    Args:
        bdf: PF BDF, e.g. '0000:af:00.0'
        vf_count: Number of kernel VFs to create (default 6)
    """
    err = _validate_bdf(bdf)
    if err:
        return err
    if vf_count <= 0:
        return f"Error: vf_count must be > 0. Got {vf_count}."

    out = _run_output(f"sudo bash {NICCTL} create_kvf {bdf} {vf_count}")
    return f"## Create Kernel VFs on {bdf}\n{out}"


@mcp.tool()
def setup_after_reboot_auto(
    nr_hugepages: int = 2048, vf_count: int = 6, trusted: bool = False
) -> str:
    """
    Auto-recover host setup after reboot for VF mode.

    Steps:
    1. Check the ICE driver; if the stock kernel driver is loaded or the
       version doesn't match (e.g. after a kernel upgrade), rebuild the
       patched out-of-tree driver automatically. This destroys any existing
       VFs, which step 6 below re-creates, so it must run first.
    2. Ensure hugepages
    3. Set CPU scaling governor to performance and confirm
    4. Auto-discover SR-IOV-capable Intel PFs
    5. Rebind each PF to kernel driver (if needed)
    6. Create VFs on each PF and bind VFs to vfio-pci
    7. Start MtlManager
    8. Report discovered VF BDFs and a suggested test VF pair

    Args:
        nr_hugepages: Number of 2MB hugepages (default 2048)
        vf_count: Number of VFs per PF (default 6)
        trusted: Create trusted VFs
    """
    if vf_count <= 0:
        return f"Error: vf_count must be > 0. Got {vf_count}."

    results: list[str] = []

    # Check the ICE driver FIRST: a stock kernel driver causes SEGFAULT in
    # iavf_tm_node_add, and a rebuild destroys any existing VFs anyway, so
    # doing this before hugepages/VF creation avoids a wasted VF-creation
    # pass and a second round-trip through this tool.
    ice = ice_driver_status()
    if "ACTION NEEDED" in ice:
        results.append("## ICE Driver: rebuilding patched out-of-tree module")
        results.append(ice_driver_rebuild())

    results.append(hugepages_set(nr_hugepages))
    results.append(_cpu_governor_set_and_confirm_performance())

    pfs = _discover_sriov_intel_pfs()
    if not pfs:
        results.append("No SR-IOV-capable Intel Ethernet PFs were discovered.")
        return "\n\n---\n\n".join(results)

    all_vfs: list[str] = []
    for pf in pfs:
        bdf = pf["bdf"]
        results.append(f"## PF {bdf}\n" + nic_bind_kernel(bdf))
        results.append(nic_create_vf(bdf=bdf, trusted=trusted, vf_count=vf_count))
        vf_raw = _run_output(f"sudo bash {NICCTL} list {bdf}")
        for line in vf_raw.splitlines():
            text = line.strip()
            if re.match(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d$", text):
                all_vfs.append(text)

    unique_vfs = sorted(set(all_vfs))
    if unique_vfs:
        results.append("## All VFs\n```\n" + "\n".join(unique_vfs) + "\n```")
        if len(unique_vfs) >= 2:
            results.append(
                "## Suggested Test Pair\n"
                f"- p_port: {unique_vfs[0]}\n"
                f"- r_port: {unique_vfs[1]}"
            )

    # Start MtlManager (required for lcore allocation in test binaries)
    mgr_result = manager_start()
    results.append(f"## MtlManager\n{mgr_result}")

    return "\n\n---\n\n".join(results)


# ===================================================================
# TOOLS — VFIO Permissions
# ===================================================================


@mcp.tool()
def vfio_setup() -> str:
    """
    Set up VFIO permissions for non-root DPDK usage.

    Creates the 'vfio' group (GID 2110), adds current user,
    installs udev rules, and fixes existing /dev/vfio/* permissions.
    Also called automatically by nic_create_vf.
    """
    notes = _ensure_vfio_permissions()
    user = os.environ.get("SUDO_USER", os.environ.get("USER", "root"))
    groups = _run_output(f"groups {user}")

    steps: list[str] = []
    if notes:
        steps.append(notes)
    steps.append(f"Current groups for {user}: {groups}")
    if "vfio" not in groups:
        steps.append("⚠ Re-login required for group membership to take effect")

    vfio_perms = _run_output("ls -l /dev/vfio/ 2>/dev/null | head -10")
    steps.append(f"VFIO devices:\n```\n{vfio_perms}\n```")

    return "## VFIO Setup\n" + "\n".join(f"- {s}" for s in steps)


# ===================================================================
# TOOLS — ICE Driver
# ===================================================================


@mcp.tool()
def ice_driver_status() -> str:
    """
    Check the ICE driver status: loaded version vs required version,
    module path, and whether it's the patched out-of-tree build.
    """
    return _ice_driver_status_impl()


@mcp.tool()
def ice_driver_rebuild() -> str:
    """
    Build and install the patched out-of-tree ICE driver.

    Downloads the correct ICE version from versions.env, applies MTL patches,
    builds, and installs. Reloads the module afterwards.

    WARNING: This will briefly disconnect NICs using the ice driver.
    """
    return _ice_driver_rebuild_impl()


# ===================================================================
# TOOLS — DPDK
# ===================================================================


@mcp.tool()
def dpdk_status() -> str:
    """Check DPDK installation status and version."""
    ver = _run_output(
        "pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'"
    )
    libs = _run_output("ldconfig -p 2>/dev/null | grep -c librte_ || echo 0")
    devbind = _run_output("which dpdk-devbind.py 2>/dev/null || echo 'not found'")

    vers = _load_versions()
    want = vers.get("DPDK_VER", "unknown")

    return (
        f"## DPDK Status\n"
        f"- Installed: {ver}\n"
        f"- Required: {want}\n"
        f"- librte_* libs in ldcache: {libs}\n"
        f"- dpdk-devbind.py: {devbind}"
    )


@mcp.tool()
def dpdk_build() -> str:
    """
    Build and install DPDK with MTL patches applied.

    Uses the version specified in versions.env. Takes 2-4 minutes on first build.
    """
    rc, out = _run_rc(
        "SETUP_ENVIRONMENT=0 "
        "SETUP_BUILD_AND_INSTALL_DPDK=1 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    # Ensure libraries are in ldcache — without this, builds linking DPDK will fail
    _run(["ldconfig"], sudo=True, check=False)
    ver_after = _run_output(
        "pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'"
    )
    return (
        f"## DPDK Build\n{_summarize_output('dpdk_build', out, rc=rc)}\n\n"
        f"## Result\nInstalled version: {ver_after}"
    )


# ===================================================================
# TOOLS — MTL Build
# ===================================================================


@mcp.tool()
def build_mtl(mode: str = "debugonly") -> str:
    """
    Build the Media Transport Library.

    Use 'debugonly' (the default) when building to run integration tests:
    the simulate-loss / redundancy packet-drop test paths are guarded by
    MTL_SIMULATE_PACKET_DROPS, which is only defined for non-release builds.
    A 'release' build silently compiles those paths out, so tests that rely
    on them (e.g. *redundant* drop tests) skip their drop assertions.

    Args:
        mode: Build mode — 'debugonly' (debug without ASAN, default — required
            for simulate-loss tests), 'debug' (with ASAN), or 'release'.
    """
    if mode not in ("release", "debug", "debugonly"):
        return f"Error: mode must be 'release', 'debug', or 'debugonly'. Got '{mode}'."

    cmd = "./build.sh"
    if mode != "release":
        cmd = f"./build.sh {mode}"

    # Auto-clean stale build directories that cause permission errors
    for stale in [
        REPO_ROOT / "build/meson-private/cmake_mtl_gpu_direct/CMakeCache.txt",
        REPO_ROOT
        / "tests/tools/RxTxApp/build/meson-private/cmake_mtl_gpu_direct/CMakeCache.txt",
    ]:
        if stale.is_file():
            _run(["rm", "-rf", str(stale.parent)], sudo=True, check=False)

    rc, out = _run_rc(cmd, timeout=600)

    # Ensure libraries are in ldcache
    _run(["ldconfig"], sudo=True, check=False)

    rxtxapp = (
        "OK"
        if (REPO_ROOT / "tests/tools/RxTxApp/build/RxTxApp").is_file()
        else "MISSING"
    )
    manager = "OK" if (REPO_ROOT / "build/manager/MtlManager").is_file() else "MISSING"
    libmtl = _run_output("ldconfig -p 2>/dev/null | grep libmtl || echo 'not found'")

    return (
        f"## MTL Build ({mode})\n{_summarize_output('build_mtl', out, rc=rc)}\n\n"
        f"## Artifacts\n- RxTxApp: {rxtxapp}\n- MtlManager: {manager}\n- libmtl: {libmtl}"
    )


# ===================================================================
# TOOLS — Runtime (MtlManager, PTP)
# ===================================================================


@mcp.tool()
def manager_start() -> str:
    """
    Start MtlManager in the background.

    MtlManager MUST be running before starting multiple MTL processes.
    It manages lcore allocation across processes.
    """
    mgr = REPO_ROOT / "build/manager/MtlManager"
    if not mgr.is_file():
        return "Error: MtlManager not built. Run build_mtl first."

    # Check if already running
    r = _run("pgrep -x MtlManager", check=False)
    if r.returncode == 0:
        pid = r.stdout.strip()
        return f"MtlManager already running (PID {pid}). No action taken."

    # Use nohup + output redirection to properly daemonize.
    # Without redirection, subprocess.run waits for stdout/stderr to close
    # even with &, causing a timeout.
    log_file = REPO_ROOT / "build/manager/MtlManager.log"
    _run(
        f"sudo nohup {mgr} > {log_file} 2>&1 &",
        check=False,
        timeout=5,
    )

    # Give it a moment to start, then verify
    time.sleep(0.5)

    r2 = _run("pgrep -x MtlManager", check=False)
    if r2.returncode == 0:
        pid = r2.stdout.strip()
        return f"MtlManager started successfully (PID {pid}). Log: {log_file}"
    return (
        f"Warning: MtlManager may not have started. "
        f"Check log at {log_file} or run `sudo {mgr}` manually."
    )


@mcp.tool()
def manager_stop() -> str:
    """Stop MtlManager if running."""
    r = _run("pgrep -x MtlManager", check=False)
    if r.returncode != 0:
        return "MtlManager is not running."

    _run(["sudo", "pkill", "-x", "MtlManager"], check=False)
    return "MtlManager stopped."


@mcp.tool()
def ptp_status() -> str:
    """
    Check PTP (Precision Time Protocol) status.

    Shows ptp4l/phc2sys process status, PHC devices, and NIC PTP capabilities.
    """
    sections: list[str] = []

    # ptp4l process
    ptp4l = _run_output("pgrep -a ptp4l 2>/dev/null || echo 'ptp4l not running'")
    sections.append(f"## ptp4l\n{ptp4l}")

    # phc2sys process
    phc2sys = _run_output("pgrep -a phc2sys 2>/dev/null || echo 'phc2sys not running'")
    sections.append(f"## phc2sys\n{phc2sys}")

    # PHC devices
    phc_devs = _run_output("ls -la /dev/ptp* 2>/dev/null || echo 'no PHC devices'")
    sections.append(f"## PHC Devices\n```\n{phc_devs}\n```")

    # NTP status (should be disabled for PTP)
    ntp = _run_output("timedatectl show -p NTP --value 2>/dev/null || echo 'unknown'")
    sections.append(
        f"## NTP\n- NTP active: {ntp} {'(should be disabled for PTP)' if ntp == 'yes' else ''}"
    )

    return "\n\n".join(sections)


# ===================================================================
# TOOLS — DPDK Device Binding Status
# ===================================================================


@mcp.tool()
def dpdk_devbind_status() -> str:
    """
    Show full dpdk-devbind status — all network devices grouped by driver.

    Shows which devices are bound to DPDK (vfio-pci), kernel, or unbound.
    """
    out = _run_output(
        "dpdk-devbind.py -s 2>/dev/null || dpdk-devbind -s 2>/dev/null || echo 'dpdk-devbind not found'"
    )
    return f"## DPDK Device Bind Status\n```\n{out}\n```"


# ===================================================================
# TOOLS — Kernel Configuration
# ===================================================================


@mcp.tool()
def memlock_status() -> str:
    """
    Check RLIMIT_MEMLOCK settings for non-root DPDK usage.

    If memlock is too low, VFIO DMA remapping will fail.
    """
    ulimit_val = _run_output("ulimit -l 2>/dev/null || echo 'unknown'")
    user = os.environ.get("USER", "unknown")

    limits_conf = _run_output(
        "grep -i memlock /etc/security/limits.conf 2>/dev/null || echo 'no memlock entries'"
    )

    return (
        f"## RLIMIT_MEMLOCK\n"
        f"- Current user: {user}\n"
        f"- ulimit -l: {ulimit_val}\n"
        f"- /etc/security/limits.conf:\n```\n{limits_conf}\n```\n\n"
        + (
            "### Action Required\n"
            f"Add to /etc/security/limits.conf:\n```\n"
            f"{user}    hard   memlock           unlimited\n"
            f"{user}    soft   memlock           unlimited\n"
            f"```\nThen reboot."
            if ulimit_val not in ("unlimited", "unknown")
            else ""
        )
    )


# ===================================================================
# TOOLS — Install Dependencies
# ===================================================================


@mcp.tool()
def install_dependencies() -> str:
    """
    Install system build dependencies (apt packages) for MTL.

    Installs: gcc, meson, libnuma-dev, libjson-c-dev, libpcap-dev,
    libgtest-dev, libssl-dev, systemtap-sdt-dev, clang, etc.
    """
    return _install_dependencies_impl()


# ===================================================================
# TOOLS — XDP / eBPF
# ===================================================================


@mcp.tool()
def build_ebpf_xdp() -> str:
    """
    Build and install eBPF/XDP support for MTL's XDP data path backend.

    This is optional — only needed if using XDP transport instead of DPDK PMD.
    """
    rc, out = _run_rc(
        "SETUP_ENVIRONMENT=0 "
        "SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=1 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=300,
    )
    return f"## eBPF/XDP Build\n{_summarize_output('ebpf_xdp_build', out, rc=rc)}"


# ===================================================================
# TOOLS — Quick Diagnostics
# ===================================================================


@mcp.tool()
def dmesg_tail(lines: int = 50, filter_str: str = "") -> str:
    """
    Show recent kernel messages (dmesg). Useful for diagnosing NIC/driver issues.

    Args:
        lines: Number of lines to show (default 50)
        filter_str: Optional grep filter (alphanumeric, dots, dashes, colons,
            spaces only — other characters are stripped). E.g. 'ice' or 'vfio'.
    """
    if lines > 200:
        lines = 200

    cmd = f"sudo dmesg --time-format iso | tail -n {lines}"
    if filter_str:
        # Sanitize filter to prevent injection
        safe_filter = re.sub(r"[^a-zA-Z0-9_.\-: ]", "", filter_str)
        cmd = (
            f"sudo dmesg --time-format iso | grep -i '{safe_filter}' | tail -n {lines}"
        )

    out = _run_output(cmd)
    return f"## dmesg (last {lines} lines{f', filter={filter_str}' if filter_str else ''})\n```\n{out}\n```"


@mcp.tool()
def status_report() -> str:
    """
    Run MTL's built-in status_report.sh for full system diagnostics.

    Collects NIC, driver, hugepages, IOMMU, and DPDK info in one report.
    """
    script = REPO_ROOT / "script" / "status_report.sh"
    if not script.is_file():
        return "Error: script/status_report.sh not found"

    out = _run_output(f"bash {script}", timeout=30)
    return f"## MTL Status Report\n```\n{out}\n```"


# ===================================================================
# TOOLS — Test Execution
# ===================================================================


def _build_has_simulate_drops() -> bool:
    """True if build/ was compiled with MTL_SIMULATE_PACKET_DROPS.

    The macro is defined only for non-release builds (see meson.build). It
    gates the simulate-loss / redundancy packet-drop test paths, so a release
    build makes those tests silently skip their drop assertions. Detected by
    inspecting the compile database emitted by meson.
    """
    ccmds = REPO_ROOT / "build/compile_commands.json"
    try:
        return "MTL_SIMULATE_PACKET_DROPS" in ccmds.read_text()
    except OSError:
        return False


_SIM_DROPS_HINT = (
    "\n\n> **Warning:** `build/` was compiled in **release** mode "
    "(no `MTL_SIMULATE_PACKET_DROPS`). Simulate-loss / redundancy "
    "packet-drop test paths are compiled out, so their drop assertions "
    'silently skip. Rebuild with `build_mtl(mode="debugonly")` before '
    "running these tests."
)


@mcp.tool()
def run_gtest(
    p_port: str = "",
    r_port: str = "",
    gtest_filter: str = "",
    dma_dev: str = "",
    timeout_seconds: int = 600,
    auto_start_stop: bool = True,
) -> str:
    """
    Run MTL integration tests (KahawaiTest).

    Auto-discovers VF ports if not specified. Returns structured pass/fail
    counts and failure details.

    Args:
        p_port: Primary port BDF (e.g. '0000:c9:01.0'). Auto-discovered if empty.
        r_port: Receiver port BDF (e.g. '0000:c9:01.1'). Auto-discovered if empty.
        gtest_filter: Gtest filter pattern (e.g. 'St20p*', 'St30*', 'Misc*'). All if empty.
        dma_dev: Comma-separated DMA devices for DMA tests (e.g. '0000:00:01.0,0000:00:01.1').
        timeout_seconds: Max seconds for test run (default 600).
        auto_start_stop: Pass --auto_start_stop flag (default True).
    """
    binary = REPO_ROOT / "build/tests/KahawaiTest"
    if not binary.is_file():
        return "Error: KahawaiTest not built. Run `build_mtl` first."

    sim_hint = "" if _build_has_simulate_drops() else _SIM_DROPS_HINT

    # Auto-discover ports from VFIO-bound VFs
    if not p_port or not r_port:
        vfs = (
            _run_output(
                "dpdk-devbind.py -s 2>/dev/null | awk '/drv=vfio-pci/{print $1}' | head -2"
            )
            .strip()
            .splitlines()
        )
        if len(vfs) < 2:
            return (
                "Error: Cannot auto-discover test ports. "
                "Need at least 2 VFs bound to vfio-pci. "
                "Run `setup_after_reboot_auto` first."
            )
        if not p_port:
            p_port = vfs[0].strip()
        if not r_port:
            r_port = vfs[1].strip()

    # Validate BDF inputs
    for label, bdf in [("p_port", p_port), ("r_port", r_port)]:
        err = _validate_bdf(bdf)
        if err:
            return f"Error: {label} — {err}"

    # Sanitize gtest_filter — only allow gtest filter characters
    if gtest_filter and not re.match(r"^[a-zA-Z0-9_.*:/-]+$", gtest_filter):
        return "Error: invalid gtest_filter characters. Use alphanumeric, *, ., :, /, -"

    # NoCtxTest cases require one KahawaiTest process per test (DPDK EAL
    # cannot be re-initialised within a single process) plus the --no_ctx /
    # --no_ctx_tests flags this tool doesn't pass. Running them here would
    # just fail/hang, so refuse up front and point at the right tool.
    if (
        re.search(r"(?:^|[:*])NoCtxTest[.*:]", gtest_filter)
        or gtest_filter == "NoCtxTest"
    ):
        return (
            "Error: NoCtxTest cases cannot be run via run_gtest (each needs its "
            "own KahawaiTest process). Use run_noctx_tests instead."
        )

    # Build command as list for safe execution
    cmd_parts: list[str] = [str(binary), "--p_port", p_port, "--r_port", r_port]
    if auto_start_stop:
        cmd_parts.append("--auto_start_stop")
    if dma_dev:
        cmd_parts.extend(["--dma_dev", dma_dev])
    if gtest_filter:
        cmd_parts.append(f"--gtest_filter={gtest_filter}")

    out = _run_output(cmd_parts, timeout=timeout_seconds)

    # Parse results
    passed = 0
    failed = 0
    total = 0
    failures: list[str] = []

    for line in out.splitlines():
        stripped = line.strip()
        if stripped.startswith("[  PASSED  ]"):
            m = re.search(r"\[\s+PASSED\s+\]\s+(\d+)", stripped)
            if m:
                passed = int(m.group(1))
        elif stripped.startswith("[  FAILED  ]"):
            m = re.search(r"\[\s+FAILED\s+\]\s+(\d+)", stripped)
            if m:
                failed = int(m.group(1))
            else:
                failures.append(stripped)
        elif "tests from" in stripped and "test suite" in stripped:
            m = re.search(r"(\d+) tests? from", stripped)
            if m:
                total = int(m.group(1))

    cmd_display = " ".join(cmd_parts)
    summary = (
        f"## GTest Results\n"
        f"- Command: `{cmd_display}`\n"
        f"- Total: {total}, Passed: {passed}, Failed: {failed}\n"
    )
    if failures:
        summary += "\n### Failed Tests\n" + "\n".join(f"- {f}" for f in failures)

    summary += sim_hint
    return f"{summary}\n\n{_summarize_output('gtest', out)}"


@mcp.tool()
def run_noctx_tests(
    port_1: str = "",
    port_2: str = "",
    port_3: str = "",
    port_4: str = "",
    gtest_filter: str = "",
    timeout_seconds: int = 600,
    cooldown_seconds: int = 10,
) -> str:
    """
    Run MTL no-context (noctx) integration tests.

    Each NoCtxTest case is run in its own KahawaiTest process because DPDK EAL
    cannot be re-initialised within a single process. The tool enumerates the
    test names that match `gtest_filter` via `--gtest_list_tests` and then
    invokes one process per test, sleeping `cooldown_seconds` between them
    (mirrors tests/integration_tests/noctx/run.sh).

    All 4 ports are required — this is the full-suite run and a handful of
    tests exercise redundant (4-port) sessions; requiring fewer here would let
    those tests fail ambiguously depending on how many ports happen to be
    supplied. Tests named with a `_pf_` infix (e.g. TSN/launch-time-pacing)
    require a PF, which this VF-oriented run can never satisfy, so they are
    excluded automatically — use `run_noctx_pf_tests` for those instead.

    Args:
        port_1, port_2, port_3, port_4: BDFs for the 4 ports. Auto-discovered
                        (any vfio-pci-bound ports, PF or VF) if left empty.
        gtest_filter: Gtest filter scoped to NoCtxTest (e.g. '*nonsplit*' or
                      'NoCtxTest.st40i_*'). All NoCtxTest cases if empty.
        timeout_seconds: Max seconds per individual test process (default 600).
        cooldown_seconds: Sleep between tests (default 10).
    """

    # Auto-discover only if the caller supplied nothing; otherwise respect
    # exactly what was passed.
    if not any([port_1, port_2, port_3, port_4]):
        discovered = (
            _run_output(
                "dpdk-devbind.py -s 2>/dev/null | awk '/drv=vfio-pci/{print $1}' | head -4"
            )
            .strip()
            .splitlines()
        )
        ports = [v.strip() for v in discovered[:4]]
    else:
        ports = [p for p in (port_1, port_2, port_3, port_4) if p]

    if len(ports) < 4:
        return (
            "Error: noctx full-suite run requires 4 ports bound to vfio-pci "
            f"(PF or VF). Found {len(ports)}. Run `setup_after_reboot_auto` first, "
            "or supply port_1..port_4 explicitly."
        )

    # Validate the BDFs we ended up with
    for idx, bdf in enumerate(ports, start=1):
        err = _validate_bdf(bdf)
        if err:
            return f"Error: port_{idx} — {err}"

    # Sanitize gtest_filter
    if gtest_filter and not re.match(r"^[a-zA-Z0-9_.*:/-]+$", gtest_filter):
        return "Error: invalid gtest_filter characters"

    binary = REPO_ROOT / "build/tests/KahawaiTest"
    if not binary.is_file():
        return "Error: KahawaiTest not built. Run `build_mtl` first."

    sim_hint = "" if _build_has_simulate_drops() else _SIM_DROPS_HINT

    port_list = ",".join(ports)

    # Default to all NoCtxTest cases; scope user filter to NoCtxTest.*
    if not gtest_filter:
        list_filter = "NoCtxTest.*"
    elif gtest_filter.startswith("NoCtxTest."):
        list_filter = gtest_filter
    else:
        list_filter = f"NoCtxTest.*{gtest_filter}*"

    # PF-only tests (name contains "_pf_") can never pass on these VF-capable
    # ports; exclude them unless the caller already wrote their own negative
    # pattern (gtest filter syntax only supports one "-" exclusion group).
    if "-" not in list_filter:
        list_filter = f"{list_filter}-NoCtxTest.*_pf_*"

    # Enumerate matching test names (one process per test below)
    listing = _run_output(
        f"{binary} --gtest_list_tests --no_ctx"
        f" --port_list={port_list}"
        f" --gtest_filter={list_filter}",
        timeout=60,
    )
    # Output format: "NoCtxTest.\n  test_a\n  test_b\n"
    test_names: list[str] = []
    current_suite = ""
    for raw in listing.splitlines():
        if not raw or raw.startswith(("DISABLED", "Note:")):
            continue
        if not raw.startswith(" "):
            current_suite = raw.strip().rstrip(".")
        else:
            name = raw.strip()
            if name and current_suite == "NoCtxTest":
                test_names.append(name)

    if not test_names:
        return (
            f"Error: no NoCtxTest cases matched filter '{list_filter}'.\n"
            f"Listing output:\n{listing}"
        )

    # Run each test in its own process; collect per-test results
    sections: list[str] = []
    results: list[tuple[str, str]] = []  # (name, PASS|FAIL|TIMEOUT)
    for idx, name in enumerate(test_names):
        full = f"NoCtxTest.{name}"
        out = _run_output(
            f"{binary} --auto_start_stop"
            f" --port_list={port_list}"
            f" --gtest_filter={full}"
            f" --no_ctx_tests",
            timeout=timeout_seconds,
        )
        if "*** TIMEOUT" in out:
            status = "TIMEOUT"
        elif re.search(r"\[\s+PASSED\s+\]\s+1\s+test", out):
            status = "PASS"
        else:
            status = "FAIL"
        results.append((name, status))
        sections.append(f"===== {full}: {status} =====\n{out}\n")
        if idx < len(test_names) - 1 and cooldown_seconds > 0:
            time.sleep(cooldown_seconds)

    combined = "\n".join(sections)
    log_path = _save_test_log("noctx", combined)

    passed = sum(1 for _, s in results if s == "PASS")
    failed = sum(1 for _, s in results if s != "PASS")
    status = "PASSED" if failed == 0 else "FAILED"

    per_test = "\n".join(f"- {s}: NoCtxTest.{n}" for n, s in results)
    last_failure = ""
    for name, st in results:
        if st != "PASS":
            for sec in sections:
                if sec.startswith(f"===== NoCtxTest.{name}:"):
                    tail = "\n".join(sec.splitlines()[-30:])
                    last_failure = (
                        f"\n### First failure: NoCtxTest.{name} (last 30 lines)\n"
                        f"```\n{tail}\n```"
                    )
                    break
            break

    return (
        f"## Noctx Test Results\n"
        f"- Status: {status}\n"
        f"- Ports: {port_list}\n"
        f"- Tests run: {len(results)} (passed {passed}, failed {failed})\n"
        f"- Full log: `{log_path}`\n"
        f"\n### Per-test results\n{per_test}\n"
        f"{last_failure}"
        f"{sim_hint}"
    )


@mcp.tool()
def run_noctx_pf_tests(
    port_1: str = "",
    port_2: str = "",
    gtest_filter: str = "",
    timeout_seconds: int = 600,
    cooldown_seconds: int = 10,
) -> str:
    """
    Run the NoCtxTest cases that require PF ports (mirrors
    tests/integration_tests/noctx/run_pf.sh).

    Tests named with a `_pf_` infix (e.g. TSN/launch-time-pacing) require a PF
    port — that offload is not advertised by VF drivers — so `run_noctx_tests`
    excludes them. This tool runs just those, against 2 PF-bound ports (all
    current PF-only tests use a single TX/RX pair).

    Args:
        port_1, port_2: BDFs for a PF TX/RX pair. Auto-discovered (vfio-pci
                        ports without a `physfn` sysfs link, i.e. real PFs,
                        not VFs) if left empty.
        gtest_filter: Substring/glob to match against the test name (e.g.
                      'st20p_tx_epoch_onward_recovers_after_ptp_step' or
                      'st20p_*'), matched anywhere in the name — no need to
                      include "_pf_" yourself, it's required separately.
                      All `_pf_` cases if empty.
        timeout_seconds: Max seconds per individual test process (default 600).
        cooldown_seconds: Sleep between tests (default 10).
    """

    if not any([port_1, port_2]):
        discovered = (
            _run_output(
                "for d in $(dpdk-devbind.py -s 2>/dev/null | awk '/drv=vfio-pci/{print $1}'); do "
                '[ ! -e "/sys/bus/pci/devices/$d/physfn" ] && echo "$d"; done | head -2'
            )
            .strip()
            .splitlines()
        )
        ports = [v.strip() for v in discovered[:2]]
    else:
        ports = [p for p in (port_1, port_2) if p]

    if len(ports) < 2:
        return (
            "Error: PF noctx tests require 2 ports bound to vfio-pci as PFs "
            f"(not VFs). Found {len(ports)}. Run `nic_bind_pmd` on a PF, or "
            "supply port_1/port_2 explicitly."
        )

    for idx, bdf in enumerate(ports, start=1):
        err = _validate_bdf(bdf)
        if err:
            return f"Error: port_{idx} — {err}"

    if gtest_filter and not re.match(r"^[a-zA-Z0-9_.*:/-]+$", gtest_filter):
        return "Error: invalid gtest_filter characters"

    binary = REPO_ROOT / "build/tests/KahawaiTest"
    if not binary.is_file():
        return "Error: KahawaiTest not built. Run `build_mtl` first."

    sim_hint = "" if _build_has_simulate_drops() else _SIM_DROPS_HINT

    port_list = ",".join(ports)

    # Scope to NoCtxTest, same convention as run_noctx_tests: don't assume
    # where "_pf_" falls relative to the caller's filter text (the naming
    # convention puts the descriptive prefix *before* "_pf_", e.g.
    # "st20p_..._pf_tsn_pacing", so a filter matching that prefix would never
    # match a glob that requires "_pf_" to come first). Enumerate with just
    # the caller's filter, then require "_pf_" as a separate post-filter.
    if not gtest_filter:
        list_filter = "NoCtxTest.*"
    elif gtest_filter.startswith("NoCtxTest."):
        list_filter = gtest_filter
    else:
        list_filter = f"NoCtxTest.*{gtest_filter}*"

    listing = _run_output(
        f"{binary} --gtest_list_tests --no_ctx"
        f" --port_list={port_list}"
        f" --gtest_filter={list_filter}",
        timeout=60,
    )
    test_names: list[str] = []
    current_suite = ""
    for raw in listing.splitlines():
        if not raw or raw.startswith(("DISABLED", "Note:")):
            continue
        if not raw.startswith(" "):
            current_suite = raw.strip().rstrip(".")
        else:
            name = raw.strip()
            if name and current_suite == "NoCtxTest" and "_pf_" in name:
                test_names.append(name)

    if not test_names:
        return (
            f"No PF-only (name contains '_pf_') NoCtxTest cases matched filter "
            f"'{list_filter}'.\nListing output:\n{listing}"
        )

    sections: list[str] = []
    results: list[tuple[str, str]] = []  # (name, PASS|FAIL|TIMEOUT)
    for idx, name in enumerate(test_names):
        full = f"NoCtxTest.{name}"
        out = _run_output(
            f"{binary} --auto_start_stop"
            f" --port_list={port_list}"
            f" --gtest_filter={full}"
            f" --no_ctx_tests",
            timeout=timeout_seconds,
        )
        if "*** TIMEOUT" in out:
            status = "TIMEOUT"
        elif re.search(r"\[\s+PASSED\s+\]\s+1\s+test", out):
            status = "PASS"
        else:
            status = "FAIL"
        results.append((name, status))
        sections.append(f"===== {full}: {status} =====\n{out}\n")
        if idx < len(test_names) - 1 and cooldown_seconds > 0:
            time.sleep(cooldown_seconds)

    combined = "\n".join(sections)
    log_path = _save_test_log("noctx_pf", combined)

    passed = sum(1 for _, s in results if s == "PASS")
    failed = sum(1 for _, s in results if s != "PASS")
    status = "PASSED" if failed == 0 else "FAILED"

    per_test = "\n".join(f"- {s}: NoCtxTest.{n}" for n, s in results)
    last_failure = ""
    for name, st in results:
        if st != "PASS":
            for sec in sections:
                if sec.startswith(f"===== NoCtxTest.{name}:"):
                    tail = "\n".join(sec.splitlines()[-30:])
                    last_failure = (
                        f"\n### First failure: NoCtxTest.{name} (last 30 lines)\n"
                        f"```\n{tail}\n```"
                    )
                    break
            break

    return (
        f"## Noctx PF Test Results\n"
        f"- Status: {status}\n"
        f"- Ports: {port_list}\n"
        f"- Tests run: {len(results)} (passed {passed}, failed {failed})\n"
        f"- Full log: `{log_path}`\n"
        f"\n### Per-test results\n{per_test}\n"
        f"{last_failure}"
        f"{sim_hint}"
    )


@mcp.tool()
def mtl_clean_rebuild(mode: str = "release") -> str:
    """
    Clean and rebuild MTL from scratch.

    Removes stale build directories, rebuilds MTL, and refreshes ldcache.
    Use when build fails with permission errors or stale artifacts.

    Args:
        mode: Build mode — 'release', 'debug', or 'debugonly'.
    """
    if mode not in ("release", "debug", "debugonly"):
        return f"Error: mode must be 'release', 'debug', or 'debugonly'. Got '{mode}'."

    # Clean
    for d in ["build", "tests/tools/RxTxApp/build"]:
        target = REPO_ROOT / d
        if target.exists():
            _run(["rm", "-rf", str(target)], sudo=True, check=False)

    # Build
    result = build_mtl(mode)

    return f"## Clean Rebuild ({mode})\n{result}"


@mcp.tool()
def log_tail(
    source: str = "mtl_manager",
    lines: int = 50,
    filter_str: str = "",
    log_path: str = "",
) -> str:
    """
    Tail MTL-related log files.

    Args:
        source: Log source — 'mtl_manager', 'dmesg', 'validation', 'syslog', or
            'saved' to read one of the full logs saved by build_mtl/dpdk_build/
            ice_driver_rebuild/install_dependencies/build_ebpf_xdp/run_gtest/
            run_noctx_tests/run_noctx_pf_tests under build/logs/ (pass its
            path via log_path).
        lines: Number of lines to show (default 50, max 200).
        filter_str: Optional grep filter (alphanumeric, dots, dashes, colons,
            spaces only — other characters are stripped).
        log_path: Path to a saved log under build/logs/, as returned by the
            tools above (e.g. 'build/logs/build_mtl_20260710_090000.log').
            Required when source='saved'.
    """
    if lines > 200:
        lines = 200

    log_paths = {
        "mtl_manager": str(REPO_ROOT / "build/manager/MtlManager.log"),
        "syslog": "/var/log/syslog",
    }

    if source == "saved":
        if not log_path:
            return "Error: log_path is required when source='saved'."
        logs_dir = (REPO_ROOT / "build" / "logs").resolve()
        candidate = (REPO_ROOT / log_path).resolve()
        if candidate.parent != logs_dir or not candidate.is_file():
            return (
                f"Error: '{log_path}' is not a saved log under build/logs/. "
                "Use the exact path returned by the tool that produced it."
            )
        log_file = str(candidate)
    elif source == "dmesg":
        return dmesg_tail(lines, filter_str)
    elif source == "validation":
        # Find latest validation log — resolve and verify it's under REPO_ROOT
        latest = _run_output(
            "ls -t tests/validation/logs/*/output.log 2>/dev/null | head -1",
            cwd=REPO_ROOT,
        )
        if not latest or "No such file" in latest:
            return "No validation logs found at tests/validation/logs/"
        log_file = os.path.realpath(os.path.join(str(REPO_ROOT), latest.strip()))
        if not log_file.startswith(str(REPO_ROOT)):
            return "Error: validation log path resolved outside repository."
    elif source in log_paths:
        log_file = log_paths[source]
    else:
        return (
            f"Error: unknown source '{source}'. "
            "Use: mtl_manager, dmesg, validation, syslog, saved."
        )

    cmd = f"sudo tail -n {int(lines)} {log_file}"
    if filter_str:
        safe_filter = re.sub(r"[^a-zA-Z0-9_.\-: ]", "", filter_str)
        cmd = f"sudo grep -i '{safe_filter}' {log_file} | tail -n {int(lines)}"

    out = _run_output(cmd)
    if not out.strip():
        return f"## Log: {source}\nNo output (file may be empty or missing)."
    return f"## Log: {source} (last {lines} lines{f', filter={filter_str}' if filter_str else ''})\n```\n{out}\n```"


# ===================================================================
# Entry point
# ===================================================================

if __name__ == "__main__":
    mcp.run(transport="stdio")
