# 00. DMF Architecture and Reference Recipe Design

This document describes the architecture of the AMWA DMF (Distributed Media
Foundation) Kubernetes reference recipe and how the components relate to one
another. Nothing here is required reading — it is the map you want open while
working through the steps in [../README.md](../README.md).

## What this reference recipe is for

This reference recipe provides an **end-to-end blueprint** for validating that a Kubernetes platform can run
DMF workloads to their SLA, from bare metal platform configuration through to
auditable, reproducible benchmark results.

It answers a practical question: **"Given my hardware and Kubernetes configuration, how
many live 1080p60 streams can I sustain at 60 FPS, and what does proper platform
configuration buy me?"**

The recipe demonstrates that you can take a media workload from initial platform setup
through profiling, deployment, validation under realistic interference conditions,
and finally publication of complete, auditable results — all using standard
Kubernetes and open-source tools.

## The reference recipe flow

```
PHASE I: FOUNDATION — Build and certify the platform
├─ BIOS / Platform     validate BIOS settings, capture baseline (docs/01, 04)
├─ Kubernetes          install cluster + DMF CPU QoS plugins (docs/02, 03)
├─ Observability       Prometheus, Grafana, Intel PCM, RDT visibility (docs/06)
└─ Workload Image      CBC FFmpeg-MXL container + test media (docs/05)

PHASE II: MEASUREMENT — Profile and benchmark
├─ Pod Deployment      deploy decoder + encoder Pods (docs/07-container-deployment)
├─ Container Profiling measure actual resource needs (docs/08-profiling-manifest)
├─ DMF-CRM Manifest    generate resource declarations from profile (docs/08-profiling-manifest)
└─ Density Benchmark   three placement policies: baseline, NUMA-pool, pinned (docs/07)

PHASE III: VALIDATION — Prove SLA is achievable
├─ Clean Baseline      verify 60 FPS is achievable when nothing interferes
├─ Noisy Neighbors     inject synthetic interference (docs/08)
├─ SLA Failure         measure FPS drop under interference
├─ RDT Mitigation      apply Intel CAT + MBA policies (docs/09)
└─ SLA Recovery        restore 60 FPS with RDT isolation

PHASE IV: REPORTING
└─ Auditable Results   metrics, placement, config, host info (docs/12)
```

## Density Testing

The recipe benchmarks the same workload under three different Kubernetes CPU placement
policies to show **how much configuration alone can achieve**:

```
         ┌─────────────────────────────────────────────────────────────┐
         │  CORE QUESTION:  How many 1080p60 streams at 60 FPS?       │
         └─────────────────────────────────────────────────────────────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
        ┌──────────┐         ┌──────────────┐      ┌─────────────┐
        │ baseline │         │  numa-pool   │      │   pinned    │
        │  ○ ○ ○   │         │  ●●● | ○○○   │      │ ■ ■ ■ ■ ■ ■ │
        │ anywhere │         │ one socket   │      │  exclusive  │
        │ Burstable│         │  per stream  │      │ Guaranteed  │
        ├──────────┤         ├──────────────┤      ├─────────────┤
        │12 streams│    →    │  14 streams  │  →   │  20 streams │
        │46.0 GB/s │         │  0.55 GB/s   │      │  1.44 GB/s  │
        │   UPI    │         │     UPI      │      │     UPI     │
        └──────────┘         └──────────────┘      └─────────────┘
             └──────────────── +67 % density ───────────────┘
                        from configuration alone
```

> **⚠️ Platform-Dependent Results**: The stream counts and performance metrics shown above
> (12, 14, 20 streams) are **reference values only** and were measured on the specific
> hardware described in [14-reference-bkc.md](14-reference-bkc.md). Your results will
> vary significantly depending on CPU count, memory capacity, socket topology, memory
> bandwidth, and interconnect characteristics. Follow this recipe to establish **your
> platform's** baseline, not to match these numbers.

**Baseline** shows what happens with default Burstable QoS and scheduler freedom — workloads
collide across NUMA sockets, inflating interconnect traffic (46 GB/s UPI).

**NUMA-pool** assigns each encoder+decoder pair to one socket — reduces interconnect load
(0.55 GB/s UPI) but no CPU exclusivity, so they still compete for cores.

**Pinned** uses Guaranteed QoS with exclusive cores — maximum isolation, predictable
placement, and the highest density.

## DMF architecture mapping in this recipe

This reference recipe does not attempt to implement every layer of the AMWA DMF
reference architecture. It focuses on the layers needed to deploy, place,
measure and validate media-processing workloads on Kubernetes:
**Infrastructure**, **Host Platform**, **Container Platform**,
**Media Exchange**, and **Media Functions**.

The mapping looks like this:

```
   ┌─────────────────────────────────────────────────────────────┬───────────────────────────────────────────────┐
   │  DMF LAYER                                                  │ RECIPE IMPLEMENTATION                         │
   ├─────────────────────────────────────────────────────────────┼───────────────────────────────────────────────┤
   │  MEDIA FUNCTIONS                                            │ CBC FFmpeg-MXL decoder + encoder workloads    │
   │                                                             │ docs/05, 07-container-deployment              │
   ├─────────────────────────────────────────────────────────────┼───────────────────────────────────────────────┤
   │  MEDIA EXCHANGE                                             │ MXL shared-memory transport and decoder→      │
   │                                                             │ encoder flow through /dev/shm/mxl             │
   │                                                             │ 07-container-deployment, 08-profiling-manifest      │
   ├─────────────────────────────────────────────────────────────┼───────────────────────────────────────────────┤
   │  CONTAINER PLATFORM                                         │ Kubernetes, containerd, kubelet CPU Manager,  │
   │                                                             │ Topology Manager, Calico                      │
   │                                                             │ docs/02, 03                                   │
   ├─────────────────────────────────────────────────────────────┼───────────────────────────────────────────────┤
   │  HOST PLATFORM                                              │ Ubuntu, kernel, host tuning, PerfSpect, Intel │
   │                                                             │ PCM, resctrl/RDT capabilities                 │
   │                                                             │ docs/04, 06, 11                               │
   ├─────────────────────────────────────────────────────────────┼───────────────────────────────────────────────┤
   │  INFRASTRUCTURE                                             │ Worker/controller nodes, BIOS, sockets, NUMA, │
   │                                                             │ memory, NICs, platform baseline               │
   │                                                             │ docs/01, 11                                   │
   └─────────────────────────────────────────────────────────────┴───────────────────────────────────────────────┘
```

Each mapped layer depends on the correctness of the layers below it:

- **Infrastructure** is the physical base: servers, sockets, NUMA layout,
  memory population, and BIOS settings.
- **Host Platform** is what runs on those machines: OS, kernel, host services,
  and low-level capabilities such as Intel PCM and `resctrl`.
- **Container Platform** is the Kubernetes layer that schedules and places the
  workloads.
- **Media Exchange** in this recipe is primarily the MXL shared-memory path and
  the creation of the data flow between decoder and encoder.
- **Media Functions** are the actual media-processing containers being measured.

The recipe also exercises several DMF cross-cutting concerns, but they are not
shown as layers here:

- **Orchestration** through `scripts/run.sh`, campaign files, and manifest generation
- **Control** through scenario selection, placement policy, and RDT policy application
- **Monitoring** through Prometheus, Grafana, Intel PCM, `resctrl`, cAdvisor,
  node-exporter, and the FPS sidecar

## Controller and Worker Nodes

The recipe uses a **controller** node and a **worker** node under test. The separation
ensures clean metrics: the controller orchestrates; the worker runs the workload and
collects telemetry:

```
  CONTROLLER  (control-plane node)            WORKER UNDER TEST
  ═══════════════════════════                ═══════════════════════════
                                             ┌── CPU 0-3 reserved ──────┐
   scripts/run.sh                            │ kubelet  containerd  PCM │
        │                                    └──────────────────────────┘
        ▼                                    ┌── CPU 4-127 allocatable ─┐
   mxlperf.cli ──── kubectl apply ──────────►│                          │
        │           (CONTROL PLANE)          │  ┌────────┐  ┌────────┐  │
        │                                    │  │decoder │  │encoder │  │
        │                                    │  │ v210   │  │ x264   │  │
        │                                    │  └───┬────┘  └───▲────┘  │
        │                                    │      │  DATA     │       │
        │                                    │      └──►/dev/shm◄┘      │
        │                                    │        /mxl  (zero-copy) │
        │                                    │  ┌──────────────────┐    │
        │◄─── PromQL ──── Prometheus ◄────────┼──┤ fps sidecar :9101│    │
        │     (METRICS PLANE)     ▲          │  └──────────────────┘    │
        ▼                         │          │                          │
   results/<run>/                 ├──────────┼── cAdvisor (per container)│
   summary.{html,csv,xlsx}        ├──────────┼── node-exporter (per CPU) │
   report.html                    ├──────────┼── PCM :9738  (per socket) │
   config.json                    └──────────┼── resctrl    (per cgroup) │
                                             └──────────────────────────┘
```

The controller never runs the workload. The worker never renders a report. This
separation makes it clear what was measured and where the bottleneck lies.

## Observability Stack QoS Insights

```
   WHAT YOU ASK                      WHO CAN ANSWER IT              WHERE
   ────────────────────────────────  ──────────────────────────   ─────────
   "did it hold 60 FPS?"        ───► fps sidecar              │   per stream
   "what did a stream cost?"    ───► cAdvisor                 │   per container
   "did exclusive cores leak?"  ───► node-exporter            │   per CPU
   "is the placement sane?"     ───► Intel PCM UPI            │   per socket
   "is memory the limit?"       ───► Intel PCM DRAM/L3        │   per socket
   "who is stealing the cache?" ───► Intel RDT CMT/MBM        │   per cgroup
   "why is it slow?"            ───► MCP deep profiler        │   per thread

   Each ring sees what the ring inside it cannot:

        ┌─ socket ──────────────────────────────┐
        │  ┌─ cgroup ────────────────────────┐  │
        │  │  ┌─ container ──────────────┐   │  │
        │  │  │  ┌─ stream ──────────┐   │   │  │
        │  │  │  │      FPS (sidecar)│   │   │  │
        │  │  │  └───────────────────┘   │   │  │
        │  │  └──────────────────────────┘   │  │
        │  └─────────────────────────────────┘  │
        └───────────────────────────────────────┘
```

## RDT and QoS Validation

The recipe proves SLA robustness by deliberately breaking it, then fixing it with RDT:

```
   1. CLEAN                2. NOISY                 3. GOVERNED
   ┌──────────┐            ┌──────────┐             ┌──────────┐
   │ 20 str   │            │ 20 str   │             │ 20 str   │
   │  ≥60 FPS │            │  <60 FPS │             │  ≥60 FPS │
   │    ✓     │            │    ✗    │             │    ✓     │
   └──────────┘            └────┬─────┘             └────▲─────┘
                                │                        │
                        ┌───────┴────────┐        ┌──────┴───────┐
                        │ noisy neighbor │        │  Intel RDT   │
                        │ host-a pod-a   │        │  CAT + MBA   │
                        │ pod-b  pod-c   │        │ cache  b/w   │
                        └────────────────┘        └──────────────┘
                          steals L3 + DRAM          gives it back
                         (docs/08 phase)           (docs/09 phase)
```

This proves that:
1. **SLA is achievable** when nothing interferes (CLEAN)
2. **SLA fails under realistic co-tenant load** (NOISY)
3. **RDT restores SLA even when neighbors are present** (GOVERNED)

## One benchmark run, start to finish

Every benchmark run follows this flow, producing an auditable result directory:

```
  run.sh pinned --streams 20 --rdt-monitor
      │
      ├─ 1 PLAN      cpu placement computed, refused early if it cannot fit
      │              ("needs 126 exclusive CPUs, only 124 allocatable")
      │
      ├─ 2 GATE      resctrl capabilities · PCM UPI telemetry available
      │              refuse to run if platform cannot measure what we need
      │
      ├─ 3 STAGE     stage neighbor #1 → deploy workload → stage remaining
      │              warm up all paths without scoring
      │
      ├─ 4 ADMIT     verify PCM again · attach RDT groups · apply CAT/MBA
      │
      ├─ 5 WARM-UP   run 30 sec, discard all samples (JIT compile, cache fill)
      │
      ├─ 6 MEASURE   the only window that counts — collect 60 sec of data
      │
      ├─ 7 COLLECT   PromQL → metrics.csv · resctrl → rdt-*.json
      │              pull all observability data and bundle it
      │
      └─ 8 TEARDOWN  delete namespace · restore RDT · kill noise
                     clean up every resource
                                │
                                ▼
              results/pinned-exclusive-20str-<UTC>/
              ├── report.html              the headline answer
              ├── metrics.csv              every sample from all exporters
              ├── config.json              every resolved setting
              ├── workload.yaml            exact YAML applied to cluster
              ├── planned-placement.json   which CPU each container wanted
              ├── pod-describe.json        which CPU each container actually got
              ├── host.json                which physical machine it ran on
              └── rdt-*.json               per-cgroup cache/bandwidth samples

              A result you cannot audit is not a result.
```

## The recipe structure

```
CRM-k8s-refrence-recipe/
│
├── README.md                             the "getting started" entry point
├── Makefile                              named shortcuts for every major step
│
├── docs/                                 the reference recipe steps
│   ├── 00-design.md                      this file — architecture overview
│   ├── 01-bios-bkc.md                    BIOS settings required for DMF QoS
│   ├── 02-kubernetes-install.md          Kubernetes cluster from bare Ubuntu
│   ├── 03-cpu-qos.md                     static CPU Manager + Topology Manager tuning
│   ├── 04-perfspect-baseline.md          capture platform baseline
│   ├── 05-ffmpeg-mxl-container.md        build CBC FFmpeg-MXL image
│   ├── 06-observability.md               Prometheus, Grafana, PCM installation
│   ├── 07-container-deployment.md        deploy decoder/encoder Pods
│   ├── 08-profiling-manifest.md          profile → DMF-CRM manifest
│   ├── 09-density.md                     benchmark three placement policies
│   ├── 10-metrics.md                     Results interpretation guide
│   ├── 11-noisy-neighbor.md              co-tenant interference testing
│   ├── 12-rdt-qos.md                     Intel RDT isolation and recovery
│   ├── 13-mcp-profiling.md               deep-dive thread-level profiling (supporting)
│   └── 14-reference-bkc.md               Reference Test Hardware Config (supporting)
│
├── scripts/                              one script per recipe step (all idempotent)
│   ├── setup-controller-worker-ssh.sh
│   ├── check-bios.sh
│   ├── install-k8s-cluster.sh
│   ├── configure-cpu-qos.sh
│   ├── run-perfspect.sh
│   ├── build-ffmpeg-mxl-image.sh
│   ├── stage-media.sh
│   ├── bootstrap-worker.sh
│   ├── install-observability.sh
│   ├── setup.sh
│   ├── preflight.sh
│   ├── run.sh                            (the main benchmark runner)
│   ├── run-campaign.sh                   (run multiple benchmarks in sequence)
│   ├── summarize.sh
│   └── teardown.sh
│
├── python/mxlperf/                       the measurement engine
│   ├── cli.py                            entry point
│   ├── render.py                         Kubernetes manifest builder
│   ├── collect.py                        PromQL collector
│   ├── rdt.py                            RDT group management
│   ├── host_noise.py                     noisy-neighbor injection
│   ├── platform.py                       platform validation
│   ├── report.py                         result aggregation
│   └── summary.py                        HTML/CSV/XLSX summary
│
├── config/                               tunable environment and node inventory
│   ├── lab.env                           lab parameters and defaults
│   └── nodes.env                         SSH inventory (fill in first)
│
├── scenarios/                            benchmark placement cases
│   ├── baseline.env
│   ├── numa-pool.env
│   └── pinned.env
│
├── noisy-neighbors/                      co-tenant interference profiles
│   ├── host-a.yaml                       CPU-intensive, host-scoped stress
│   ├── pod-a.yaml                        memory-intensive
│   ├── pod-b.yaml                        cache-intensive
│   └── pod-c.yaml                        interconnect-intensive
│
├── campaigns/                            ready-made run sequences
│   ├── density.env                       baseline → numa-pool → pinned
│   ├── sweep.env                         density with varying stream counts
│   └── rdt-*.env                         RDT isolation sweeps
│
├── observability/                        Helm values and PCM scrape config
│   ├── prometheus-values.yaml
│   ├── pcm-scrape.yaml
│   └── fps-exporter.py
│
├── cluster/                              network and cluster setup
│   └── calico-installation.yaml
│
├── tests/                                unit tests (no cluster required)
│
└── results/                              one directory per run
    ├── baseline-baseline-12str-<UTC>/
    ├── numa-pool-numa-pool-14str-<UTC>/
    ├── pinned-exclusive-20str-<UTC>/
    └── summary.html                      comparative report across all runs
```

## What gets built

Following the recipe produces:

1. **A reproducible Kubernetes platform** — nodes know their BIOS, CPU topology,
   and QoS constraints are configured correctly
2. **A profiled DMF workload** — containers have been measured and their resource
   requirements encoded into DMF-CRM manifests
3. **Three baseline benchmarks** — clean performance under each placement policy
4. **Interference validation** — proof that workloads fail under co-tenant load
5. **RDT mitigation** — proof that Intel RDT policies restore performance isolation
6. **Auditable results** — metrics, configs, and host info all bundled together

Every result can be cross-referenced back to:
- The exact config that produced it (`config.json`)
- The exact YAML deployed (`workload.yaml`)
- The exact machine it ran on (`host.json`)
- Every metric sample collected (`metrics.csv`, `rdt-*.json`)

> **⚠️ Reference Results, Not Targets**: The benchmarks, stream counts, and performance
> figures produced by this recipe are **specific to your hardware and configuration**.
> Use this recipe to establish baselines on your own platform, validate that your
> Kubernetes setup can run DMF workloads, and measure the impact of configuration
> changes. Do not expect to match the numbers from the reference hardware — you may
> achieve higher or lower density depending on your CPU generation, memory capacity,
> and interconnect topology. See [14-reference-bkc.md](14-reference-bkc.md) for details
> on the hardware used for the reference measurements.

## Everything is reproducible

- All configuration is in plain `.env` files
- All orchestration is shell scripts or Python modules
- No templating languages, no operators, no state outside `results/`
- Every run produces a timestamped directory with complete audit trail
- Re-run the same command and you get comparable results (modulo noise)
