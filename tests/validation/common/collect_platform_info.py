"""Collect platform SW/HW configuration from a remote host via SSH.

This module gathers system information from the measured host during test
execution and saves it as ``platform_config.json`` in the log directory.
The performance report generator then picks it up automatically.

Usage from conftest.py::

    from common.collect_platform_info import collect_and_save_platform_info
    collect_and_save_platform_info(host, log_path)
"""

import json
import logging
import os
from typing import Any, Dict, Optional

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

    # MTL version
    mtl_ver = _remote_cmd(host, "cat /proc/mtl/version 2>/dev/null || echo ''")
    if not mtl_ver:
        mtl_ver = _remote_cmd(
            host,
            "grep '#define MTL_VERSION' /usr/local/include/mtl/mtl_api.h 2>/dev/null "
            "| head -1 | awk '{print $3}' | tr -d '\"'"
        )
    if not mtl_ver:
        # Try the build path
        mtl_ver = _remote_cmd(
            host,
            "cat /root/awilczyn/Media-Transport-Library/version.txt 2>/dev/null || echo ''"
        )
    sw["mtl_version"] = mtl_ver or "N/A"

    # DPDK version
    dpdk_ver = _remote_cmd(
        host,
        "pkg-config --modversion libdpdk 2>/dev/null || echo ''"
    )
    if dpdk_ver:
        sw["dpdk_driver"] = f"{dpdk_ver} (patched with MTL patches)"
    else:
        sw["dpdk_driver"] = "N/A"

    # ICE driver version
    ice_ver = _remote_cmd(
        host,
        "cat /sys/module/ice/version 2>/dev/null || echo ''"
    )
    if ice_ver:
        # Check if it's a Kahawai build
        kahawai_ver = _remote_cmd(
            host,
            "modinfo ice 2>/dev/null | grep -i kahawai | head -1"
        )
        if kahawai_ver:
            # Extract the Kahawai version tag
            import re
            m = re.search(r'(Kahawai[_\s][\d.]+)', kahawai_ver, re.IGNORECASE)
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
        "cat /sys/kernel/debug/ice/*/ddp_pkg_version 2>/dev/null | head -1 || echo ''"
    )
    if not ddp_ver:
        ddp_ver = _remote_cmd(
            host,
            "dmesg | grep -i 'DDP package' | tail -1 | grep -oP 'version [\\d.]+' | awk '{print $2}'"
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
    hp_1g = _remote_cmd(host, "cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo 0")
    hp_2m = _remote_cmd(host, "cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0")
    hp_parts = []
    if hp_1g and hp_1g != "0":
        hp_parts.append(f"1G x {hp_1g}")
    if hp_2m and hp_2m != "0":
        hp_parts.append(f"2M x {hp_2m}")
    sw["hugepages"] = " + ".join(hp_parts) if hp_parts else "N/A"

    # CPU governor
    governor = _remote_cmd(
        host,
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A'"
    )
    sw["cpu_cores"] = f'Set in "{governor}" mode.' if governor and governor != "N/A" else "N/A"

    return sw


def _collect_hw_configuration(host) -> Dict[str, str]:
    """Collect hardware configuration from a remote host."""
    hw = {}

    # Server model
    server = _remote_cmd(host, "sudo dmidecode -t system 2>/dev/null | grep 'Product Name' | head -1")
    if ":" in server:
        hw["server"] = server.split(":", 1)[1].strip()
    else:
        hw["server"] = server or "N/A"

    # CPU info
    cpu_model = _remote_cmd(host, "grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2")
    cpu_model = cpu_model.strip()
    nproc = _remote_cmd(host, "nproc")
    sockets = _remote_cmd(host, "lscpu | grep 'Socket(s):' | awk '{print $2}'")
    cores = _remote_cmd(host, "lscpu | grep 'Core(s) per socket:' | awk '{print $4}'")
    threads = _remote_cmd(host, "lscpu | grep 'Thread(s) per core:' | awk '{print $4}'")
    if cpu_model and nproc:
        detail = f"{nproc} CPUs"
        if sockets and cores and threads:
            detail = f"{nproc} CPUs ({sockets} sockets x {cores} cores x {threads} threads)"
        hw["cpu"] = f"{cpu_model}, {detail}"
    else:
        hw["cpu"] = "N/A"

    # Memory
    mem_type = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Type:' | grep -v 'Error\\|Detail\\|Unknown' | head -1 | awk '{print $2}'"
    )
    mem_speed = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Speed:' | grep -v 'Unknown\\|Configured' | head -1 | awk '{print $2, $3}'"
    )
    mem_total = _remote_cmd(host, "free -h | awk '/^Mem:/ {print $2}'")
    mem_dimm_size = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Size:' | grep -v 'No Module\\|Maximum' | head -1 | awk '{print $2, $3}'"
    )
    mem_dimm_count = _remote_cmd(
        host,
        "sudo dmidecode -t memory 2>/dev/null | grep 'Size:' | grep -v 'No Module\\|Maximum' | wc -l"
    )
    parts = []
    if mem_type:
        parts.append(mem_type)
    if mem_speed:
        parts.append(mem_speed)
    if mem_total:
        # Round to nearest GB
        total_str = mem_total.rstrip("GiMBT")
        try:
            total_gb = round(float(total_str))
            parts.append(f"{total_gb}GB")
        except (ValueError, TypeError):
            parts.append(mem_total)
    if mem_dimm_count and mem_dimm_size:
        parts.append(f"({mem_dimm_count}x{mem_dimm_size})")
    hw["memory"] = ", ".join(parts) if parts else "N/A"

    # NIC info (E810 physical function cards)
    nic_info = _remote_cmd(
        host,
        "lspci -d 8086: 2>/dev/null | grep -i 'ethernet' | grep -v 'Virtual' | head -1"
    )
    if nic_info:
        # Get subsystem name for more detail
        pci_addr = nic_info.split()[0]
        subsys = _remote_cmd(host, f"lspci -s {pci_addr} -v 2>/dev/null | grep 'Subsystem:' | cut -d: -f2")
        subsys = subsys.strip() if subsys else ""

        # Count physical ports
        pf_count = _remote_cmd(
            host,
            "lspci -d 8086: 2>/dev/null | grep -i 'ethernet' | grep -v 'Virtual' | wc -l"
        )

        # Get link speed
        # Find a netdev for this PCI address
        speed_info = _remote_cmd(
            host,
            f"ls /sys/bus/pci/devices/0000:{pci_addr}/net/ 2>/dev/null | head -1"
        )
        link_speed = ""
        if speed_info:
            link_speed = _remote_cmd(
                host,
                f"cat /sys/class/net/{speed_info}/speed 2>/dev/null || echo ''"
            )

        # Get PCIe info
        pcie = _remote_cmd(
            host,
            f"lspci -s {pci_addr} -vvv 2>/dev/null | grep 'LnkSta:' | head -1"
        )
        pcie_gen = ""
        if pcie:
            import re
            m = re.search(r'Speed (\S+),.*Width (x\d+)', pcie)
            if m:
                speed_to_gen = {"2.5GT/s": "Gen1", "5GT/s": "Gen2", "8GT/s": "Gen3", "16GT/s": "Gen4", "32GT/s": "Gen5"}
                gen = speed_to_gen.get(m.group(1), m.group(1))
                pcie_gen = f"PCIe {gen} {m.group(2)}"

        nic_name = subsys if subsys else nic_info.split(":", 2)[-1].strip() if ":" in nic_info else nic_info
        nic_parts = [f"Intel(R) {nic_name}" if not nic_name.startswith("Intel") else nic_name]
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

    config = {
        "sw_configuration": _collect_sw_configuration(host),
        "hw_configuration": _collect_hw_configuration(host),
    }

    logger.info(f"Platform info collected from {host.name}")
    return config


def collect_and_save_platform_info(host, save_dir: str) -> Optional[str]:
    """Collect platform info from a host and save as platform_config.json.

    Args:
        host: Remote host object with connection.execute_command().
        save_dir: Directory to save platform_config.json into.

    Returns:
        Path to the saved file, or None on failure.
    """
    try:
        config = collect_platform_info(host)

        os.makedirs(save_dir, exist_ok=True)
        output_path = os.path.join(save_dir, "platform_config.json")
        with open(output_path, "w") as f:
            json.dump(config, f, indent=4)

        logger.info(f"Platform config saved to: {output_path}")
        return output_path
    except Exception as e:
        logger.warning(f"Failed to collect/save platform info from {host.name}: {e}")
        return None
