#!/usr/bin/env python3
"""Root-only, run-scoped resctrl helper for mxl-k8s-qos-lab.

Installed as /usr/local/sbin/mxl-rdt-host. Inputs are base64 JSON so the sudo
rule exposes one tightly validated executable rather than arbitrary shell.
"""
from __future__ import annotations

import argparse
import base64
import csv
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

RESCTRL = Path("/sys/fs/resctrl")
STATE = Path("/run/mxl-rdt")
DATA = Path("/tmp/mxl-rdt")
# Bump whenever payload handling or profile set changes; controller checks this.
HELPER_VERSION = 8
CAT_PARTITION_FRACTION = {"cat-guarded": 0.75, "cat-strong": 0.875}
CAT_SHARED_PROFILES = {"cat-16-1"}

MBA_LEVELS = ("80", "60", "40", "20", "10")
VALID_PROFILE = (
    {"none"}
    | set(CAT_SHARED_PROFILES)
    | set(CAT_PARTITION_FRACTION)
    | {f"mba-{level}" for level in MBA_LEVELS}
    | {f"{cat}+mba-{level}" for cat in (set(CAT_PARTITION_FRACTION) | CAT_SHARED_PROFILES) for level in MBA_LEVELS}
)


def fail(message: str) -> None:
    raise RuntimeError(message)


def payload(value: str) -> dict:
    if len(value) > 65536:
        fail("RDT payload exceeds 64 KiB limit")
    try:
        data = json.loads(base64.urlsafe_b64decode(value.encode()).decode())
    except Exception as error:
        raise RuntimeError("invalid RDT payload") from error
    if not isinstance(data, dict):
        fail("RDT payload must be an object")
    return data


def mounted() -> bool:
    return any(line.split()[2:3] == ["resctrl"] for line in Path("/proc/mounts").read_text().splitlines())


def ensure_mounted() -> None:
    if not mounted():
        RESCTRL.mkdir(parents=True, exist_ok=True)
        subprocess.run(["mount", "-t", "resctrl", "resctrl", str(RESCTRL)], check=True)
    if not (RESCTRL / "info").is_dir():
        fail("resctrl mounted without capability information")


def read(path: Path, default: str = "") -> str:
    try:
        return path.read_text().strip()
    except OSError:
        return default


def capabilities() -> dict:
    ensure_mounted()
    mount_line = next(
        (line for line in Path("/proc/mounts").read_text().splitlines() if line.split()[2:3] == ["resctrl"]),
        "",
    )
    info: dict[str, dict[str, str]] = {}
    for feature in ("L3", "L2", "MB", "L3_MON"):
        directory = RESCTRL / "info" / feature
        if directory.is_dir():
            info[feature] = {p.name: read(p) for p in sorted(directory.iterdir()) if p.is_file()}
    return {
        "mounted": True,
        "helper_version": HELPER_VERSION,
        "supported_profiles": sorted(VALID_PROFILE),
        "mount": mount_line,
        "info": info,
        "schemata": read(RESCTRL / "schemata"),
        "mode": read(RESCTRL / "mode"),
    }


def descendants(root: int) -> set[int]:
    found = {root}
    changed = True
    while changed:
        changed = False
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit() or int(proc.name) in found:
                continue
            try:
                fields = (proc / "stat").read_text().split()
                if int(fields[3]) in found:
                    found.add(int(proc.name)); changed = True
            except (OSError, ValueError, IndexError):
                pass
    return found


def task_ids(pids: list[int]) -> list[int]:
    tids: set[int] = set()
    for pid in pids:
        for task in Path(f"/proc/{pid}/task").glob("[0-9]*"):
            if task.name.isdigit():
                tids.add(int(task.name))
    return sorted(tids)


def comm_matches(name: str, comm: str) -> bool:
    # stress-ng renames workers to "stress-ng-<stressor>"; the parent alone moves no traffic.
    return name == comm or name.startswith(comm + "-")


def matching_pids(pod_uids: list[str], comm: str) -> list[int]:
    needles = {uid.lower().replace("-", "") for uid in pod_uids if re.fullmatch(r"[0-9a-fA-F-]{16,64}", uid)}
    result: set[int] = set()
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cgroup = (proc / "cgroup").read_text().lower().replace("-", "").replace("_", "")
            name = (proc / "comm").read_text().strip()
            if comm_matches(name, comm) and any(uid in cgroup for uid in needles):
                result.add(int(proc.name))
        except OSError:
            pass
    return sorted(result)


def resolve_groups(data: dict) -> dict[str, list[int]]:
    groups = data.get("groups", {})
    if not isinstance(groups, dict) or not groups or len(groups) > 6:
        fail("RDT groups missing")
    resolved: dict[str, list[int]] = {}
    for name, spec in groups.items():
        if not re.fullmatch(r"[a-z][a-z0-9-]{0,30}", name) or not isinstance(spec, dict):
            fail("invalid RDT group")
        kind = spec.get("kind")
        if kind == "pods":
            comm = spec.get("comm")
            if comm not in {"ffmpeg", "stress-ng"}:
                fail("invalid monitored command")
            pod_uids = spec.get("pod_uids", [])
            if not isinstance(pod_uids, list) or len(pod_uids) > 256:
                fail("invalid Pod UID list")
            pids = matching_pids(pod_uids, comm)
        elif kind == "host-noise":
            try:
                supervisor = int(read(Path("/tmp/mxl-host-noise/supervisor.pid"), "0"))
            except ValueError:
                supervisor = 0
            pids = sorted(
                pid for pid in descendants(supervisor)
                if supervisor > 1 and comm_matches(read(Path(f"/proc/{pid}/comm")), "stress-ng")
            )
        else:
            fail("invalid RDT group kind")
        pids = task_ids(pids)
        if not pids:
            fail(f"no host PIDs resolved for RDT group {name}")
        resolved[name] = pids
    return resolved


def write_tasks(directory: Path, pids: list[int]) -> None:
    for pid in pids:
        try:
            (directory / "tasks").write_text(f"{pid}\n")
        except FileNotFoundError:
            fail(f"process disappeared during RDT association: {pid}")


def l3_masks(fraction: float) -> tuple[str, str]:
    info = RESCTRL / "info/L3"
    full = int(read(info / "cbm_mask"), 16)
    bits = [bit for bit in range(full.bit_length()) if full & (1 << bit)]
    minimum = int(read(info / "min_cbm_bits", "1"))
    noise_count = max(minimum, round(len(bits) * (1.0 - fraction)))
    if noise_count >= len(bits):
        fail("insufficient LLC ways for requested CAT split")
    noise_bits = bits[:noise_count]
    workload_bits = bits[noise_count:]
    if len(workload_bits) < minimum:
        fail("workload CAT mask violates minimum CBM bits")
    noise = sum(1 << bit for bit in noise_bits)
    workload = sum(1 << bit for bit in workload_bits)
    return f"{workload:x}", f"{noise:x}"


def l3_masks_shared_one_way() -> tuple[str, str]:
    info = RESCTRL / "info/L3"
    full = int(read(info / "cbm_mask"), 16)
    bits = [bit for bit in range(full.bit_length()) if full & (1 << bit)]
    if not bits:
        fail("resctrl L3 cbm_mask has no allocatable ways")
    minimum = int(read(info / "min_cbm_bits", "1"))
    if minimum > 1:
        fail("cat-16-1 requires min_cbm_bits <= 1")
    noise = 1 << bits[0]
    workload = full
    return f"{workload:x}", f"{noise:x}"




def resource_ids(schemata: str, resource: str) -> list[str]:
    # Kernel indents schemata lines, so compare on stripped text.
    for line in schemata.splitlines():
        stripped = line.strip()
        if stripped.startswith(resource + ":"):
            return [part.split("=", 1)[0] for part in stripped.split(":", 1)[1].split(";") if "=" in part]
    return []


def schemata_line(resource: str, value: str, root_schemata: str) -> str:
    ids = resource_ids(root_schemata, resource)
    if not ids:
        available = sorted({line.strip().split(":", 1)[0] for line in root_schemata.splitlines() if ":" in line})
        fail(f"resctrl resource unavailable: {resource} (available: {', '.join(available) or 'none'})")
    return resource + ":" + ";".join(f"{domain}={value}" for domain in ids)


def make_groups(resolved: dict[str, list[int]], profile: str) -> dict[str, Path]:
    root_schemata = read(RESCTRL / "schemata")
    paths: dict[str, Path] = {}
    control = profile != "none"
    # Every group except "noise" is a protected class and keeps the workload allocation.
    if control and ("noise" not in resolved or set(resolved) == {"noise"}):
        fail("RDT control requires a noise group and at least one protected group")
    for name, pids in resolved.items():
        if control:
            directory = RESCTRL / f"mxl-{name}"
            directory.mkdir()
            lines = [line.strip() for line in root_schemata.splitlines() if line.strip()]
            # Combined profiles set both resources in the same control group.
            for part in profile.split("+"):
                if part in CAT_PARTITION_FRACTION:
                    workload, noise = l3_masks(CAT_PARTITION_FRACTION[part])
                    replacement = schemata_line("L3", noise if name == "noise" else workload, root_schemata)
                    lines = [replacement if line.startswith("L3:") else line for line in lines]
                elif part in CAT_SHARED_PROFILES:
                    workload, noise = l3_masks_shared_one_way()    
                    replacement = schemata_line("L3", noise if name == "noise" else workload, root_schemata)
                    lines = [replacement if line.startswith("L3:") else line for line in lines]
                elif part.startswith("mba-"):
                    mba = part.rsplit("-", 1)[1] if name == "noise" else "100"
                    replacement = schemata_line("MB", mba, root_schemata)
                    lines = [replacement if line.startswith("MB:") else line for line in lines]
                else:
                    fail(f"unsupported RDT control component: {part}")
            (directory / "schemata").write_text("\n".join(lines) + "\n")
            write_tasks(directory, pids)
            paths[name] = directory
        else:
            directory = RESCTRL / "mon_groups" / f"mxl-{name}"
            directory.mkdir(parents=True)
            write_tasks(directory, pids)
            paths[name] = directory
    return paths


def counter(path: Path) -> int | str:
    # resctrl reports "Unavailable" when no RMID data exists; empty keeps the row skippable.
    raw = read(path, "")
    return int(raw) if raw.isdigit() else ""


def sample(paths: dict[str, Path], output: Path, delay: int, interval: int, duration: int) -> None:
    time.sleep(delay)
    fields = ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"]
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader()
        deadline = time.monotonic() + duration
        while True:
            now = time.time()
            for name, directory in paths.items():
                for domain in sorted((directory / "mon_data").glob("mon_L3_*")):
                    writer.writerow({
                        "timestamp": f"{now:.6f}", "group": name, "domain": domain.name,
                        "llc_occupancy_bytes": counter(domain / "llc_occupancy"),
                        "mbm_local_bytes": counter(domain / "mbm_local_bytes"),
                        "mbm_total_bytes": counter(domain / "mbm_total_bytes"),
                    })
            handle.flush()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(interval, remaining))


def cleanup() -> None:
    # Moving tasks to root restores default class/monitoring before group removal.
    for directory in sorted(RESCTRL.glob("mxl-*"), reverse=True):
        if directory.is_dir():
            for pid in read(directory / "tasks").splitlines():
                if pid.isdigit() and Path(f"/proc/{pid}").exists():
                    try: (RESCTRL / "tasks").write_text(pid + "\n")
                    except OSError: pass
            try: directory.rmdir()
            except OSError: pass
    mon_root = RESCTRL / "mon_groups"
    for directory in sorted(mon_root.glob("mxl-*"), reverse=True) if mon_root.is_dir() else []:
        if directory.is_dir():
            for pid in read(directory / "tasks").splitlines():
                if pid.isdigit() and Path(f"/proc/{pid}").exists():
                    try: (RESCTRL / "tasks").write_text(pid + "\n")
                    except OSError: pass
            try: directory.rmdir()
            except OSError: pass


def start(data: dict) -> dict:
    ensure_mounted()
    profile = data.get("profile", "none")
    if profile not in VALID_PROFILE:
        fail("unsupported RDT control profile")
    delay = int(data.get("delay", 0)); duration = int(data.get("duration", 0)); interval = int(data.get("interval", 1))
    if not 0 <= delay <= 3600 or not 1 <= duration <= 86400 or not 1 <= interval <= 60:
        fail("invalid RDT sampling timing")
    STATE.mkdir(mode=0o755, parents=True, exist_ok=True)
    DATA.mkdir(mode=0o755, parents=True, exist_ok=True)
    for path in DATA.glob("*"):
        if path.is_file():
            path.unlink()
    pid_file = STATE / "sampler.pid"
    old = int(read(pid_file, "0") or 0)
    if old > 1 and Path(f"/proc/{old}").exists():
        fail(f"RDT sampler already running: {old}")
    cleanup()
    before = capabilities()
    (DATA / "capabilities.json").write_text(json.dumps(before, indent=2, sort_keys=True) + "\n")
    try:
        resolved = resolve_groups(data)
        paths = make_groups(resolved, profile)
    except Exception:
        cleanup()
        raise
    mapping = {name: {"task_ids": pids, "path": str(paths[name])} for name, pids in resolved.items()}
    (DATA / "pid-map.json").write_text(json.dumps(mapping, indent=2, sort_keys=True) + "\n")
    output = DATA / "samples.csv"
    pid = os.fork()
    if pid == 0:
        os.setsid()
        devnull = os.open(os.devnull, os.O_RDWR)
        for fd in (0, 1, 2):
            os.dup2(devnull, fd)
        if devnull > 2:
            os.close(devnull)
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        try:
            sample(paths, output, delay, interval, duration)
        except Exception as error:
            (DATA / "sampler-error.txt").write_text(str(error) + "\n")
        os._exit(0)
    pid_file.write_text(str(pid) + "\n")
    uid = int(os.environ.get("SUDO_UID", "0")); gid = int(os.environ.get("SUDO_GID", "0"))
    for path in (DATA, DATA / "capabilities.json", DATA / "pid-map.json"):
        os.chown(path, uid, gid)
    return {"sampler_pid": pid, "profile": profile, "groups": mapping}


def stop() -> dict:
    ensure_mounted()
    pid = int(read(STATE / "sampler.pid", "0") or 0)
    if pid > 1 and Path(f"/proc/{pid}").exists():
        try: os.kill(pid, signal.SIGTERM)
        except ProcessLookupError: pass
        for _ in range(50):
            if not Path(f"/proc/{pid}").exists(): break
            time.sleep(0.1)
        if Path(f"/proc/{pid}").exists():
            try: os.kill(pid, signal.SIGKILL)
            except ProcessLookupError: pass
    cleanup()
    restored = not any(RESCTRL.glob("mxl-*")) and not any((RESCTRL / "mon_groups").glob("mxl-*"))
    (DATA / "after.json").write_text(json.dumps(capabilities(), indent=2, sort_keys=True) + "\n")
    (STATE / "sampler.pid").unlink(missing_ok=True)
    uid = int(os.environ.get("SUDO_UID", "0")); gid = int(os.environ.get("SUDO_GID", "0"))
    for path in DATA.glob("*"):
        try: os.chown(path, uid, gid)
        except OSError: pass
    return {"stopped": True, "restored": restored, "sampler_error": read(DATA / "sampler-error.txt")}


def main() -> int:
    if os.geteuid() != 0:
        fail("mxl-rdt-host must run as root")
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["capabilities", "start", "stop"])
    parser.add_argument("payload", nargs="?", default="")
    args = parser.parse_args()
    result = capabilities() if args.command == "capabilities" else start(payload(args.payload)) if args.command == "start" else stop()
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"FATAL: {error}", file=sys.stderr)
        raise SystemExit(2)
