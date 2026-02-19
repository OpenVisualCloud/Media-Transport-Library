#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2026 Intel Corporation
"""Generate an HTML performance report from pytest log directories.

Usage:
    python generate_report.py <log_root> [-o report.html]

Scans all timestamped subdirectories under <log_root> for pytest.log files,
extracts sweep results, platform config, topology, throughput and CPU cores,
and produces a single self-contained HTML file.
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
class HostInfo:
    name: str = ""
    role: str = ""
    ip: str = ""
    pci: list = field(default_factory=list)
    dsa_device: str = ""
    vfs: list = field(default_factory=list)
    is_dut: bool = False
    nic_numa: str = ""
    dsa_pci: str = ""
    dsa_numa: str = ""
    numa_match: str = ""
    firmware_version: str = ""
    used_vf: str = ""


@dataclass
class IterationStep:
    sessions: int
    passed: bool
    detail: str


@dataclass
class SweepTest:
    """One completed sweep test with all extracted data."""

    key: str = ""
    max_sessions: int = 0
    tested_side: str = ""  # TX / RX
    mode: str = ""  # SC / MC
    redundant: bool = False
    dsa: bool = False
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
    steps: list = field(default_factory=list)
    rxtxapp_config: dict = field(default_factory=dict)


# ── Regex patterns ────────────────────────────────────────────────────

RE_HOST_ASSIGN = re.compile(
    r"Host assignment.*?(?:SUT|DUT)=(\S+)\s.*?measured (TX|RX)\s*\|"
    r"\s*TX=(\S+),\s*RX=(\S+)"
)
RE_SWEEP_RESULT = re.compile(
    r"Sweep: max (\d+) sessions for "
    r"(REDUNDANT )?(TX|RX) (SC|MC)"
    r"(?: with DSA \([^)]+\))?"
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
RE_TOPO_HOST = re.compile(r"^\s*-\s*name:\s*(\S+)")
RE_TOPO_ROLE = re.compile(r"role:\s*(\S+)")
RE_TOPO_PCI = re.compile(r"pci_address:\s*(\S+)")
RE_TOPO_IP = re.compile(r"ip_address:\s*(\S+)")
RE_TOPO_DSA = re.compile(r"dsa_device:\s*(\S+)")
RE_TOPO_DUT = re.compile(r"is_dut:\s*(true|false)", re.IGNORECASE)

# DSA configuration summary (box-drawing table in log)
RE_BOX_NIC = re.compile(r"║\s*NIC:\s+(\S+)\s*║")
RE_BOX_NIC_NUMA = re.compile(r"║\s*NIC NUMA:\s*(\S+)")
RE_BOX_DSA_DEV = re.compile(r"║\s*DSA Device:\s*(\S+)")
RE_BOX_DSA_NUMA = re.compile(r"║\s*DSA NUMA:\s*(\S+)")
RE_BOX_NUMA_MATCH = re.compile(r"║\s*NUMA Match:\s*(.+?)║")


# ── Parsing ───────────────────────────────────────────────────────────


def _parse_topology(log_path: str) -> list[HostInfo]:
    """Extract host topology from YAML dump in the log header."""
    hosts: list[HostInfo] = []
    in_topo = False
    cur: HostInfo | None = None

    with open(log_path, "r", errors="replace") as f:
        for line in f:
            if "topology_config.yaml content:" in line:
                in_topo = True
                continue
            if in_topo and re.match(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", line):
                if cur:
                    hosts.append(cur)
                break
            if not in_topo:
                continue

            m = RE_TOPO_HOST.search(line)
            if m:
                if cur:
                    hosts.append(cur)
                cur = HostInfo(name=m.group(1))
                continue
            if cur is None:
                continue
            for pat, attr in [
                (RE_TOPO_ROLE, "role"),
                (RE_TOPO_IP, "ip"),
                (RE_TOPO_DSA, "dsa_device"),
            ]:
                m = pat.search(line)
                if m:
                    setattr(cur, attr, m.group(1))
            m = RE_TOPO_PCI.search(line)
            if m:
                cur.pci.append(m.group(1))
            m = RE_TOPO_DUT.search(line)
            if m:
                cur.is_dut = m.group(1).lower() == "true"

    # VFs
    vf_map: dict[str, list[str]] = {}
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            m = RE_VFS.search(line)
            if m:
                vfs = [v.strip().strip("'\"") for v in m.group(2).split(",")]
                vf_map[m.group(1)] = vfs
    for h in hosts:
        if h.name in vf_map:
            h.vfs = vf_map[h.name]

    return hosts


def _parse_dsa_box(log_path: str, hosts: list[HostInfo]):
    """Parse DSA configuration box-drawing summary from log, update SUT host."""
    sut_host = None
    for h in hosts:
        if h.is_dut or h.role == "sut":
            sut_host = h
            break
    if not sut_host:
        return
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            m = RE_BOX_NIC.search(line)
            if m:
                sut_host.used_vf = m.group(1)
            m = RE_BOX_NIC_NUMA.search(line)
            if m and m.group(1) not in ("?",):
                sut_host.nic_numa = m.group(1)
            m = RE_BOX_DSA_DEV.search(line)
            if m:
                sut_host.dsa_pci = m.group(1)
            m = RE_BOX_DSA_NUMA.search(line)
            if m:
                sut_host.dsa_numa = m.group(1)
            m = RE_BOX_NUMA_MATCH.search(line)
            if m:
                sut_host.numa_match = m.group(1).strip()


_RES_MAP = {"1080p": (1920, 1080), "2160p": (3840, 2160), "4320p": (7680, 4320)}


def _build_fallback_config(t) -> dict:
    """Synthesize a config dict from SweepTest metadata when no actual
    RXTXAPP_CONFIG was captured in the log (backward compat with old logs).
    """
    w, h = _RES_MAP.get(t.resolution, (0, 0))
    session_key = "tx_sessions" if t.tested_side == "TX" else "rx_sessions"
    opposite_key = "rx_sessions" if t.tested_side == "TX" else "tx_sessions"

    session_block = {
        "st20p": [
            {
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
        ],
    }
    if t.tested_side == "TX":
        session_block["st20p"][0]["input_format"] = "YUV422RFC4175PG2BE10"

    cfg: dict = {"interfaces": [], session_key: [session_block], opposite_key: []}

    # Extract CLI args from command string
    if t.command:
        m = re.search(r"--dma_dev\s+(\S+)", t.command)
        if m:
            cfg["_cli_dma_dev"] = m.group(1)
        m = re.search(r"--sch_session_quota\s+(\d+)", t.command)
        if m:
            cfg["_cli_sch_session_quota"] = int(m.group(1))
        if "--disable_migrate" in t.command:
            cfg["_cli_disable_migrate"] = True
        m = re.search(r"--test_time\s+(\d+)", t.command)
        if m:
            cfg["_cli_test_time"] = int(m.group(1))

    if t.redundant:
        cfg["_mode"] = "redundant (ST2022-7)"

    cfg["_note"] = "reconstructed from sweep metadata (config not captured in log)"
    return cfg


def _parse_sweeps(log_path: str, dir_name: str) -> list[SweepTest]:
    """Parse pytest.log for sweep tests with associated telemetry."""
    tests: list[SweepTest] = []

    # Per-test-block state
    host_assign = ""
    cmd = ""
    cores = core_ids = ""
    m_tx = m_rx = c_tx = c_rx = 0.0
    in_m = in_c = False
    steps: list[IterationStep] = []
    collecting = False
    # RxTxApp config collection
    in_config = False
    config_lines: list[str] = []
    rxtxapp_config: dict = {}

    with open(log_path, "r", errors="replace") as f:
        for line in f:
            # New test block
            m = RE_HOST_ASSIGN.search(line)
            if m:
                host_assign = line.strip()
                # strip log prefix  (timestamp + logger)
                ha = re.sub(
                    r"^\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+\S+\s+\S+\s*",
                    "",
                    host_assign,
                )
                host_assign = ha.strip()
                cmd = ""
                cores = core_ids = ""
                m_tx = m_rx = c_tx = c_rx = 0.0
                in_m = in_c = False
                steps = []
                collecting = False
                in_config = False
                config_lines = []
                rxtxapp_config = {}
                continue

            # Command
            mc = RE_TEST_CMD.search(line)
            if mc:
                cmd = mc.group(1).strip()
                continue

            # CPU cores
            mc = RE_CPU_CORES.search(line)
            if mc:
                cores = mc.group(1)
                core_ids = mc.group(2)
                continue

            # Throughput section headers
            if RE_MEASURED_HDR.search(line):
                in_m, in_c = True, False
                continue
            if RE_COMPANION_HDR.search(line):
                in_c, in_m = True, False
                continue

            # DEV throughput values
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

            # SWEEP SUMMARY block → collect steps
            if "SWEEP SUMMARY:" in line:
                collecting = True
                steps = []
                continue
            if collecting:
                ms = RE_STEP.search(line)
                if ms:
                    steps.append(
                        IterationStep(
                            sessions=int(ms.group(1)),
                            passed=ms.group(2) == "✓",
                            detail=ms.group(3).strip(),
                        )
                    )
                    continue  # RxTxApp config block within sweep summary
                if "RXTXAPP_CONFIG_BEGIN" in line:
                    in_config = True
                    config_lines = []
                    continue
                if "RXTXAPP_CONFIG_END" in line:
                    in_config = False
                    try:
                        rxtxapp_config = json.loads("\n".join(config_lines))
                    except (json.JSONDecodeError, ValueError):
                        rxtxapp_config = {}
                    continue
                if in_config:
                    # Strip log prefix: "TIMESTAMP LOGGER LEVEL  content"
                    stripped = re.sub(
                        r"^\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+\S+\s+\S+\s*",
                        "",
                        line,
                    ).rstrip()
                    config_lines.append(stripped)
                    continue
            # Sweep result → finalize
            ms = RE_SWEEP_RESULT.search(line)
            if ms:
                ts_m = re.match(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", line)
                st = SweepTest(
                    max_sessions=int(ms.group(1)),
                    tested_side=ms.group(3),
                    mode=ms.group(4),
                    redundant=bool(ms.group(2)),
                    dsa="DSA" in line,
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
                ds = " +DSA" if st.dsa else ""
                st.key = f"{rd}{st.tested_side} {st.mode}{ds} @ {st.fps}fps / {st.resolution}"
                tests.append(st)
                collecting = False
                continue

    return tests


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
    """Scan log dirs → (tests, per_host_platforms, hosts).

    *per_host_platforms* is ``{hostname: {sw_configuration: …, hw_configuration: …}}``.
    Falls back to the legacy single ``platform_config.json`` keyed as ``"_legacy"``.
    """
    best: dict[str, SweepTest] = {}
    per_host: dict[str, dict] = {}
    legacy_platform = None
    hosts: list[HostInfo] = []

    log_root = os.path.abspath(log_root)
    dirs = sorted(
        d
        for d in os.listdir(log_root)
        if os.path.isdir(os.path.join(log_root, d)) and re.match(r"\d{4}-", d)
    )

    for dn in dirs:
        dp = os.path.join(log_root, dn)
        lp = os.path.join(dp, "pytest.log")
        if not os.path.exists(lp):
            continue
        # Per-host platform configs
        ph = _load_per_host_platforms(dp)
        if ph:
            per_host.update(ph)
        # Legacy single file
        pc = _load_platform(dp)
        if pc:
            legacy_platform = pc
        topo = _parse_topology(lp)
        if topo:
            hosts = topo
        for t in _parse_sweeps(lp, dn):
            best[t.key] = t

    # Also check top-level log_root for per-host configs
    ph = _load_per_host_platforms(log_root)
    if ph:
        per_host.update(ph)
    if not per_host and legacy_platform:
        per_host["_legacy"] = legacy_platform

    # Second pass: enrich hosts with DSA box info from any log
    if hosts:
        for dn in dirs:
            lp = os.path.join(log_root, dn, "pytest.log")
            if os.path.exists(lp):
                _parse_dsa_box(lp, hosts)

    return list(best.values()), per_host, hosts


# ── HTML ──────────────────────────────────────────────────────────────


def _esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _tput(mbps: float) -> str:
    if mbps <= 0:
        return "—"
    return f"{mbps / 1000:.2f} Gb/s" if mbps >= 1000 else f"{mbps:.1f} Mb/s"


def _fix_mtl_version(ver: str) -> str:
    """Return *ver* if it looks valid, else try local git describe."""
    if ver and len(ver) >= 4 and ver not in ("b,", "N/A"):
        return ver
    import subprocess

    try:
        return (
            subprocess.check_output(
                ["git", "describe", "--tags"],
                cwd=os.path.dirname(
                    os.path.dirname(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
                    )
                ),
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
            or ver
        )
    except Exception:
        return ver


# Resolution label → (width, height)
def _generate_html(tests, per_host, hosts) -> str:
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    p = [_CSS.replace("{{TS}}", now)]

    # ── Platform & topology ──
    # Find DUT and companion hosts
    dut = comp = None
    for h in hosts:
        if h.is_dut:
            dut = h
        elif h.role == "client":
            comp = h
    if not dut:
        for h in hosts:
            if h.role == "sut" and h != comp:
                dut = h
                break
    if not comp:
        for h in hosts:
            if h != dut:
                comp = h
                break

    # Resolve per-host platform data
    def _plat(h: HostInfo | None) -> dict:
        if not h:
            return per_host.get("_legacy", {})
        if h.name in per_host:
            return per_host[h.name]
        return per_host.get("_legacy", {})

    dut_plat = _plat(dut)
    comp_plat = _plat(comp)

    p.append('<div class="sec"><h2>Platform &amp; Topology</h2>')

    # -- helper --
    def _kv_table(title: str, rows: list[tuple[str, str]]) -> str:
        h = [
            f'<div class="section-title">{title}</div>',
            '<table class="kv"><tr><th>Parameter</th><th>Value</th></tr>',
        ]
        for k, v in rows:
            h.append(f"<tr><td>{k}</td><td>{v}</td></tr>")
        h.append("</table>")
        return "\n".join(h)

    dut_label = f"Host 1 ({_esc(dut.name)}) [DUT]" if dut else "Host 1 [DUT]"
    comp_label = f"Host 2 ({_esc(comp.name)})" if comp else "Host 2"

    # -- SW Configuration side-by-side (per-host) --
    def _sw_fields(plat: dict) -> list[tuple[str, str]]:
        sw = plat.get("sw_configuration", {})
        mtl_ver = _fix_mtl_version(sw.get("mtl_version", "N/A") or "N/A")
        return [
            ("OS", sw.get("os", "N/A")),
            ("Kernel", sw.get("kernel", "N/A")),
            ("MTL Version", mtl_ver),
            ("DPDK Version", sw.get("dpdk_driver", "N/A")),
            ("ICE Version", sw.get("ice_version", "N/A")),
            ("BIOS VT-X &amp; VT-D", _esc(sw.get("bios_vtx_vtd", "N/A"))),
            ("Hugepages", sw.get("hugepages", "N/A")),
            ("CPU Cores", sw.get("cpu_cores", "N/A")),
        ]

    p.append('<div class="side-by-side">')
    p.append(
        f"<div>{_kv_table(f'SW Configuration — {dut_label}', _sw_fields(dut_plat))}</div>"
    )
    p.append(
        f"<div>{_kv_table(f'SW Configuration — {comp_label}', _sw_fields(comp_plat))}</div>"
    )
    p.append("</div>")

    # -- HW Configuration side-by-side (per-host) --
    def _hw_fields(plat: dict) -> list[tuple[str, str]]:
        hw = plat.get("hw_configuration", {})
        return [
            ("Server", hw.get("server", "N/A")),
            ("CPU", hw.get("cpu", "N/A")),
            ("Memory (RAM)", hw.get("memory", "N/A")),
            ("NIC", hw.get("nic", "N/A")),
        ]

    p.append('<div class="side-by-side">')
    p.append(
        f"<div>{_kv_table(f'HW Configuration — {dut_label}', _hw_fields(dut_plat))}</div>"
    )
    p.append(
        f"<div>{_kv_table(f'HW Configuration — {comp_label}', _hw_fields(comp_plat))}</div>"
    )
    p.append("</div>")

    # -- NIC Configuration side-by-side (per-host) --
    def _nic_rows(h: HostInfo | None, plat: dict) -> list[tuple[str, str]]:
        if not h:
            return [("NIC", "N/A")]
        hw = plat.get("hw_configuration", {})
        pf = h.pci[0] if h.pci else "N/A"
        vf = h.used_vf or (h.vfs[0] if h.vfs else "N/A")
        fw = hw.get("firmware_version", "") or "N/A"
        numa = hw.get("nic_numa", "") or h.nic_numa or "N/A"
        return [
            ("Host", _esc(h.name)),
            ("NIC Interface Type", "VF"),
            ("PCI Address (PF)", _esc(pf)),
            ("PCI Address (VF)", _esc(vf)),
            ("Firmware Version", _esc(fw)),
            ("NUMA Node", numa),
        ]

    p.append('<div class="side-by-side">')
    p.append(
        f"<div>{_kv_table(f'NIC Configuration — {dut_label}', _nic_rows(dut, dut_plat))}</div>"
    )
    p.append(
        f"<div>{_kv_table(f'NIC Configuration — {comp_label}', _nic_rows(comp, comp_plat))}</div>"
    )
    p.append("</div>")

    # -- DSA Configuration side-by-side --
    def _dsa_rows(h: HostInfo | None, is_dut: bool) -> list[tuple[str, str]]:
        if not is_dut:
            return [("DSA", "Not used (companion)")]
        if not h or (not h.dsa_device and not h.dsa_pci):
            return [("DSA", "Not used")]
        return [
            ("DSA Device", _esc(h.dsa_pci or h.dsa_device)),
            ("DSA NUMA Node", h.dsa_numa if h.dsa_numa else "N/A"),
            ("NUMA Match", _esc(h.numa_match) if h.numa_match else "N/A"),
        ]

    p.append('<div class="side-by-side">')
    p.append(
        f"<div>{_kv_table(f'DSA Configuration — {dut_label}', _dsa_rows(dut, True))}</div>"
    )
    p.append(
        f"<div>{_kv_table(f'DSA Configuration — {comp_label}', _dsa_rows(comp, False))}</div>"
    )
    p.append("</div>")

    p.append("</div>")

    # ── Summary tables: SC then MC ──
    for label, mode in [("Single Core (SC)", "SC"), ("Multi Core (MC)", "MC")]:
        group = sorted(
            [t for t in tests if t.mode == mode],
            key=lambda x: (x.resolution, x.fps, x.dsa, x.redundant),
        )
        if not group:
            continue
        p.append(f'<div class="sec"><h2>{label} — Max Sessions</h2>')
        p.append(
            '<table class="mx"><thead><tr>'
            "<th>Tested Side</th><th>Resolution</th><th>FPS</th>"
            "<th>Max Sessions</th>"
            "<th>Cores</th><th>Measured Throughput</th><th>Run</th>"
            "</tr></thead><tbody>"
        )
        for t in group:
            cl = "p" if t.max_sessions > 0 else "f"
            meas = t.measured_dev_rx if t.tested_side == "RX" else t.measured_dev_tx
            side = f"REDUNDANT {t.tested_side}" if t.redundant else t.tested_side
            if t.dsa:
                side += " +DSA"
            p.append(
                f'<tr class="{cl}"><td>{side}</td>'
                f"<td>{t.resolution}</td><td>{t.fps}</td>"
                f'<td class="n">{t.max_sessions}</td>'
                f'<td class="n">{t.cores_used}</td>'
                f"<td>{_tput(meas)}</td>"
                f"<td>{t.log_dir}</td></tr>"
            )
        p.append("</tbody></table></div>")

    # ── Detail cards ──
    p.append('<div class="sec"><h2>Sweep Details</h2>')
    for t in sorted(tests, key=lambda x: (x.mode, x.resolution, x.fps, x.dsa)):
        meas = t.measured_dev_rx if t.tested_side == "RX" else t.measured_dev_tx
        comp = t.companion_dev_tx if t.tested_side == "RX" else t.companion_dev_rx
        other = "TX" if t.tested_side == "RX" else "RX"

        p.append(
            f"<details><summary>{_esc(t.key)} — "
            f'<b>{t.max_sessions} sessions</b></summary><div class="db">'
        )

        # Info grid
        p.append('<div class="ig">')
        p.append(f"<div><b>Host Assignment</b><br>{_esc(t.host_assignment)}</div>")
        p.append(
            f"<div><b>Cores Used</b><br>{t.cores_used} (ids: {_esc(t.core_ids)})</div>"
        )
        p.append(f"<div><b>Measured ({t.tested_side})</b><br>{_tput(meas)}</div>")
        p.append(f"<div><b>Companion ({other})</b><br>{_tput(comp)}</div>")
        p.append("</div>")

        # Command
        if t.command:
            short = t.command
            for prefix in [
                "/root/awilczyn/Media-Transport-Library/tests/tools/RxTxApp/build/",
                "/root/awilczyn/Media-Transport-Library/tests/",
            ]:
                short = short.replace(prefix, "")
            p.append(
                f'<div class="cm"><b>Command:</b> <code>{_esc(short)}</code></div>'
            )

        # Test config JSON (actual or reconstructed from sweep metadata)
        cfg = t.rxtxapp_config or _build_fallback_config(t)
        if cfg:
            cfg_json = json.dumps(cfg, indent=2)
            p.append(
                f'<div class="cfg"><b>Test Configuration (JSON):</b>'
                f"<pre><code>{_esc(cfg_json)}</code></pre></div>"
            )

        # Steps
        if t.steps:
            p.append(
                '<table class="st"><thead><tr>'
                "<th>Sessions</th><th>Result</th><th>Detail</th>"
                "</tr></thead><tbody>"
            )
            for s in t.steps:
                ic = "✓" if s.passed else "✗"
                cl = "p" if s.passed else "f"
                p.append(
                    f'<tr class="{cl}"><td class="n">{s.sessions}</td>'
                    f"<td>{ic}</td><td>{_esc(s.detail)}</td></tr>"
                )
            p.append("</tbody></table>")

        p.append("</div></details>")

    p.append("</div></div></body></html>")
    return "\n".join(p)


_CSS = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>MTL Performance Report</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font:14px/1.5 system-ui,-apple-system,sans-serif;background:#f5f5f5;color:#333}
.w{max-width:1500px;margin:0 auto;padding:20px}
h1{text-align:center;padding:20px 0;color:#1a1a2e;font-size:1.8em}
h1 small{display:block;font-size:.5em;color:#666;font-weight:normal}
h2{margin:24px 0 12px;color:#16213e;border-bottom:2px solid #0f3460;padding-bottom:6px}
h3{color:#0f3460;margin:0 0 8px;font-size:.95em}
.sec{margin-bottom:28px}
.section-title{font-weight:700;font-size:15px;margin:20px 0 6px;color:#333}
table.kv{width:100%;min-width:320px;border-collapse:collapse;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.1);border-radius:6px;overflow:hidden}
table.kv th{text-align:center;background:#16213e;color:#fff;padding:8px 12px;font-size:.85em}
table.kv td{padding:6px 12px;border-bottom:1px solid #eee;font-size:.85em}
table.kv td:first-child{font-weight:600;background:#f5f7fa;white-space:nowrap;width:40%}
table.kv td:last-child{white-space:normal;word-break:break-word;min-width:200px}
.side-by-side{display:flex;gap:30px;flex-wrap:wrap;align-items:flex-start}
.side-by-side>div{flex:1 1 45%;min-width:380px}
table.mx{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.1)}
table.mx th{background:#16213e;color:#fff;padding:9px 12px;text-align:left;font-size:.83em}
table.mx td{padding:7px 12px;border-bottom:1px solid #eee;font-size:.83em}
table.mx tr:hover{background:#f0f4ff}
table.mx tr.p td:nth-child(6){color:#27ae60;font-weight:700}
table.mx tr.f td:nth-child(6){color:#e74c3c;font-weight:700}
td.n{text-align:center;font-variant-numeric:tabular-nums}
details{background:#fff;border-radius:8px;margin:10px 0;box-shadow:0 1px 3px rgba(0,0,0,.1)}
details summary{padding:12px 16px;cursor:pointer;font-size:.9em;background:#f8f9fa;border-radius:8px}
details[open] summary{border-radius:8px 8px 0 0;border-bottom:1px solid #eee}
.db{padding:14px 16px}
.ig{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px;margin-bottom:12px}
.ig>div{background:#f8f9fa;padding:8px 12px;border-radius:6px;font-size:.85em}
.cm{background:#f0f4ff;padding:8px 12px;border-radius:6px;margin-bottom:12px;font-size:.82em;word-break:break-all}
.cm code{font-family:'SF Mono',Monaco,Consolas,monospace;font-size:.95em}
.cfg{background:#f8faf8;border:1px solid #e0e8e0;padding:8px 12px;border-radius:6px;margin-bottom:12px;font-size:.82em}
.cfg pre{margin:6px 0 0;padding:10px;background:#1a1a2e;color:#e0e0e0;border-radius:4px;overflow-x:auto;cursor:text;user-select:all}
.cfg code{font-family:'SF Mono',Monaco,Consolas,monospace;font-size:.95em}
table.st{width:100%;border-collapse:collapse;font-size:.83em}
table.st th{background:#f0f4ff;padding:7px 12px;text-align:left}
table.st td{padding:5px 12px;border-bottom:1px solid #f0f0f0}
table.st tr.p td:nth-child(2){color:#27ae60}
table.st tr.f td:nth-child(2){color:#e74c3c}
@media print{body{background:#fff}.w{max-width:100%}}
</style></head><body><div class="w">
<h1>MTL ST2110 VF Performance Report<small>Generated {{TS}}</small></h1>
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
