from __future__ import annotations

import base64
import csv
import json
from pathlib import Path
from typing import Any

from .common import ROOT, dump_json, read_env, run
from .host_noise import is_host_noise

REMOTE_HELPER = "/usr/local/sbin/mxl-rdt-host"
REMOTE_DATA = "/tmp/mxl-rdt"
# Must match HELPER_VERSION in scripts/mxl-rdt-host.py.
EXPECTED_HELPER_VERSION = 8
CAT_PROFILES = ("cat-guarded", "cat-strong", "cat-16-1")
MBA_LEVELS = ("80", "60", "40", "20", "10")
VALID_PROFILES = (
    {"none"}
    | set(CAT_PROFILES)
    | {f"mba-{level}" for level in MBA_LEVELS}
    | {f"{cat}+mba-{level}" for cat in CAT_PROFILES for level in MBA_LEVELS}
)


def is_rdt_enabled(cfg: dict[str, str]) -> bool:
    return cfg.get("RDT_MONITOR", "0") == "1" or cfg.get("RDT_CONTROL_PROFILE", "none") != "none"


def validate_rdt_config(cfg: dict[str, str]) -> None:
    profile = cfg.get("RDT_CONTROL_PROFILE", "none")
    if profile not in VALID_PROFILES:
        raise ValueError(f"unsupported RDT_CONTROL_PROFILE: {profile}")
    if profile != "none" and cfg.get("NOISY_NEIGHBOR_ENABLED") != "1":
        raise ValueError("RDT resource control requires a noisy neighbor; clean scenarios remain unrestricted")
    # No lever/noise pairing rule: CAT limits streaming cache pollution from bandwidth
    # noise, and MBA slows the fill rate of LLC-thrashing noise. Both cross-pairings are
    # legitimate experiments, so the choice stays with the operator and the evidence.


def _target(cfg: dict[str, str]) -> str:
    inventory = read_env(ROOT / "config/nodes.env")
    node = cfg.get("NODE", cfg["LAB_DEFAULT_NODE"])
    key = node.upper().replace("-", "_") + "_HOST"
    address = inventory.get(key)
    if not address:
        raise ValueError(f"RDT has no SSH address for node {node} ({key})")
    user = inventory.get("LAB_SSH_USER")
    if not user:
        raise ValueError("RDT needs LAB_SSH_USER in config/nodes.env")
    return f"{user}@{address}"


def _ssh(cfg: dict[str, str], args: list[str], *, check: bool = True) -> str:
    return run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", _target(cfg), *args], check=check)


def _helper(cfg: dict[str, str], command: str, data: dict[str, Any] | None = None, *, check: bool = True) -> str:
    prefix = [] if _target(cfg).startswith("root@") else ["sudo", "-n"]
    args = [*prefix, REMOTE_HELPER, command]
    if data is not None:
        encoded = base64.urlsafe_b64encode(json.dumps(data, separators=(",", ":")).encode()).decode()
        args.extend(["--", encoded])
    return _ssh(cfg, args, check=check)


def rdt_capabilities(cfg: dict[str, str]) -> dict[str, Any]:
    raw = _helper(cfg, "capabilities")
    capabilities = json.loads(raw)
    version = capabilities.get("helper_version", 0)
    if version != EXPECTED_HELPER_VERSION:
        raise RuntimeError(
            f"worker RDT helper is version {version}, controller needs {EXPECTED_HELPER_VERSION}. "
            "Run scripts/update-rdt-helper.sh NODE to reinstall it."
        )
    profile = cfg.get("RDT_CONTROL_PROFILE", "none")
    supported = capabilities.get("supported_profiles", [])
    if profile != "none" and supported and profile not in supported:
        raise RuntimeError(f"worker RDT helper does not support control profile {profile}")
    return capabilities


def kubernetes_groups(namespace: str, app: str, cfg: dict[str, str]) -> dict[str, dict[str, Any]]:
    session = cfg.get("RDT_FOCUS_SESSION", "").strip()
    groups: dict[str, dict[str, Any]] = {}
    for role in ("encoder", "decoder"):
        selector = f"app={app},role={role}" + (f",session={session}" if session else "")
        pods = json.loads(run(["kubectl", "-n", namespace, "get", "pods", "-l", selector, "-o", "json"]))
        uids = [item["metadata"]["uid"] for item in pods.get("items", [])]
        if uids:
            groups[role] = {"kind": "pods", "comm": "ffmpeg", "pod_uids": uids}
    if not groups:
        target = f"session {session}" if session else "any session"
        raise RuntimeError(f"RDT could not resolve encoder/decoder Pod UIDs for {target}")
    if cfg.get("NOISY_NEIGHBOR_ENABLED") == "1":
        if is_host_noise(cfg):
            groups["noise"] = {"kind": "host-noise"}
        else:
            selector = f"app={app}-noise,role=noisy-neighbor"
            pods = json.loads(run(["kubectl", "-n", namespace, "get", "pods", "-l", selector, "-o", "json"]))
            uids = sorted(item.get("metadata", {}).get("uid", "") for item in pods.get("items", []))
            uids = [uid for uid in uids if uid]
            if not uids:
                raise RuntimeError("RDT could not resolve noisy-neighbor Pod UIDs")
            groups["noise"] = {"kind": "pods", "comm": "stress-ng", "pod_uids": uids}

    return groups


def start_rdt(cfg: dict[str, str], namespace: str, app: str, warmup: int, measure: int) -> dict[str, Any]:
    validate_rdt_config(cfg)
    data = {
        "profile": cfg.get("RDT_CONTROL_PROFILE", "none"),
        "groups": kubernetes_groups(namespace, app, cfg),
        "delay": warmup if cfg.get("RDT_INCLUDE_WARMUP", "0") != "1" else 0,
        "duration": measure + (0 if cfg.get("RDT_INCLUDE_WARMUP", "0") != "1" else warmup),
        "interval": int(cfg.get("RDT_INTERVAL_SECONDS", "1")),
    }
    return json.loads(_helper(cfg, "start", data))


def stop_rdt(cfg: dict[str, str]) -> dict[str, Any]:
    raw = _helper(cfg, "stop", check=False)
    if not raw.strip():
        return {"stopped": False, "restored": False, "error": "RDT helper returned no status"}
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {"stopped": False, "restored": False, "error": raw.strip()}


def _copy(cfg: dict[str, str], remote: str, local: Path) -> None:
    run(["scp", "-q", f"{_target(cfg)}:{remote}", str(local)])


def collect_rdt(cfg: dict[str, str], output: Path) -> list[dict[str, Any]]:
    _copy(cfg, f"{REMOTE_DATA}/capabilities.json", output / "rdt-capabilities.json")
    _copy(cfg, f"{REMOTE_DATA}/pid-map.json", output / "rdt-pid-map.json")
    _copy(cfg, f"{REMOTE_DATA}/after.json", output / "rdt-after.json")
    _copy(cfg, f"{REMOTE_DATA}/samples.csv", output / "rdt-samples.csv")
    samples: dict[tuple[str, str], list[dict[str, float]]] = {}
    with (output / "rdt-samples.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                sample = {key: float(row[key]) for key in ("timestamp", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes")}
            except (KeyError, TypeError, ValueError):
                continue
            samples.setdefault((row["group"], row["domain"]), []).append(sample)
    summaries = []
    metric_rows: list[dict[str, Any]] = []
    for (group, domain), values in sorted(samples.items()):
        if not values:
            continue
        occupancy = [item["llc_occupancy_bytes"] for item in values]
        # Only mbm_local_bytes and mbm_total_bytes are hardware counters; remote is derived per interval.
        rates: dict[str, list[float]] = {key: [] for key in ("mbm_local_bytes", "mbm_total_bytes", "mbm_remote_bytes")}
        for first, last in zip(values, values[1:]):
            elapsed = last["timestamp"] - first["timestamp"]
            if elapsed <= 0:
                continue
            interval: dict[str, float] = {}
            for key in ("mbm_local_bytes", "mbm_total_bytes"):
                if last[key] < first[key]:
                    raise RuntimeError(f"RDT counter reset detected for {group}/{domain}/{key}")
                interval[key] = (last[key] - first[key]) / elapsed
                rates[key].append(interval[key])
            rates["mbm_remote_bytes"].append(max(0.0, interval["mbm_total_bytes"] - interval["mbm_local_bytes"]))
        summary = {
            "group": group, "domain": domain, "sample_count": len(values),
            "llc_occupancy_bytes_avg": sum(occupancy) / len(occupancy),
            "llc_occupancy_bytes_min": min(occupancy), "llc_occupancy_bytes_max": max(occupancy),
        }
        for key, series in rates.items():
            metric = key.replace("_bytes", "_bytes_per_second")
            summary[metric + "_avg"] = sum(series) / len(series) if series else 0.0
            summary[metric + "_min"] = min(series) if series else 0.0
            summary[metric + "_max"] = max(series) if series else 0.0
        summaries.append(summary)
        labels = json.dumps({"group": group, "domain": domain}, sort_keys=True)
        for metric in ("llc_occupancy_bytes", "mbm_local_bytes_per_second", "mbm_total_bytes_per_second", "mbm_remote_bytes_per_second"):
            metric_rows.append({
                "category": "RDT", "metric": "rdt_" + metric, "unit": "bytes" if metric == "llc_occupancy_bytes" else "bytes/s",
                "value": f"avg={summary[metric + '_avg']:.6f};min={summary[metric + '_min']:.6f};max={summary[metric + '_max']:.6f}",
                "scope": labels, "session": "", "role": group,
            })
    dump_json(output / "rdt-summary.json", summaries)
    # A group that never held cache and never moved bytes was not really associated with
    # its tasks, so the A/B evidence would be silently wrong.
    for group in {item["group"] for item in summaries}:
        rows = [item for item in summaries if item["group"] == group]
        if all(item["llc_occupancy_bytes_max"] == 0 and item["mbm_total_bytes_per_second_max"] == 0 for item in rows):
            raise RuntimeError(
                f"RDT group {group} measured zero occupancy and zero bandwidth for the whole window; "
                "its tasks were not associated with the resctrl group"
            )
    return metric_rows


def append_rdt_metrics(output: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    fields = ["category", "metric", "unit", "value", "scope", "session", "role"]
    with (output / "metrics.csv").open("a", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})
