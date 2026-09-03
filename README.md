# AMWA DMF Kubernetes Reference Recipe

A step-by-step guide for deploying and validating AMWA DMF (Alliance for Media
Workflow Association — Distributed Media Foundation) workloads on Kubernetes,
using CBC FFmpeg-MXL as the reference media container.

This guide shows:
- how to build a Kubernetes cluster from bare Ubuntu with the
  CPU QoS plugins DMF containers require (static CPU Manager, Topology Manager,
  Intel RDT)
- how to build, profile and deploy the FFmpeg-MXL container as standardised DMF
  decoder and encoder Pods
- how to generate a DMF-CRM deployment manifest from container profiling data;
- how to benchmark three density scenarios and understand the results
- how to protect the workload from co-tenant interference using Intel RDT CAT and
  MBA policies

On the reference machine, the three density cases produce **12 → 14 → 20 streams**
for the same hardware and the same FFmpeg command line, with nothing changed but
Kubernetes CPU placement policy:

| Case | What changes | Streams at ≥ 59.5 FPS | Cross-socket UPI | CPU per stream |
|---|---|---|---|---|
| `baseline` | No changes applied (Burstable Pods, scheduler decides) | **12** | 46.0 GB/s | 5.77 cores |
| `numa-pool` | both Pods of a stream confined to one socket | **14** | 0.55 GB/s | 5.01 cores |
| `pinned` | Guaranteed Pods, exclusive cores, NUMA-aligned | **20** | 1.44 GB/s | 4.76 cores |


##  Step by step guide to Installing K8s and tooling, Profiling and Deploying FFmpeg-mxl Pods

Step by Step Guide
For a new deployment, follow the single-page
**[clean-host quickstart](docs/QUICKSTART.md)**. It covers both direct and proxied
networks, a one-stream smoke test, and the full density campaign. Use the numbered
documents below when a step needs platform-specific detail.

| Step | Document | What you get |
|---|---|---|
| **Quickstart** | [QUICKSTART.md](docs/QUICKSTART.md) | **Start here:** clean hosts to a measured MXL run |
| **0** | [00-before-you-start.md](docs/00-before-you-start.md) | Detailed host, tool, SSH, and inventory prerequisites |
| - | [00-design.md](docs/00-design.md) | DMF architecture overview and lab design — read once, keep open |
| 1 | [01-bios-bkc.md](docs/01-bios-bkc.md) | Xeon BIOS settings required for DMF QoS, and how to verify them |
| 2 | [02-kubernetes-install.md](docs/02-kubernetes-install.md) | Kubernetes cluster from bare Ubuntu with all DMF-required plugins |
| 3 | [03-cpu-qos.md](docs/03-cpu-qos.md) | Static CPU Manager, Topology Manager and RDT — the QoS layer DMF depends on |
| 4 | [04-perfspect-baseline.md](docs/04-perfspect-baseline.md) | Platform baseline report — capture before any measurement |
| 5 | [05-ffmpeg-mxl-container.md](docs/05-ffmpeg-mxl-container.md) | Build the CBC FFmpeg-MXL container and prepare test media |
| 6 | [06-observability.md](docs/06-observability.md) | Observability stack: Prometheus, Grafana, node-exporter, Intel PCM |
| 7 | [07-container-deployment.md](docs/07-container-deployment.md) | Deploy decoder and encoder containers as Kubernetes Pods |
| 8 | [08-profiling-manifest.md](docs/08-profiling-manifest.md) | Profile containers and capture the resolved run artifacts (`metrics.csv`, `config.json`, `workload.yaml`) |
| 9 | [09-density.md](docs/09-density.md) | Benchmark the three density cases: baseline, NUMA pooling, pinned |
| 10 | [10-metrics.md](docs/10-metrics.md) | Understand and interpret benchmark results |
| 11 | [11-noisy-neighbor.md](docs/11-noisy-neighbor.md) | Noisy neighbor testing — measure co-tenant interference |
| 12 | [12-rdt-qos.md](docs/12-rdt-qos.md) | Apply Intel RDT CAT and MBA to restore performance isolation |
| 13 | [13-mcp-profiling.md](docs/13-mcp-profiling.md) | Deep-dive profiling — diagnose why a specific stream missed its deadline |
| 14 | [14-reference-bkc.md](docs/14-reference-bkc.md) | Reference platform (BKC) — the exact hardware all published numbers came from |
| 15 | [15-dmf-crm-manifest.md](docs/15-dmf-crm-manifest.md) | Generate a DMF CRM manifest from a completed profile and PerfSpect baseline |

## Quickstart — from bare hardware to first DMF benchmark

```bash
# 1. Describe your machines (node names, SSH addresses, CPU topology)
#    Edit config/nodes.env and config/lab.env — see 02-kubernetes-install.md

# 2. SSH keys: controller → every worker
scripts/setup-controller-worker-ssh.sh

# 3. Platform prerequisites
scripts/check-bios.sh                    # verify BIOS BKC (01-bios-bkc.md)

# 4. Install Kubernetes with DMF plugins
scripts/install-k8s-cluster.sh           # kubeadm + Calico (02-kubernetes-install.md)
scripts/configure-cpu-qos.sh             # static CPU Manager + Topology Manager (03-cpu-qos.md)

# 5. Platform baseline
scripts/configure-power.sh               # P-state/governor/EPB/EPP/ELC (04-perfspect-baseline.md)
scripts/run-perfspect.sh                 # capture PerfSpect report (04-perfspect-baseline.md)

# 6. Build the FFmpeg-MXL container
scripts/build-ffmpeg-mxl-image.sh        # builds on the worker (05-ffmpeg-mxl-container.md)
scripts/stage-media.sh <clip.mp4>        # stage test media

# 7. Install observability
scripts/bootstrap-worker.sh             # Intel PCM, RDT helper, stress-ng
scripts/install-observability.sh        # Prometheus and Grafana (06-observability.md)

# 8. Deploy and profile (07-container-deployment.md, 08-profiling-manifest.md)
scripts/setup.sh                        # install the mxl-perf runner
scripts/preflight.sh                    # verify the cluster is ready

# 9. Run the three DMF density benchmark cases
scripts/run-campaign.sh campaigns/density.env
open results/summary.html               # 3 rows: baseline, numa-pool, pinned
```

## Folder Directory Overview

```
config/lab.env          every tunable, commented; the one file to edit
config/nodes.env        SSH inventory: host addresses and login (fill in first)
scenarios/              the three benchmark cases: baseline, numa-pool, pinned
noisy-neighbors/        the four co-tenant profiles: host-a, pod-a, pod-b, pod-c
campaigns/              ready-made run lists (density, RDT sweeps)
scripts/                one script per step of the guide, all idempotent
python/mxlperf/         the runner: plan → deploy → collect → report → summarize
observability/          Helm values and the host PCM scrape wiring
cluster/                Calico network configuration
docs/                   the numbered guide above
results/                one directory per run, plus summary.html / summary.xlsx
```

## Requirements

* Two hosts: a controller (any modest machine) and at least one Intel Xeon worker
  with Intel RDT support, running Ubuntu 22.04 or 24.04.
* A non-root login that exists on every host, with passwordless SSH from the
  controller and `sudo` on the workers. The login name goes in `config/nodes.env`;
  nothing in this repo assumes a particular username, hostname or IP address.
* `kubectl`, `helm` and `python3` on the controller.
* One 1080p60 source clip (not shipped here — see
  [05-ffmpeg-mxl-container.md](docs/05-ffmpeg-mxl-container.md)).

Full detail, with the commands, is in
[00-before-you-start.md](docs/00-before-you-start.md). In short:

* Two clean hosts: a control plane (any small machine) and at least one Intel Xeon
  worker with Intel RDT, running Ubuntu 22.04 or 24.04.
* One login that exists on both hosts with the same name, able to become root, and
  passwordless SSH from the control-plane host to itself and to every worker. The
  login goes in `config/nodes.env`; nothing in this repo assumes a particular user,
  hostname or address. `root` is supported and makes the install unattended; any
  `sudo`-capable login works and prompts per host.
* Behind a proxy: two lines in `config/lab.env` (`LAB_HTTP_PROXY`,
  `LAB_HTTPS_PROXY`), and the cluster installer configures `apt`, containerd,
  `kubeadm` and `kubectl` on every node from them. Every script also keeps the
  cluster's own addresses out of the proxy for itself, so nothing here needs a
  hand-written `no_proxy`.
* On the control-plane host: `git` (or `unzip`), `curl`, `python3`, `python3-venv`
  and `helm`. `kubectl` is installed for you in chapter 2. The worker needs nothing
  installed by hand.
* Outbound internet from every host — the installers fetch containerd, Kubernetes,
  Calico, Helm charts, PerfSpect, PCM and the FFmpeg-MXL sources.
* One 1080p60 source clip. It is not shipped here; see
  [05-ffmpeg-mxl-container.md](docs/05-ffmpeg-mxl-container.md).

Every published number was measured on the platform recorded in
[14-reference-bkc.md](docs/14-reference-bkc.md). Different silicon produces
different absolute stream counts; the *ordering* of the three cases should
reproduce on any dual-socket Intel Xeon with RDT and the BIOS settings from
[01-bios-bkc.md](docs/01-bios-bkc.md).
