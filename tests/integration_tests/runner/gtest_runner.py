#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Lean KahawaiTest integration runner (replaces gtest.sh, noctx/run.sh, noctx/run_pf.sh).

Shell (nic_setup.sh) performs only create-tvf and bind-pvf (plus teardown).
Profiles and runs come from tests/integration_tests/profiles.yaml.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import signal
import socket
import subprocess  # nosec B404 # runs KahawaiTest and nic_setup.sh via argv lists, no shell
import sys
import time
import xml.etree.ElementTree as ET  # nosec B405 # parses local JUnit XML produced by KahawaiTest
from pathlib import Path
from typing import Any

import yaml

HOST_FAULT_EXIT = 3
BDF_RE = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$")
RUNNER_DIR = Path(__file__).resolve().parent
REPO_ROOT = RUNNER_DIR.parent.parent.parent
DEFAULT_PROFILES = RUNNER_DIR.parent / "profiles.yaml"

CONFIG_ERRORS = (
    re.compile(r"EAL: Cannot use IOVA as"),
    re.compile(r"libmtl\.so: cannot open shared object file:"),
    re.compile(r"EAL: Cannot set up DMA remapping, error 12"),
    re.compile(r"Cannot bind to driver vfio-pci"),
    re.compile(r"mt_user_params_check\(\d+\), invalid ip"),
)
NIC_IDS = {
    "0x1592": "E810",
    "0x1593": "E810",
    "0x12d1": "E830",
    "0x12d2": "E830",
    "0x1248": "E835",
    "0x1249": "E835",
}


class HostFault(RuntimeError):
    pass


def load_profiles(yaml_path: Path = DEFAULT_PROFILES) -> list[dict[str, Any]]:
    data = yaml.safe_load(yaml_path.read_text(encoding="utf-8")) or {}
    pdefs = data.get("profiles") or {}
    group_of = {
        name: str(grp)
        for grp, members in (data.get("groups") or {}).items()
        for name in (members or [])
    }
    runs: list[dict[str, Any]] = []
    for tier, items in (data.get("runs") or {}).items():
        for r in items or []:
            pd = pdefs.get(r.get("profile", "default")) or {}
            env = {str(k): str(v) for k, v in (pd.get("env") or {}).items()}
            env.update({str(k): str(v) for k, v in (r.get("env") or {}).items()})
            runs.append(
                {
                    "name": str(r["name"]),
                    "tier": str(tier),
                    "group": group_of.get(str(r["name"]), ""),
                    "mode": "noctx" if pd.get("instance") == "own" else "shared",
                    "topology": str(r["topology"]),
                    "filter": str(r["filter"]),
                    "extra_args": [str(x) for x in (pd.get("extra_args") or [])],
                    "env": env,
                    "dma_auto": bool(r.get("dma_auto", False)),
                    "needs_root": bool(r.get("needs_root", True)),
                }
            )
    return runs


def parse_bdfs(raw: str | None) -> list[str]:
    if not raw:
        return []
    out = [x for x in re.split(r"[\s,]+", raw.strip()) if x]
    for bdf in out:
        if not BDF_RE.match(bdf):
            raise ValueError(f"Invalid PCI BDF '{bdf}'")
    return list(dict.fromkeys(out))


def _rtxt(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def scan_nics(sysfs_pci: Path) -> list[dict[str, str]]:
    if not sysfs_pci.is_dir():
        return []
    nics = []
    for dev in sorted(sysfs_pci.iterdir()):
        if not BDF_RE.match(dev.name) or not _rtxt(dev / "class").startswith("0x0200"):
            continue
        drv = (dev / "driver").resolve().name if (dev / "driver").exists() else ""
        pf = (dev / "physfn").resolve().name if (dev / "physfn").exists() else ""
        nets = sorted((dev / "net").iterdir()) if (dev / "net").is_dir() else []
        state = _rtxt(nets[0] / "operstate").lower() if nets else ""
        nics.append(
            {
                "bdf": dev.name,
                "drv": drv,
                "pf": pf,
                "state": state,
                "id": _rtxt(dev / "device").lower(),
            }
        )
    return nics


def detect_nic(nics: list[dict[str, str]], ports: list[str]) -> str:
    by = {n["bdf"]: n for n in nics}
    for port in ports:
        info = by.get(port, {})
        for dev_id in (
            by.get(info.get("pf", ""), {}).get("id", ""),
            info.get("id", ""),
        ):
            if dev_id in NIC_IDS:
                return NIC_IDS[dev_id]
    return os.environ.get("MTL_NIC_LABEL", "unknown")


def list_cases(binary: Path, flt: str) -> list[str]:
    cmd = [str(binary), "--gtest_list_tests", f"--gtest_filter={flt}"]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
    if res.returncode != 0:
        raise RuntimeError(
            f"--gtest_list_tests failed (rc={res.returncode}) for '{flt}'"
        )
    cases, suite = [], ""
    for line in res.stdout.splitlines():
        line = line.split("#", 1)[0].rstrip()
        if line and not line.startswith(" ") and line.endswith("."):
            suite = line[:-1]
        elif line.startswith("  ") and suite:
            cases.append(f"{suite}.{line.strip()}")
    return cases


def list_case_metadata(binary: Path) -> dict[str, dict[str, Any]]:
    res = subprocess.run(
        [str(binary), "--list-json"],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if res.returncode != 0:
        return {}
    try:
        return {
            it["case"]: it
            for it in json.loads(res.stdout)
            if isinstance(it, dict) and "case" in it
        }
    except json.JSONDecodeError:
        return {}


def parse_xml(
    xml_path: Path, ctx: dict[str, str], attempt: int
) -> list[dict[str, Any]]:
    if not xml_path.is_file():
        return []
    out = []
    for tc in (
        ET.parse(xml_path)  # nosec B314 # parses local JUnit XML written by KahawaiTest
        .getroot()
        .iter("testcase")
    ):
        cls, name = tc.attrib.get("classname", ""), tc.attrib.get("name", "")
        full = f"{cls}.{name}" if cls else name
        fail = tc.find("failure")
        if fail is None:
            fail = tc.find("error")
        skip = tc.find("skipped")
        if fail is not None:
            res, msg = (
                "failed",
                (fail.attrib.get("message") or fail.text or "").strip()[:400],
            )
        elif skip is not None or tc.attrib.get("result") == "skipped":
            res = "skipped"
            msg = (skip.attrib.get("message") if skip is not None else "") or ""
        else:
            res, msg = "passed", ""
        ms = round(float(tc.attrib.get("time", "0") or 0) * 1000, 1)
        out.append(
            {
                **ctx,
                "case": full,
                "attempt": attempt,
                "result": res,
                "duration_ms": ms,
                "message": msg,
            }
        )
    return out


def run_cmd(
    cmd: list[str], env: dict[str, str], log_path: Path, timeout_s: int, quiet: bool
) -> tuple[int, str]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w+", encoding="utf-8", errors="replace") as fh:
        proc = subprocess.Popen(
            cmd, stdout=fh, stderr=subprocess.STDOUT, env=env, start_new_session=True
        )
        try:
            rc = proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            for sig, wait_s in ((signal.SIGTERM, 15), (signal.SIGKILL, 5)):
                try:
                    os.killpg(proc.pid, sig)
                    proc.wait(timeout=wait_s)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
            rc = 124
        fh.flush()
        fh.seek(0)
        out = fh.read()
    if not quiet and out:
        sys.stdout.write(out)
    return rc, out


def nic_shell(nic_setup: Path, args: list[str], dry_run: bool) -> list[str]:
    if dry_run:
        print(f"[dry-run] {nic_setup} {' '.join(args)}")
        return []
    proc = subprocess.run(
        [str(nic_setup), *args],
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        raise HostFault(f"nic_setup.sh {' '.join(args)} failed (rc={proc.returncode})")
    return [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]


def _sudo(needs_root: bool, no_sudo: bool, extra_env: dict[str, str]) -> list[str]:
    if not needs_root or os.geteuid() == 0 or no_sudo:
        return []
    path_kv = f"PATH={os.environ.get('PATH', '')}"
    return ["sudo", "-E", "env", path_kv, *[f"{k}={v}" for k, v in extra_env.items()]]


def _check_config(label: str, out: str) -> None:
    for pat in CONFIG_ERRORS:
        if pat.search(out):
            raise HostFault(f"Config error in {label}: {pat.pattern}")


def _mk(
    ctx: dict[str, str], case: str, att: int, res: str, msg: str = ""
) -> dict[str, Any]:
    return {
        **ctx,
        "case": case,
        "attempt": att,
        "result": res,
        "duration_ms": 0.0,
        "message": msg,
    }


def _run_case(
    a: argparse.Namespace,
    ctx: dict[str, str],
    base: list[str],
    case: str,
    root: bool,
    env_map: dict[str, str],
    timeout_s: int,
    start_att: int,
    out_dir: Path,
    isolate_sh: Path,
    first_msg: str = "",
    iso: bool = False,
) -> dict[str, Any]:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", case)
    sweep = [*_sudo(True, a.no_sudo, {}), str(isolate_sh), "--sweep"]
    final = _mk(ctx, case, start_att, "failed", first_msg)
    for att in range(start_att, max(1, a.retries) + 1):
        if att > 1 and a.retry_delay > 0:
            time.sleep(a.retry_delay)
        xml = out_dir / "junit" / f"{ctx['profile']}_{safe}_try{att}.xml"
        log = out_dir / "logs" / f"{ctx['profile']}_{safe}_try{att}.log"
        inner = [*base, f"--gtest_filter={case}", f"--gtest_output=xml:{xml}"]
        pfx = _sudo(root, a.no_sudo, env_map)
        cmd = [*pfx, str(isolate_sh), "--", *inner] if iso else [*pfx, *inner]
        if att > 1:
            print(f"  ↻ retrying {case} ({att}/{a.retries})")
        if iso:
            subprocess.run(sweep, capture_output=True, check=False)
        rc, out = run_cmd(cmd, {**os.environ, **env_map}, log, timeout_s, a.quiet)
        if iso:
            subprocess.run(sweep, capture_output=True, check=False)
        parsed = parse_xml(xml, ctx, att)
        if rc != 0:
            _check_config(case, out)
        cur = (
            parsed[0]
            if parsed
            else _mk(
                ctx,
                case,
                att,
                "passed" if rc == 0 else "failed",
                "" if rc == 0 else f"rc={rc}",
            )
        )
        if cur["result"] in ("passed", "skipped"):
            if att > 1 and cur["result"] == "passed":
                msg = (
                    f"Passed on retry {att}; first: {first_msg}"
                    if first_msg
                    else f"Passed on retry {att}"
                )
                return {**cur, "result": "flaky", "message": msg}
            return cur
        final = cur
    return final


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="MTL KahawaiTest runner (shell: create-tvf + bind-pvf only)."
    )
    add = ap.add_argument
    add("--binary")
    add("--repo-root", default=str(REPO_ROOT))
    add("--profiles", default=str(DEFAULT_PROFILES))
    add("--nic-setup", default=str(RUNNER_DIR / "nic_setup.sh"))
    add("--tier", choices=("pr", "nightly"), default="nightly")
    add("--group", help="Run only profiles in this profiles.yaml group")
    add("--profile", help="Regex of profile names")
    add("--filter", help="Override gtest_filter")
    add("--tvf-pf", help="PF BDF(s) for nic_setup.sh create-tvf")
    add("--tvf-count", type=int, default=6)
    add("--bind-pvf", help="PCI BDF(s) for nic_setup.sh bind-pvf")
    add("--ports", help="Already-bound VF/DPDK port BDFs")
    add("--pf-ports", help="Already-bound PF port BDFs for noctx_pf")
    add("--out-dir", default=f"/tmp/mtl_gtest_{os.getpid()}")
    add("--log-file", help="Optional combined log file path for CI compatibility")
    add("--p-sip", default=f"192.168.{secrets.randbelow(256)}.1")
    add("--retries", type=int, default=2)
    add("--retry-delay", type=int, default=5)
    add("--timeout", type=int, default=1800)
    add("--noctx-timeout", type=int, default=2100)
    add("--noctx-sleep", type=int, default=0)
    for flag in (
        "--exit-on-failure",
        "--fail-fast",
        "--no-cleanup",
        "--no-sudo",
        "--dry-run",
        "--list-tests",
        "--quiet",
    ):
        add(flag, action="store_true")
    add("--sysfs-pci", default="/sys/bus/pci/devices", help=argparse.SUPPRESS)
    add("--sysfs-hugepages", default="/sys/kernel/mm/hugepages", help=argparse.SUPPRESS)
    return ap.parse_args(argv)


def _setup_ports(
    a: argparse.Namespace,
    selected: list[dict[str, Any]],
    nic_setup: Path,
    created_pfs: list[str],
    bound_pvfs: list[str],
) -> tuple[list[str], list[str], list[str], str]:
    nics = scan_nics(Path(a.sysfs_pci))
    pvf_req, tvf_req = parse_bdfs(a.bind_pvf), parse_bdfs(a.tvf_pf)
    pf_ports: list[str] = []
    if pvf_req:
        bound_pvfs.extend(pvf_req)
        pf_ports = nic_shell(nic_setup, ["bind-pvf", *pvf_req], a.dry_run) or list(
            pvf_req
        )
    if a.pf_ports:
        pf_ports = parse_bdfs(a.pf_ports)

    created_vfs, created_sib = [], []
    if tvf_req:
        created_pfs.append(tvf_req[0])
        created_vfs = nic_shell(
            nic_setup, ["create-tvf", tvf_req[0], str(a.tvf_count)], a.dry_run
        ) or [f"{tvf_req[0].rsplit('.', 1)[0]}.{i}" for i in range(1, a.tvf_count + 1)]
        if len(tvf_req) >= 2:
            created_pfs.append(tvf_req[1])
            created_sib = nic_shell(
                nic_setup, ["create-tvf", tvf_req[1], "2"], a.dry_run
            ) or [f"{tvf_req[1].rsplit('.', 1)[0]}.{i}" for i in (1, 2)]
        nics = scan_nics(Path(a.sysfs_pci))

    topos = {p["topology"] for p in selected}
    need_vf = (
        4 if topos & {"vf_quad", "cross_pf_quad"} else (2 if "vf_pair" in topos else 0)
    )
    vf_ports: list[str] = []
    if a.ports:
        vf_ports = parse_bdfs(a.ports)
    elif created_vfs:
        vf_ports = created_vfs
    elif need_vf and len(pf_ports) >= need_vf:
        vf_ports = list(pf_ports)
    elif need_vf:
        by_pf: dict[str, list[str]] = {}
        for n in nics:
            if n["drv"] == "vfio-pci":
                by_pf.setdefault(n["pf"] or "standalone", []).append(n["bdf"])
        vf_ports = next(
            (sorted(v) for _, v in sorted(by_pf.items()) if len(v) >= need_vf), []
        )
        if len(vf_ports) < need_vf:
            up = [
                n["bdf"]
                for n in nics
                if not n["pf"] and n["drv"] == "ice" and n["state"] == "up"
            ]
            if up:
                created_pfs.append(up[0])
                vf_ports = nic_shell(
                    nic_setup, ["create-tvf", up[0], str(a.tvf_count)], a.dry_run
                ) or [
                    f"{up[0].rsplit('.', 1)[0]}.{i}" for i in range(1, a.tvf_count + 1)
                ]
                nics = scan_nics(Path(a.sysfs_pci))
        if len(vf_ports) < need_vf and not a.dry_run:
            raise HostFault(
                f"Need {need_vf} vfio-pci ports, found {len(vf_ports)}; pass --tvf-pf, --bind-pvf, or --ports"
            )

    noctx_ports = (
        [created_vfs[0], created_sib[0], created_vfs[1], created_sib[1]]
        if created_vfs and len(created_sib) >= 2
        else list(vf_ports[:4])
    )
    if vf_ports and not created_vfs and not pvf_req and not a.dry_run:
        pfs_used = list(
            dict.fromkeys(n["pf"] for n in nics if n["bdf"] in vf_ports[:2] and n["pf"])
        )
        if pfs_used:
            nic_shell(nic_setup, ["vf-rss-lut", *pfs_used], False)

    if any(p["topology"] != "kernel_lo" for p in selected) and not a.dry_run:
        hp_free = sum(
            int(_rtxt(Path(a.sysfs_hugepages) / s / "free_hugepages") or 0)
            for s in ("hugepages-2048kB", "hugepages-1048576kB")
        )
        if hp_free == 0:
            raise HostFault("No free 2 MB or 1 GB hugepages on this host")

    return vf_ports, noctx_ports, pf_ports, detect_nic(nics, vf_ports or pf_ports)


def main(argv: list[str] | None = None) -> int:
    a = _parse_args(argv)
    repo = Path(a.repo_root).resolve()
    local_bin = repo / ".local_install/mtl/bin/KahawaiTest"
    binary = (
        Path(a.binary)
        if a.binary
        else (local_bin if local_bin.is_file() else repo / "build/tests/KahawaiTest")
    )
    nic_setup, isolate_sh = (
        Path(a.nic_setup).resolve(),
        repo / "tests/tools/isolate/isolate.sh",
    )

    tiers = {"pr"} if a.tier == "pr" else {"pr", "nightly"}
    selected = [
        p
        for p in load_profiles(Path(a.profiles))
        if p["tier"] in tiers
        and (not a.group or p["group"] == a.group)
        and (not a.profile or re.search(a.profile, p["name"]))
    ]
    if not selected:
        print("Error: no profiles selected", file=sys.stderr)
        return 2
    if a.list_tests:
        for p in selected:
            n = len(list_cases(binary, a.filter or p["filter"]))
            print(f"{p['name']} ({p['tier']}, {p['topology']}): {n} cases")
        return 0

    out_dir = Path(a.out_dir).resolve()
    jsonl = out_dir / "results.jsonl"
    if not a.dry_run:
        if not binary.is_file():
            print(f"Error: KahawaiTest not found at {binary}", file=sys.stderr)
            return 1
        (out_dir / "junit").mkdir(parents=True, exist_ok=True)
        (out_dir / "logs").mkdir(parents=True, exist_ok=True)
        jsonl.write_text("", encoding="utf-8")

    created_pfs: list[str] = []
    bound_pvfs: list[str] = []

    def cleanup() -> None:
        if a.no_cleanup:
            return
        for action, items in (("restore-pvf", bound_pvfs), ("disable-vf", created_pfs)):
            if items:
                batch, items[:] = list(items), []
                try:
                    nic_shell(nic_setup, [action, *batch], a.dry_run)
                except HostFault as exc:
                    print(f"Warning: {exc}", file=sys.stderr)

    def _sig(signum: int, _f: Any) -> None:
        cleanup()
        sys.exit(128 + signum)

    prev_i, prev_t = signal.signal(signal.SIGINT, _sig), signal.signal(
        signal.SIGTERM, _sig
    )
    records: list[dict[str, Any]] = []
    host, t0, fault_msg, any_fail = socket.gethostname(), time.monotonic(), "", False
    vf_ports: list[str] = []
    noctx_ports: list[str] = []
    pf_ports: list[str] = []
    nic_label = "unknown"
    done_profiles: set[str] = set()

    def record(rec: dict[str, Any]) -> None:
        records.append(rec)
        if not a.dry_run:
            with jsonl.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(rec) + "\n")

    try:
        vf_ports, noctx_ports, pf_ports, nic_label = _setup_ports(
            a, selected, nic_setup, created_pfs, bound_pvfs
        )
        for p in selected:
            name, topo, eff_flt = p["name"], p["topology"], a.filter or p["filter"]
            penv, root = p["env"], p["needs_root"]
            ctx = {"profile": name, "topology": topo, "nic": nic_label, "host": host}
            if p["mode"] == "shared":
                xml = out_dir / "junit" / f"gtest_{name}.xml"
                base = [str(binary), f"--p_sip={a.p_sip}", "--auto_start_stop"]
                if topo == "kernel_lo":
                    base += ["--p_port", "kernel:lo", "--r_port", "kernel:lo"]
                elif topo == "vf_quad":
                    base += ["--port_list", ",".join(vf_ports[:4])]
                else:
                    base += ["--p_port", vf_ports[0], "--r_port", vf_ports[1]]
                if p["dma_auto"]:
                    base.append("--dma_auto")
                base += p["extra_args"] + (["--gtest_fail_fast"] if a.fail_fast else [])
                cmd = [
                    *_sudo(root, a.no_sudo, penv),
                    *base,
                    f"--gtest_output=xml:{xml}",
                    f"--gtest_filter={eff_flt}",
                ]
                print(f"\n==> [{name}] topology={topo} filter={eff_flt}")
                if a.dry_run:
                    print(f"[dry-run] {' '.join(cmd)}")
                    done_profiles.add(name)
                    continue
                rc, out = run_cmd(
                    cmd,
                    {**os.environ, **penv},
                    out_dir / "logs" / f"{name}.log",
                    a.timeout,
                    a.quiet,
                )
                parsed = parse_xml(xml, ctx, 1)
                if rc != 0 and not parsed:
                    _check_config(name, out)
                if not parsed and rc != 0:
                    record(
                        _mk(
                            ctx,
                            f"{name}.<process>",
                            1,
                            "failed",
                            f"rc={rc} without XML",
                        )
                    )
                    any_fail = True
                retry_env = {
                    k: v
                    for k, v in penv.items()
                    if k not in ("GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX")
                }
                for r in parsed:
                    final = (
                        _run_case(
                            a,
                            ctx,
                            base,
                            r["case"],
                            root,
                            retry_env,
                            a.timeout,
                            2,
                            out_dir,
                            isolate_sh,
                            r["message"],
                        )
                        if (r["result"] == "failed" and a.retries >= 2)
                        else r
                    )
                    record(final)
                    any_fail = any_fail or final["result"] == "failed"
            else:
                ports = pf_ports[:2] if topo == "pf_pair" else noctx_ports[:4]
                need = 2 if topo == "pf_pair" else 4
                if len(ports) < need:
                    if name == "noctx_pf":
                        print(f"\n==> [{name}] skipped (pass --bind-pvf or --pf-ports)")
                        record(_mk(ctx, f"{name}.*", 0, "skipped", "no PF ports"))
                        done_profiles.add(name)
                        continue
                    raise HostFault(f"{name} needs {need} ports, got {len(ports)}")
                cases = list_cases(binary, eff_flt)
                meta_map = list_case_metadata(binary)
                base = [
                    str(binary),
                    "--auto_start_stop",
                    f"--p_sip={a.p_sip}",
                    f"--port_list={','.join(ports)}",
                ]
                print(
                    f"\n==> [{name}] {len(cases)} per-process cases on {','.join(ports)}"
                )
                for idx, case in enumerate(cases, 1):
                    iso = (
                        bool(meta_map.get(case, {}).get("isolated"))
                        and isolate_sh.is_file()
                    )
                    cenv = dict(penv)
                    if iso:
                        cenv["MTL_ISOLATE_PORTS"] = ",".join(ports[0::2])
                        cenv["MTL_ISOLATE_MEM_PORTS"] = ",".join(ports[1::2])
                    if a.dry_run:
                        dcmd = [
                            *_sudo(root, a.no_sudo, cenv),
                            *base,
                            f"--gtest_filter={case}",
                        ]
                        print(f"[dry-run] ({idx}/{len(cases)}) {' '.join(dcmd)}")
                        continue
                    final_c = _run_case(
                        a,
                        ctx,
                        base,
                        case,
                        root,
                        cenv,
                        a.noctx_timeout,
                        1,
                        out_dir,
                        isolate_sh,
                        iso=iso,
                    )
                    record(final_c)
                    if final_c["result"] == "failed":
                        any_fail = True
                        if a.exit_on_failure:
                            break
                    if a.noctx_sleep > 0 and idx < len(cases):
                        time.sleep(a.noctx_sleep)
            done_profiles.add(name)
            if any_fail and a.exit_on_failure:
                break
    except HostFault as exc:
        fault_msg = str(exc)
        for p in selected:
            if p["name"] not in done_profiles:
                ctx = {
                    "profile": p["name"],
                    "topology": p["topology"],
                    "nic": nic_label,
                    "host": host,
                }
                record(
                    _mk(ctx, f"{p['name']}.*", 0, "not_run", f"host_fault: {fault_msg}")
                )
    finally:
        cleanup()
        signal.signal(signal.SIGINT, prev_i)
        signal.signal(signal.SIGTERM, prev_t)

    counts = {
        k: sum(1 for r in records if r["result"] == k)
        for k in ("passed", "failed", "flaky", "skipped", "not_run")
    }
    summary = {
        "host": host,
        "nic": nic_label,
        "tier": a.tier,
        "ports": {"vf": vf_ports, "noctx": noctx_ports, "pf": pf_ports},
        "counts": counts,
        "host_fault": fault_msg,
        "elapsed_s": round(time.monotonic() - t0, 1),
        "results_jsonl": str(jsonl),
    }
    if not a.dry_run:
        (out_dir / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8"
        )
        if a.log_file:
            lp = Path(a.log_file)
            lp.parent.mkdir(parents=True, exist_ok=True)
            lp.write_text(
                jsonl.read_text(encoding="utf-8")
                + f"=== GTEST SUMMARY ({host}/{nic_label}): {counts} elapsed={summary['elapsed_s']}s ===\n",
                encoding="utf-8",
            )
    print(
        f"\n=== GTEST SUMMARY ({host}/{nic_label}): {counts} elapsed={summary['elapsed_s']}s ==="
    )
    return HOST_FAULT_EXIT if fault_msg else (1 if any_fail else 0)


if __name__ == "__main__":
    sys.exit(main())
