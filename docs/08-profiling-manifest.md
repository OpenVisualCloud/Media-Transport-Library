# 8. Container profiling and DMF-CRM manifest generation

The AMWA DMF Container Resource Management (CRM) workflow requires knowing what
resources a container actually needs before specifying those requirements in a
deployment manifest. This step measures the resource usage of a running
decoder/encoder stream and produces a DMF-CRM manifest that encodes those
requirements as Kubernetes resource specifications.

Before proceeding:

* A stream must be deployed and running — see [07-container-deployment.md](07-container-deployment.md).
* The observability stack must be running — see [06-observability.md](06-observability.md).
* The MCP profiling server must be installed — see [13-mcp-profiling.md](13-mcp-profiling.md).

## What profiling measures

To set correct CPU and memory requests in a DMF-CRM manifest, you need to know:

| Resource | Where it comes from | How to read it |
|---|---|---|
| CPU (cores) | cAdvisor → Prometheus | `container_cpu_usage_seconds_total` averaged over the measurement window |
| Memory (bytes) | cAdvisor → Prometheus | `container_memory_working_set_bytes` peak |
| NUMA node affinity | `/sys/devices/system/node/` | Which NUMA node holds the container's allocated CPUs |
| Cache footprint | Intel RDT `resctrl` | LLC occupancy in MiB via `cqm_occup_llc` |
| Memory bandwidth | Intel RDT `resctrl` | `mbm_total_bytes` rate in GB/s |
| Thread utilisation | MCP profiler | Per-core load, starvation, IRQ distribution |

## Deploy a profiling run

```bash
scripts/run.sh pinned --streams 1 --rdt-monitor  \
  --warmup 1m --measure 5m
```

This deploys a single pinned stream (`s01`) with RDT monitoring, warms up for one
minute, measures for three minutes, and leaves the Pods running.

## Reference: result files from a profiling run

```
results/<run-dir>/
├── workload.yaml          the exact manifests applied — the source of truth
├── config.json            all resolved settings (CPUs, threads, margins)
├── metrics.csv            every Prometheus sample during the measurement window
├── rdt-encoder.json       RDT LLC occupancy and MBM for encoder cgroup
├── rdt-decoder.json       RDT LLC occupancy and MBM for decoder cgroup
├── planned-placement.json which CPUs each container was assigned
├── pod-describe.txt       kubectl describe output with actual cpusets
└── host.json              live hardware probe (lscpu, numactl, meminfo)
```



Next: [09-density.md](09-density.md) — benchmark the three DMF density cases.

See also: [15-dmf-crm-manifest.md](15-dmf-crm-manifest.md) — generate the DMF-CRM manifest from the profiling results.
