# Performance comparison of two MTL builds (perf-compare)

perf-compare runs two instances of the same end-to-end workload at the same time, one per
MTL build or configuration, and reports the difference between them. An instance is a TX
application on one host and an RX application on the other. Each instance has a PF of its
own on both hosts, CPU cores of its own, L3 cache ways of its own and a build tree of its
own. Both are measured through USDT, including the scheduler counters of
`mtl_sch_get_stats()`.

It answers "is build B slower or faster than build A, and where", for example the nightly
"main today against main yesterday", a branch against `main`, or, by hand (§6.4), `v26.09`
against `v26.01`.
It also compares one build with two configurations, such as two values of
`--sch_session_quota`. It does not search for a session ceiling; `perf-pytest.yml` does
that.

This page is the design and the plan. Section 9 tracks what is implemented and proved.

## 1. Requirements

| # | Requirement | Where it is met |
| --- | --- | --- |
| R1 | Two instances, each a TX app on one host and an RX app on the other | §3 |
| R2 | Each instance on a PF of its own, on both hosts | §4.1 |
| R3 | Each instance on lcores of its own, isolated from the other | §4.2, §4.3 |
| R4 | The instances must not interfere with each other | §4 |
| R5 | Measure with USDT and the scheduler stats of `mtl_sch_get_stats()` | §5 |
| R6 | Choose a commit, tag or branch per instance; `main` is the default reference | §6 |
| R7 | Every night: `main` of the day before against `main` of the day | §6.3 |
| R8 | Same version, different configuration | §6.1 |
| R9 | Instances as equal as possible | §3.2 |
| R10 | Optionally other frameworks: RxTxApp, FFmpeg, GStreamer | §7 |

## 2. Decisions

| Question | Decision | Why |
| --- | --- | --- |
| Run A and B one after the other, or at the same time? | At the same time, then again with the slots swapped | Concurrent runs share every time-varying disturbance (the shared switch floods up to 47 Gb/s of other runners' multicast, other jobs, thermal state) as common mode, which a sequential run turns into a difference. The swap cancels what differs between the two slots (§4.6). |
| Extend `perf-pytest.yml` or a new workflow? | A new workflow, `perf-compare.yml` | perf-pytest restores one tree and searches a capacity ceiling for hours; perf-compare builds and restores two trees and reports a difference after minutes. One workflow for both would give every input two meanings. Both need the `sut` runner, so GitHub runs them one after the other. |
| A pytest case or a standalone tool? | A pytest case in `tests/acceptance/tests/dual/performance/` | The workflow overlays `tests/acceptance` from the workflow commit onto each ref, so both builds run the same harness. The case reuses the topology, the VFs on both PFs, the media ramdisk, the RxTxApp config builder and the log layout. |
| How do two builds that expect the same path run side by side? | Each instance runs in a private mount namespace with its tree bind-mounted over `<mtl>/.local_install` | DPDK bakes `RTE_EAL_PMD_PATH` into `librte_eal` as an absolute path with no override, so a moved tree loads the wrong drivers or none; `v26.01` pins DPDK 25.11 and `main` 26.07. |
| How are the CPUs isolated? | A transient systemd unit per instance whose `AllowedCPUs` (a cgroup v2 cpuset) is the slot, no `--lcores`, app threads off the scheduler CPUs | §4.2. `taskset` isolates nothing: `numa_bind()` in `mtl_init()` widened a `[2,3]` mask to `0-31` on the test host. `isolate.sh` holds one host-wide lock for its one partition, so two instances cannot use it at once. |
| How is the cache shared? | Intel RDT CAT: each slot gets an equal, exclusive set of L3 ways | Without it the instance with the larger footprint evicts the other one, and the slot swap does not cancel that: it follows the instance, not the slot. |
| What is measured? | The MTL stats dump through the USDT probe `sys:log_msg`, the tasklet counters through the new probe `sys:tasklet_stat`, per-thread CPU from `/proc`, memory bandwidth from RDT | §5. `sys:log_msg` fires whatever the log level of the app is and exists in every release since 2024, so `v26.01` is measured the same way as `main`. |
| Which application? | RxTxApp now; FFmpeg and GStreamer can reuse the instance and measurement code | §7 |

## 3. Topology and one round

### 3.1. Roles

The SUT (`mtl-runner-7`, the `sut` label) runs the RX application of both instances and
the companion (`mtl-runner-8`) runs the TX application of both. Both hosts are measured.

```text
            companion (runner-8)                  SUT (runner-7)
slot 0   TX  VF of PF 15:00.0, CPUs 2-9   ──────▶  RX  VF of PF 15:00.0, CPUs 4-11
slot 1   TX  VF of PF 15:00.1, CPUs 10-17 ──────▶  RX  VF of PF 15:00.1, CPUs 12-19
round 1: A in slot 0, B in slot 1        round 2: B in slot 0, A in slot 1
```

### 3.2. Equal instances

Both instances get the same RxTxApp JSON except the interface name and the IPs, the same
number of CPUs, and the same base arguments, `--dedicated_sys_lcore --disable_migrate`,
the settings of the perf-pytest measured app. Migration is off because it changes the
number of cores during the run. `args_a` and `args_b` are appended to the instance they
name; they are the only intended difference besides the build.

### 3.3. One round

1. Start the RX application of both instances on the SUT, then the TX application of both
   on the companion, each as a transient systemd unit (§4). Attach the `sys:log_msg`
   collector to each of the four processes (§5.1).
2. Wait until the last stats dump of every instance shows every session at 99% of its
   frame rate (steady). Move the application threads of each instance off its scheduler
   CPUs (§4.2), then wait the warm-up (30 s).
3. Untraced window (`test_time`): only `sys:log_msg` is attached. `/proc`, the RDT
   counters and the monotonic clock are read at both ends; the window is that span for
   all four processes.
4. Traced window (`test_time / 2`): `sys:tasklet_time_measure` and, where the build has it,
   `sys:tasklet_stat` are attached as well, so the scheduler times every tasklet.
5. Stop the TX applications, then the RX applications, with SIGINT, and copy the
   application logs and collector records of the round to the pytest log folder.

Two rounds is the default; round 2 swaps the slots. The NIC MAC counters of each PF are
read before and after each round, so a loss on the way between the hosts is told apart
from a loss in the receiver.

## 4. Isolation

### 4.1. NIC

Each slot uses a VF of its own PF on both hosts: slot 0 is `host.vfs[0]` (PF 0), slot 1 is
`host.vfs_r[0]` (PF 1). The E830 has a PCIe x16 Gen5 link, far above two 100G ports. Each
slot has its own subnet (slot 1 takes the redundant pools of `ip_pools`) and unicast
streams, so no stream of one slot reaches the other.

### 4.2. CPU

`cpu_layout()` in `mtl_engine/perf_compare.py` computes the slots on each host from its
topology:

1. Take the NUMA node of the VF and the first hardware thread of each of its physical
   cores, so no slot uses an SMT sibling of a core of the other slot.
2. Leave the first two cores to the OS (the collectors run on CPU 1).
3. Give each slot 8 cores, from `isolcpus` when the node isolates 16 or more
   (`mtl-runner-7`: `isolcpus=4-20`), else from the cores that are not isolated, so both
   slots of a host are always treated alike.

The unit has `AllowedCPUs=<slot>`, `AllowedMemoryNodes=<node>` and `Nice=-20`, and the
application gets no `--lcores`:

- With `--lcores`, MTL passes `-l 0,<lcores>` to EAL, which pins the main lcore to CPU 0
  and treats a failure to do so as fatal (`rte_eal_init`, "Cannot set affinity"). Every
  instance would need CPU 0.
- Without it, EAL takes its lcores from the affinity mask of the process and the main
  lcore is the lowest CPU, so the cpuset alone places everything.
- The process starts on the first CPU of the slot (`taskset -c <first>`) and its mask is
  then widened to the slot (`taskset -c <slot>`), the start CPU trick of `isolate.sh`.
  EAL reads the wide mask, the schedulers take the other CPUs, and the application and
  control threads start where the main thread runs. The kernel clamps the `numa_bind()`
  of `mtl_init()` to the cpuset.
- Once the instance is steady, every thread that may still run anywhere in the slot (the
  application, pipeline and MTL admin threads) gets the slot's CPUs that no scheduler
  uses. CFS would otherwise move one onto a CPU a scheduler polls.

Measured on the test host for an RX instance on CPUs 8-11: `mtl_sch_0` on 9, `mtl_sch_1`
on 10, both pinned; the EAL control threads on 8; the application threads allowed 8-11
until the move, then 8 and 11. The move took the scheduler preemptions from about 150 to
about 15 a second there.

### 4.3. Software state

`unshare --mount --ipc` gives each instance:

- its own build: the slot tree is bind-mounted over `<mtl>/.local_install`, and
  `LD_LIBRARY_PATH` points into it, so even a tree whose DPDK has other sonames than the
  ones in the linker cache loads its own libraries;
- no MtlManager: a tmpfs over `/var/run/imtl` hides the daemon's socket, so a `v26.01`
  library never talks to a `main` MtlManager. MTL falls back to its SysV shared memory
  lcore table, which is private to the IPC namespace ("clear shm as we are the first
  user" in the log).

DPDK already runs with `--in-memory`, so the instances share no hugepage files.

### 4.4. Cache and memory bandwidth

When `/sys/fs/resctrl` offers L3 allocation and no other user has groups there, the round:

1. creates the groups `mtl_perf_0` and `mtl_perf_1` with `ways / 3` L3 ways each, at the
   end of the mask without the `shareable_bits`: those are the ways the NIC writes into
   (DDIO), which both instances use and which must not belong to one of them;
2. gives the rest of the ways, the DDIO ones included, to the default group, after saving
   its mask to `/run/mtl-perf-compare.schemata`;
3. puts each instance into its group before `exec`, so every thread it creates inherits
   the group.

On the test host (12 ways, `shareable_bits` `c00`) the slots get `00f` and `0f0`, and
the default group `f00`, which holds the DDIO ways. The groups are removed and the default
mask restored at the end of the case, before the next setup, and by
`.github/scripts/ci/cleanup.sh`, which every CI job runs. A host without RDT runs without
it, and the report says so.

The groups also count memory bandwidth and LLC occupancy (MBM), reported per instance.
Memory bandwidth allocation (MBA) is not used: the load is a few GB/s on a host with
hundreds, and MBA throttles by inserting delay, which changes the latency it would
protect.

### 4.5. What still differs, and how it is handled

| Residual effect | Handling |
| --- | --- |
| PF 0 and PF 1, and the two core sets, are not identical | The slot swap: each instance runs once in each slot |
| `mtl-runner-7` isolates cores, `mtl-runner-8` does not | Both slots of a host get the same treatment; the hosts are never compared with each other |
| Shared memory controller, DDIO ways, socket power budget | Common mode for both instances; the A/A run measures what is left |
| Another job on the companion, which also serves other repositories | The round notes it when `Runner.Worker` ran on the companion |
| Host tasks on the slot cores of a host without `isolcpus` (`mtl-runner-8`), and on the SMT siblings of slot cores | Not prevented: the cpuset bounds the instance, not the host. Reported as `sch preemptions (/s)`; an exclusive cpuset partition per slot, under a root-level slice, would prevent it |

An A/A run (`ref_a` = `ref_b`) measures the noise floor of the rig. The nightly run is
one whenever `main` did not move for a day.

## 5. Measurement

### 5.1. Collectors

Each process gets a bpftrace collector by PID, as a transient unit on CPU 1. bpftrace
resolves `usdt::sys:<probe>` through the libraries the process mapped, so it reads the
slot tree of that instance through the mount namespace and needs no path:

- `sys:log_msg` for the whole run: every MTL log line, the stats dump included, with its
  timestamp, whatever the log level of the app is;
- `sys:tasklet_time_measure` and `sys:tasklet_stat` for the traced window only: the first
  makes the scheduler time each tasklet call, the second reports the cumulative time and
  work counters of `mtl_sch_get_stats()` at each stats dump. A build without
  `sys:tasklet_stat`, any release before it, is traced without the work counters.

The bpftrace programs and the instance scripts are written to files on the host and the
units run those files: systemd expands `$` in `ExecStart=` (`$$` became `$`, and
the RDT join failed), so no script text goes through it.

The untraced dumps are the ones whose whole period lies between the two `/proc` reads,
by the host's CLOCK_MONOTONIC (`/proc/timer_list`, the clock of the bpftrace timestamps),
so all four processes are measured over the same span and no clock is shared between the
hosts. The traced dumps are the ones that hold tasklet time lines; the first of them
started untraced, so it is only the base of the cumulative counters. The last dump of a
run is dropped, as the application was stopping. The collector units are checked with
the applications, and a build with `sys:tasklet_stat` whose round has no work rows fails
the round.

### 5.2. Metrics

Untraced window, the production view; these are the primary regression signals:

| Metric | Source | Meaning |
| --- | --- | --- |
| `sch_N avg loop (ns)` | `SCH(N:...): ... avg loop X ns` | Time of one pass over all tasklets of scheduler N. More work per packet makes the loop longer at the same load. Always on, no tracing. |
| `video cpu busy (%)` | `RX/TX_VIDEO_SESSION(...): ... cpu busy X` | MTL's own load score of a session, the avg loop over the packet interval |
| `scheduler threads`, `other threads`, `process (cores)` | `/proc/<pid>/task/*/stat` | CPU in cores; a scheduler thread is the `t_pid` of its `SCH` line, the others are the app, pipeline and control threads |
| `sch preemptions (/s)` | `nonvoluntary_ctxt_switches` of the scheduler threads | Isolation quality |
| `memory bandwidth (MB/s)`, `LLC occupancy (MB)` | `mon_data` of the RDT group | Memory traffic and cache footprint of the instance |
| `fps (worst session)`, `RX/TX rate (Mb/s)` | `RX/TX_VIDEO_SESSION(...): fps`, `DEV(N): Avr rate` | The workload; a run below 99% of the frame rate is not compared |
| `incomplete frames`, `hw dropped pkts` | `incomplete frames`, `Status: rx_hw_dropped_packets` | Loss in the receiver |

Traced window, the cost of each tasklet:

| Metric | Source | Meaning |
| --- | --- | --- |
| `<tasklet> avg (ns/call)` | `SCH(N,M): tasklet <name>, avg` | Average of all calls; every build prints it |
| `<tasklet> work (ns/call)` | `sys:tasklet_stat` | Average time of the calls that did work |
| `<tasklet> work (% of a core)` | `sys:tasklet_stat` | Work time over the window, summed over the schedulers |
| `RX work (ns/pkt)`, `TX work (ns/pkt)` | `sys:tasklet_stat` and `DEV` packets | Work time of `rvs_pkt_rx`, or of `tx_video_sessions_mgr` and `video_transmitter`, per packet |
| `sch_N avg loop, traced (ns)` | `SCH(N): time avg` | The loop time with the tracing cost; compare it only with the same row |

`work` counts the calls that returned something other than `MTL_TASKLET_ALL_DONE`. A
polling tasklet is called millions of times a second and nearly all calls are idle, so the
average of all calls hardly moves with the load; the work counters do.

Compare traced rows only between builds that trace alike. A build with the cumulative
counters of `mtl_sch_get_stats()` makes two more counter updates per tasklet call while
traced: on the test host its traced loop and `video_transmitter` rows read 3-4% above a
build without them, while every untraced row agreed.

### 5.3. Statistics

For each metric and instance the round value is the median of the dumps of its window, or
the window difference for `/proc`, RDT and the cumulative counters. The instance value is
the mean of its rounds, the difference is `(B - A) / A`, and the noise is the larger
spread of one instance between its rounds, which includes the slot effect. A difference
within the noise is reported as `~`. With `fail_pct` set, the case fails when an
`avg loop (ns)`, `process (cores)` or `work (ns/pkt)` row of B is worse than A by more than
`fail_pct` beyond the noise; unset, it only reports.

A round is valid when every instance reached steady state, no application or collector
exited early, the median frame rate of every session in the untraced window was at least
99% of the nominal rate (59.94 for p59) and the collectors saw at least two untraced and
two traced dumps. An invalid round fails the case with the reason; for a candidate that
cannot carry the load that failure is the result. Incomplete frames and dropped packets
do not invalidate a round; they are counted, and the round notes them.

## 6. CI

### 6.1. Inputs of `perf-compare.yml`

| Input | Default | Meaning |
| --- | --- | --- |
| `ref_a` | `main` | Reference build: branch, tag or SHA |
| `ref_b` | `main` | Candidate build; equal to `ref_a` it makes an A/A run |
| `args_a`, `args_b` | empty | Extra RxTxApp arguments of each instance |
| `sessions` | `16` | Sessions per instance; 16 × 1080p59 is 42 Gb/s per PF |
| `video` | `1080p59` | `1080p59`, `1080p50`, `2160p59`, `2160p50` |
| `test_time` | `120` | Untraced window in seconds; the traced one is half |
| `rounds` | `2` | Rounds; an even number keeps the slot swap balanced |
| `fail_pct` | empty | Fail on a regression beyond this percentage |

### 6.2. Jobs

1. `refs` resolves both refs to SHAs on a GitHub-hosted runner
   (`task ci:workflow -- perf-compare-refs`).
2. `build-a` and `build-b` call `build.yml` with `branch: <sha>`. `build-b` waits for
   `build-a`: a called workflow takes the caller's name in `github.workflow`, so two calls
   in one run share the `build.yml` concurrency group, and the second would cancel the
   first. `build-b` is skipped when both SHAs are equal.
3. `compare` runs on `[self-hosted, perf, sut]`:
   1. check out the commit `build-a` built, overlay the tests from the workflow commit, run
      `validate-host` with the `build-a` outputs, which proves that the restored tree is
      the one that build hashed, and move `.local_install` to `<workspace>/../perf-compare/a`;
   2. the same for B into `.../b`, with `validate-host` also syncing the companion; `.local_install` keeps B as hard links, for the framework's own use
      (`nicctl.sh`, MtlManager);
   3. copy both trees to the same path on the companion, check bpftrace on both hosts and
      export the two paths as `PERF_COMPARE_SLOTS`;
   4. `task ci:pytest-run -- perf-compare`;
   5. append the report to the job summary and upload it with the logs;
   6. stop every unit, undo the RDT groups and remove the trees on both hosts.

The trees live next to the workspace, not in `RUNNER_TEMP`: the companion is a runner as
well, and a runner empties its `_temp` whenever it starts a job. Both `validate-host`
runs align ICE, so the host ends with the driver of ref B; a kernel module cannot differ
between two processes.

### 6.3. Nightly

The schedule runs with `ref_a` = the last commit of `main` before 24 hours ago and `ref_b`
= `main`. GitHub starts a scheduled run hours late, so "24 hours ago" is counted from the
real start time and the window still covers one day. When `main` did not move, the night
is an A/A run.

### 6.4. Host requirements and the first run

bpftrace on both perf hosts is host image content (`setup_environment.sh` lists it; jobs
install nothing). The job fails at the copy step on a host without it.

A ref goes through `build.yml` and the compare job only when its tree has the
`ci:workflow` task that overlays the tests (`main` since 2026-09-01; `build.yml`,
`perf-pytest.yml` and `custom-pytest.yml` have the same limit). The `v26.01` and `v26.09`
tags do not have it, so they are compared by hand (§8); the measurement itself works for
them, as `sys:log_msg` does.

GitHub dispatches a workflow only when its file is on the default branch, so the first
`perf-compare.yml` run is possible after the workflow lands on `main`. Until then the case
runs by hand (§8).

## 7. Other frameworks

The instance and measurement code runs whatever application command it is given;
nothing in it is RxTxApp specific. A framework needs three things to be compared with it:

1. A multi-session TX and RX command. FFmpeg can send N streams from one input
   (`mtl_engine/ffmpeg.py` does it for its RGB24 multi mode) and take N inputs; GStreamer
   can run N `mtl_st20p_rx` branches in one pipeline.
2. Lcores. The FFmpeg and GStreamer plugins have no lcore option, and none is needed: the
   cpuset places EAL's lcores (§4.2).
3. Frame processing. Both frameworks convert and copy each frame, which dominates
   `process` CPU ("encoder noise"); the scheduler and tasklet metrics stay comparable.

RxTxApp is the first framework because it runs N sessions per process in every release.
FFmpeg and GStreamer are a later step: they need builders for the multi-session commands,
which `mtl_engine` does not have yet.

## 8. Running it by hand

On a dual-host acceptance setup (`gen_config.py` with both hosts and two PCI devices):

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml \
  tests/dual/performance/test_perf_compare.py -k "1080p and 59fps" \
  --perf_compare_slots=/srv/mtl/a,/srv/mtl/b --perf_compare_labels=main,my-branch
```

A slot is a `.local_install` tree built at `<mtl>/.local_install` and copied to the same
path on both hosts: build ref A there and move it to its slot, then the same for ref B.
`--perf_compare_slots=<mtl>/.local_install,<mtl>/.local_install` is an A/A run of the
current build. The report is `logs/performance/latest/perf_compare.md`, the raw values
`perf_compare.json` next to it, and the logs and collector records of each round
`perf_compare/round<N>/`.

## 9. Status

| Step | State |
| --- | --- |
| Scheduler stats API and the `perf_testing` tool | Done; 14 new unit tests pass, `perf_testing` builds against `main` |
| USDT probe `sys:tasklet_stat` | Done; six arguments, because bpftrace 0.14 reads only `arg0`-`arg5` |
| Instance placement, RDT, collectors, parser, statistics, report (`mtl_engine/perf_compare.py`) | Done |
| pytest case, options, CI scripts, `perf-compare.yml` | Done; the workflow runs once it is on `main` (§6.4) |
| Proof on one host | Done |
| Proof on `mtl-runner-7` / `mtl-runner-8` | Done |
| FFmpeg and GStreamer | Not started (§7) |

The proof on one host (E810, kernel 5.15, bpftrace 0.14) ran `pc.run_round()` with both
instances' TX and RX on one host: A was `main` without `sys:tasklet_stat`, B this change,
4 × 1080p50 each, TSC pacing, 2 rounds. Both rounds were valid, every untraced row of A
and B agreed within the noise, as two builds with no data path change should, and B
alone had the work rows (`RX work` 316 ns per packet, `TX work` 661 ns per packet).

The proof on the perf pair ran the pytest case itself, from a private checkout with the
CI virtualenv and the CI configuration of `perf-pytest.yml`: A was the tree the last CI
job had left in the runner workspace (a pull request build, without
`sys:tasklet_stat`), B the same tree with this change's MTL built on `mtl-runner-7`;
16 × 1080p59 per instance, E830 rate limiter pacing, 2 rounds, `test_time` 60. The case
passed in 6 min 22 s with both rounds valid. RX ran on isolated cores 4-11 and 12-19,
TX on 2-9 and 10-17; CAT gave each slot 5 of the 16 L3 ways; the schedulers saw 0.3
(RX) and 2 (TX) preemptions a second. The spread between the rounds was 0-2% for most
rows. TX agreed within it. On RX, B's second scheduler looped 15.7% longer, `rvs_pkt_rx`
took 9% more per call and the instance held 15% less LLC, each far beyond the spread. A
and B differ in source and in build host, so that is a real difference between those two
trees, found at that resolution. It is not a verdict on any one commit. The runners were
left as found: no units, no RDT groups and the full L3 mask.
