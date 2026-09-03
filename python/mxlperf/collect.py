from __future__ import annotations

import csv
import json
import os
import time
from collections import defaultdict
from pathlib import Path
from typing import Any

import requests

from .common import dump_json, run, write_rows


def pods(namespace: str, app: str) -> list[dict[str, str]]:
    data = json.loads(run(["kubectl", "-n", namespace, "get", "pods", "-l", f"app={app}", "-o", "json"]))
    result = []
    for item in data["items"]:
        labels = item["metadata"].get("labels", {})
        result.append({
            "pod": item["metadata"]["name"],
            "role": labels.get("role", "unknown"),
            "session": labels.get("session", "unknown"),
            "node": item["spec"].get("nodeName", "unknown"),
        })
    return sorted(result, key=lambda row: (row["session"], row["role"]))


def process_snapshot(namespace: str, items: list[dict[str, str]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    placement: list[dict[str, Any]] = []
    threads: list[dict[str, Any]] = []
    shell = r'''pid=$(pidof ffmpeg | awk '{print $1}'); test -n "$pid" || exit 4
allowed=$(awk '/Cpus_allowed_list/{print $2}' /proc/$pid/status)
printf 'P|%s|%s\n' "$pid" "$allowed"
for t in /proc/$pid/task/*; do
  test -r "$t/stat" || continue
  awk '{print "T|" $1 "|" $14 "|" $15 "|" $39}' "$t/stat"
done'''
    for item in items:
        output = run(
            ["kubectl", "-n", namespace, "exec", item["pod"], "-c", item["role"], "--", "bash", "-c", shell],
            check=False,
        )
        for line in output.splitlines():
            fields = line.split("|")
            if fields[0] == "P":
                placement.append({**item, "pid": fields[1], "allowed_cpu_ids": fields[2]})
            elif fields[0] == "T":
                threads.append({**item, "tid": fields[1], "ticks": int(fields[2]) + int(fields[3]), "cpu": int(fields[4])})
    return placement, threads


def progress_snapshot(namespace: str, items: list[dict[str, str]]) -> dict[tuple[str, str], tuple[int, float]]:
    result: dict[tuple[str, str], tuple[int, float]] = {}
    shell = r'''awk -F= '$1=="frame"{frame=$2} END{if(frame!="") print frame}' /run/mxl/progress 2>/dev/null'''
    for item in items:
        value = run(
            ["kubectl", "-n", namespace, "exec", item["pod"], "-c", item["role"], "--", "bash", "-c", shell],
            check=False,
        ).strip()
        if value.isdigit():
            result[(item["session"], item["role"])] = (int(value), time.monotonic())
    return result


def node_cpu_snapshot(namespace: str, item: dict[str, str]) -> dict[int, tuple[int, int]]:
    output = run(
        ["kubectl", "-n", namespace, "exec", item["pod"], "-c", item["role"], "--", "cat", "/proc/stat"],
        check=False,
    )
    result: dict[int, tuple[int, int]] = {}
    for line in output.splitlines():
        fields = line.split()
        if not fields or not fields[0].startswith("cpu") or not fields[0][3:].isdigit():
            continue
        values = [int(value) for value in fields[1:9]]
        total = sum(values)
        idle = values[3] + values[4]
        result[int(fields[0][3:])] = (total, idle)
    return result


def node_numa_snapshot(namespace: str, item: dict[str, str]) -> dict[tuple[int, str], int]:
    shell = r'''for file in /sys/devices/system/node/node*/numastat; do
  node=${file%/numastat}; node=${node##*node}
  awk -v node="$node" '{print node "|" $1 "|" $2}' "$file"
done'''
    output = run(
        ["kubectl", "-n", namespace, "exec", item["pod"], "-c", item["role"], "--", "bash", "-c", shell],
        check=False,
    )
    result: dict[tuple[int, str], int] = {}
    for line in output.splitlines():
        fields = line.split("|")
        if len(fields) != 3:
            continue
        try:
            result[(int(fields[0]), fields[1])] = int(fields[2])
        except ValueError:
            continue
    return result


def prom_query_range(url: str, query: str, start: int, end: int, step: int) -> list[dict[str, Any]]:
    response = requests.get(
        f"{url.rstrip('/')}/api/v1/query_range",
        params={"query": query, "start": start, "end": end, "step": step},
        timeout=30,
    )
    response.raise_for_status()
    payload = response.json()
    if payload.get("status") != "success":
        raise RuntimeError(str(payload))
    return payload["data"]["result"]


def prom_query(url: str, query: str) -> list[dict[str, Any]]:
    response = requests.get(
        f"{url.rstrip('/')}/api/v1/query",
        params={"query": query},
        timeout=30,
    )
    response.raise_for_status()
    payload = response.json()
    if payload.get("status") != "success":
        raise RuntimeError(str(payload))
    return payload["data"]["result"]


def aggregate_series(series: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for item in series:
        values = [float(value) for _, value in item.get("values", []) if value not in ("NaN", "+Inf", "-Inf")]
        if values:
            result.append({"labels": item.get("metric", {}), "avg": sum(values) / len(values), "min": min(values), "max": max(values)})
    return result


def aggregate_counter_rate(series: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for item in series:
        values = [(float(timestamp), float(value)) for timestamp, value in item.get("values", [])]
        if len(values) >= 2 and values[-1][0] > values[0][0]:
            rate = max(0.0, values[-1][1] - values[0][1]) / (values[-1][0] - values[0][0])
            result.append({"labels": item.get("metric", {}), "avg": rate, "min": rate, "max": rate})
    return result


COUNTER_RATE_METRICS = {
    "fps",
    "cross_numa_upi_incoming_bytes_per_second",
    "socket_upi_incoming_bytes_per_second",
}


def aggregate_metric(name: str, series: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return aggregate_counter_rate(series) if name in COUNTER_RATE_METRICS else aggregate_series(series)


def upi_incoming_query(pcm_selector: str) -> str:
    # PCM exports cumulative incoming bytes. Keep raw sum here; aggregate_metric
    # calculates exact capture-window slope without a PromQL rate window leaking
    # warm-up or pre-measurement traffic into short runs.
    return (
        'sum({__name__=~"Incoming_Data_Traffic_On_Link_[0-3]",'
        f'{pcm_selector},aggregate="system"}})'
    )


def pcm_metric_selector(node: str) -> str:
    """Select the host PCM exporter scraped for this worker.

    PCM runs as a host systemd service, not as a Pod: inside a Pod its threads
    are confined to the Pod's cpuset, and under a dense pinned workload that
    cpuset can collapse onto one socket, which hides exactly the cross-socket
    UPI traffic the study is about. See scripts/install-pcm-host.sh.
    """
    node_data = json.loads(run(["kubectl", "get", "node", node, "-o", "json"]))
    node_ip = next(
        address["address"] for address in node_data["status"].get("addresses", [])
        if address.get("type") == "InternalIP"
    )
    return f'job="pcm-sensor-server",instance=~"{node_ip}(:.*)?"'


def capture(
    output: Path,
    namespace: str,
    app: str,
    node: str,
    prom_url: str,
    duration: int,
    step: int,
    min_fps: float,
) -> None:
    output.mkdir(parents=True, exist_ok=True)
    items = pods(namespace, app)
    placement_start, first = process_snapshot(namespace, items)
    node_cpu_start = node_cpu_snapshot(namespace, items[0]) if items else {}
    node_numa_start = node_numa_snapshot(namespace, items[0]) if items else {}
    started = int(time.time())
    progress_start = progress_snapshot(namespace, items)
    clock_ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    previous = {(r["pod"], r["tid"]): r["ticks"] for r in first}
    usage = defaultdict(float)
    observed = defaultdict(set)
    placement_end = placement_start
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        time.sleep(min(step, max(0, deadline - time.monotonic())))
        placement_end, current = process_snapshot(namespace, items)
        for row in current:
            key = (row["pod"], row["tid"])
            old_ticks = previous.get(key, row["ticks"])
            delta = max(0, row["ticks"] - old_ticks)
            role_key = (row["session"], row["role"])
            if delta:
                observed[role_key].add(row["cpu"])
                usage[(row["session"], row["role"], row["cpu"])] += delta / clock_ticks
            previous[key] = row["ticks"]
    ended = int(time.time())
    node_cpu_end = node_cpu_snapshot(namespace, items[0]) if items else {}
    node_numa_end = node_numa_snapshot(namespace, items[0]) if items else {}
    progress_end = progress_snapshot(namespace, items)

    node_data = json.loads(run(["kubectl", "get", "node", node, "-o", "json"]))
    node_ip = next(
        address["address"] for address in node_data["status"].get("addresses", [])
        if address.get("type") == "InternalIP"
    )
    node_selector = f'instance=~"{node_ip}(:.*)?"'
    pcm_selector = pcm_metric_selector(node)

    queries = {
        "fps": f'mxl_ffmpeg_frames_total{{namespace="{namespace}"}}',
        "reported_fps": f'mxl_ffmpeg_fps{{namespace="{namespace}"}}',
        "workload_cpu_cores": f'sum by (pod) (rate(container_cpu_usage_seconds_total{{namespace="{namespace}",container=~"decoder|encoder"}}[30s]))',
        "workload_memory_bytes": f'sum by (pod) (container_memory_working_set_bytes{{namespace="{namespace}",container=~"decoder|encoder"}})',
        "node_busy_cores": f'sum(rate(node_cpu_seconds_total{{mode!="idle",mode!="iowait",{node_selector}}}[2m]))',
        "node_memory_used_bytes": f'node_memory_MemTotal_bytes{{{node_selector}}}-node_memory_MemAvailable_bytes{{{node_selector}}}',
        "node_memory_total_bytes": f'node_memory_MemTotal_bytes{{{node_selector}}}',
        "drops": f'mxl_ffmpeg_drop_frames_total{{namespace="{namespace}"}}',
        "output_time": f'mxl_ffmpeg_out_time_seconds{{namespace="{namespace}"}}',
        "l3_cache_hits_per_second": f'sum(rate(L3_Cache_Hits{{{pcm_selector}}}[2m]))',
        "l3_cache_misses_per_second": f'sum(rate(L3_Cache_Misses{{{pcm_selector}}}[2m]))',
        "l3_cache_hit_ratio": f'sum(rate(L3_Cache_Hits{{{pcm_selector}}}[2m])) / (sum(rate(L3_Cache_Hits{{{pcm_selector}}}[2m])) + sum(rate(L3_Cache_Misses{{{pcm_selector}}}[2m])))',
        "l2_cache_hit_ratio": (
            f'sum(rate(L2_Cache_Hits{{{pcm_selector}}}[2m])) / '
            f'clamp_min(sum(rate(L2_Cache_Hits{{{pcm_selector}}}[2m])) + '
            f'sum(rate(L2_Cache_Misses{{{pcm_selector}}}[2m])), 1)'
        ),
        "l3_cache_miss_rate": (
            f'sum(rate(L3_Cache_Misses{{{pcm_selector}}}[2m])) / '
            f'clamp_min(sum(rate(L3_Cache_Hits{{{pcm_selector}}}[2m])) + '
            f'sum(rate(L3_Cache_Misses{{{pcm_selector}}}[2m])), 1)'
        ),
        "dram_read_bytes_per_second": f'sum(rate(DRAM_Reads{{{pcm_selector},aggregate="system"}}[2m]))',
        "dram_write_bytes_per_second": f'sum(rate(DRAM_Writes{{{pcm_selector},aggregate="system"}}[2m]))',
        "local_memory_bandwidth_bytes_per_second": f'sum(Local_Memory_Bandwidth{{{pcm_selector},aggregate="system"}})',
        "remote_memory_bandwidth_bytes_per_second": f'sum(Remote_Memory_Bandwidth{{{pcm_selector},aggregate="system"}})',
        "remote_memory_bandwidth_ratio": (
            f'sum(Remote_Memory_Bandwidth{{{pcm_selector},aggregate="system"}}) / '
            f'clamp_min(sum(Local_Memory_Bandwidth{{{pcm_selector},aggregate="system"}}) + '
            f'sum(Remote_Memory_Bandwidth{{{pcm_selector},aggregate="system"}}), 1)'
        ),
        "socket_local_memory_bandwidth_bytes_per_second": f'Local_Memory_Bandwidth{{{pcm_selector},aggregate="socket"}}',
        "socket_remote_memory_bandwidth_bytes_per_second": f'Remote_Memory_Bandwidth{{{pcm_selector},aggregate="socket"}}',
        "cross_numa_upi_incoming_bytes_per_second": upi_incoming_query(pcm_selector),
        "socket_upi_incoming_bytes_per_second": (
            'sum by (socket) ({__name__=~"Incoming_Data_Traffic_On_Link_[0-3]",'
            f'{pcm_selector},aggregate="system"}})'
        ),
    }
    raw_prom: dict[str, Any] = {}
    for name, query in queries.items():
        try:
            series = prom_query_range(prom_url, query, started, ended, step)
            raw_prom[name] = aggregate_metric(name, series)
        except Exception as error:  # preserve partial report
            raw_prom[name] = {"error": str(error), "query": query}
    dump_json(output / "prometheus.json", raw_prom)
    dump_json(output / "window.json", {"start": started, "end": ended, "duration_s": ended - started, "min_fps": min_fps})

    placement_by_role = {(r["session"], r["role"]): r for r in placement_end or placement_start}
    cpu_rows = []
    metric_rows: list[dict[str, Any]] = []
    elapsed = max(1, ended - started)
    for key, (last_frame, last_timestamp) in sorted(progress_end.items()):
        if key not in progress_start:
            continue
        session, role = key
        first_frame, first_timestamp = progress_start[key]
        true_fps = max(0, last_frame - first_frame) / max(0.001, last_timestamp - first_timestamp)
        metric_rows.append({
            "category": "FFmpeg progress", "metric": "fps", "unit": "frames/s",
            "value": f"avg={true_fps:.6f};min={true_fps:.6f};max={true_fps:.6f}",
            "scope": "measurement-window frame delta", "session": session, "role": role,
        })
    for key in sorted(placement_by_role):
        session, role = key
        allowed = placement_by_role[key]["allowed_cpu_ids"]
        used = sorted(observed[key])
        total_seconds = sum(seconds for (s, r, _), seconds in usage.items() if (s, r) == key)
        demand = total_seconds / max(1, ended - started)
        avg_used = demand * 100 / len(used) if used else 0
        metric_rows.extend([
            {"category": "CPU Placement", "metric": "allowed CPU IDs", "unit": "cpu-list", "value": allowed, "scope": "process", "session": session, "role": role},
            {"category": "CPU Placement", "metric": "measured used CPU IDs", "unit": "cpu-list", "value": ",".join(map(str, used)) or "none", "scope": "process", "session": session, "role": role},
            {"category": "CPU Placement", "metric": "measured used CPU count", "unit": "cores", "value": len(used), "scope": "process", "session": session, "role": role},
            {"category": "CPU", "metric": "FFmpeg CPU demand", "unit": "cores", "value": f"{demand:.3f}", "scope": "process", "session": session, "role": role},
            {"category": "CPU", "metric": "average FFmpeg utilization per measured used CPU", "unit": "%", "value": f"{avg_used:.3f}", "scope": "process", "session": session, "role": role},
        ])
        for cpu in used:
            seconds = usage[(session, role, cpu)]
            cpu_rows.append({
                "session": session, "role": role, "cpu_id": cpu,
                "ffmpeg_cpu_seconds": f"{seconds:.3f}",
                "ffmpeg_utilization_pct": f"{seconds * 100 / max(1, ended - started):.3f}",
                "attribution": "thread user+system tick delta for sample interval attributed to sampled last CPU",
            })
    for name, series in raw_prom.items():
        if not isinstance(series, list):
            continue
        for item in series:
            labels = item["labels"]
            session = labels.get("session", "")
            role = labels.get("role", "")
            if not session:
                pod = labels.get("pod", labels.get("exported_pod", ""))
                session = pod.rsplit("-", 1)[-1] if pod else ""
                if not role:
                    role = "decoder" if "-decoder-" in pod else "encoder" if "-encoder-" in pod else ""
            metric_rows.append({
                "category": "Prometheus", "metric": name, "unit": "native",
                "value": f"avg={item['avg']:.6f};min={item['min']:.6f};max={item['max']:.6f}",
                "scope": json.dumps(labels, sort_keys=True),
                "session": session, "role": role,
            })
    write_rows(output / "metrics.csv", metric_rows)
    node_cpu_rows = []
    for cpu_id in sorted(node_cpu_start.keys() & node_cpu_end.keys()):
        start_total, start_idle = node_cpu_start[cpu_id]
        end_total, end_idle = node_cpu_end[cpu_id]
        total_delta = max(0, end_total - start_total)
        idle_delta = max(0, end_idle - start_idle)
        busy_pct = 100 * max(0, total_delta - idle_delta) / total_delta if total_delta else 0
        node_cpu_rows.append({
            "cpu_id": cpu_id,
            "real_cpu_total_usage_pct": f"{busy_pct:.3f}",
            "total_jiffies": total_delta,
            "busy_jiffies": max(0, total_delta - idle_delta),
            "scope": "all host work during exact measurement window; idle and iowait excluded",
        })
    with (output / "core-system-usage.csv").open("w", newline="") as handle:
        fields = ["cpu_id", "real_cpu_total_usage_pct", "total_jiffies", "busy_jiffies", "scope"]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader(); writer.writerows(node_cpu_rows)
    with (output / "cpu-usage.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["session", "role", "cpu_id", "ffmpeg_cpu_seconds", "ffmpeg_utilization_pct", "attribution"])
        writer.writeheader(); writer.writerows(cpu_rows)
    with (output / "cpu-placement.csv").open("w", newline="") as handle:
        fields = ["pod", "role", "session", "node", "pid", "allowed_cpu_ids"]
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(placement_end or placement_start)
    numa_rows = []
    for key in sorted(node_numa_start.keys() | node_numa_end.keys()):
        node_id, counter = key
        start_value = node_numa_start.get(key, 0)
        end_value = node_numa_end.get(key, start_value)
        numa_rows.append({
            "node": node_id,
            "counter": counter,
            "start_pages": start_value,
            "end_pages": end_value,
            "delta_pages": max(0, end_value - start_value),
            "scope": "whole worker during exact measurement window",
        })
    with (output / "numa-node-counters.csv").open("w", newline="") as handle:
        fields = ["node", "counter", "start_pages", "end_pages", "delta_pages", "scope"]
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(numa_rows)
