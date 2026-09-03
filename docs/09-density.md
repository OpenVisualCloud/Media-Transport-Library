# 9. DMF density benchmarking — three placement cases

The headline measurement: how many live 1080p60 streams one worker sustains under
each CPU QoS policy. This step answers the core DMF question: **how does
Kubernetes CPU placement configuration affect the density of DMF media workloads?**

Three cases are benchmarked, all running the same FFmpeg command line on the same
hardware. Only the Kubernetes placement policy changes.

## Run the campaign

```bash
scripts/run-campaign.sh campaigns/density.env
```

Three runs, about 12 minutes total ([campaigns/density.env](../campaigns/density.env)):

```
baseline  --streams 12 --rdt-monitor --warmup 1m --measure 2m
numa-pool --streams 14 --rdt-monitor --warmup 1m --measure 2m
pinned    --streams 20 --rdt-monitor --warmup 1m --measure 2m
```

Then open the summary:

```
results/summary.html      one row per run, colour-coded pass/fail
results/summary.xlsx      same data, plus a platform sheet
results/summary.csv       same data, for scripting
```

A single run, if you prefer:

```bash
scripts/run.sh pinned --streams 20 --rdt-monitor --warmup 1m --measure 2m
scripts/run.sh pinned --streams 20 --dry-run          # render and validate only
```

## The three cases

| | `baseline` | `numa-pool` | `pinned` |
|---|---|---|---|
| Kubernetes QoS | Burstable | Burstable | **Guaranteed** |
| CPU request (dec / enc) | 500m / 500m | 500m / 500m | 1 / 5 |
| CPU affinity | none — scheduler and CFS decide | `taskset` to one socket's 62 CPUs | exclusive cores from the kubelet |
| NUMA alignment | none | by hand, sessions alternate sockets | kubelet Topology Manager, `single-numa-node` |
| Needs step 3 | no | no | **yes** |
| Encoder threads | 15, slices 2 | 15, slices 2 | 15, slices 2 |

All three run the *same FFmpeg command line* with the same threading. Only
placement changes. That is the point: the difference is configuration, not
tuning.

### Reference result (reference platform: 2 sockets × 64 cores, see [14-reference-bkc.md](14-reference-bkc.md))

| Scenario | Streams | Min encoder FPS | CPU per stream (cores) | Cross-socket UPI (GB/s) | L3 hit ratio | DRAM total (GB/s) |
|---|---|---|---|---|---|---|
| `baseline` | 12 | 59.78 | 5.77 | 46.0 | 46.7 % | 113.8 |
| `numa-pool` | 14 | 59.71 | 5.01 | 0.55 | 57.8 % | 119.6 |
| `pinned` | **20** | 59.73 | 4.76 | 1.44 | 52.5 % | 196.5 |

Read it as a chain of cause and effect:

1. **`baseline` wastes the interconnect.** 46 GB/s of cross-socket UPI traffic:
   FFmpeg threads migrate between sockets and keep reading memory that is now
   remote. That traffic buys nothing.
2. **`numa-pool` removes it almost entirely** (46 → 0.55 GB/s) with nothing but
   `taskset`. Cost per stream falls from 5.77 to 5.01 cores and the L3 hit ratio
   rises. Two more streams fit, on any cluster, with no kubelet configuration.
3. **`pinned` converts locality into density.** Exclusive cores stop CFS from
   migrating threads at all, so per-stream cost drops again to 4.76 cores and 20
   streams fit. The residual 1.44 GB/s UPI is the honest cost of a workload that
   *is* spread over both sockets, not thrash.

+67 % density, same silicon, same encoder settings.

## Why 20 streams and not more

`pinned` gives each stream 6 exclusive CPUs — 1 for the decoder, 5 for the
encoder (`scenarios/pinned.env`). With 124 allocatable CPUs (128 − 4 reserved):

```
20 streams × 6 CPUs = 120 exclusive CPUs  +  20 × 2 sidecars × 5m ≈ 0.2 CPU
```

21 streams needs 126 and the planner refuses before anything is deployed:

```
FATAL: Kubernetes CPU Manager placement needs 126 exclusive CPUs plus 0.2 shared
CPUs, but only 124 CPUs are allocatable
```

The encoder runs **15 threads on 5 cores** with `slices=2` and
`sliced-threads=0` — frame-based threading. Oversubscribing threads to cores is
deliberate: libx264 frame threads spend much of their life waiting on references,
so 3× oversubscription fills the cores without adding slice-boundary quality loss.
Sliced threading with fewer slices than threads is rejected outright:

```
FATAL: SLICES=2 limits sliced-thread parallelism below ENC_THREADS=15;
use SLICES >= ENC_THREADS or SLICED_THREADS=0 for frame threading
```

The limit really is CPU count, not memory: at 20 streams the reference worker
draws 137.9 GB/s of the 819.2 GB/s its 16 populated channels can deliver — 16.8 %
of peak. A machine with more cores at the same memory configuration should scale
further, which is why `install-platform-probe.sh` is worth having.

## Finding the limit on your own hardware

Different silicon has a different number. Walk it up:

```bash
scripts/run-campaign.sh campaigns/density-sweep.env
```

Three stream counts per scenario. The limit is the highest count whose row still
shows `Pass`. Then edit `campaigns/density.env` to your numbers and use it as your
own regression baseline.

To tune the per-stream core split, change `DEC_CORES` / `ENC_CORES` /
`ENC_THREADS` in `scenarios/pinned.env`, or override for one run:

```bash
scripts/run.sh pinned --streams 24 --enc-cores 4 --enc-threads 12 --measure 2m
```

## Troubleshooting

### Too many open files

Decoders come up, then die around the tenth stream, and their logs end with:

```
inotify_init1 failed: Too many open files
Failed to create instance : inotify_init1 failed: Too many open files
[out#0/mxl] Could not write header (incorrect codec parameters ?): Input/output error
Conversion failed!
```

Nothing is wrong with the codec or the media. Every MXL flow holds one inotify
instance, and `fs.inotify.max_user_instances` defaults to 128 for *all* processes
running as root on the host — kubelet, containerd, systemd and journald have
already taken most of them, so a dense run runs out. Fix it once per worker:

```bash
scripts/bootstrap-worker.sh          # includes install-worker-limits.sh
```

`scripts/preflight.sh` checks the value, so a run never has to fail to find out.

## What each run leaves behind

`results/<scenario>-<placement>-<N>str-<UTC>/`:

| File | Contents |
|---|---|
| `report.html` | the single-run report: per-stream FPS, CPU, cache, memory |
| `config.json` | every resolved setting, so the run is reproducible |
| `workload.yaml` | the exact manifests that were applied |
| `ffmpeg-commandlines.json` | the exact FFmpeg command line per Pod |
| `planned-placement.json` | which CPUs each container was meant to get |
| `metrics.csv` | every metric sample in the measurement window |
| `host.json` | live hardware probe: `lscpu -J`, `numactl --hardware`, meminfo, cmdline |
| `pod-describe.txt` | `kubectl describe pods`, including the real cpusets |
| `rdt-*.json` | RDT groups, masks and per-group counters (with `--rdt-monitor`) |
| `noisy-neighbor-*.json` | neighbor evidence before and after (with `--noisy-neighbor`) |

Warm-up samples are excluded from the score. `--warmup 1m --measure 2m` means the
first minute is discarded and the pass/fail decision uses the second and third.

The namespace is deleted after each run unless you pass `--keep`.

Next: [10-metrics.md](10-metrics.md) — understand and interpret your benchmark results.
