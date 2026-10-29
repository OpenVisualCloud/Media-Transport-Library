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
| How is the L3 shared? | Intel RDT CAT: equal, exclusive ways per slot; none with `split` or `redundant`, whose slots share no L3 (§3.3) | Otherwise the larger footprint evicts the other, and the swap does not cancel that: it follows the instance, not the slot. |
| One NUMA node for both slots, or one each? | Both on the node of their NIC; with both NICs, each NIC's slots on its own node, with `split` and `redundant` a socket each (§3.5) | A slot on the other node would reach its NIC and hugepages across the socket link, which no production MTL does (DPDK's control data aside, §3.1); the swap cancels that in the mean but not in the spread. |
| How do the builds use a second NIC? | As `nics` says: `span`, an instance on a port of each NIC; `split` and `redundant`, a build on each; `pairs` or `pooled`, a pair of slots on each, rows apart or pooled (§3.5) | `span` loads one instance with two NICs; `pairs` and `pooled` repeat the comparison on the other socket; `split` and `redundant` give each build a socket, the swap cancelling it. |
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
| Steady, warm-up | Once a dump shows an instance's sessions and started schedulers, its app threads leave the scheduler CPUs (§3.1). Every session of every instance must then reach 99% of its frame rate within `120 + 3 × TX queues` s, a queue a session and port; then 30 s of warm-up | |
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

`cpu_layout()` gives each slot `cores` (8) physical cores of the VF's NUMA node, one
hardware thread each so no slot holds an SMT sibling of another's, after two cores for
the OS and the collectors. A node has two slots, or one with `split` and `redundant`,
which give each build a node of its own. The slots are isolated cores when the node
isolates as many as they take, else none is. `isolcpus=4-20,68-84` (and the same
`nohz_full` and `rcu_nocbs`) isolates 17 cores on the node of each NIC of the perf
hosts: two slots are then 4-11 and 12-19 on NIC 0's node 0 and 68-75 and 76-83 on NIC
1's node 2, and `cores` takes at most 8; the one slot of `split` and `redundant` takes
up to all 17, 4-20 and 68-84. With node 0's alone, NIC 1's slots are cores that are not
isolated.

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
protect. With `split` and `redundant` the slots are on different sockets and share no L3,
so their groups keep every way and only monitor.

On these SNC2 hosts kernel 6.8's resctrl monitoring knows no sub-NUMA clusters (that
came with 6.11), so check the memory bandwidth rows in an A/A run before trusting them.

A host without RDT runs without CAT, as the report's placement table says. The case
restores the default mask, saved in `/run/mtl-perf-compare.schemata`, at its end, or
after an aborted job `perf-compare.sh clean` (§6).

### 3.4. What still differs

| Residual effect | Handling |
| --- | --- |
| PF 0 and PF 1, and the two core sets, are not identical | The slot swap: each instance runs once in each slot |
| With `split` and `redundant`, the slots are on different sockets and NICs, and socket 0 also carries the OS cores, the main lcore CPU 0 and the collectors on CPU 1 | The slot swap; a slot that is isolated while the other is not refuses the run |
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
| `split` | 0: port 0 of NIC 0, 1: port 0 of NIC 1, each the one slot of its NIC's node, so `cores` goes up to the node's 17 isolated cores (§3.1) | One table per role, as `single`; each build has a socket, its L3, memory and NIC, to itself, which a load at the limit of a core needs (below). The swap cancels the difference between the NICs in the mean, not in the noise |
| `redundant` | As `split`, with port 1 of the NIC beside port 0: ST 2022-7, each session sent and received on both | As `split`; the port rate, `hw dropped pkts` and `RX work` rows hold both legs. A redundant session is charged on both ports, so multi-core instances take perf-pytest's redundant quotas: 18 1080p59 sessions a TX scheduler, 10 an RX one, 9 with a DSA, unless `args` give one |
| `pairs` | 0 and 1 on NIC 0, 2 and 3 on NIC 1, each on its NIC's node; NIC 1's pair runs the other way round, so every round has each build on both NICs | A section and rows of their own per NIC, all key rows in the one verdict |
| `pooled` | As `pairs` | One table per role, each NIC of each round a sample, so one round already gives a noise estimate: more samples, but the noise holds the NIC difference too |

Two instances on one socket take some of each other's capacity, which a load at the
limit of a core shows. Measured on the perf pair with perf-compare's units at 36 x
1080p59 RX with a DSA on one core (`single_core: RX`), perf-pytest's single core limit:

| Instances on node 0 | L3 of a slot | Result |
| --- | --- | --- |
| One | 5 of 16 ways | 59.94 fps, no drop, 16 us loops |
| Two, twice | 5 of 16 ways | 3 of the 4 below 1 fps, a third of the packets dropped by the NIC |
| Two | 5 ways and the 2 the NIC writes into | 6 and 40 fps |
| Two | 7 of 16 ways | 53 and 55 fps, up to millions of drops a dump |
| Two, three times | All 16, no CAT | Once both at rate; twice one or both just under 99% |

`split` gives each build what the lone instance had, a socket of its own; it is expected
to carry that load, which its first run at it shows.

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
| `schedulers (CPUs)` | `SCH(N:...)` lines | The CPUs MTL's schedulers poll, one each, of the slot's `--lcores`; Test Information gives the slot's size |
| `video cpu busy (%)` | `RX/TX_VIDEO_SESSION(...): ... cpu busy X` | MTL's load score of a session: its scheduler's avg loop over 3 packet intervals (RX) or a TX bulk of them, refreshed every 6 s ([perf_compare_metrics.md](perf_compare_metrics.md#43-cpu)) |
| `scheduler`, `application`, `other` and `all threads (cores)`, `per frame (ms)` | `/proc/<pid>/task/*/stat` | CPU by stage, in cores: the schedulers (the `t_pid` of each `SCH` line) send or receive the packets, the application threads (`tx_st20p_N`, `rx_st20p_N`) fill or read the frames and convert them; per frame, the cores over the frames per second of all sessions, shown only |
| `sch preemptions (/s)` | `nonvoluntary_ctxt_switches` of the scheduler threads | Isolation quality; what the swap leaves is a cost of the build |
| `memory bandwidth (MB/s)`, `LLC occupancy (MB)` | `mon_data` of the RDT group | Memory traffic over the window, and the cache footprint at its end |
| `IPC`, `core clock (GHz)` | `perf stat`: `instructions`, `cycles`, `task-clock` | Instructions per cycle, and the clock while the threads run; a clock that differs moves every time row |
| `LLC hits (M/s)`, `LLC misses (M/s)`, `LLC miss ratio (%)` | `perf stat`: `cache-references`, `cache-misses` | L3 requests of the instance's cores that hit or went to memory; a packet read while still in the DDIO ways is a hit |
| `fps (worst session)`, `RX/TX rate (Gb/s)`, `RX/TX rate, port N (Gb/s)` | `RX/TX_VIDEO_SESSION(...): fps`, `DEV(N): Avr rate` | The workload: the lowest median frame rate of a session, and the rate of the instance and, on two ports, of each |
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

[perf_compare_metrics.md §5](perf_compare_metrics.md#5-from-rounds-to-a-verdict) takes the
rules apart: a build's value is the mean of its valid rounds, a change is from 2% and
twice the noise, twice the standard error of the larger build's mean, and its confidence
is how many noises it spans. What each row counts as better:

| Better | Rows |
| --- | --- |
| Higher | `fps (worst session)` |
| Lower | Every other scored row, a cost: loop and call times (a call is also a wait for the tasklets after it), CPU, preemptions, memory traffic, L3 misses, loss and late frames |
| No direction | `core clock`; `IPC`, which idle polls raise as much as speed does; LLC occupancy, hits and miss ratio; the port rates, which carry pads and other traffic; the re-plans, which releases count differently; the CPU per frame, which follows its cores row |

A change of a row with a better direction is a regression (❌) or an improvement (✅) of
the candidate, of any other row it only differs (➖); the mark, the word and the bold
better value carry it for a reader who cannot tell the tints apart. The summary's
verdict counts them on the key rows, and the avg and p99 of each tasklet's calls follow
outside it: the timed window runs slower than production, and a tasklet on several
schedulers would count several times. The rough noise of a few rounds lets an A/A run
change a row now and then.

The case passes or fails on the validity of its rounds (§6), never on a score: for a
candidate that cannot carry the load, the failure is the result. A round that raises
ends the rounds; the report keeps the earlier ones and names the error.

`perf_compare.html` gives the builds, the rounds, the summary, and each role's rows in
groups: Delivery whole, CPU by stage and the call times as tables of cells, and the
others with the rows that changed shown and the rest folded. The setup follows, open,
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

The inputs of `perf-compare.yml` in the order of the form, in four groups: what is
compared, the load, where and how the instances run, and the measurement; and the pytest
options of a run by hand (§5.4):

| Input | pytest | Default | Meaning |
| --- | --- | --- | --- |
| `ref_a`, `ref_b` | `--perf_compare_slots`, `--perf_compare_labels` | `main`, `main` | A, the baseline, and B, the candidate: a branch, tag or commit, with an optional `~n` or `^`; the same twice measures the noise. pytest takes the two built trees and their labels |
| `args_a`, `args_b` | `--perf_compare_args_a`, `--perf_compare_args_b` | empty | Extra RxTxApp options of each build: letters, digits, spaces and `_./:=,+-`. RxTxApp ignores an option it does not know |
| `video` | `-k "1080p and 59fps"` | `1080p59` | 1080p, 2160p or 4320p at 59, 50, 29 or 25 fps, from the perf media set |
| `pixel_format` | `--perf_compare_pixel_format` | `none` | `none`, `yuv422p10le`, `v210` or `y210`: the pixel format of the applications, which MTL converts to the format on the wire on TX and back on RX (below) |
| `sessions` | `--perf_compare_sessions` | `36` | Video sessions of each build on each port it uses, in every `nics` mode (`span` puts that many on each of its two ports) |
| `nics` | `--perf_compare_nics` | `single` | `single`, `split`, `redundant`, `span`, `pairs` or `pooled` (§3.5); each choice of the form says what it does, and the case takes the word before its colon |
| `dma` | `--perf_compare_dma` | `none` | `none`, `A`, `B` or `both`: the builds whose RX instances copy with a DSA (§3.5) |
| `single_core` | `--perf_compare_single_core` | `none` | `none`, `RX`, `TX` or `both`: the roles whose instances run as perf-pytest's single core, `--sch_session_quota 99` without `--dedicated_sys_lcore`: a scheduler on each NIC node of the slot, the first NIC's with MTL's system tasklets. Alone, a core carries 28 RX, 36 with DMA, or 35 TX 1080p59 (perf-pytest); at that limit run `split` |
| `cores` | `--perf_compare_cores` | `8` | CPUs of a slot on each NIC node it uses: on the perf pair at most 8, or 17 with `split` and `redundant` (§3.1). The first takes the application threads, the rest are its `--lcores`: one for MTL's system tasklets except in a single core role, the others for as many schedulers as MTL's quota needs |
| `test_time` | `test_time` of `test_config.yaml` | `120` | Seconds of the untraced window; at least 30, for two whole stats dumps. The three later windows add half of it each |
| `rounds` | `--perf_compare_rounds` | `4` | Rounds; A and B swap places each round, so keep it even. Fewer than 4 give a rough noise estimate |

Where two inputs depend on each other, one input limits the other, and the refs job,
or the case for what only the case knows, refuses a pair that cannot run before
anything is built or started:

| Inputs | How they interact | Checked by |
| --- | --- | --- |
| `sessions`, `video` | A port carries at most 100 Gb/s of sessions (below) | refs job; the case against each port's own link |
| `sessions`, `single_core` | An MTL scheduler takes 60 video sessions at most | refs job and case |
| `cores`, `single_core` | At least 3 CPUs, or 2 when both roles are single core | refs job and case |
| `pixel_format`, `sessions`, `cores` | The conversion runs on the application threads, on the slot's first CPU and the lcores the schedulers leave: at `cores` 8 about 20 x 1080p59 sessions in yuv422p10le and 24 in v210 hold their rate; `split` and `redundant` take up to 17 (below) | none needed |
| `args`, `nics` | With `redundant`, a build's `--sch_session_quota` replaces the quota the case gives (§3.5) | none needed |
| `args`, `single_core` | `--sch_session_quota`, `--sch_data_quota`, `--dedicated_sys_lcore` and `--rx_separate_lcore` would give a single core instance more schedulers | case |
| `args`, `dma` / `cores` / `test_time` | `--dma_dev`, `--lcores` and `--tasklet_time` are the case's own; by any prefix RxTxApp takes | case |
| `nics`, `single_core` | `span` gets a scheduler on each NIC's node, as MTL schedules a session on its port's node; at a core's limit two instances on a socket fall short of it, `split` gives each build a socket (§3.5) | none needed |
| `nics`, `cores` | `cores` is a slot's on each node, so a `span` instance has twice as many; a node has two slots, or one with `split` and `redundant`, and the isolated cores of the perf pair give 8 a slot, or 17 (§3.1) | case |
| `nics`, hardware | `pooled` takes NICs alike in speed, firmware and isolation | case |

A 59 fps session takes 2.76 Gb/s on the wire at 1080p, 11.0 at 2160p and 44.1 at 4320p
(20 bits a pixel, paced over 1125 of 1080 lines, in MTL's packets of 1320 bytes of
pixels, 1406 on the wire); 50, 29 and 25 fps take 0.83, 0.5 and 0.42 of that. 36 x
1080p59, 99.3 Gb/s, is the most a 100G port carries (at 37 no session reached its
rate); 2160p59 fits 9, 4320p59 2.

With a `pixel_format` other than `none` the applications work in that format, as an
application in that format does: MTL converts a frame to the format on the wire in `st20p_tx_put_frame()` and back in
`st20p_rx_get_frame()`, both on the application threads and with its own SIMD
converters, as no converter plugin is registered. Their CPU shows in `application
threads (cores)` and per frame in CPU by stage, apart from the schedulers. The TX source stays the perf media in the format
on the wire, read as frames of the chosen one: a conversion does the same work whatever
the pixels hold, and nothing checks them.

The conversion costs the application threads several CPUs. At `cores`
8, two instances a host as in `single`, 36 x 1080p59 sessions held 33 fps in yuv422p10le
and 43 in v210, the TX application threads using every CPU the schedulers left. These
held their rate:

| `pixel_format` | Sessions at 1080p59 | Cores of the TX application threads, of the 5 free | RX application threads |
| --- | --- | --- | --- |
| `yuv422p10le` | 20 | 4.5 | 2.5 |
| `v210` | 24 | 4.1 | 2.5 |

By hand, one instance a host on a slot of 16 CPUs carried 36 x 1080p59 TX sessions in
yuv422p10le at 59.94 fps, their application threads using 8.6 cores. `split` at `cores`
16 has that slot, with the other build on the other socket; RX at that load is not
measured yet.

`y210` was not measured; its frames are as large as yuv422p10le's. The pipelines add frames of that format on the
node of the session, 2 a TX session and 3 an RX one: at 36 x 1080p, 0.6 and 0.9 GB an
instance in planar or Y210, two thirds of that in V210.

At 36 x 1080p59 every session holds its rate, but two `main` TX instances on the
companion count about 210,000 build timeout frames each a round, which the rounds note;
`main` beside `v26.01` counts none. Such late frames follow the load of both TX
instances, so an A/A run of `main` notes them every round.

The refs job rejects a bad ref, a number that is not a positive integer, a `test_time`
under 30, and the pairs of inputs it checks above, before the compare job builds anything; the
numbers are strings, as a number input reaches a step as `120.0`. A ref takes no option, `^{/regex}`, comma or newline:
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
| `not steady after N s: <instance> (N fps at worst, N of N sessions)` or `(no session yet)`, `a session fell to N fps` | A session stayed under 99% of its rate, in the last dump. The `cores` of a slot, or the companion's hugepages, which hold every TX source, can limit the sessions before the link | Fewer `sessions`; for a candidate that cannot carry the load the failure is the result |
| `N schedulers, single core runs one a NUMA node` | An instance of a `single_core` role had more schedulers than its slot has NUMA nodes, which none of the options the case allows asks for | Its application output: the `sch_start` lines and the scheduler of each session |
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
| `NUMA node N has N isolated free cores and N others, not N of either` | The slots of a NIC's node need more cores than it has of either: `cores` a slot, two slots a node or one with `split` and `redundant`; 8 or 17 on the perf pair (§3.1) | A smaller `cores`, or `split` |
| `vfio-pci still refuses DSA`, `NUMA node N has N DSA, its slots need N`, `DSA <BDF> is not on vfio-pci` | vfio-pci kept its denylist, a node has fewer DSA than slots, or a DSA would not bind | `doc/dma.md`; DSA enabled in the BIOS on every node in use |
| `N of M sessions copied by DMA` | MTL gave a session of a DMA instance no DMA device, or it copied nothing | Its log: `rv_init_dma` and the DMA lines of its dumps |
| A change in an A/A run | The rough noise estimate of a few rounds | More rounds; a low confidence change is a hint |
| The job fails copying to the companion | A host has no bpftrace | Host image content, [ci_runner_setup.md](ci_runner_setup.md) |
| `No perf-compare report found` | The case failed or skipped before a round, as without a SUT or a VF on two PFs | The pytest log |
| `mtl-perf-*` units or `mtl_perf_*` groups left over | A job ended before its cleanup | `task ci:perf-compare -- sweep` on each host; `-- clean` also deletes both slot trees, on the companion only with `SHADOW_HOST_IP` and `SHADOW_HOST_USER` set |
| EAL: "directory path /tmp is world-writable and insecure" | The checkout is below a world-writable directory | Move it out of `/tmp` |
