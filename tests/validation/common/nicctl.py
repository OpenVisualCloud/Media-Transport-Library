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

    def _parse_vf_list(self, output: str, all: bool = True) -> list:
        if "No VFs found" in output:
            return []
        vf_info_regex = r"\d{1,3}\s+(.+)\s+vfio" if all else r"(\d{4}:\S+)"
        return re.findall(vf_info_regex, output)

    def vfio_list(self, pci_addr: str = "all") -> list:
        """Returns list of VFs created on host."""
        resp = self.connection.execute_command(
            f"{self.nicctl} list {pci_addr}", shell=True
        )
        return self._parse_vf_list(resp.stdout, "all" in pci_addr)

    def create_vfs(self, pci_id: str, num_of_vfs: int = 6) -> list:
        """Create VFs on NIC.
        :param pci_id: pci_id of the nic adapter
        :param num_of_vfs: number of VFs to create
        :return: returns list of created vfs
        """
        resp = self.connection.execute_command(
            f"{self.nicctl} create_vf {pci_id} {num_of_vfs}", shell=True
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
