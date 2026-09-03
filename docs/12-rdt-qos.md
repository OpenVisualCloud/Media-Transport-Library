# 12. Intel RDT isolation — CAT and MBA policies

Intel Resource Director Technology (RDT) is the mechanism DMF deployments use to
enforce resource isolation between media workloads and co-tenants. It does two
things this lab needs: it *attributes* cache and memory bandwidth to specific
processes, and it *limits* them.

Step 11 measured the damage a co-tenant does. This step takes it back.


## Two modes

```bash
# Monitor only: attribute LLC and bandwidth, change nothing
scripts/run.sh pinned --streams 20 --noisy-neighbor host-a --rdt-monitor \
  --warmup 1m --measure 2m

# Control: apply a policy to the neighbor (monitoring is implied)
scripts/run.sh pinned --streams 20 --noisy-neighbor host-a --rdt-control mba-20 \
  --warmup 1m --measure 2m
```

`--rdt-monitor` creates `mon_groups` under `/sys/fs/resctrl` and samples counters
once a second for the measurement window. `--rdt-control <profile>` creates real
control groups with schemata that restrict the noise group.

Groups the runner creates:

| Group | Tasks | Treatment under control |
|---|---|---|
| `mxl-encoder` | every encoder container's threads | protected: full cache, MB 100 |
| `mxl-decoder` | every decoder container's threads | protected: full cache, MB 100 |
| `mxl-workload` | encoders + decoders together | protected |
| `mxl-noise` | the noisy neighbor's threads (Pods or the host `stress-ng` tree) | **restricted by the profile** |

Only the noise group is ever restricted. The video workload always keeps its full
allocation — this measures protecting a tenant, not throttling one.

Narrow the monitoring to one session when you want per-stream detail:

```bash
scripts/run.sh pinned --streams 20 --rdt-monitor --rdt-focus-session s01
```

## The profiles

### CAT — L3 Cache Allocation Technology

Partitions cache *ways* between the noise and the workload by writing L3 bitmasks.
The reference worker reports `cbm_mask=ffff`, i.e. 16 allocatable ways with
`min_cbm_bits=1`.

| Profile | Workload mask | Noise mask | Meaning |
|---|---|---|---|
| `cat-guarded` | `fff0` (12 ways) | `f` (4 ways) | Workload keeps 75 % of the cache; noise is confined to a quarter. |
| `cat-strong` | `fffc` (14 ways) | `3` (2 ways) | Workload keeps 87.5 %; noise gets almost nothing. |
| `cat-16-1` | `ffff` (all 16) | `1` (1 way) | Not a partition: the workload keeps **all** ways and the noise is confined to a single way it *shares*. |

`cat-16-1` deserves the explanation. `cat-guarded` and `cat-strong` are strict
partitions: the workload can never use the ways given to the noise, so a
guaranteed-but-smaller cache can be worse than a large shared one. `cat-16-1`
gives the noise one way while leaving the workload's mask complete, so the
workload can still use all 16 ways when the noise is idle. On the reference
campaign this is what wins against the heaviest neighbors.

### MBA — Memory Bandwidth Allocation

Throttles the noise group's memory bandwidth to a percentage. `mba-80`, `mba-60`,
`mba-40`, `mba-20`, `mba-10`; the reference platform reports
`min_bandwidth=10 bandwidth_gran=10`, so 10 % steps are the hardware granularity.
The workload group is always set to `MB:100`.

MBA throttles *the group's requests*, not the memory controller. It reduces
queueing delay for everyone else, which is exactly the latency the encoder feels.

### Combinations

Any CAT profile plus any MBA level: `cat-16-1+mba-20`, `cat-guarded+mba-80`,
`cat-strong+mba-40`, … Both resources are set in the same control group. Use a
combination when neither alone is enough: CAT stops the eviction, MBA stops the
queueing.

## Sweep it properly

One prepared campaign per neighbor. Each starts with an unmanaged reference row,
then repeats the identical workload with one policy applied, so rows differ by
exactly one variable:

```bash
scripts/run-campaign.sh campaigns/rdt-host-a.env     # 20 streams
scripts/run-campaign.sh campaigns/rdt-pod-a.env      # 18 streams
scripts/run-campaign.sh campaigns/rdt-pod-b.env      # 16 streams
scripts/run-campaign.sh campaigns/rdt-pod-c.env      # 12 streams
```

Seven rows each: `--rdt-monitor` (no control), `mba-80`, `mba-20`,
`cat-guarded`, `cat-strong`, `cat-16-1`, `cat-16-1+mba-20`. About 25 minutes per
campaign.

## Reading the result

For each row, in this order:

| Column | Expected effect |
|---|---|
| `RDT noise LLC occupancy (MiB)` | **falls** when CAT is applied — proof the policy took effect |
| `RDT noise total MBM (GB/s)` | **falls** when MBA is applied |
| `RDT encoder LLC occupancy (MiB)` | **rises** — the cache the encoders got back |
| `Whole worker L3 cache hit ratio (%)` | recovers toward the clean-run value |
| `Minimum encoder FPS` | ≥ 59.5 means the policy is a fix, not just an improvement |

First check that the policy *did* something (the two noise columns), then whether
it was enough (FPS). A policy that changes the noise counters but not the FPS
tells you the bottleneck is elsewhere — usually stolen CPU time, which RDT cannot
address.

Reference outcome of the four campaigns:

| Neighbor | Policy that restored ≥ 59.5 FPS |
|---|---|
| `host-a` (host scope, also steals CPU) | `cat-16-1+mba-20` |
| `pod-a` (light, 5 CPUs/socket) | `mba-20` |
| `pod-b` (heavy, 12 CPUs/socket) | `mba-20` |
| `pod-c` (worst case, 24 CPUs/socket) | `cat-16-1+mba-20` |

The pattern: **bandwidth throttling does most of the work**. Cache partitioning
alone rarely rescues a run, because the encoder's problem is DRAM latency under
queueing, not cache capacity as such. Against the most aggressive neighbors,
adding `cat-16-1` on top of `mba-20` supplies the last few tenths of a frame per
second — and `cat-16-1` beats the strict partitions precisely because it does not
take ways away from the workload.

## Requirements and safety

Set up by `scripts/install-rdt-host.sh` (run for you by
`scripts/bootstrap-worker.sh`):

* `/sys/fs/resctrl` mounted.
* `/usr/local/sbin/mxl-rdt-host` — the RDT helper; `scripts/update-rdt-helper.sh`
  refreshes it.
* CPU flags `rdt_a cat_l3 mba cqm_occup_llc cqm_mbm_total cqm_mbm_local`, from
  BIOS ([01-bios-bkc.md](01-bios-bkc.md)).

Check any time:

```bash
scripts/check-rdt-host.sh                # capabilities, helper version, CLOS/RMID counts
```

Capabilities on the reference platform:

```
L3       num_closids=15  cbm_mask=ffff  min_cbm_bits=1  shareable_bits=c000
MB       min_bandwidth=10  bandwidth_gran=10
L3_MON   num_rmids=672
```

`num_closids=15` is the ceiling on simultaneous control groups — with four groups
per run there is ample headroom.

Every run is *scoped*: groups are created at the start, torn down at the end, and
the run fails loudly if restoration did not complete:

```
FATAL: RDT cleanup/restoration failed: {...}
```

`rdt-start.json` and `rdt-stop.json` in the result directory record the schemata
actually written and the state restored, so a policy claim in a report can always
be checked against what the kernel was told.

Next: [13-mcp-profiling.md](13-mcp-profiling.md).
