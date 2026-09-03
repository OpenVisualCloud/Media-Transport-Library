from __future__ import annotations

import shlex
from pathlib import Path
from typing import Any

import yaml

from . import topology
from .common import ROOT, compact_cpus, dump_json, utc_stamp

SIDECAR_CPU_REQUEST = "5m"


def cpu_pools(cfg: dict[str, str]) -> list[list[int]]:
    return topology.cpu_pools(topology.from_config(cfg))


def plan_cpu_sets(cfg: dict[str, str]) -> list[dict[str, Any]]:
    streams = int(cfg["STREAMS"])
    mode = cfg["PLACEMENT"]
    if mode == "free":
        return [{"session": i, "socket": "scheduler", "decoder": [], "encoder": []} for i in range(1, streams + 1)]
    pools = cpu_pools(cfg)
    if mode == "numa-pool":
        return [
            {"session": i, "socket": (i - 1) % 2, "decoder": pools[(i - 1) % 2], "encoder": pools[(i - 1) % 2]}
            for i in range(1, streams + 1)
        ]
    if mode != "exclusive":
        raise ValueError(f"PLACEMENT must be free, exclusive, or numa-pool: {mode}")
    dec, enc = int(cfg["DEC_CORES"]), int(cfg["ENC_CORES"])
    enc_threads = int(cfg.get("ENC_THREADS") or enc)
    slices = int(cfg.get("SLICES") or enc_threads)
    sliced_threads = cfg.get("SLICED_THREADS", "1") == "1"
    if sliced_threads and slices < enc_threads:
        raise ValueError(
            f"SLICES={slices} limits sliced-thread parallelism below ENC_THREADS={enc_threads}; "
            "use SLICES >= ENC_THREADS or SLICED_THREADS=0 for frame threading"
        )
    # DEC_CORES and ENC_CORES are physical cores, so capacity is counted in cores:
    # with SMT enabled a whole core costs threads-per-core logical CPUs, and only
    # fully-free sibling groups count, because under full-pcpus-only a core with
    # one sibling reserved cannot be handed out exclusively at all.
    topo = topology.from_config(cfg)
    threads = topo.threads_per_core
    if topology.full_pcpus_only(cfg):
        partial = topology.partially_reserved_cores(topo)
        if partial:
            raise ValueError(
                f"LAB_RESERVED_CPUS reserves only part of core(s) {compact_cpus(partial)}; "
                "under full-pcpus-only the free sibling of a reserved thread can never be "
                "handed out exclusively, so those cores are lost. Reserve whole cores "
                f"(the sibling of CPU N is N+{topo.physical_cores}), or set "
                "LAB_FULL_PCPUS_ONLY=false to allow thread-granular packing"
            )
        allocatable_cpus = sum(len(groups) for groups in topology.core_pools(topo)) * threads
    else:
        # Threads are handed out individually, so a half-reserved core still
        # contributes its free sibling.
        allocatable_cpus = sum(len(pool) for pool in topology.cpu_pools(topo))
    workload_cores = streams * (dec + enc)
    workload_cpus = workload_cores * threads
    sidecar_cpus = streams * 2 * int(SIDECAR_CPU_REQUEST.removesuffix("m")) / 1000
    if workload_cpus + sidecar_cpus > allocatable_cpus:
        cpus_per_stream = (dec + enc) * threads + 2 * int(SIDECAR_CPU_REQUEST.removesuffix("m")) / 1000
        maximum_streams = int(allocatable_cpus // cpus_per_stream)
        raise ValueError(
            f"Kubernetes CPU Manager placement needs {workload_cores:g} exclusive cores "
            f"({workload_cpus:g} CPUs at {threads} thread(s) per core) plus "
            f"{sidecar_cpus:g} shared CPUs, but only {allocatable_cpus / threads:g} cores "
            f"({allocatable_cpus} CPUs) are allocatable; "
            f"at most {maximum_streams} streams fit this topology"
        )
    return [
        {"session": session, "socket": "kubelet", "decoder": [], "encoder": []}
        for session in range(1, streams + 1)
    ]


def _exclusive_cpu_request(cfg: dict[str, str], cores_key: str) -> str:
    """Whole physical cores, expressed as the CPU request the kubelet needs."""
    return topology.cpu_request_for_cores(topology.from_config(cfg), int(cfg[cores_key]))


def resources(cpu: str, memory: str, guaranteed: bool = False) -> dict[str, Any]:
    result = {"requests": {"cpu": str(cpu), "memory": memory}, "limits": {"memory": memory}}
    if guaranteed:
        result["limits"]["cpu"] = str(cpu)
    return result


def taskset(cpus: list[int]) -> str:
    return f"taskset -c {','.join(map(str, cpus))} " if cpus else ""


def sidecar(role: str, guaranteed: bool = False) -> dict[str, Any]:
    container = {
        "name": "fps-exporter",
        "image": "python:3-slim",
        "command": ["python3", "/app/exporter.py"],
        "env": [
            {"name": "ROLE", "value": role},
            {"name": "PROGRESS_FILE", "value": "/run/mxl/progress"},
            {"name": "POD", "valueFrom": {"fieldRef": {"fieldPath": "metadata.name"}}},
        ],
        "ports": [{"name": "metrics", "containerPort": 9101}],
        "volumeMounts": [
            {"name": "progress", "mountPath": "/run/mxl", "readOnly": True},
            {"name": "exporter", "mountPath": "/app", "readOnly": True},
        ],
        "resources": {"requests": {"memory": "32Mi"}, "limits": {"memory": "64Mi"}},
    }
    if guaranteed:
        container["resources"] = {
            "requests": {"cpu": SIDECAR_CPU_REQUEST, "memory": "64Mi"},
            "limits": {"cpu": SIDECAR_CPU_REQUEST, "memory": "64Mi"},
        }
    return container


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


def noisy_neighbor_cpu_request(cfg: dict[str, str]) -> str:
    """The neighbor's effective CPU request.

    A Guaranteed neighbor gets exclusive cores from the static CPU Manager, so an
    integer NOISY_NEIGHBOR_CPU_REQUEST counts *physical cores* - same currency as
    DEC_CORES and ENC_CORES - and costs threads-per-core CPUs. That keeps the
    neighbor's cache and bandwidth footprint identical with SMT on and off, and
    keeps the request a multiple of threads-per-core so full-pcpus-only admits it.
    Burstable neighbors ask in millicores and are passed through untouched.
    """
    request = cfg.get("NOISY_NEIGHBOR_CPU_REQUEST", "10m")
    if cfg.get("NOISY_NEIGHBOR_GUARANTEED", "0") != "1" or not request.isdigit():
        return request
    return topology.cpu_request_for_cores(topology.from_config(cfg), int(request))


def noisy_neighbor_pods(cfg: dict[str, str]) -> list[dict[str, Any]]:
    if cfg.get("NOISY_NEIGHBOR_SCOPE", "pod") != "pod":
        raise ValueError("Kubernetes noisy-neighbor Pod requires NOISY_NEIGHBOR_SCOPE=pod")
    count = noisy_neighbor_pod_count(cfg)
    args = shlex.split(cfg["NOISY_NEIGHBOR_ARGS"])
    if not args:
        raise ValueError("NOISY_NEIGHBOR_ARGS must not be empty")
    forbidden = {"--all", "-a", "--sequential", "--seq", "--permute", "--class"}
    if forbidden.intersection(args):
        raise ValueError(
            "NOISY_NEIGHBOR_ARGS may not use broad stressor selectors; choose explicit bounded stressors"
        )
    memory_limit = cfg.get("NOISY_NEIGHBOR_MEMORY_LIMIT", "")
    if not memory_limit:
        raise ValueError("NOISY_NEIGHBOR_MEMORY_LIMIT is required")
    guaranteed = cfg.get("NOISY_NEIGHBOR_GUARANTEED", "0") == "1"
    cpu_request = noisy_neighbor_cpu_request(cfg)
    memory_request = cfg.get("NOISY_NEIGHBOR_MEMORY_REQUEST", "64Mi")
    resources = {
        "requests": {"cpu": cpu_request, "memory": memory_request},
        "limits": {"memory": memory_limit},
    }
    if guaranteed:
        resources = {
            "requests": {"cpu": cpu_request, "memory": memory_limit},
            "limits": {"cpu": cpu_request, "memory": memory_limit},
        }
    labels = {
        "app": f"{cfg['LAB_APP']}-noise",
        "scenario": cfg["SCENARIO"],
        "role": "noisy-neighbor",
    }
    worker_nodes = [node.strip() for node in cfg.get("LAB_WORKERS", "").split(",") if node.strip()]
    spread_by_node = noisy_neighbor_spread_by_node(cfg)
    pods = []
    for index in range(count):
        name = "mxl-noisy-neighbor" if count == 1 else f"mxl-noisy-neighbor-{index + 1}"
        pod_labels = dict(labels)
        pod_labels["noise-instance"] = str(index + 1)
        spec: dict[str, Any] = {
            "restartPolicy": "Never",
            "terminationGracePeriodSeconds": 10,
            "securityContext": {"runAsNonRoot": True, "runAsUser": 65534, "runAsGroup": 65534, "seccompProfile": {"type": "RuntimeDefault"}},
            "containers": [{
                "name": "stress-ng",
                "image": cfg["NOISY_NEIGHBOR_IMAGE"],
                "imagePullPolicy": cfg.get("NOISY_NEIGHBOR_IMAGE_PULL_POLICY", "IfNotPresent"),
                "workingDir": "/tmp",
                "command": ["stress-ng"],
                "args": args,
                "securityContext": {
                    "allowPrivilegeEscalation": False,
                    "capabilities": {"drop": ["ALL"]},
                    "readOnlyRootFilesystem": True,
                },
                "resources": resources,
                "volumeMounts": [{"name": "tmp", "mountPath": "/tmp"}],
            }],
            "volumes": [{"name": "tmp", "emptyDir": {}}],
        }
        if spread_by_node:
            affinity: dict[str, Any] = {
                "podAntiAffinity": {
                    "requiredDuringSchedulingIgnoredDuringExecution": [{
                        "labelSelector": {
                            "matchLabels": {
                                "app": labels["app"],
                                "role": labels["role"],
                            },
                        },
                        "topologyKey": "kubernetes.io/hostname",
                    }],
                },
            }
            if worker_nodes:
                affinity["nodeAffinity"] = {
                    "requiredDuringSchedulingIgnoredDuringExecution": {
                        "nodeSelectorTerms": [{
                            "matchExpressions": [{
                                "key": "kubernetes.io/hostname",
                                "operator": "In",
                                "values": worker_nodes,
                            }],
                        }],
                    },
                }
            spec["affinity"] = affinity
        else:
            spec["nodeSelector"] = {"kubernetes.io/hostname": cfg["NODE"]}

        pods.append({
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": name,
                "namespace": cfg["LAB_NAMESPACE"],
                "labels": pod_labels,
                "annotations": {
                    "mxl-perf/profile": cfg["NOISY_NEIGHBOR_PROFILE"],
                    "mxl-perf/arguments": cfg["NOISY_NEIGHBOR_ARGS"],
                    "mxl-perf/noise-instance": str(index + 1),
                },
            },
            "spec": spec,
        })
    return pods


def noisy_neighbor_pod(cfg: dict[str, str]) -> dict[str, Any]:
    # Backward-compatible helper used by existing tests and callers.
    return noisy_neighbor_pods(cfg)[0]



def resolved_ffmpeg_command(container: dict[str, Any]) -> str:
    command = container["args"][0].splitlines()[-1]
    if "exec " in command:
        command = command.rsplit("exec ", 1)[-1]
    env = {item["name"]: item.get("value", "") for item in container.get("env", []) if "value" in item}
    for name, value in sorted(env.items(), key=lambda item: -len(item[0])):
        command = command.replace(f'"${name}"', shlex.quote(value))
        command = command.replace(f"${name}", shlex.quote(value))
    return command


def pod_spec(cfg: dict[str, str], placement: dict[str, Any], role: str) -> dict[str, Any]:
    session = placement["session"]
    sid = f"s{session:02d}"
    video_id = f"c4de{session:04d}-0000-4000-8000-{session:012x}"
    cpus = placement[role]
    guaranteed = cfg["PLACEMENT"] == "exclusive"
    affinity = taskset(cpus) if cfg["PLACEMENT"] == "numa-pool" else ""
    threads = cfg.get("DEC_THREADS" if role == "decoder" else "ENC_THREADS", "")
    thread_args = f"-threads {threads} " if threads else ""
    filter_threads = (
        cfg.get("DEC_FILTER_THREADS", cfg.get("FILTER_THREADS", ""))
        if role == "decoder"
        else cfg.get("ENC_FILTER_THREADS", "")
    )
    filter_args = f"-filter_threads {filter_threads} " if filter_threads else ""
    if role == "decoder":
        command = (
            f'rm -f "$MXL_DOMAIN/$VIDEO_ID.mxl-flow"; '
            f'exec {affinity}$FFMPEG -hide_banner -loglevel info {filter_args}{thread_args}'
            f'-stream_loop -1 -re -i "$MEDIA_DIR/$INPUT_FILE" -c:v v210 '
            f'-progress /run/mxl/progress -f mxl -video_flow_id "$VIDEO_ID" "$MXL_DOMAIN"'
        )
        cpu_request = _exclusive_cpu_request(cfg, "DEC_CORES") if guaranteed else cfg["DEC_CPU_REQUEST"]
        memory = cfg["DEC_MEMORY"]
    else:
        x264 = ""
        if cfg.get("SLICES"):
            sliced_threads = cfg.get("SLICED_THREADS", "1")
            x264 = f"-x264-params sliced-threads={sliced_threads}:slices={cfg['SLICES']}"
            if threads:
                x264 += f":threads={threads}"
            if cfg.get("X264_EXTRA"):
                x264 += f":{cfg['X264_EXTRA']}"
            x264 += " "
        command = (
            f'for attempt in $(seq 1 120); do [[ -e "$MXL_DOMAIN/$VIDEO_ID.mxl-flow" ]] && break; sleep 1; done; '
            f'[[ -e "$MXL_DOMAIN/$VIDEO_ID.mxl-flow" ]] || {{ echo "MXL flow did not appear" >&2; exit 1; }}; '
            f'exec {affinity}$FFMPEG -hide_banner -loglevel info {filter_args}-f mxl '
            f'-grain_index_init 1 -on_too_late 0 -i "$MXL_DOMAIN/$VIDEO_ID.mxl-flow" {thread_args}'
            f'-vf format=yuv420p -c:v libx264 -preset "$PRESET" -tune zerolatency '
            f'-bf 0 -rc-lookahead 0 -g 30 -sc_threshold 0 {x264}'
            f'-maxrate "$BITRATE" -bufsize 1M -progress /run/mxl/progress -f null -'
        )
        cpu_request = _exclusive_cpu_request(cfg, "ENC_CORES") if guaranteed else cfg["ENC_CPU_REQUEST"]
        memory = cfg["ENC_MEMORY"]
    env = [
        {"name": "VIDEO_ID", "value": video_id},
        {"name": "FFMPEG", "value": cfg["LAB_FFMPEG"]},
        {"name": "MEDIA_DIR", "value": cfg["LAB_MEDIA_MOUNT"]},
        {"name": "MXL_DOMAIN", "value": cfg["LAB_MXL_MOUNT"]},
        {"name": "INPUT_FILE", "value": cfg["INPUT_FILE"]},
        {"name": "PRESET", "value": cfg["PRESET"]},
        {"name": "BITRATE", "value": cfg["BITRATE"]},
    ]
    main = {
        "name": role,
        "image": cfg["LAB_IMAGE"],
        "imagePullPolicy": cfg["LAB_IMAGE_PULL_POLICY"],
        "command": ["/usr/bin/env", "bash", "-c"],
        "args": [f"set -euo pipefail\nmkdir -p \"$MXL_DOMAIN\"\n{command}"],
        "env": env,
        "resources": resources(cpu_request, memory, guaranteed),
        "volumeMounts": [
            {"name": "mxl", "mountPath": cfg["LAB_MXL_MOUNT"]},
            {"name": "progress", "mountPath": "/run/mxl"},
        ],
    }
    if role == "decoder":
        main["volumeMounts"].append({"name": "media", "mountPath": cfg["LAB_MEDIA_MOUNT"], "readOnly": True})
    labels = {"app": cfg["LAB_APP"], "scenario": cfg["SCENARIO"], "session": sid, "role": role}
    volumes = [
        {"name": "mxl", "hostPath": {"path": cfg["LAB_MXL_HOSTPATH"], "type": "DirectoryOrCreate"}},
        {"name": "progress", "emptyDir": {}},
        {"name": "exporter", "configMap": {"name": "mxl-fps-exporter"}},
    ]
    if role == "decoder":
        volumes.append({"name": "media", "hostPath": {"path": cfg["LAB_MEDIA_HOSTPATH"], "type": "DirectoryOrCreate"}})
    return {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": f"mxl-{role}-{sid}", "namespace": cfg["LAB_NAMESPACE"], "labels": labels},
        "spec": {
            "nodeSelector": {"kubernetes.io/hostname": cfg["NODE"]},
            "restartPolicy": "Never",
            "containers": [main, sidecar(role, guaranteed)],
            "volumes": volumes,
        },
    }


def render(cfg: dict[str, str], output: Path | None = None) -> Path:
    cfg = dict(cfg)
    cfg["NODE"] = cfg.get("NODE", cfg["LAB_DEFAULT_NODE"])
    cfg["INPUT_FILE"] = cfg.get("INPUT_FILE") or cfg[f"LAB_INPUT_{cfg['RESOLUTION'].upper()}"]
    if cfg["PLACEMENT"] == "exclusive":
        # Integer request=limit makes the main container eligible for static CPU Manager cores.
        # With SMT on, a whole core costs threads-per-core CPUs, and full-pcpus-only
        # admits the container only when the request is that multiple.
        cfg["DEC_CPU_REQUEST"] = _exclusive_cpu_request(cfg, "DEC_CORES")
        cfg["ENC_CPU_REQUEST"] = _exclusive_cpu_request(cfg, "ENC_CORES")
    output = output or ROOT / ".rendered" / f"{cfg['SCENARIO']}-{utc_stamp()}"
    output.mkdir(parents=True, exist_ok=False)
    placements = plan_cpu_sets(cfg)
    documents: list[dict[str, Any]] = [
        {"apiVersion": "v1", "kind": "Namespace", "metadata": {"name": cfg["LAB_NAMESPACE"]}},
        {
            "apiVersion": "monitoring.coreos.com/v1",
            "kind": "PodMonitor",
            "metadata": {"name": "mxl-workloads", "namespace": cfg["LAB_NAMESPACE"], "labels": {"release": cfg["LAB_PROM_RELEASE"]}},
            "spec": {
                "selector": {"matchLabels": {"app": cfg["LAB_APP"]}},
                "podMetricsEndpoints": [{"port": "metrics", "interval": "5s"}],
            },
        },
    ]
    noise_enabled = cfg.get("NOISY_NEIGHBOR_ENABLED") == "1"
    noise_scope = cfg.get("NOISY_NEIGHBOR_SCOPE", "pod")
    if noise_enabled and noise_scope not in ("pod", "host"):
        raise ValueError(f"NOISY_NEIGHBOR_SCOPE must be pod or host: {noise_scope}")
    if noise_enabled and noise_scope == "pod":
        documents.extend(noisy_neighbor_pods(cfg))
    for placement in placements:
        documents.extend([pod_spec(cfg, placement, "decoder"), pod_spec(cfg, placement, "encoder")])
    (output / "workload.yaml").write_text(yaml.safe_dump_all(documents, sort_keys=False))
    command_lines = []
    for document in documents:
        if document.get("kind") != "Pod":
            continue
        container = document["spec"]["containers"][0]
        if document["metadata"]["labels"].get("role") == "noisy-neighbor":
            continue
        command_lines.append({
            "pod": document["metadata"]["name"],
            "role": document["metadata"]["labels"]["role"],
            "command": resolved_ffmpeg_command(container),
        })
    dump_json(output / "ffmpeg-commandlines.json", command_lines)
    if noise_enabled:
        noise_metadata = {
            "enabled": True,
            "profile": cfg["NOISY_NEIGHBOR_PROFILE"],
            "scope": noise_scope,
            "image": cfg["NOISY_NEIGHBOR_IMAGE"],
            "args": shlex.split(cfg["NOISY_NEIGHBOR_ARGS"]),
            "node": cfg["NODE"],
        }
        if noise_scope == "pod":
            pod_count = noisy_neighbor_pod_count(cfg)
            noise_metadata.update({
                "cpu_request": noisy_neighbor_cpu_request(cfg),
                "cpu_cores_requested": cfg.get("NOISY_NEIGHBOR_CPU_REQUEST"),
                "memory_request": cfg.get("NOISY_NEIGHBOR_MEMORY_REQUEST"),
                "memory_limit": cfg.get("NOISY_NEIGHBOR_MEMORY_LIMIT"),
                "qos": "Burstable",
                "pod_count": pod_count,
                "pod_names": [
                    "mxl-noisy-neighbor" if pod_count == 1 else f"mxl-noisy-neighbor-{index + 1}"
                    for index in range(pod_count)
                ],
            })
        else:
            noise_metadata.update({
                "stressor": cfg.get("NOISY_NEIGHBOR_HOST_STRESSOR"),
                "workers_per_socket": cfg.get("NOISY_NEIGHBOR_HOST_WORKERS_PER_SOCKET"),
                "extra_args": shlex.split(cfg.get("NOISY_NEIGHBOR_HOST_EXTRA_ARGS", "")),
                "execution": "SSH host process outside kubepods; one affinity-constrained stress-ng process per socket",
            })
        dump_json(output / "noisy-neighbor.json", noise_metadata)
    dump_json(output / "config.json", cfg)
    dump_json(output / "placement.json", placements)
    lines = [
        f"scenario={cfg['SCENARIO']} placement={cfg['PLACEMENT']} node={cfg['NODE']} streams={cfg['STREAMS']}",
        f"resolution={cfg['RESOLUTION']} preset={cfg['PRESET']} threads={cfg.get('ENC_THREADS','auto')} slices={cfg.get('SLICES','auto')}",
    ]
    if noise_enabled:
        lines.append(
            f"noisy-neighbor={cfg['NOISY_NEIGHBOR_PROFILE']} scope={noise_scope} image={cfg['NOISY_NEIGHBOR_IMAGE']} "
            f"args={cfg['NOISY_NEIGHBOR_ARGS']}"
        )
    for p in placements:
        if cfg["PLACEMENT"] == "exclusive":
            lines.append(
                f"s{p['session']:02d} socket=kubelet "
                f"decoder={cfg['DEC_CORES']} cores ({cfg['DEC_CPU_REQUEST']} CPUs) "
                f"encoder={cfg['ENC_CORES']} cores ({cfg['ENC_CPU_REQUEST']} CPUs)"
            )
        else:
            lines.append(
                f"s{p['session']:02d} socket={p['socket']} decoder={compact_cpus(p['decoder'])} encoder={compact_cpus(p['encoder'])}"
            )
    (output / "README.txt").write_text("\n".join(lines) + "\n")
    return output
