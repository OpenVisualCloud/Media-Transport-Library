#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Generate an HTML performance report from pytest log directories.

Usage:
    python generate_report.py <log_root> [-o report.html]

Scans timestamped subdirectories under <log_root> for pytest.log and per-test
.log files, extracts sweep results, platform config, throughput and CPU cores,
and produces a single self-contained HTML report.
"""

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime

# ── Data classes ──────────────────────────────────────────────────────


@dataclass
class NicDmaInfo:
    """NIC and DMA configuration parsed from RxTxApp logs."""

    vf_addr: str = ""
    pf_addr: str = ""
    nic_numa: str = ""
    dma_device: str = ""
    dma_numa: str = ""
    numa_match: str = ""  # "SAME" or "DIFFERENT"
    pf_vf_pairs: list = field(default_factory=list)  # [(pf, vf), ...]


@dataclass
class HostInfo:
    """Host info derived from runtime log data."""

    name: str = ""
    role: str = ""  # "dut" / "companion"
    vfs: list = field(default_factory=list)
    nic_dma: NicDmaInfo = field(default_factory=NicDmaInfo)


@dataclass
class SweepTest:
    """One completed sweep test with all extracted data."""

    key: str = ""
    max_sessions: int = 0
    tested_side: str = ""  # TX / RX
    mode: str = ""  # SC / MC
    redundant: bool = False
    dma: bool = False
    fps: int = 0
    resolution: str = ""
    log_dir: str = ""
    timestamp: str = ""
    host_assignment: str = ""
    command: str = ""
    cores_used: int = 0
    core_ids: str = ""
    measured_dev_tx: float = 0.0
    measured_dev_rx: float = 0.0
    companion_dev_tx: float = 0.0
    companion_dev_rx: float = 0.0
    steps: list = field(default_factory=list)  # [(sessions, passed, detail)]
    rxtxapp_config: dict = field(default_factory=dict)


# ── Regex patterns ────────────────────────────────────────────────────

RE_HOST_ASSIGN = re.compile(
    r"Host assignment.*?(?:SUT|DUT)=(\S+)\s.*?measured (TX|RX)\s*\|"
    r"\s*TX=(\S+),\s*RX=(\S+)"
)
RE_SWEEP_RESULT = re.compile(
    r"Sweep: max (\d+) sessions for "
    r"(REDUNDANT )?(TX|RX) (SC|MC)"
    r"(?: with (?:DSA|DMA) \([^)]+\))?"
    r" @ (\d+)fps / (\d+p)"
)
RE_STEP = re.compile(r"(\d+) sessions\s+(✓|✗)\s+(.+)")
RE_CPU_CORES = re.compile(
    r"\[CPU_CORES\] cores_used=(\d+)\s+max_simultaneous=\d+\s+"
    r"samples=\d+\s+core_ids=([\d,]+)"
)
RE_TEST_CMD = re.compile(r"(?:Test command|COMMAND):\s+(.+)")
RE_MEASURED_HDR = re.compile(r"---\s+Measured\s+\(")
RE_COMPANION_HDR = re.compile(r"---\s+Companion\s+\(")
RE_DEV_TX = re.compile(r"DEV TX:\s+([\d.]+)\s+Mb/s")
RE_DEV_RX = re.compile(r"DEV RX:\s+([\d.]+)\s+Mb/s")
RE_VFS = re.compile(r"Host (\S+): redundant port VFs: \[(.+?)\]")
RE_DMA_ENABLED = re.compile(
    r"(?:DSA|DMA) enabled\s*(?:\((\w+)\))?\s*:\s*(\S+)\s*\("
    r"(.+?),\s*NUMA\s*(\d+)\)\s*"
    r"for NIC\s*(\S+)\s*\(NUMA\s*(\d+)\)\s*"
    r"on\s*(\S+)\s*.\s*NUMA\s*(\w+)"
)
RE_REDUNDANT_MODE = re.compile(
    r"Redundant mode:\s*interfaces=\[(.+?)\],\s*direction=(\w+)"
)
RE_NICCTL_CMD = re.compile(
    r"Executing\s+>[\d.]+\.(\d+)>\s+'[^']*nicctl\.sh\s+list\s+([\da-fA-F:.]+)'"
)
RE_PCI_LINE = re.compile(r"^([\da-fA-F]{4}:[\da-fA-F]{2}:[\da-fA-F]{2}\.\d+)\s*$")
RE_LOG_PREFIX = re.compile(
    r"^\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+\S+\s+\S+\s*"
)

_RES_MAP = {"1080p": (1920, 1080), "2160p": (3840, 2160), "4320p": (7680, 4320)}


# ── Parsing ───────────────────────────────────────────────────────────


def _parse_log_file(log_path: str, dir_name: str = ""):
    """Parse a single log file for VFs, NIC/DMA info, and sweep tests.

    Returns (vf_map, nic_info, pf_vf_map, sweep_tests) where:
    - vf_map: {hostname: [vf_addrs]} from "redundant port VFs" lines
    - nic_info: {hostname: NicDmaInfo}
    - pf_vf_map: {ip_suffix: {pf: [vfs]}} from nicctl output
    - sweep_tests: list of SweepTest
    """
    vf_map: dict[str, list[str]] = {}
    info: dict[str, NicDmaInfo] = {}
    pf_vf_map: dict[str, dict[str, list[str]]] = {}

    # nicctl state
    nicctl_suffix: str | None = None
    nicctl_pf: str | None = None
    collecting_vfs = False

    # Redundant mode state
    vfs_by_direction: dict[str, list[str]] = {}
    host_for_direction: dict[str, str] = {}

    # Sweep parsing state
    tests: list[SweepTest] = []
    host_assign = cmd = cores = core_ids = ""
    m_tx = m_rx = c_tx = c_rx = 0.0
    in_m = in_c = collecting_sweep = in_config = False
    steps: list[tuple[int, bool, str]] = []
    config_lines: list[str] = []
    rxtxapp_config: dict = {}

    if not dir_name:
        dir_name = os.path.basename(os.path.dirname(log_path))

    with open(log_path, "r", errors="replace") as f:
        for line in f:
            # ── VF list ──
            m = RE_VFS.search(line)
            if m:
                vfs = [v.strip().strip("'\"") for v in m.group(2).split(",")]
                vf_map[m.group(1)] = vfs
                continue

            # ── DMA/DSA enabled ──
            m = RE_DMA_ENABLED.search(line)
            if m:
                _role, dma_dev, _dtype, dma_numa = m.group(1, 2, 3, 4)
                nic_vf, nic_numa, hostname, numa_match = m.group(5, 6, 7, 8)
                nd = info.setdefault(hostname, NicDmaInfo())
                nd.vf_addr = nic_vf
                nd.nic_numa = nic_numa
                nd.dma_device = dma_dev
                nd.dma_numa = dma_numa
                nd.numa_match = numa_match
                continue

            # ── Redundant mode ──
            m = RE_REDUNDANT_MODE.search(line)
            if m:
                direction = m.group(2).lower()
                vfs_by_direction[direction] = [
                    v.strip().strip("'\"") for v in m.group(1).split(",")
                ]
                continue

            # ── Host assignment ──
            m = RE_HOST_ASSIGN.search(line)
            if m:
                host_for_direction["tx"] = m.group(3)
                host_for_direction["rx"] = m.group(4)
                # Also reset sweep-block state
                host_assign = RE_LOG_PREFIX.sub("", line).strip()
                cmd = cores = core_ids = ""
                m_tx = m_rx = c_tx = c_rx = 0.0
                in_m = in_c = collecting_sweep = in_config = False
                steps, config_lines, rxtxapp_config = [], [], {}
                continue

            # ── nicctl PF→VF mapping ──
            m = RE_NICCTL_CMD.search(line)
            if m:
                nicctl_suffix, nicctl_pf = m.group(1), m.group(2)
                collecting_vfs = False
                continue
            if nicctl_pf and "stdout>>" in line:
                collecting_vfs = True
                pf_vf_map.setdefault(nicctl_suffix, {})[nicctl_pf] = []
                continue
            if collecting_vfs:
                stripped = line.strip()
                vm = RE_PCI_LINE.match(stripped)
                if vm:
                    pf_vf_map[nicctl_suffix][nicctl_pf].append(vm.group(1))
                elif stripped:
                    collecting_vfs = False
                    nicctl_pf = None
                continue

            # ── Test command ──
            mc = RE_TEST_CMD.search(line)
            if mc:
                cmd = mc.group(1).strip()
                continue

            # ── CPU cores ──
            mc = RE_CPU_CORES.search(line)
            if mc:
                cores, core_ids = mc.group(1), mc.group(2)
                continue

            # ── Throughput headers/values ──
            if RE_MEASURED_HDR.search(line):
                in_m, in_c = True, False
                continue
            if RE_COMPANION_HDR.search(line):
                in_c, in_m = True, False
                continue
            if in_m or in_c:
                t = RE_DEV_TX.search(line)
                r = RE_DEV_RX.search(line)
                if t:
                    v = float(t.group(1))
                    if in_m:
                        m_tx = v
                    else:
                        c_tx = v
                if r:
                    v = float(r.group(1))
                    if in_m:
                        m_rx = v
                    else:
                        c_rx = v
                    in_m = in_c = False

            # ── Sweep summary block ──
            if "SWEEP SUMMARY:" in line:
                collecting_sweep = True
                steps = []
                continue
            if collecting_sweep:
                ms = RE_STEP.search(line)
                if ms:
                    steps.append(
                        (int(ms.group(1)), ms.group(2) == "✓", ms.group(3).strip())
                    )
                    continue
                if "RXTXAPP_CONFIG_BEGIN" in line:
                    in_config, config_lines = True, []
                    continue
                if "RXTXAPP_CONFIG_END" in line:
                    in_config = False
                    try:
                        rxtxapp_config = json.loads("\n".join(config_lines))
                    except (json.JSONDecodeError, ValueError):
                        rxtxapp_config = {}
                    continue
                if in_config:
                    config_lines.append(RE_LOG_PREFIX.sub("", line).rstrip())
                    continue

            # ── Sweep result line → finalize test ──
            ms = RE_SWEEP_RESULT.search(line)
            if ms:
                ts_m = re.match(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", line)
                st = SweepTest(
                    max_sessions=int(ms.group(1)),
                    tested_side=ms.group(3),
                    mode=ms.group(4),
                    redundant=bool(ms.group(2)),
                    dma="DMA" in line or "DSA" in line,
                    fps=int(ms.group(5)),
                    resolution=ms.group(6),
                    log_dir=dir_name,
                    timestamp=ts_m.group(1) if ts_m else dir_name,
                    host_assignment=host_assign,
                    command=cmd,
                    cores_used=int(cores) if cores else 0,
                    core_ids=core_ids,
                    measured_dev_tx=m_tx,
                    measured_dev_rx=m_rx,
                    companion_dev_tx=c_tx,
                    companion_dev_rx=c_rx,
                    steps=list(steps),
                    rxtxapp_config=dict(rxtxapp_config),
                )
                rd = "REDUNDANT " if st.redundant else ""
                ds = " +DMA" if st.dma else ""
                st.key = f"{rd}{st.tested_side} {st.mode}{ds} @ {st.fps}fps / {st.resolution}"
                tests.append(st)
                collecting_sweep = False
                continue

    # Fill VF addresses from Redundant mode lines
    for direction, vfs in vfs_by_direction.items():
        hostname = host_for_direction.get(direction)
        if hostname and vfs:
            nd = info.setdefault(hostname, NicDmaInfo())
            if not nd.vf_addr:
                nd.vf_addr = vfs[0]

    # Cross-reference: find PF for each host's VF using nicctl output,
    # and resolve all redundant VFs to PF/VF pairs
    for hostname, nd in info.items():
        suffix_m = re.search(r"_(\d+)$", hostname)
        if not suffix_m:
            continue
        host_pfs = pf_vf_map.get(suffix_m.group(1), {})
        if nd.vf_addr and not nd.pf_addr:
            for pf, vfs in host_pfs.items():
                if nd.vf_addr in vfs:
                    nd.pf_addr = pf
                    break
        # Resolve redundant VFs → PF/VF pairs
        direction = host_for_direction.get("tx") == hostname and "tx" or "rx"
        redundant_vfs = vfs_by_direction.get(direction, [])
        if hostname in [host_for_direction.get(d) for d in vfs_by_direction]:
            # Find which direction this host maps to
            for d, h in host_for_direction.items():
                if h == hostname:
                    redundant_vfs = vfs_by_direction.get(d, [])
                    break
        if redundant_vfs and host_pfs:
            pairs = []
            for vf in redundant_vfs:
                for pf, vf_list in host_pfs.items():
                    if vf in vf_list:
                        pairs.append((pf, vf))
                        break
            if pairs:
                nd.pf_vf_pairs = pairs

    return vf_map, info, pf_vf_map, tests


def _merge_nic_info(target: NicDmaInfo, source: NicDmaInfo):
    """Merge non-empty fields from *source* into *target*."""
    for attr in (
        "vf_addr",
        "pf_addr",
        "nic_numa",
        "dma_device",
        "dma_numa",
        "numa_match",
    ):
        val = getattr(source, attr)
        if val:
            setattr(target, attr, val)
    if source.pf_vf_pairs:
        target.pf_vf_pairs = source.pf_vf_pairs


def _load_platform(log_dir: str) -> dict | None:
    p = os.path.join(log_dir, "platform_config.json")
    try:
        with open(p) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None


def _load_per_host_platforms(directory: str) -> dict[str, dict]:
    """Load ``<hostname>_platform_config.json`` files from *directory*."""
    result: dict[str, dict] = {}
    if not os.path.isdir(directory):
        return result
    for fn in os.listdir(directory):
        if fn.endswith("_platform_config.json") and fn != "platform_config.json":
            hostname = fn.replace("_platform_config.json", "")
            try:
                with open(os.path.join(directory, fn)) as f:
                    result[hostname] = json.load(f)
            except (json.JSONDecodeError, OSError):
                pass
    return result


def scan_logs(log_root: str):
    """Scan log dirs → (tests, per_host_platforms, hosts)."""
    best: dict[str, SweepTest] = {}
    per_host: dict[str, dict] = {}
    legacy_platform = None
    vf_map: dict[str, list[str]] = {}
    nic_dma_map: dict[str, NicDmaInfo] = {}
    all_pf_vf: dict[str, dict[str, list[str]]] = {}

    log_root = os.path.abspath(log_root)
    dirs = sorted(
        d
        for d in os.listdir(log_root)
        if os.path.isdir(os.path.join(log_root, d)) and re.match(r"\d{4}-", d)
    )

    for dn in dirs:
        dp = os.path.join(log_root, dn)

        # Platform configs
        ph = _load_per_host_platforms(dp)
        if ph:
            per_host.update(ph)
        pc = _load_platform(dp)
        if pc:
            legacy_platform = pc

        # Collect log files: pytest.log + per-test .log files
        log_files: list[str] = []
        lp = os.path.join(dp, "pytest.log")
        if os.path.exists(lp):
            log_files.append(lp)
        tests_dir = os.path.join(dp, "tests")
        if os.path.isdir(tests_dir):
            for root, _, files in os.walk(tests_dir):
                log_files.extend(
                    os.path.join(root, fn) for fn in files if fn.endswith(".log")
                )
        if not log_files:
            continue

        for lf in log_files:
            vfs, nd, pf_vf, sweeps = _parse_log_file(lf, dir_name=dn)
            vf_map.update(vfs)
            # Accumulate nicctl PF→VF mapping
            for suffix, pf_dict in pf_vf.items():
                merged = all_pf_vf.setdefault(suffix, {})
                for pf, vf_list in pf_dict.items():
                    if vf_list:
                        merged[pf] = vf_list
            # Merge NIC/DMA info (don't overwrite non-empty fields)
            for hostname, new_nd in nd.items():
                existing = nic_dma_map.get(hostname)
                if existing:
                    _merge_nic_info(existing, new_nd)
                else:
                    nic_dma_map[hostname] = new_nd
            for t in sweeps:
                best[t.key] = t

    # Top-level per-host configs
    ph = _load_per_host_platforms(log_root)
    if ph:
        per_host.update(ph)
    if not per_host and legacy_platform:
        per_host["_legacy"] = legacy_platform

    # Derive hosts from sweep results
    all_tests = list(best.values())
    hosts = _extract_hosts(all_tests, vf_map)

    # Enrich hosts with NIC/DMA info
    for h in hosts:
        nd = nic_dma_map.get(h.name)
        if nd:
            h.nic_dma = nd
        # Companion: use "redundant port VFs" as authoritative VF source
        if h.role != "dut":
            host_vfs = vf_map.get(h.name, [])
            if host_vfs:
                h.nic_dma.vf_addr = host_vfs[0]
                if not h.nic_dma.pf_addr:
                    suffix_m = re.search(r"_(\d+)$", h.name)
                    if suffix_m:
                        for pf, pf_vfs in all_pf_vf.get(suffix_m.group(1), {}).items():
                            if h.nic_dma.vf_addr in pf_vfs:
                                h.nic_dma.pf_addr = pf
                                break
        # Default single PF/VF pair
        if not h.nic_dma.pf_vf_pairs and h.nic_dma.pf_addr and h.nic_dma.vf_addr:
            h.nic_dma.pf_vf_pairs = [(h.nic_dma.pf_addr, h.nic_dma.vf_addr)]
        # Fallback NUMA from platform_config.json
        if not h.nic_dma.nic_numa:
            plat = per_host.get(h.name, per_host.get("_legacy", {}))
            h.nic_dma.nic_numa = (
                plat.get("hw_configuration", {}).get("nic_numa", "") or ""
            )

    return all_tests, per_host, hosts


def _extract_hosts(
    tests: list[SweepTest], vf_map: dict[str, list[str]]
) -> list[HostInfo]:
    """Derive host names and roles from sweep test host_assignment strings."""
    dut_name = ""
    all_hosts: set[str] = set()
    for t in tests:
        if not t.host_assignment:
            continue
        m = RE_HOST_ASSIGN.search(t.host_assignment)
        if m:
            dut_name = m.group(1)
            all_hosts.add(m.group(3))
            all_hosts.add(m.group(4))
    return [
        HostInfo(
            name=n, role="dut" if n == dut_name else "companion", vfs=vf_map.get(n, [])
        )
        for n in sorted(all_hosts)
    ]


def _build_fallback_config(t: SweepTest) -> dict:
    """Synthesize a config dict when no RXTXAPP_CONFIG was captured."""
    w, h = _RES_MAP.get(t.resolution, (0, 0))
    session_key = "tx_sessions" if t.tested_side == "TX" else "rx_sessions"
    opposite_key = "rx_sessions" if t.tested_side == "TX" else "tx_sessions"
    session = {
        "replicas": t.max_sessions,
        "start_port": 5004,
        "payload_type": 96,
        "width": w,
        "height": h,
        "fps": f"p{t.fps}",
        "pacing": "narrow",
        "packing": "GPM",
        "transport_format": "YUV_422_10bit",
    }
    if t.tested_side == "TX":
        session["input_format"] = "YUV422RFC4175PG2BE10"
    cfg: dict = {
        "interfaces": [],
        session_key: [{"st20p": [session]}],
        opposite_key: [],
    }
    if t.command:
        for pat, key in [
            (r"--dma_dev\s+(\S+)", "_cli_dma_dev"),
            (r"--sch_session_quota\s+(\d+)", "_cli_sch_session_quota"),
        ]:
            m = re.search(pat, t.command)
            if m:
                val = m.group(1)
                cfg[key] = int(val) if val.isdigit() else val
        if "--disable_migrate" in t.command:
            cfg["_cli_disable_migrate"] = True
        tt = re.search(r"--test_time\s+(\d+)", t.command)
        if tt:
            cfg["_cli_test_time"] = int(tt.group(1))
    if t.redundant:
        cfg["_mode"] = "redundant (ST2022-7)"
    cfg["_note"] = "reconstructed from sweep metadata (config not captured in log)"
    return cfg


# ── HTML ──────────────────────────────────────────────────────────────


def _esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _tput(mbps: float) -> str:
    if mbps <= 0:
        return "—"
    return f"{mbps / 1000:.2f} Gb/s" if mbps >= 1000 else f"{mbps:.1f} Mb/s"


def _kv_table(title: str, rows: list[tuple[str, str]]) -> str:
    lines = [
        f'<div class="section-title">{title}</div>',
        '<table class="kv"><tr><th>Parameter</th><th>Value</th></tr>',
    ]
    for k, v in rows:
        lines.append(f"<tr><td>{k}</td><td>{v}</td></tr>")
    lines.append("</table>")
    return "\n".join(lines)


def _generate_html(tests, per_host, hosts) -> str:
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    p = [_CSS.replace("{{TS}}", now)]

    # Resolve DUT / companion
    dut = comp = None
    for h in hosts:
        if h.role == "dut":
            dut = h
        elif h.role == "companion":
            comp = h

    def _plat(h):
        if h and h.name in per_host:
            return per_host[h.name]
        return per_host.get("_legacy", {})

    dut_plat, comp_plat = _plat(dut), _plat(comp)
    dut_label = f"Host 1 ({_esc(dut.name)}) [DUT]" if dut else "Host 1 [DUT]"
    comp_label = f"Host 2 ({_esc(comp.name)})" if comp else "Host 2"

    # ── Platform & Topology section ──
    p.append('<div class="sec"><h2>Platform &amp; Topology</h2>')

    def _side_by_side(title_l, rows_l, title_r, rows_r):
        p.append('<div class="side-by-side">')
        p.append(f"<div>{_kv_table(title_l, rows_l)}</div>")
        p.append(f"<div>{_kv_table(title_r, rows_r)}</div>")
        p.append("</div>")

    def _config_rows(plat, keys):
        section = plat.get(keys[0], {})
        return [(label, section.get(key, "N/A")) for label, key in keys[1:]]

    _SW_KEYS = [
        "sw_configuration",
        ("OS", "os"),
        ("Kernel", "kernel"),
        ("MTL Version", "mtl_version"),
        ("DPDK Version", "dpdk_driver"),
        ("ICE Version", "ice_version"),
        ("BIOS VT-X &amp; VT-D", "bios_vtx_vtd"),
        ("Hugepages", "hugepages"),
        ("CPU Cores", "cpu_cores"),
    ]
    _HW_KEYS = [
        "hw_configuration",
        ("Server", "server"),
        ("CPU", "cpu"),
        ("Memory (RAM)", "memory"),
        ("NIC", "nic"),
    ]

    _side_by_side(
        f"SW Configuration — {dut_label}",
        _config_rows(dut_plat, _SW_KEYS),
        f"SW Configuration — {comp_label}",
        _config_rows(comp_plat, _SW_KEYS),
    )
    _side_by_side(
        f"HW Configuration — {dut_label}",
        _config_rows(dut_plat, _HW_KEYS),
        f"HW Configuration — {comp_label}",
        _config_rows(comp_plat, _HW_KEYS),
    )

    # NIC Configuration
    def _nic_rows(h, plat, show_all_pairs=True):
        if not h:
            return [("NIC", "N/A")]
        hw = plat.get("hw_configuration", {})
        nd = h.nic_dma
        rows = [("Host", _esc(h.name)), ("NIC Interface Type", "VF")]
        pairs = nd.pf_vf_pairs if show_all_pairs else []
        if len(pairs) > 1:
            for i, (pf, vf) in enumerate(pairs, 1):
                rows += [
                    (f"PCI Address (PF {i})", _esc(pf)),
                    (f"PCI Address (VF {i})", _esc(vf)),
                ]
        else:
            pf = pairs[0][0] if pairs else nd.pf_addr or "N/A"
            vf = pairs[0][1] if pairs else nd.vf_addr or "N/A"
            rows += [("PCI Address (PF)", _esc(pf)), ("PCI Address (VF)", _esc(vf))]
        rows.append(("Firmware Version", _esc(hw.get("firmware_version", "") or "N/A")))
        rows.append(
            ("NUMA Node", _esc(str(nd.nic_numa or hw.get("nic_numa", "") or "N/A")))
        )
        return rows

    _side_by_side(
        f"NIC Configuration — {dut_label}",
        _nic_rows(dut, dut_plat, show_all_pairs=False),
        f"NIC Configuration — {comp_label}",
        _nic_rows(comp, comp_plat, show_all_pairs=True),
    )

    # DMA Configuration
    def _dma_rows(h, is_dut):
        if not is_dut:
            return [("DMA", "Not used (companion)")]
        if not h or not h.nic_dma.dma_device:
            dma_dev = ""
            for t in tests:
                if t.command:
                    dm = re.search(r"--dma_dev\s+(\S+)", t.command)
                    if dm:
                        dma_dev = dm.group(1)
                        break
            return [("DMA Device", _esc(dma_dev))] if dma_dev else [("DMA", "Not used")]
        nd = h.nic_dma
        sym = "✓" if nd.numa_match == "SAME" else "✗"
        return [
            ("DMA Device", _esc(nd.dma_device)),
            ("DMA NUMA Node", _esc(str(nd.dma_numa))),
            ("NUMA Match", f"{sym} {nd.numa_match}"),
        ]

    _side_by_side(
        f"DMA Configuration — {dut_label}",
        _dma_rows(dut, True),
        f"DMA Configuration — {comp_label}",
        _dma_rows(comp, False),
    )
    p.append("</div>")

    # ── Summary tables ──
    _SECTIONS = [
        ("Single Core (SC)", "SC", False),
        ("Single Core (SC) — Redundant (ST2022-7)", "SC", True),
        ("Multi Core (MC)", "MC", False),
        ("Multi Core (MC) — Redundant (ST2022-7)", "MC", True),
    ]
    for label, mode, redundant in _SECTIONS:
        group = sorted(
            [t for t in tests if t.mode == mode and t.redundant == redundant],
            key=lambda x: (x.fps, x.resolution, x.tested_side, x.dma),
        )
        if not group:
            continue
        p.append(f'<div class="sec"><h2>{label} — Max Sessions</h2>')
        for fps in sorted({t.fps for t in group}):
            fps_group = [t for t in group if t.fps == fps]
            p.append(f"<h3>{fps} fps</h3>")
            p.append(
                '<table class="mx"><thead><tr>'
                "<th>Tested Side</th><th>Resolution</th>"
                "<th>Max Sessions</th><th>Cores</th>"
                "<th>Measured Throughput</th><th>Run</th>"
                "</tr></thead><tbody>"
            )
            for t in fps_group:
                cl = "p" if t.max_sessions > 0 else "f"
                meas = t.measured_dev_rx if t.tested_side == "RX" else t.measured_dev_tx
                side = f"{t.tested_side} +DMA" if t.dma else t.tested_side
                p.append(
                    f'<tr class="{cl}"><td>{side}</td><td>{t.resolution}</td>'
                    f'<td class="ms">{t.max_sessions}</td>'
                    f'<td class="n">{t.cores_used}</td>'
                    f"<td>{_tput(meas)}</td><td>{t.log_dir}</td></tr>"
                )
            p.append("</tbody></table>")
        p.append("</div>")

    # ── Detail cards ──
    p.append('<div class="sec"><h2>Sweep Details</h2>')
    for t in sorted(
        tests,
        key=lambda x: (x.mode, x.redundant, x.resolution, x.fps, x.tested_side, x.dma),
    ):
        meas = t.measured_dev_rx if t.tested_side == "RX" else t.measured_dev_tx
        comp_tput = t.companion_dev_tx if t.tested_side == "RX" else t.companion_dev_rx
        other = "TX" if t.tested_side == "RX" else "RX"

        p.append(
            f"<details><summary>{_esc(t.key)} — "
            f'<b>{t.max_sessions} sessions</b></summary><div class="db">'
        )
        p.append('<div class="ig">')
        p.append(f"<div><b>Host Assignment</b><br>{_esc(t.host_assignment)}</div>")
        p.append(
            f"<div><b>Cores Used</b><br>{t.cores_used} (ids: {_esc(t.core_ids)})</div>"
        )
        p.append(f"<div><b>Measured ({t.tested_side})</b><br>{_tput(meas)}</div>")
        p.append(f"<div><b>Companion ({other})</b><br>{_tput(comp_tput)}</div>")
        p.append("</div>")

        if t.command:
            short = re.sub(r"\S*/RxTxApp/build/RxTxApp", "RxTxApp", t.command)
            short = re.sub(r"\S*/tests/config\.json", "config.json", short)
            p.append(
                f'<div class="cm"><b>Command:</b> <code>{_esc(short)}</code></div>'
            )

        cfg = t.rxtxapp_config or _build_fallback_config(t)
        if cfg:
            p.append(
                f'<div class="cfg"><b>Test Configuration (JSON):</b>'
                f"<pre><code>{_esc(json.dumps(cfg, indent=2))}</code></pre></div>"
            )

        if t.steps:
            p.append(
                '<table class="st"><thead><tr>'
                "<th>Sessions</th><th>Result</th><th>Detail</th>"
                "</tr></thead><tbody>"
            )
            for sessions, passed, detail in t.steps:
                ic = "✓" if passed else "✗"
                cl = "p" if passed else "f"
                p.append(
                    f'<tr class="{cl}"><td class="n">{sessions}</td>'
                    f"<td>{ic}</td><td>{_esc(detail)}</td></tr>"
                )
            p.append("</tbody></table>")

        p.append("</div></details>")

    p.append("</div></div></body></html>")
    return "\n".join(p)


_CSS = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>MTL Performance Report</title>
<style>
:root{
  --brand:#16213e;--brand-light:#1a2744;--accent:#2980b9;
  --green:#27ae60;--red:#e74c3c;
  --bg:#f4f6f9;--card:#fff;--border:#e2e6ed;
  --text:#2c3e50;--text-muted:#6c7a8d;
  --radius:8px;--shadow:0 2px 6px rgba(22,33,62,.08);
  --font-mono:'SF Mono',Monaco,Consolas,'Liberation Mono',monospace;
}
*{margin:0;padding:0;box-sizing:border-box}
body{font:14px/1.6 'Inter',system-ui,-apple-system,'Segoe UI',sans-serif;
  background:var(--bg);color:var(--text)}
.w{max-width:1440px;margin:0 auto;padding:24px 32px}

/* ── Header ── */
.hdr{background:linear-gradient(135deg,var(--brand) 0%,var(--brand-light) 100%);
  color:#fff;padding:28px 32px;border-radius:var(--radius);margin-bottom:28px;
  box-shadow:var(--shadow);text-align:center}
.hdr h1{font-size:1.7em;font-weight:700;letter-spacing:-.02em;margin:0}
.hdr .sub{font-size:.82em;color:rgba(255,255,255,.7);margin-top:4px;font-weight:400}

/* ── Sections ── */
.sec{background:var(--card);border-radius:var(--radius);padding:20px 24px;
  margin-bottom:20px;box-shadow:var(--shadow);border:1px solid var(--border)}
.sec h2{font-size:1.15em;color:var(--brand);margin:0 0 16px;padding-bottom:10px;
  border-bottom:2px solid var(--accent);letter-spacing:-.01em}

/* ── FPS sub-headings ── */
h3{font-size:1em;font-weight:600;color:var(--accent);margin:20px 0 10px;
  padding:8px 16px;background:linear-gradient(90deg,#eaf2fb 0%,transparent 100%);
  border-left:3px solid var(--accent);border-radius:2px 4px 4px 2px}
.sec h3:first-of-type{margin-top:4px}

/* ── KV tables (platform info) ── */
table.kv{width:100%;min-width:320px;border-collapse:collapse;background:var(--card);
  border-radius:6px;overflow:hidden;border:1px solid var(--border)}
table.kv th{text-align:center;background:var(--brand);color:#fff;
  padding:9px 14px;font-size:.82em;font-weight:600;letter-spacing:.03em;text-transform:uppercase}
table.kv td{padding:7px 14px;border-bottom:1px solid var(--border);font-size:.84em}
table.kv td:first-child{font-weight:600;background:#f8f9fb;white-space:nowrap;width:40%;color:var(--text)}
table.kv td:last-child{white-space:normal;word-break:break-word;min-width:200px;color:var(--text-muted)}
table.kv tr:last-child td{border-bottom:none}
.side-by-side{display:flex;gap:24px;flex-wrap:wrap;align-items:flex-start}
.side-by-side>div{flex:1 1 45%;min-width:380px}

/* ── Results tables ── */
table.mx{width:100%;border-collapse:collapse;background:var(--card);
  border-radius:6px;overflow:hidden;border:1px solid var(--border)}
table.mx th{background:var(--brand);color:#fff;padding:10px 14px;text-align:left;
  font-size:.8em;font-weight:600;letter-spacing:.03em;text-transform:uppercase}
table.mx td{padding:9px 14px;border-bottom:1px solid var(--border);font-size:.84em}
table.mx tr:last-child td{border-bottom:none}
table.mx tr:hover{background:#f0f4fa}
table.mx tr:nth-child(even){background:#fafbfc}
table.mx tr:nth-child(even):hover{background:#f0f4fa}
td.n{text-align:center;font-variant-numeric:tabular-nums}
td.ms{text-align:center;font-variant-numeric:tabular-nums;
  font-weight:700;font-size:1em;color:var(--green)}
tr.f td.ms{color:var(--red)}

/* ── Detail cards ── */
details{background:var(--card);border-radius:var(--radius);margin:8px 0;
  border:1px solid var(--border);overflow:hidden}
details summary{padding:12px 16px;cursor:pointer;font-size:.88em;
  background:#f8f9fb;transition:background .15s}
details summary:hover{background:#eef1f6}
details[open] summary{border-bottom:1px solid var(--border)}
.db{padding:16px}
.ig{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));
  gap:10px;margin-bottom:14px}
.ig>div{background:#f8f9fb;padding:10px 14px;border-radius:6px;
  font-size:.84em;border:1px solid var(--border)}
.ig>div b{display:block;font-size:.78em;color:var(--text-muted);
  text-transform:uppercase;letter-spacing:.03em;margin-bottom:2px}
.cm{background:#f0f4ff;padding:10px 14px;border-radius:6px;margin-bottom:12px;
  font-size:.82em;word-break:break-all;border:1px solid #d6e0f0}
.cm code{font-family:var(--font-mono);font-size:.93em}
.cfg{background:#f8faf8;border:1px solid #d4e4d4;padding:10px 14px;
  border-radius:6px;margin-bottom:12px;font-size:.82em}
.cfg pre{margin:8px 0 0;padding:12px;background:var(--brand);color:#e0e0e0;
  border-radius:4px;overflow-x:auto;cursor:text;user-select:all;line-height:1.5}
.cfg code{font-family:var(--font-mono);font-size:.93em}

/* ── Sweep step tables ── */
table.st{width:100%;border-collapse:collapse;font-size:.83em}
table.st th{background:#f0f4fa;padding:7px 12px;text-align:left;font-weight:600;color:var(--text-muted)}
table.st td{padding:5px 12px;border-bottom:1px solid #f0f0f0}
table.st tr.p td:nth-child(2){color:var(--green);font-weight:600}
table.st tr.f td:nth-child(2){color:var(--red);font-weight:600}

.section-title{font-weight:700;font-size:15px;margin:20px 0 6px;color:var(--text)}
@media print{body{background:#fff}.w{max-width:100%}
  .sec{box-shadow:none;border:1px solid #ddd}}
</style></head><body><div class="w">
<div class="hdr">
<h1>MTL ST2110 VF Performance Report</h1>
<div class="sub">Generated {{TS}}</div>
</div>
"""


# ── Main ──────────────────────────────────────────────────────────────


def main():
    ap = argparse.ArgumentParser(description="Generate HTML performance report")
    ap.add_argument("log_root", help="Root dir with timestamped log subdirs")
    ap.add_argument(
        "-o", "--output", default="performance_report.html", help="Output HTML file"
    )
    args = ap.parse_args()

    if not os.path.isdir(args.log_root):
        print(f"Error: {args.log_root} is not a directory", file=sys.stderr)
        sys.exit(1)

    tests, per_host, hosts = scan_logs(args.log_root)
    if not tests:
        print("No sweep results found.", file=sys.stderr)
        sys.exit(1)

    html = _generate_html(tests, per_host, hosts)
    with open(args.output, "w") as f:
        f.write(html)

    sc = sum(1 for t in tests if t.mode == "SC")
    mc = sum(1 for t in tests if t.mode == "MC")
    print(f"Report: {args.output}  ({len(tests)} tests: {sc} SC, {mc} MC)")


if __name__ == "__main__":
    main()
