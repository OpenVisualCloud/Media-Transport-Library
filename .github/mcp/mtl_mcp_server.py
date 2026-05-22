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
import subprocess
import textwrap
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent  # .github/mcp -> repo root
NICCTL = REPO_ROOT / "script" / "nicctl.sh"
VERSIONS_ENV = REPO_ROOT / "versions.env"

mcp = FastMCP(
    "mtl-system-setup",
    instructions=textwrap.dedent("""\
        MTL System Setup MCP Server — tools for preparing a Linux host to run
        the Media Transport Library (SMPTE ST 2110 over DPDK).

        Common workflows:
        • First-time setup:  system_status → iommu_status → hugepages_set →
                             nic_create_vf → build_mtl → manager_start
        • After reboot:      hugepages_set → nic_create_vf → manager_start
        • Switch PF→VF:      nic_bind_kernel → nic_create_vf
        • Switch VF→PF:      nic_destroy_vf → nic_bind_pmd
        • Kernel socket mode: nic_bind_kernel (leave NIC in kernel)
    """),
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _run(
    cmd: list[str] | str,
    *,
    sudo: bool = False,
    check: bool = True,
    timeout: int = 300,
    cwd: str | Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a command and return CompletedProcess."""
    if isinstance(cmd, str):
        cmd = ["bash", "-c", cmd]
    if sudo:
        cmd = ["sudo"] + cmd
    merged_env = {**os.environ, **(env or {})}
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
        cwd=cwd or REPO_ROOT,
        env=merged_env,
    )


def _run_output(cmd: list[str] | str, **kw: Any) -> str:
    """Run and return combined stdout+stderr, never raising on non-zero rc."""
    r = _run(cmd, check=False, **kw)
    out = r.stdout
    if r.stderr:
        out += "\n" + r.stderr
    return out.strip()


def _load_versions() -> dict[str, str]:
    """Parse versions.env into a dict."""
    result: dict[str, str] = {}
    if VERSIONS_ENV.is_file():
        for line in VERSIONS_ENV.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                # Expand simple ${VAR} references within the file
                for ref_k, ref_v in result.items():
                    v = v.replace(f"${{{ref_k}}}", ref_v)
                result[k.strip()] = v.strip()
    return result


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
        vendor = (dev / "vendor").read_text().strip() if (dev / "vendor").is_file() else ""
        class_code = (dev / "class").read_text().strip() if (dev / "class").is_file() else ""
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


def _cpu_governor_set_and_confirm_performance() -> str:
    """Set all CPU scaling governors to performance and confirm status."""
    set_out = _run_output(
        "for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; "
        "do echo performance | sudo tee $cpu; done"
    )
    verify_out = _run_output(
        "for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; "
        "do printf '%s=%s\\n' \"$cpu\" \"$(cat $cpu 2>/dev/null)\"; done"
    )

    total = 0
    non_perf: list[str] = []
    for line in verify_out.splitlines():
        if not line.strip():
            continue
        total += 1
        _, _, gov = line.partition("=")
        if gov.strip() != "performance":
            non_perf.append(line)

    status = "PASS" if total > 0 and not non_perf else "FAIL"
    summary = f"- CPUs checked: {total}\\n- Status: {status}"
    if non_perf:
        summary += "\\n- Non-performance entries:\\n" + "\\n".join(non_perf)

    return (
        "## CPU Governor (set to performance + confirm)\\n"
        f"{summary}\\n\\n"
        f"Set output:\\n```\\n{set_out}\\n```\\n\\n"
        f"Verify output:\\n```\\n{verify_out}\\n```"
    )


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
    lshw = _run_output("sudo lshw -c network -businfo 2>/dev/null || echo 'lshw not available'")
    sections.append(f"## Network Devices (lshw)\n```\n{lshw}\n```")

    # dpdk-devbind status — shows driver bindings (vfio-pci / kernel / unbound)
    devbind = _run_output(
        "dpdk-devbind.py -s 2>/dev/null || dpdk-devbind -s 2>/dev/null || echo 'dpdk-devbind not found'"
    )
    sections.append(f"## DPDK Device Bindings (dpdk-devbind)\n```\n{devbind}\n```")

    # ICE driver
    ice_ver = _run_output("modinfo ice 2>/dev/null | grep '^version:' | head -1 || echo 'not loaded'")
    ice_path = _run_output("modinfo -n ice 2>/dev/null || echo 'not found'")
    sections.append(f"## ICE Driver\n- {ice_ver}\n- Path: {ice_path}")

    # DPDK
    dpdk_ver = _run_output("pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'")
    sections.append(f"## DPDK\n- Version: {dpdk_ver}")

    # MTL
    mtl_installed = _run_output("ldconfig -p 2>/dev/null | grep libmtl || echo 'not installed'")
    rxtxapp = "OK" if (REPO_ROOT / "tests/tools/RxTxApp/build/RxTxApp").is_file() else "NOT BUILT"
    manager = "OK" if (REPO_ROOT / "build/manager/MtlManager").is_file() else "NOT BUILT"
    sections.append(
        f"## MTL\n- libmtl: {mtl_installed}\n- RxTxApp: {rxtxapp}\n- MtlManager: {manager}"
    )

    # VFIO devices
    vfio = _run_output("ls -l /dev/vfio/ 2>/dev/null | head -20 || echo 'no VFIO devices'")
    sections.append(f"## VFIO Devices\n```\n{vfio}\n```")

    # Versions from versions.env
    vers = _load_versions()
    ver_lines = "\n".join(f"- {k}: {v}" for k, v in vers.items() if not k.endswith("_REPO"))
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
    info = _run_output("grep -i huge /proc/meminfo")
    return f"## Hugepages\n```\n{info}\n```"


@mcp.tool()
def hugepages_set(nr_hugepages: int = 2048, size_kb: int = 2048) -> str:
    """
    Configure hugepages. Default: 2048 x 2MB = 4GB.

    Args:
        nr_hugepages: Number of hugepages to allocate (default 2048).
        size_kb: Hugepage size in KB. Use 2048 for 2MB pages (default).
    """
    hp_path = f"/sys/kernel/mm/hugepages/hugepages-{size_kb}kB/nr_hugepages"
    if not Path(hp_path).exists():
        return f"Error: hugepage size {size_kb}kB not supported. Available:\n" + _run_output(
            "ls /sys/kernel/mm/hugepages/"
        )

    _run(f"echo {nr_hugepages} | sudo tee {hp_path}", sudo=False)
    after = _run_output("grep -i huge /proc/meminfo")
    total_mb = nr_hugepages * size_kb // 1024
    return f"Set {nr_hugepages} x {size_kb}kB hugepages ({total_mb} MB total).\n\n```\n{after}\n```"


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
    out = _run_output("dpdk-devbind.py -s 2>/dev/null || dpdk-devbind -s 2>/dev/null || echo 'dpdk-devbind not found'")
    return f"## NIC Status\n```\n{out}\n```"


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

    # Check VFIO permissions
    vfio_perms = _run_output("ls -l /dev/vfio/ 2>/dev/null | head -10")

    return (
        f"## Create {'Trusted ' if trusted else ''}VFs on {bdf}\n{out}\n\n"
        f"## VF BDFs\n```\n{vf_list}\n```\n\n"
        f"## VFIO Devices\n```\n{vfio_perms}\n```\n\n"
        f"**Remember these VF BDFs** — use them as interface names in JSON configs."
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
    1. Ensure hugepages
    2. Set CPU scaling governor to performance and confirm
    3. Auto-discover SR-IOV-capable Intel PFs
    4. Rebind each PF to kernel driver (if needed)
    5. Create VFs on each PF and bind VFs to vfio-pci
    6. Report discovered VF BDFs and a suggested test VF pair

    Args:
        nr_hugepages: Number of 2MB hugepages (default 2048)
        vf_count: Number of VFs per PF (default 6)
        trusted: Create trusted VFs
    """
    if vf_count <= 0:
        return f"Error: vf_count must be > 0. Got {vf_count}."

    results: list[str] = []
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
        results.append(
            nic_create_vf(bdf=bdf, trusted=trusted, vf_count=vf_count)
        )
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

    results.append(
        "## Next Step\n"
        "Run `manager_start` before launching test binaries."
    )
    return "\n\n---\n\n".join(results)


# ===================================================================
# TOOLS — VFIO Permissions
# ===================================================================


@mcp.tool()
def vfio_setup() -> str:
    """
    Set up VFIO permissions for non-root DPDK usage.

    Creates the 'vfio' group (GID 2110), adds current user, and
    installs udev rules so /dev/vfio/* devices are group-accessible.
    Requires re-login after first run.
    """
    steps: list[str] = []
    user = os.environ.get("SUDO_USER", os.environ.get("USER", "root"))

    # Check if group exists
    r = _run("getent group 2110", check=False)
    if r.returncode != 0:
        _run(["sudo", "groupadd", "-g", "2110", "vfio"])
        steps.append("Created group 'vfio' (GID 2110)")
    else:
        steps.append("Group 'vfio' (GID 2110) already exists")

    # Add user to group
    _run(["sudo", "usermod", "-aG", "vfio", user])
    steps.append(f"Added user '{user}' to group 'vfio'")

    # udev rule
    udev_rule = 'SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"\n'
    udev_path = Path("/etc/udev/rules.d/10-vfio.rules")
    if udev_path.is_file() and udev_rule.strip() in udev_path.read_text():
        steps.append("udev rule already in place")
    else:
        _run(f"echo '{udev_rule.strip()}' | sudo tee {udev_path}", sudo=False)
        _run(["sudo", "udevadm", "control", "--reload-rules"])
        _run(["sudo", "udevadm", "trigger"])
        steps.append(f"Installed udev rule at {udev_path} and reloaded")

    # Check current groups
    groups = _run_output(f"groups {user}")
    steps.append(f"Current groups for {user}: {groups}")

    if "vfio" not in groups:
        steps.append("⚠ Re-login required for group membership to take effect")

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
    vers = _load_versions()
    want = vers.get("ICE_VER", "unknown")

    live_ver = _run_output("modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}' || echo 'not loaded'")
    live_path = _run_output("modinfo -n ice 2>/dev/null || echo 'not found'")

    oot_ko = f"/lib/modules/{_run_output('uname -r')}/updates/drivers/net/ethernet/intel/ice/ice.ko"
    oot_exists = Path(oot_ko).is_file()
    oot_ver = ""
    if oot_exists:
        oot_ver = _run_output(f"modinfo {oot_ko} 2>/dev/null | awk '/^version:/ {{print $2; exit}}'")

    is_oot = "updates/" in live_path
    matches = want in live_ver if live_ver != "not loaded" else False

    status = "OK" if (is_oot and matches) else "ACTION NEEDED"

    return (
        f"## ICE Driver Status: {status}\n"
        f"- Required version: {want}\n"
        f"- Live version: {live_ver}\n"
        f"- Live module path: {live_path}\n"
        f"- Out-of-tree module: {'EXISTS' if oot_exists else 'MISSING'} at {oot_ko}\n"
        f"  - OOT version: {oot_ver or 'N/A'}\n"
        f"- Using out-of-tree: {'YES' if is_oot else 'NO — stock kernel driver'}\n"
        + (
            "\n### Issue\n"
            "MTL requires the patched out-of-tree ICE driver for rate-limit pacing.\n"
            "Stock kernel ice does not support the iavf TM virtchnl messages.\n"
            "Use `build_ice_driver` tool or run:\n"
            "```\n"
            "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 bash .github/scripts/setup_environment.sh\n"
            "```"
            if not (is_oot and matches)
            else ""
        )
    )


@mcp.tool()
def ice_driver_rebuild() -> str:
    """
    Build and install the patched out-of-tree ICE driver.

    Downloads the correct ICE version from versions.env, applies MTL patches,
    builds, and installs. Reloads the module afterwards.

    WARNING: This will briefly disconnect NICs using the ice driver.
    """
    out = _run_output(
        "SETUP_ENVIRONMENT=0 "
        "SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )

    # Reload
    reload_out = _run_output(
        "sudo depmod -a && sudo rmmod irdma 2>/dev/null; "
        "sudo rmmod ice 2>/dev/null; sudo modprobe ice; "
        "modinfo ice | head -5"
    )

    return f"## ICE Driver Build\n{out}\n\n## Reload\n{reload_out}"


# ===================================================================
# TOOLS — DPDK
# ===================================================================


@mcp.tool()
def dpdk_status() -> str:
    """Check DPDK installation status and version."""
    ver = _run_output("pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'")
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
    out = _run_output(
        "SETUP_ENVIRONMENT=0 "
        "SETUP_BUILD_AND_INSTALL_DPDK=1 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    ver_after = _run_output("pkg-config --modversion libdpdk 2>/dev/null || echo 'not installed'")
    return f"## DPDK Build\n{out}\n\n## Result\nInstalled version: {ver_after}"


# ===================================================================
# TOOLS — MTL Build
# ===================================================================


@mcp.tool()
def build_mtl(mode: str = "release") -> str:
    """
    Build the Media Transport Library.

    Args:
        mode: Build mode — 'release', 'debug' (with ASAN), or 'debugonly' (debug without ASAN).
    """
    if mode not in ("release", "debug", "debugonly"):
        return f"Error: mode must be 'release', 'debug', or 'debugonly'. Got '{mode}'."

    cmd = "./build.sh"
    if mode != "release":
        cmd = f"./build.sh {mode}"

    out = _run_output(cmd, timeout=600)

    rxtxapp = "OK" if (REPO_ROOT / "tests/tools/RxTxApp/build/RxTxApp").is_file() else "MISSING"
    manager = "OK" if (REPO_ROOT / "build/manager/MtlManager").is_file() else "MISSING"
    libmtl = _run_output("ldconfig -p 2>/dev/null | grep libmtl || echo 'not found'")

    return (
        f"## MTL Build ({mode})\n{out}\n\n"
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
        return f"MtlManager already running (PID {pid})"

    _run(f"sudo {mgr} &", check=False, timeout=5)

    r2 = _run("pgrep -x MtlManager", check=False)
    if r2.returncode == 0:
        return f"MtlManager started (PID {r2.stdout.strip()})"
    return "Warning: MtlManager may not have started. Check `sudo MtlManager` manually."


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
    sections.append(f"## NTP\n- NTP active: {ntp} {'(should be disabled for PTP)' if ntp == 'yes' else ''}")

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
    out = _run_output("dpdk-devbind.py -s 2>/dev/null || dpdk-devbind -s 2>/dev/null || echo 'dpdk-devbind not found'")
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

    limits_conf = _run_output(f"grep -i memlock /etc/security/limits.conf 2>/dev/null || echo 'no memlock entries'")

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
# TOOLS — Full Setup Workflow
# ===================================================================


@mcp.tool()
def setup_for_vf_mode(pf_bdf: str, nr_hugepages: int = 2048, trusted: bool = False) -> str:
    """
    One-shot setup for VF mode (most common for Intel E810/E830).

    Steps: hugepages → create VFs → verify VFIO → reminder to start MtlManager.
    Run this after every reboot.

    Args:
        pf_bdf: PF BDF to create VFs from, e.g. '0000:af:00.0'
        nr_hugepages: Number of 2MB hugepages (default 2048 = 4GB)
        trusted: Create trusted VFs
    """
    results: list[str] = []

    # Hugepages
    results.append(hugepages_set(nr_hugepages))

    # Create VFs
    results.append(nic_create_vf(pf_bdf, trusted))

    # Reminder
    results.append(
        "## Next Steps\n"
        "1. Note the VF BDFs above — use them in JSON config `interfaces.name`\n"
        "2. Start MtlManager: use `manager_start` tool or `sudo MtlManager`\n"
        "3. Run your application, e.g.:\n"
        "   `./tests/tools/RxTxApp/build/RxTxApp --config_file config/tx_1v.json`"
    )

    return "\n\n---\n\n".join(results)


@mcp.tool()
def setup_for_pf_mode(pf_bdf: str, nr_hugepages: int = 2048) -> str:
    """
    One-shot setup for PF mode (bind PF directly to DPDK PMD).

    Use this for non-E800 series NICs or when VFs are not needed.
    WARNING: This takes the NIC away from the kernel entirely.

    Args:
        pf_bdf: PF BDF to bind to DPDK, e.g. '0000:32:00.0'
        nr_hugepages: Number of 2MB hugepages (default 2048 = 4GB)
    """
    results: list[str] = []

    results.append(hugepages_set(nr_hugepages))
    results.append(nic_bind_pmd(pf_bdf))

    results.append(
        "## Next Steps\n"
        f"1. Use PF BDF `{pf_bdf}` directly in JSON config `interfaces.name`\n"
        "2. Start MtlManager: use `manager_start` tool\n"
        "3. Run your application"
    )

    return "\n\n---\n\n".join(results)


@mcp.tool()
def setup_for_kernel_mode(nr_hugepages: int = 2048) -> str:
    """
    Setup for kernel socket transport mode (experimental).

    NICs stay bound to kernel driver. MTL uses UDP sockets instead of DPDK PMD.
    Lower performance but works with any NIC. Hugepages still needed for DPDK internals.

    Args:
        nr_hugepages: Number of 2MB hugepages (default 2048 = 4GB)
    """
    results: list[str] = []

    results.append(hugepages_set(nr_hugepages))

    # Show available kernel interfaces
    interfaces = _run_output("ip -br link show | grep -v lo")
    results.append(f"## Available Kernel Interfaces\n```\n{interfaces}\n```")

    results.append(
        "## Kernel Socket Config\n"
        "Use the kernel interface name (e.g., `ens801f0`) in JSON config:\n"
        "```json\n"
        '{\n'
        '    "interfaces": [\n'
        '        {\n'
        '            "name": "kernel:ens801f0",\n'
        '            "ip": "192.168.88.80"\n'
        '        }\n'
        '    ]\n'
        '}\n'
        "```\n\n"
        "Refer to sample configs:\n"
        "- TX: tests/tools/RxTxApp/script/kernel_socket_json/tx.json\n"
        "- RX: tests/tools/RxTxApp/script/kernel_socket_json/rx.json\n\n"
        "⚠ This is experimental — limited performance and pacing accuracy."
    )

    return "\n\n---\n\n".join(results)


@mcp.tool()
def setup_validation_base(
    nr_hugepages: int = 2048,
    build_mode: str = "release",
    include_ffmpeg_plugin: bool = False,
    include_gstreamer_plugin: bool = False,
) -> str:
    """
    One-shot broad host setup for validation environments.

    This covers non-pytest-specific responsibilities and is intended to replace
    generic setup logic in setup_validation.sh:
    - apt dependencies
    - DPDK build/install
    - ICE driver status/rebuild
    - MTL build
    - hugepages
    - CPU governor performance + confirmation
    - optional FFmpeg/GStreamer plugin builds

    Pytest-specific setup (NFS media mount, localhost root SSH, validation venv,
    topology/test config generation) remains in setup_validation.sh.

    Args:
        nr_hugepages: Number of 2MB hugepages (default 2048)
        build_mode: release/debug/debugonly (default release)
        include_ffmpeg_plugin: Build FFmpeg plugin (default False)
        include_gstreamer_plugin: Build GStreamer plugin (default False)
    """
    if build_mode not in ("release", "debug", "debugonly"):
        return (
            "Error: build_mode must be one of release/debug/debugonly. "
            f"Got '{build_mode}'."
        )

    results: list[str] = []

    results.append("## Step 1: Install Dependencies\n" + install_dependencies())
    results.append("## Step 2: Build DPDK\n" + dpdk_build())
    results.append("## Step 3: ICE Driver Status\n" + ice_driver_status())

    ice_status = ice_driver_status()
    if "ACTION NEEDED" in ice_status:
        results.append("## Step 3b: Rebuild ICE Driver\n" + ice_driver_rebuild())

    results.append("## Step 4: Build MTL\n" + build_mtl(build_mode))
    results.append("## Step 5: Hugepages\n" + hugepages_set(nr_hugepages))
    results.append(
        "## Step 6: CPU Governor\n" + _cpu_governor_set_and_confirm_performance()
    )

    if include_ffmpeg_plugin:
        results.append("## Step 7: FFmpeg Plugin\n" + build_ffmpeg_plugin())
    else:
        results.append("## Step 7: FFmpeg Plugin\nSkipped (include_ffmpeg_plugin=False)")

    if include_gstreamer_plugin:
        results.append("## Step 8: GStreamer Plugin\n" + build_gstreamer_plugin())
    else:
        results.append(
            "## Step 8: GStreamer Plugin\nSkipped (include_gstreamer_plugin=False)"
        )

    results.append(
        "## Next Step\n"
        "Run pytest-specific setup via .github/scripts/setup_validation.sh "
        "(NFS/SSH/venv/config stages)."
    )
    return "\n\n---\n\n".join(results)


# ===================================================================
# TOOLS — Ecosystem Plugins
# ===================================================================


@mcp.tool()
def build_ffmpeg_plugin() -> str:
    """Build the MTL FFmpeg plugin (ecosystem/ffmpeg_plugin)."""
    build_sh = REPO_ROOT / "ecosystem/ffmpeg_plugin/build.sh"
    if not build_sh.is_file():
        return "Error: ecosystem/ffmpeg_plugin/build.sh not found"

    out = _run_output(
        "ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN=1 "
        "SETUP_ENVIRONMENT=0 SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    return f"## FFmpeg Plugin Build\n{out}"


@mcp.tool()
def build_gstreamer_plugin() -> str:
    """Build the MTL GStreamer plugin (ecosystem/gstreamer_plugin)."""
    build_sh = REPO_ROOT / "ecosystem/gstreamer_plugin/build.sh"
    if not build_sh.is_file():
        return "Error: ecosystem/gstreamer_plugin/build.sh not found"

    out = _run_output(
        "ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=1 "
        "SETUP_ENVIRONMENT=0 SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=600,
    )
    return f"## GStreamer Plugin Build\n{out}"


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
    out = _run_output(
        "SETUP_ENVIRONMENT=1 "
        "SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=300,
    )
    return f"## Install Dependencies\n{out}"


# ===================================================================
# TOOLS — XDP / eBPF
# ===================================================================


@mcp.tool()
def build_ebpf_xdp() -> str:
    """
    Build and install eBPF/XDP support for MTL's XDP data path backend.

    This is optional — only needed if using XDP transport instead of DPDK PMD.
    """
    out = _run_output(
        "SETUP_ENVIRONMENT=0 "
        "SETUP_BUILD_AND_INSTALL_DPDK=0 "
        "SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 "
        "SETUP_BUILD_AND_INSTALL_EBPF_XDP=1 "
        "SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 "
        "MTL_BUILD_AND_INSTALL=0 "
        "bash .github/scripts/setup_environment.sh",
        timeout=300,
    )
    return f"## eBPF/XDP Build\n{out}"


# ===================================================================
# TOOLS — Quick Diagnostics
# ===================================================================


@mcp.tool()
def dmesg_tail(lines: int = 50, filter_str: str = "") -> str:
    """
    Show recent kernel messages (dmesg). Useful for diagnosing NIC/driver issues.

    Args:
        lines: Number of lines to show (default 50)
        filter_str: Optional grep filter, e.g. 'ice' or 'vfio'
    """
    if lines > 200:
        lines = 200

    cmd = f"sudo dmesg --time-format iso | tail -n {lines}"
    if filter_str:
        # Sanitize filter to prevent injection
        safe_filter = re.sub(r'[^a-zA-Z0-9_.\-: ]', '', filter_str)
        cmd = f"sudo dmesg --time-format iso | grep -i '{safe_filter}' | tail -n {lines}"

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
# Entry point
# ===================================================================

if __name__ == "__main__":
    mcp.run(transport="stdio")
