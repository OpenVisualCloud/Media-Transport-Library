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


def optimize_cpu_cores_for_turbo(host, isolcpus: str | None = None) -> str | None:
    """Offline unused CPU cores to maximize turbo frequency on active cores.

    On high-core-count servers (e.g. 256 logical CPUs), all cores staying
    in C0 limits turbo boost to the all-core frequency (~3600 MHz on GNR).
    By offlining unused cores, the remaining cores can reach higher turbo
    bins (e.g. 3800-3900 MHz with ~21 active cores).

    Keeps CPUs 0 through max(isolcpus) online and offlines the rest.
    Returns the original online CPU range string for later restoration,
    or None if no changes were made.
    """
    try:
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/online", shell=True
        )
        original_online = (result.stdout or "").strip()

        # Determine max CPU to keep online from isolcpus range
        max_cpu = 20  # default fallback
        if isolcpus:
            for part in isolcpus.replace(",", "-").split("-"):
                try:
                    max_cpu = max(max_cpu, int(part))
                except ValueError:
                    pass

        result = host.connection.execute_command("nproc --all", shell=True)
        total_cpus = int((result.stdout or "1").strip())
        if total_cpus <= max_cpu + 1:
            return None  # already few enough cores

        # Offline CPUs beyond the needed range
        host.connection.execute_command(
            f"for c in $(seq {max_cpu + 1} {total_cpus - 1}); do "
            f"echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done",
            shell=True,
        )
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/online", shell=True
        )
        new_online = (result.stdout or "").strip()
        logger.info(
            f"Host {host.name}: optimized turbo — "
            f"online CPUs: {new_online} (was {original_online})"
        )
        return original_online
    except Exception as e:
        logger.warning(f"Host {host.name}: could not optimize cores for turbo: {e}")
        return None


def restore_cpu_cores(host, original_online: str) -> None:
    """Bring all CPU cores back online after turbo optimization."""
    try:
        # Extract max CPU from original range (e.g. "0-255" -> 255)
        max_cpu = 0
        for part in original_online.replace(",", "-").split("-"):
            try:
                max_cpu = max(max_cpu, int(part))
            except ValueError:
                pass

        host.connection.execute_command(
            f"for c in $(seq 0 {max_cpu}); do "
            f"echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done",
            shell=True,
        )
        # A core that comes back online gets the default governor of the
        # driver; give it the one of cpu0, which stayed online.
        host.connection.execute_command(
            "g=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor); "
            f"for c in $(seq 1 {max_cpu}); do "
            "echo $g > /sys/devices/system/cpu/cpu$c/cpufreq/"
            "scaling_governor 2>/dev/null; done",
            shell=True,
        )
        result = host.connection.execute_command(
            "cat /sys/devices/system/cpu/online", shell=True
        )
        logger.info(
            f"Host {host.name}: restored CPU cores — "
            f"online: {(result.stdout or '').strip()}"
        )
    except Exception as e:
        logger.warning(f"Host {host.name}: could not restore CPU cores: {e}")


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
