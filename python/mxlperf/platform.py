"""Worker platform specification: core topology and theoretical DRAM peak.

Probe runs unprivileged over SSH. Memory transfer rate needs DMI, which normally
requires root, so it may also be declared per node in config/nodes.env as
<NODE>_MEM_TRANSFER_MT_S. Peak bandwidth is a hardware ceiling, never a
measurement; measured PCM traffic is compared against it only as context.
"""
from __future__ import annotations

import json
import re

PROBE_COMMAND = (
    "printf '%s\\n' '=== lscpu-json ==='; lscpu -J; "
    "printf '%s\\n' '=== meminfo ==='; grep MemTotal /proc/meminfo; "
    "printf '%s\\n' '=== dimms ==='; "
    "for d in /sys/devices/system/edac/mc/mc*/dimm*; do "
    "[ -r \"$d/size\" ] || continue; "
    "printf 'dimm %s %s %s\\n' "
    "\"$(cat $d/dimm_label 2>/dev/null || echo unknown)\" "
    "\"$(cat $d/size 2>/dev/null || echo 0)\" "
    "\"$(cat $d/dimm_mem_type 2>/dev/null || echo unknown)\"; done; "
    "printf '%s\\n' '=== dmi-memory ==='; "
    "if [ \"$(id -u)\" -eq 0 ]; then admin=''; else admin='sudo -n'; fi; "
    "$admin dmidecode -t memory 2>/dev/null | grep -E 'Configured Memory Speed|Speed:' || true; "
    # Power, because it decides what frequency the cores a run is given actually
    # reach: a powersave governor costs streams and would otherwise be read as a
    # property of the hardware. Every per-CPU value is collapsed to its distinct
    # set, so 'performance' on all but one CPU does not read as 'performance'.
    "printf '%s\\n' '=== power ==='; "
    "printf 'scaling_driver %s\\n' "
    "\"$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null)\"; "
    "printf 'pstate_status %s\\n' "
    "\"$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null)\"; "
    "printf 'no_turbo %s\\n' "
    "\"$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null)\"; "
    "printf 'governor %s\\n' "
    "\"$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd, -)\"; "
    "printf 'epb %s\\n' "
    "\"$(cat /sys/devices/system/cpu/cpu*/power/energy_perf_bias 2>/dev/null | sort -u | paste -sd, -)\"; "
    "printf 'epp %s\\n' "
    "\"$(cat /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference 2>/dev/null | sort -u | paste -sd, -)\""
)

# What the probe reports under '=== power ==='. Absent files give an empty string,
# which is not the same as a wrong value: an older kernel simply does not have
# intel_pstate, and a run on it is still comparable if the governor matches.
_POWER_FIELDS = ("scaling_driver", "pstate_status", "no_turbo", "governor", "epb", "epp")

_LSCPU_FIELDS = {
    "Model name:": "cpu_model",
    "Socket(s):": "sockets",
    "Core(s) per socket:": "cores_per_socket",
    "Thread(s) per core:": "threads_per_core",
    "CPU(s):": "logical_cpus",
    "NUMA node(s):": "numa_nodes",
    "CPU max MHz:": "cpu_max_mhz",
    "L3 cache:": "l3_cache",
}


def _sections(raw: str) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current = ""
    for line in raw.splitlines():
        match = re.fullmatch(r"=== (.+) ===", line.strip())
        if match:
            current = match.group(1)
            sections[current] = []
        elif current:
            sections[current].append(line)
    return sections


def _lscpu_values(lines: list[str]) -> dict[str, str]:
    try:
        entries = json.loads("\n".join(lines)).get("lscpu", [])
    except (json.JSONDecodeError, AttributeError):
        return {}
    values: dict[str, str] = {}

    def walk(items: list[dict]) -> None:
        for item in items:
            field, data = item.get("field", ""), item.get("data", "")
            if field in _LSCPU_FIELDS and _LSCPU_FIELDS[field] not in values:
                values[_LSCPU_FIELDS[field]] = data
            walk(item.get("children", []))

    walk(entries)
    return values


def _int(value: str | None) -> int | None:
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return None


def _dimms(lines: list[str]) -> list[dict[str, object]]:
    dimms = []
    for line in lines:
        parts = line.split()
        if len(parts) != 4 or parts[0] != "dimm":
            continue
        size_mib = _int(parts[2])
        if not size_mib:
            continue
        dimms.append({"label": parts[1], "size_mib": size_mib, "type": parts[3]})
    return dimms


def _power(lines: list[str]) -> dict[str, str]:
    values = {field: "" for field in _POWER_FIELDS}
    for line in lines:
        key, _, value = line.strip().partition(" ")
        if key in values:
            values[key] = value.strip()
    return values


def _transfer_mt_s(lines: list[str]) -> int | None:
    rates = [_int(match.group(1)) for match in re.finditer(r"(\d+)\s*MT/s", "\n".join(lines))]
    rates = [rate for rate in rates if rate]
    return max(rates) if rates else None


def parse_platform(raw: str, declared_mt_s: int | None = None) -> dict[str, object]:
    sections = _sections(raw)
    cpu = _lscpu_values(sections.get("lscpu-json", []))
    dimms = _dimms(sections.get("dimms", []))
    channels = sorted({
        match.group(0)
        for dimm in dimms
        if (match := re.search(r"SrcID#\d+_MC#\d+_Chan#\d+", str(dimm["label"])))
    })
    sockets = _int(cpu.get("sockets"))
    probed_mt_s = _transfer_mt_s(sections.get("dmi-memory", []))
    transfer_mt_s = probed_mt_s or declared_mt_s
    memory_total_kib = None
    for line in sections.get("meminfo", []):
        if line.startswith("MemTotal:"):
            memory_total_kib = _int(line.split()[1])
    spec: dict[str, object] = {
        "cpu_model": cpu.get("cpu_model", ""),
        "sockets": sockets,
        "cores_per_socket": _int(cpu.get("cores_per_socket")),
        "threads_per_core": _int(cpu.get("threads_per_core")),
        "logical_cpus": _int(cpu.get("logical_cpus")),
        "numa_nodes": _int(cpu.get("numa_nodes")),
        "cpu_max_mhz": cpu.get("cpu_max_mhz", ""),
        "l3_cache": cpu.get("l3_cache", ""),
        "memory_total_gib": round(memory_total_kib / 2**20, 1) if memory_total_kib else None,
        "populated_dimms": len(dimms),
        "dimm_size_gib": round(dimms[0]["size_mib"] / 1024, 1) if dimms else None,
        "dimm_type": dimms[0]["type"] if dimms else "",
        "populated_channels": len(channels) or None,
        "channels_per_socket": len(channels) // sockets if channels and sockets else None,
        "memory_transfer_mt_s": transfer_mt_s,
        "memory_transfer_source": "dmi" if probed_mt_s else ("declared" if declared_mt_s else ""),
        "power": _power(sections.get("power", [])),
    }
    if channels and transfer_mt_s:
        # DDR transfers 8 bytes per channel per transfer.
        spec["theoretical_dram_gbps"] = round(len(channels) * transfer_mt_s * 8 / 1000, 1)
    return spec


def platform_summary_line(spec: dict[str, object]) -> str:
    if not spec:
        return "unavailable"
    peak = spec.get("theoretical_dram_gbps")
    peak_text = f"{peak} GB/s theoretical DRAM peak" if peak else "theoretical DRAM peak unavailable"
    return (
        f"{spec.get('cpu_model', 'unknown CPU')}, {spec.get('sockets', '?')} sockets x "
        f"{spec.get('cores_per_socket', '?')} cores, {spec.get('logical_cpus', '?')} logical CPUs, "
        f"L3 {spec.get('l3_cache', 'unknown')}, {spec.get('populated_dimms', '?')} x "
        f"{spec.get('dimm_size_gib', '?')} GiB {spec.get('dimm_type', '')}, "
        f"{spec.get('populated_channels', '?')} channels, {peak_text}"
    )
