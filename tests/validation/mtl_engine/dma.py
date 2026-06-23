# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""
DMA device management — detection, NUMA validation, setup.

Supports Intel DSA (Data Streaming Accelerator), CBDMA, and other DMA engines
compatible with DPDK.
"""

import logging
import re
from dataclasses import dataclass
from typing import Optional

logger = logging.getLogger(__name__)

# Intel DMA Accelerator device IDs
DMA_DEVICE_IDS = {
    "8086:0b25": "Intel Data Streaming Accelerator (DSA)",
    "8086:0b00": "Intel CBDMA (QuickData Technology)",
}

# Default device ID to search for (DSA first, then CBDMA as fallback)
DEFAULT_DMA_DEVICE_IDS = ["8086:0b25", "8086:0b00"]

# PCI address pattern: domain:bus:device.function (e.g., 0000:6a:01.0)
PCI_ADDRESS_PATTERN = re.compile(
    r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]$"
)

# Device ID pattern: vendor:device (e.g., 8086:0b25)
DEVICE_ID_PATTERN = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$")


@dataclass
class DMADevice:
    """Represents a DMA accelerator device with its properties."""

    pci_address: str  # Full PCI address (e.g., "0000:6a:01.0")
    numa_node: int  # NUMA node the device is attached to
    device_id: str = "8086:0b25"  # Device ID (vendor:device)
    verified: bool = False  # Whether device was verified to exist

    def __str__(self):
        return f"DMA({self.pci_address}, NUMA {self.numa_node})"


def is_pci_address(value: str) -> bool:
    """Check if value is a PCI address format (e.g., 0000:6a:01.0)."""
    return bool(PCI_ADDRESS_PATTERN.match(value))


def is_device_id(value: str) -> bool:
    """Check if value is a device ID format (e.g., 8086:0b25)."""
    return bool(DEVICE_ID_PATTERN.match(value))


def get_numa_node_from_pci(host, pci_address: str) -> int:
    """Get NUMA node for a PCI device (0 if unknown)."""
    try:
        # Normalize PCI address (add domain if missing)
        if len(pci_address.split(":")) == 2:
            pci_address = f"0000:{pci_address}"

        cmd = f"cat /sys/bus/pci/devices/{pci_address}/numa_node 2>/dev/null || echo -1"
        result = host.connection.execute_command(cmd, shell=True)

        if result and result.stdout:
            numa = int(result.stdout.strip())
            # -1 means NUMA info not available, default to 0
            return max(0, numa)
    except Exception as e:
        logger.debug(f"Could not get NUMA node for {pci_address}: {e}")

    # Fallback: estimate NUMA from PCI bus number
    # Typically bus < 0x80 is NUMA 0, >= 0x80 is NUMA 1
    try:
        bus = int(pci_address.split(":")[1], 16)
        return 0 if bus < 0x80 else 1
    except (ValueError, IndexError):
        return 0


def discover_dma_devices(host, device_id: str = "8086:0b25") -> list[DMADevice]:
    """Discover all DMA devices on a host by device ID via lspci."""
    devices = []

    try:
        cmd = f"lspci -Dn -d {device_id} 2>/dev/null || true"
        result = host.connection.execute_command(cmd, shell=True)

        if result and result.stdout:
            for line in result.stdout.strip().splitlines():
                # Format: "0000:6a:01.0 0880: 8086:0b25"
                parts = line.split()
                if parts:
                    pci_addr = parts[0]
                    numa = get_numa_node_from_pci(host, pci_addr)
                    devices.append(
                        DMADevice(
                            pci_address=pci_addr,
                            numa_node=numa,
                            device_id=device_id,
                            verified=True,
                        )
                    )

        if devices:
            logger.info(
                f"Discovered {len(devices)} DMA device(s) on {host.name}: "
                f"{[str(d) for d in devices]}"
            )
        else:
            logger.debug(f"No DMA devices with ID {device_id} found on {host.name}")

    except Exception as e:
        logger.warning(f"Failed to discover DMA devices on {host.name}: {e}")

    return devices


def verify_dma_device(host, pci_address: str) -> Optional[DMADevice]:
    """Verify a specific DMA device exists at the given PCI address."""
    try:
        # Normalize PCI address
        if len(pci_address.split(":")) == 2:
            pci_address = f"0000:{pci_address}"

        # Extract bus:device.function for lspci -s
        bdf = ":".join(pci_address.split(":")[1:])

        cmd = f"lspci -s {bdf} -n 2>/dev/null | grep -i '0b25\\|0b00\\|dsa\\|cbdma\\|accelerator' || true"
        result = host.connection.execute_command(cmd, shell=True)

        if result and result.stdout and result.stdout.strip():
            numa = get_numa_node_from_pci(host, pci_address)
            logger.info(
                f"Verified DMA device at {pci_address} on {host.name} (NUMA {numa})"
            )
            return DMADevice(
                pci_address=pci_address,
                numa_node=numa,
                verified=True,
            )
        else:
            logger.debug(f"DMA device not found at {pci_address} on {host.name}")
            return None

    except Exception as e:
        logger.warning(f"Failed to verify DMA device {pci_address} on {host.name}: {e}")
        return None


def _prepare_idxd_for_unbind(host) -> bool:
    """Prepare Intel DSA/IAA subsystem for safe device unbinding.

    The idxd kernel driver on certain kernels (e.g. 6.8.0-100-generic) has a
    use-after-free bug in idxd_remove → idxd_conf_device_release → __slab_free
    that causes a segfault when any DSA device is unbound from idxd.

    Additionally, vfio-pci has a built-in denylist that includes Intel DSA
    (8086:0b25), requiring disable_denylist=1 to allow binding.

    Strategy:
    1. Blacklist idxd modules (persistent across reboots)
    2. Disable all DSA/IAA workqueues via sysfs bus unbind
    3. Disable all DSA/IAA devices via sysfs bus unbind
    4. Remove idxd module chain (avoids per-device unbind segfault)
    5. Configure vfio-pci denylist

    Returns True if idxd modules were successfully removed (all DSA devices
    will have driver=none and can be bound to vfio-pci safely).
    """
    # Blacklist idxd modules so they don't reload after reboot
    host.connection.execute_command(
        'echo -e "blacklist iaa_crypto\\nblacklist idxd\\nblacklist idxd_bus" '
        "| sudo tee /etc/modprobe.d/blacklist-idxd.conf >/dev/null 2>&1 || true",
        shell=True,
    )

    # Check if idxd is loaded — if not, skip the disable/rmmod dance
    result = host.connection.execute_command(
        "lsmod | grep -q '^idxd ' && echo loaded || echo not_loaded",
        shell=True,
    )
    idxd_loaded = (
        result.stdout.strip() if result and result.stdout else ""
    ) == "loaded"

    modules_removed = False
    if idxd_loaded:
        # Disable all DSA/IAA workqueues (unbind from bus driver)
        # This releases refcounts so modules can be removed cleanly
        host.connection.execute_command(
            "for wq in $(ls /sys/bus/dsa/drivers/dsa/ 2>/dev/null | grep '^wq'); do "
            "  echo $wq | sudo tee /sys/bus/dsa/drivers/dsa/unbind 2>/dev/null; "
            "done; "
            "for wq in $(ls /sys/bus/dsa/drivers/crypto/ 2>/dev/null | grep '^wq'); do "
            "  echo $wq | sudo tee /sys/bus/dsa/drivers/crypto/unbind 2>/dev/null; "
            "done; "
            "for wq in $(ls /sys/bus/dsa/drivers/user/ 2>/dev/null | grep '^wq'); do "
            "  echo $wq | sudo tee /sys/bus/dsa/drivers/user/unbind 2>/dev/null; "
            "done",
            shell=True,
            timeout=30,
            expected_return_codes=None,
        )

        # Disable all DSA and IAA devices (unbind from idxd bus driver)
        host.connection.execute_command(
            "for dev in $(ls /sys/bus/dsa/drivers/idxd/ 2>/dev/null "
            "  | grep -E '^(dsa|iax)'); do "
            "  echo $dev | sudo tee /sys/bus/dsa/drivers/idxd/unbind 2>/dev/null; "
            "done",
            shell=True,
            timeout=30,
            expected_return_codes=None,
        )

        # Remove idxd module chain (cleanly unbinds all PCI devices from idxd)
        # This avoids the per-device unbind segfault in idxd_remove
        result = host.connection.execute_command(
            "sudo modprobe -r iaa_crypto 2>&1; "
            "sudo modprobe -r idxd 2>&1; "
            "sudo modprobe -r idxd_bus 2>&1; "
            "lsmod | grep -q '^idxd ' && echo still_loaded || echo removed",
            shell=True,
            timeout=30,
            expected_return_codes=None,
        )
        status = (
            result.stdout.strip().split("\n")[-1] if result and result.stdout else ""
        )
        modules_removed = status == "removed"

        if modules_removed:
            logger.info(
                f"idxd modules removed on {host.name} — " "DSA devices now unbound"
            )
        else:
            logger.warning(
                f"Could not remove idxd modules on {host.name} (modules in use). "
                f"Per-device unbind may segfault on buggy kernels."
            )

    # Reload vfio-pci with denylist disabled (DSA 8086:0b25 is on the denylist)
    # Only reload if denylist is currently enabled to avoid disrupting VFs
    result = host.connection.execute_command(
        "cat /sys/module/vfio_pci/parameters/disable_denylist 2>/dev/null || echo N",
        shell=True,
    )
    denylist_disabled = (
        result.stdout.strip().upper() if result and result.stdout else "N"
    )
    if denylist_disabled not in ("Y", "1"):
        host.connection.execute_command(
            "sudo modprobe -r vfio_pci 2>/dev/null; "
            "sudo modprobe vfio-pci disable_denylist=1 "
            "2>/dev/null || true",
            shell=True,
        )

    # Make disable_denylist persistent across reboots
    host.connection.execute_command(
        'echo "options vfio-pci disable_denylist=1" '
        "| sudo tee /etc/modprobe.d/vfio-pci-denylist.conf >/dev/null 2>&1 || true",
        shell=True,
    )

    return modules_removed


def _sysfs_bind_vfio(host, pci_address: str) -> bool:
    """Bind a PCI device to vfio-pci via sysfs (bypass dpdk-devbind.py).

    Uses driver_override → unbind (if needed) → direct vfio-pci/bind.
    The direct bind path avoids drivers_probe which can hang when the
    PCI device is in a partially-removed kernel state.

    Returns True if the device is successfully bound to vfio-pci.
    """
    # Check current driver first
    result = host.connection.execute_command(
        f"basename $(readlink /sys/bus/pci/devices/{pci_address}/driver "
        f"2>/dev/null) 2>/dev/null || echo none",
        shell=True,
        timeout=10,
    )
    current_drv = result.stdout.strip() if result and result.stdout else "none"

    if current_drv == "vfio-pci":
        return True

    # Set driver_override so the device will only match vfio-pci
    result = host.connection.execute_command(
        f'echo "vfio-pci" | sudo tee /sys/bus/pci/devices/{pci_address}/driver_override '
        f">/dev/null 2>&1; echo $?",
        shell=True,
        timeout=10,
    )
    rc = result.stdout.strip() if result and result.stdout else "1"
    if rc != "0":
        logger.debug(f"Could not set driver_override for {pci_address}: rc={rc}")
        return False

    # Unbind from current driver only if one is bound
    # Skip for driver=none — unbinding a device whose driver was force-removed
    # can trigger kernel segfaults (e.g. idxd use-after-free)
    if current_drv not in ("none", "vfio-pci"):
        result = host.connection.execute_command(
            f"echo {pci_address} | sudo tee "
            f"/sys/bus/pci/devices/{pci_address}/driver/unbind >/dev/null 2>&1; echo $?",
            shell=True,
            timeout=15,
            expected_return_codes=None,
        )
        # Segfault/timeout in tee is tolerable; device may end up unbound anyway

    # Bind directly to vfio-pci driver (avoids drivers_probe which can hang)
    host.connection.execute_command(
        f"echo {pci_address} | sudo tee /sys/bus/pci/drivers/vfio-pci/bind "
        f">/dev/null 2>&1 || true",
        shell=True,
        timeout=15,
        expected_return_codes=None,
    )

    # Verify
    result = host.connection.execute_command(
        f"basename $(readlink /sys/bus/pci/devices/{pci_address}/driver 2>/dev/null) "
        f"2>/dev/null || echo none",
        shell=True,
        timeout=10,
    )
    drv = result.stdout.strip() if result and result.stdout else "none"
    if drv == "vfio-pci":
        return True

    logger.debug(f"sysfs bind failed for {pci_address}: driver={drv}")
    return False


def bind_dma_to_vfio(host, pci_address: str) -> bool:
    """Bind a DMA device to vfio-pci and verify its IOMMU group is viable.

    DPDK requires ALL devices in an IOMMU group to use vfio-pci (or be
    unbound).  If the group contains devices with kernel drivers the
    DMA device cannot be used and this function returns False.
    """
    try:
        host.connection.execute_command("sudo modprobe vfio-pci", shell=True)

        # Check if already bound to vfio-pci
        result = host.connection.execute_command(
            f"sudo dpdk-devbind.py --status-dev dma 2>/dev/null"
            f" | grep '{pci_address}' || true",
            shell=True,
            timeout=10,
        )
        current_status = result.stdout.strip() if result and result.stdout else ""

        if "drv=vfio-pci" not in current_status:
            logger.info(f"Binding DMA {pci_address} to vfio-pci on {host.name}")

            # Prepare idxd subsystem once per host: disable workqueues, rmmod,
            # configure vfio-pci denylist.  Cached to avoid redundant work
            # when binding multiple DMA devices in a loop.
            if not getattr(host, "_idxd_prepared", False):
                _prepare_idxd_for_unbind(host)
                host._idxd_prepared = True

            # Try manual sysfs bind first (avoids dpdk-devbind.py kernel crashes)
            if not _sysfs_bind_vfio(host, pci_address):
                logger.debug(
                    f"sysfs bind failed for {pci_address}, " "trying dpdk-devbind.py"
                )
                host.connection.execute_command(
                    f"sudo dpdk-devbind.py -b vfio-pci {pci_address}",
                    shell=True,
                    timeout=60,
                    expected_return_codes=None,
                )

        # Verify the bind succeeded
        result = host.connection.execute_command(
            f"sudo dpdk-devbind.py --status-dev dma 2>/dev/null"
            f" | grep '{pci_address}' || true",
            shell=True,
            timeout=10,
        )
        bind_status = result.stdout.strip() if result and result.stdout else ""
        if "drv=vfio-pci" not in bind_status:
            logger.error(
                f"Failed to bind DMA {pci_address} to vfio-pci "
                f"on {host.name}: {bind_status or 'not found'}"
            )
            return False

        # ── IOMMU group viability check ──
        # Find peer devices in the same group that still have a kernel driver.
        result = host.connection.execute_command(
            f"iommu_grp=$(basename $(readlink "
            f"/sys/bus/pci/devices/{pci_address}/iommu_group 2>/dev/null) "
            f"2>/dev/null); "
            f'[ -n "$iommu_grp" ] && '
            f"for dev in /sys/kernel/iommu_groups/$iommu_grp/devices/*; do "
            f"  drv=$(basename $(readlink $dev/driver 2>/dev/null) 2>/dev/null); "
            f'  [ -n "$drv" ] && [ "$drv" != "vfio-pci" ] && '
            f"  echo $(basename $dev)=$drv; "
            f"done 2>/dev/null || true",
            shell=True,
        )
        blockers = result.stdout.strip() if result and result.stdout else ""
        if blockers:
            logger.warning(
                f"IOMMU group for DMA {pci_address} on {host.name} has "
                f"kernel-bound peers: {blockers} — device unusable by DPDK"
            )
            # Unbind to avoid leaving an orphaned vfio-pci binding
            host.connection.execute_command(
                f"sudo dpdk-devbind.py -u {pci_address} 2>/dev/null || true",
                shell=True,
            )
            return False

        logger.info(f"DMA {pci_address} on {host.name}: {bind_status}")
        logger.info(
            f"DMA {pci_address} bound to vfio-pci on {host.name} "
            f"(IOMMU group viable)"
        )
        return True
    except Exception as e:
        logger.error(f"Error binding DMA {pci_address} to vfio-pci on {host.name}: {e}")
        return False


def _collect_dma_candidates(host, dma_config: Optional[str] = None) -> list[DMADevice]:
    """Build the candidate device list from *dma_config* (PCI address, device
    ID, or ``None`` for auto-discovery with DSA-first fallback to CBDMA).
    """
    if dma_config and is_pci_address(dma_config):
        dev = verify_dma_device(host, dma_config)
        return [dev] if dev else []
    if dma_config and is_device_id(dma_config):
        return discover_dma_devices(host, dma_config)
    for device_id in DEFAULT_DMA_DEVICE_IDS:
        candidates = discover_dma_devices(host, device_id)
        if candidates:
            return candidates
    return []


def setup_host_dma(
    host, nic_pci_address: str, dma_config: Optional[str] = None, role: str = ""
) -> Optional[str]:
    """Auto-discover DMA device, bind to vfio-pci, return PCI address for --dma_dev.

    By default (dma_config=None), auto-discovers DMA devices on the host
    (DSA first, then CBDMA), sorted by NUMA affinity to the NIC.
    Optionally, *dma_config* can be a PCI address or device ID to target
    a specific device.

    Returns the PCI address of the first viable device, or None.
    """
    nic_numa = get_numa_node_from_pci(host, nic_pci_address)
    role_str = f" ({role})" if role else ""

    candidates = _collect_dma_candidates(host, dma_config)
    if not candidates:
        logger.info(f"DMA not available{role_str} on {host.name} (config={dma_config})")
        return None

    # Prefer same-NUMA devices, then fall back to cross-NUMA
    same_numa = [d for d in candidates if d.numa_node == nic_numa]
    diff_numa = [d for d in candidates if d.numa_node != nic_numa]
    ordered = same_numa + diff_numa

    # ── Try each candidate until one binds successfully ──
    for dev in ordered:
        if not bind_dma_to_vfio(host, dev.pci_address):
            logger.info(
                f"DMA {dev.pci_address} not usable on {host.name}, "
                f"trying next candidate"
            )
            continue

        numa_match = "SAME" if dev.numa_node == nic_numa else "DIFFERENT"
        device_type = DMA_DEVICE_IDS.get(dev.device_id, "DMA Accelerator")

        # Store DMA info on host for later display in results
        if not hasattr(host, "_dma_info"):
            host._dma_info = {}
        host._dma_info[role or "default"] = {
            "dma_device": dev.pci_address,
            "dma_numa": dev.numa_node,
            "nic_address": nic_pci_address,
            "nic_numa": nic_numa,
            "numa_match": numa_match,
            "device_type": device_type,
            "role": role,
        }

        if dev.numa_node != nic_numa:
            logger.warning(
                f"NUMA mismatch: NIC {nic_pci_address} (NUMA {nic_numa}) vs "
                f"DMA {dev.pci_address} (NUMA {dev.numa_node}) — "
                f"cross-NUMA DMA will have higher latency"
            )

        logger.info(
            f"DMA enabled{role_str}: {dev.pci_address} ({device_type}, "
            f"NUMA {dev.numa_node}) for NIC {nic_pci_address} (NUMA {nic_numa}) "
            f"on {host.name} — NUMA {numa_match}"
        )
        return dev.pci_address

    logger.info(
        f"DMA not available{role_str} on {host.name} — "
        f"no bindable device found (config={dma_config})"
    )
    return None


def setup_host_dma_all(
    host,
    nic_pci_address: str,
    dma_config: Optional[str] = None,
    role: str = "",
) -> Optional[str]:
    """Auto-discover ALL DMA devices, bind to vfio-pci, return comma-separated
    PCI address string ready for RxTxApp's ``--dma_dev`` argument.

    Like :func:`setup_host_dma` but binds *every* viable same-NUMA device
    instead of only the first one.

    Returns ``"addr1,addr2,..."`` or ``None`` if no device could be bound.
    """
    nic_numa = get_numa_node_from_pci(host, nic_pci_address)
    role_str = f" ({role})" if role else ""

    candidates = _collect_dma_candidates(host, dma_config)
    if not candidates:
        logger.info(
            f"DMA not available{role_str} on {host.name} " f"(config={dma_config})"
        )
        return None

    # Use only same-NUMA devices — cross-socket DSA causes DPDK EAL
    # SIGABRT.  Cross-NUMA within same socket also risks dmadev array
    # overflow since each DSA has 16 work queues.
    same_numa = [d for d in candidates if d.numa_node == nic_numa]
    if not same_numa:
        # No same-NUMA devices; fall back to closest cross-NUMA
        same_numa = candidates
        logger.warning(
            f"No same-NUMA DMA for NIC {nic_pci_address} "
            f"(NUMA {nic_numa}), falling back to cross-NUMA devices"
        )

    # ── Try ALL same-NUMA candidates ──
    bound_devices: list[DMADevice] = []
    for dev in same_numa:
        if not bind_dma_to_vfio(host, dev.pci_address):
            logger.info(f"DMA {dev.pci_address} not usable on {host.name}, " "skipping")
            continue
        bound_devices.append(dev)

    if not bound_devices:
        logger.info(
            f"DMA not available{role_str} on {host.name} — "
            f"no bindable device found (config={dma_config})"
        )
        return None

    # Build comma-separated list
    pci_list = ",".join(d.pci_address for d in bound_devices)
    device_types = {DMA_DEVICE_IDS.get(d.device_id, "DMA") for d in bound_devices}
    device_type_str = "/".join(sorted(device_types))
    numa_nodes = sorted({d.numa_node for d in bound_devices})

    # Store DMA info on host for later display in results
    numa_match = (
        "SAME" if all(d.numa_node == nic_numa for d in bound_devices) else "MIXED"
    )
    if not hasattr(host, "_dma_info"):
        host._dma_info = {}
    host._dma_info[role or "default"] = {
        "dma_device": pci_list,
        "dma_numa": numa_nodes,
        "nic_address": nic_pci_address,
        "nic_numa": nic_numa,
        "numa_match": numa_match,
        "device_type": device_type_str,
        "role": role,
        "num_devices": len(bound_devices),
    }

    logger.info(
        f"DMA enabled{role_str}: {len(bound_devices)} device(s) [{pci_list}] "
        f"({device_type_str}, NUMA {numa_nodes}) for NIC {nic_pci_address} "
        f"(NUMA {nic_numa}) on {host.name} \u2014 NUMA {numa_match}"
    )
    return pci_list
