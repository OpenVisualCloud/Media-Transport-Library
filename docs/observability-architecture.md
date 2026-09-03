# Observability Stack Architecture

## Overview

The observability stack comprises five independent metric sources that collectively provide evidence for performance analysis. Each answers a specific question that the others cannot.

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        MONITORING NAMESPACE (Cluster)                       │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────┐  ┌──────────────────────────────────┐   │
│  │      Prometheus Server        │  │       Grafana Dashboard          │   │
│  │  • TSDB (7-day retention)     │  │  • Visualization & alerting      │   │
│  │  • Query API                  │  │  • Queries Prometheus HTTP API   │   │
│  │  • Job: pcm-sensor-server     │  │  • Port: 3000 → 13000 (port-fw) │   │
│  │  • Job: node-exporter         │  │                                  │   │
│  │  • Job: kube-state-metrics    │  │                                  │   │
│  │  • PodMonitor: FPS sidecars   │  │                                  │   │
│  └────────────┬─────────────────┘  └──────────────────────────────────┘   │
│               │                                                             │
│               │ Scrapes every 5-15s                                        │
│               │                                                             │
│   ┌───────────┼───────────────────────────────────────────────────┐       │
│   │           │                                                   │       │
│   │   ┌───────▼────────┐  ┌──────────────┐  ┌──────────────┐   │       │
│   │   │ ServiceMonitor │  │ PodMonitor   │  │ Kubernetes   │   │       │
│   │   │(PCM scraping)  │  │(FPS metrics) │  │ Scrape Cfg   │   │       │
│   │   │ Job: pcm-      │  │ Per-run      │  │(node-exp,kbm)│   │       │
│   │   │ sensor-server  │  │              │  │              │   │       │
│   │   └───────┬────────┘  └──────────────┘  └──────────────┘   │       │
│   │           │                                                   │       │
│   └───────────┼───────────────────────────────────────────────────┘       │
│               │                                                             │
│   ┌───────────┼──────────────────────────────────────────────────┐       │
│   │           │                                                  │       │
│   │   ┌───────▼──────────────┐  ┌──────────────────────────┐   │       │
│   │   │Service + EndpointSlice│  │ Prometheus Operator CRDs│   │       │
│   │   │(Manual, headless)     │  │ • ServiceMonitor        │   │       │
│   │   │ Targets: 127.0.0.1:   │  │ • PodMonitor            │   │       │
│   │   │ 9738 on each node     │  │ • PrometheusRule        │   │       │
│   │   └───────┬──────────────┘  └──────────────────────────┘   │       │
│   │           │                                                  │       │
│   └───────────┼──────────────────────────────────────────────────┘       │
│               │                                                             │
└───────────────┼─────────────────────────────────────────────────────────────┘
                │
                │ http://<node_ip>:9738/metrics (5s interval)
                │
    ┌───────────┴──────────────┐
    │                          │
┌───▼──────────────┐  ┌────────▼────────────┐
│   WORKER NODE 1  │  │   WORKER NODE N     │
├──────────────────┤  ├─────────────────────┤
│                  │  │                     │
│ ┌──────────────┐ │  │ ┌──────────────┐   │
│ │ PCM Exporter │ │  │ │ PCM Exporter │   │
│ │ :9738        │ │  │ │ :9738        │   │
│ │              │ │  │ │              │   │
│ │ Reads:       │ │  │ │ Reads:       │   │
│ │ • UPI links  │ │  │ │ • UPI links  │   │
│ │ • DRAM R/W   │ │  │ │ • DRAM R/W   │   │
│ │ • L3 cache   │ │  │ │ • L3 cache   │   │
│ │              │ │  │ │              │   │
│ │ CPUAffinity: │ │  │ │ CPUAffinity: │   │
│ │ 0-3 (both    │ │  │ │ 0-3 (both    │   │
│ │ sockets)     │ │  │ │ sockets)     │   │
│ └──────────────┘ │  │ └──────────────┘   │
│                  │  │                     │
│ ┌──────────────┐ │  │ ┌──────────────┐   │
│ │node-exporter │ │  │ │node-exporter │   │
│ │:9100 (DS)    │ │  │ │:9100 (DS)    │   │
│ │              │ │  │ │              │   │
│ │ Per-CPU      │ │  │ │ Per-CPU      │   │
│ │ utilization  │ │  │ │ utilization  │   │
│ └──────────────┘ │  │ └──────────────┘   │
│                  │  │                     │
│ ┌──────────────┐ │  │ ┌──────────────┐   │
│ │  FFmpeg Pod  │ │  │ │  FFmpeg Pod  │   │
│ │ (Decoder)    │ │  │ │ (Decoder)    │   │
│ │              │ │  │ │              │   │
│ │ ┌──────────┐ │  │ │ ┌──────────┐   │   │
│ │ │FPS Meter │ │  │ │ │FPS Meter │   │   │
│ │ │Sidecar   │ │  │ │ │Sidecar   │   │   │
│ │ │:8000     │ │  │ │ │:8000     │   │   │
│ │ └──────────┘ │  │ │ └──────────┘   │   │
│ └──────────────┘ │  │ └──────────────┘   │
│                  │  │                     │
│ ┌──────────────┐ │  │ ┌──────────────┐   │
│ │  FFmpeg Pod  │ │  │ │  FFmpeg Pod  │   │
│ │ (Encoder)    │ │  │ │ (Encoder)    │   │
│ │              │ │  │ │              │   │
│ │ ┌──────────┐ │  │ │ ┌──────────┐   │   │
│ │ │FPS Meter │ │  │ │ │FPS Meter │   │   │
│ │ │Sidecar   │ │  │ │ │Sidecar   │   │   │
│ │ │:8000     │ │  │ │ │:8000     │   │   │
│ │ └──────────┘ │  │ │ └──────────┘   │   │
│ └──────────────┘ │  │ └──────────────┘   │
│                  │  │                     │
│ ┌──────────────┐ │  │ ┌──────────────┐   │
│ │ /dev/shm/mxl │ │  │ │ /dev/shm/mxl │   │
│ │ MXL flow     │ │  │ │ MXL flow     │   │
│ │ (tmpfs)      │ │  │ │ (tmpfs)      │   │
│ └──────────────┘ │  │ └──────────────┘   │
│                  │  │                     │
└──────────────────┘  └─────────────────────┘
```

## Metric Sources & Responsibilities

### 1. **FFmpeg FPS Sidecar** (Container-based)
- **Runs in:** Every decoder/encoder Pod
- **Measures:** Frame rate per stream
- **Metrics:** `mxl_ffmpeg_fps`, `mxl_ffmpeg_speed_ratio`, `mxl_ffmpeg_drop_frames_total`
- **Export:** PodMonitor → Prometheus (created per run)
- **Why:** Direct evidence of whether a stream meets `LAB_MIN_FPS` (59.5)

### 2. **cAdvisor** (Kubernetes built-in)
- **Runs in:** kubelet
- **Measures:** Per-container CPU time, memory usage
- **Metrics:** `container_cpu_usage_seconds_total`, `container_memory_usage_bytes`
- **Export:** Prometheus via ServiceMonitor
- **Why:** Encoder vs decoder CPU attribution

### 3. **node-exporter** (DaemonSet)
- **Runs on:** Every worker (deployed by kube-prometheus-stack)
- **Measures:** Per-CPU utilization, disk I/O, network
- **Metrics:** `node_cpu_seconds_total`, `node_filesystem_*`
- **Export:** Prometheus via ServiceMonitor
- **Why:** Which physical cores worked, reserved CPUs stayed quiet?

### 4. **Intel PCM** (Host systemd service)
- **Runs on:** Worker (NOT in a Pod; see [Why This Architecture](#why-this-architecture))
- **Measures:** Cross-socket UPI traffic, DRAM read/write, L3 hit ratio
- **Metrics:** `Incoming_Data_Traffic_On_Link_*`, `Read_Data_Volume_On_Memory_Channel_*`
- **Export:** HTTP endpoint on :9738 → Prometheus via ServiceMonitor + manual Service/EndpointSlice
- **Why:** Uncore counters no in-cluster exporter can access; core evidence for NUMA density measurements

### 5. **Intel RDT** (`resctrl`)
- **Runs on:** Worker (on-demand per run)
- **Measures:** Per-cgroup LLC occupancy, memory bandwidth
- **Metrics:** Custom output via `mxl-rdt-host` helper
- **Export:** Collected by runner, stored in run results
- **Why:** Detailed LLC contention and memory bandwidth attribution; see [12-rdt-qos.md](12-rdt-qos.md)

---

## Installation Flow

### Phase 1: Worker Bootstrap

```
Controller
    │
    └─ scripts/bootstrap-worker.sh [NODE]
       │
       ├─ install-worker-limits.sh
       │  └─ Raise fs.inotify.max_user_instances to 8192
       │
       ├─ install-pcm-host.sh
       │  ├─ Build pcm-sensor-server from intel/pcm
       │  ├─ Load MSR kernel module
       │  ├─ Install → /usr/local/sbin/pcm-sensor-server
       │  └─ Create systemd service: pcm-sensor-server.service
       │
       ├─ install-host-noise.sh
       │  └─ stress-ng, numactl, util-linux
       │
       ├─ install-rdt-host.sh
       │  ├─ Mount /sys/fs/resctrl
       │  └─ Install mxl-rdt-host helper + sudoers rule
       │
       └─ install-platform-probe.sh
          └─ Sudoers rule for dmidecode (DRAM peak calculation)
```

### Phase 2: Cluster Observability Stack

```
Controller
    │
    └─ scripts/install-observability.sh
       │
       ├─ 1. Helm install kube-prometheus-stack → monitoring namespace
       │     └─ Prometheus, Grafana, node-exporter DaemonSet, kube-state-metrics
       │
       ├─ 2. For each worker:
       │     ├─ Get node IP
       │     ├─ Verify PCM is running (curl :9738/metrics)
       │     └─ kubectl apply: Service, EndpointSlice, ServiceMonitor
       │
       └─ 3. Verify Prometheus sees PCM target (query via API server proxy)
```

---

## Data Flow: Metrics → Prometheus → Grafana

### PCM Path (Most Complex)

```
Worker: pcm-sensor-server.service (systemd)
    │
    ├─ Reads /dev/cpu/*/msr (MSR kernel interface)
    ├─ Reads /proc/cpuinfo, DMI tables
    └─ Aggregates counters
        │
        └─ Exposes HTTP endpoint: http://localhost:9738/metrics
           │
           └─ Prometheus text format: metric_name{labels} value
              │
              │ (Example)
              └─ Incoming_Data_Traffic_On_Link_0{aggregate="system"} 524288000
                 │
                 │
                 └─→ [Kubernetes abstraction layer]
                    │
                    ├─ Service: pcm-sensor-server (headless, no Pod selector)
                    ├─ EndpointSlice: points at <node_ip>:9738
                    └─ ServiceMonitor: tells Prometheus Operator to scrape
                        │
                        └─→ Prometheus Operator updates Prometheus config
                            │
                            └─→ Prometheus scrape job: pcm-sensor-server
                                │
                                ├─ Every 5 seconds:
                                │  GET http://<node_ip>:9738/metrics
                                │
                                └─ Parse + store in TSDB
                                   (7-day retention, job label = pcm-sensor-server)
                                    │
                                    └─→ Query API available at http://127.0.0.1:19090
                                        │
                                        └─→ Grafana queries + visualizes
```

### FFmpeg FPS Path (Per-Run)

```
Pod: decoder/encoder + FPS sidecar
    │
    └─ ffmpeg -progress /run/mxl/progress
        │
        └─ fps_exporter.py tails /run/mxl/progress
            │
            ├─ Parse frame count, speed ratio, drops
            │
            └─ Expose metrics: http://localhost:8000/metrics
               │
               │ (Example)
               └─ mxl_ffmpeg_fps{pod=s01,stream_type=encoder} 60.0
                  │
                  └─→ PodMonitor (created by runner per run)
                      │
                      └─→ Prometheus Operator updates config
                          │
                          └─→ Prometheus scrapes during run window
                              │
                              └─→ Queried by runner to validate
                                 (pass/fail: all streams ≥ LAB_MIN_FPS)
```

---

## Why This Architecture

| Design Choice | Reason |
|---|---|
| **PCM as systemd service, not Pod** | PCM's cpuset inheritance determines which NUMA topology it can see. Inside a Pod under dense workload, that cpuset collapses to one socket, hiding UPI links — the entire point of the measurement. As a host service with fixed CPUAffinity, it always sees both sockets. |
| **Manual Service + EndpointSlice** | Kubernetes Services normally select Pods. Here, we need to point at a host process on :9738. EndpointSlice allows pointing at any IP:port without a Pod selector. |
| **ServiceMonitor CRD** | Pull-based scraping is Kubernetes-native. ServiceMonitor lets Prometheus Operator auto-generate scrape configs for both Pods and the host PCM service. |
| **PodMonitor per run** | FPS metrics are run-specific. Runner injects a PodMonitor at deployment time, deleted after the run. |
| **Prometheus retention: 7d** | Enough to hold benchmark runs and historical comparisons; balances storage and query depth. |
| **node-exporter DaemonSet** | Standard Kubernetes observability. Per-CPU metrics show which cores were used and whether reserved CPUs stayed quiet. |

---

## Access & Queries

### Port-forward to Prometheus

```bash
scripts/port-forward-prometheus.sh
# Then: http://127.0.0.1:19090
```

### Useful Prometheus Queries

```promql
# PCM health check
up{job="pcm-sensor-server"}

# UPI cross-socket traffic (bytes/sec)
Incoming_Data_Traffic_On_Link_0{aggregate="system"}
Incoming_Data_Traffic_On_Link_1{aggregate="system"}

# L3 cache hit ratio
sum(rate(L3_Cache_Hits[2m])) / (sum(rate(L3_Cache_Hits[2m])) + sum(rate(L3_Cache_Misses[2m])))

# Per-encoder FPS during a run
mxl_ffmpeg_fps{stream_type="encoder"}

# Encoder CPU time (per container)
rate(container_cpu_usage_seconds_total{pod=~"s[0-9]+.*encoder"}[1m])

# Per-CPU utilization on worker
rate(node_cpu_seconds_total[1m])
```

### Port-forward to Grafana

```bash
kubectl -n monitoring port-forward svc/monitoring-grafana 13000:80
# Then: http://127.0.0.1:13000
# User: admin
# Password: kubectl -n monitoring get secret monitoring-grafana \
#   -o jsonpath='{.data.admin-password}' | base64 -d
```

---

## Verification

### Full health check

```bash
scripts/preflight.sh
```

### Manual verification (no Kubernetes in the way)

```bash
# PCM service is running and serving counters
ssh <user>@<worker> "systemctl status pcm-sensor-server --no-pager | head -5"

# PCM metrics are reachable
ssh <user>@<worker> "curl -s localhost:9738/metrics | grep -c Incoming_Data_Traffic_On_Link_"

# node-exporter is running
ssh <user>@<worker> "curl -s localhost:9100/metrics | head -20"
```

---

## Next Steps

- Deploy decoder/encoder Pods: [07-container-deployment.md](07-container-deployment.md)
- Enable per-run RDT profiling: [12-rdt-qos.md](12-rdt-qos.md)
- Profiler integration: [13-mcp-profiling.md](13-mcp-profiling.md)
