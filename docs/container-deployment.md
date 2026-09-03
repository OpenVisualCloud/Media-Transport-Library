# Container deployment — decoder and encoder Pods

This step deploys the CBC FFmpeg-MXL container as Kubernetes Pods, following the
AMWA DMF pattern: one **decoder** Pod and one **encoder** Pod per stream, both on
the same worker node, exchanging frames through a shared MXL memory flow on
`/dev/shm/mxl`.

Before proceeding:

* The FFmpeg-MXL image must be built — see [05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md).
* The observability stack must be running — see [06-observability.md](06-observability.md).
* A test clip must be staged on the worker — see [05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md#stage-the-test-clips).

> **Troubleshooting first:** If proxy or API connectivity problems appear while
> running commands in this chapter, use
> [02-kubernetes-install-troubleshooting.md](02-kubernetes-install-troubleshooting.md).
> Set `LAB_HTTP_PROXY`/`LAB_HTTPS_PROXY` in `config/lab.env` when needed. Scripts
> construct `NO_PROXY` for cluster addresses; manually typed `kubectl` commands may
> still require shell `no_proxy`/`NO_PROXY`.

## DMF container pattern

In the DMF model a *stream* is a pair of co-located Pods:

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Worker node                                                 │
  │                                                              │
  │  ┌─────────────────────────┐  ┌──────────────────────────┐   │
  │  │  decoder Pod (s01-dec)  │  │  encoder Pod (s01-enc)   │   │
  │  │                         │  │                          │   │
  │  │  ffmpeg -f mxl ...      │  │  ffmpeg -f mxl -i ...    │   │
  │  │  /dev/shm/mxl/<id>.flow │  │  /dev/shm/mxl/<id>.flow  │   │
  │  │                         │  │                          │   │
  │  │  FPS sidecar :9101      │  │  FPS sidecar :9101       │   │
  │  └──────────────────────┬──┘  └──▲───────────────────────┘   │
  │                         │        │                           │
  │        hostPath /dev/shm/mxl (tmpfs, shared)                 │
  └──────────────────────────────────────────────────────────────┘
```

The `hostPath` mount on `/dev/shm/mxl` is the MXL shared-memory bus. It is
zero-copy: the decoder writes a `v210` flow file and the encoder reads it directly
from memory with no network, no serialisation and no copy.

## Pod specifications

The resource requests and limits below are for the `pinned` scenario (Guaranteed
QoS class, exclusive cores). For the `baseline` or `numa-pool` scenarios, set the
CPU requests to `500m` with no CPU limits — a missing CPU limit produces a
Burstable QoS class, which is what both scenarios require. Memory requests and
limits can remain as shown.

### Decoder Pod

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: s01-dec
  namespace: mxl-perf
  labels:
    stream: s01
    role: decoder
spec:
  nodeName: worker-1          # pin to the measured worker
  restartPolicy: Never
  volumes:
    - name: mxl-shm
      hostPath:
        path: /dev/shm/mxl
        type: DirectoryOrCreate
    - name: media
      hostPath:
        path: /opt/mxl-media
        type: Directory
  containers:
    - name: decoder
      image: localhost/mxl-ffmpeg-full:v1
      imagePullPolicy: Never
      command:
        - /opt/bin/ffmpeg
        - -stream_loop
        - "-1"
        - -re
        - -i
        - /media/Mixed_40sec_1920x1080_60fps_8bit_420_crf23_veryslow.mp4
        - -c:v
        - v210
        - -progress
        - /run/mxl/progress
        - -f
        - mxl
        - -video_flow_id
        - "1001"
        - /dev/shm/mxl
      resources:
        requests:
          cpu: "1"
          memory: "2Gi"
        limits:
          cpu: "1"
          memory: "2Gi"
      volumeMounts:
        - name: mxl-shm
          mountPath: /dev/shm/mxl
        - name: media
          mountPath: /media
          readOnly: true
    - name: fps-sidecar
      image: localhost/mxl-ffmpeg-full:v1
      imagePullPolicy: Never
      command:
        - python3
        - /opt/fps_exporter.py
        - /run/mxl/progress
      ports:
        - containerPort: 9101
      resources:
        requests:
          cpu: "5m"
          memory: "32Mi"
```

### Encoder Pod

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: s01-enc
  namespace: mxl-perf
  labels:
    stream: s01
    role: encoder
spec:
  nodeName: worker-1
  restartPolicy: Never
  volumes:
    - name: mxl-shm
      hostPath:
        path: /dev/shm/mxl
        type: DirectoryOrCreate
  containers:
    - name: encoder
      image: localhost/mxl-ffmpeg-full:v1
      imagePullPolicy: Never
      command:
        - /opt/bin/ffmpeg
        - -f
        - mxl
        - -grain_index_init
        - "1"
        - -on_too_late
        - "0"
        - -i
        - /dev/shm/mxl/1001.mxl-flow
        - -threads
        - "15"
        - -vf
        - format=yuv420p
        - -c:v
        - libx264
        - -preset
        - medium
        - -tune
        - zerolatency
        - -bf
        - "0"
        - -rc-lookahead
        - "0"
        - -g
        - "30"
        - -sc_threshold
        - "0"
        - -x264-params
        - sliced-threads=0:slices=2:threads=15
        - -maxrate
        - 12M
        - -bufsize
        - 1M
        - -progress
        - /run/mxl/progress
        - -f
        - null
        - "-"
      resources:
        requests:
          cpu: "5"
          memory: "4Gi"
        limits:
          cpu: "5"
          memory: "4Gi"
      volumeMounts:
        - name: mxl-shm
          mountPath: /dev/shm/mxl
    - name: fps-sidecar
      image: localhost/mxl-ffmpeg-full:v1
      imagePullPolicy: Never
      command:
        - python3
        - /opt/fps_exporter.py
        - /run/mxl/progress
      ports:
        - containerPort: 9101
      resources:
        requests:
          cpu: "5m"
          memory: "32Mi"
```

## Deploying with the runner

The `mxlperf` runner generates and applies manifests automatically, resolving CPU
placement, NUMA alignment and session numbering:

```bash
scripts/setup.sh                              # creates .venv and the mxl-perf command
scripts/preflight.sh                          # verify the cluster is ready

# Deploy a single pinned stream and leave it running for inspection
scripts/run.sh pinned --streams 1 --keep --warmup 30s --measure 1m
```

With `--keep` the Pods stay up after the run completes. Inspect them:

```bash
kubectl -n mxl-perf get pods -o wide          # both Pods on the same node
kubectl -n mxl-perf describe pod s01-dec      # cpuset, resource allocation
kubectl -n mxl-perf logs s01-enc -c encoder   # FFmpeg output
```

For a manual dry run that shows the exact YAML the runner would apply:

```bash
scripts/run.sh pinned --streams 1 --dry-run
```

The generated `workload.yaml` is saved with every run result under
`results/<run-dir>/workload.yaml`.

## QoS classes and placement

| Scenario | Decoder CPU request | Encoder CPU request | Kubernetes QoS class | Core allocation |
|---|---|---|---|---|
| `baseline` | `500m` | `500m` | Burstable | shared pool, scheduler decides |
| `numa-pool` | `500m` | `500m` | Burstable | shared pool, `taskset` to one socket |
| `pinned` | `1` (whole number) | `5` (whole number) | **Guaranteed** | exclusive cores from kubelet |

Only `pinned` requires the static CPU Manager from [03-cpu-qos.md](03-cpu-qos.md).
`baseline` and `numa-pool` deploy on any Kubernetes cluster.

## Verify a deployed stream

```bash
# Stream is live and holding FPS
kubectl -n mxl-perf get pods
# Both s01-dec and s01-enc should be Running

# FPS from the sidecar (requires port-forward-prometheus.sh)
curl -s http://127.0.0.1:19090/api/v1/query \
  --data-urlencode 'query=mxl_ffmpeg_fps{role="encoder"}' | python3 -m json.tool

# In pinned mode: confirm exclusive cores were assigned
kubectl -n mxl-perf describe pod s01-enc | grep -A2 "Limits\|Requests\|cpu"
```

## Teardown

```bash
kubectl delete namespace mxl-perf            # removes all stream Pods
scripts/teardown.sh                          # runner-managed teardown
```

Next: [08-profiling-manifest.md](08-profiling-manifest.md) — profile the running
containers and generate a DMF-CRM deployment manifest.
