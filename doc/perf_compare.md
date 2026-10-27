# Performance comparison of two MTL builds (perf-compare)

perf-compare runs the same end-to-end workload for two MTL builds or configurations at
the same time and reports how they differ. Each runs as an instance, a TX application on
one host and an RX one on the other, with a PF, CPU cores, L3 ways and a build tree of
its own. It answers "is B slower or faster than A, and where": `main` today against
yesterday, a branch against `main`, `v26.09` against `v26.01`, or two values of
`--sch_session_quota`. It does not search for a session ceiling, as `perf-pytest.yml`
does.

A is the baseline, B the candidate under test: every difference is B's, `(B - A) / A`.
An A/A run, of one build twice, measures the noise floor of the rig.

## 1. Decisions

| Question | Decision | Why |
| --- | --- | --- |
| Run A and B one after the other, or at the same time? | At the same time, then again with the slots swapped | Concurrent runs share every time-varying disturbance (the shared switch's multicast floods, other jobs, thermal state) as common mode, which a sequential run turns into a difference. The swap cancels what differs between the slots (§3.4). |
| Extend `perf-pytest.yml`? | No: a workflow of its own, `perf-compare.yml` | A dispatch form shows every input of its workflow and cannot hide one by mode. The steps both run are already one script each (`ci:pytest-setup`, `ci:pytest-run`, `ci:report`). |
| A pytest case or a standalone tool? | A case in `tests/acceptance/tests/dual/performance/` | Both builds run under the workflow commit's harness, with its topology, VFs, media ramdisk, RxTxApp config builder and log layout. |
| Where are A and B built? | In the compare job, on the SUT, with the workflow commit's build scripts | One host, compiler and set of flags for both, so a difference is the code's. `build.yml` cannot build `v26.01`: its scripts install into `/usr/local`. |
| How do two builds that expect the same path run side by side? | A private mount namespace binds each tree over `<mtl>/.local_install` (§3.2) | DPDK bakes `RTE_EAL_PMD_PATH` into `librte_eal` as an absolute path with no override, so a moved tree loads the wrong drivers or none; `v26.01` pins DPDK 25.11 and `main` 26.07. |
| How are the CPUs isolated? | A transient systemd unit per instance whose cgroup v2 cpuset is the slot and CPU 0, the main lcore MTL gives EAL; the slot's CPUs but its first are its `--lcores` (§3.1) | `taskset` isolates nothing: `numa_bind()` in `mtl_init()` widened a `[2,3]` mask to `0-31`. `isolate.sh` holds one host-wide lock for its one partition. |
| How is the L3 shared? | Intel RDT CAT: equal, exclusive ways per slot (§3.3) | Otherwise the larger footprint evicts the other, and the swap does not cancel that: it follows the instance, not the slot. |
| One NUMA node for both slots, or one each? | Both on the node of their NIC; with both NICs, each NIC's slots on its own node (§3.5) | A slot on the other node would reach its NIC and hugepages across the socket link, which no production MTL does (DPDK's control data aside, §3.1); the swap cancels that in the mean but not in the spread. |
| How do the builds use a second NIC? | As the `nics` input says: an instance on a port of each NIC (`span`), or a pair of slots on each NIC whose rows stay apart (`pairs`) or are pooled (`pooled`) (§3.5) | `span` loads one MTL instance with two NICs; `pairs` and `pooled` repeat the comparison on the other socket, a block of its own, never A on one socket and B on the other. |
| Does an instance copy with DMA? | Per build (`dma`): its RX instances, each with a DSA of its own on its port's node (§3.5) | MTL has no TX DMA, gives a session only a DMA device on its port's node, and a vfio device belongs to one process. |
| What is measured? | The stats dump through `sys:log_msg`, the tasklet counters through `sys:tasklet_stat`, `/proc`, RDT and `perf stat` (§4) | `sys:log_msg` fires at any log level and exists in every release since 2024, so `v26.01` is measured the same way as `main`. |
| Which application? | RxTxApp | FFmpeg and GStreamer measure their own frame copies more than MTL. |
| Does the report say whether the candidate regressed? | Per row and in a verdict (§4.4); the case never fails on a score, only on an invalid round or a setup it cannot run | The answer first, from the candidate's side, as benchmark tools give it. |

## 2. Topology and one round

### 2.1. Topology

The SUT (`mtl-runner-7`, label `sut`) runs both RX applications and the companion
(`mtl-runner-8`) both TX ones, and both hosts are measured. Each RX receives its own
instance's TX, so a change on one side can show on the other; `args_b` reaches both
applications of B.

```text
 companion mtl-runner-8: TX                              SUT mtl-runner-7: RX
┌───────────────────────────────────────┐              ┌───────────────────────────────────────┐
│ CPUs 0-1: OS, CI runner, collectors   │              │ CPUs 0-1: OS, CI runner, collectors   │
│                                       │              │                                       │
│ slot 0  unit mtl-perf-tx0             │  PF 15:00.0  │ slot 0  unit mtl-perf-rx0             │
│         CPUs 4-11, node 0, L3 group 0 │ ──── VF ───▶ │         CPUs 4-11, node 0, L3 group 0 │
│         tree a (round 1), b (round 2) │  unicast,    │         tree a (round 1), b (round 2) │
│                                       │  subnet 0    │                                       │
│ slot 1  unit mtl-perf-tx1             │  PF 15:00.1  │ slot 1  unit mtl-perf-rx1             │
│         CPUs 12-19, node 0, L3 group 1│ ──── VF ───▶ │         CPUs 12-19, node 0, L3 group 1│
│         tree b (round 1), a (round 2) │  subnet 1    │         tree b (round 1), a (round 2) │
│                                       │              │                                       │
│ L3 default group: DDIO ways, the rest │              │ L3 default group: DDIO ways, the rest │
└───────────────────────────────────────┘              └───────────────────────────────────────┘
```

Slot 0 uses a VF of PF 0 (`host.vfs[0]`) on both hosts and slot 1 a VF of PF 1
(`host.vfs_r[0]`) on the subnet of the redundant `ip_pools`, so no stream of one slot
reaches the other; the E830's PCIe x16 Gen5 link is far above two 100G ports. Both
instances get the same RxTxApp JSON but for the interface and the IPs, the same number
of CPUs, and perf-pytest's `--dedicated_sys_lcore --disable_migrate` (migration would
change the cores in use mid-run). `args_a` and `args_b` are the only intended difference
besides the build.

### 2.2. One round

```text
      start  steady  warm-up     untraced window     traced window      timed window    sessions window    stop
       │        │    ├───────┤├───────────────────┤├────────────────┤├────────────────┤├────────────────┤  │
RX A,B ┼═══════════════════════════════════════════════════════════════════════════════════════════════════┼
TX A,B   ┼═══════════════════════════════════════════════════════════════════════════════════════════════┼
log_msg  ├─────────────────────────────────────────────────────────────────────────────────────────────────┤ every stats dump
/proc, RDT, clock             ▲ read 1            ▲ read 2
perf stat                    ├─────────────────────┤
tasklet_time_sample + tasklet_stat                 ├────────────────┤
tasklet_time_measure                                                 ├────────────────┤
sessions_time_measure                                                                  ├────────────────┤
           ▲ move the app threads off the scheduler CPUs
```

| Phase | What runs besides the `sys:log_msg` collectors, and why | Its dumps, and what they give |
| --- | --- | --- |
| Start | Both RX applications, then both TX ones (a unicast TX session waits for the ARP reply of its RX), each a transient systemd unit (§3) | |
| Steady, warm-up | Once a dump shows an instance's sessions and started schedulers, its app threads leave the scheduler CPUs (§3.1). Every session of every instance must then reach 99% of its frame rate within `120 + 3 × sessions` s; then 30 s of warm-up | |
| Untraced, `test_time` | `perf stat`, with `/proc`, RDT and CLOCK_MONOTONIC read at both ends. Nothing is timed: the production view | Those whose whole period lies between the two `/proc` reads: the primary rows |
| Traced, `test_time / 2` | `sys:tasklet_time_sample` and `sys:tasklet_stat`, where the build has them: each scheduler times 1 loop in 64, a cost the primary rows must not carry | Those with `sys:tasklet_stat` records; the first, partly untraced, is only the base of the counters. The time and work of each tasklet |
| Timed, `test_time / 2` | `sys:tasklet_time_measure`, which every release has: every call is timed, which costs 10-45% of the loop and would add to the traced counters | Those with call times but the first, which started untimed: the time of every tasklet call |
| Sessions, `test_time / 2` | `sys:sessions_time_measure`, which every release has: every video session is timed inside its tasklet's call, and its clock reads would add to the call times | Those with session times but the first: the time of every video session |
| Stop | The last probe is detached a dump period early, so that each application prints the dump that closes it; SIGINT stops TX, then RX, and the logs go to the pytest log folder | The last dump of the run is dropped |

MTL prints a stats dump every 10 s, its dump period. Each window after the untraced one
gets a dump period more at each end. The dumps are placed by each host's own
CLOCK_MONOTONIC (`/proc/timer_list`, the clock of bpftrace), so the four processes share
one span without a clock common to the hosts.

Each round swaps the slots, 4 rounds by default. `wire_balance()`, shared with
perf-pytest, reads the unicast MAC counters of each PF around each round, so a loss
between the hosts is noted apart from one in the receiver; see why [the perf pair needs
a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own).

## 3. Isolation

One instance on one host, from the outside in:

```text
systemd-run --unit mtl-perf-rx0          AllowedCPUs=0,4-11 AllowedMemoryNodes=0 (cgroup v2), Nice=-20
└─ unshare --mount --ipc                 its own mount table and SysV IPC
   └─ sh /tmp/mtl-perf-compare/A_rx0.sh
      ├─ mount --bind <slot tree> <mtl>/.local_install     its build, at the path DPDK expects (§3.2)
      ├─ mount -t tmpfs tmpfs /var/run/imtl                no MtlManager; a private lcore table
      ├─ echo $$ > /sys/fs/resctrl/mtl_perf_0/tasks        its L3 ways (§3.3)
      └─ cd <mtl> && exec env LD_LIBRARY_PATH=<its libs> RxTxApp … --lcores 5-11 …
                                       schedulers on 5-11, application and EAL control threads on 4 (§3.1)
mtl-perf-rx0-{log,trace,timed,sessions}  bpftrace -p <pid>, the probes of each window, on CPU 1
mtl-perf-rx0-pmu                         perf stat -p <pid>, the untraced window, on CPU 1
```

### 3.1. CPU

`cpu_layout()` gives each slot 8 physical cores of the VF's NUMA node, one hardware
thread each so no slot holds an SMT sibling of the other's, after two cores for the OS
and the collectors. Both slots are isolated cores when the node isolates 16 or more,
else neither is. `isolcpus=4-20,68-84` (and the same `nohz_full` and `rcu_nocbs`)
isolates 17 cores on the node of each NIC of the perf hosts: the slots are then 4-11
and 12-19 on NIC 0's node 0 and 68-75 and 76-83 on NIC 1's node 2. With node 0's
alone, NIC 1's slots are cores that are not isolated.

The slot's CPUs but its first are the instance's `--lcores`, as perf-pytest gives MTL
the isolated CPUs of its NIC's node, and the cpuset keeps the instance in the slot and
CPU 0:

- MTL passes `-l 0,<lcores>` to EAL and RxTxApp has no option for the main lcore, so
  CPU 0 is the main lcore of every instance and the cpuset has it too, else EAL fails
  ("Cannot set affinity"). `rte_eal_init()` runs there, on a thread of its own. The
  process starts where the kernel puts it in the cpuset, often CPU 0, and so do the
  threads it starts until they are moved (below), before the warm-up.
- EAL pins its control threads (interrupts, the iavf events, telemetry) to the CPUs of
  the cpuset that are no lcore: the slot's first, where the application threads also
  always have a CPU no scheduler polls.
- The cpuset also has the memory of CPU 0's node, for NIC 1's slots node 0 beside node
  2. DPDK allocates on the main lcore's node for a thread that is no lcore and names no
  node, such as iavf's rate limiter and flow rules from MTL's main thread, and its
  dmadev data only there: with node 2 alone each such allocation faults a 1 GB page of
  node 2 and drops it, and a DSA fails to probe ("Cannot initialize dmadevs data"). So
  DPDK's control data of NIC 1's instances is on node 0; MTL puts their queues, buffers
  and frames on their port's node 2.
- Once the instance's sessions and schedulers run, each thread allowed more than one
  CPU, the main thread and those it started, gets one of the slot's CPUs that no
  scheduler uses, in turn; EAL pins each of its own threads to one. CFS would otherwise
  move one onto a polled CPU, and the kernel never balances isolated CPUs: 36 TX
  sessions of 1080p59 left on the one CPU they started on sent 40 to 43 fps, spread over
  the free CPUs 59.94.

### 3.2. Software state

`unshare --mount --ipc` gives each instance its own build, through the bind mount and an
`LD_LIBRARY_PATH` for a DPDK whose sonames the linker cache lacks. A tmpfs over
`/var/run/imtl` hides MtlManager's socket, so a `v26.01` library never talks to a `main`
daemon: MTL falls back to a SysV lcore table private to the IPC namespace ("clear shm as
we are the first user"). DPDK's `--in-memory` shares no hugepage file.

### 3.3. Cache and memory bandwidth

When resctrl offers L3 allocation and holds no other groups, the groups `mtl_perf_0` and
`mtl_perf_1` get `ways / 3` L3 ways each (with a pair on each NIC, `mtl_perf_2` and
`mtl_perf_3` the same ways in the other socket's L3), at the end of the mask away from the
`shareable_bits`. Those are the ways the NIC writes into (DDIO), which both instances
use and neither may own, so the default group keeps them with the rest: with 12 ways and
`shareable_bits` `c00`, the slots get `00f` and `0f0` and the default group `f00`. Each
instance joins its group before `exec`, and the group also gives its memory bandwidth
(MBM) and LLC occupancy (CMT). MBA is not used: the load is a few GB/s on a host with
hundreds, and MBA throttles by inserting delay, which changes the latency it would
protect.

On these SNC2 hosts kernel 6.8's resctrl monitoring knows no sub-NUMA clusters (that
came with 6.11), so check the memory bandwidth rows in an A/A run before trusting them.

A host without RDT runs without CAT, as the report's placement table says. The case
restores the default mask, saved in `/run/mtl-perf-compare.schemata`, at its end, or
after an aborted job `perf-compare.sh clean` (§6).

### 3.4. What still differs

| Residual effect | Handling |
| --- | --- |
| PF 0 and PF 1, and the two core sets, are not identical | The slot swap: each instance runs once in each slot |
| The two hosts are not identical | Both slots of a host are treated alike; the hosts are never compared |
| Shared memory controller, DDIO ways, socket power budget | Common mode; an A/A run measures what is left. The nightly is one when `main` did not move, and equal refs run the same binaries |
| Another job on the companion, which serves other repositories too | The round notes a `Runner.Worker` on the companion at its start or end |
| Host tasks and IRQs on the SMT siblings of slot cores, IRQs on the slot cores | Not prevented: the cpuset bounds the instance, not the host. A task on a slot core shows as `sch preemptions (/s)`, the rest as a longer loop and a wider spread |
| NIC 0 and NIC 1, and their sockets | `span`: both slots have a port of each. `pairs`: each NIC's rows apart. `pooled`: refused unless both NICs link at one speed, run one firmware and are alike isolated |

### 3.5. Two NICs and DMA

`nics` sets how the builds use the NICs of the perf pair, each of two ports, NIC 0 on
node 0 and NIC 1 on node 2. The topology then declares all four ports
(`PERF_PORTS=4`); NIC 1's ports take a VF each and the subnets 100 above NIC 0's.

| `nics` | Slots of a host | Results |
| --- | --- | --- |
| `single` | 0 and 1: port 0 and 1 of NIC 0 | As above |
| `span` | 0 and 1: port 0 or 1 of each NIC, the cores and memory of both nodes, `sessions` on each port | One instance a build drives both NICs; `mtl_init()` binds its application threads to port 0's node, and the case spreads them over the free CPUs of the slot |
| `pairs` | 0 and 1 on NIC 0, 2 and 3 on NIC 1, each on its NIC's node; NIC 1's pair runs the other way round, so every round has each build on both NICs | A section and rows of their own per NIC, all key rows in the one verdict |
| `pooled` | As `pairs` | One table per role, each NIC of each round a sample, so one round already gives a noise estimate: more samples, but the noise holds the NIC difference too |

RxTxApp allocates a TX source on the node of its instance's first port, so in `span`
NIC 1's TX sessions read theirs across the socket link, and node 0 holds the sources
of both TX instances: about 18 GB at 36 x 1080p59 a port, of its 24 1 GB hugepages. A
TX that runs out reads its source through `mmap()`, which fails the round (§6).

`dma` gives the RX instances of A, B or both `--dma_dev`: the slot's DSA on each of its
nodes, the i-th slot of a node its i-th DSA, bound to vfio-pci, so the DSA follows the
slot when the builds swap. vfio-pci must already allow DSA (`disable_denylist`, set by
the session's `host_dma_devices`): lifting it in the case would reload the module and
unbind the VFs. MTL falls back to the CPU copy for a session it gives no DMA, so a
round in which a session of a DMA instance copied nothing by DMA is invalid. DMA also
changes MTL's scheduler quota (12 rather than 8 1080p sessions a scheduler), so its
scheduler rows move with it.

## 4. Measurement

### 4.1. Collectors

The collectors attach by PID, on CPU 1. bpftrace resolves `usdt::sys:<probe>` through
the libraries the process mapped, so it reads the tree of the instance;
[usdt.md](usdt.md) describes the probes. A build without `sys:tasklet_time_sample`, as
no release up to `v26.09`, is not traced, and its traced rows read n/a.

```text
RxTxApp + libmtl (slot tree)        bpftrace -p <pid>, CPU 1           perf_compare.py, the case
 stats dump, every 10 s ──── sys:log_msg ───▶ "<ns> L <line>"   ─┐
 the counters at a dump ── sys:tasklet_stat ─▶ "<ns> T <sch> <tasklet> cnt sum wcnt wsum"
 /proc/<pid>/task/*, RDT mon_data, /proc/timer_list ─── read at both ends of the untraced window
 the PMU counters of its threads ─── perf stat -x, ─── counted over the untraced window
                                                                 │
          parse_dumps():  records into dumps, each with its CLOCK_MONOTONIC time
          windows():      the dumps of each window (§2.2)
          metrics():      median over dumps; differences of /proc, RDT, perf and the counters
          compare():      mean over rounds, (B - A) / A, noise = 2 standard errors
          report_html():  perf_compare.html, next to the round logs
          summary_html(): perf_compare_summary.html, for the job summary
```

`perf stat -x, -p <pid>` counts each process over the untraced window. Counting samples
nothing: the kernel switches the counters with a thread, which a pinned scheduler thread
hardly does. Its rates divide by the `/proc` span, as `duration_time` sums the threads
of a `-p` process. The perf is the first of `/usr/bin/perf` and
`/usr/lib/linux-tools/*/perf`, newest first, that counts every event: Ubuntu's
`/usr/bin/perf` refuses to run without the tools of the running kernel.

### 4.2. Metrics

[perf_compare_metrics.md](perf_compare_metrics.md) takes each row apart: its source line in
MTL, how MTL and the case compute it, and why the numbers hold.

| Metric | Source | Meaning |
| --- | --- | --- |
| **Untraced window** | | **The production view, the primary signals** |
| `sch_N avg loop (ns)` | `SCH(N:...): ... avg loop X ns` | One pass over the tasklets of scheduler N; more work per packet lengthens it at the same load |
| `video cpu busy (%)` | `RX/TX_VIDEO_SESSION(...): ... cpu busy X` | MTL's load score of a session: its scheduler's avg loop over 3 packet intervals (RX) or a TX bulk of them, refreshed every 6 s ([perf_compare_metrics.md](perf_compare_metrics.md#43-cpu)) |
| `scheduler threads (cores)`, `other threads (cores)` | `/proc/<pid>/task/*/stat` | CPU in cores; a scheduler thread is the `t_pid` of its `SCH` line |
| `sch preemptions (/s)` | `nonvoluntary_ctxt_switches` of the scheduler threads | Isolation quality; what the swap leaves is a cost of the build |
| `memory bandwidth (MB/s)`, `LLC occupancy (MB)` | `mon_data` of the RDT group | Memory traffic over the window, and the cache footprint at its end |
| `IPC`, `core clock (GHz)` | `perf stat`: `instructions`, `cycles`, `task-clock` | Instructions per cycle, and the clock while the threads run; a clock that differs moves every time row |
| `LLC hits (M/s)`, `LLC misses (M/s)`, `LLC miss ratio (%)` | `perf stat`: `cache-references`, `cache-misses` | L3 requests of the instance's cores that hit or went to memory; a packet read while still in the DDIO ways is a hit |
| `fps (worst session)`, `RX/TX rate (Mb/s)` | `RX/TX_VIDEO_SESSION(...): fps`, `DEV(N): Avr rate` | The workload: the lowest median frame rate of a session, and the port rate |
| `incomplete frames`, `hw dropped pkts` | `incomplete frames`, `Status: rx_hw_dropped_packets` | Loss in the receiver |
| `build timeout frames`, `epoch drop`, `transmitter mismatch troffset` | `TX_VIDEO_SESSION(...)` lines, printed when they moved | TX frames sent late, summed over the window: built after their time, periods without their frame, a first packet the rate limiter sent late. A session can hold its rate and still count them (§5.1) |
| `transmitter recalculate warmup (/s)` | the same lines | Rate limiter re-plans of the pads before a frame as the clock outran the plan: a pad burst stalled 2 µs or more, or the shaper went idle. It can differ by slot, which the swap cancels. Up to `v26.09` MTL counts only the warm-up re-plan |
| **Traced window** | | **The cost of each tasklet** |
| `<tasklet> avg (ns/call)` | `sys:tasklet_stat` | Average of all calls, the idle ones included |
| `rvs_pkt_rx work (ns/call)`, `rvs_pkt_rx work (% of a core)` | `sys:tasklet_stat` | The calls that received packets: their average, and their time summed over the schedulers |
| `RX work (ns/pkt)` | `sys:tasklet_stat`, `DEV` packets | Work time of `rvs_pkt_rx` per received packet |
| `sch_N avg loop, traced (ns)` | `SCH(N:...): avg loop` | Against the untraced loop, the cost of the sampling |
| **Timed, sessions windows** | | **Call times in us, as MTL prints them** |
| `sch_N <tasklet> avg, min, max, p90, p95, p99, timed (us)` | `SCH(N,i): tasklet <name>, avg ... p99 ...` | The tasklet's calls on scheduler N: the median over dumps of avg and percentiles, the least min, the largest max. No release up to `v26.09` prints percentiles: n/a |
| `sch_N RX/TX video session avg, min, max, session timed (us)` | `RX/TX_VIDEO_SESSION(N,s): tasklet time avg ...` | The part of a tasklet call that serves one session (its RX packets, or building its TX packets), over the sessions of scheduler N |

Work is a call that did not return `MTL_TASKLET_ALL_DONE` (`tasklet_stat` in
[usdt.md](usdt.md)). A polling tasklet is nearly always idle, so its average call hardly
moves with the load while its work does. Only `rvs_pkt_rx` tells work by its return
value: pending when it received packets. A paced `tx_video_sessions_mgr` builds in calls
that return all done (its ring is full, the bulk waits in its inflight) and
`video_transmitter` returns pending while a packet waits for the pacer, so TX has no
work rows and compares by its loop, CPU, cache and late frame rows. Per-scheduler rows
pair up only while both builds place the tasklets alike.

### 4.3. Tracing cost

A timed call reads the TSC twice, and timing every loop made it 11-30% longer on the
perf pair (38-45% on the host it was first measured on). The traced window samples instead: each scheduler times 1 loop in 64 at
random gaps and counts a timed loop as many times as its gap (`tasklet_stat` in
[usdt.md](usdt.md) says what stays exact). That costs 0-1.4% of the loop, and nothing
while the probe is detached. The timed and sessions windows come after the measured ones
and change none of their rows, but their calls carry the clock reads and find more work
waiting: they compare the builds, not production, and their tails (p95, p99, max) carry
less of that than the min and the avg.

### 4.4. Statistics and scoring

| Step | Rule |
| --- | --- |
| Round value | The median of the window's dumps; the sum of its dumps for the loss and late frame rows, which MTL prints per dump; the change over the window for `/proc`, the RDT memory traffic, `perf stat` and the cumulative counters; LLC occupancy is the reading at the end |
| Instance value, difference | The mean of its rounds; `(B - A) / A`. A and B run at once, so each round's difference, in the Rounds column, is a paired one |
| Noise | Twice the standard error of a build's mean over its rounds, the larger build's, so it includes the slot effect. For two rounds it is their difference; more rounds of the same spread narrow it, where a range would widen. A time MTL prints has at least one print step of noise, of the smaller value: 1 ns for a loop, 10 ns for a call or a session |
| Change | From 2% and twice the noise, a size and a margin over the noise as LNT and Perfherder also ask for; the factor keeps the rough noise estimate of a few rounds from making a change. No change reads `~`, and one valid round scores no row (of `pooled`, one sample) |
| Confidence | High from 5 times the noise, medium from 3, else low |
| Outcome | A change of a row with a better direction is a regression (❌) or an improvement (✅) of the candidate, in a light red or green tint; a change of any other row only differs (➖), in grey. A color-blind reader may not tell red from green, so the mark, the word and the bold better value carry the outcome as well |
| Better higher | `fps (worst session)` |
| Better lower | Every other scored row, a cost: loop and call times (a call is also a wait for the tasklets after it), CPU, preemptions, memory traffic, L3 misses, loss and late frames |
| No better direction | `core clock`; `IPC`, which idle polls raise as much as speed does; LLC occupancy, hits and miss ratio; the port rates, which carry pads and other traffic; the re-plans, which releases count differently |
| From 0 | A cost that A counts as 0 scores as the mirror of B at 0, 100%, and reads `from 0` |

The summary opens with a verdict on the key rows both builds have, each scheduler's
loop, `video cpu busy`, `scheduler threads`, `memory bandwidth`, `LLC misses` and
`RX work (ns/pkt)`, plus the loss and late frame rows once a build counted any. It
counts regressions and improvements, names the largest of each (a row from 0 has no
size), reports loss and late frames, and states the rule and the rounds, with a warning
under 4. The key rows are not independent (`video cpu busy` follows its loop), so their
counts are not votes, and the rough noise of a few rounds lets an A/A run change a row
now and then. The avg and p99 of each tasklet's calls follow, outside the verdict: the
timed window runs slower than production, and a tasklet on several schedulers would
count several times.

The case passes or fails on the validity of its rounds (§6), never on a score: for a
candidate that cannot carry the load, the failure is the result. A round that raises
ends the rounds; the report keeps the earlier ones and names the error.

`perf_compare.html` gives the builds, the rounds, the summary, and each role's rows in
groups that show the rows that changed and fold the others. The setup follows, open,
then each instance's command and RxTxApp configuration, folded. The job summary keeps no
style, so it gets `perf_compare_summary.html`: the same without the setup and the
instances, the role tables folded.

### 4.5. Layout effects

A difference can come from where the data sits, not from what the code does. The
schedulers sit in one array in `struct mtl_main_impl`, so a few bytes more in
`struct mtl_sch_impl` or `struct mt_sch_tasklet_impl` moved one RX scheduler 10-15% on
the perf pair with no code change. When a candidate differs from the reference on one
scheduler only, compare the reference with itself padded to the candidate's struct sizes
before blaming the code, over enough rounds to see the slow state that `main` reaches on
its own in about 1 round in 10.

## 5. Running it

### 5.1. Inputs

The inputs of `perf-compare.yml`, and the pytest options of a run by hand (§5.4):

| Input | pytest | Default | Meaning |
| --- | --- | --- | --- |
| `ref_a`, `ref_b` | `--perf_compare_slots`, `--perf_compare_labels` | `main`, `main` | A, the baseline, and B, the candidate: a branch, tag or commit, with an optional `~n` or `^`; the same twice measures the noise. pytest takes the two built trees and their labels |
| `args_a`, `args_b` | `--perf_compare_args_a`, `--perf_compare_args_b` | empty | Extra RxTxApp options of each build: letters, digits, spaces and `_./:=,+-`; not `--tasklet_time`, which would time every window, nor `--dma_dev` or `--lcores`. RxTxApp ignores an option it does not know |
| `dma` | `--perf_compare_dma` | `none` | `none`, `A`, `B` or `both`: the builds whose RX instances copy with a DSA (§3.5) |
| `nics` | `--perf_compare_nics` | `single` | `single`, `span`, `pairs` or `pooled` (§3.5); each choice of the form says what it does, and the case takes the word before its colon |
| `video` | `-k "1080p and 59fps"` | `1080p59` | 1080p, 2160p or 4320p at 59, 50, 29 or 25 fps, from the perf media set |
| `sessions` | `--perf_compare_sessions` | `36` | Video sessions of each build on each port it uses, in every `nics` mode (`span` puts that many on each of its two ports); the case refuses a count that a port's link does not carry (below) |
| `test_time` | `test_time` of `test_config.yaml` | `120` | Seconds of the untraced window; at least 30, for two whole stats dumps. The three later windows add half of it each |
| `rounds` | `--perf_compare_rounds` | `4` | Rounds; A and B swap places each round, so keep it even. Fewer than 4 give a rough noise estimate |

A 59 fps session takes 2.76 Gb/s on the wire at 1080p, 11.0 at 2160p and 44.1 at 4320p
(20 bits a pixel, paced over 1125 of 1080 lines, in MTL's packets of 1320 bytes of
pixels, 1406 on the wire); 50, 29 and 25 fps take 0.83, 0.5 and 0.42 of that. 36 x
1080p59, 99.3 Gb/s, is the most a 100G port carries (at 37 no session reached its
rate); 2160p59 fits 9, 4320p59 2.

At 36 x 1080p59 every session holds its rate, but two `main` TX instances on the
companion count about 210,000 build timeout frames each a round, which the rounds note;
`main` beside `v26.01` counts none. Such late frames follow the load of both TX
instances, so an A/A run of `main` notes them every round.

The refs job rejects a bad ref, a number that is not a positive integer, and a
`test_time` under 30, before the compare job builds anything; the numbers are strings, as a number
input reaches a step as `120.0`. A ref takes no option, `^{/regex}`, comma or newline:
it reaches `git`, `GITHUB_OUTPUT` and the labels. The case rejects bad arguments before
it starts an instance, as they reach a root shell script on both hosts.

### 5.2. Jobs

1. `refs` resolves both refs to SHAs (`task ci:workflow -- perf-compare-refs`).
2. `build` runs `build.yml` for the workflow commit, which the framework itself runs:
   the tests, the CI scripts, the ICE driver, MtlManager and `nicctl.sh`.
3. `compare`, on `[self-hosted, perf, sut]`:
   1. restores that build (`validate-host`);
   2. `task ci:perf-compare -- build` builds both slots, and `-- sync` copies them to
      the companion and exports their paths as `PERF_COMPARE_SLOTS` in `GITHUB_ENV`;
   3. prepares the acceptance virtualenv and the pytest configuration, then runs
      `task ci:pytest-run -- perf-compare`;
   4. `task ci:report -- perf-compare` puts the summary into the job summary; the
      report and the logs are uploaded and the summary links them;
   5. `task ci:perf-compare -- clean` removes the units, the RDT groups and the trees.

A slot, DPDK, libmtl and RxTxApp, is built from `git archive <sha>` with the workflow
commit's `build_dpdk.sh` but the ref's own `versions.env` and DPDK patches, at the
workspace's `.local_install`, the path DPDK bakes in, and kept in
`<workspace>/../perf-compare/{a,b}` on both hosts. Only the workflow commit's DPDK and
MTL step aside meanwhile, for the ffmpeg watchdog of
[ci_runner_setup.md](ci_runner_setup.md). The ICE driver is the workflow commit's: a
kernel module cannot differ per process.

### 5.3. Nightly and refs

The schedule (16:00 UTC) takes as A the `main` that the last passed night compared as
its B, and as B the `main` of this run (`GITHUB_SHA`), so every commit is in one night's
window however late GitHub starts the run, 2 to 7 hours:

```text
main   ──●───●──●────────●───●──────●───────●──▶
night 1  ├──── A₁ … B₁ ──┤
night 2                  ├── A₂ = B₁ … B₂ ──┤
```

A failed night widens the next one. A is `main` of 24 hours ago when no night passed
yet, when the Actions API read fails, and when the last night's B is not an ancestor of
this one, as after an older night is run again. When `main` did not move, the night is
an A/A run.

The workflow ref ("Use workflow from": the tests and the CI scripts) must have this
workflow; `ref_a` and `ref_b` can be any ref with `tests/tools/RxTxApp`, a
`versions.env` and the DPDK patches it names: `v25.12-rc1`, `v26.01`, `v26.09` and
later. Both perf hosts need bpftrace, and optionally a perf: see the prerequisites in
[ci_runner_setup.md](ci_runner_setup.md).

### 5.4. By hand

On a dual-host acceptance setup (`gen_config.py` with both hosts and two PCI devices,
or the card's device four times for a `nics` that uses both cards):

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml \
  tests/dual/performance/test_perf_compare.py -k "1080p and 59fps" \
  --perf_compare_slots=/srv/mtl/a,/srv/mtl/b --perf_compare_labels=main,my-branch
```

A run takes one case, selected with `-k`, as each writes the run's one report; without
`--perf_compare_slots` the cases are deselected. A slot is a tree built at
`<mtl>/.local_install` and copied to the same path on both hosts;
`perf-compare.sh build` with `SHA_A` and `SHA_B` set to full SHAs builds both in
`../perf-compare/{a,b}`. `<mtl>/.local_install` twice is an A/A run of the current
build. The report is `logs/performance/latest/perf_compare.html`, each round's logs and
records in `perf_compare/round<N>/` next to it.

## 6. Troubleshooting

The Rounds table of the report gives each round's problems, which fail the case, and
notes, which do not.

| Problem, or (note) | Cause | What to do |
| --- | --- | --- |
| `not steady after N s`, `a session fell to N fps` | A session stayed under 99% of its rate. The 8 cores of a slot, or the companion's hugepages, which hold the TX source of every session of both builds, can limit the sessions before the link | Fewer `sessions`; for a candidate that cannot carry the load the failure is the result |
| `exited before steady state`, `exited during the measurement` | An application or a collector stopped | The application's output and its `.log` and `.trace` records in `perf_compare/round<N>/`; a timed or sessions collector's output only stays in `/tmp/mtl-perf-compare/` on its host, until the next round |
| `N untraced, N traced, N timed and N session timed dumps`, `N of M sessions` | A window held fewer than two whole dumps, or not every session reported | A `test_time` of 30 or more; the application and collector logs |
| `its source is not in hugepages` | A TX read its source through `mmap()` | More hugepages on the companion, or fewer sessions |
| `no rvs_pkt_rx work in sys:tasklet_stat` | A traced RX instance gave no work counters | Its `.trace` record |
| `failed: <error>` | The round raised, such as when an application died between two `/proc` reads; the rounds stop | The pytest log |
| Incomplete frames, dropped packets, late TX frames (note) | Loss in the receiver, or TX frames sent late | Their rows count them |
| `packets lost between the hosts` (note) | Loss on the wire | [The perf pair needs a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own) |
| `a CI job ran on <host>` (note) | Another job on the companion at the round's start or end | Rerun if the round stands out |
| `the builds pace TX differently` (note) | A build's TX fell back from the rate limiter (`pacing way:`, info level) | Read its re-plan row as that, not as a gain |
| `perf stat did not count every event` (note) | The host's perf left an event of the instance uncounted, so it has no PMU rows | Its `.pmu` record in `perf_compare/round<N>/` |
| Placement `perf` no | No perf on the host counts every event: no PMU rows at all | A `linux-tools-<version>` package of a 6.8 kernel |
| Placement `CAT` no | No L3 allocation, or other resctrl groups | Remove the groups, or accept a shared L3 |
| `N x <video> on a port take N Gb/s, and <PF> links at N Gb/s` or `<PF> has no link` | The sessions of a port do not fit its link, or the port has none: `sessions` counts per port also with two NICs | Fewer `sessions`, or the cable and the link of that port |
| `--perf_compare_nics <mode> takes two ports of each of two NICs`, `both NICs are on NUMA node N` | The topology declares fewer than four ports (`PERF_PORTS`), or they are not two ports of each of two cards on two nodes | The second card of both hosts, cabled port to port |
| `pooled takes alike NICs, but ...` | The NICs differ in speed, firmware or isolated cores | Make them alike, or run `pairs` |
| `vfio-pci still refuses DSA`, `NUMA node N has N DSA, its slots need N`, `DSA <BDF> is not on vfio-pci` | vfio-pci kept its denylist, a node has fewer DSA than slots, or a DSA would not bind | `doc/dma.md`; DSA enabled in the BIOS on every node in use |
| `N of M sessions copied by DMA` | MTL gave a session of a DMA instance no DMA device, or it copied nothing | Its log: `rv_init_dma` and the DMA lines of its dumps |
| A change in an A/A run | The rough noise estimate of a few rounds | More rounds; a low confidence change is a hint |
| The job fails copying to the companion | A host has no bpftrace | Host image content, [ci_runner_setup.md](ci_runner_setup.md) |
| `No perf-compare report found` | The case failed or skipped before a round, as without a SUT or a VF on two PFs | The pytest log |
| `mtl-perf-*` units or `mtl_perf_*` groups left over | A job ended before its cleanup | `task ci:perf-compare -- sweep` on each host; `-- clean` also deletes both slot trees, on the companion only with `SHADOW_HOST_IP` and `SHADOW_HOST_USER` set |
| EAL: "directory path /tmp is world-writable and insecure" | The checkout is below a world-writable directory | Move it out of `/tmp` |
