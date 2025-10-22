# # SPDX-License-Identifier: BSD-3-Clause
# # Copyright 2025 Intel Corporation
import re

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
        vf_info_regex = r"(\d{4}[0-9a-fA-F:.]+)\(?\S*\)?\s+\S*\s*vfio"
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
        self.connection.execute_command(
            self.nicctl + " bind_pmd " + pci_id, shell=True
            )

    def bind_kernel(self, pci_id: str) -> None:
        """Bind VF to kernel driver."""
        self.connection.execute_command(
            self.nicctl + " bind_kernel " + pci_id, shell=True
            )


class InterfaceSetup:
    def __init__(self, hosts, mtl_path):
        self.hosts = hosts
        self.mtl_path = mtl_path
        self.nicctl_objs = {host.name: Nicctl(mtl_path, host) for host in hosts.values()}
        self.customs = []
        self.cleanups = []

    def get_test_interfaces(self, hosts, interface_type="VF", count=2):
        selected_interfaces = {k: [] for k in hosts.keys()}
        for host in hosts.values():
            if getattr(host.topology.extra_info, "custom_interface", None):
                selected_interfaces[host.name] = [
                    host.topology.extra_info["custom_interface"]]
                self.customs.append(host.name)
                if len(selected_interfaces[host.name]) < count:
                    raise Exception(f"Not enough interfaces for test on host {host.name} in extra_info.custom_interface")
            else:
                if interface_type == "VF":
                    vfs = self.nicctl_objs[host.name].create_vfs(host.network_interfaces[0].pci_address.lspci, count)
                    selected_interfaces[host.name] = vfs
                    self.register_cleanup(self.nicctl_objs[host.name], host.network_interfaces[0].pci_address.lspci, interface_type)
                elif interface_type == "PF":
                    try:
                        selected_interfaces[host.name] = []
                        for i in range(count):
                            self.nicctl_objs[host.name].bind_pmd(host.network_interfaces[i].pci_address.lspci)
                            selected_interfaces[host.name].append(str(host.network_interfaces[i]))
                            self.register_cleanup(self.nicctl_objs[host.name], host.network_interfaces[i].pci_address.lspci, interface_type)
                    except IndexError:
                        raise Exception(f"Not enough interfaces for test on host {host.name} in topology config.")
                elif interface_type == "VFxPF":
                    for i in range(count):
                        vfs = self.nicctl_objs[host.name].create_vfs(host.network_interfaces[i].pci_address.lspci, 1)
                        selected_interfaces[host.name].extend(vfs)
                        self.register_cleanup(self.nicctl_objs[host.name], host.network_interfaces[i].pci_address.lspci, "VF")
                else:
                    raise Exception(f"Unknown interface type {interface_type}")
        return selected_interfaces

    def get_test_interfaces_list(self, hosts, interface_type="VF", count=2):
        selected_interfaces = self.get_test_interfaces(hosts, interface_type, count)
        interface_list = []
        for host in hosts.values():
            interface_list.extend(selected_interfaces[host.name])
        return interface_list

    def register_cleanup(self, nicctl, interface, if_type):
        self.cleanups.append((nicctl, interface, if_type))

    def cleanup(self):
        for nicctl, interface, if_type in self.cleanups:
            if if_type == "VF":
                nicctl.disable_vf(interface)
            elif if_type == "PF":
                nicctl.bind_kernel(interface)
