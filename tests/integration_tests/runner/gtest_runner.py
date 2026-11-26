#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Pure-stdlib KahawaiTest integration runner with disjoint VF-pair parallelism."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import queue
import re
import secrets
import signal
import socket
import subprocess  # nosec B404 # argv lists only, no shell
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET  # nosec B405 # parses local JUnit XML
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
NICCTL, ISOLATE = REPO / "script/nicctl.sh", REPO / "tests/tools/isolate/isolate.sh"
BDF_RE = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$")
CFG_ERR = re.compile(
    r"EAL: Cannot use IOVA as|libmtl\.so: cannot open shared object file:"
    r"|EAL: Cannot set up DMA remapping, error 12|Cannot bind to driver vfio-pci"
    r"|mt_user_params_check\(\d+\), invalid ip"
)
NIC_IDS = {"0x1592": "E810", "0x1593": "E810", "0x12d1": "E830", "0x12d2": "E830"}
NIC_IDS |= {"0x1248": "E835", "0x1249": "E835"}
NOCTX_ISO = (
    "st20p_default_timestamps st20p_user_pacing st20p_exact_user_pacing "
    "st30p_user_pacing vrx_margin rl_staged_overload"
).split()
NOCTX_ENV = {
    "NOCTX_REQUIRE_STRICT": "1",
    "NOCTX_STABILITY_SECONDS": "60",
    "NOCTX_TEST_DURATION": "10",
    "MTL_ISOLATE": "require",
}
_KL = (
    "St20p.tx_create_free_mix:St20p.rx_create_free_mix:St20p.digest_1080p_s1:"
    "St20p.digest_1080i_s2:St20p.put_frame_abort:St20p.query_params:"
    "St20p.session_stats_reset:St20p.update_source_and_destination"
)
_F34 = "St3*:St4*-St30p.redundant*:St40p.redundant*"
_FRD = "St20p.redundant*:St30p.redundant*:St40p.redundant*"
_F_MODES = "NoCtxTest.init_mode_*:NoCtxTest.tx_queue_cnt_24"
_F_NOCTX = (
    "NoCtxTest.*-NoCtxTest.*_pf_*:NoCtxTest.init_mode_*:NoCtxTest.tx_queue_cnt_24"
)

GROUPS = {
    "pr_core": "st2110_20 st2110_20p st2110_22 st2110_3x_4x",
    "core_misc": "core_misc dma_cvt st20p_kernel_loopback redundant_stats",
    "init_modes": "init_modes",
    "noctx": "noctx noctx_pf",
    "pacing_dma": "dma_cvt init_modes",
}
# (name, filter, topo, flags)
# Execution modes (IOVA, pacing, RSS, queues, PTP, hdr_split, init flags) live
# inside self-contained NoCtxTest cases rather than CLI flag-wrapper profiles.
RUNS = (
    ("st2110_20", "St20_rx*:St20_tx*", "vf_pair", "pr,dma,par,bw"),
    ("st2110_20p", "St20p*-St20p.redundant*", "vf_pair", "pr,par,bw"),
    ("st2110_22", "St22*", "vf_pair", "pr,par"),
    ("st2110_3x_4x", _F34, "vf_pair", "pr,par"),
    ("core_misc", "Main*:Misc*:Sch*:TxUserTimestampSync*", "vf_pair", "par"),
    ("dma_cvt", "Dma*:Cvt*", "vf_pair", "dma,par"),
    ("st20p_kernel_loopback", _KL, "kernel_lo", "noroot,par"),
    ("redundant_stats", _FRD, "vf_quad", ""),
    ("init_modes", _F_MODES, "vf_pair", "noctx,par"),
    ("noctx", _F_NOCTX, "cross_pf_quad", "noctx,env"),
    ("noctx_pf", "NoCtxTest.*_pf_*", "pf_pair", "noctx"),
)


class HostFault(RuntimeError):
    pass


def _bdfs(raw: str | None) -> list[str]:
    out = [x for x in re.split(r"[\s,]+", (raw or "").strip()) if x]
    if any(not BDF_RE.match(b) for b in out):
        raise ValueError(f"Invalid PCI BDF in '{raw}'")
    return list(dict.fromkeys(out))


def _nicctl(sub: str, *args: str) -> None:
    cmd = ([] if os.geteuid() == 0 else ["sudo", "-E"]) + [str(NICCTL), sub, *args]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired as exc:
        raise HostFault(f"nicctl.sh {sub} {' '.join(args)} timed out") from exc
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    if r.returncode != 0:
        raise HostFault(f"nicctl.sh {sub} {' '.join(args)} failed (rc={r.returncode})")


def _slot_sip(base_sip: str, slot: int) -> str:
    parts = base_sip.split(".")
    if len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        if slot == 0:
            return base_sip
        oct2 = (int(parts[2]) + slot * 7) % 250 + 1
        return f"{parts[0]}.{parts[1]}.{oct2}.{parts[3]}"
    return f"192.168.{(slot * 7 + 10) % 250 + 1}.1"


def _expand_cpulist(raw: str) -> list[int]:
    cpus: list[int] = []
    for part in raw.strip().split(","):
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            cpus.extend(range(int(lo), int(hi) + 1))
        else:
            cpus.append(int(part))
    return sorted(dict.fromkeys(cpus))


def _allowed_cpus() -> set[int] | None:
    allowed: set[int] | None = None
    if hasattr(os, "sched_getaffinity"):
        try:
            allowed = set(os.sched_getaffinity(0))
        except OSError:
            allowed = None
    online_p = Path("/sys/devices/system/cpu/online")
    if online_p.is_file():
        online = set(_expand_cpulist(online_p.read_text()))
        allowed = online if allowed is None else (allowed & online)
    return allowed


def _physical_cores(cpus: list[int]) -> list[int]:
    """Keep one logical CPU per physical core, excluding core 0 and its siblings."""
    allowed = _allowed_cpus()
    seen: set[int] = set()
    phys: list[int] = []
    for c in cpus:
        if c in seen:
            continue
        if allowed is not None and c not in allowed:
            continue
        sib_p = Path(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list")
        sibs = _expand_cpulist(sib_p.read_text()) if sib_p.is_file() else [c]
        seen.update(sibs)
        if 0 in sibs:
            continue
        phys.append(c)
    return phys


def _partition_lcores(bdf: str, want_slots: int, min_per_slot: int = 6) -> list[str]:
    """Split the PCI device's NUMA physical cores into disjoint --lcores slices."""
    if want_slots <= 1:
        return [""]
    cpulist_p = Path("/sys/bus/pci/devices") / bdf / "local_cpulist"
    if not cpulist_p.is_file():
        return [""]
    cpus = _physical_cores(_expand_cpulist(cpulist_p.read_text()))
    slots = min(want_slots, len(cpus) // min_per_slot)
    if slots <= 1:
        return [""]
    per = len(cpus) // slots
    return [
        ",".join(str(c) for c in cpus[i * per : (i + 1) * per]) for i in range(slots)
    ]


def _setup_ports(
    ports_arg: str | None, selected: list, created: list[str], bound: list[str]
) -> tuple[list[str], list[str], list[str], str]:
    def scan():
        by, pf_vfs, up_ice = {}, {}, []
        sysfs = Path("/sys/bus/pci/devices")
        for d in sorted(sysfs.iterdir()) if sysfs.is_dir() else ():
            cls = (d / "class").read_text().strip() if (d / "class").is_file() else ""
            if not BDF_RE.match(d.name) or not cls.startswith("0x0200"):
                continue
            drv = (d / "driver").resolve().name if (d / "driver").exists() else ""
            pf = (d / "physfn").resolve().name if (d / "physfn").exists() else ""
            dev = d / "device"
            did = dev.read_text().strip().lower() if dev.is_file() else ""
            by[d.name] = (drv, pf, did)
            if drv == "vfio-pci" and pf:
                pf_vfs.setdefault(pf, []).append(d.name)
            nets = sorted((d / "net").iterdir()) if (d / "net").is_dir() else []
            op = nets[0] / "operstate" if nets else None
            if not pf and drv == "ice" and op and op.is_file():
                if op.read_text().strip() == "up":
                    up_ice.append(d.name)
        return by, pf_vfs, up_ice

    pvf = _bdfs(os.environ.get("BIND_PVFS"))
    for bdf in pvf:
        bound.append(bdf)
        _nicctl("bind_pmd", bdf)
    by, pf_vfs, up_ice = scan()
    topos = {r[2] for r in selected}
    need = 4 if "vf_quad" in topos else (2 if "vf_pair" in topos else 0)
    noctx_p: list[str] = []

    if "cross_pf_quad" in topos and not ports_arg and len(pvf) < 4:
        ready = [pf for pf, vfs in sorted(pf_vfs.items()) if len(vfs) >= 2]
        for pf in [p for p in up_ice if p not in ready][: max(0, 2 - len(ready))]:
            created.append(pf)
            _nicctl("create_tvf", pf, "6" if not ready else "2")
            ready.append(pf)
        if created:
            by, pf_vfs, up_ice = scan()
        if len(ready) >= 2:
            a, b = pf_vfs.get(ready[0], []), pf_vfs.get(ready[1], [])
            if len(a) >= 2 and len(b) >= 2:
                noctx_p = [a[0], b[0], a[1], b[1]]

    vf_p = (
        _bdfs(ports_arg) if ports_arg else pf_vfs.get(created[0], []) if created else []
    )
    if need and len(vf_p) < need:
        if ports_arg:
            raise HostFault(
                f"Need {need} ports from --ports, got {len(vf_p)} ({ports_arg})"
            )
        by_pf: dict[str, list[str]] = {}
        for bdf, (drv, pf, _) in sorted(by.items()):
            if drv == "vfio-pci":
                by_pf.setdefault(pf or "_", []).append(bdf)
        cands = [
            v for _, v in sorted(by_pf.items(), key=lambda kv: (-len(kv[1]), kv[0]))
        ]
        found = next((v for v in cands if len(v) >= need), [])
        vf_p = pvf if len(pvf) >= need else found
        avail_ice = [p for p in up_ice if p not in created and p not in pf_vfs]
        if len(vf_p) < need and avail_ice:
            created.append(avail_ice[0])
            _nicctl("create_tvf", avail_ice[0], "6")
            by, pf_vfs, _ = scan()
            vf_p = pf_vfs.get(created[-1], [])
        if len(vf_p) < need:
            raise HostFault(f"Need {need} vfio-pci ports, got {len(vf_p)}")

    noctx_p = noctx_p or (vf_p or pvf)[:4]
    if not pvf:
        pfs = (by[p][1] for p in [*vf_p, *noctx_p] if p in by and by[p][1])
        for pf in dict.fromkeys(pf for pf in pfs if pf not in created):
            _nicctl("vf_rss_lut", pf)
    if any(r[2] != "kernel_lo" for r in selected):
        hp = Path("/sys/kernel/mm/hugepages")
        hfs = [
            hp / s / "free_hugepages"
            for s in ("hugepages-2048kB", "hugepages-1048576kB")
        ]
        if sum(int(p.read_text()) for p in hfs if p.is_file()) == 0:
            raise HostFault("No free 2 MB or 1 GB hugepages on this host")
    nic = os.environ.get("MTL_NIC_LABEL", "")
    for p in [] if nic else (vf_p or noctx_p or pvf):
        pf_id = by.get(by.get(p, ("", "", ""))[1], ("", "", ""))[2]
        nic = NIC_IDS.get(pf_id) or NIC_IDS.get(by.get(p, ("", "", ""))[2], "")
        if nic:
            break
    return vf_p, noctx_p, pvf, nic or "unknown"


def _sudo(root: bool, extra: dict[str, str]) -> list[str]:
    if not root or os.geteuid() == 0:
        return []
    path = f"PATH={os.environ.get('PATH', '')}"
    return ["sudo", "-E", "env", path, *[f"{k}={v}" for k, v in extra.items()]]


def _run_cmd(
    cmd: list[str],
    env: dict[str, str],
    to_s: int,
    tmp: Path,
    log_fh,
    io_lock: threading.Lock,
    active_procs: set[subprocess.Popen] | None = None,
    state_lock: threading.Lock | None = None,
) -> tuple[int, str]:
    with tmp.open("w+", encoding="utf-8", errors="replace") as fh:
        proc = subprocess.Popen(
            cmd, stdout=fh, stderr=subprocess.STDOUT, env=env, start_new_session=True
        )
        if active_procs is not None and state_lock is not None:
            with state_lock:
                active_procs.add(proc)
        try:
            rc = proc.wait(timeout=to_s)
        except subprocess.TimeoutExpired:
            for sig, ws in ((signal.SIGTERM, 15), (signal.SIGKILL, 5)):
                try:
                    os.killpg(proc.pid, sig)
                    proc.wait(timeout=ws)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
            rc = 124
        finally:
            if active_procs is not None and state_lock is not None:
                with state_lock:
                    active_procs.discard(proc)
        fh.flush()
        fh.seek(0)
        out = fh.read()
    try:
        tmp.unlink(missing_ok=True)
    except OSError:
        pass
    if out:
        with io_lock:
            sys.stdout.write(out)
            log_fh.write(out)
            log_fh.flush()
    return rc, out


def _parse_xml(xml: Path, ctx: dict[str, str]) -> list[dict]:
    if not xml.is_file():
        return []
    try:
        root = ET.parse(xml).getroot()  # nosec B314
    except (ET.ParseError, OSError):
        return []
    out = []
    for tc in root.iter("testcase"):
        cls, name = tc.attrib.get("classname", ""), tc.attrib.get("name", "")
        case = f"{cls}.{name}" if cls else name
        fail = tc.find("failure")
        fail = tc.find("error") if fail is None else fail
        skip = tc.find("skipped")
        status = tc.attrib.get("status", "")
        result_attr = tc.attrib.get("result", "")
        if fail is not None:
            res = "failed"
            msg = (fail.attrib.get("message") or fail.text or "").strip()[:400]
        elif skip is not None or result_attr == "skipped":
            res = "skipped"
            msg = (skip.attrib.get("message") if skip is not None else "") or ""
        elif status == "notrun" or result_attr == "suppressed":
            is_dis = (
                cls.startswith("DISABLED_")
                or name.startswith("DISABLED_")
                or "/DISABLED_" in name
            )
            res = "skipped" if is_dis else "not_run"
            msg = "disabled" if is_dis else "not run (aborted)"
        else:
            res, msg = "passed", ""
        ms = round(float(tc.attrib.get("time", "0") or 0) * 1000, 1)
        out.append(
            {"case": case, **ctx, "result": res, "duration_ms": ms, "message": msg}
        )
    return out


def _list_cases(binary: Path, flt: str) -> list[str]:
    cmd = [str(binary), "--gtest_list_tests", f"--gtest_filter={flt}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        raise RuntimeError(f"--gtest_list_tests failed (rc={r.returncode}) for '{flt}'")
    cases, suite = [], ""
    for ln in r.stdout.splitlines():
        ln = ln.split("#", 1)[0].rstrip()
        if ln and not ln.startswith(" ") and ln.endswith("."):
            suite = ln[:-1]
        elif ln.startswith("  ") and suite:
            name = ln.strip()
            if (
                suite.startswith("DISABLED_")
                or name.startswith("DISABLED_")
                or "/DISABLED_" in name
            ):
                continue
            cases.append(f"{suite}.{name}")
    return cases


def main(argv: list[str] | None = None) -> int:
    e = os.environ
    ap = argparse.ArgumentParser(description="MTL KahawaiTest integration runner.")
    add = ap.add_argument
    add("--ports", default=e.get("TEST_PORTS"))
    add("--group", choices=tuple(GROUPS), default=e.get("GTEST_GROUP") or None)
    add("--profile", default=e.get("GTEST_CASES") or None)
    add(
        "--tier",
        choices=("pr", "nightly"),
        default="pr" if e.get("NIGHTLY") == "0" else "nightly",
    )
    tmp = str(Path(tempfile.gettempdir()) / f"mtl_gtest_{os.getpid()}")
    add("--out-dir", default=e.get("GTEST_OUT_DIR") or e.get("TMP_FOLDER") or tmp)
    add("--retries", type=int, default=int(e.get("MAX_RETRIES", "2")))
    add("--timeout", type=int, default=int(e.get("TEST_CASE_TIMEOUT", "1800")))
    add(
        "--jobs",
        type=int,
        default=int(e.get("GTEST_JOBS", "0")),
        help="Concurrent DPDK VF-pair workers (0=auto up to 3, 1=sequential).",
    )
    add(
        "--exit-on-failure",
        action="store_true",
        default=e.get("EXIT_ON_FAILURE") == "1",
    )
    add("--binary", default=e.get("KAHAWAI_TEST_BINARY") or None)
    add("--p-sip", default=e.get("TEST_P_SIP") or f"192.168.{secrets.randbelow(200)}.1")
    add("--log-file", default=e.get("LOG_FILE") or None)
    a = ap.parse_args(argv)
    if a.jobs < 0:
        return print(f"Error: --jobs must be >= 0, got {a.jobs}", file=sys.stderr) or 2

    loc = REPO / ".local_install/mtl/bin/KahawaiTest"
    bld = REPO / "build/tests/KahawaiTest"
    if a.binary:
        binary = Path(a.binary)
    elif bld.is_file() and (
        not loc.is_file() or bld.stat().st_mtime >= loc.stat().st_mtime
    ):
        binary = bld
    else:
        binary = loc
    allowed = set(GROUPS[a.group].split()) if a.group else None
    selected = [
        r
        for r in RUNS
        if (a.tier == "nightly" or "pr" in r[3].split(","))
        and (allowed is None or r[0] in allowed)
        and (not a.profile or re.search(a.profile, r[0]))
    ]
    if not selected:
        return print("Error: no profiles selected", file=sys.stderr) or 2
    if not binary.is_file():
        return print(f"Error: KahawaiTest not found at {binary}", file=sys.stderr) or 1

    out = Path(a.out_dir).resolve()
    (out / "junit").mkdir(parents=True, exist_ok=True)
    jsonl = out / "results.jsonl"
    jsonl.write_text("", encoding="utf-8")
    log_p = Path(a.log_file).resolve() if a.log_file else out / "gtest.log"
    log_p.parent.mkdir(parents=True, exist_ok=True)
    created, bound, records, done = [], [], [], set()
    io_lock = threading.Lock()
    state_lock = threading.Lock()
    dma_lock = threading.Lock()
    bw_lock = threading.Lock()
    stop_ev = threading.Event()
    active_procs: set[subprocess.Popen] = set()
    sig_rc = [0]
    tmp_seq = [0]

    def next_tmp(tag: str) -> Path:
        with state_lock:
            tmp_seq[0] += 1
            return out / f"tmp_{tag}_{tmp_seq[0]}.log"

    def kill_active(sig: int) -> None:
        with state_lock:
            procs = list(active_procs)
        for p in procs:
            try:
                os.killpg(p.pid, sig)
            except ProcessLookupError:
                pass

    def cleanup() -> None:
        for sub, items in (("bind_kernel", bound), ("disable_vf", created)):
            batch, items[:] = list(items), []
            for bdf in batch:
                try:
                    _nicctl(sub, bdf)
                except HostFault as exc:
                    print(f"Warning: {exc}", file=sys.stderr)

    def _sig(signum: int, _f) -> None:
        sig_rc[0] = 128 + signum
        stop_ev.set()
        kill_active(signal.SIGTERM)
        raise KeyboardInterrupt(f"signal {signum}")

    pi, pt = signal.signal(signal.SIGINT, _sig), signal.signal(signal.SIGTERM, _sig)
    host, t0, fault, failed = socket.gethostname(), time.monotonic(), "", False
    nic = e.get("MTL_NIC_LABEL") or "unknown"
    noctx_to, delay = int(e.get("NOCTX_CASE_TIMEOUT", "2100")), int(
        e.get("RETRY_DELAY", "5")
    )

    def emit(
        ctx: dict[str, str], case: str, res: str, ms: float = 0.0, msg: str = ""
    ) -> None:
        rec = {"case": case, **ctx, "result": res, "duration_ms": ms, "message": msg}
        with io_lock:
            records.append(rec)
            with jsonl.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(rec) + "\n")

    with log_p.open("w", encoding="utf-8", errors="replace") as log_fh:

        def tee(msg: str) -> None:
            with io_lock:
                sys.stdout.write(msg)
                log_fh.write(msg)
                log_fh.flush()

        def run_case(ctx, base, case, root, extra, to_s, start, first="", iso=False):
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", case)
            sweep = [*_sudo(True, {}), str(ISOLATE), "--sweep"]
            sh = ("GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX")
            env = {k: v for k, v in {**e, **extra}.items() if k not in sh}
            fin = {
                "case": case,
                **ctx,
                "result": "failed",
                "duration_ms": 0.0,
                "message": first,
            }
            for att in range(start, max(1, a.retries) + 1):
                if stop_ev.is_set():
                    return {**fin, "result": "not_run", "message": "aborted"}
                if att > 1 and delay > 0:
                    if stop_ev.wait(delay):
                        return {**fin, "result": "not_run", "message": "aborted"}
                xml = out / "junit" / f"{ctx['profile']}_{safe}_try{att}.xml"
                xml.unlink(missing_ok=True)
                iso_p = [str(ISOLATE), "--"] if iso else []
                fa, xa = f"--gtest_filter={case}", f"--gtest_output=xml:{xml}"
                cmd = [*_sudo(root, extra), *iso_p, *base, fa, xa]
                if att > 1:
                    tee(f"  retrying {case} ({att}/{a.retries})\n")
                if iso:
                    subprocess.run(sweep, capture_output=True)
                rc, txt = _run_cmd(
                    cmd,
                    env,
                    to_s,
                    next_tmp(safe),
                    log_fh,
                    io_lock,
                    active_procs,
                    state_lock,
                )
                if iso:
                    subprocess.run(sweep, capture_output=True)
                if rc != 0 and (m := CFG_ERR.search(txt)):
                    raise HostFault(f"Config error in {case}: {m.group(0)}")
                parsed = _parse_xml(xml, ctx)
                st = "passed" if rc == 0 else "failed"
                cur = (
                    parsed[0]
                    if parsed
                    else {**fin, "result": st, "message": "" if rc == 0 else f"rc={rc}"}
                )
                if rc != 0 and cur["result"] in ("passed", "skipped"):
                    cur = {**cur, "result": "failed", "message": f"rc={rc}"}
                if cur["result"] in ("passed", "skipped"):
                    if att > 1 and cur["result"] == "passed":
                        ms = f"Passed on retry {att}" + (
                            f"; first: {first}" if first else ""
                        )
                        return {**cur, "result": "flaky", "message": ms}
                    return cur
                fin = cur
            return fin

        def run_shared(name, flt, topo, flags, pair_ports, sip, lcores=""):
            penv = dict(NOCTX_ENV) if "env" in flags else {}
            root = "noroot" not in flags
            ctx = {"profile": name, "nic": nic, "host": host}
            xml = out / "junit" / f"gtest_{name}.xml"
            xml.unlink(missing_ok=True)
            if topo == "kernel_lo":
                pargs = ["--p_port", "kernel:lo", "--r_port", "kernel:lo"]
            elif topo == "vf_quad":
                pargs = ["--port_list", ",".join(pair_ports[:4])]
            else:
                pargs = ["--p_port", pair_ports[0], "--r_port", pair_ports[1]]
            base = [str(binary), f"--p_sip={sip}", *pargs]
            if lcores:
                base.append(f"--lcores={lcores}")
            if a.tier == "pr" and a.retries < 2:
                base.append("--gtest_fail_fast")
            xa, fa = f"--gtest_output=xml:{xml}", f"--gtest_filter={flt}"
            cmd = [*_sudo(root, penv), *base, xa, fa]
            lc_msg = f" lcores={lcores}" if lcores else ""
            tee(
                f"\n==> [{name}] topology={topo} ports={pair_ports}{lc_msg} filter={flt}\n"
            )
            rc, txt = _run_cmd(
                cmd,
                {**e, **penv},
                a.timeout,
                next_tmp(name),
                log_fh,
                io_lock,
                active_procs,
                state_lock,
            )
            parsed = _parse_xml(xml, ctx)
            prof_failed = False
            if rc != 0 and not parsed:
                if m := CFG_ERR.search(txt):
                    raise HostFault(f"Config error in {name}: {m.group(0)}")
                emit(ctx, f"{name}.<process>", "failed", msg=f"rc={rc} without XML")
                prof_failed = True
            has_xml_fail = any(r["result"] in ("failed", "not_run") for r in parsed)
            for r in parsed:
                if r["result"] == "failed" and a.retries >= 2 and not stop_ev.is_set():
                    r = run_case(
                        ctx, base, r["case"], root, penv, a.timeout, 2, r["message"]
                    )
                emit(ctx, r["case"], r["result"], r["duration_ms"], r["message"])
                prof_failed = prof_failed or r["result"] in ("failed", "not_run")
            if rc != 0 and parsed and not has_xml_fail:
                emit(ctx, f"{name}.<process>", "failed", msg=f"process exited rc={rc}")
                prof_failed = True
            return prof_failed

        try:
            vf_p, noctx_p, pf_p, nic = _setup_ports(a.ports, selected, created, bound)
            vf_pairs = [(vf_p[i], vf_p[i + 1]) for i in range(0, len(vf_p) - 1, 2)]
            want_dpdk = (
                max(1, min(a.jobs, len(vf_pairs)))
                if a.jobs > 0
                else max(1, min(3, len(vf_pairs)))
            )
            lcore_slices = (
                _partition_lcores(vf_pairs[0][0], want_dpdk) if vf_pairs else [""]
            )
            max_dpdk = max(1, min(want_dpdk, len(lcore_slices)))
            vf_pairs = vf_pairs[:max_dpdk]

            par_runs = [r for r in selected if "par" in r[3].split(",")]
            ser_runs = [r for r in selected if "par" not in r[3].split(",")]

            if par_runs:
                # Populate disjoint VF pair pool (disjoint --p_sip and --lcores per slot).
                pair_pool: queue.Queue[tuple[int, tuple[str, str], str]] = queue.Queue()
                for idx, pair in enumerate(vf_pairs):
                    lc = lcore_slices[idx] if idx < len(lcore_slices) else ""
                    pair_pool.put((idx, pair, lc))

                # Build parallel task list: TSC init_mode cases first (before any RL).
                tasks: list[tuple] = []
                noctx_par_names: set[str] = set()
                for name, flt, topo, flag_s in par_runs:
                    flags = set(flag_s.split(",")) if flag_s else set()
                    if "noctx" in flags:
                        cases = _list_cases(binary, flt)
                        # Run TSC pacing before RL on any VF pair (DPDK < 26.07.91).
                        cases.sort(key=lambda c: (0 if "tsc" in c else 1, c))
                        tee(
                            f"\n==> [{name}] queuing {len(cases)} per-process cases "
                            f"(jobs={max_dpdk})\n"
                        )
                        noctx_par_names.add(name)
                        for c in cases:
                            tasks.append(("noctx_case", name, c, topo, flags))
                    else:
                        tasks.append(("shared", name, flt, topo, flags))

                # Place TSC noctx_case tasks before RL/AUTO tasks so VF pairs stay clean.
                tasks.sort(
                    key=lambda t: (
                        0
                        if (t[0] == "noctx_case" and "tsc" in t[2])
                        else (1 if t[3] == "kernel_lo" else 2)
                    )
                )

                has_klo = any(t[3] == "kernel_lo" for t in tasks)
                klo_lcores = ""
                if has_klo and vf_pairs and max_dpdk > 1:
                    klo_slices = _partition_lcores(vf_pairs[0][0], max_dpdk + 1)
                    if len(klo_slices) >= max_dpdk + 1:
                        lcore_slices = klo_slices[:max_dpdk]
                        klo_lcores = klo_slices[max_dpdk]
                        pair_pool = queue.Queue()
                        for idx, pair in enumerate(vf_pairs):
                            pair_pool.put((idx, pair, lcore_slices[idx]))
                    else:
                        has_klo = False
                pool_workers = max_dpdk + (1 if has_klo else 0)
                if len(tasks) > 1 and pool_workers > 1:
                    tee(
                        f"\n==> Parallel dispatch: {len(tasks)} tasks across "
                        f"{max_dpdk} VF pair(s)"
                        f"{' + kernel:lo' if has_klo else ''}\n"
                    )

                def _exec_task(task: tuple) -> tuple[str, bool]:
                    kind, name, arg, topo, flags = task
                    if stop_ev.is_set():
                        return name, False
                    need_pair = topo == "vf_pair"
                    need_dma = "dma" in flags
                    need_bw = "bw" in flags
                    slot_item = None
                    with contextlib.ExitStack() as stack:
                        if need_bw:
                            stack.enter_context(bw_lock)
                        if need_dma:
                            stack.enter_context(dma_lock)
                        try:
                            if need_pair:
                                slot_item = pair_pool.get()
                                slot_id, pair, lc = slot_item
                                sip = _slot_sip(a.p_sip, slot_id)
                            else:
                                pair, lc = ("kernel:lo", "kernel:lo"), klo_lcores
                                sip = _slot_sip(a.p_sip, 100)
                            if stop_ev.is_set():
                                return name, False
                            if kind == "shared":
                                pf = run_shared(name, arg, topo, flags, pair, sip, lc)
                                with state_lock:
                                    done.add(name)
                                return name, pf
                            ctx = {"profile": name, "nic": nic, "host": host}
                            base = [
                                str(binary),
                                f"--p_sip={sip}",
                                f"--p_port={pair[0]}",
                                f"--r_port={pair[1]}",
                            ]
                            if lc:
                                base.append(f"--lcores={lc}")
                            r = run_case(
                                ctx, base, arg, True, {}, noctx_to, 1, iso=False
                            )
                            emit(
                                ctx,
                                r["case"],
                                r["result"],
                                r["duration_ms"],
                                r["message"],
                            )
                            return name, r["result"] in ("failed", "not_run")
                        finally:
                            if slot_item is not None:
                                pair_pool.put(slot_item)

                with ThreadPoolExecutor(max_workers=pool_workers) as ex:
                    futs = [ex.submit(_exec_task, t) for t in tasks]
                    try:
                        for fut in as_completed(futs):
                            _, task_failed = fut.result()
                            if task_failed:
                                failed = True
                                if a.exit_on_failure:
                                    stop_ev.set()
                    except BaseException:
                        stop_ev.set()
                        kill_active(signal.SIGTERM)
                        for fut in futs:
                            fut.cancel()
                        raise
                for n in noctx_par_names:
                    done.add(n)

            for name, flt, topo, flag_s in ser_runs:
                if failed and a.exit_on_failure:
                    break
                flags = set(flag_s.split(",")) if flag_s else set()
                penv = dict(NOCTX_ENV) if "env" in flags else {}
                root = "noroot" not in flags
                ctx = {"profile": name, "nic": nic, "host": host}
                if "noctx" not in flags:
                    ports = vf_p[:4] if topo == "vf_quad" else vf_p[:2]
                    if run_shared(name, flt, topo, flags, ports, a.p_sip):
                        failed = True
                else:
                    ports, need = (
                        (pf_p[:2], 2) if topo == "pf_pair" else (noctx_p[:4], 4)
                    )
                    if len(ports) < need:
                        if topo == "pf_pair":
                            tee(f"\n==> [{name}] skipped (no PF ports)\n")
                            emit(ctx, f"{name}.*", "skipped", msg="no PF ports")
                            done.add(name)
                            continue
                        raise HostFault(f"{name} needs {need} ports, got {len(ports)}")
                    pl = ",".join(ports)
                    base = [str(binary), f"--p_sip={a.p_sip}", f"--port_list={pl}"]
                    cases = _list_cases(binary, flt)
                    tee(f"\n==> [{name}] {len(cases)} per-process cases on {pl}\n")
                    for c in cases:
                        iso = any(sub in c for sub in NOCTX_ISO) and ISOLATE.is_file()
                        ie = {}
                        if iso:
                            ie = {
                                "MTL_ISOLATE_PORTS": ",".join(ports[0::2]),
                                "MTL_ISOLATE_MEM_PORTS": ",".join(ports[1::2]),
                            }
                        r = run_case(
                            ctx, base, c, root, {**penv, **ie}, noctx_to, 1, iso=iso
                        )
                        emit(
                            ctx, r["case"], r["result"], r["duration_ms"], r["message"]
                        )
                        if r["result"] == "failed":
                            failed = True
                            if a.exit_on_failure:
                                break
                done.add(name)
        except KeyboardInterrupt:
            stop_ev.set()
            kill_active(signal.SIGTERM)
            if not sig_rc[0]:
                sig_rc[0] = 130
        except HostFault as exc:
            fault = str(exc)
            for name, *_ in selected:
                if name not in done:
                    ctx = {"profile": name, "nic": nic, "host": host}
                    emit(ctx, f"{name}.*", "not_run", msg=f"host_fault: {fault}")
        finally:
            cleanup()
            signal.signal(signal.SIGINT, pi)
            signal.signal(signal.SIGTERM, pt)
        keys = ("passed", "failed", "flaky", "skipped", "not_run")
        cnt = {k: sum(1 for r in records if r["result"] == k) for k in keys}
        el = round(time.monotonic() - t0, 1)
        tee(f"\n=== GTEST SUMMARY ({host}/{nic}): {cnt} elapsed={el}s ===\n")
    if sig_rc[0]:
        return sig_rc[0]
    return 3 if fault else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
