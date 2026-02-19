"""Collect platform SW/HW configuration from a remote host via SSH.

This module gathers system information from test hosts and returns it as
a dict with ``sw_configuration`` and ``hw_configuration`` keys.  The
conftest ``collect_platform_config`` fixture saves these per-host JSON
files into the log directory, and the report generator picks them up.

Usage::

    from common.collect_platform_info import collect_platform_info
    config = collect_platform_info(host)
"""

import logging
import re
from typing import Any, Dict

logger = logging.getLogger(__name__)


def _remote_cmd(host, cmd: str) -> str:
    """Execute a command on a remote host and return stripped stdout."""
    try:
        result = host.connection.execute_command(cmd)
        return (result.stdout or "").strip()
    except Exception as e:
        logger.warning(f"Remote command failed on {host.name}: {cmd!r} -> {e}")
        return ""


def _collect_sw_configuration(host) -> Dict[str, str]:
    """Collect software configuration from a remote host."""
    sw = {}

    # OS
    out = _remote_cmd(host, "grep PRETTY_NAME /etc/os-release")
    if "=" in out:
        sw["os"] = out.split("=", 1)[1].strip().strip('"')
    else:
        sw["os"] = out

    # Kernel
    sw["kernel"] = _remote_cmd(host, "uname -r")

    # MTL version — prefer git describe from build path
    build_path = (
        getattr(host, "build_path", "") or "/root/awilczyn/Media-Transport-Library"
    )
    mtl_ver = _remote_cmd(host, f"cd {build_path} && git describe --tags 2>/dev/null")
    if not mtl_ver:
        mtl_ver = _remote_cmd(host, "cat /proc/mtl/version 2>/dev/null")
    if not mtl_ver:
        mtl_ver = _remote_cmd(host, f"cat {build_path}/version.txt 2>/dev/null")
    sw["mtl_version"] = mtl_ver or "N/A"

    # DPDK version
    dpdk_ver = _remote_cmd(
        host, "pkg-config --modversion libdpdk 2>/dev/null || echo ''"
    )
    if dpdk_ver:
        sw["dpdk_driver"] = f"{dpdk_ver} (patched with MTL patches)"
    else:
        sw["dpdk_driver"] = "N/A"

    # ICE driver version
    ice_ver = _remote_cmd(host, "cat /sys/module/ice/version 2>/dev/null || echo ''")
    if ice_ver:
        # Check if it's a Kahawai build
        kahawai_ver = _remote_cmd(
            host, "modinfo ice 2>/dev/null | grep -i kahawai | head -1"
        )
        if kahawai_ver:
            # Extract the Kahawai version tag
            m = re.search(r"(Kahawai[_\s][\d.]+)", kahawai_ver, re.IGNORECASE)
            if m:
                sw["ice_version"] = f"{m.group(1)} (patched with MTL patches)"
            else:
                sw["ice_version"] = f"{ice_ver} (patched with MTL patches)"
        else:
            sw["ice_version"] = ice_ver
    else:
        sw["ice_version"] = "N/A"

    # DDP version
    ddp_ver = _remote_cmd(
        host,
        "cat /sys/kernel/debug/ice/*/ddp_pkg_version 2>/dev/null | head -1 || echo ''",
    )
    if not ddp_ver:
        ddp_ver = _remote_cmd(
            host,
            "dmesg | grep -i 'DDP package' | tail -1 | grep -oP 'version [\\d.]+' | awk '{print $2}'",
        )
    sw["ddp_version"] = ddp_ver or "N/A"

    # BIOS VT-X & VT-D (check IOMMU)
    cmdline = _remote_cmd(host, "cat /proc/cmdline")
    iommu_parts = []
    if "intel_iommu=on" in cmdline:
        iommu_parts.append("intel_iommu=on")
    if "iommu=pt" in cmdline:
        iommu_parts.append("iommu=pt")
    if iommu_parts:
        sw["bios_vtx_vtd"] = f"Enabled ({', '.join(iommu_parts)})"
    else:
        sw["bios_vtx_vtd"] = "Not detected"

    # NIC ports settings
    sw["nic_ports_settings"] = "NIC ports set as VF's (create_vf)."
    sw["nic_ports_order"] = "1.1, 1.2 ([card_no.port_no])."
    sw["video_files"] = "Stored in RAMdisk."

    # Hugepages
    hp_1g = _remote_cmd(
        host,
        "cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo 0",
    )
    hp_2m = _remote_cmd(
        host,
        "cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0",
    )
    hp_parts = []
    if hp_1g and hp_1g != "0":
        hp_parts.append(f"1G x {hp_1g}")
    if hp_2m and hp_2m != "0":
        hp_parts.append(f"2M x {hp_2m}")
    sw["hugepages"] = " + ".join(hp_parts) if hp_parts else "N/A"

    # CPU governor
    governor = _remote_cmd(
        host,
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A'",
    )
    sw["cpu_cores"] = (
        f'Set in "{governor}" mode.' if governor and governor != "N/A" else "N/A"
    )

    return sw


def _collect_hw_configuration(host, topo_pci_addrs: list = None) -> Dict[str, str]:
    """Collect hardware configuration from a remote host.

    Args:
        host: remote host with connection.execute_command().
        topo_pci_addrs: PCI BDF addresses from the topology config
            (e.g. ``["0000:15:00.1", "0000:15:00.0"]``).  When given,
            the **first** address is used to identify the test NIC
            instead of guessing via lspci grep.
    """
    hw = {}

    # Server model
    server = _remote_cmd(
        host, "sudo dmidecode -t system 2>/dev/null | grep 'Product Name' | head -1"
    )
    if ":" in server:
        hw["server"] = server.split(":", 1)[1].strip()
    else:
        hw["server"] = server or "N/A"

    # CPU info
    cpu_model = _remote_cmd(
        host, "grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2"
    )
    cpu_model = cpu_model.strip()
    nproc = _remote_cmd(host, "nproc")
    sockets = _remote_cmd(host, "lscpu | grep 'Socket(s):' | awk '{print $2}'")
    cores = _remote_cmd(host, "lscpu | grep 'Core(s) per socket:' | awk '{print $4}'")
    threads = _remote_cmd(host, "lscpu | grep 'Thread(s) per core:' | awk '{print $4}'")
    if cpu_model and nproc:
        detail = f"{nproc} CPUs"
        if sockets and cores and threads:
            detail = (
                f"{nproc} CPUs ({sockets} sockets x {cores} cores x {threads} threads)"
            )
        hw["cpu"] = f"{cpu_model}, {detail}"
    else:
        hw["cpu"] = "N/A"

    # Memory
    mem_type = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Type:' | grep -v 'Error\\|Detail\\|Unknown' | head -1 | awk '{print $2}'",
    )
    mem_speed = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Speed:' | grep -v 'Unknown\\|Configured' | head -1 | awk '{print $2, $3}'",
    )
    mem_total = _remote_cmd(host, "free -h | awk '/^Mem:/ {print $2}'")
    mem_dimm_size = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Size:' | grep -v 'No Module\\|Maximum' | head -1 | awk '{print $2, $3}'",
    )
    mem_dimm_count = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Size:' | grep -v 'No Module\\|Maximum' | wc -l",
    )
    parts = []
    if mem_type:
        parts.append(mem_type)
    if mem_speed:
        parts.append(mem_speed)
    if mem_total:
        total_str = mem_total.rstrip("GiMBT")
        try:
            total_gb = round(float(total_str))
            parts.append(f"{total_gb}GB")
        except (ValueError, TypeError):
            parts.append(mem_total)
    if mem_dimm_count and mem_dimm_size:
        parts.append(f"({mem_dimm_count}x{mem_dimm_size})")
    hw["memory"] = ", ".join(parts) if parts else "N/A"

    # ── Identify the test NIC via topology PCI address ──
    # Derive PF BDF from the first topology PCI address (strip domain,
    # keep bus:slot, set function to .0 to get the PF)
    pf_bdf = ""
    if topo_pci_addrs:
        raw = topo_pci_addrs[0]  # e.g. "0000:15:00.1"
        short = raw.split(":", 1)[-1] if raw.count(":") > 1 else raw  # "15:00.1"
        # PF is always function .0 on the same slot
        pf_bdf = re.sub(r"\.[0-9]+$", ".0", short)  # "15:00.0"

    # Find the netdev interface for the test NIC PF
    nic_iface = ""
    if pf_bdf:
        nic_iface = _remote_cmd(
            host, f"ls /sys/bus/pci/devices/0000:{pf_bdf}/net/ 2>/dev/null | head -1"
        )
    # Fallback: first ice-driver interface
    if not nic_iface:
        nic_iface = _remote_cmd(
            host,
            "for iface in $(ls /sys/class/net/); do "
            "  drv=$(basename $(readlink /sys/class/net/$iface/device/driver "
            "    2>/dev/null) 2>/dev/null); "
            '  if [ "$drv" = "ice" ]; then echo $iface; break; fi; '
            "done",
        )

    # NIC firmware version
    fw_ver = ""
    if nic_iface:
        fw_ver = _remote_cmd(
            host,
            f"ethtool -i {nic_iface} 2>/dev/null | grep firmware-version "
            f"| cut -d' ' -f2-",
        )
    hw["firmware_version"] = fw_ver or "N/A"

    # NIC NUMA node
    nic_numa = ""
    if nic_iface:
        nic_numa = _remote_cmd(
            host, f"cat /sys/class/net/{nic_iface}/device/numa_node 2>/dev/null"
        )
    hw["nic_numa"] = nic_numa if nic_numa and nic_numa != "-1" else "N/A"

    # NIC description — use the PF from topology, fall back to lspci grep
    pci_addr = pf_bdf  # e.g. "15:00.0"
    if not pci_addr:
        nic_line = _remote_cmd(
            host, "lspci 2>/dev/null | grep -i 'Ethernet.*E810' | head -1"
        ) or _remote_cmd(
            host,
            "lspci -d 8086: 2>/dev/null | grep -i 'ethernet' "
            "| grep -v 'Virtual' | head -1",
        )
        pci_addr = nic_line.split()[0] if nic_line else ""

    if pci_addr:
        nic_info = _remote_cmd(host, f"lspci -s {pci_addr} 2>/dev/null")
        subsys = _remote_cmd(
            host,
            f"lspci -s {pci_addr} -v 2>/dev/null " f"| grep 'Subsystem:' | cut -d: -f2",
        )
        subsys = subsys.strip() if subsys else ""

        # Count PF ports that share the same device ID
        dev_id = _remote_cmd(
            host, f"lspci -s {pci_addr} -n 2>/dev/null | awk '{{print $3}}'"
        )  # e.g. "8086:12d2"
        pf_count = ""
        if dev_id:
            pf_count = _remote_cmd(host, f"lspci -d {dev_id} 2>/dev/null | wc -l")

        # Link speed
        link_speed = ""
        if nic_iface:
            link_speed = _remote_cmd(
                host, f"cat /sys/class/net/{nic_iface}/speed 2>/dev/null || echo ''"
            )

        # PCIe gen/width
        pcie = _remote_cmd(
            host, f"lspci -s {pci_addr} -vvv 2>/dev/null | grep 'LnkSta:' | head -1"
        )
        pcie_gen = ""
        if pcie:
            m = re.search(r"Speed (\S+),.*Width (x\d+)", pcie)
            if m:
                speed_to_gen = {
                    "2.5GT/s": "Gen1",
                    "5GT/s": "Gen2",
                    "8GT/s": "Gen3",
                    "16GT/s": "Gen4",
                    "32GT/s": "Gen5",
                }
                gen = speed_to_gen.get(m.group(1), m.group(1))
                pcie_gen = f"PCIe {gen} {m.group(2)}"

        # Build NIC description
        if subsys and "Device" not in subsys:
            nic_name = subsys
        elif nic_info:
            # Use the full lspci description (e.g. "Intel Corporation Device 12d2")
            nic_name = (
                nic_info.split(":", 2)[-1].strip() if ":" in nic_info else nic_info
            )
        else:
            nic_name = "Unknown"
        nic_parts = [
            f"Intel(R) {nic_name}" if not nic_name.startswith("Intel") else nic_name
        ]
        if pf_count:
            nic_parts.append(f"{pf_count} ports")
        if link_speed and link_speed.isdigit():
            speed_gbps = int(link_speed) // 1000
            nic_parts.append(f"{speed_gbps}Gb/s per port")
        if pcie_gen:
            nic_parts.append(pcie_gen)
        hw["nic"] = ", ".join(nic_parts)
    else:
        hw["nic"] = "N/A"

    return hw


def collect_platform_info(host) -> Dict[str, Any]:
    """Collect full platform configuration from a remote host.

    Args:
        host: A host object with host.connection.execute_command() support
              (e.g. from mfd_connect SSHConnection).

    Returns:
        Dict with ``sw_configuration`` and ``hw_configuration`` keys.
    """
    logger.info(f"Collecting platform info from host: {host.name}")

    # Extract topology PCI addresses so we identify the correct test NIC
    topo_pci: list[str] = []
    try:
        for nic in host.network_interfaces:
            addr = getattr(nic.pci_address, "lspci", None) or str(nic.pci_address)
            topo_pci.append(addr)
    except (AttributeError, TypeError):
        pass  # no topology PCI info available

    config = {
        "sw_configuration": _collect_sw_configuration(host),
        "hw_configuration": _collect_hw_configuration(host, topo_pci),
    }

    logger.info(f"Platform info collected from {host.name}")
    return config
