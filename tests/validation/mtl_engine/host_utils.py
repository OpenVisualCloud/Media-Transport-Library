# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""Shared helpers for remote-host operations (kill, stop, VFIO reset, log)."""

import logging
import signal
import time

logger = logging.getLogger(__name__)


def kill_all_rxtxapp(*hosts) -> None:
    """Kill all RxTxApp processes on the given hosts."""
    for host in hosts:
        try:
            host.connection.execute_command(
                "pkill -9 -f '[R]xTxApp' || true",
                shell=True,
                timeout=15,
            )
        except Exception:
            pass


def stop_remote_process(process, host=None) -> None:
    """Stop a remote process via SIGKILL and clean up leftover RxTxApp."""
    try:
        process.kill(wait=None, with_signal=signal.SIGKILL)
    except Exception:
        pass
    time.sleep(2)
    if host is not None:
        kill_all_rxtxapp(host)


def reset_vfio_bindings(host, host_name: str, vf_list: list) -> None:
    """Unbind/rebind VFs to force VFIO group release after a DPDK crash."""
    kill_all_rxtxapp(host)
    time.sleep(2)

    for vf in vf_list:
        if not vf:
            continue
        try:
            host.connection.execute_command(
                f"echo '{vf}' > /sys/bus/pci/devices/{vf}/driver/unbind "
                f"2>/dev/null || true",
                shell=True,
                timeout=15,
            )
            time.sleep(1)
            host.connection.execute_command(
                f"dpdk-devbind.py -b vfio-pci {vf}",
                shell=True,
                timeout=30,
            )
            result = host.connection.execute_command(
                f"dpdk-devbind.py -s | grep '{vf}' | head -1",
                shell=True,
                timeout=15,
            )
            status = (result.stdout or "").strip()
            if "vfio-pci" in status:
                logger.debug(f"Reset VF {vf} on {host_name} — vfio-pci ✓")
            else:
                logger.warning(f"VF {vf} on {host_name} NOT bound: {status}")
        except Exception as e:
            logger.warning(f"Could not reset VF {vf} on {host_name}: {e}")


def ensure_vfio_bound(host, host_name: str, vf_list: list) -> bool:
    """Ensure all VFs are bound to vfio-pci; rebind any that aren't.

    Returns True if any VF had to be rebound.
    """
    any_rebound = False
    for vf in vf_list:
        if not vf:
            continue
        try:
            result = host.connection.execute_command(
                f"dpdk-devbind.py -s | grep '{vf}' | head -1",
                shell=True,
                timeout=15,
            )
            status = (result.stdout or "").strip()
            if "drv=vfio-pci" in status:
                continue  # Already properly bound

            logger.warning(
                f"VF {vf} on {host_name} not bound to vfio-pci "
                f"({status or 'no status'}), rebinding…"
            )
            any_rebound = True
            host.connection.execute_command(
                f"echo '{vf}' > /sys/bus/pci/devices/{vf}/driver/unbind "
                f"2>/dev/null || true",
                shell=True,
                timeout=15,
            )
            time.sleep(1)
            host.connection.execute_command(
                f"dpdk-devbind.py -b vfio-pci {vf}",
                shell=True,
                timeout=30,
            )
            result = host.connection.execute_command(
                f"dpdk-devbind.py -s | grep '{vf}' | head -1",
                shell=True,
                timeout=15,
            )
            new_status = (result.stdout or "").strip()
            if "vfio-pci" in new_status:
                logger.info(f"Rebound VF {vf} on {host_name} — vfio-pci ✓")
            else:
                logger.error(f"Failed to rebind VF {vf} on {host_name}: {new_status}")
        except Exception as e:
            logger.warning(f"Could not check VF {vf} on {host_name}: {e}")
    return any_rebound


def read_remote_log(host, log_path: str) -> list:
    """Read a log file from a remote host and return its lines."""
    try:
        result = host.connection.execute_command(
            f"cat {log_path}",
            shell=True,
        )
        return result.stdout.splitlines() if result.stdout else []
    except Exception:
        return []
