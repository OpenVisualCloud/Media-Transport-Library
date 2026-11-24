# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Host readiness helpers — hugepages, CPU governor, isolation, PF link state.

The check_* helpers only read a host setting; the host owns it. The other
helpers change the host for one test or session.
"""

import logging
import time

logger = logging.getLogger(__name__)


def check_hugepages(host) -> None:
    """Fail when the host has no free 2 MB or 1 GB hugepages.

    Hugepages are a host setting: the host reserves and mounts them. Without
    them each DPDK process stops in EAL with "Cannot get hugepage information".
    """
    result = host.connection.execute_command(
        "cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages"
        " /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages"
        " 2>/dev/null | awk '{sum += $1} END {print sum + 0}'",
        shell=True,
    )
    if int((result.stdout or "0").strip() or 0) == 0:
        raise RuntimeError(
            f"Host {host.name}: no free 2 MB or 1 GB hugepages; reserve them on the host"
        )


def check_turbo_boost_enabled(host) -> None:
    """Warn if turbo boost is disabled."""
    result = host.connection.execute_command(
        "{ grep -qx 1 /sys/devices/system/cpu/intel_pstate/no_turbo"
        " || grep -qx 0 /sys/devices/system/cpu/cpufreq/boost; } 2>/dev/null"
        " && echo disabled",
        shell=True,
        expected_return_codes=None,
    )
    if (result.stdout or "").strip() == "disabled":
        logger.warning(f"Host {host.name}: turbo boost is disabled")


def check_cpu_performance_governor(host) -> None:
    """Warn if a core does not use the 'performance' governor."""
    result = host.connection.execute_command(
        "cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null"
        " | sort -u",
        shell=True,
    )
    governors = (result.stdout or "").split()
    if governors and governors != ["performance"]:
        logger.warning(
            f"Host {host.name}: CPU governor is {', '.join(governors)},"
            " not 'performance'"
        )


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
