"""DMF CRM resource manifest generator.

Consumes Intel PerfSpect baseline JSON and profiling metrics (metrics.csv)
to produce a valid MxlMediaFunctionParameters manifest, then validates it
against the upstream jt-dmf-crm schema using jsonschema (the same library
used by AMWA-TV/jt-dmf-crm manifest/validate_manifest.py).

Typical usage via the wrapper script::

    scripts/create-dmf-manifest.sh \\
        --perfspect results/perfspect/k8s-w2/latest \\
        --profile results/pinned-20240115T120000Z \\
        --service encoder \\
        --output manifest/resource-manifest.yaml

Or directly::

    python3 -m mxlperf.dmf_manifest --help
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import yaml

# Root of this repo, used to locate bundled schema.
_MODULE_ROOT = Path(__file__).resolve().parents[2]
_BUNDLED_SCHEMA = _MODULE_ROOT / "manifest" / "schema" / "resource_manifest_schema.yaml"
_RESOLUTION_MAP = {
    "1080p": (1920, 1080),
    "2160p": (3840, 2160),
    "4k": (3840, 2160),
    "720p": (1280, 720),
    "2k": (2048, 1080),
}

# Frame dimensions for resolution tokens not in _RESOLUTION_MAP are left to
# the caller; no generator-enforced allowlist is applied.  The original manifest
# schema carries no resolution constraints.

# ──────────────────────────────────────────────────────────────────────────────
# PerfSpect JSON parsing
# ──────────────────────────────────────────────────────────────────────────────


def _coerce_int(value: Any) -> int | None:
    """Convert a string or number to int, returning None on failure."""
    if value is None:
        return None
    try:
        return int(str(value).strip().split(".")[0])
    except (TypeError, ValueError):
        return None


def _find_json_file(directory: Path) -> Path:
    """Return the single JSON file inside a PerfSpect report directory.

    PerfSpect writes one JSON file per report run.  The filename follows the
    pattern ``<hostname>_*.json`` or simply ``report.json``.

    Raises ``FileNotFoundError`` when no JSON file exists and ``ValueError``
    when more than one JSON file is present (ambiguous).
    """
    candidates = sorted(directory.glob("*.json"))
    if not candidates:
        raise FileNotFoundError(
            f"No JSON file found in PerfSpect directory: {directory}\n"
            "Run  scripts/run-perfspect.sh <node>  to generate a baseline report."
        )
    if len(candidates) > 1:
        names = ", ".join(p.name for p in candidates)
        raise ValueError(
            f"Multiple JSON files in PerfSpect directory: {directory}\n"
            f"  Found: {names}\n"
            "Pass the exact file path instead of the directory."
        )
    return candidates[0]


def _norm_key(k: str) -> str:
    """Normalise a field name for loose matching.

    Strips trailing colons (lscpu format uses ``"Socket(s):"``), converts to
    lower-case, and replaces spaces, hyphens, and parentheses with underscores.
    """
    return (
        k.lower()
        .rstrip(":")
        .strip()
        .replace(" ", "_")
        .replace("-", "_")
        .replace("(", "_")
        .replace(")", "_")
        .strip("_")
    )


def _search(data: Any, *keys: str) -> Any:
    """Case-insensitive recursive key search in a nested dict/list."""
    if isinstance(data, dict):
        needle_set = {_norm_key(key) for key in keys}
        for k, v in data.items():
            if _norm_key(k) in needle_set:
                return v
        for v in data.values():
            result = _search(v, *keys)
            if result is not None:
                return result
    elif isinstance(data, list):
        for item in data:
            result = _search(item, *keys)
            if result is not None:
                return result
    return None




def _split_features(raw: Any) -> list[str]:
    """Convert a feature field to a list of feature strings."""
    if raw is None:
        return []
    if isinstance(raw, list):
        return [str(v).strip() for v in raw if str(v).strip()]

    s = str(raw).strip()
    if not s:
        return []

    # Common separators in report-style output.
    if any(sep in s for sep in [",", ";", "|", "\n"]):
        parts = re.split(r"[,;|\n]+", s)
        return [p.strip() for p in parts if p.strip()]

    # lscpu flags are often whitespace-separated.
    parts = s.split()
    if len(parts) > 1:
        return [p.strip() for p in parts if p.strip()]
    return [s]


def _normalize_rate(value: Any, preferred_unit: str = "") -> str | None:
    """Normalize throughput/bandwidth strings while preserving reported units."""
    if value is None:
        return None

    s = str(value).strip()
    if not s:
        return None

    # Bare numeric values can be emitted with a hint unit when known.
    if re.fullmatch(r"[0-9]+(\.[0-9]+)?", s):
        return f"{s}{preferred_unit}" if preferred_unit else s

    # Remove internal spaces in common unit strings, e.g. "12.5 GB/s".
    compact = re.sub(r"\s+", "", s)
    return compact or None


def _normalize_storage_type(raw_type: Any, *hints: Any) -> str | None:
    """Map free-form storage media values to schema enum (nvme|ssd|hdd)."""
    tokens = [str(raw_type or "").lower()]
    tokens.extend(str(h or "").lower() for h in hints)
    text = " ".join(tokens)

    if "nvme" in text:
        return "nvme"
    if any(x in text for x in ["ssd", "solid state"]):
        return "ssd"
    if any(x in text for x in ["hdd", "spinning", "rotational", "sas", "sata"]):
        return "hdd"
    if str(raw_type).strip().lower() in {"0", "false", "no"}:
        return "ssd"
    if str(raw_type).strip().lower() in {"1", "true", "yes"}:
        return "hdd"
    return None


def _collect_prefixed_sections(data: dict[str, Any], *prefixes: str) -> list[Any]:
    """Collect top-level values where key matches one of the prefixes."""
    normalized_prefixes = tuple(_norm_key(p) for p in prefixes)
    sections: list[Any] = []
    for key, value in data.items():
        nk = _norm_key(str(key))
        if nk.startswith(normalized_prefixes):
            sections.append(value)
    return sections


def _dict_has_any_key(data: dict[str, Any], *keys: str) -> bool:
    """Check whether a dict contains any of the given keys (non-recursive)."""
    wanted = {_norm_key(k) for k in keys}
    return any(_norm_key(str(k)) in wanted for k in data.keys())


def _find_direct_value(data: Any, *keys: str) -> Any:
    """Return the first directly-matching key from a dict or list of dicts."""
    wanted = {_norm_key(k) for k in keys}
    if isinstance(data, dict):
        for key, value in data.items():
            if _norm_key(str(key)) in wanted:
                return value
    elif isinstance(data, list):
        for item in data:
            if isinstance(item, dict):
                value = _find_direct_value(item, *keys)
                if value is not None:
                    return value
    return None


def _section_dicts(section: Any) -> list[dict[str, Any]]:
    """Normalize a section to its dictionary records, preserving source order."""
    if isinstance(section, dict):
        return [section]
    if isinstance(section, list):
        return [item for item in section if isinstance(item, dict)]
    return []


def parse_perfspect_json(path: Path) -> dict[str, Any]:
    """Parse a PerfSpect report JSON file into DMF host_capabilities fields.

    PerfSpect ``report --all --format json`` produces a JSON file with system
    configuration data.  This function supports two common structures:

    1. **Flat key/value records** — a list of ``{"Category": ..., "Attribute":
       ..., "Value": ...}`` objects (older PerfSpect versions).
    2. **Hierarchical** — a nested dict with CPU/Memory/NUMA sub-dicts.

    Unknown fields are silently skipped; only populated fields appear in the
    returned dict. If ``path`` is a directory the single ``*.json`` file
    inside it is used automatically.

    Returns a dict suitable for the ``host_capabilities`` section of the
    manifest. An empty dict is returned if the file cannot yield any
    recognisable fields.
    """
    if path.is_dir():
        path = _find_json_file(path)

    raw = json.loads(path.read_text())

    caps: dict[str, Any] = {}

    # ── flat record list  {Category, Attribute, Value} ──────────────────────
    if isinstance(raw, list):
        by_cat: dict[str, dict[str, str]] = {}
        for entry in raw:
            if not isinstance(entry, dict):
                continue
            cat = str(entry.get("Category", entry.get("category", ""))).strip()
            attr = str(entry.get("Attribute", entry.get("attribute", ""))).strip()
            val = entry.get("Value", entry.get("value", ""))
            if cat and attr:
                by_cat.setdefault(cat, {})[attr] = val
        raw = by_cat  # reuse hierarchical path below

    # ── hierarchical dict ────────────────────────────────────────────────────
    if isinstance(raw, dict):
        caps = _extract_hierarchical(raw)

    if not caps:
        print(
            f"WARNING: no recognisable platform fields found in {path}.\n"
            "         See docs/15-dmf-crm-manifest.md for expected JSON structure.",
            file=sys.stderr,
        )
    return caps


# Field aliases used when searching the hierarchical dict.
_CPU_MODEL_KEYS = ("Model name", "Model", "cpu_model", "CPU Model", "Processor")
_MICROARCH_KEYS = ("Microarchitecture", "microarchitecture", "Micro Architecture", "Generation")
_SOCKETS_KEYS = ("Socket(s)", "Sockets", "sockets", "CPU Sockets")
_CORES_KEYS = (
    "Core(s) per socket", "Cores per Socket", "cores_per_socket",
    "Physical Cores per Socket", "Cores Per Socket",
)
_TOTAL_CORES_KEYS = ("CPU(s)", "logical_cpus", "Logical CPUs", "Total Logical CPUs", "Total CPUs")
_NUMA_KEYS = ("NUMA node(s)", "NUMA Nodes", "numa_nodes", "Nodes", "NUMA nodes")
_BASE_FREQ_KEYS = (
    "CPU MHz", "Base Frequency", "base_freq_mhz", "CPU base Frequency",
    "Base Freq", "cpu_base_freq", "CPU Base Frequency",
)
_CPU_FEATURE_KEYS = (
    "Flags", "flags", "CPU Flags", "Features", "Feature Flags",
    "Instruction Set", "Instruction Sets", "Capabilities", "CPU Features",
)
_MEM_INSTALLED_KEYS = ("Installed Memory",)
_MEM_TOTAL_KEYS = ("MemTotal", "Total Memory", "Memory Size", "total_gb", "memory_size_gb")
_MEM_TYPE_KEYS = ("Memory Type", "Type", "dimm_type", "DIMM Type")
_MEM_SPEED_KEYS = (
    "Configured Memory Speed", "Speed", "memory_transfer_mt_s",
    "DIMM Speed", "Speed MT/s", "Configured Speed",
)
_NET_NAME_KEYS = ("name", "interface", "interface_name", "ifname", "device", "nic", "adapter")
_NET_IN_BW_KEYS = (
    "input_bandwidth", "ingress_bandwidth", "rx_bandwidth", "receive_bandwidth",
    "available_input_bandwidth", "rx", "rx_gbps", "in",
)
_NET_OUT_BW_KEYS = (
    "output_bandwidth", "egress_bandwidth", "tx_bandwidth", "transmit_bandwidth",
    "available_output_bandwidth", "tx", "tx_gbps", "out",
)
_NET_LINK_BW_KEYS = ("bandwidth", "link_speed", "speed", "available_bandwidth")
_NET_FEATURE_KEYS = ("features", "feature", "capabilities", "offloads", "flags")
_STORAGE_NAME_KEYS = ("name", "device", "device_name", "disk", "drive", "path", "model")
_STORAGE_TYPE_KEYS = ("type", "media", "media_type", "drive_type", "rotational", "rota")
_STORAGE_READ_KEYS = (
    "read", "read_speed", "read_bandwidth", "read_throughput", "sequential_read",
    "seq_read", "max_read",
)
_STORAGE_WRITE_KEYS = (
    "write", "write_speed", "write_bandwidth", "write_throughput", "sequential_write",
    "seq_write", "max_write",
)

# ── PerfSpect ISA yes/no map ──────────────────────────────────────────────────
# Centralized, explicit mapping from PerfSpect human-readable ISA labels to the
# exact CPU feature identifiers used by Kubernetes Node Feature Discovery (NFD)
# CPUID labels (feature.node.kubernetes.io/cpu-cpuid.<NAME>=true).
#
# Keys are lowercase for case-insensitive lookup.  Values are the canonical NFD
# CPUID name (uppercase, no separators) or a list of names when one PerfSpect
# label maps to multiple NFD features (e.g. PREFETCHIT0/1 → PREFETCHIT0 +
# PREFETCHIT1).
#
# Labels with parenthesized abbreviations whose NFD name differs from the bare
# abbreviation (e.g. AES-NI → AES, SHA_NI → SHA) are included explicitly so
# the authoritative NFD name is always used.
_ISA_LABEL_MAP: dict[str, str | list[str]] = {
    # Labels without a parenthesized abbreviation
    "avx-512 foundation": "AVX512F",
    "vector aes": "VAES",
    "amx-fp16 instruction": "AMXFP16",
    "amx-complex instruction": "AMXCOMPLEX",
    "avx-ifma instruction": "AVXIFMA",
    "avx-ne-convert instruction": "AVXNECONVERT",
    "avx-vnni-int8 instruction": "AVXVNNIINT8",
    # PREFETCHIT0/1 is a single PerfSpect label that covers two NFD features.
    "prefetchit0/1 instruction": ["PREFETCHIT0", "PREFETCHIT1"],
    "serialize instruction": "SERIALIZE",
    "umonitor, umwait, tpause instructions": "WAITPKG",
    # Labels whose parenthesized abbreviation differs from the NFD CPUID name
    "advanced encryption standard new instructions (aes-ni)": "AES",
    "sha1/sha256 instruction extensions (sha_ni)": "SHA",
    # Labels whose parenthesized abbreviation uses underscores that NFD omits
    "advanced matrix extensions (amx)": "AMX",
    "advanced vector extensions (avx512_fp16)": "AVX512FP16",
    "cache line demote (cldemote)": "CLDEMOTE",
    "compare and add if condition is met (cmpccxadd)": "CMPCCXADD",
    "enqueue command instruction (enqcmd)": "ENQCMD",
    "move 64 bytes as direct store (movdir64b)": "MOVDIR64B",
    "move doubleword as direct store (movdiri)": "MOVDIRI",
    "transactional synchronization extensions (tsxldtrk)": "TSXLDTRK",
    "vector neural network instructions (avx512_bf16)": "AVX512BF16",
    "vector neural network instructions (avx512_vnni)": "AVX512VNNI",
}

# Values that represent a supported/present capability.
_ISA_POSITIVE: frozenset[str] = frozenset({"yes", "true", "1"})


def _normalize_isa_label(label: str) -> str | list[str]:
    """Normalize a PerfSpect ISA human-readable label to NFD CPUID feature name(s).

    Returns a single NFD CPUID string, or a list of strings when one label maps
    to multiple NFD features (e.g. ``PREFETCHIT0/1`` → ``["PREFETCHIT0",
    "PREFETCHIT1"]``).

    Priority:
    1. Case-insensitive lookup in ``_ISA_LABEL_MAP`` (the authoritative source).
    2. Generic fallback: uppercase the parenthesized abbreviation with underscores
       and hyphens removed, or uppercase the whole label after stripping noise words.
    """
    lower = label.strip().lower()
    if lower in _ISA_LABEL_MAP:
        return _ISA_LABEL_MAP[lower]
    # Generic fallback for labels not in the map.
    m = re.search(r"\(([^)]+)\)", label)
    if m:
        abbr = m.group(1).strip()
        return re.sub(r"[_\-]", "", abbr).upper()
    normalized = re.sub(
        r"\s+(?:New\s+)?(?:Instruction(?:s|\s+Extensions?)?|Extensions?)\b.*$",
        "",
        label,
        flags=re.IGNORECASE,
    ).strip()
    normalized = re.sub(r"[\s,/_\-]+", "", normalized).upper()
    return normalized


def _parse_isa_features(isa_raw: Any) -> list[str]:
    """Parse a PerfSpect ``ISA`` field into normalized positive NFD CPUID identifiers.

    *isa_raw* may be a list of Yes/No maps (the canonical PerfSpect format) or a
    single such map.  Non-dict list entries are silently skipped.

    Supported positive values: boolean ``True``, numeric ``1`` or ``1.0``, and
    strings ``"Yes"``, ``"True"``, or ``"1"`` (all case-insensitive).

    Source order is preserved; duplicates are removed case-insensitively.
    """
    entries: list[dict[str, Any]] = []
    if isinstance(isa_raw, list):
        for item in isa_raw:
            if isinstance(item, dict):
                entries.append(item)
    elif isinstance(isa_raw, dict):
        entries.append(isa_raw)
    else:
        return []

    seen: set[str] = set()
    result: list[str] = []

    def _add(feature_id: str) -> None:
        key = feature_id.upper()
        if key not in seen:
            seen.add(key)
            result.append(feature_id)

    for obj in entries:
        for label, value in obj.items():
            if isinstance(value, bool):
                if not value:
                    continue
            elif isinstance(value, (int, float)):
                if value != 1:
                    continue
            elif str(value).strip().lower() not in _ISA_POSITIVE:
                continue
            normalized = _normalize_isa_label(str(label).strip())
            if isinstance(normalized, list):
                for nfd_id in normalized:
                    if nfd_id:
                        _add(nfd_id)
            elif normalized:
                _add(normalized)
    return result


def _extract_hierarchical(data: dict) -> dict[str, Any]:
    caps: dict[str, Any] = {}

    # ── CPU section ──────────────────────────────────────────────────────────
    # PerfSpect commonly emits top-level sections as one-element arrays, e.g.
    # "CPU": [{"CPU Model": "...", "Sockets": "2", ...}].  Parse CPU
    # metadata from the first dict record and discover ISA independently.
    cpu_raw = _find_direct_value(data, "CPU", "cpu", "Processor")
    cpu_dicts = _section_dicts(cpu_raw)
    flat: dict[str, Any] = cpu_dicts[0] if cpu_dicts else data

    if flat:
        model = _search(flat, *_CPU_MODEL_KEYS)
        generation = _search(flat, *_MICROARCH_KEYS)
        sockets = _coerce_int(_search(flat, *_SOCKETS_KEYS))
        cores_per_socket = _coerce_int(_search(flat, *_CORES_KEYS))
        numa_nodes = _coerce_int(_search(flat, *_NUMA_KEYS))
        total_logical = _coerce_int(_search(flat, *_TOTAL_CORES_KEYS))

        base_freq = _search(flat, *_BASE_FREQ_KEYS)
        total_cores = (
            (sockets * cores_per_socket)
            if sockets and cores_per_socket
            else total_logical
        )

        cpu_caps: dict[str, Any] = {}
        if numa_nodes:
            cpu_caps["numa_nodes"] = numa_nodes
        if total_cores:
            cpu_caps["number_of_cores"] = total_cores
        if base_freq:
            freq_str = str(base_freq).strip()
            # Normalise bare MHz numbers: "2500.0" → "2500MHz"
            if re.fullmatch(r"[0-9]+(\.[0-9]+)?", freq_str):
                freq_str = f"{int(float(freq_str))}MHz"
            cpu_caps["base_frequency"] = freq_str
        if generation:
            cpu_caps["generation"] = str(generation).strip()
        elif model:
            cpu_caps["generation"] = str(model).strip()

        cpu_features = _split_features(_search(flat, *_CPU_FEATURE_KEYS))

        # Discover ISA case-insensitively within the CPU section first, then at
        # the document root. ISA is authoritative when it yields usable
        # positive features; Flags/Features remain the fallback only.
        isa_raw = _find_direct_value(flat, "ISA")
        if isa_raw is None:
            isa_raw = _find_direct_value(data, "ISA")
        isa_features = _parse_isa_features(isa_raw)
        if isa_features:
            cpu_features = isa_features

        if cpu_features:
            cpu_caps["features"] = cpu_features

        if cpu_caps:
            caps["cpu"] = cpu_caps

    # ── Memory section ───────────────────────────────────────────────────────
    mem_section = _find_direct_value(data, "Memory", "memory", "DRAM")
    # NUMA nodes from memory section (fallback)
    if "cpu" not in caps or "numa_nodes" not in caps.get("cpu", {}):
        fallback_numa = _coerce_int(_search(data, *_NUMA_KEYS))
        if fallback_numa and isinstance(caps.get("cpu"), dict):
            caps["cpu"]["numa_nodes"] = fallback_numa

    # Try to discover per-NUMA memory entries
    numa_count = caps.get("cpu", {}).get("numa_nodes") if isinstance(caps.get("cpu"), dict) else None
    mem_entries: list[dict[str, Any]] = []

    mem_dicts = _section_dicts(mem_section)
    mem_record = mem_dicts[0] if mem_dicts else None

    if isinstance(mem_record, dict):
        dimm_section = (
            _find_direct_value(data, "DIMM", "DIMMs", "dimms")
            or _find_direct_value(mem_record, "DIMM", "DIMMs", "dimms")
        )
        dimm_dicts = _section_dicts(dimm_section)
        dimm_record = dimm_dicts[0] if dimm_dicts else None

        installed_memory = _search(mem_record, *_MEM_INSTALLED_KEYS)
        mem_total = _search(mem_record, *_MEM_TOTAL_KEYS)
        mem_type = (
            _search(dimm_record, *_MEM_TYPE_KEYS)
            if isinstance(dimm_record, dict)
            else None
        ) or _search(mem_record, *_MEM_TYPE_KEYS)
        mem_speed = (
            _search(dimm_record, *_MEM_SPEED_KEYS)
            if isinstance(dimm_record, dict)
            else None
        ) or _search(mem_record, *_MEM_SPEED_KEYS)
        speed_str = _normalize_rate(mem_speed, preferred_unit="MT/s")

        if installed_memory:
            entry: dict[str, Any] = {
                "type": "ram",
                "size": str(installed_memory).strip(),
            }
            if mem_type:
                entry["generation"] = str(mem_type).strip()
            if speed_str:
                entry["throughput"] = {"total": speed_str}
            mem_entries.append(entry)
        else:
            node_count = numa_count or 1
            size_str = None
            if mem_total:
                total_val = re.search(r"([0-9]+)", str(mem_total))
                if total_val:
                    total_gb = int(total_val.group(1))
                    per_node_gb = total_gb // node_count
                    if per_node_gb >= 1024:
                        size_str = f"{per_node_gb // 1024}Ti"
                    else:
                        size_str = f"{per_node_gb}Gi"

            for node_idx in range(node_count if node_count <= 8 else 1):
                entry = {"type": "ram"}
                if node_count > 1:
                    entry["node"] = node_idx
                has_details = False
                if mem_type:
                    entry["generation"] = str(mem_type).strip()
                    has_details = True
                if size_str:
                    entry["size"] = size_str
                    has_details = True
                if speed_str:
                    entry["throughput"] = {"total": speed_str}
                    has_details = True
                if has_details:
                    mem_entries.append(entry)

    if mem_entries:
        caps["memory"] = mem_entries

    # ── Network section ──────────────────────────────────────────────────────
    net_sections: list[Any] = []
    direct_network = data.get("Network") or data.get("network")
    if direct_network is not None:
        net_sections.append(direct_network)
    net_sections.extend(_collect_prefixed_sections(data, "Network", "NIC", "Ethernet", "Interface"))

    network_entries: list[dict[str, Any]] = []
    network_seen: set[tuple[Any, ...]] = set()

    def _append_network_item(item: dict[str, Any]) -> None:
        name = _search(item, *_NET_NAME_KEYS)
        if not name:
            return
        clean_name = str(name).strip()
        net_entry: dict[str, Any] = {"name": clean_name}

        bw_in = _normalize_rate(_search(item, *_NET_IN_BW_KEYS), preferred_unit="Gbps")
        bw_out = _normalize_rate(_search(item, *_NET_OUT_BW_KEYS), preferred_unit="Gbps")
        if not bw_in and not bw_out:
            link = _normalize_rate(_search(item, *_NET_LINK_BW_KEYS))
            if link:
                bw_in = bw_in or link
                bw_out = bw_out or link
        if bw_in or bw_out:
            bandwidth: dict[str, str] = {}
            if bw_in:
                bandwidth["input"] = bw_in
            if bw_out:
                bandwidth["output"] = bw_out
            net_entry["bandwidth"] = bandwidth

        features = _split_features(_search(item, *_NET_FEATURE_KEYS))
        if features:
            net_entry["features"] = features

        key = (
            clean_name,
            net_entry.get("bandwidth", {}).get("input") if isinstance(net_entry.get("bandwidth"), dict) else "",
            net_entry.get("bandwidth", {}).get("output") if isinstance(net_entry.get("bandwidth"), dict) else "",
            tuple(net_entry.get("features", [])),
        )
        if key in network_seen:
            return
        network_seen.add(key)
        network_entries.append(net_entry)

    for section in net_sections:
        if isinstance(section, list):
            for item in section:
                if isinstance(item, dict):
                    _append_network_item(item)
        elif isinstance(section, dict):
            # Either a single interface descriptor or a mapping of descriptors.
            if _dict_has_any_key(section, *_NET_NAME_KEYS):
                _append_network_item(section)
            else:
                for val in section.values():
                    if isinstance(val, dict):
                        _append_network_item(val)
                    elif isinstance(val, list):
                        for nested in val:
                            if isinstance(nested, dict):
                                _append_network_item(nested)

    if network_entries:
        caps["network"] = network_entries

    # ── Storage section ──────────────────────────────────────────────────────
    storage_sections: list[Any] = []
    direct_storage = data.get("Storage") or data.get("storage")
    if direct_storage is not None:
        storage_sections.append(direct_storage)
    storage_sections.extend(_collect_prefixed_sections(data, "Storage", "Disk", "Drive", "NVMe", "Block"))

    storage_entries: list[dict[str, Any]] = []
    storage_seen: set[tuple[Any, ...]] = set()

    def _append_storage_item(item: dict[str, Any]) -> None:
        name = _search(item, *_STORAGE_NAME_KEYS)
        if not name:
            return
        clean_name = str(name).strip()
        raw_type = _search(item, *_STORAGE_TYPE_KEYS)
        stype = _normalize_storage_type(raw_type, name, item.get("model"), item.get("Model"))

        st_entry: dict[str, Any] = {"name": clean_name}
        if stype:
            st_entry["type"] = stype

        read_rate = _normalize_rate(_search(item, *_STORAGE_READ_KEYS), preferred_unit="MB/s")
        write_rate = _normalize_rate(_search(item, *_STORAGE_WRITE_KEYS), preferred_unit="MB/s")
        if read_rate or write_rate:
            throughput: dict[str, str] = {}
            if read_rate:
                throughput["read"] = read_rate
            if write_rate:
                throughput["write"] = write_rate
            st_entry["throughput"] = throughput

        key = (
            clean_name,
            st_entry.get("type", ""),
            st_entry.get("throughput", {}).get("read") if isinstance(st_entry.get("throughput"), dict) else "",
            st_entry.get("throughput", {}).get("write") if isinstance(st_entry.get("throughput"), dict) else "",
        )
        if key in storage_seen:
            return
        storage_seen.add(key)
        storage_entries.append(st_entry)

    for section in storage_sections:
        if isinstance(section, list):
            for item in section:
                if isinstance(item, dict):
                    _append_storage_item(item)
        elif isinstance(section, dict):
            if _dict_has_any_key(section, *_STORAGE_NAME_KEYS):
                _append_storage_item(section)
            else:
                for val in section.values():
                    if isinstance(val, dict):
                        _append_storage_item(val)
                    elif isinstance(val, list):
                        for nested in val:
                            if isinstance(nested, dict):
                                _append_storage_item(nested)

    if storage_entries:
        caps["storage"] = storage_entries

    return caps


# ──────────────────────────────────────────────────────────────────────────────
# Profiling metrics.csv parsing
# ──────────────────────────────────────────────────────────────────────────────

def _parse_avg(value_str: str) -> float | None:
    """Extract the ``avg=`` component from ``avg=X;min=Y;max=Z`` strings."""
    match = re.search(r"avg=([0-9.eE+\-]+)", str(value_str))
    if match:
        return float(match.group(1))
    try:
        return float(value_str)
    except (TypeError, ValueError):
        return None


def _parse_scope_labels(scope_str: str) -> dict[str, Any]:
    """Best-effort parse of the serialized Prometheus label scope."""
    if not scope_str:
        return {}
    try:
        parsed = json.loads(scope_str)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _metric_row_service(row: dict[str, str]) -> str:
    """Return the service identifier associated with a metrics row."""
    for key in ("service", "role"):
        value = str(row.get(key, "")).strip()
        if value:
            return value

    labels = _parse_scope_labels(str(row.get("scope", "")))
    for key in ("service", "role"):
        value = str(labels.get(key, "")).strip()
        if value:
            return value

    pod = str(labels.get("pod") or labels.get("exported_pod") or "").strip()
    if "-decoder-" in pod:
        return "decoder"
    if "-encoder-" in pod:
        return "encoder"
    return ""


def parse_metrics_csv(
    csv_path: Path,
    service: str,
    session: str = "",
) -> dict[str, Any]:
    """Extract workload CPU and memory requirements from a profiling metrics.csv.

    The file is produced by ``mxlperf.collect.capture`` and has the columns::

        category, metric, unit, value, scope, session, role

    Returns a dict::

        {
            "cpu_cores":  float | None,   # measured CPU demand in cores
            "memory_mib": int   | None,   # peak working-set memory in MiB
            "cpu_metric_found": bool,     # whether a usable CPU metric matched
            "memory_metric_found": bool,  # whether a usable memory metric matched
        }

    Only rows matching the requested *service* (and *session* when given) are
    used.  ``cpu_cores`` comes from the ``workload_cpu_cores`` Prometheus metric
    (or the legacy ``FFmpeg CPU demand`` row when that is absent);
    ``memory_mib`` from the ``workload_memory_bytes`` Prometheus metric.
    """
    cpu_cores: float | None = None
    memory_bytes: float | None = None
    cpu_metric_found = False
    memory_metric_found = False

    with csv_path.open(newline="") as fh:
        for row in csv.DictReader(fh):
            row_service = _metric_row_service(row)
            row_session = row.get("session", "")
            if row_service != service:
                continue
            if session and row_session and row_session != session:
                continue

            cat = row.get("category", "")
            metric = row.get("metric", "")
            val_str = row.get("value", "")

            if (cat == "Prometheus" and metric == "workload_cpu_cores") or \
               (cat == "CPU" and metric == "FFmpeg CPU demand"):
                v = _parse_avg(val_str)
                if v is not None:
                    cpu_metric_found = True
                    cpu_cores = max(cpu_cores or 0.0, v)

            if cat == "Prometheus" and metric == "workload_memory_bytes":
                v = _parse_avg(val_str)
                if v is not None:
                    memory_metric_found = True
                    memory_bytes = max(memory_bytes or 0.0, v)

    memory_mib: int | None = None
    if memory_bytes is not None:
        memory_mib = (int(memory_bytes) + 2**20 - 1) // 2**20  # ceiling MiB

    return {
        "cpu_cores": cpu_cores,
        "memory_mib": memory_mib,
        "cpu_metric_found": cpu_metric_found,
        "memory_metric_found": memory_metric_found,
    }


# ──────────────────────────────────────────────────────────────────────────────
# Unit conversion helpers
# ──────────────────────────────────────────────────────────────────────────────


def cores_to_millicore_str(cores: float) -> str:
    """Convert a float number of cores to a Kubernetes CPU string.

    Examples: 0.5 → ``"500m"``, 1.0 → ``"1000m"``, 5.0 → ``"5000m"``.
    The schema regex ``^[0-9]+(m|\\.[0-9]+)?$`` requires either an integer,
    a decimal, or an integer suffixed with ``m``.  We always use ``m``.
    """
    millivalue = round(cores * 1000)
    return f"{millivalue}m"


def mib_to_k8s_memory(mib: int) -> str:
    """Convert MiB to the smallest Kubernetes memory suffix that fits.

    Schema pattern: ``^[0-9]+(Mi|Gi|Ti)$``.
    """
    if mib % 1024 == 0:
        gib = mib // 1024
        if gib % 1024 == 0:
            return f"{gib // 1024}Ti"
        return f"{gib}Gi"
    return f"{mib}Mi"


# ──────────────────────────────────────────────────────────────────────────────
# Manifest builder
# ──────────────────────────────────────────────────────────────────────────────


def build_manifest(
    *,
    name: str,
    role: str,
    scenario: str = "",
    resolution: str = "1080p",
    host_caps: dict[str, Any] | None = None,
    cpu_cores: float | None = None,
    memory_mib: int | None = None,
    namespace: str = "mxl",
    cpu_margin: float = 1.20,
    memory_margin: float = 1.25,
    # Legacy kwargs kept for backwards compatibility with existing call-sites
    # that may pass these; they are silently accepted but not emitted because
    # spec.args is not part of the original manifest schema.
    preset: str = "",
    bitrate: str = "",
    dec_threads: str = "",
    enc_threads: str = "",
    filter_threads: str = "",
    dec_filter_threads: str = "",
    enc_filter_threads: str = "",
    slices: str = "",
) -> dict[str, Any]:
    """Assemble the manifest dict from extracted source values.

    Only fields that have non-None values are included.  Call
    ``validate_manifest`` on the result before writing it.

    The original manifest schema does not include a ``spec.args`` field, so
    no ``args`` key is emitted regardless of the kwargs supplied.

    Resolution follows the schema/authoritative config input.  The generator
    applies no hard-coded allowlist: any ``resolution`` string is accepted.
    Well-known tokens (``1080p``, ``4k``, ``2160p``, ``720p``, ``2k``) are
    mapped to frame dimensions; unknown tokens leave frame dimensions unset.

    Args:
        name:           ``metadata.name`` (required).
        role:           ``spec.role`` — derived from ``--service``.
        scenario:       Scenario name from ``scenarios/*.env`` (informational).
        resolution:     e.g. ``"1080p"`` → sets frame dimensions.
        host_caps:      Dict from ``parse_perfspect_json``; written to
                        ``spec.requirements.host_capabilities`` including any
                        ``cpu.features`` discovered by PerfSpect.
        cpu_cores:      Measured CPU demand in cores (from ``parse_metrics_csv``).
        memory_mib:     Measured peak working-set in MiB.
        namespace:      Kubernetes namespace.
        cpu_margin:     Multiplier applied to measured CPU (default 1.20 = +20 %).
        memory_margin:  Multiplier applied to measured memory (default 1.25 = +25 %).
    """
    frame_dims = _RESOLUTION_MAP.get(resolution.lower())

    if frame_dims is not None:
        frame_w, frame_h = frame_dims
        video_input: dict[str, Any] = {
            "media_type": "video/v210",
            "frame_width": frame_w,
            "frame_height": frame_h,
            "frame_rate": "25",
        }
        video_output: dict[str, Any] = {
            "media_type": "video/H264",
            "frame_width": frame_w,
            "frame_height": frame_h,
            "frame_rate": "25",
        }
    else:
        video_input = {"media_type": "video/v210", "frame_rate": "25"}
        video_output = {"media_type": "video/H264", "frame_rate": "25"}

    manifest: dict[str, Any] = {
        "apiVersion": "mxl.media.fn/v1alpha1",
        "kind": "MxlMediaFunctionParameters",
        "metadata": {
            "name": name,
            "namespace": namespace,
        },
        "spec": {
            "role": role,
            "flow": {"label": scenario or role},
            "inputs": [
                {
                    "primary-feed": None,
                    "format": {"video": video_input},
                }
            ],
            "outputs": {
                "encoded-output": {
                    "format": {"video": video_output}
                }
            },
        },
    }

    # spec.args is not part of the original manifest schema and is not emitted.

    # ── workload requirements ────────────────────────────────────────────────
    requirements: dict[str, Any] = {}

    if cpu_cores is not None:
        requested_cores = cpu_cores * cpu_margin
        requirements["cpu"] = {"cpu": cores_to_millicore_str(requested_cores)}

    if memory_mib is not None:
        requested_mib = int(memory_mib * memory_margin)
        # Round up to next MiB boundary divisible by 8 for alignment
        requested_mib = ((requested_mib + 7) // 8) * 8
        requirements["memory"] = [{"type": "ram", "size": mib_to_k8s_memory(requested_mib)}]

    if host_caps:
        requirements["host_capabilities"] = host_caps

    if requirements:
        manifest["spec"]["requirements"] = requirements

    return manifest


# ──────────────────────────────────────────────────────────────────────────────
# Validation
# ──────────────────────────────────────────────────────────────────────────────


def validate_manifest(
    manifest: dict[str, Any],
    schema_path: Path | None = None,
) -> tuple[bool, list[str]]:
    """Validate *manifest* against the jt-dmf-crm schema using jsonschema.

    This mirrors the logic in ``AMWA-TV/jt-dmf-crm manifest/validate_manifest.py``
    (which uses ``Draft7Validator``).

    Args:
        manifest:    The manifest dict to validate.
        schema_path: Path to the schema YAML.  Defaults to the bundled copy at
                     ``manifest/schema/resource_manifest_schema.yaml``.

    Returns:
        ``(is_valid, error_messages)`` where *error_messages* is an empty list
        when *is_valid* is ``True``.
    """
    try:
        from jsonschema import Draft7Validator
    except ImportError as exc:
        return False, [
            f"jsonschema is not installed: {exc}\n"
            "Run: pip install jsonschema"
        ]

    if schema_path is None:
        schema_path = _BUNDLED_SCHEMA
    if not schema_path.exists():
        return False, [f"Schema file not found: {schema_path}"]

    schema = yaml.safe_load(schema_path.read_text())
    validator = Draft7Validator(schema)
    errors = list(validator.iter_errors(manifest))
    if not errors:
        return True, []
    messages = []
    for err in errors:
        path = " -> ".join(str(p) for p in err.path) or "(root)"
        messages.append(f"  {path}: {err.message}")
    return False, messages


# ──────────────────────────────────────────────────────────────────────────────
# CLI helpers
# ──────────────────────────────────────────────────────────────────────────────


def _resolve_perfspect_dir(path_str: str) -> Path:
    """Resolve ``latest`` symlinks and validate that the baseline path exists."""
    p = Path(path_str)
    if not p.exists():
        raise SystemExit(
            f"ERROR: PerfSpect baseline path not found: {p}\n"
            "       Run  scripts/run-perfspect.sh <node>  first."
        )
    return p.resolve()


def _resolve_profile_dir(path_str: str) -> Path:
    """Resolve and validate the profiling run directory."""
    p = Path(path_str)
    if not p.exists():
        raise SystemExit(
            f"ERROR: Profiling directory not found: {p}\n"
            "       Run  scripts/run.sh <scenario>  first."
        )
    return p.resolve()


def _find_profile_artifact(profile_dir: Path, filename: str) -> Path:
    """Locate a profiling artifact inside a run directory."""
    candidate = profile_dir / filename
    if candidate.exists():
        return candidate
    # Some run layouts nest further
    for sub in sorted(profile_dir.iterdir()):
        if sub.is_dir():
            nested = sub / filename
            if nested.exists():
                return nested
    raise SystemExit(
        f"ERROR: {filename} not found under {profile_dir}\n"
        "       Ensure the profiling run completed successfully and produced\n"
        "       results as described in docs/08-profiling-manifest.md."
    )


def _find_metrics_csv(profile_dir: Path) -> Path:
    """Locate the metrics.csv inside a profiling run directory."""
    return _find_profile_artifact(profile_dir, "metrics.csv")


def _load_profile_config(profile_dir: Path) -> tuple[Path, dict[str, Any]]:
    """Load the authoritative resolved profile configuration from config.json."""
    config_path = _find_profile_artifact(profile_dir, "config.json")
    try:
        raw = json.loads(config_path.read_text())
    except json.JSONDecodeError as exc:
        raise SystemExit(
            f"ERROR: invalid JSON in {config_path}: {exc}\n"
            "       The profiling run directory must contain a valid resolved config.json."
        ) from exc

    if not isinstance(raw, dict):
        raise SystemExit(
            f"ERROR: {config_path} must contain a JSON object.\n"
            "       The profiling run directory must contain the resolved scenario configuration."
        )
    return config_path, raw


def _config_str(cfg: dict[str, Any], key: str) -> str:
    """Return a config value coerced to a stripped string."""
    value = cfg.get(key, "")
    if value is None:
        return ""
    return str(value).strip()


def _require_profile_setting(cfg: dict[str, Any], config_path: Path, key: str) -> str:
    """Require a non-empty setting from the resolved profile config."""
    value = _config_str(cfg, key)
    if value:
        return value
    raise SystemExit(
        f"ERROR: {config_path} is missing required setting {key}.\n"
        "       The profile config is the source of truth for scenario and workload settings."
    )


def _available_metric_services(csv_path: Path) -> list[str]:
    """Return the distinct services present in metrics relevant to manifest generation."""
    services: set[str] = set()
    with csv_path.open(newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("metric", "") not in {"workload_cpu_cores", "FFmpeg CPU demand", "workload_memory_bytes"}:
                continue
            service = _metric_row_service(row)
            if service:
                services.add(service)
    return sorted(services)


# ──────────────────────────────────────────────────────────────────────────────
# CLI entry point
# ──────────────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    """CLI entry point.  Returns 0 on success, non-zero on failure."""
    parser = argparse.ArgumentParser(
        prog="create-dmf-manifest",
        description=(
            "Generate a DMF CRM MxlMediaFunctionParameters manifest from\n"
            "PerfSpect baseline and profiling results."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Encoder manifest from the latest PerfSpect run and a named profiling run
  scripts/create-dmf-manifest.sh \\
      --perfspect results/perfspect/k8s-w2/latest \\
      --profile   results/pinned-20240115T120000Z \\
      --service   encoder \\
      --output    manifest/mxl-encoder-pinned.yaml

  # Skip validation (offline / jt-dmf-crm not available)
  scripts/create-dmf-manifest.sh ... --no-validate

See docs/15-dmf-crm-manifest.md for full documentation.
""",
    )

    parser.add_argument(
        "--perfspect",
        required=True,
        metavar="DIR",
        help=(
            "Required. PerfSpect baseline directory or JSON file.  "
            "Accepts the 'results/perfspect/<node>/latest' layout."
        ),
    )
    parser.add_argument(
        "--profile",
        required=True,
        metavar="DIR",
        help=(
            "Profiling run directory (contains metrics.csv, config.json, …).  "
            "See docs/08-profiling-manifest.md."
        ),
    )
    parser.add_argument(
        "--service",
        required=True,
        help=(
            "Service identifier used to select CPU and memory metrics from metrics.csv.  "
            "The same value is written to spec.role."
        ),
    )
    parser.add_argument(
        "--name",
        default="",
        help=(
            "metadata.name for the manifest.  "
            "Defaults to  '<service>-<scenario>-<resolution>'."
        ),
    )
    parser.add_argument(
        "--output",
        default="manifest/resource-manifest.yaml",
        help="Output path for the generated manifest (default: manifest/resource-manifest.yaml).",
    )
    parser.add_argument(
        "--namespace",
        default="mxl",
        help="Kubernetes namespace to embed in the manifest (default: mxl).",
    )
    parser.add_argument(
        "--cpu-margin",
        type=float,
        default=1.20,
        metavar="FACTOR",
        help="Multiplier applied to measured CPU demand (default: 1.20 = +20%%).",
    )
    parser.add_argument(
        "--memory-margin",
        type=float,
        default=1.25,
        metavar="FACTOR",
        help="Multiplier applied to measured memory (default: 1.25 = +25%%).",
    )
    parser.add_argument(
        "--schema",
        metavar="FILE",
        default=str(_BUNDLED_SCHEMA),
        help=f"Schema YAML for validation (default: bundled copy at {_BUNDLED_SCHEMA}).",
    )
    parser.add_argument(
        "--no-validate",
        action="store_true",
        help="Skip schema validation (for offline use or troubleshooting).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the manifest to stdout instead of writing it to --output.",
    )

    args = parser.parse_args(argv)

    # ── load resolved profile config ──────────────────────────────────────────
    profile_dir = _resolve_profile_dir(args.profile)
    config_path, cfg = _load_profile_config(profile_dir)
    scenario = _require_profile_setting(cfg, config_path, "SCENARIO")
    resolution = _require_profile_setting(cfg, config_path, "RESOLUTION")
    # No generator-enforced allowlist: resolution follows the schema/authoritative
    # config input.  Unknown resolution tokens are accepted; frame dimensions for
    # well-known tokens (1080p, 4k, etc.) are mapped in _RESOLUTION_MAP.

    preset = _config_str(cfg, "PRESET")
    bitrate = _config_str(cfg, "BITRATE")
    dec_threads = _config_str(cfg, "DEC_THREADS")
    enc_threads = _config_str(cfg, "ENC_THREADS")
    filter_threads = _config_str(cfg, "FILTER_THREADS")
    dec_filter_threads = _config_str(cfg, "DEC_FILTER_THREADS")
    enc_filter_threads = _config_str(cfg, "ENC_FILTER_THREADS")
    slices = _config_str(cfg, "SLICES")

    manifest_name = args.name or f"{args.service}-{scenario}-{resolution}"

    # ── PerfSpect ─────────────────────────────────────────────────────────────
    perf_path = _resolve_perfspect_dir(args.perfspect)
    print(f"Reading PerfSpect baseline: {perf_path}", file=sys.stderr)
    try:
        host_caps = parse_perfspect_json(perf_path)
    except (FileNotFoundError, ValueError, json.JSONDecodeError, OSError) as exc:
        raise SystemExit(
            f"ERROR: failed to read usable PerfSpect baseline from {perf_path}: {exc}\n"
            "       Provide a PerfSpect JSON file, or a directory/symlink containing exactly one JSON baseline report."
        ) from exc
    if not host_caps:
        raise SystemExit(
            f"ERROR: no usable host capability data found in PerfSpect baseline: {perf_path}\n"
            "       Ensure the baseline is a valid PerfSpect report with CPU/memory/platform fields."
        )
    cpu_caps = host_caps.get("cpu", {})
    if not isinstance(cpu_caps, dict) or not cpu_caps.get("features"):
        raise SystemExit(
            f"ERROR: no CPU features found in PerfSpect baseline: {perf_path}\n"
            "       spec.requirements.host_capabilities.cpu.features requires PerfSpect CPU feature data.\n"
            "       Ensure the baseline contains a usable ISA map or CPU Flags/Features field.\n"
            "       See docs/15-dmf-crm-manifest.md for expected JSON structure."
        )

    # ── Profiling ─────────────────────────────────────────────────────────────
    cpu_cores: float | None = None
    memory_mib: int | None = None

    metrics_path = _find_metrics_csv(profile_dir)
    print(f"Reading profiling metrics: {metrics_path}", file=sys.stderr)
    prof = parse_metrics_csv(metrics_path, service=args.service)
    cpu_cores = prof["cpu_cores"]
    memory_mib = prof["memory_mib"]

    available_services = _available_metric_services(metrics_path)
    available_hint = (
        f" Available services: {', '.join(available_services)}."
        if available_services else
        " No services were discovered in workload CPU or workload_memory_bytes rows."
    )
    if not prof["cpu_metric_found"] or cpu_cores is None:
        raise SystemExit(
            f"ERROR: no usable workload CPU metric found in {metrics_path} "
            f"for service '{args.service}'.{available_hint}"
        )
    if not prof["memory_metric_found"] or memory_mib is None:
        raise SystemExit(
            f"ERROR: no usable 'workload_memory_bytes' metric found in {metrics_path} "
            f"for service '{args.service}'.{available_hint}"
        )

    # ── Generate ──────────────────────────────────────────────────────────────
    manifest = build_manifest(
        name=manifest_name,
        role=args.service,
        scenario=scenario,
        resolution=resolution,
        preset=preset,
        bitrate=bitrate,
        dec_threads=dec_threads,
        enc_threads=enc_threads,
        filter_threads=filter_threads,
        dec_filter_threads=dec_filter_threads,
        enc_filter_threads=enc_filter_threads,
        slices=slices,
        host_caps=host_caps or None,
        cpu_cores=cpu_cores,
        memory_mib=memory_mib,
        namespace=args.namespace,
        cpu_margin=args.cpu_margin,
        memory_margin=args.memory_margin,
    )

    # ── Validate ──────────────────────────────────────────────────────────────
    if not args.no_validate:
        schema_path = Path(args.schema)
        is_valid, errors = validate_manifest(manifest, schema_path)
        if is_valid:
            print("✅ Manifest validated against jt-dmf-crm schema.", file=sys.stderr)
        else:
            print("❌ Manifest validation FAILED:", file=sys.stderr)
            for msg in errors:
                print(msg, file=sys.stderr)
            print(
                "\nTip: use --no-validate to write the manifest anyway for debugging.",
                file=sys.stderr,
            )
            return 1

    # ── Write provenance comment + YAML ───────────────────────────────────────
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    perfspect_src = str(args.perfspect)
    profile_src = str(args.profile)

    provenance = (
        f"# Generated by create-dmf-manifest  {stamp}\n"
        f"# PerfSpect source : {perfspect_src}\n"
        f"# Profiling source : {profile_src}\n"
        f"# Profile config   : {config_path}\n"
        f"# Scenario         : {scenario}\n"
        f"# Service / role   : {args.service}\n"
        f"# Schema           : {args.schema}\n"
        "#\n"
        "# Validation: AMWA-TV/jt-dmf-crm manifest/validate_manifest.py\n"
        "#   python3 <jt-dmf-crm>/manifest/validate_manifest.py <this file>\n"
        "#\n"
    )
    yaml_body = yaml.dump(manifest, default_flow_style=False, sort_keys=False, allow_unicode=True)
    output_text = provenance + yaml_body

    if args.dry_run:
        print(output_text)
        return 0

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(output_text)
    print(f"Manifest written to: {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
