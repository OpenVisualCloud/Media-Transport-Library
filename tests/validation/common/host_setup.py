# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Host readiness helpers — hugepages, CPU governor, isolation, PF link state.

These run once per host during session setup to ensure the system is
configured for DPDK / ST 2110 workloads.
"""

import logging
import time

logger = logging.getLogger(__name__)


def ensure_hugepage_access(host) -> None:
    """Ensure 1GB hugepages are allocated, mounted correctly, and writable.

    DPDK requires 1GB hugepages mapped via hugetlbfs.  On systems that
    allocate hugepages post-boot via sysfs (e.g. GNR/Granite Rapids where
    boot-time allocation via kernel cmdline hangs at EFI stub), this
    helper allocates 24 x 1GB per NUMA node if fewer than 24 exist per
    node, remounts /dev/hugepages with pagesize=1G if needed, and fixes
    permissions.
    """
    try:
        result = host.connection.execute_command(
            "cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages"
            " 2>/dev/null || echo 0",
            shell=True,
        )
        nr_1g = int((result.stdout or "0").strip())

        numa_result = host.connection.execute_command(
            "ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l",
            shell=True,
        )
        numa_count = max(1, int((numa_result.stdout or "1").strip()))
        target_per_node = 24
        target_total = target_per_node * numa_count

        if nr_1g < target_total:
            host.connection.execute_command(
                "for n in $(ls -d /sys/devices/system/node/node*"
                " 2>/dev/null | grep -o '[0-9]*$'); do "
                "sudo sh -c 'echo "
                f"{target_per_node}"
                " > /sys/devices/system/node/"
                "node$n/hugepages/"
                "hugepages-1048576kB/nr_hugepages'; "
                "done",
                shell=True,
            )
            logger.info(
                f"Host {host.name}: allocated 1GB hugepages"
                f" ({target_per_node} per node, {target_total} total)"
            )

        # Ensure /dev/hugepages is mounted with 1GB pagesize
        result = host.connection.execute_command(
            "mount | grep '/dev/hugepages'"
            " | grep -q 'pagesize=1024M' && echo ok || echo remount",
            shell=True,
        )
        if "remount" in (result.stdout or ""):
            host.connection.execute_command(
                "sudo umount /dev/hugepages 2>/dev/null; "
                "sudo mount -t hugetlbfs -o pagesize=1G nodev /dev/hugepages",
                shell=True,
            )
            logger.info(f"Host {host.name}: remounted /dev/hugepages with pagesize=1G")

        host.connection.execute_command(
            "sudo chmod 1777 /dev/hugepages 2>/dev/null || true",
            shell=True,
        )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not ensure hugepage access: {e}")


def ensure_turbo_boost_enabled(host) -> None:
    """Enable turbo boost if it is currently disabled.

    Checks intel_pstate/no_turbo first, then falls back to cpufreq/boost.
    Without turbo boost the single-core TX capacity drops ~26%.
    """
    try:
        # Try intel_pstate first
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null",
            shell=True,
        )
        val = (result.stdout or "").strip()
        if val == "1":
            host.connection.execute_command(
                "echo 0 | sudo tee"
                " /sys/devices/system/cpu/intel_pstate/no_turbo >/dev/null",
                shell=True,
            )
            logger.info(f"Host {host.name}: turbo boost enabled (intel_pstate)")
            return
        if val == "0":
            return  # already enabled

        # Fallback: cpufreq/boost
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null",
            shell=True,
        )
        val = (result.stdout or "").strip()
        if val == "0":
            host.connection.execute_command(
                "echo 1 | sudo tee"
                " /sys/devices/system/cpu/cpufreq/boost >/dev/null",
                shell=True,
            )
            logger.info(f"Host {host.name}: turbo boost enabled (cpufreq)")
            return
        if val == "1":
            return  # already enabled

        logger.warning(
            f"Host {host.name}: could not determine turbo boost state"
            " (neither intel_pstate nor cpufreq interface found)"
        )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not enable turbo boost: {e}")


def ensure_cpu_performance_governor(host) -> None:
    """Set the CPU frequency scaling governor to 'performance' on all cores."""
    try:
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" " 2>/dev/null",
            shell=True,
        )
        current = (result.stdout or "").strip()
        if current == "performance":
            return
        host.connection.execute_command(
            "command -v cpupower >/dev/null 2>&1 "
            "&& sudo cpupower frequency-set -g performance "
            "|| echo performance | sudo tee"
            " /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
            " >/dev/null",
            shell=True,
        )
        logger.info(
            f"Host {host.name}: CPU governor set to 'performance'" f" (was '{current}')"
        )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not set CPU performance governor: {e}")


def check_cpu_isolation(host) -> None:
    """Warn if the kernel was not booted with isolcpus/nohz_full/rcu_nocbs."""
    try:
        result = host.connection.execute_command("cat /proc/cmdline", shell=True)
        cmdline = (result.stdout or "").strip()
        has_isolcpus = "isolcpus=" in cmdline
        has_nohz = "nohz_full=" in cmdline
        has_rcu = "rcu_nocbs=" in cmdline
        if has_isolcpus and has_nohz and has_rcu:
            return
        missing = []
        if not has_isolcpus:
            missing.append("isolcpus")
        if not has_nohz:
            missing.append("nohz_full")
        if not has_rcu:
            missing.append("rcu_nocbs")
        logger.warning(
            f"Host {host.name}: kernel booted WITHOUT"
            f" {', '.join(missing)}. "
            f"Performance may be degraded. Add to GRUB and reboot."
        )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not check CPU isolation: {e}")


def ensure_pf_up(host, pf_pci: str) -> None:
    """Bring the PF interface UP if it is not already.

    VFs rely on the PF admin queue being functional.  If the PF was never
    brought UP (e.g. fresh boot), the VF probe fails with ENODEV (-19).
    """
    try:
        result = host.connection.execute_command(
            f"basename /sys/bus/pci/devices/{pf_pci}/net/* 2>/dev/null",
            shell=True,
        )
        pf_if = (result.stdout or "").strip()
        if not pf_if or pf_if == "*":
            return  # PF has no kernel interface (bound to vfio-pci itself)
        state_result = host.connection.execute_command(
            f"cat /sys/class/net/{pf_if}/operstate 2>/dev/null",
            shell=True,
        )
        state = (state_result.stdout or "").strip()
        if state != "up":
            host.connection.execute_command(f"sudo ip link set {pf_if} up", shell=True)
            time.sleep(2)
            logger.info(
                f"Host {host.name}: brought PF {pf_if} ({pf_pci}) UP"
                f" (was '{state}')"
            )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not ensure PF UP for {pf_pci}: {e}")
