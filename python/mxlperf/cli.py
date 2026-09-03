from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
import yaml
from pathlib import Path
from urllib.error import URLError
from urllib.parse import urlparse
from urllib.request import urlopen

from .collect import capture, pcm_metric_selector, prom_query
from .common import ROOT, dump_json, load_config, read_env, run, utc_stamp, expand_cpu_spec
from .host_noise import host_noise_snapshot, is_host_noise, start_host_noise, stop_host_noise
from .platform import PROBE_COMMAND, parse_platform
from .render import render
from . import topology
from .report import build_report
from .rdt import (
    VALID_PROFILES, append_rdt_metrics, collect_rdt, is_rdt_enabled, rdt_capabilities,
    start_rdt, stop_rdt, validate_rdt_config,
)
from .summary import build_summary


def duration(value: str) -> int:
    unit = value[-1:] if value[-1:].isalpha() else "s"
    number = int(value[:-1] if value[-1:].isalpha() else value)
    return number * {"s": 1, "m": 60, "h": 3600}.get(unit, 0)


def overrides(args) -> list[str]:
    values = list(args.set or [])
    mapping = {
        "streams": "STREAMS", "node": "NODE", "resolution": "RESOLUTION", "preset": "PRESET",
        "enc_threads": "ENC_THREADS", "dec_threads": "DEC_THREADS", "filter_threads": "FILTER_THREADS",
        "slices": "SLICES", "dec_cores": "DEC_CORES", "enc_cores": "ENC_CORES",
        "rdt_control": "RDT_CONTROL_PROFILE", "rdt_focus_session": "RDT_FOCUS_SESSION",
    }
    for attribute, key in mapping.items():
        value = getattr(args, attribute, None)
        if value is not None:
            values.append(f"{key}={value}")
    return values


def prometheus_ready(url: str) -> bool:
    try:
        with urlopen(f"{url.rstrip('/')}/-/ready", timeout=2) as response:
            return response.status == 200
    except (OSError, URLError):
        return False


def ensure_prometheus(url: str) -> subprocess.Popen[str] | None:
    if prometheus_ready(url):
        return None
    parsed = urlparse(url)
    if parsed.hostname not in ("127.0.0.1", "localhost") or parsed.port is None:
        raise RuntimeError(f"Prometheus is unavailable: {url}")
    process = subprocess.Popen(
        [
            "kubectl", "-n", "monitoring", "port-forward",
            "svc/monitoring-kube-prometheus-prometheus", f"{parsed.port}:9090",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    for _ in range(40):
        if prometheus_ready(url):
            return process
        if process.poll() is not None:
            error = process.stderr.read().strip() if process.stderr else ""
            raise RuntimeError(f"Prometheus port-forward failed: {error}")
        time.sleep(0.25)
    process.terminate()
    process.wait(timeout=5)
    raise RuntimeError(f"Prometheus did not become ready: {url}")


def wait_for_pcm_cross_numa(
    prom_url: str,
    pcm_selector: str,
    maximum_sample_age: float = 30,
    timeout: int = 120,
    query_fn=prom_query,
) -> None:
    if not pcm_selector:
        raise RuntimeError("pinned measurement requires pcm-sensor-server on target worker")
    probes = {
        "UPI": (
            'time() - max(timestamp(label_replace('
            '{__name__=~"Incoming_Data_Traffic_On_Link_[0-3]",'
            f'{pcm_selector},aggregate="system"}},'
            '"pcm_link","$1","__name__","(.*)")))'
        ),
    }
    deadline = time.monotonic() + timeout
    missing = list(probes)
    failures: dict[str, str] = {}
    while True:
        missing = []
        for name, query in probes.items():
            try:
                result = query_fn(prom_url, query)
                if not result:
                    missing.append(name)
                    failures[name] = "query returned no series"
                    continue
                value = float(result[0]["value"][1])
                if value < 0:
                    missing.append(name)
                    failures[name] = f"sample is {-value:.1f}s in the future; check host clocks"
                elif value > maximum_sample_age:
                    missing.append(name)
                    failures[name] = f"newest sample is {value:.1f}s old"
            except (KeyError, IndexError, TypeError, ValueError, OSError, RuntimeError) as error:
                missing.append(name)
                failures[name] = f"Prometheus query failed: {error}"
        if not missing:
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(2, remaining))
    details = "; ".join(f"{name}: {failures.get(name, 'unavailable')}" for name in missing)
    raise RuntimeError(
        "pinned measurement aborted: PCM cross-NUMA series unavailable after "
        f"{timeout}s ({details}). No comparable score was recorded."
    )


def capture_host(result: Path, cfg: dict[str, str]) -> None:
    node = cfg.get("NODE", cfg["LAB_DEFAULT_NODE"])
    node_json = json.loads(run(["kubectl", "get", "node", node, "-o", "json"]))
    host = {
        "node": node,
        "kubernetes": {
            "capacity": node_json["status"].get("capacity", {}),
            "allocatable": node_json["status"].get("allocatable", {}),
            "nodeInfo": node_json["status"].get("nodeInfo", {}),
            "addresses": node_json["status"].get("addresses", []),
        },
        "configured_topology": {
            "sockets": cfg.get("LAB_SOCKET_COUNT"), "cores_per_socket": cfg.get("LAB_CORES_PER_SOCKET"),
            "threads_per_core": cfg.get("LAB_THREADS_PER_CORE", "1"),
            "reserved_cpu_ids": cfg.get("LAB_RESERVED_CPUS"),
            "cpu_numbering": cfg.get("LAB_CPU_NUMBERING", "alternating"),
            "socket0_parity": cfg.get("LAB_SOCKET0_PARITY"),
            "full_pcpus_only": topology.full_pcpus_only(cfg),
        },
    }
    try:
        inventory = read_env(ROOT / "config/nodes.env")
        address_key = node.upper().replace("-", "_") + "_HOST"
        address = inventory.get(address_key)
        user = inventory.get("LAB_SSH_USER")
        if not address or not user:
            host["live_hardware_probe_error"] = (
                f"config/nodes.env needs {address_key} and LAB_SSH_USER for the hardware probe"
            )
        else:
            probe = run([
                "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", f"{user}@{address}",
                "printf '%s\\n' '=== uname ==='; uname -a; "
                "printf '%s\\n' '=== lscpu-json ==='; lscpu -J; "
                "printf '%s\\n' '=== memory ==='; cat /proc/meminfo; "
                "printf '%s\\n' '=== cmdline ==='; cat /proc/cmdline; "
                "printf '%s\\n' '=== numa ==='; numactl --hardware 2>/dev/null || true",
            ])
            host["live_hardware_probe"] = probe
            spec_raw = run([
                "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", f"{user}@{address}",
                PROBE_COMMAND,
            ])
            declared = inventory.get(node.upper().replace("-", "_") + "_MEM_TRANSFER_MT_S")
            host["platform_spec"] = parse_platform(spec_raw, int(declared) if declared and declared.isdigit() else None)
    except (OSError, RuntimeError, ValueError) as error:
        host["live_hardware_probe_error"] = str(error)
    mismatches = topology_mismatches(cfg, host.get("platform_spec") or {})
    if mismatches:
        # Recorded rather than raised: the run already happened, and a reader of
        # this result needs to know the cpusets were planned for a different
        # machine than the one that produced the score.
        host["configured_topology_mismatch"] = mismatches
    power = power_mismatches(cfg, host.get("platform_spec") or {})
    if power:
        host["configured_power_mismatch"] = power
    dump_json(result / "host.json", host)


def topology_mismatches(cfg: dict[str, str], spec: dict[str, object]) -> list[str]:
    """Where config/lab.env disagrees with what the worker actually reports."""
    expected = [
        ("sockets", "LAB_SOCKET_COUNT", cfg.get("LAB_SOCKET_COUNT")),
        ("cores_per_socket", "LAB_CORES_PER_SOCKET", cfg.get("LAB_CORES_PER_SOCKET")),
        ("threads_per_core", "LAB_THREADS_PER_CORE", cfg.get("LAB_THREADS_PER_CORE", "1")),
    ]
    problems = []
    for field, name, configured in expected:
        observed = spec.get(field)
        if observed is None or configured is None or int(configured) == int(observed):
            continue
        problems.append(
            f"{name}={configured} but the worker reports {observed}; "
            "the planned cpusets and the stream capacity were computed for the "
            "configured value, so this score is not comparable"
        )
    return problems


def power_mismatches(cfg: dict[str, str], spec: dict[str, object]) -> list[str]:
    """Where the worker's live power settings disagree with config/lab.env.

    The governor, the P-state driver and EPB decide what frequency the cores a
    run is given actually reach, so they belong in the result next to the score -
    otherwise a powersave node just looks like slower hardware.
    scripts/configure-power.sh sets them; this only records what was true.
    """
    power = spec.get("power")
    if not isinstance(power, dict):
        return []
    expected = [
        ("governor", "LAB_POWER_GOVERNOR", cfg.get("LAB_POWER_GOVERNOR", "performance")),
        ("pstate_status", "LAB_POWER_PSTATE_DRIVER", cfg.get("LAB_POWER_PSTATE_DRIVER", "active")),
        ("epb", "LAB_POWER_EPB", cfg.get("LAB_POWER_EPB", "0")),
    ]
    problems = []
    for field, name, configured in expected:
        observed = str(power.get(field, ""))
        # Empty means the kernel does not expose it, and "skip" means the lab was
        # told not to manage it: neither is a disagreement.
        if not observed or configured == "skip" or observed == configured:
            continue
        problems.append(
            f"{name}={configured} but the worker reports {observed}; "
            "run scripts/configure-power.sh, and treat this score as measured on a "
            "different platform configuration"
        )
    return problems


def noisy_neighbor_pod_count(cfg: dict[str, str]) -> int:
    raw = cfg.get("NOISY_NEIGHBOR_POD_COUNT", "1")
    try:
        count = int(raw)
    except ValueError as error:
        raise ValueError("NOISY_NEIGHBOR_POD_COUNT must be an integer") from error
    if count < 1:
        raise ValueError("NOISY_NEIGHBOR_POD_COUNT must be >= 1")
    return count


def noisy_neighbor_spread_by_node(cfg: dict[str, str]) -> bool:
    return cfg.get("NOISY_NEIGHBOR_SPREAD_BY_NODE", "0") == "1"


def parse_cpuset_from_runtime_evidence(raw: str) -> str:
    lines = [line.strip() for line in raw.splitlines()]
    for index, line in enumerate(lines):
        if line == "=== cpuset ===":
            for item in lines[index + 1:]:
                if item and not item.startswith("==="):
                    return item
            break
    return ""


def cpu_socket_id(cfg: dict[str, str], cpu: int) -> int:
    # Via the topology model, so a sibling thread reports its own core's socket
    # instead of a socket id that does not exist on a two-socket machine.
    return topology.socket_id(topology.from_config(cfg), cpu)


def noisy_neighbor_numa_report(cfg: dict[str, str], pod_details: list[dict]) -> dict[str, object]:
    per_pod = []
    for pod in pod_details:
        cpuset = parse_cpuset_from_runtime_evidence(str(pod.get("runtime_evidence", "")))
        cpus = expand_cpu_spec(cpuset) if cpuset else []
        sockets = sorted({cpu_socket_id(cfg, cpu) for cpu in cpus}) if cpus else []
        per_pod.append({
            "name": pod.get("name", "unknown"),
            "node": pod.get("node", ""),
            "cpuset_effective": cpuset,
            "cpu_ids": cpus,
            "socket_ids": sockets,
        })

    status = "n/a"
    same_socket = None
    if len(per_pod) >= 2:
        single_socket_assignments = [item["socket_ids"] for item in per_pod if len(item["socket_ids"]) == 1]
        if len(single_socket_assignments) == len(per_pod):
            unique = sorted({item[0] for item in single_socket_assignments})
            if len(unique) == len(per_pod):
                status = "split"
                same_socket = False
            elif len(unique) == 1:
                status = "same-socket"
                same_socket = True
            else:
                status = "partial-split"
                same_socket = False
        else:
            status = "mixed-or-multisocket"
            same_socket = False

    return {
        "status": status,
        "same_socket": same_socket,
        "pod_count": len(per_pod),
        "pods": per_pod,
    }


def apply_documents(documents: list[dict]) -> None:
    if not documents:
        return
    run(["kubectl", "apply", "-f", "-"], input_text=yaml.safe_dump_all(documents, sort_keys=False))


def split_rendered_documents(rendered: Path) -> tuple[list[dict], list[dict], list[dict]]:
    common: list[dict] = []
    noise: list[dict] = []
    workload: list[dict] = []
    for document in yaml.safe_load_all((rendered / "workload.yaml").read_text()):
        if not isinstance(document, dict):
            continue
        if document.get("kind") != "Pod":
            common.append(document)
            continue
        role = document.get("metadata", {}).get("labels", {}).get("role")
        if role == "noisy-neighbor":
            noise.append(document)
        else:
            workload.append(document)
    return common, noise, workload


def pod_names_from_documents(documents: list[dict]) -> list[str]:
    names = []
    for document in documents:
        if document.get("kind") != "Pod":
            continue
        name = document.get("metadata", {}).get("name")
        if isinstance(name, str) and name:
            names.append(name)
    return names


def noisy_neighbor_selector(cfg: dict[str, str]) -> str:
    return f"app={cfg['LAB_APP']}-noise,role=noisy-neighbor"


def noisy_neighbor_pods(namespace: str, cfg: dict[str, str]) -> list[dict]:
    pods = json.loads(run([
        "kubectl", "-n", namespace, "get", "pods", "-l", noisy_neighbor_selector(cfg), "-o", "json",
    ]))
    return sorted(pods.get("items", []), key=lambda item: item.get("metadata", {}).get("name", ""))


def noisy_neighbor_logs(namespace: str, cfg: dict[str, str]) -> str:
    chunks = []
    for pod in noisy_neighbor_pods(namespace, cfg):
        name = pod.get("metadata", {}).get("name", "unknown")
        logs = run(["kubectl", "-n", namespace, "logs", name], check=False).strip()
        chunks.append(f"=== {name} ===\n{logs}")
    return "\n\n".join(chunks)


def noisy_neighbor_snapshot(namespace: str, cfg: dict[str, str]) -> dict:
    pods = noisy_neighbor_pods(namespace, cfg)
    expected = noisy_neighbor_pod_count(cfg)
    if len(pods) != expected:
        raise RuntimeError(
            f"noisy-neighbor expected {expected} Pod(s), found {len(pods)} with selector {noisy_neighbor_selector(cfg)}"
        )
    details = []
    combined_evidence = []
    for pod in pods:
        name = pod.get("metadata", {}).get("name", "unknown")
        status = pod.get("status", {})
        container_statuses = status.get("containerStatuses", [])
        terminated = [
            item.get("state", {}).get("terminated", {})
            for item in container_statuses
            if item.get("state", {}).get("terminated")
        ]
        if status.get("phase") != "Running" or terminated:
            logs = run(["kubectl", "-n", namespace, "logs", name], check=False).strip()
            reason = terminated[0].get("reason", status.get("phase", "not running")) if terminated else status.get("phase", "not running")

            exit_code = terminated[0].get("exitCode", "unknown") if terminated else "unknown"
            raise RuntimeError(
                f"noisy-neighbor Pod {name} stopped before evidence capture: reason={reason} exit_code={exit_code}\n{logs}"
            )
        raw = run([
            "kubectl", "-n", namespace, "exec", name, "--", "sh", "-c",
            "printf '%s\\n' '=== version ==='; stress-ng --version; "
            "printf '%s\\n' '=== cpu.stat ==='; cat /sys/fs/cgroup/cpu.stat; "
            "printf '%s\\n' '=== cpuset ==='; cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null || true; "
            "printf '%s\\n' '=== memory.current ==='; cat /sys/fs/cgroup/memory.current; "
            "printf '%s\\n' '=== processes ==='; ps -eo pid,psr,pcpu,rss,comm,args 2>/dev/null || true",
        ])
        details.append({
            "name": name,
            "phase": status.get("phase"),
            "pod_ip": status.get("podIP"),
            "node": pod.get("spec", {}).get("nodeName"),
            "container_statuses": container_statuses,
            "runtime_evidence": raw,
        })
        combined_evidence.append(f"=== {name} ===\n{raw}")

    numa = noisy_neighbor_numa_report(cfg, details)

    return {
        "phase": "Running",
        "node": details[0].get("node", ""),
        "pod_count": len(details),
        "pod_names": [item["name"] for item in details],
        "pods": details,
        "numa": numa,
        "runtime_evidence": "\n\n".join(combined_evidence),
    }


def wait_for_noisy_neighbor(namespace: str, cfg: dict[str, str], timeout: int = 240) -> None:
    expected = noisy_neighbor_pod_count(cfg)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pods = noisy_neighbor_pods(namespace, cfg)
        if len(pods) != expected:
            time.sleep(1)
            continue
        ready = 0
        for pod in pods:
            status = pod.get("status", {})
            if any(
                condition.get("type") == "Ready" and condition.get("status") == "True"
                for condition in status.get("conditions", [])
            ):
                ready += 1
                continue
            container_statuses = status.get("containerStatuses", [])
            terminated = [
                item.get("state", {}).get("terminated", {})
                for item in container_statuses
                if item.get("state", {}).get("terminated")
            ]
            if status.get("phase") == "Failed" or terminated:
                name = pod.get("metadata", {}).get("name", "unknown")
                logs = run(["kubectl", "-n", namespace, "logs", name], check=False).strip()
                reason = terminated[0].get("reason", status.get("phase", "Failed")) if terminated else status.get("phase", "Failed")
                exit_code = terminated[0].get("exitCode", "unknown") if terminated else "unknown"
                raise RuntimeError(
                    f"noisy-neighbor Pod {name} exited before measurement: reason={reason} exit_code={exit_code}\n{logs}"
                )
        if ready == expected:
            return
        time.sleep(1)
    raise RuntimeError(f"noisy-neighbor did not become Ready within {timeout}s  (expected {expected} Pod(s))")


def wait_for_named_pod_ready(namespace: str, pod_name: str, timeout: int = 240) -> None:
    run([
        "kubectl", "-n", namespace, "wait", "--for=condition=Ready", f"pod/{pod_name}", f"--timeout={timeout}s",
    ])


def wait_for_workload_ready(namespace: str, app: str, timeout: int = 240) -> None:
    try:
        run([
            "kubectl", "-n", namespace, "wait", "--for=condition=Ready", "pod",
            f"-l=app={app}", f"--timeout={timeout}s",
        ])
    except RuntimeError as error:
        pods = run([
            "kubectl", "-n", namespace, "get", "pods", f"-l=app={app}",
            "-o=custom-columns=NAME:.metadata.name,PHASE:.status.phase,REASON:.status.containerStatuses[*].state.waiting.reason,NODE:.spec.nodeName",
        ], check=False).strip()
        events = run([
            "kubectl", "-n", namespace, "get", "events", "--sort-by=.lastTimestamp",
        ], check=False).strip()
        raise RuntimeError(f"{error}\n\nPod status:\n{pods}\n\nRecent events:\n{events}") from error


def best_effort_split_noisy_neighbor_sockets(namespace: str, cfg: dict[str, str], noise_documents: list[dict]) -> dict[str, object]:
    retries = max(0, int(cfg.get("NOISY_NEIGHBOR_SOCKET_SPLIT_RETRIES", "2")))
    last = {"status": "n/a", "same_socket": None}
    for attempt in range(retries + 1):
        wait_for_noisy_neighbor(namespace, cfg)
        snapshot = noisy_neighbor_snapshot(namespace, cfg)
        numa = snapshot.get("numa", {}) if isinstance(snapshot.get("numa"), dict) else {}
        status = str(numa.get("status", "n/a"))
        if status == "split":
            return numa
        last = numa
        if attempt == retries:
            break
        print(
            f"retrying noisy-neighbor placement for socket split (attempt {attempt + 2}/{retries + 1})",
            file=sys.stderr,
        )
        run([
            "kubectl", "-n", namespace, "delete", "pods", "-l", noisy_neighbor_selector(cfg),
            "--wait=true", "--ignore-not-found=true",
        ], check=False)
        apply_documents(noise_documents)
    return last


def best_effort_split_noisy_neighbor_sockets_staged(namespace: str, cfg: dict[str, str], late_noise_documents: list[dict]) -> dict[str, object]:
    retries = max(0, int(cfg.get("NOISY_NEIGHBOR_SOCKET_SPLIT_RETRIES", "2")))
    late_names = pod_names_from_documents(late_noise_documents)
    last = {"status": "n/a", "same_socket": None}
    for attempt in range(retries + 1):
        wait_for_noisy_neighbor(namespace, cfg)
        snapshot = noisy_neighbor_snapshot(namespace, cfg)
        numa = snapshot.get("numa", {}) if isinstance(snapshot.get("numa"), dict) else {}
        status = str(numa.get("status", "n/a"))
        if status == "split":
            return numa
        last = numa
        if attempt == retries:
            break
        print(
            f"retrying late noisy-neighbor placement for socket split (attempt {attempt + 2}/{retries + 1})",
            file=sys.stderr,
        )
        if late_names:
            run([
                "kubectl", "-n", namespace, "delete", "pods", *late_names,
                "--wait=true", "--ignore-not-found=true",
            ], check=False)
        apply_documents(late_noise_documents)
    return last


def cmd_render(args) -> int:
    cfg = load_config(args.scenario, overrides(args), args.noisy_neighbor or "")
    if getattr(args, "rdt_monitor", False):
        cfg["RDT_MONITOR"] = "1"
    validate_rdt_config(cfg)
    output = render(cfg, Path(args.output) if args.output else None)
    print(output)
    return 0


def cmd_run(args) -> int:
    cfg = load_config(args.scenario, overrides(args), args.noisy_neighbor or "")
    if getattr(args, "rdt_monitor", False):
        cfg["RDT_MONITOR"] = "1"
    validate_rdt_config(cfg)
    rendered = render(cfg)
    print((rendered / "README.txt").read_text(), file=sys.stderr)
    if args.dry_run:
        run(["kubectl", "apply", "--dry-run=client", "-f", str(rendered / "workload.yaml")])
        print(rendered); return 0
    namespace = cfg["LAB_NAMESPACE"]
    prometheus_process = ensure_prometheus(cfg["LAB_PROM_URL"])
    host_noise_started = False
    rdt_started = False
    rdt_start_evidence: dict = {}
    noisy_neighbor_placement_precheck: dict[str, object] = {}
    manifest_common, manifest_noise, manifest_workload = split_rendered_documents(rendered)
    try:
        # PCM runs as a host systemd service (scripts/install-pcm-host.sh), so
        # its socket/UPI view never shrinks when exclusive Pods take the CPU
        # pool. All that is needed here is proof that it is actually reporting.
        pcm_selector = pcm_metric_selector(cfg.get("NODE", cfg["LAB_DEFAULT_NODE"]))
        if is_rdt_enabled(cfg):
            print("validating host RDT/resctrl capabilities", file=sys.stderr)
            rdt_capabilities(cfg)
        if cfg.get("PLACEMENT") == "exclusive":
            print("validating PCM UPI telemetry before pinned admission", file=sys.stderr)
            wait_for_pcm_cross_numa(cfg["LAB_PROM_URL"], pcm_selector)
        if is_host_noise(cfg):
            print("starting host-wide noisy neighbor on both socket CPU pools", file=sys.stderr)
            start_host_noise(cfg)
            host_noise_started = True
        apply_documents(manifest_common)
        exporter = run([
            "kubectl", "-n", namespace, "create", "configmap", "mxl-fps-exporter",
            f"--from-file=exporter.py={ROOT / 'observability/fps_exporter.py'}", "--dry-run=client", "-o", "yaml",
        ])
        run(["kubectl", "apply", "-f", "-"], input_text=exporter)

        pod_noise_enabled = cfg.get("NOISY_NEIGHBOR_ENABLED") == "1" and not is_host_noise(cfg)
        pinned_socket_split_best_effort = (
            pod_noise_enabled
            and cfg.get("PLACEMENT") == "exclusive"
            and noisy_neighbor_pod_count(cfg) >= 2
            and not noisy_neighbor_spread_by_node(cfg)
        )

        if pinned_socket_split_best_effort:
            print("staged noisy-neighbor launch: NN-1, workload, then remaining noisy-neighbor pods", file=sys.stderr)
            first_noise = manifest_noise[:1]
            late_noise = manifest_noise[1:]
            if first_noise:
                apply_documents(first_noise)
                first_name = pod_names_from_documents(first_noise)[0]
                wait_for_named_pod_ready(namespace, first_name)
            apply_documents(manifest_workload)
            run([
                "kubectl", "-n", namespace, "wait", "--for=condition=Ready", "pod",
                f"-l=app={cfg['LAB_APP']},role in (decoder,encoder)", "--timeout=240s",
            ])
            if late_noise:
                apply_documents(late_noise)
            noisy_neighbor_placement_precheck = best_effort_split_noisy_neighbor_sockets_staged(namespace, cfg, late_noise)
            if noisy_neighbor_placement_precheck.get("status") != "split":
                print(
                    "warning: noisy-neighbor pods did not reach clean socket split after staged launch",
                    file=sys.stderr,
                )
            if cfg.get("NOISY_NEIGHBOR_REQUIRE_SOCKET_SPLIT", "0") == "1" and noisy_neighbor_placement_precheck.get("status") != "split":
                raise RuntimeError(
                    f"noisy-neighbor socket split required but status={noisy_neighbor_placement_precheck.get('status', 'n/a')}"
                )
        else:
            apply_documents(manifest_noise + manifest_workload)

        if not pinned_socket_split_best_effort:
            wait_for_workload_ready(namespace, cfg["LAB_APP"])
        if pod_noise_enabled and not pinned_socket_split_best_effort:
            wait_for_noisy_neighbor(namespace, cfg)
        if cfg.get("PLACEMENT") == "exclusive":
            print("revalidating PCM UPI telemetry after pinned admission", file=sys.stderr)
            wait_for_pcm_cross_numa(cfg["LAB_PROM_URL"], pcm_selector)
        warmup = duration(args.warmup or f"{cfg['LAB_WARMUP']}s")
        measure = duration(args.measure or f"{cfg['LAB_DURATION']}s")
        if is_rdt_enabled(cfg):
            mode = cfg.get("RDT_CONTROL_PROFILE", "none")
            print(f"starting run-scoped RDT monitoring; control={mode}", file=sys.stderr)
            rdt_start_evidence = start_rdt(cfg, namespace, cfg["LAB_APP"], warmup, measure)
            rdt_started = True
        if warmup:
            print(f"warm-up {warmup}s", file=sys.stderr); time.sleep(warmup)
        result = ROOT / cfg["LAB_RESULTS_DIR"] / f"{cfg['SCENARIO']}-{cfg['PLACEMENT']}-{cfg['STREAMS']}str-{utc_stamp()}"
        result.mkdir(parents=True)
        shutil.copy(rendered / "config.json", result / "config.json")
        shutil.copy(rendered / "placement.json", result / "planned-placement.json")
        shutil.copy(rendered / "workload.yaml", result / "workload.yaml")
        shutil.copy(rendered / "ffmpeg-commandlines.json", result / "ffmpeg-commandlines.json")
        if cfg.get("NOISY_NEIGHBOR_ENABLED") == "1":
            shutil.copy(rendered / "noisy-neighbor.json", result / "noisy-neighbor.json")
            snapshot = host_noise_snapshot(cfg) if is_host_noise(cfg) else noisy_neighbor_snapshot(namespace, cfg)
            if noisy_neighbor_placement_precheck and isinstance(snapshot, dict):
                snapshot.setdefault("numa", {})
                if isinstance(snapshot["numa"], dict):
                    snapshot["numa"].setdefault("precheck", noisy_neighbor_placement_precheck)
            dump_json(result / "noisy-neighbor-before.json", snapshot)
        capture_host(result, cfg)
        capture(result, namespace, cfg["LAB_APP"], cfg.get("NODE", cfg["LAB_DEFAULT_NODE"]), cfg["LAB_PROM_URL"], measure, int(cfg["LAB_STEP"]), float(cfg["LAB_MIN_FPS"]))
        if cfg.get("NOISY_NEIGHBOR_ENABLED") == "1":
            snapshot = host_noise_snapshot(cfg) if is_host_noise(cfg) else noisy_neighbor_snapshot(namespace, cfg)
            if noisy_neighbor_placement_precheck and isinstance(snapshot, dict):
                snapshot.setdefault("numa", {})
                if isinstance(snapshot["numa"], dict):
                    snapshot["numa"].setdefault("precheck", noisy_neighbor_placement_precheck)
            dump_json(result / "noisy-neighbor-after.json", snapshot)
            if is_host_noise(cfg):
                (result / "noisy-neighbor.log").write_text(stop_host_noise(cfg))
                host_noise_started = False
            else:
                (result / "noisy-neighbor.log").write_text(noisy_neighbor_logs(namespace, cfg))
        if rdt_started:
            rdt_stop_evidence = stop_rdt(cfg)
            rdt_started = False
            dump_json(result / "rdt-start.json", rdt_start_evidence)
            dump_json(result / "rdt-stop.json", rdt_stop_evidence)
            if not rdt_stop_evidence.get("restored"):
                raise RuntimeError(f"RDT cleanup/restoration failed: {rdt_stop_evidence}")
            append_rdt_metrics(result, collect_rdt(cfg, result))
        run(["kubectl", "-n", namespace, "get", "pods", "-o", "wide"], check=False)
        (result / "pod-describe.txt").write_text(run(["kubectl", "-n", namespace, "describe", "pods"], check=False))
        build_report(result)
        print(result)
    finally:
        if rdt_started:
            stop_rdt(cfg)
        if host_noise_started:
            stop_host_noise(cfg)
        if not args.keep:
            run(["kubectl", "delete", "namespace", namespace, "--wait=true", "--ignore-not-found=true"], check=False)
        if prometheus_process is not None:
            prometheus_process.terminate()
            try:
                prometheus_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                prometheus_process.kill()
                prometheus_process.wait(timeout=5)
    return 0


def cmd_teardown(args) -> int:
    cfg = read_env(ROOT / "config/lab.env")
    namespace = args.namespace or cfg["LAB_NAMESPACE"]
    run(["kubectl", "delete", "namespace", namespace, "--wait=true", "--ignore-not-found=true"])
    return 0


def cmd_report(args) -> int:
    build_report(Path(args.result)); return 0


def cmd_summarize(args) -> int:
    html_path, xlsx_path = build_summary(Path(args.results), args.min_fps)
    print(html_path); print(xlsx_path); return 0


def add_scenario_flags(parser) -> None:
    parser.add_argument("scenario")
    parser.add_argument("--streams", type=int)
    parser.add_argument("--node")
    parser.add_argument("--resolution", choices=["1080p", "4k"])
    parser.add_argument("--preset")
    parser.add_argument("--enc-threads", type=int)
    parser.add_argument("--dec-threads", type=int)
    parser.add_argument("--filter-threads", type=int)
    parser.add_argument("--slices", type=int)
    parser.add_argument("--dec-cores", type=int)
    parser.add_argument("--enc-cores", type=int)
    parser.add_argument("--rdt-monitor", action="store_true", help="enable run-scoped RDT CMT/MBM observability")
    parser.add_argument("--rdt-focus-session", metavar="SESSION", help="restrict RDT encoder/decoder groups to one session, such as s01")
    parser.add_argument("--rdt-control", choices=sorted(VALID_PROFILES), help="optional RDT noise-isolation policy; also enables monitoring")
    parser.add_argument("--set", action="append", default=[])
    parser.add_argument(
        "--noisy-neighbor", metavar="PROFILE", default="",
        help="run alongside noisy-neighbors/PROFILE.env: host-a (host scope, steals CPU too) "
             "or pod-a / pod-b / pod-c (Guaranteed Pods, one per socket)",
    )


def main() -> int:
    parser = argparse.ArgumentParser(prog="mxl-perf")
    commands = parser.add_subparsers(dest="command", required=True)
    render_cmd = commands.add_parser("render"); add_scenario_flags(render_cmd); render_cmd.add_argument("--output"); render_cmd.set_defaults(func=cmd_render)
    run_cmd = commands.add_parser("run"); add_scenario_flags(run_cmd)
    run_cmd.add_argument("--warmup"); run_cmd.add_argument("--measure"); run_cmd.add_argument("--dry-run", action="store_true"); run_cmd.add_argument("--keep", action="store_true"); run_cmd.set_defaults(func=cmd_run)
    teardown = commands.add_parser("teardown"); teardown.add_argument("--namespace"); teardown.set_defaults(func=cmd_teardown)
    report = commands.add_parser("report"); report.add_argument("result"); report.set_defaults(func=cmd_report)
    summary = commands.add_parser("summarize"); summary.add_argument("results"); summary.add_argument("--min-fps", type=float, default=59.5); summary.set_defaults(func=cmd_summarize)
    args = parser.parse_args()
    try:
        return args.func(args)
    except (ValueError, RuntimeError, OSError) as error:
        print(f"FATAL: {error}", file=sys.stderr); return 2


if __name__ == "__main__":
    raise SystemExit(main())
