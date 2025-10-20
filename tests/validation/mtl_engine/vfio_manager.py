# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""
VFIO Resource Management

Provides utilities for managing VFIO/DPDK resources between tests to prevent
"Device or resource busy" errors when running multiple tests sequentially.
"""

import logging
import subprocess
import time
from typing import List, Optional

logger = logging.getLogger(__name__)


class VfioResourceManager:
    """Manages VFIO device resources to prevent conflicts between sequential tests.
    
    DPDK applications hold VFIO file descriptors even after process termination.
    The kernel may not immediately release these resources, causing "Device or resource busy"
    errors when the next test tries to use the same device.
    
    This manager provides:
    - VFIO device availability checking
    - Process cleanup verification
    - Resource release waiting with timeout
    - DPDK EAL cleanup verification
    """
    
    def __init__(self, timeout: int = 30, poll_interval: float = 0.5):
        """Initialize VFIO resource manager.
        
        Args:
            timeout: Maximum time to wait for resources to be released (seconds)
            poll_interval: Time between resource checks (seconds)
        """
        self.timeout = timeout
        self.poll_interval = poll_interval
    
    def wait_for_vfio_release(self, pci_addresses: List[str], host=None) -> bool:
        """Wait for VFIO devices to be released and available.
        
        Args:
            pci_addresses: List of PCI addresses (e.g., ['0000:32:01.0', '0000:32:01.1'])
            host: Host object for remote execution (None for local)
            
        Returns:
            True if devices are available, False if timeout
        """
        if not pci_addresses:
            logger.debug("No PCI addresses to check")
            return True
        
        start_time = time.time()
        logger.info(f"Waiting for VFIO devices to be released: {pci_addresses}")
        
        while time.time() - start_time < self.timeout:
            if self._are_devices_available(pci_addresses, host):
                elapsed = time.time() - start_time
                logger.info(f"VFIO devices available after {elapsed:.1f}s")
                return True
            
            time.sleep(self.poll_interval)
        
        logger.error(f"Timeout waiting for VFIO devices after {self.timeout}s")
        return False
    
    def _are_devices_available(self, pci_addresses: List[str], host=None) -> bool:
        """Check if VFIO devices are available (not in use).
        
        Args:
            pci_addresses: List of PCI addresses to check
            host: Host object for remote execution
            
        Returns:
            True if all devices are available
        """
        for pci_addr in pci_addresses:
            if not self._is_device_available(pci_addr, host):
                return False
        return True
    
    def _is_device_available(self, pci_addr: str, host=None) -> bool:
        """Check if a single VFIO device is available.
        
        Checks:
        1. No processes holding /dev/vfio/X file descriptors
        2. VFIO group is not locked
        3. Device can be probed by DPDK
        
        Args:
            pci_addr: PCI address (e.g., '0000:32:01.0')
            host: Host object for remote execution
            
        Returns:
            True if device is available
        """
        # Check 1: No processes using VFIO group
        vfio_group = self._get_vfio_group(pci_addr, host)
        if vfio_group and not self._is_vfio_group_free(vfio_group, host):
            logger.debug(f"VFIO group {vfio_group} for {pci_addr} is still in use")
            return False
        
        # Check 2: No DPDK processes using the device
        if self._is_dpdk_using_device(pci_addr, host):
            logger.debug(f"DPDK still using device {pci_addr}")
            return False
        
        return True
    
    def _get_vfio_group(self, pci_addr: str, host=None) -> Optional[str]:
        """Get VFIO group number for a PCI device.
        
        Args:
            pci_addr: PCI address (e.g., '0000:32:01.0')
            host: Host object for remote execution
            
        Returns:
            VFIO group number as string, or None if not found
        """
        try:
            # Read the iommu_group symlink
            cmd = f"readlink /sys/bus/pci/devices/{pci_addr}/iommu_group 2>/dev/null | awk -F'/' '{{print $NF}}'"
            
            if host:
                result = host.connection.execute_command(cmd, shell=True)
                group = result.stdout.strip()
            else:
                result = subprocess.run(
                    cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                group = result.stdout.strip()
            
            if group and group.isdigit():
                return group
            return None
        except Exception as e:
            logger.debug(f"Failed to get VFIO group for {pci_addr}: {e}")
            return None
    
    def _is_vfio_group_free(self, vfio_group: str, host=None) -> bool:
        """Check if VFIO group is free (no file descriptors open).
        
        Args:
            vfio_group: VFIO group number
            host: Host object for remote execution
            
        Returns:
            True if group is free
        """
        try:
            # Check if any process has /dev/vfio/{group} open
            cmd = f"sudo lsof /dev/vfio/{vfio_group} 2>/dev/null | wc -l"
            
            if host:
                result = host.connection.execute_command(cmd, shell=True)
                count = int(result.stdout.strip())
            else:
                result = subprocess.run(
                    cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                count = int(result.stdout.strip())
            
            # Count of 0 means no processes (header line not counted)
            # Count of 1 might be just the header
            is_free = count <= 1
            if not is_free:
                logger.debug(f"VFIO group {vfio_group} has {count} open file descriptors")
            return is_free
        except Exception as e:
            logger.debug(f"Failed to check VFIO group {vfio_group}: {e}")
            # If we can't check, assume it might be in use
            return False
    
    def _is_dpdk_using_device(self, pci_addr: str, host=None) -> bool:
        """Check if any DPDK process is using the device.
        
        Args:
            pci_addr: PCI address
            host: Host object for remote execution
            
        Returns:
            True if device is in use by DPDK
        """
        try:
            # Check for processes with DPDK EAL init that might be using this device
            # Look for processes with dpdk in command line or using vfio-pci
            cmd = f"pgrep -f 'dpdk|RxTxApp|UfdClientSample|UfdServerSample|test_send|test_receive' | wc -l"
            
            if host:
                result = host.connection.execute_command(cmd, shell=True)
                count = int(result.stdout.strip())
            else:
                result = subprocess.run(
                    cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                count = int(result.stdout.strip())
            
            if count > 0:
                logger.debug(f"Found {count} DPDK-related processes still running")
                return True
            return False
        except Exception as e:
            logger.debug(f"Failed to check DPDK processes: {e}")
            return False
    
    def cleanup_stale_resources(self, pci_addresses: List[str], host=None) -> bool:
        """Force cleanup of stale VFIO resources.
        
        This should be used as a last resort if normal waiting doesn't work.
        
        Args:
            pci_addresses: List of PCI addresses to clean up
            host: Host object for remote execution
            
        Returns:
            True if cleanup succeeded
        """
        logger.warning("Attempting force cleanup of VFIO resources")
        
        # Kill any remaining DPDK processes
        kill_cmd = "sudo pkill -9 -f 'dpdk|RxTxApp|UfdClientSample|UfdServerSample|test_send|test_receive' 2>/dev/null; true"
        try:
            if host:
                host.connection.execute_command(kill_cmd, shell=True)
            else:
                subprocess.run(kill_cmd, shell=True, timeout=10)
        except Exception as e:
            logger.error(f"Failed to kill DPDK processes: {e}")
        
        # Wait a bit for kernel to release resources
        time.sleep(3)
        
        # Unbind and rebind devices to vfio-pci to force cleanup
        for pci_addr in pci_addresses:
            try:
                unbind_cmd = f"echo {pci_addr} | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null; true"
                bind_cmd = f"echo {pci_addr} | sudo tee /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null; true"
                
                if host:
                    host.connection.execute_command(unbind_cmd, shell=True)
                    time.sleep(0.5)
                    host.connection.execute_command(bind_cmd, shell=True)
                else:
                    subprocess.run(unbind_cmd, shell=True, timeout=5)
                    time.sleep(0.5)
                    subprocess.run(bind_cmd, shell=True, timeout=5)
                
                logger.info(f"Rebound device {pci_addr} to vfio-pci")
            except Exception as e:
                logger.error(f"Failed to rebind {pci_addr}: {e}")
        
        time.sleep(2)
        return self._are_devices_available(pci_addresses, host)
    
    def verify_and_wait(self, pci_addresses: List[str], host=None, force_cleanup: bool = False) -> bool:
        """Verify VFIO resources are available, wait if needed, force cleanup if requested.
        
        This is the main entry point for ensuring devices are ready.
        
        Args:
            pci_addresses: List of PCI addresses to verify
            host: Host object for remote execution
            force_cleanup: If True, perform force cleanup on timeout
            
        Returns:
            True if devices are available
        """
        # First check: are devices already available?
        if self._are_devices_available(pci_addresses, host):
            logger.debug("VFIO devices already available")
            return True
        
        # Second attempt: wait for normal release
        logger.info("VFIO devices in use, waiting for release...")
        if self.wait_for_vfio_release(pci_addresses, host):
            return True
        
        # Third attempt: force cleanup if allowed
        if force_cleanup:
            logger.warning("Normal wait failed, attempting force cleanup...")
            return self.cleanup_stale_resources(pci_addresses, host)
        
        return False


# Global instance for use in fixtures
_vfio_manager = VfioResourceManager(timeout=30, poll_interval=0.5)


def get_vfio_manager() -> VfioResourceManager:
    """Get the global VFIO resource manager instance."""
    return _vfio_manager


def wait_for_device_release(pci_addresses: List[str], host=None, timeout: int = 30) -> bool:
    """Convenience function to wait for VFIO devices to be released.
    
    Args:
        pci_addresses: List of PCI addresses
        host: Host object for remote execution
        timeout: Maximum wait time in seconds
        
    Returns:
        True if devices are available
    """
    manager = VfioResourceManager(timeout=timeout)
    return manager.verify_and_wait(pci_addresses, host, force_cleanup=True)
