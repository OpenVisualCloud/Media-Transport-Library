# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""The host config of a stability run (``stability.yaml``).

Example: ``stability.yaml.example``. Each path that is not absolute is relative
to the MTL tree. The EBU LIST password is never in this file: ``password_file``
names a file of ``KEY=VALUE`` lines, read as text (``EBU_PASSWORD=...``).
"""

from __future__ import annotations

import dataclasses
import glob
import json
import os
import re
import shutil
from typing import Optional

import yaml


def cpu_list(text: str) -> list[int]:
    """'2-4,8' -> [2, 3, 4, 8]."""
    cpus: list[int] = []
    for part in str(text or "").split(","):
        part = part.strip()
        if not part:
            continue
        lo, _, hi = part.partition("-")
        cpus.extend(range(int(lo), int(hi or lo) + 1))
    return cpus


def cpu_text(cpus: list[int]) -> str:
    """[2, 3, 4, 8] -> '2-4,8'."""
    out, run = [], []
    for c in sorted(set(cpus)):
        if run and c == run[-1] + 1:
            run.append(c)
            continue
        if run:
            out.append(f"{run[0]}-{run[-1]}" if len(run) > 1 else str(run[0]))
        run = [c]
    if run:
        out.append(f"{run[0]}-{run[-1]}" if len(run) > 1 else str(run[0]))
    return ",".join(out)


def read_kv_file(path: str) -> dict[str, str]:
    """KEY=VALUE lines; no shell, no expansion."""
    values: dict[str, str] = {}
    with open(os.path.expanduser(path), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip().strip("'\"")
    return values


@dataclasses.dataclass
class EbuConfig:
    ip: str = ""
    user: str = ""
    password_file: str = ""
    allow_wide: bool = False  # TSC pacing in a VM is often only "wide"
    keep_failed_pcaps: int = 20
    keep_failed_mb: int = 2000  # and at most this much: a 1 s UHD capture is ~1.3 GB

    def password(self) -> str:
        if not self.password_file:
            return ""
        kv = read_kv_file(self.password_file)
        return kv.get("EBU_PASSWORD") or kv.get("EBU_LIST_PASSWORD", "")


@dataclasses.dataclass
class CaptureConfig:
    interface: str = ""  # a kernel netdev with hardware RX timestamps
    # Or a command that captures for the engine, e.g. on a VM host that owns the
    # PHC: "mtl-capture {group} {port} {seconds} {out}". It prints "dropped=<n>".
    command: str = ""
    dir: str = "/var/tmp/mtl-stability/pcap"
    video_frames: int = 6  # frames of one video capture
    audio_seconds: float = 2.0
    anc_seconds: float = 4.0
    gap_s: float = 5.0  # pause between two captures


@dataclasses.dataclass
class CpuConfig:
    housekeeping: str = ""  # every process of the host that is not MTL
    app: str = ""  # app threads of the workers, the MTL control threads
    lcores: str = ""  # the MTL schedulers: an isolated cpuset partition
    fence: bool = True  # False: run with no CPU fence (debug only)


@dataclasses.dataclass
class HostConfig:
    mtl_path: str = ""
    build_dir: str = "build"  # the meson build of MTL in the tree
    rxtxapp: str = ""
    mtl_manager: str = ""
    ffmpeg: str = ""
    ffmpeg_jpegxs: bool = True
    gst_launch: str = ""
    gst_plugin_path: str = ""
    ld_library_path: str = ""
    plugins: dict = dataclasses.field(default_factory=dict)  # name -> .so path
    st22_codec: str = ""  # "jpegxs" (SVT) or "sample"; empty: no st22p
    media_dir: str = "/dev/shm/mtl-stability-media"
    run_dir: str = "/var/tmp/mtl-stability/runs"
    tx_ports: list = dataclasses.field(default_factory=list)
    rx_ports: list = dataclasses.field(default_factory=list)
    pacing_way: str = ""  # "", rl, tsc, tsn, ...
    ptp: bool = False
    user_time_offset_ns: int = 0
    # The rxtxapp-integrity worker checks the SHA-256 of each frame. Its user-meta
    # packet (sequence number 0) is not ST 2110 conform, so its streams get no
    # EBU LIST verdict; false leaves the worker out.
    integrity: bool = True
    lcore_slices: dict = dataclasses.field(default_factory=dict)
    cpus: CpuConfig = dataclasses.field(default_factory=CpuConfig)
    capture: CaptureConfig = dataclasses.field(default_factory=CaptureConfig)
    ebu: EbuConfig = dataclasses.field(default_factory=EbuConfig)
    scan_interval_s: float = 10.0
    bind_ports: bool = True  # script/nicctl.sh bind_pmd on each port first
    # A port may be a MAC address. A VF on vfio-pci has no netdev, so the
    # engine keeps each MAC -> PCI address it reads in this file.
    mac_table: str = "/var/tmp/mtl-stability/vf-macs"
    max_error_lines: int = 200

    # -- derived -----------------------------------------------------------

    def path(self, p: str) -> str:
        p = os.path.expanduser(p)
        return p if os.path.isabs(p) else os.path.join(self.mtl_path, p)

    def kahawai_json(self, *, st22: bool = False, convert: bool = False) -> str:
        """The MTL plugin file of one worker: RxTxApp reads it from its cwd."""
        plugins = []
        if st22 and self.st22_codec == "jpegxs" and "st22_svt_jpegxs" in self.plugins:
            plugins.append(
                {
                    "enabled": 1,
                    "name": "st22_svt_jpegxs",
                    "path": self.plugins["st22_svt_jpegxs"],
                }
            )
        if st22 and self.st22_codec == "sample" and "st22_sample" in self.plugins:
            plugins.append(
                {
                    "enabled": 1,
                    "name": "st22_sample",
                    "path": self.plugins["st22_sample"],
                }
            )
        if convert and "color_convert_sample" in self.plugins:
            plugins.append(
                {
                    "enabled": 1,
                    "name": "color_convert_sample",
                    "path": self.plugins["color_convert_sample"],
                }
            )
        return json.dumps({"plugins": plugins}, indent=2)

    def check(self) -> list[str]:
        """What is missing for a run, in words."""
        problems = []
        if not os.path.isfile(self.rxtxapp):
            problems.append(f"no RxTxApp at {self.rxtxapp}")
        if not self.tx_ports or not self.rx_ports:
            problems.append("ports: tx and rx need at least one PCI address each")
        if self.ffmpeg and not shutil.which(self.ffmpeg):
            problems.append(f"no ffmpeg at {self.ffmpeg}")
        if self.gst_launch and not shutil.which(self.gst_launch):
            problems.append(f"no gst-launch-1.0 at {self.gst_launch}")
        for name, so in self.plugins.items():
            if not os.path.isfile(so):
                problems.append(f"plugin {name}: no file {so}")
        if self.cpus.fence and not (self.cpus.app and self.cpus.lcores):
            problems.append("cpus: app and lcores are necessary for the fence")
        if self.capture.command and not shutil.which(self.capture.command.split()[0]):
            problems.append(f"capture: no command {self.capture.command.split()[0]}")
        if self.capture.interface and not os.path.isdir(
            f"/sys/class/net/{self.capture.interface}"
        ):
            problems.append(f"capture: no netdev {self.capture.interface}")
        if self.ebu.ip and not self.ebu.password():
            problems.append(f"ebu: no EBU_PASSWORD in {self.ebu.password_file}")
        return problems


MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$", re.I)


def _live_macs() -> dict[str, str]:
    """MAC -> PCI address of each netdev that is a PCI function."""
    out = {}
    for n in glob.glob("/sys/class/net/*"):
        dev = os.path.join(n, "device")
        if not os.path.exists(dev):
            continue
        bdf = os.path.basename(os.path.realpath(dev))
        if re.match(r"^[0-9a-f]{4}:", bdf):
            out[_read_text(os.path.join(n, "address")).lower()] = bdf
    return out


def _read_text(path: str) -> str:
    try:
        with open(path, encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return ""


def resolve_port(port: str, table: str) -> str:
    """A PCI address as it is; a MAC address -> the PCI address of its function."""
    if not MAC_RE.match(port):
        return port
    known = {}
    if table and os.path.exists(table):
        for line in open(table, encoding="ascii"):
            parts = line.split()
            if len(parts) == 2:
                known[parts[0].lower()] = parts[1]
    live = _live_macs()
    if any(known.get(m) != b for m, b in live.items()) and table:
        known.update(live)
        os.makedirs(os.path.dirname(table), exist_ok=True)
        with open(table, "w", encoding="ascii") as f:
            f.writelines(f"{m} {b}\n" for m, b in sorted(known.items()))
    bdf = live.get(port.lower()) or known.get(port.lower())
    if not bdf:
        raise ValueError(f"port {port}: no PCI function has this MAC (see {table})")
    return bdf


def _sub(cls, data: Optional[dict]):
    data = data or {}
    names = {f.name for f in dataclasses.fields(cls)}
    unknown = set(data) - names
    if unknown:
        raise ValueError(f"{cls.__name__}: unknown keys {sorted(unknown)}")
    return cls(**data)


def load(path: str) -> HostConfig:
    with open(path, encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    cpus = _sub(CpuConfig, data.pop("cpus", None))
    capture = _sub(CaptureConfig, data.pop("capture", None))
    ebu = _sub(EbuConfig, data.pop("ebu", None))
    ports = data.pop("ports", {}) or {}
    host = _sub(HostConfig, data)
    host.cpus, host.capture, host.ebu = cpus, capture, ebu
    host.tx_ports = [resolve_port(p, host.mac_table) for p in ports.get("tx", [])]
    host.rx_ports = [resolve_port(p, host.mac_table) for p in ports.get("rx", [])]
    host.mtl_path = os.path.expanduser(host.mtl_path or os.getcwd())
    host.rxtxapp = host.path(host.rxtxapp or "tests/tools/RxTxApp/build/RxTxApp")
    if host.mtl_manager:
        host.mtl_manager = host.path(host.mtl_manager)
    host.plugins = {k: host.path(v) for k, v in (host.plugins or {}).items()}
    if host.ebu.password_file:
        host.ebu.password_file = os.path.expanduser(host.ebu.password_file)
        # /etc/mtl-ci/runner.env of a CI runner holds all three.
        if os.path.isfile(host.ebu.password_file) and not (
            host.ebu.ip and host.ebu.user
        ):
            kv = read_kv_file(host.ebu.password_file)
            host.ebu.ip = host.ebu.ip or kv.get("EBU_IP", "")
            host.ebu.user = host.ebu.user or kv.get("EBU_USER", "")
    # YAML reads "rxtxapp-extconv: 20" as an int, and a CPU list key alike.
    host.lcore_slices = {k: str(v) for k, v in (host.lcore_slices or {}).items()}
    for f in ("housekeeping", "app", "lcores"):
        setattr(host.cpus, f, str(getattr(host.cpus, f)))
    if not host.lcore_slices and host.cpus.lcores:
        host.lcore_slices = default_lcore_slices(cpu_list(host.cpus.lcores))
    return host


def default_lcore_slices(lcores: list[int]) -> dict[str, str]:
    """Split the lcore CPUs among the RxTxApp workers: half to the mix TX.

    FFmpeg and GStreamer take no lcore list; MtlManager gives them free lcores
    of their own CPU mask (the app CPUs plus the rest of the lcore CPUs).
    """
    n = len(lcores)
    if n < 4:
        return {}
    # The rest (2 or more) is for FFmpeg and GStreamer.
    tx_n = max(2, n * 3 // 10)
    rx_n = max(1, n // 5)
    ext_n = 1
    int_n = max(1, n // 5)
    it = iter(lcores)
    slices = {}
    for name, k in (
        ("rxtxapp-mix", tx_n),
        ("rxtxapp-rx", rx_n),
        ("rxtxapp-extconv", ext_n),
        ("rxtxapp-integrity", int_n),
    ):
        slices[name] = cpu_text([next(it) for _ in range(k)])
    return slices


# ---------------------------------------------------------------------------
# A host config from the facts of the host, for the pytest wrapper in CI
# ---------------------------------------------------------------------------

HOUSEKEEPING_CORES = 2


def _node_of(bdf: str) -> int:
    try:
        with open(f"/sys/bus/pci/devices/{bdf}/numa_node", encoding="ascii") as f:
            node = int(f.read())
    except (OSError, ValueError):
        node = -1
    return max(node, 0)


def _cores_of_node(node: int) -> list[list[int]]:
    """The physical cores of a NUMA node, each as [thread, sibling, ...]."""
    seen, cores = set(), []
    for c in cpu_list(
        open(f"/sys/devices/system/node/node{node}/cpulist", encoding="ascii").read()
    ):
        if c in seen:
            continue
        path = f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list"
        try:
            sib = cpu_list(open(path, encoding="ascii").read())
        except OSError:
            sib = [c]
        seen.update(sib)
        cores.append(sib)
    return cores


def auto_cpus(port: str) -> CpuConfig:
    """housekeeping: each other node and the first cores of the port node;
    app: 40 % of the other cores (both threads); lcores: the first thread of
    each core that is left (its sibling stays idle, as in a busy-poll fence)."""
    node = _node_of(port)
    cores = _cores_of_node(node)
    if len(cores) < HOUSEKEEPING_CORES + 4:
        raise RuntimeError(
            f"NUMA node {node} has {len(cores)} cores; a fence needs 6 or more"
        )
    all_cpus = cpu_list(open("/sys/devices/system/cpu/online", encoding="ascii").read())
    node_cpus = {c for core in cores for c in core}
    house = [c for c in all_cpus if c not in node_cpus]
    house += [c for core in cores[:HOUSEKEEPING_CORES] for c in core]
    rest = cores[HOUSEKEEPING_CORES:]
    n_app = max(2, len(rest) * 2 // 5)
    app = [c for core in rest[:n_app] for c in core]
    lcores = [core[0] for core in rest[n_app:]]
    return CpuConfig(
        housekeeping=cpu_text(house), app=cpu_text(app), lcores=cpu_text(lcores)
    )


def auto_host(
    mtl_path: str,
    tx_ports: list,
    rx_ports: list,
    *,
    capture_interface: str = "",
    ebu_ip: str = "",
    ebu_user: str = "",
    ebu_password_file: str = "",
    **overrides,
) -> HostConfig:
    install = os.path.join(mtl_path, ".local_install")
    host = HostConfig(
        mtl_path=mtl_path, tx_ports=list(tx_ports), rx_ports=list(rx_ports)
    )
    host.rxtxapp = host.path("tests/tools/RxTxApp/build/RxTxApp")
    ffmpeg = os.path.join(install, "bin", "ffmpeg")
    host.ffmpeg = ffmpeg if os.path.isfile(ffmpeg) else (shutil.which("ffmpeg") or "")
    host.gst_launch = shutil.which("gst-launch-1.0") or ""
    host.cpus = auto_cpus(tx_ports[0])
    host.lcore_slices = default_lcore_slices(cpu_list(host.cpus.lcores))
    host.capture = CaptureConfig(interface=capture_interface)
    host.ebu = EbuConfig(ip=ebu_ip, user=ebu_user, password_file=ebu_password_file)
    for k, v in overrides.items():
        setattr(host, k, v)
    return host
