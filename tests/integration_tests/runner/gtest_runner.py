#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Pure-stdlib KahawaiTest integration runner."""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import signal
import socket
import subprocess  # nosec B404 # argv lists only, no shell
import sys
import tempfile
import time
import xml.etree.ElementTree as ET  # nosec B405 # parses local JUnit XML
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
_PF = "St20p.digest*:St20p.transport_*:St20p.tx_*no_epoch_drop"
_KL = (
    "St20p.tx_create_free_mix:St20p.rx_create_free_mix:St20p.digest_1080p_s1:"
    "St20p.digest_1080i_s2:St20p.put_frame_abort:St20p.query_params:"
    "St20p.session_stats_reset:St20p.update_source_and_destination"
)
_F20 = "St20_rx*:St20_tx*-St20_tx.create_queue_cnt"
_F34 = "St3*:St4*-St30p.redundant*:St40p.redundant*"
_FRD = "St20p.redundant*:St30p.redundant*:St40p.redundant*"
_P0 = "--rss_mode l3_l4 --pacing_way"
_P_TSC = f"{_P0} tsc --iova_mode va --multi_src_port"
_P_PA = f"{_P0} auto --iova_mode pa --multi_src_port"
_P_VA = f"{_P0} auto --iova_mode va --multi_src_port"

# Run TSC before RL init: ice PF keeps VF queue rate limits on DPDK < 26.07.91.
GROUPS = {
    "pr_core": "st2110_20 st2110_20p st2110_20_tx_queue_cnt st2110_22 st2110_3x_4x",
    "core_misc": "core_misc redundant_stats st20p_kernel_loopback",
    "pacing_dma": "st20p_tsc_pacing dma_cvt_va st20p_auto_pacing_pa st20p_auto_pacing_va",
    "noctx": "noctx noctx_pf",
}
# (name, filter, topo, extra_args_str, flags)
_FPA, _FPV = f"{_PF}:Dma.iova_pa*-St20p.*ext*", f"{_PF}-St20p.*ext*"
_Q24, _VA = "--queue_cnt 24", "--iova_mode va"
RUNS = (
    ("st2110_20", _F20, "vf_pair", "", "pr,dma"),
    ("st2110_20p", "St20p*-St20p.redundant*", "vf_pair", "", "pr"),
    ("st2110_20_tx_queue_cnt", "St20_tx.create_queue_cnt", "vf_pair", _Q24, "pr"),
    ("st2110_22", "St22*", "vf_pair", "", "pr"),
    ("st2110_3x_4x", _F34, "vf_pair", "", "pr"),
    ("core_misc", "Main*:Misc*:Sch*:TxUserTimestampSync*", "vf_pair", "", ""),
    ("redundant_stats", _FRD, "vf_quad", "", ""),
    ("st20p_kernel_loopback", _KL, "kernel_lo", "", "noroot"),
    ("st20p_tsc_pacing", _FPV, "vf_pair", _P_TSC, ""),
    ("dma_cvt_va", "Dma*:Cvt*-Dma.iova_pa*", "vf_pair", _VA, "dma"),
    ("st20p_auto_pacing_pa", _FPA, "vf_pair", _P_PA, ""),
    ("st20p_auto_pacing_va", _FPV, "vf_pair", _P_VA, ""),
    ("noctx", "NoCtxTest.*-NoCtxTest.*_pf_*", "cross_pf_quad", "", "noctx,env"),
    ("noctx_pf", "NoCtxTest.*_pf_*", "pf_pair", "", "noctx"),
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
        by_pf: dict[str, list[str]] = {}
        for bdf, (drv, pf, _) in sorted(by.items()):
            if drv == "vfio-pci":
                by_pf.setdefault(pf or "_", []).append(bdf)
        found = next((v for _, v in sorted(by_pf.items()) if len(v) >= need), [])
        vf_p = pvf if len(pvf) >= need else found
        if len(vf_p) < need and up_ice:
            created.append(up_ice[0])
            _nicctl("create_tvf", up_ice[0], "6")
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
    cmd: list[str], env: dict[str, str], to_s: int, tmp: Path, log_fh
) -> tuple[int, str]:
    with tmp.open("w+", encoding="utf-8", errors="replace") as fh:
        proc = subprocess.Popen(
            cmd, stdout=fh, stderr=subprocess.STDOUT, env=env, start_new_session=True
        )
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
        fh.flush()
        fh.seek(0)
        out = fh.read()
    if out:
        sys.stdout.write(out)
        log_fh.write(out)
        log_fh.flush()
    return rc, out


def _parse_xml(xml: Path, ctx: dict[str, str]) -> list[dict]:
    if not xml.is_file():
        return []
    out = []
    for tc in ET.parse(xml).getroot().iter("testcase"):  # nosec B314
        cls, name = tc.attrib.get("classname", ""), tc.attrib.get("name", "")
        fail = tc.find("failure")
        fail = tc.find("error") if fail is None else fail
        skip = tc.find("skipped")
        if fail is not None:
            res = "failed"
            msg = (fail.attrib.get("message") or fail.text or "").strip()[:400]
        elif skip is not None or tc.attrib.get("result") == "skipped":
            res = "skipped"
            msg = (skip.attrib.get("message") if skip is not None else "") or ""
        else:
            res, msg = "passed", ""
        ms = round(float(tc.attrib.get("time", "0") or 0) * 1000, 1)
        case = f"{cls}.{name}" if cls else name
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
            cases.append(f"{suite}.{ln.strip()}")
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
        "--exit-on-failure",
        action="store_true",
        default=e.get("EXIT_ON_FAILURE") == "1",
    )
    add("--binary", default=e.get("KAHAWAI_TEST_BINARY") or None)
    add("--p-sip", default=e.get("TEST_P_SIP") or f"192.168.{secrets.randbelow(256)}.1")
    add("--log-file", default=e.get("LOG_FILE") or None)
    a = ap.parse_args(argv)

    loc = REPO / ".local_install/mtl/bin/KahawaiTest"
    binary = (
        Path(a.binary)
        if a.binary
        else (loc if loc.is_file() else REPO / "build/tests/KahawaiTest")
    )
    allowed = set(GROUPS[a.group].split()) if a.group else None
    selected = [
        r
        for r in RUNS
        if (a.tier == "nightly" or "pr" in r[4].split(","))
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

    def cleanup() -> None:
        for sub, items in (("bind_kernel", bound), ("disable_vf", created)):
            batch, items[:] = list(items), []
            for bdf in batch:
                try:
                    _nicctl(sub, bdf)
                except HostFault as exc:
                    print(f"Warning: {exc}", file=sys.stderr)

    def _sig(signum: int, _f) -> None:
        cleanup()
        sys.exit(128 + signum)

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
        records.append(rec)
        with jsonl.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec) + "\n")

    with log_p.open("w", encoding="utf-8", errors="replace") as log_fh:

        def tee(msg: str) -> None:
            sys.stdout.write(msg)
            log_fh.write(msg)

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
                if att > 1 and delay > 0:
                    time.sleep(delay)
                xml = out / "junit" / f"{ctx['profile']}_{safe}_try{att}.xml"
                iso_p = [str(ISOLATE), "--"] if iso else []
                fa, xa = f"--gtest_filter={case}", f"--gtest_output=xml:{xml}"
                cmd = [*_sudo(root, extra), *iso_p, *base, fa, xa]
                if att > 1:
                    tee(f"  retrying {case} ({att}/{a.retries})\n")
                if iso:
                    subprocess.run(sweep, capture_output=True)
                rc, txt = _run_cmd(cmd, env, to_s, out / "last.log", log_fh)
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
                if cur["result"] in ("passed", "skipped"):
                    if att > 1 and cur["result"] == "passed":
                        ms = f"Passed on retry {att}" + (
                            f"; first: {first}" if first else ""
                        )
                        return {**cur, "result": "flaky", "message": ms}
                    return cur
                fin = cur
            return fin

        try:
            vf_p, noctx_p, pf_p, nic = _setup_ports(a.ports, selected, created, bound)
            for name, flt, topo, extra_s, flag_s in selected:
                flags = set(flag_s.split(",")) if flag_s else set()
                penv = dict(NOCTX_ENV) if "env" in flags else {}
                root, ctx = "noroot" not in flags, {
                    "profile": name,
                    "nic": nic,
                    "host": host,
                }
                if "noctx" not in flags:
                    xml = out / "junit" / f"gtest_{name}.xml"
                    if topo == "kernel_lo":
                        pargs = ["--p_port", "kernel:lo", "--r_port", "kernel:lo"]
                    elif topo == "vf_quad":
                        pargs = ["--port_list", ",".join(vf_p[:4])]
                    else:
                        pargs = ["--p_port", vf_p[0], "--r_port", vf_p[1]]
                    base = [str(binary), f"--p_sip={a.p_sip}", "--auto_start_stop"]
                    base += [*pargs, *extra_s.split()]
                    if "dma" in flags:
                        base.append("--dma_auto")
                    if a.tier == "pr":
                        base.append("--gtest_fail_fast")
                    xa, fa = f"--gtest_output=xml:{xml}", f"--gtest_filter={flt}"
                    cmd = [*_sudo(root, penv), *base, xa, fa]
                    tee(f"\n==> [{name}] topology={topo} filter={flt}\n")
                    rc, txt = _run_cmd(
                        cmd, {**e, **penv}, a.timeout, out / "last.log", log_fh
                    )
                    parsed = _parse_xml(xml, ctx)
                    if rc != 0 and not parsed:
                        if m := CFG_ERR.search(txt):
                            raise HostFault(f"Config error in {name}: {m.group(0)}")
                        emit(
                            ctx,
                            f"{name}.<process>",
                            "failed",
                            msg=f"rc={rc} without XML",
                        )
                        failed = True
                    for r in parsed:
                        if r["result"] == "failed" and a.retries >= 2:
                            r = run_case(
                                ctx,
                                base,
                                r["case"],
                                root,
                                penv,
                                a.timeout,
                                2,
                                r["message"],
                            )
                        emit(
                            ctx, r["case"], r["result"], r["duration_ms"], r["message"]
                        )
                        failed = failed or r["result"] == "failed"
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
                    base = [
                        str(binary),
                        "--auto_start_stop",
                        f"--p_sip={a.p_sip}",
                        f"--port_list={pl}",
                    ]
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
                if failed and a.exit_on_failure:
                    break
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
    return 3 if fault else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
