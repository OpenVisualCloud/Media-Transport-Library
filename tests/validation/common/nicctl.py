# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
import re
import time

import pytest
from mfd_network_adapter import NetworkInterface

logger = logging.getLogger(__name__)

# All nicctl shell calls must be bounded. The kernel sysfs writes done by
# nicctl.sh (sriov_numvfs, dpdk-devbind unbind) can block forever in
# pci_disable_sriov() / vfio_unregister_group_dev() if any process still
# holds /dev/vfio/<group> open. The timeouts below convert that infinite
# wait into a logged warning + escape-hatch (PCI remove/rescan).
_NICCTL_TIMEOUT = 30
_NICCTL_LONG_TIMEOUT = 60  # create_vf binds N VFs, allow more headroom
_VFIO_IDLE_TIMEOUT = 20  # how long to wait for VFIO group to be released


class Nicctl:
    """Wrapper of nicctl.sh script from Media-Transport-Library."""

    tool_name = "nicctl.sh"

    def __init__(self, mtl_path: str, host):
        self.host = host
        self.connection = host.connection
        self.mtl_path = mtl_path
        self.nicctl = self._nicctl_path

    @property
    def _nicctl_path(self) -> str:
        """Returns the path to the nicctl.sh script."""
        nicctl_path = getattr(self.host.topology.extra_info, "nicctl_path", None)
        if not nicctl_path:
            return str(self.connection.path(self.mtl_path, "script", self.tool_name))
        return str(self.connection.path(nicctl_path, self.tool_name))

    def _parse_vf_list(self, output: str) -> list:
        if "No VFs found" in output:
            return []
        # Match PCI addresses from both:
        # 1. list_vf output (bare PCI addresses, one per line)
        # 2. create_vf output ("Bind 0000:xx:yy.z(...) to vfio-pci success")
        vf_info_regex = r"(\d{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d+)"
        return re.findall(vf_info_regex, output)

    def vfio_list(self, pci_addr: str = "all") -> list:
        """Returns list of VFs created on host."""
        resp = self.connection.execute_command(
            f"{self.nicctl} list {pci_addr}", shell=True, timeout=_NICCTL_TIMEOUT
        )
        return self._parse_vf_list(resp.stdout)

    def create_vfs(self, pci_id: str, num_of_vfs: int = 6) -> list:
        """Create VFs on NIC, idempotently.

        If the PF already has at least ``num_of_vfs`` VFs bound to vfio-pci,
        return them without touching sysfs. This is the primary defence
        against the ``vfio_unregister_group_dev`` hang: a session that
        creates VFs once and reuses them across tests never triggers the
        kernel refcount race that the implicit ``echo 0 > sriov_numvfs``
        on re-creation would.

        :param pci_id: pci_id of the nic adapter
        :param num_of_vfs: minimum number of VFs required
        :return: list of VF PCI addresses (existing or freshly created)
        """
        existing = self.vfio_list(pci_id)
        if len(existing) >= num_of_vfs:
            logger.debug(
                "Reusing %d existing VFs on %s (requested %d)",
                len(existing),
                pci_id,
                num_of_vfs,
            )
            return existing
        self.connection.execute_command(
            f"sudo {self.nicctl} create_vf {pci_id} {num_of_vfs}",
            shell=True,
            timeout=_NICCTL_LONG_TIMEOUT,
        )
        # Allow VFIO bindings to stabilize after VF creation.
        # Without this delay, the first DPDK process to open a VF may
        # hit "Unable to reset device! Error: 11 (Resource temporarily
        # unavailable)" because the VFIO group/container is not fully
        # initialized yet.
        time.sleep(2)
        # Use vfio_list (nicctl.sh list) to get clean VF addresses.
        # The create_vf output mixes PF and VF PCI addresses in status
        # messages, while list_vf outputs only VF addresses.
        return self.vfio_list(pci_id)

    def disable_vf(self, pci_id: str) -> None:
        """Remove VFs on NIC.

        Robust against the well-known VFIO refcount hang: if any process
        still holds ``/dev/vfio/<group>`` open, the kernel sysfs write
        ``echo 0 > sriov_numvfs`` blocks forever in
        ``vfio_unregister_group_dev``. We mitigate in three layers:

          1. Wait for the IOMMU group to go idle (lsof poll).
          2. Run ``nicctl.sh disable_vf`` with a hard timeout.
          3. If it timed out, fall back to PCI remove/rescan — the
             kernel's documented escape hatch that does not wait on
             refcounts.

        :param pci_id: pci_id of the nic adapter
        """
        self._wait_vfio_idle(pci_id, timeout_s=_VFIO_IDLE_TIMEOUT)
        try:
            self.connection.execute_command(
                f"{self.nicctl} disable_vf {pci_id}",
                shell=True,
                timeout=_NICCTL_TIMEOUT,
            )
            return
        except Exception as e:
            logger.warning(
                "disable_vf %s timed out (%s); attempting PCI remove/rescan",
                pci_id,
                e,
            )
            self._force_pci_reset(pci_id)

    def bind_pmd(self, pci_id: str) -> None:
        """Bind VF to DPDK PMD driver."""
        self.connection.execute_command(
            self.nicctl + " bind_pmd " + pci_id,
            shell=True,
            timeout=_NICCTL_TIMEOUT,
        )

    def bind_kernel(self, pci_id: str) -> None:
        """Bind VF to kernel driver."""
        self._wait_vfio_idle(pci_id, timeout_s=_VFIO_IDLE_TIMEOUT)
        try:
            self.connection.execute_command(
                self.nicctl + " bind_kernel " + pci_id,
                shell=True,
                timeout=_NICCTL_TIMEOUT,
            )
        except Exception as e:
            logger.warning(
                "bind_kernel %s timed out (%s); attempting PCI remove/rescan",
                pci_id,
                e,
            )
            self._force_pci_reset(pci_id)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------
    def _wait_vfio_idle(self, pci_id: str, timeout_s: int) -> bool:
        """Poll until no userspace process holds /dev/vfio/<group> for *pci_id*.

        Returns True if idle (or no VFIO group bound), False on timeout.
        Never raises — best-effort precondition check.
        """
        try:
            res = self.connection.execute_command(
                f"readlink /sys/bus/pci/devices/{pci_id}/iommu_group 2>/dev/null "
                f"| awk -F/ '{{print $NF}}'",
                shell=True,
                timeout=5,
                expected_return_codes=None,
            )
            group = (res.stdout or "").strip()
        except Exception:
            return True  # cannot probe, let nicctl try
        if not group:
            return True
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                check = self.connection.execute_command(
                    f"sudo fuser /dev/vfio/{group} 2>/dev/null; echo EXIT:$?",
                    shell=True,
                    timeout=5,
                    expected_return_codes=None,
                )
                # fuser exit 1 == no process holds the fd
                if "EXIT:1" in (check.stdout or ""):
                    return True
            except Exception:
                return False
            time.sleep(0.5)
        logger.warning(
            "VFIO group %s for %s still busy after %ds — disable_vf may block",
            group,
            pci_id,
            timeout_s,
        )
        return False

    def _force_pci_reset(self, pci_id: str) -> None:
        """Last-resort: PCI remove + rescan. Does not wait on refcounts."""
        try:
            self.connection.execute_command(
                f"echo 1 | sudo tee /sys/bus/pci/devices/{pci_id}/remove "
                f">/dev/null 2>&1; sleep 1; "
                "echo 1 | sudo tee /sys/bus/pci/rescan >/dev/null 2>&1; true",
                shell=True,
                timeout=15,
                expected_return_codes=None,
            )
            logger.info("PCI %s removed and rescanned", pci_id)
        except Exception as e:
            logger.error("PCI reset of %s failed: %s", pci_id, e)

    def prepare_vfs_for_test(self, nic: NetworkInterface) -> list:
        """Prepare VFs for test."""
        if hasattr(self.host, "vfs") and self.host.vfs:
            if hasattr(self.host, "st2110_dev"):
                return [vf for vf in self.host.vfs if vf != self.host.st2110_dev]
        else:
            nic_pci = str(nic.pci_address)
            self.host.vfs = self.vfio_list(nic_pci)
            if not self.host.vfs:
                self.create_vfs(nic_pci)
                self.host.vfs = self.vfio_list(nic_pci)
        return self.host.vfs


class InterfaceSetup:
    def __init__(self, hosts, mtl_path, host_mtl_paths=None):
        self.hosts = hosts
        self.mtl_path = mtl_path
        self.host_mtl_paths = host_mtl_paths or {}
        self.nicctl_objs = {
            host.name: Nicctl(self.host_mtl_paths.get(host.name, mtl_path), host)
            for host in hosts.values()
        }
        self.customs = []
        self.cleanups = []
        self.ip_cleanups = []  # Track (connection, interface, ip) for cleanup

    def get_test_interfaces(self, interface_type="VF", count=2, host=None) -> dict:
        """
        Creates VFs and binds them into dpdk or bind PF into dpdk.

        :param interface_type: VF - create X VFs on first available test adapter,
                               PF - prepare list of PFs PCI addresses for test,
                               xxVFxPF - create xx number VFs per each PF. E.G if you need 3 VFs
                                    on every PF and you have 2 PF then pass 3VFxPF param with count=6
                                    if you type just VFxPF then one VF will be created on each PF.
        :param count: total number of VFs or PFs needed for test.
        :param host: You can specify host if you need to test only on this host
        :return: Returns dictionary with list of PCI addresses of VFs or PFs per host name.
        """
        if host:
            hosts = [host]
        else:
            hosts = list(self.hosts.values())
        selected_interfaces = {k.name: [] for k in hosts}
        for host in hosts:
            if getattr(host.topology.extra_info, "custom_interface", None):
                selected_interfaces[host.name] = [
                    host.topology.extra_info["custom_interface"]
                ]
                self.customs.append(host.name)
                if len(selected_interfaces[host.name]) < count:
                    raise Exception(
                        f"Not enough interfaces for test on host {host.name} in extra_info.custom_interface"
                    )
            else:
                if interface_type.lower() == "vf":
                    vfs = self.nicctl_objs[host.name].create_vfs(
                        host.network_interfaces[0].pci_address.lspci, count
                    )
                    selected_interfaces[host.name] = vfs
                    self.register_cleanup(
                        self.nicctl_objs[host.name],
                        host.network_interfaces[0].pci_address.lspci,
                        interface_type,
                    )
                elif interface_type.lower() == "pf":
                    try:
                        selected_interfaces[host.name] = []
                        for i in range(count):
                            self.nicctl_objs[host.name].bind_pmd(
                                host.network_interfaces[i].pci_address.lspci
                            )
                            selected_interfaces[host.name].append(
                                str(host.network_interfaces[i].pci_address.lspci)
                            )
                            self.register_cleanup(
                                self.nicctl_objs[host.name],
                                host.network_interfaces[i].pci_address.lspci,
                                interface_type,
                            )
                    except IndexError:
                        raise Exception(
                            f"Not enough interfaces for test on host {host.name} in topology config."
                        )
                elif "vfxpf" in interface_type.lower():
                    vfs_count = interface_type.lower().split("vfxpf")[0]
                    vfs_count = int(vfs_count) if vfs_count else 1
                    for i in range(count // vfs_count):
                        try:
                            vfs = self.nicctl_objs[host.name].create_vfs(
                                host.network_interfaces[i].pci_address.lspci, vfs_count
                            )
                            selected_interfaces[host.name].extend(vfs)
                            self.register_cleanup(
                                self.nicctl_objs[host.name],
                                host.network_interfaces[i].pci_address.lspci,
                                "VF",
                            )
                        except IndexError:
                            raise Exception(
                                f"Not enough interfaces for test on host {host.name} in topology config. "
                                f"Expected {count // vfs_count} adapters "
                                f"to be able to create total {count} VFs - {vfs_count} per adapter"
                            )
                else:
                    raise Exception(f"Unknown interface type {interface_type}")
        return selected_interfaces

    def get_interfaces_list_single(self, interface_type="VF", count=2) -> list:
        """
        Wrapper for get_test_interfaces method if you use only single node tests and need only list of interfaces
        """
        host = list(self.hosts.values())[0]
        selected_interfaces = self.get_test_interfaces(interface_type, count, host=host)
        return selected_interfaces[host.name]

    def get_mixed_interfaces_list_single(
        self,
        tx_interface_type: str = "PF",
        rx_interface_type: str = "VF",
        tx_index: int = 0,
        rx_index: int = 1,
    ) -> list:
        """
        Get a mixed interface list for single-node tests where TX and RX use different interface types.

        :param tx_interface_type: Type for TX interface (PF or VF)
        :param rx_interface_type: Type for RX interface (PF or VF)
        :param tx_index: Index of NIC from topology for TX
        :param rx_index: Index of NIC from topology for RX
        :return: List with [tx_interface, rx_interface]
        :raises pytest.skip: If not enough interfaces are configured
        """
        host = list(self.hosts.values())[0]

        if getattr(host.topology.extra_info, "custom_interface", None):
            pytest.skip(
                "Mixed interface tests are not supported with extra_info.custom_interface"
            )

        required_nics = max(tx_index, rx_index) + 1
        if len(host.network_interfaces) < required_nics:
            pytest.skip(
                "Mixed interface tests require at least "
                f"{required_nics} network interfaces in topology config. "
                f"Found {len(host.network_interfaces)} interface(s)."
            )

        tx_interface = self._get_single_interface_by_type(
            host, tx_interface_type, tx_index
        )
        rx_interface = self._get_single_interface_by_type(
            host, rx_interface_type, rx_index
        )

        return [tx_interface, rx_interface]

    def _get_single_interface_by_type(
        self, host, interface_type: str, index: int
    ) -> str:
        interface_type = interface_type.lower()
        nicctl = self.nicctl_objs[host.name]
        pci_addr = host.network_interfaces[index].pci_address.lspci

        if interface_type == "pf":
            nicctl.bind_pmd(pci_addr)
            self.register_cleanup(nicctl, pci_addr, "PF")
            return str(pci_addr)

        if interface_type == "vf":
            vfs = nicctl.create_vfs(pci_addr, 1)
            if not vfs:
                raise Exception(
                    f"Failed to create VF on PF {pci_addr} for host {host.name}"
                )
            self.register_cleanup(nicctl, pci_addr, "VF")
            return vfs[0]

        raise Exception(f"Unknown interface type {interface_type}")

    def get_pmd_kernel_interfaces(self, interface_type="VF") -> list:
        """
        Get hybrid interface list with one DPDK interface (VF/PF) and one kernel socket interface.
        Requires at least two interfaces configured in topology.

        :param interface_type: Type of DPDK interface (VF or PF)
        :return: List with [dpdk_interface, "kernel:<interface_name>"]
        :raises pytest.skip: If less than 2 interfaces configured in topology
        """

        # Check that we have at least 2 interfaces in topology config
        host = list(self.hosts.values())[0]
        if len(host.network_interfaces) < 2:
            pytest.skip(
                "PMD+kernel tests require at least 2 network interfaces in topology config. "
                f"Found {len(host.network_interfaces)} interface(s). "
                "Add a second interface to topology_config.yaml to run these tests."
            )

        # Get one interface for DPDK mode (creates VF/PF on first interface)
        dpdk_interfaces = self.get_interfaces_list_single(interface_type, count=1)

        # Use second interface from topology for kernel socket mode
        kernel_interface = host.network_interfaces[1].name

        return [dpdk_interfaces[0], f"kernel:{kernel_interface}"]

    def register_cleanup(self, nicctl, interface, if_type):
        self.cleanups.append((nicctl, interface, if_type))

    def register_ip_cleanup(self, connection, interface_name: str, ip_address: str):
        """Register kernel interface IP for cleanup after test."""
        self.ip_cleanups.append((connection, interface_name, ip_address))

    def cleanup_kernel_ips(self):
        """Remove all IP addresses configured on kernel interfaces during tests."""
        for connection, interface_name, ip_address in self.ip_cleanups:
            try:
                connection.execute_command(
                    f"sudo ip addr del {ip_address} dev {interface_name}", shell=True
                )
            except Exception:
                pass

    def cleanup(self):
        self.cleanup_kernel_ips()
        # Per-test interface cleanup intentionally does NOT call disable_vf:
        # VFs created at session start by the ``nic_port_list`` fixture are
        # reused across all tests (see Nicctl.create_vfs idempotency), which
        # eliminates the kernel ``vfio_unregister_group_dev`` hang window
        # entirely. We still rebind PFs back to the kernel driver because PF
        # driver state is not session-scoped — different tests need PF in
        # different drivers (ice vs vfio-pci). Each rebind is wrapped so a
        # single stuck device cannot cascade into the rest of the teardown.
        for nicctl, interface, if_type in self.cleanups:
            if if_type.lower() != "pf":
                continue
            try:
                nicctl.bind_kernel(interface)
            except Exception as e:
                logger.warning("PF rebind of %s failed: %s — continuing", interface, e)


def _cleanup_hugepages(host, host_name: str) -> None:
    """Remove stale DPDK hugepage mappings left after SIGKILL."""
    try:
        result = host.connection.execute_command(
            "ls /dev/hugepages/rtemap_* 2>/dev/null | wc -l",
            shell=True,
            timeout=10,
        )
        count = int((result.stdout or "0").strip())
        if count > 0:
            host.connection.execute_command(
                "sudo rm -f /dev/hugepages/rtemap_*",
                shell=True,
                timeout=15,
            )
            logger.info(f"Cleaned {count} stale hugepage files on {host_name}")
    except Exception as e:
        logger.warning(f"Hugepage cleanup on {host_name}: {e}")

    # Clean stale System V shared memory segments left by crashed MTL processes
    try:
        result = host.connection.execute_command(
            "ipcs -m 2>/dev/null | awk 'NR>3 && $6==0 {print $2}'",
            shell=True,
            timeout=10,
        )
        stale_ids = (result.stdout or "").strip().split()
        if stale_ids:
            for shm_id in stale_ids:
                host.connection.execute_command(
                    f"sudo ipcrm -m {shm_id}",
                    shell=True,
                    timeout=5,
                )
            logger.info(
                f"Cleaned {len(stale_ids)} stale SysV SHM segments on {host_name}"
            )
    except Exception as e:
        logger.warning(f"SysV SHM cleanup on {host_name}: {e}")


def _flr_rebind_vf(host, vf: str, host_name: str) -> bool:
    """Unbind, perform FLR, and rebind a single VF to vfio-pci.

    Returns True if the VF ended up bound to vfio-pci.
    """
    # Unbind
    host.connection.execute_command(
        f"sudo sh -c \"echo '{vf}' > /sys/bus/pci/devices/{vf}/driver/unbind\" "
        f"2>/dev/null || true",
        shell=True,
        timeout=15,
    )
    time.sleep(0.5)

    # Function Level Reset — clears PF queue state
    host.connection.execute_command(
        f'sudo sh -c "echo 1 > /sys/bus/pci/devices/{vf}/reset" '
        f"2>/dev/null || true",
        shell=True,
        timeout=15,
    )
    time.sleep(1)

    # Rebind to vfio-pci
    host.connection.execute_command(
        f"sudo dpdk-devbind.py -b vfio-pci {vf}",
        shell=True,
        timeout=30,
    )
    result = host.connection.execute_command(
        f"sudo dpdk-devbind.py -s | grep '{vf}' | head -1",
        shell=True,
        timeout=15,
    )
    status = (result.stdout or "").strip()
    bound = "vfio-pci" in status
    if bound:
        logger.debug(f"FLR + rebind VF {vf} on {host_name} — vfio-pci OK")
    else:
        logger.warning(f"VF {vf} on {host_name} NOT bound after FLR: {status}")
    return bound


def reset_vfio_bindings(host, host_name: str, vf_list: list) -> None:
    """Unbind/rebind VFs with FLR to fully reset after a DPDK crash.

    Also kills stale processes and cleans up hugepage files.
    """
    from mtl_engine.execute import kill_stale_processes

    kill_stale_processes(host)
    time.sleep(2)
    _cleanup_hugepages(host, host_name)

    for vf in vf_list:
        if not vf:
            continue
        try:
            _flr_rebind_vf(host, vf, host_name)
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
                f"sudo dpdk-devbind.py -s | grep '{vf}' | head -1",
                shell=True,
                timeout=15,
            )
            status = (result.stdout or "").strip()
            if "drv=vfio-pci" in status:
                continue

            logger.warning(
                f"VF {vf} on {host_name} not bound to vfio-pci "
                f"({status or 'no status'}), rebinding with FLR"
            )
            any_rebound = True
            if _flr_rebind_vf(host, vf, host_name):
                logger.info(f"Rebound VF {vf} on {host_name} — vfio-pci OK")
            else:
                logger.error(f"Failed to rebind VF {vf} on {host_name}")
        except Exception as e:
            logger.warning(f"Could not check VF {vf} on {host_name}: {e}")
    return any_rebound
