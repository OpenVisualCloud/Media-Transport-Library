# 11. Noisy neighbor testing

DMF workloads deployed on shared Kubernetes nodes must tolerate co-tenants. This
step introduces a stressor workload alongside the video streams and measures how
much the co-tenant degrades the DMF SLA, so that step 12 can apply Intel RDT to
recover it.

Step 9 measured a machine that belongs entirely to the video workload. Real
clusters are not like that. This step adds a co-tenant and measures the damage,
so step 12 can measure the repair.

## Run one

```bash
scripts/run.sh pinned --streams 20 --noisy-neighbor host-a --rdt-monitor --warmup 1m --measure 2m
scripts/run.sh pinned --streams 18 --noisy-neighbor pod-a  --rdt-monitor --warmup 1m --measure 2m
scripts/run.sh pinned --streams 16 --noisy-neighbor pod-b  --rdt-monitor --warmup 1m --measure 2m
scripts/run.sh pinned --streams 12 --noisy-neighbor pod-c  --rdt-monitor --warmup 1m --measure 2m
```

Add `--rdt-monitor` to every noisy-neighbor run. Without it you see the FPS drop
but not the cause: RDT is what attributes LLC occupancy and memory bandwidth to
*the noise* versus *the encoders*.

## The four profiles

All four are `stress-ng` memory-bandwidth stressors, so the pressure is on the
shared resources RDT can actually control: last-level cache and memory bandwidth.
They differ in scope and in intensity.

| Profile | Scope | Shape | Reference footprint |
|---|---|---|---|
| [`host-a`](../noisy-neighbors/host-a.env) | **host**, outside Kubernetes | 2 STREAM workers per socket, `taskset` + `numactl --membind` | ~5 busy cores, 284 MiB LLC, 74 GB/s DRAM |
| [`pod-a`](../noisy-neighbors/pod-a.env) | 2 Guaranteed Pods, one per socket | 5 exclusive CPUs each, `--stream 4 --stream-l3-size 32M` | 107–455 MiB LLC, 129–205 GB/s DRAM |
| [`pod-b`](../noisy-neighbors/pod-b.env) | 2 Guaranteed Pods, one per socket | 12 exclusive CPUs each, `--stream 24 --stream-l3-size 196M` | 471 MiB LLC, 279 GB/s DRAM |
| [`pod-c`](../noisy-neighbors/pod-c.env) | 2 Guaranteed Pods, one per socket | 24 exclusive CPUs each, `--stream 24 --stream-l3-size 512M` | 520 MiB LLC, 462 GB/s DRAM, worker L3 hit ratio down to ~14 % |

### One neighbor per socket

The pod profiles set `NOISY_NEIGHBOR_POD_COUNT=2` and
`NOISY_NEIGHBOR_REQUIRE_SOCKET_SPLIT=1`: two neighbor Pods, one on each socket. A
neighbor on only one socket produces a lopsided result that is impossible to
compare.

Getting that split in a dense `pinned` run needs care, so the runner launches in
stages: first neighbor → the whole video workload → remaining neighbors, then it
verifies the achieved split by reading each Pod's effective cpuset and mapping CPU
IDs to sockets. If the split fails it retries placement
(`NOISY_NEIGHBOR_SOCKET_SPLIT_RETRIES=2`), and with `REQUIRE_SOCKET_SPLIT=1` it
aborts rather than record an incomparable row. The summary reports what happened:

```
NN socket placement            split | same-socket | partial-split | mixed-or-multisocket
NN same NUMA socket            False
NN precheck socket placement   split
```

Only trust rows that say `split`.

The split only happens when the machine is dense enough to force it. At the
campaign stream counts below, socket 0 is too full for both neighbors, so the
kubelet has to put the second one on socket 1. In a small smoke test
(`--streams 4`) both fit on one socket and the run aborts with:

```
warning: noisy-neighbor pods did not reach clean socket split after staged launch
FATAL: noisy-neighbor socket split required but status=same-socket
```

That is the guard working, not a bug. For a quick end-to-end check of the noisy
neighbor and RDT plumbing use `host-a`, which pins itself to both sockets and so
needs no scheduler cooperation:

```bash
scripts/run.sh pinned --streams 4 --noisy-neighbor host-a --rdt-control mba-20 \
  --warmup 20s --measure 40s
```

For the Pod profiles, use the campaign stream counts.

### One neighbor per worker instead

Everything above keeps both neighbors on the worker under measurement, which is
what every published row in this repo used. On a cluster with more than one worker
you can spread them one per node instead:

```bash
scripts/run.sh pinned --streams 20 --noisy-neighbor pod-b \
  --set NOISY_NEIGHBOR_SPREAD_BY_NODE=1
```

That replaces the fixed node selector with required hostname anti-affinity limited
to the nodes in `LAB_WORKERS`, so no two neighbor Pods may share a node. If there
are fewer eligible workers than `NOISY_NEIGHBOR_POD_COUNT`, the surplus Pods stay
`Pending` and the run blocks waiting for them — check with
`kubectl -n mxl-perf get pods -l role=noisy-neighbor -o wide`.

Two things to know before using it:

* It is a **different experiment**. A neighbor on another machine shares no LLC,
  no memory controller and no interconnect with the encoders, so the result is not
  comparable with the rows in [09-density.md](09-density.md). Use it to measure
  cluster-level effects, not platform contention.
* The staged launch and the socket-split check are skipped, because the neighbors
  are no longer on the measured worker. `NOISY_NEIGHBOR_REQUIRE_SOCKET_SPLIT` has
  no effect in this mode even though the pod profiles set it to `1`.

## Reading the damage

Compare a noisy row against the clean row for the same scenario and stream count:

| Column | What it tells you |
|---|---|
| `Minimum encoder FPS` | the verdict — below 59.5 the run fails |
| `RDT noise LLC occupancy (MiB)` | how much cache the neighbor took |
| `RDT noise total MBM (GB/s)` | how much memory bandwidth it is consuming |
| `RDT encoder LLC occupancy (MiB)` | how much cache the encoders were left with |
| `Whole worker L3 cache hit ratio (%)` | the cost of that eviction |
| `Whole worker DRAM read+write (GB/s)` | total pressure on the memory controllers |
| `Measured DRAM share of theoretical peak (%)` | how close the platform is to its bandwidth ceiling |

The mechanism, in one sentence: the neighbor's streaming working set evicts the
encoders' reference frames from L3, every eviction becomes a DRAM read, DRAM
latency rises with queueing, and the encoder misses its frame deadline.

Reference behaviour at 20 pinned streams with `host-a` and no RDT control: minimum
FPS falls from 59.7 to ~58.1 while the neighbor holds ~287 MiB of LLC and 76 GB/s
of memory bandwidth, and the worker's L3 hit ratio drops from 52.5 % to ~40 %.
20 streams no longer fit.

## Choosing a stream count

Use the highest count that passes *cleanly* for the scenario, then add the
neighbor. If the pair passes, the platform tolerates that co-tenant; if not, step
12 is the question of whether RDT can recover it. The prepared RDT campaigns pick
counts that make each case interesting rather than hopeless: 20 for `host-a`, 18 for `pod-a`, 16 for `pod-b`, 12 for `pod-c`.

## Safety rails

The profiles are deliberately constrained, and the runner rejects attempts to
loosen them:

* `NOISY_NEIGHBOR_HOST_EXTRA_ARGS` may not contain `--all`, `--sequential`,
  `--class`, `--taskset`, `--timeout` or any `--stream*`/`--cache*` option. Those
  either fight the profile's own affinity and lifecycle management or turn a
  bounded stressor into an unbounded one:
  ```
  FATAL: host noisy-neighbor extra arguments contain lifecycle, affinity,
  or broad stressor options
  ```
* Pod neighbors run unprivileged with a read-only root filesystem.
* Host noise is supervised with a PID file and stopped in the run's `finally`
  block, so a failed or interrupted run cannot leave `stress-ng` behind. Its full
  `stress-ng` log is saved as `noisy-neighbor.log`.
* Neighbor evidence is captured twice, before and after the measurement window
  (`noisy-neighbor-before.json` / `-after.json`), including each Pod's effective
  cpuset, `cpu.stat` and process list. If a neighbor died mid-run the run fails
  instead of reporting a flatteringly quiet result.

Next: [12-rdt-qos.md](12-rdt-qos.md).
