# # SPDX-License-Identifier: BSD-3-Clause
# # Copyright 2025 Intel Corporation
import re

import pytest
from mfd_network_adapter import NetworkInterface


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
        # Match either:
        # 1. Lines with "vfio" (e.g., from create_vf output)
        # 2. Lines with just PCI addresses (e.g., from list command)
        vf_info_regex = r"(\d{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d+)"
        return re.findall(vf_info_regex, output)

    def vfio_list(self, pci_addr: str = "all") -> list:
        """Returns list of VFs created on host."""
        resp = self.connection.execute_command(
            f"{self.nicctl} list {pci_addr}", shell=True
        )
        return self._parse_vf_list(resp.stdout)

    def create_vfs(self, pci_id: str, num_of_vfs: int = 6) -> list:
        """Create VFs on NIC.
        :param pci_id: pci_id of the nic adapter
        :param num_of_vfs: number of VFs to create
        :return: returns list of created vfs
        """
        resp = self.connection.execute_command(
            f"sudo {self.nicctl} create_vf {pci_id} {num_of_vfs}", shell=True
        )
        return self._parse_vf_list(resp.stdout)

    def disable_vf(self, pci_id: str) -> None:
        """Remove VFs on NIC.
        :param pci_id: pci_id of the nic adapter
        """
        self.connection.execute_command(
            self.nicctl + " disable_vf " + pci_id, shell=True
        )

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

    def bind_pmd(self, pci_id: str) -> None:
        """Bind VF to DPDK PMD driver."""
        self.connection.execute_command(self.nicctl + " bind_pmd " + pci_id, shell=True)

    def bind_kernel(self, pci_id: str) -> None:
        """Bind VF to kernel driver."""
        self.connection.execute_command(
            self.nicctl + " bind_kernel " + pci_id, shell=True
        )


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
        for nicctl, interface, if_type in self.cleanups:
            if if_type.lower() == "vf":
                nicctl.disable_vf(interface)
            elif if_type.lower() == "pf":
                nicctl.bind_kernel(interface)
