# 10. Results and interpretation

After running `scripts/run-campaign.sh campaigns/density.env`, open:

```
results/summary.html      one row per run, colour-coded pass/fail
results/summary.xlsx      same data, plus a platform sheet
results/summary.csv       same data, for scripting
```

This page explains what every column means, where it comes from, and how to
interpret results in the DMF context. The key question is always: **which
placement policy allows how many DMF streams to sustain their SLA?**

## Reading a result in 30 seconds

1. `Pass` — did every stream hold ≥ 59.5 FPS? This is the DMF SLA verdict.
2. `Encoder CPU busy ÷ Streams` — what did one stream cost? Lower is better.
3. `Cross-socket UPI` — is the placement sane? (tens of GB/s means it is not)
4. `L3 hit ratio` and `DRAM share of theoretical peak` — is memory the limit?
5. `RDT noise` columns — is a co-tenant taking the machine?

## Metrics reference

What every column in `results/summary.html` (and `summary.csv` / `summary.xlsx`)
means, where it comes from, and how to read it.

## The verdict

| Column | Source | Meaning |
|---|---|---|
| `Minimum encoder FPS` | FPS sidecar → Prometheus | The **lowest** per-stream average FPS across the measurement window. Minimum, not mean: one starved stream is a failed run. |
| `Pass` | computed | `Minimum encoder FPS ≥ LAB_MIN_FPS` (59.5). Green/red in the HTML. |

Warm-up samples are excluded. `--warmup 1m --measure 2m` discards the first
minute and scores the remaining two.

## Configuration echo

| Column | Meaning |
|---|---|
| `Scenario` | `baseline`, `numa-pool` or `pinned` |
| `CPU placement` | `free`, `numa-pool`, `exclusive` — the planner mode behind the scenario |
| `Streams` | concurrent decoder+encoder pairs |
| `Resolution`, `Preset`, `Bitrate` | 1080p/4k, libx264 preset, `-maxrate` |
| `Encoder cores per session` | physical cores each encoder container actually had — `ENC_CORES`. With SMT on the container's CPU request is twice this, and it owns both threads of each core |
| `Encoder threads`, `Slices`, `Sliced threads` | `-threads`, `slices=`, `sliced-threads=` |
| `x264 overrides` | extra `-x264-params`, if any |
| `Worker node`, `Result directory` | where it ran, where the raw evidence is |

## CPU

| Column | Source | Meaning |
|---|---|---|
| `Encoder CPU busy (core-equivalents)` | cAdvisor container CPU seconds | Total CPU the encoders consumed, in whole cores. Divide by `Streams` for cost per stream — the density metric that matters. |
| `Decoder CPU busy (core-equivalents)` | cAdvisor | Same for decoders. On the reference platform the decoder is ~0.43 cores per stream. |
| `Encoder-CPU real total usage (avg %)` | node-exporter, per CPU | Utilisation of the logical CPUs the encoders were assigned, *including anything else running there*. In `pinned` this should equal the encoder's own usage; if it is higher, something is stealing exclusive cores. With SMT on the average is over both threads of each core. |
| `Encoder avg utilization per used CPU (%)` | node-exporter | How hard each used CPU worked. Low values with low FPS mean serialisation, not saturation. |
| `Decoder-*` equivalents | | Same for decoders. |

Encoder cost per stream is the cleanest single number for comparing
configurations: 5.77 → 5.01 → 4.76 cores across the three cases.

## Memory and interconnect (Intel PCM, whole worker)

| Column | PCM metric | Meaning |
|---|---|---|
| `Cross-socket UPI incoming bandwidth (GB/s)` | `Incoming_Data_Traffic_On_Link_0..3` | Data crossing between sockets. **The `numa-pool` headline**: 46.0 → 0.55 GB/s. High values mean threads are reading memory that belongs to the other socket. |
| `Cross-socket UPI incoming (GB/s per stream)` | derived | Normalised, so runs at different densities are comparable. |
| `Whole worker L3 cache hit ratio (%)` | `L3_Cache_Hits`, `L3_Cache_Misses` | Falls when a noisy neighbor evicts encoder working sets. Under `pod-c` it collapses to ~14 %. |
| `Whole worker L3 misses (M/s)` | `L3_Cache_Misses` | Absolute miss rate; every miss becomes a DRAM access. |
| `Whole worker DRAM read / write / read+write (GB/s)` | `DRAM_Reads`, `DRAM_Writes` | Memory controller traffic. |
| `Whole worker DRAM read+write (GB/s per stream)` | derived | Per-stream memory cost. |
| `Theoretical DRAM peak (GB/s)` | DMI | `populated channels × MT/s × 8`. Needs `scripts/install-platform-probe.sh`; shows `unavailable` otherwise. |
| `Measured DRAM share of theoretical peak (%)` | derived | How close the platform is to its bandwidth ceiling. Above ~70 % latency rises sharply and encoders start missing deadlines. |

These are **whole-socket uncore counters**, so they include everything on the
machine — that is exactly why they catch a noisy neighbor that per-container
metrics cannot see.

## Intel RDT (per cgroup)

Present when the run used `--rdt-monitor` or `--rdt-control`.

| Column | Meaning |
|---|---|
| `RDT monitor` / `RDT control profile` | whether RDT was active, and which policy |
| `RDT workload LLC occupancy (MiB)` | cache held by encoders + decoders together |
| `RDT workload-group MBM total (GB/s)` | memory bandwidth used by the video workload |
| `RDT encoder / decoder LLC occupancy (MiB)` | split by role |
| `RDT encoder / decoder-group MBM total (GB/s)` | split by role |
| `RDT noise LLC occupancy (MiB)` | cache held **by the neighbor** — falls when CAT is applied |
| `RDT noise total MBM (GB/s)` | bandwidth used **by the neighbor** — falls when MBA is applied |
| `RDT focus session` | set when monitoring was narrowed to one stream |

CMT (`llc_occupancy`) is a sampled occupancy, not a rate: it says how much of the
cache that group currently holds. MBM (`mbm_total_bytes`) is a counter, reported
here as an averaged rate.

## Noisy neighbor

| Column | Meaning |
|---|---|
| `Noisy neighbor` | `enabled` or blank |
| `Noise profile` | `host-a`, `pod-a`, `pod-b`, `pod-c` |
| `NN socket placement` | `split` (one neighbor per socket — the only comparable case), `same-socket`, `partial-split`, `mixed-or-multisocket`, `n/a` |
| `NN same NUMA socket` | `True` means both neighbors landed on one socket |
| `NN precheck socket placement` | placement observed before the measurement window, for comparison with the final state |
| `stress-ng image` / `stress-ng arguments` | exactly what the neighbor ran |

## Platform sheet

`summary.xlsx` carries a second sheet with the worker's identity: CPU model,
sockets, cores per socket, threads per core, logical CPUs, NUMA nodes, max MHz,
installed memory, populated DIMMs and channels, DIMM type and size, memory
transfer rate and its source, and the theoretical DRAM peak. It is collected live
per run (`host.json`) so a result can never be attributed to the wrong machine.


`unavailable` in a column means the metric source was not there — usually PCM
([06-observability.md](06-observability.md)) or the DMI probe. It never means zero.

Next: [11-noisy-neighbor.md](11-noisy-neighbor.md) — test co-tenant interference.
