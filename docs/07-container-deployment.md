# 7. Container deployment — decoder and encoder Pods

This step deploys the CBC FFmpeg-MXL container as Kubernetes Pods, following the
AMWA DMF pattern: one **decoder** Pod and one **encoder** Pod per stream, both on
the same worker node, exchanging frames through a shared MXL memory flow on
`/dev/shm/mxl`.

Before proceeding:

* The FFmpeg-MXL image must be built — see [05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md).
* The observability stack must be running — see [06-observability.md](06-observability.md).
* A test clip must be staged on the worker — see [05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md#stage-the-test-clips).

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

To learn how to modify scenario values, CPU/memory settings, or the underlying Pod
templates, see [Configuring YAML and scenario values](#configuring-yaml-and-scenario-values).

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

`scripts/run.sh` is the authoritative deployment path. It wraps the `mxl-perf`
command, which renders templates, resolves CPU placement and NUMA alignment, creates
the `mxl-perf` namespace, and applies the generated manifests with `kubectl apply`.

### Recommended workflow

Follow these steps in order for every deployment:

```bash
# Step 1 — install the mxl-perf command into a local virtualenv (once per machine)
scripts/setup.sh

# Step 2 — verify the cluster is ready before deploying anything
scripts/preflight.sh

# Step 3 — validate the manifests without touching the cluster (dry-run)
scripts/run.sh baseline --streams 1 --dry-run    # baseline stream
scripts/run.sh pinned   --streams 1 --dry-run    # pinned stream

# Step 4 — deploy a real, running workload
scripts/run.sh baseline --streams 1 --keep       # baseline: one decoder + one encoder Pod
scripts/run.sh pinned   --streams 1 --keep       # pinned:   one decoder + one encoder Pod

# Step 5 — after you have finished inspecting Pods and collecting measurements,
#           tear down the workload to prevent stale Pods from causing
#           immutable-Pod update errors when you switch scenarios
scripts/teardown.sh

# If the namespace itself should also be removed (e.g. before a clean campaign run):
kubectl delete namespace mxl-perf --ignore-not-found --wait=true
```

### What each stream creates

Each `--streams 1` invocation creates exactly **two Pods** in the `mxl-perf` namespace:

| Pod name | Container | Role |
|---|---|---|
| `s01-dec` | `decoder` + `fps-sidecar` | reads the media file, writes an MXL flow |
| `s01-enc` | `encoder` + `fps-sidecar` | reads the MXL flow, encodes to H.264 |

Both Pods are scheduled on the same worker node and share `/dev/shm/mxl` via a
`hostPath` volume.  Adding `--streams 3` produces three pairs: `s01-dec`/`s01-enc`,
`s02-dec`/`s02-enc`, `s03-dec`/`s03-enc`.

### `--dry-run` versus a real run

| Flag | Effect |
|---|---|
| `--dry-run` | Renders manifests and runs `kubectl apply --dry-run=client`; **no Pods are created**; lets you inspect the YAML before applying |
| *(no flag)* | Renders manifests and runs `kubectl apply`; **Pods are created and start immediately** |

### `--keep` — leave Pods running for inspection

Without `--keep` the runner deletes the Pods when the measurement phase ends.
With `--keep` the Pods remain in the `mxl-perf` namespace after the run script
exits, so you can inspect them with `kubectl`.

```bash
# Baseline: deploy one stream and leave it running
scripts/run.sh baseline --streams 1 --keep

# Pinned: deploy one stream with exclusive cores and leave it running
scripts/run.sh pinned --streams 1 --keep
```

### Manual apply from the generated workload.yaml

Every real run (without `--dry-run`) saves the generated manifests to:

```
results/<run-dir>/workload.yaml
```

You can re-apply that file directly if the namespace and config map already exist:

```bash
kubectl apply -f results/<run-dir>/workload.yaml
```

> **Note:** `workload.yaml` contains only the Pod objects for the stream. The
> namespace, config map, and sidecar resources are separate and are created
> automatically by the runner before Pods are applied. Use `scripts/run.sh` as
> the authoritative path; the manual `kubectl apply` above is for re-applying or
> inspecting a previously generated manifest.

## Configuring YAML and scenario values

Pod manifests are generated from two sources of configuration that serve
different purposes. Understanding the distinction makes it easy to customise a
run without touching both.

### Scenario-specific settings — `scenarios/`

Each file in `scenarios/` (e.g. `baseline.env`, `numa-pool.env`, `pinned.env`)
is a shell-style key=value file that describes **one performance scenario**.
These values change the scheduling and resource policy of the Pods:

| Variable | Effect on the generated YAML |
|---|---|
| `DEC_CPU_REQUEST` / `ENC_CPU_REQUEST` | `resources.requests.cpu` for the decoder / encoder container |
| `DEC_CORES` / `ENC_CORES` | `resources.limits.cpu` (only set when `PLACEMENT=exclusive`; omitting the limit produces Burstable QoS) |
| `DEC_MEMORY` / `ENC_MEMORY` | `resources.requests.memory` and `resources.limits.memory` |
| `ENC_THREADS` / `DEC_THREADS` | FFmpeg `-threads` flag passed in the container `command` |
| `SLICES` / `SLICED_THREADS` | FFmpeg `-x264-params slices=…:sliced-threads=…` |
| `PLACEMENT` | Runner placement strategy (`free`, `numa-pool`, or `exclusive`) |
| `STREAMS` | Number of decoder+encoder Pod pairs to deploy |
| `BITRATE` / `PRESET` / `RESOLUTION` | FFmpeg encoding parameters passed in the container `command` |

**To create a new scenario**, copy an existing `.env` file, change the values
you care about, and pass the scenario name to the runner:

```bash
cp scenarios/pinned.env scenarios/my-scenario.env
# Edit my-scenario.env …
scripts/run.sh my-scenario --streams 2 --warmup 30s --measure 2m
```

**Key differences between the built-in scenarios:**

* `baseline.env` — `DEC_CPU_REQUEST=500m`, `ENC_CPU_REQUEST=500m`, no CPU
  limits → **Burstable** QoS, free scheduler placement.
* `numa-pool.env` — same CPU requests as baseline (Burstable), but the runner
  pins each session to the CPUs of a single NUMA socket via `taskset`.
* `pinned.env` — integer CPU requests equal to `DEC_CORES`/`ENC_CORES` limits
  → **Guaranteed** QoS; the kubelet static CPU Manager assigns exclusive cores.

### Pod YAML fields adjusted per scenario

The table below summarises which fields in the Pod spec change between scenarios
and what drives each value:

| Pod YAML field | Scenario variable | `baseline` | `numa-pool` | `pinned` |
|---|---|---|---|---|
| `resources.requests.cpu` (decoder) | `DEC_CPU_REQUEST` | `500m` | `500m` | `1` |
| `resources.limits.cpu` (decoder) | `DEC_CORES` | *(absent)* | *(absent)* | `1` |
| `resources.requests.cpu` (encoder) | `ENC_CPU_REQUEST` | `500m` | `500m` | `5` |
| `resources.limits.cpu` (encoder) | `ENC_CORES` | *(absent)* | *(absent)* | `5` |
| `resources.requests.memory` | `DEC_MEMORY` / `ENC_MEMORY` | `2Gi` / `1Gi` | `2Gi` / `1Gi` | `2Gi` / `1Gi` |
| FFmpeg `-threads` (encoder) | `ENC_THREADS` | `15` | `15` | `15` |
| FFmpeg `-x264-params slices=…` | `SLICES` | `2` | `2` | `2` |
| FFmpeg `-x264-params sliced-threads=…` | `SLICED_THREADS` | *(absent)* | *(absent)* | `0` |

Fields that are **absent** in a scenario are simply not included in the
generated manifest; a missing CPU limit is what allows Kubernetes to assign
**Burstable** rather than **Guaranteed** QoS.

All other Pod fields (image, `nodeName`, `hostPath` volumes, sidecar ports,
`restartPolicy`) are **fixed** across all scenarios and are set directly in the
runner templates. Edit `scripts/run.sh` or the template directory if you need
to change those values.

## QoS classes and placement

| Scenario | Decoder CPU request | Encoder CPU request | Kubernetes QoS class | Core allocation |
|---|---|---|---|---|
| `baseline` | `500m` | `500m` | Burstable | shared pool, scheduler decides |
| `numa-pool` | `500m` | `500m` | Burstable | shared pool, `taskset` to one socket |
| `pinned` | `1` (whole number) | `5` (whole number) | **Guaranteed** | exclusive cores from kubelet |

Only `pinned` requires the static CPU Manager from [03-cpu-qos.md](03-cpu-qos.md).
`baseline` and `numa-pool` deploy on any Kubernetes cluster.

## Verify a deployed stream

### Check that Pods are Running and on the intended node

```bash
# Both s01-dec and s01-enc should show STATUS=Running on the same node
kubectl -n mxl-perf get pods -o wide
```

Expected output (example):

```
NAME      READY   STATUS    RESTARTS   AGE   NODE
s01-dec   2/2     Running   0          30s   k8s-w2
s01-enc   2/2     Running   0          30s   k8s-w2
```

`READY 2/2` means both the main container and the `fps-sidecar` container are up.

### Inspect Pod details

```bash
# Decoder Pod — see nodeName, cpuset (pinned scenario), resource allocation
kubectl -n mxl-perf describe pod s01-dec

# Encoder Pod — see nodeName, cpuset, resource allocation
kubectl -n mxl-perf describe pod s01-enc

# In pinned mode: confirm exclusive cores were assigned
kubectl -n mxl-perf describe pod s01-enc | grep -A2 "Limits\|Requests\|cpu"
```

### Read container logs

```bash
# Decoder FFmpeg output
kubectl -n mxl-perf logs s01-dec -c decoder

# Encoder FFmpeg output (look for fps= lines to confirm transcoding is running)
kubectl -n mxl-perf logs s01-enc -c encoder

# Follow logs in real time
kubectl -n mxl-perf logs -f s01-dec -c decoder
kubectl -n mxl-perf logs -f s01-enc -c encoder
```

### Check live FPS from the sidecar (requires Prometheus port-forward)

```bash
# FPS from the sidecar (requires port-forward-prometheus.sh)
curl -s http://127.0.0.1:19090/api/v1/query \
  --data-urlencode 'query=mxl_ffmpeg_fps{role="encoder"}' | python3 -m json.tool
```

### Troubleshooting

If a Pod is stuck in `Pending` or `ContainerCreating`:

```bash
# Show events that explain scheduling or image-pull failures
kubectl -n mxl-perf describe pod s01-dec | tail -20

# Check node availability and allocatable CPU
kubectl get nodes -o wide
kubectl describe node <node-name> | grep -A5 "Allocatable\|Conditions"
```

If the decoder or encoder container exits immediately (CrashLoopBackOff):

```bash
# Print logs from the previous (failed) container instance
kubectl -n mxl-perf logs s01-dec -c decoder --previous
kubectl -n mxl-perf logs s01-enc -c encoder --previous
```

## Teardown

```bash
kubectl delete namespace mxl-perf            # removes all stream Pods
scripts/teardown.sh                          # runner-managed teardown
```

Next: [08-profiling-manifest.md](08-profiling-manifest.md) — profile the running
containers and generate a DMF-CRM deployment manifest.
