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


def bind_dma_to_vfio(host, pci_address: str) -> bool:
    """Bind a DMA device to vfio-pci and verify its IOMMU group is viable.

    DPDK requires ALL devices in an IOMMU group to use vfio-pci (or be
    unbound).  If the group contains devices with kernel drivers the
    DMA device cannot be used and this function returns False.
    """
    try:
        host.connection.execute_command("sudo modprobe vfio-pci", shell=True)

        # Bind to vfio-pci if not already
        result = host.connection.execute_command(
            f"dpdk-devbind.py -s | grep '{pci_address}'",
            shell=True,
        )
        current = result.stdout.strip() if result and result.stdout else ""
        if "drv=vfio-pci" not in current:
            logger.info(f"Binding DMA {pci_address} to vfio-pci on {host.name}")
            host.connection.execute_command(
                f"sudo dpdk-devbind.py -b vfio-pci {pci_address}",
                shell=True,
            )

        # Verify the bind succeeded
        result = host.connection.execute_command(
            f"dpdk-devbind.py -s | grep '{pci_address}'",
            shell=True,
        )
        if not (result and result.stdout and "drv=vfio-pci" in result.stdout):
            logger.error(f"Failed to bind DMA {pci_address} to vfio-pci on {host.name}")
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

        logger.info(
            f"DMA {pci_address} bound to vfio-pci on {host.name} "
            f"(IOMMU group viable)"
        )
        return True
    except Exception as e:
        logger.error(f"Error binding DMA {pci_address} to vfio-pci on {host.name}: {e}")
        return False


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

    # ── Collect candidate devices ──
    if dma_config and is_pci_address(dma_config):
        dev = verify_dma_device(host, dma_config)
        candidates = [dev] if dev else []
    elif dma_config and is_device_id(dma_config):
        candidates = discover_dma_devices(host, dma_config)
    else:
        candidates = []
        for device_id in DEFAULT_DMA_DEVICE_IDS:
            candidates = discover_dma_devices(host, device_id)
            if candidates:
                break

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
