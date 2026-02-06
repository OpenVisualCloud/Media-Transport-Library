# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
DSA (Data Streaming Accelerator) Device Management

This module provides utilities for detecting, validating, and managing DSA devices
for use with MTL (Media Transport Library). DSA can be used to offload DMA operations.

Key Features:
- Auto-detect DSA devices on a host by device ID (e.g., 8086:0b25) or PCI address
- Support for CBDMA (8086:0b00) on older Intel platforms (Ice Lake)
- Validate NUMA node alignment between NIC and DSA for optimal performance
- Support both local and remote host DSA detection via SSH
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
PCI_ADDRESS_PATTERN = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]$")

# Device ID pattern: vendor:device (e.g., 8086:0b25)
DEVICE_ID_PATTERN = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$")


@dataclass
class DSADevice:
    """Represents a DSA device with its properties."""

    pci_address: str  # Full PCI address (e.g., "0000:6a:01.0")
    numa_node: int  # NUMA node the device is attached to
    device_id: str = "8086:0b25"  # Device ID (vendor:device)
    verified: bool = False  # Whether device was verified to exist

    def __str__(self):
        return f"DSA({self.pci_address}, NUMA {self.numa_node})"


def is_pci_address(value: str) -> bool:
    """Check if value is a PCI address format (e.g., 0000:6a:01.0)."""
    return bool(PCI_ADDRESS_PATTERN.match(value))


def is_device_id(value: str) -> bool:
    """Check if value is a device ID format (e.g., 8086:0b25)."""
    return bool(DEVICE_ID_PATTERN.match(value))


def get_numa_node_from_pci(host, pci_address: str) -> int:
    """
    Get NUMA node for a PCI device.

    Args:
        host: Host object with connection
        pci_address: PCI address (e.g., "0000:6a:01.0")

    Returns:
        NUMA node number, or 0 if unable to determine
    """
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


def discover_dsa_devices(host, device_id: str = "8086:0b25") -> list[DSADevice]:
    """
    Discover all DSA devices on a host by device ID.

    Args:
        host: Host object with connection
        device_id: PCI device ID to search for (default: Intel DSA 8086:0b25)

    Returns:
        List of DSADevice objects found on the host
    """
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
                        DSADevice(
                            pci_address=pci_addr,
                            numa_node=numa,
                            device_id=device_id,
                            verified=True,
                        )
                    )

        if devices:
            logger.info(f"Discovered {len(devices)} DSA device(s) on {host.name}: " f"{[str(d) for d in devices]}")
        else:
            logger.debug(f"No DSA devices with ID {device_id} found on {host.name}")

    except Exception as e:
        logger.warning(f"Failed to discover DSA devices on {host.name}: {e}")

    return devices


def verify_dsa_device(host, pci_address: str) -> Optional[DSADevice]:
    """
    Verify that a specific DSA device exists at the given PCI address.

    Args:
        host: Host object with connection
        pci_address: PCI address to verify (e.g., "0000:6a:01.0")

    Returns:
        DSADevice if found, None otherwise
    """
    try:
        # Normalize PCI address
        if len(pci_address.split(":")) == 2:
            pci_address = f"0000:{pci_address}"

        # Extract bus:device.function for lspci -s
        bdf = ":".join(pci_address.split(":")[1:])

        cmd = f"lspci -s {bdf} -n 2>/dev/null | grep -i '0b25\\|dsa\\|accelerator' || true"
        result = host.connection.execute_command(cmd, shell=True)

        if result and result.stdout and result.stdout.strip():
            numa = get_numa_node_from_pci(host, pci_address)
            logger.info(f"Verified DSA device at {pci_address} on {host.name} (NUMA {numa})")
            return DSADevice(
                pci_address=pci_address,
                numa_node=numa,
                verified=True,
            )
        else:
            logger.debug(f"DSA device not found at {pci_address} on {host.name}")
            return None

    except Exception as e:
        logger.warning(f"Failed to verify DSA device {pci_address} on {host.name}: {e}")
        return None


def get_dsa_for_nic(
    host,
    nic_pci_address: str,
    dsa_config: Optional[str] = None,
    require_same_numa: bool = True,
) -> Optional[DSADevice]:
    """
    Get the best DSA device for a given NIC, with NUMA validation.

    This is the main entry point for getting a DSA device. It will:
    1. If dsa_config is a PCI address, verify that device exists
    2. If dsa_config is a device ID (e.g., 8086:0b25), auto-discover matching devices
    3. If dsa_config is None, try auto-discovery with default Intel DSA ID
    4. Validate NUMA alignment between NIC and DSA
    5. Return the best matching DSA device

    Args:
        host: Host object with connection
        nic_pci_address: PCI address of the NIC (e.g., "0000:31:00.1")
        dsa_config: DSA device specification - can be:
            - PCI address (e.g., "0000:6a:01.0")
            - Device ID (e.g., "8086:0b25")
            - None for auto-discovery
        require_same_numa: If True, warn when DSA and NIC are on different NUMA nodes

    Returns:
        DSADevice if found and valid, None otherwise
    """
    nic_numa = get_numa_node_from_pci(host, nic_pci_address)
    logger.debug(f"NIC {nic_pci_address} is on NUMA node {nic_numa}")

    dsa_device = None

    # Case 1: Specific PCI address provided
    if dsa_config and is_pci_address(dsa_config):
        dsa_device = verify_dsa_device(host, dsa_config)
        if not dsa_device:
            logger.warning(
                f"╔══════════════════════════════════════════════════════════════╗\n"
                f"║  DSA DEVICE NOT FOUND                                        ║\n"
                f"║  Configured: {dsa_config:<46} ║\n"
                f"║  Host: {host.name:<52} ║\n"
                f"║  DSA will NOT be used for this test                          ║\n"
                f"╚══════════════════════════════════════════════════════════════╝"
            )
            return None

    # Case 2: Device ID provided (e.g., 8086:0b25) - discover matching devices
    elif dsa_config and is_device_id(dsa_config):
        devices = discover_dsa_devices(host, dsa_config)
        if not devices:
            logger.warning(
                f"╔══════════════════════════════════════════════════════════════╗\n"
                f"║  NO DMA DEVICES FOUND                                        ║\n"
                f"║  Device ID: {dsa_config:<47} ║\n"
                f"║  Host: {host.name:<52} ║\n"
                f"║  DMA offload will NOT be used for this test                  ║\n"
                f"╚══════════════════════════════════════════════════════════════╝"
            )
            return None

        # Prefer DSA on same NUMA node as NIC
        same_numa = [d for d in devices if d.numa_node == nic_numa]
        dsa_device = same_numa[0] if same_numa else devices[0]

    # Case 3: Auto-discovery - try DSA first, then CBDMA as fallback
    elif dsa_config is None:
        devices = []
        for device_id in DEFAULT_DMA_DEVICE_IDS:
            devices = discover_dsa_devices(host, device_id)
            if devices:
                logger.info(f"Auto-discovered {DMA_DEVICE_IDS.get(device_id, device_id)} on {host.name}")
                break

        if devices:
            same_numa = [d for d in devices if d.numa_node == nic_numa]
            dsa_device = same_numa[0] if same_numa else devices[0]
        else:
            logger.debug(f"No DMA accelerator devices auto-discovered on {host.name}")
            return None

    else:
        logger.warning(f"Invalid DSA config format: {dsa_config}")
        return None

    # NUMA validation
    if dsa_device and require_same_numa and dsa_device.numa_node != nic_numa:
        logger.warning(
            f"╔══════════════════════════════════════════════════════════════╗\n"
            f"║  ⚠️  NUMA NODE MISMATCH - PERFORMANCE WARNING                 ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  NIC: {nic_pci_address:<20} NUMA: {nic_numa:<22} ║\n"
            f"║  DSA: {dsa_device.pci_address:<20} NUMA: {dsa_device.numa_node:<22} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  Cross-NUMA DMA operations will have higher latency!         ║\n"
            f"║  Consider using a DSA device on NUMA node {nic_numa}                  ║\n"
            f"╚══════════════════════════════════════════════════════════════╝"
        )

    return dsa_device


def get_host_dsa_config(host, topology_config: dict = None) -> Optional[str]:
    """
    Get DSA configuration for a host from topology config.

    Looks for dsa_device or dsa_address in multiple locations:
    1. Host object attributes (dsa_device, dsa_address)
    2. conftest.py extracted extra config (for fields filtered from TopologyModel)
    3. Host-level config in hosts list (topology_config dict)
    4. Network interface config

    Args:
        host: Host object
        topology_config: Optional topology configuration dict

    Returns:
        DSA config string (PCI address or device ID), or None if not configured
    """
    # First try to get from host object if it has dsa attribute
    if hasattr(host, "dsa_device"):
        return host.dsa_device

    if hasattr(host, "dsa_address"):
        return host.dsa_address

    # Try to get from conftest.py extracted extra config
    # This is where dsa_device ends up after being filtered from TopologyModel
    try:
        from conftest import get_host_extra_config
        extra_config = get_host_extra_config(host.name)
        if "dsa_device" in extra_config:
            return extra_config["dsa_device"]
        if "dsa_address" in extra_config:
            return extra_config["dsa_address"]
    except ImportError:
        pass  # conftest not available, continue with other methods

    # Try topology config dict directly
    if topology_config:
        # Check hosts list for direct config
        for host_cfg in topology_config.get("hosts", []):
            if host_cfg.get("name") == host.name:
                # Check for dsa_device or dsa_address at host level
                if "dsa_device" in host_cfg:
                    return host_cfg["dsa_device"]
                if "dsa_address" in host_cfg:
                    return host_cfg["dsa_address"]

                # Check network_interfaces for DSA config
                for nic_cfg in host_cfg.get("network_interfaces", []):
                    if "dsa_device" in nic_cfg:
                        return nic_cfg["dsa_device"]
                    if "dsa_address" in nic_cfg:
                        return nic_cfg["dsa_address"]

    return None


def setup_host_dsa(
    host, nic_pci_address: str, dsa_config: Optional[str] = None, role: str = ""
) -> Optional[str]:
    """
    Setup and validate DSA for a host, returning the PCI address to use.

    This is the high-level function that tests should call to get a validated
    DSA device address for use with RxTxApp's --dma_dev parameter.

    Args:
        host: Host object with connection
        nic_pci_address: PCI address of the NIC being used
        dsa_config: DSA configuration (PCI address, device ID, or None for auto)
        role: Role of this host in the test (e.g., "TX", "RX", "TX/RX")

    Returns:
        DSA PCI address string if available and valid, None otherwise
    """
    nic_numa = get_numa_node_from_pci(host, nic_pci_address)
    dsa = get_dsa_for_nic(host, nic_pci_address, dsa_config, require_same_numa=True)

    # Build title with role if provided
    role_str = f" ({role})" if role else ""
    title = f"DSA CONFIGURATION SUMMARY{role_str}"

    if dsa:
        numa_match = "✓ SAME" if dsa.numa_node == nic_numa else "✗ DIFFERENT"
        device_type = DMA_DEVICE_IDS.get(dsa.device_id, "DMA Accelerator")

        # Store DSA info on host for later display in results
        if not hasattr(host, "_dsa_info"):
            host._dsa_info = {}
        host._dsa_info[role or "default"] = {
            "dsa_device": dsa.pci_address,
            "dsa_numa": dsa.numa_node,
            "nic_address": nic_pci_address,
            "nic_numa": nic_numa,
            "numa_match": numa_match,
            "device_type": device_type,
            "role": role,
        }

        logger.info(
            f"\n"
            f"╔══════════════════════════════════════════════════════════════╗\n"
            f"║  {title:<60} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  Host:        {host.name:<46} ║\n"
            f"║  Role:        {role if role else 'N/A':<46} ║\n"
            f"║  NIC:         {nic_pci_address:<46} ║\n"
            f"║  NIC NUMA:    {nic_numa:<46} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  DSA Device:  {dsa.pci_address:<46} ║\n"
            f"║  DSA NUMA:    {dsa.numa_node:<46} ║\n"
            f"║  Device Type: {device_type:<46} ║\n"
            f"║  NUMA Match:  {numa_match:<46} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  Status:      ✓ DSA ENABLED                                  ║\n"
            f"╚══════════════════════════════════════════════════════════════╝"
        )
        return dsa.pci_address
    else:
        # Check what DMA devices are available for diagnostic info
        available_devices = []
        for dev_id in DEFAULT_DMA_DEVICE_IDS:
            devices = discover_dsa_devices(host, dev_id)
            if devices:
                available_devices.extend([(d.pci_address, d.numa_node, dev_id) for d in devices])

        logger.info(
            f"\n"
            f"╔══════════════════════════════════════════════════════════════╗\n"
            f"║  {title:<60} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  Host:        {host.name:<46} ║\n"
            f"║  Role:        {role if role else 'N/A':<46} ║\n"
            f"║  NIC:         {nic_pci_address:<46} ║\n"
            f"║  NIC NUMA:    {nic_numa:<46} ║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  DSA Config:  {str(dsa_config):<46} ║\n"
            f"║  Available:   {len(available_devices)} DMA device(s) on host{' ' * 26}║\n"
            f"╠══════════════════════════════════════════════════════════════╣\n"
            f"║  Status:      ✗ DSA NOT AVAILABLE                            ║\n"
            f"╚══════════════════════════════════════════════════════════════╝"
        )

        if available_devices:
            logger.info("Available DMA devices on host:")
            for pci, numa, dev_id in available_devices:
                dev_type = DMA_DEVICE_IDS.get(dev_id, dev_id)
                logger.info(f"  - {pci} (NUMA {numa}) - {dev_type}")

        return None


def get_dsa_info(host, role: str = "") -> Optional[dict]:
    """
    Get stored DSA configuration info for a host.

    This returns the DSA info that was stored during setup_host_dsa().
    Useful for displaying DSA details in test results.

    Args:
        host: Host object
        role: Role to look up (e.g., "TX", "RX")

    Returns:
        Dict with DSA info or None if not set up
    """
    if hasattr(host, "_dsa_info"):
        return host._dsa_info.get(role or "default")
    return None
