from __future__ import annotations

import shlex
import time
from pathlib import Path
from typing import Any

from .common import ROOT, compact_cpus, read_env, run
from .render import cpu_pools

REMOTE_DIR = "/tmp/mxl-host-noise"
PID_FILE = f"{REMOTE_DIR}/supervisor.pid"
LOG_FILE = f"{REMOTE_DIR}/stress-ng.log"


def is_host_noise(cfg: dict[str, str]) -> bool:
    return cfg.get("NOISY_NEIGHBOR_ENABLED") == "1" and cfg.get("NOISY_NEIGHBOR_SCOPE", "pod") == "host"


def ssh_target(cfg: dict[str, str]) -> str:
    inventory = read_env(ROOT / "config/nodes.env")
    node = cfg.get("NODE", cfg["LAB_DEFAULT_NODE"])
    address_key = node.upper().replace("-", "_") + "_HOST"
    address = inventory.get(address_key)
    if not address:
        raise ValueError(f"host noisy-neighbor has no SSH address for node {node} ({address_key})")
    user = inventory.get("LAB_SSH_USER")
    if not user:
        raise ValueError("host noisy-neighbor needs LAB_SSH_USER in config/nodes.env")
    return f"{user}@{address}"


def host_noise_plan(cfg: dict[str, str]) -> dict[str, Any]:
    if cfg.get("NOISY_NEIGHBOR_SCOPE", "pod") != "host":
        raise ValueError("host noisy-neighbor requires NOISY_NEIGHBOR_SCOPE=host")
    stressor = cfg.get("NOISY_NEIGHBOR_HOST_STRESSOR", "cache")
    if stressor not in {"cache", "bandwidth"}:
        raise ValueError(f"unsupported host noisy-neighbor stressor: {stressor}")
    try:
        workers = int(cfg["NOISY_NEIGHBOR_HOST_WORKERS_PER_SOCKET"])
    except (KeyError, ValueError) as error:
        raise ValueError("NOISY_NEIGHBOR_HOST_WORKERS_PER_SOCKET must be an integer") from error
    pools = cpu_pools(cfg)
    if workers < 1 or workers > min(map(len, pools)):
        raise ValueError(
            f"host {stressor} workers per socket must be between 1 and {min(map(len, pools))}: {workers}"
        )
    extra = shlex.split(cfg.get("NOISY_NEIGHBOR_HOST_EXTRA_ARGS", "--metrics-brief"))
    forbidden = {
        "--all", "-a", "--sequential", "--seq", "--permute", "--class",
        "--taskset", "--timeout", "--cache", "--cache-ops",
        "--cache-level", "--cache-size", "--cache-ways", "--cache-no-affinity",
        "--stream", "--stream-ops", "--stream-index", "--stream-l3-size",
    }
    if forbidden.intersection(arg.split("=", 1)[0] for arg in extra):
        raise ValueError("host noisy-neighbor extra arguments contain lifecycle, affinity, or broad stressor options")
    commands = []
    for socket_id, cpus in enumerate(pools):
        prefix = ["taskset", "-c", compact_cpus(cpus)]
        if stressor == "cache":
            stress_args = [
                "--cache", str(workers), "--cache-level", "3", "--cache-size", "32M",
                "--cache-ways", "16", "--cache-no-affinity",
            ]
        else:
            prefix.extend(["numactl", f"--membind={socket_id}"])
            stress_args = [
                "--stream", str(workers), "--stream-l3-size", "32M", "--stream-index", "0",
            ]
        argv = [*prefix, "stress-ng", *stress_args, *extra, "--timeout", "0"]
        commands.append({"socket": socket_id, "numa_node": socket_id, "cpus": cpus, "argv": argv})
    return {
        "scope": "host",
        "stressor": stressor,
        "workers_per_socket": workers,
        "total_workers": workers * len(pools),
        "commands": commands,
    }


def _ssh(cfg: dict[str, str], script: str, *, check: bool = True) -> str:
    target = ssh_target(cfg)
    try:
        return run([
            "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            target, script,
        ], check=check)
    except RuntimeError as error:
        if "Permission denied (publickey)" in str(error):
            raise RuntimeError(
                f"host noisy-neighbor cannot authenticate from controller to {target}; "
                "authorize controller ~/.ssh/id_ed25519.pub on worker or run "
                "scripts/setup-controller-worker-ssh.sh from development host"
            ) from error
        raise


def host_noise_snapshot(cfg: dict[str, str]) -> dict[str, Any]:
    script = f"""
set -eu
pid=$(cat {shlex.quote(PID_FILE)} 2>/dev/null || true)
[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || exit 3
printf '%s\n' '=== version ==='
stress-ng --version
printf '%s\n' '=== supervisor ==='
printf 'pid=%s\n' "$pid"
count=$(ps -o comm= --sid "$pid" | awk '$1 ~ /^stress-ng/ {{n++}} END {{print n+0}}')
printf 'stress_process_count=%s\n' "$count"
printf '%s\n' '=== processes ==='
ps -o pid,ppid,sid,psr,pcpu,time,stat,comm,args --sid "$pid"
printf '%s\n' '=== process-status ==='
for process in $(ps -o pid= --sid "$pid"); do
  printf '%s\n' "--- pid=$process ---"
  grep -E '^(Name|Pid|PPid|Cpus_allowed_list|Mems_allowed_list|voluntary_ctxt_switches|nonvoluntary_ctxt_switches):' "/proc/$process/status" 2>/dev/null || true
  awk '{{print "utime_ticks=" $14 " stime_ticks=" $15 " processor=" $39}}' "/proc/$process/stat" 2>/dev/null || true
    printf 'numa_policy_tokens='
    awk '{{for (i = 2; i <= NF; i++) if ($i ~ /^(bind|interleave|prefer):/) print $i}}' "/proc/$process/numa_maps" 2>/dev/null | sort -u | tr '\n' ','
    printf '\n'
    command -v numastat >/dev/null && numastat -p "$process" 2>/dev/null || true
done
printf '%s\n' '=== log-tail ==='
tail -n 80 {shlex.quote(LOG_FILE)} 2>/dev/null || true
""".strip()
    raw = _ssh(cfg, script)
    process_count = 0
    for line in raw.splitlines():
        if line.startswith("stress_process_count="):
            process_count = int(line.partition("=")[2])
            break
    return {
        "phase": "Running",
        "scope": "host",
        "node": cfg.get("NODE", cfg["LAB_DEFAULT_NODE"]),
        "ssh_target": ssh_target(cfg),
        "stress_process_count": process_count,
        "runtime_evidence": raw,
    }


def start_host_noise(cfg: dict[str, str]) -> dict[str, Any]:
    plan = host_noise_plan(cfg)
    command_strings = [shlex.join(item["argv"]) for item in plan["commands"]]
    supervisor = f"""
p0=''; p1=''
cleanup() {{
  trap - TERM INT EXIT
  [ -z "$p0" ] || kill "$p0" 2>/dev/null || true
  [ -z "$p1" ] || kill "$p1" 2>/dev/null || true
  [ -z "$p0" ] || wait "$p0" 2>/dev/null || true
  [ -z "$p1" ] || wait "$p1" 2>/dev/null || true
}}
trap 'cleanup; exit 0' TERM INT
trap cleanup EXIT
{command_strings[0]} & p0=$!
{command_strings[1]} & p1=$!
while kill -0 "$p0" 2>/dev/null && kill -0 "$p1" 2>/dev/null; do sleep 1; done
exit 1
""".strip()
    launch = f"nohup setsid sh -c {shlex.quote(supervisor)} >{shlex.quote(LOG_FILE)} 2>&1 </dev/null & pid=$!; echo \"$pid\" >{shlex.quote(PID_FILE)}"
    script = f"""
set -eu
command -v stress-ng >/dev/null || {{ echo 'stress-ng missing; run scripts/install-host-noise.sh on worker as root' >&2; exit 4; }}
command -v taskset >/dev/null
command -v setsid >/dev/null
{'command -v numactl >/dev/null' if plan['stressor'] == 'bandwidth' else ''}
mkdir -p {shlex.quote(REMOTE_DIR)}
old=$(cat {shlex.quote(PID_FILE)} 2>/dev/null || true)
if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
  echo "host noisy-neighbor already running: pid=$old" >&2
  exit 5
fi
rm -f {shlex.quote(PID_FILE)} {shlex.quote(LOG_FILE)}
{launch}
printf '%s\n' "$pid"
""".strip()
    _ssh(cfg, script)
    deadline = time.monotonic() + 15
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            snapshot = host_noise_snapshot(cfg)
            if snapshot["stress_process_count"] >= 2:
                return snapshot
        except (OSError, RuntimeError) as error:
            last_error = error
        time.sleep(0.5)
    stop_host_noise(cfg)
    raise RuntimeError(f"host noisy-neighbor failed to start both socket workers: {last_error or 'processes missing'}")


def stop_host_noise(cfg: dict[str, str]) -> str:
    script = f"""
set -u
pid=$(cat {shlex.quote(PID_FILE)} 2>/dev/null || true)
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
  /bin/kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
  i=0
  while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  if kill -0 "$pid" 2>/dev/null; then
    /bin/kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
  fi
fi
rm -f {shlex.quote(PID_FILE)}
cat {shlex.quote(LOG_FILE)} 2>/dev/null || true
""".strip()
    return _ssh(cfg, script, check=False)
