# 14. Reference platform (BKC)

Every number published in this repo was measured on the platform described here.
This is the lab's BKC — best known configuration — and it exists so that a result
measured somewhere else can be compared against something concrete. When your
result differs, compare against this page first, then against your own PerfSpect
report ([04-perfspect-baseline.md](04-perfspect-baseline.md)).

This page describes *what the hardware and software were*, not which machines
they were. Nothing here needs to be reproduced exactly; the section at the end
says which differences matter and which do not.

## Cluster

Two nodes: one control plane that never runs the workload, and one worker that is
the machine under test.

| | Control plane | Worker under test |
|---|---|---|
| Role | Kubernetes control plane only | runs the whole video workload |
| OS | Ubuntu 22.04 LTS | Ubuntu 24.04 LTS |
| Kernel | 6.8.0-124-generic | 6.17.0-19-generic |
| Kubernetes | v1.35.1 (kubeadm) | v1.35.1 |
| Container runtime | containerd 2.2.4 | containerd 2.3.1 |
| CNI | Calico v3.32.0 (tigera-operator v1.42.0), VXLAN, BGP disabled, pod CIDR 10.244.0.0/16 | |

## Worker hardware

| | |
|---|---|
| System | 2-socket Xeon server, 2U rack |
| BIOS | vendor release from early 2025, configured per [01-bios-bkc.md](01-bios-bkc.md) |
| CPU | Intel Xeon 6767P, 2 sockets × 64 cores, **1 thread/core** |
| CPU frequency | 800 MHz min, 3900 MHz max, turbo enabled |
| Logical CPUs | 128 (124 allocatable, `0-3` reserved) |
| NUMA | 2 nodes, one per socket. **node 0 = even CPU IDs, node 1 = odd** (`LAB_SOCKET0_PARITY=even`) |
| L3 cache | 672 MiB total, 2 instances (16 allocatable ways per socket) |
| Memory | 16 × 32 GB DDR5-6400, dual rank — 512 GB installed, ~503 GiB usable, ~257 GB per NUMA node |
| Memory channels | 16 populated (all channels) → 819.2 GB/s theoretical peak |
| `/dev/shm` | 252 GiB tmpfs (MXL flows live here) |
| Root filesystem | 879 GB local storage |
| Network | 2 × 100 GbE (Intel E830-CC) plus 2 × 10 GbE (Intel X550) |

**About the network.** The NIC is not performance-critical for this lab and does
not need to match. Video never crosses the network: the decoder hands frames to
the encoders through MXL shared memory in `/dev/shm/mxl`, and the pod network
carries only Kubernetes control traffic and metrics scrapes. Any supported
NIC with a working driver is fine.

## Verified platform state

| Check | Value |
|---|---|
| Hyper-Threading | disabled (1 thread/core) |
| Sub-NUMA Clustering | disabled (2 nodes for 2 sockets) |
| Turbo | enabled (`intel_pstate/no_turbo=0`) |
| Idle states exposed | `state0`, `state1` only — deep C-states off |
| P-state driver | `intel_pstate`, `status=active` (HWP), `intel_pstate=active` on the kernel command line |
| CPU governor | `performance` on every CPU |
| Energy-performance bias (EPB) | 0 (performance) on every CPU |
| Energy-performance preference (EPP) | requested 0; sysfs reports `performance`, which is what `intel_pstate` pins it to under this governor |
| Efficiency Latency Control (ELC) | `latency` where the part exposes it — PerfSpect marks `--elc` as Sierra Forest and newer, and reports it as unsupported otherwise |
| Power profile persistence | `mxl-power-profile.service` enabled (EPB/EPP/ELC are MSRs and do not survive a reboot) |
| Intel RDT L3 | `num_closids=15  cbm_mask=ffff  min_cbm_bits=1  shareable_bits=c000` |
| Intel RDT MB | `min_bandwidth=10  bandwidth_gran=10` |
| Intel RDT L3_MON | `num_rmids=672` |
| `fs.inotify.max_user_instances` | 8192 (`scripts/install-worker-limits.sh`; the kernel default of 128 caps a run at ~10 streams) |

The six power rows above are set by `scripts/configure-power.sh` from the
`worker power and frequency` block of `config/lab.env`, not by the BIOS alone;
`scripts/configure-power.sh --verify [NODE]` reads them back. Every run records the
live values in `results/<run>/host.json` under `platform_spec.power`, and a
disagreement with `config/lab.env` appears in `host.json` as
`configured_power_mismatch`.

## Kubelet CPU QoS (worker)

```yaml
cpuManagerPolicy: static
cpuManagerPolicyOptions: {full-pcpus-only: "true", strict-cpu-reservation: "true"}
cpuManagerReconcilePeriod: 10s
reservedSystemCPUs: 0-3
topologyManagerPolicy: single-numa-node
topologyManagerScope: container
cgroupDriver: systemd
cgroupsPerQOS: true
```

Verify with `scripts/configure-cpu-qos.sh --verify [NODE]`, which reads the live
configuration through the API server.

Legacy notes that describe `taskset`-based pinning refer to the historical
shared-pool approach used for comparison cases (`numa-pool` or host noise). The
`pinned` reference results on this page use Kubernetes static CPU Manager +
Topology Manager, not manual `taskset` CPU ownership.

## Software versions

| Component | Version |
|---|---|
| FFmpeg + MXL image | built from `cbcrc/guidance-for-building-ffmpeg-with-mxl` @ `9b5098a`, tagged `localhost/mxl-ffmpeg-full:v1` |
| Intel PCM | `intel/pcm` @ `2026-07-08-public`, `pcm-sensor-server` on host port 9738 |
| PerfSpect | 3.17.0_2026-04-20_956744c9 |
| nerdctl | 2.3.2 |
| kube-prometheus-stack | current chart from `prometheus-community` |
| stress-ng | distribution package on the host; `ghcr.io/colinianking/stress-ng:latest` in Pods |
| RDT helper | `mxl-rdt-host` version 8 |

## Reference results

Density, clean machine, 1080p60, preset `medium`, 12 Mb/s cap, pass floor 59.5 FPS:

| Scenario | Streams | Min encoder FPS | CPU/stream (cores) | UPI (GB/s) | L3 hit ratio | DRAM (GB/s) |
|---|---|---|---|---|---|---|
| `baseline` | 12 | 59.78 | 5.77 | 46.0 | 46.7 % | 113.8 |
| `numa-pool` | 14 | 59.71 | 5.01 | 0.55 | 57.8 % | 119.6 |
| `pinned` | 20 | 59.73 | 4.76 | 1.44 | 52.5 % | 196.5 |

RDT QoS validation — the policy that restored ≥ 59.5 FPS for each neighbor:

| Neighbor | Streams | Winning policy |
|---|---|---|
| `host-a` | 20 (`pinned`) | `cat-16-1+mba-20` |
| `pod-a` | 18 (`pinned`) | `mba-20` |
| `pod-b` | 16 (`pinned`) | `mba-20` |
| `pod-c` | 12 (`pinned`) | `cat-16-1+mba-20` |

## What is safe to differ on your hardware

* **Absolute stream counts.** They scale with cores, frequency, memory bandwidth
  and content complexity. The *ordering* `baseline < numa-pool < pinned` is what
  should reproduce.
* **CPU numbering.** Socket 0 may own the odd IDs instead; set
  `LAB_SOCKET0_PARITY=odd`. If each socket owns a contiguous block, set
  `LAB_CPU_NUMBERING=contiguous`. Check `lscpu -p=CPU,NODE` before the first run.
* **Cache ways.** `cbm_mask` is not always `ffff`. The CAT profiles compute masks
  from the live `cbm_mask` and `min_cbm_bits`, so they adapt — but `cat-16-1`
  requires `min_cbm_bits=1`.
* **MBA granularity.** With `bandwidth_gran=20` the `mba-10` profile is
  unavailable; use `mba-20` and up.
* **Server vendor, chassis, BIOS version and NIC.** None of these affect the
  results as long as the BIOS *settings* in [01-bios-bkc.md](01-bios-bkc.md) are
  applied.
* **Control-plane specification.** It only runs the API server and the
  observability stack; a modest machine is enough.
* **OS and containerd version.** Any Ubuntu 22.04 or 24.04 with a matching
  Kubernetes version works.

## What is not safe to differ

* SNC on, deep C-states enabled, or half-populated memory channels. Each of these
  changes the results in a way that is not comparable, and none of them announces
  itself in the FPS number alone. Run `scripts/check-bios.sh` — it catches all of
  them.
* **The power profile.** A `powersave` governor, the `intel_pstate` driver in
  `passive` mode, or a non-zero EPB each cost frequency the encoder needs, and all
  three look exactly like slower hardware. `scripts/check-bios.sh` reports them and
  `scripts/configure-power.sh` fixes them; EPB, EPP and ELC also have to be
  re-applied after every reboot, which is what `mxl-power-profile.service` is for.
* **Hyper-Threading not declared.** SMT on is supported and holds the physical
  footprint of a scenario constant, so the stream count stays comparable — but
  only if `LAB_THREADS_PER_CORE` matches the hardware and `LAB_RESERVED_CPUS`
  covers whole cores. A mismatch either fails admission or hands out half the cores
  the scenario claims. Every run records this in `host.json`.
* **Socket count.** The CPU-pool planner requires exactly two sockets and stops
  with an error otherwise. One-socket and four-socket machines need the planner
  in [python/mxlperf/render.py](../python/mxlperf/render.py) extended first.
* **Intel RDT support.** Without `cat_l3` and `mba` in the CPU flags, steps 11 and
  12 cannot run at all. Steps 1–10 still work.
