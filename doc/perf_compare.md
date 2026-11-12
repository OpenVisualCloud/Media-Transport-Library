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
| How is the L3 shared? | Intel RDT CAT: equal, exclusive ways per slot; none with `split`, `redundant` or `whole`, whose slots share no L3 (§3.3) | Otherwise the larger footprint evicts the other, and the swap does not cancel that: it follows the instance, not the slot. |
| One NUMA node for both slots, or one each? | Both on the node of their NIC; with `split`, `redundant` and `whole` one on each NIC's node, a socket each (§3.5) | A slot on the other node would reach its NIC and hugepages across the socket link, which no production MTL does (DPDK's control data aside, §3.1); the swap cancels that in the mean but not in the spread. |
| How many CPUs has a slot? | All its node gives: on the perf pair half its isolated cores for each slot of `single`, all of them with `split`, `redundant` and `whole` (§3.1) | MTL starts as many schedulers as its quota needs, so more CPUs leave them as they are and give the application threads room; a smaller count only starved those. |
| How do the builds use a second NIC? | As `nics` says: `split`, `redundant` and `whole` give each build a NIC and its socket (§3.5) | At a core's limit two instances on one socket fall short of it; the swap cancels the difference between the sockets. |
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
| Start | Both RX applications, then both TX ones (a unicast TX session waits for the ARP reply of its RX), each a transient systemd unit (§3), and the companion's ARP keepalive (§3.6) | |
| Steady, warm-up | Once a dump shows an instance's sessions and started schedulers, its app threads leave the scheduler CPUs (§3.1). Every session of every instance must then reach 99% of its frame rate within `120 + 3 × TX queues` s, a queue a session and port, or the round is overloaded: measured, and noted so, unless a session collapsed (§4.4); then 30 s of warm-up | |
| Untraced, `test_time` | `perf stat`, with `/proc`, RDT and CLOCK_MONOTONIC read at both ends. Nothing is timed: the production view | Those whose whole period lies between the two `/proc` reads: the primary rows |
| Traced, `test_time / 2` | `sys:tasklet_time_sample` and `sys:tasklet_stat`, where the build has them: each scheduler times 1 loop in 64, a cost the primary rows must not carry | Those with `sys:tasklet_stat` records; the first, partly untraced, is only the base of the counters. The time and work of each tasklet |
| Timed, `test_time / 2` | `sys:tasklet_time_measure`, which every release has: every call is timed, which costs 10-45% of the loop and would add to the traced counters | Those with call times but the first, which started untimed: the time of every tasklet call |
| Sessions, `test_time / 2` | `sys:sessions_time_measure`, which every release has: every video session is timed inside its tasklet's call, and its clock reads would add to the call times | Those with session times but the first: the time of every video session |
| Stop | The last probe is detached a dump period early, so that each application prints the dump that closes it; SIGINT stops TX, then RX, and the logs go to the pytest log folder | The last dump of the run is dropped |

MTL prints a stats dump every 10 s, its dump period. Each window after the untraced one
gets a dump period more at each end. The dumps are placed by each host's own
CLOCK_MONOTONIC (`/proc/timer_list`, the clock of bpftrace), so the four processes share
one span without a clock common to the hosts.

Each round swaps the slots, 4 rounds by default. Through each round the companion keeps
the switch learning the RX ports, and each window counts what every RX got against
what the TX of its slot sent (§3.6).

## 3. Isolation

One instance on one host, from the outside in:

```text
systemd-run --unit mtl-perf-rx0          AllowedCPUs=0,4-17 AllowedMemoryNodes=0 (cgroup v2), Nice=-20
└─ unshare --mount --ipc                 its own mount table and SysV IPC
   └─ sh /tmp/mtl-perf-compare/A_rx0.sh
      ├─ mount --bind <slot tree> <mtl>/.local_install     its build, at the path DPDK expects (§3.2)
      ├─ mount -t tmpfs tmpfs /var/run/imtl                no MtlManager; a private lcore table
      ├─ echo $$ > /sys/fs/resctrl/mtl_perf_0/tasks        its L3 ways (§3.3)
      └─ cd <mtl> && exec env LD_LIBRARY_PATH=<its libs> RxTxApp … --lcores 5-17 …
                                       schedulers on 5-17, application and EAL control threads on 4 (§3.1)
mtl-perf-rx0-{log,trace,timed,sessions}  bpftrace -p <pid>, the probes of each window, on CPU 1
mtl-perf-rx0-pmu                         perf stat -p <pid>, the untraced window, on CPU 1
mtl-perf-arp (companion)                 an ARP probe for each RX port every 10 s, on CPU 1 (§3.6)
```

### 3.1. CPU

`cpu_layout()` splits the physical cores of the VF's NUMA node into equal slots, one
hardware thread each so no slot holds an SMT sibling of another's, after the node's
first two cores, on node 0 the OS's and the collectors'. A node has two slots, or one with `split`, `redundant` and
`whole`, which give each build a node of its own; their two slots take as many cores as
the smaller of the two has. The slots are the node's isolated cores when they give each
8 (`ISOLATED_SLOT_CPUS`, the slot of the measurements below; 36 x 1080p59 RX without DMA
take 7) or are at least as many as its other cores, else those other cores, so a slot is
isolated where that gives it 8 and otherwise as large as the node allows. A slot needs 3
of one kind (`SLOT_CPUS`: the application threads', the system tasklets' and a
scheduler's). `isolcpus=4-31,68-95` (and the same `nohz_full`
and `rcu_nocbs`) isolates 28 cores on the node of each NIC of the perf hosts: the two
slots of `single` are then 4-17 and 18-31 on NIC 0's node 0, and the one slot of
`split`, `redundant` and `whole` all 28: 4-31 on node 0, 68-95 on NIC 1's node 2. With
node 0's alone, NIC 1's slots are cores that are not isolated, which those modes refuse.

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
`mtl_perf_1` get `ways / 3` L3 ways each, at the end of the mask away from the
`shareable_bits`. Those are the ways the NIC writes into (DDIO), which both instances
use and neither may own, so the default group keeps them with the rest: with 12 ways and
`shareable_bits` `c00`, the slots get `00f` and `0f0` and the default group `f00`. Each
instance joins its group before `exec`, and the group also gives its memory bandwidth
(MBM) and LLC occupancy (CMT). MBA is not used: it throttles by inserting delay, which
changes the latency it would protect. With `split`, `redundant` and `whole` the slots are on different sockets and share no L3,
so their groups keep every way and only monitor.

On these SNC2 hosts kernel 6.8's resctrl does not scale MBM and occupancy by the
sub-NUMA clusters of an L3 (6.11 does), so both read twice what the instance moves and
holds: main's TX instance at 72 x 1080p59 in yuv422p10le reads 236 GB/s on a node
whose four DDR5-6400 channels peak at 205. A ratio of two builds holds; an A/A run
cannot show a factor both share.

A host without RDT runs without CAT, as the report's placement table says. The case
restores the default mask, saved in `/run/mtl-perf-compare.schemata`, at its end, or
after an aborted job `perf-compare.sh clean` (§6).

### 3.4. What still differs

| Residual effect | Handling |
| --- | --- |
| PF 0 and PF 1, and the two core sets, are not identical | The slot swap: each instance runs once in each slot |
| With `split`, `redundant` and `whole`, the slots are on different sockets and NICs, and socket 0 also carries the OS cores, the main lcore CPU 0 and the collectors on CPU 1 | The slot swap; a slot that is isolated while the other is not refuses the run |
| The two hosts are not identical | Both slots of a host are treated alike; the hosts are never compared |
| Shared memory controller, DDIO ways, socket power budget | Common mode; an A/A run measures what is left. The nightly is one when `main` did not move, and equal refs run the same binaries |
| Another job on the companion, which serves other repositories too | The round notes a `Runner.Worker` on the companion at its start or end |
| Host tasks and IRQs on the SMT siblings of slot cores, IRQs on the slot cores | Not prevented: the cpuset bounds the instance, not the host. A task on a slot core shows as `sch preemptions (/s)`, the rest as a longer loop and a wider spread |

### 3.5. Two NICs and DMA

`nics` sets how the builds use the NICs of the perf pair, each of two ports, NIC 0 on
node 0 and NIC 1 on node 2. The topology then declares all four ports
(`PERF_PORTS=4`); NIC 1's ports take a VF each and the subnets 100 above NIC 0's.

| `nics` | Slots of a host | Results |
| --- | --- | --- |
| `single` | 0 and 1: port 0 and 1 of NIC 0 | As above |
| `split` | 0: port 0 of NIC 0, 1: port 0 of NIC 1, each the one slot of its NIC's node, with all its 28 isolated cores (§3.1) | As `single`; each build has a socket, its L3, memory and NIC, to itself, which a load at the limit of a core needs (below). The swap cancels the difference between the NICs in the mean, not in the noise |
| `redundant` | As `split`, with port 1 of the NIC beside port 0: ST 2022-7, each session sent and received on both | As `split`; the port rate, `hw dropped pkts` and `RX work` rows hold both legs. A redundant session is charged on both ports, so multi-core instances take perf-pytest's redundant quotas: 18 1080p59 sessions a TX scheduler, 10 an RX one, 9 with a DSA, unless `args` give one |
| `whole` | As `redundant`, but each port with sessions of its own, port 0's filled before port 1's (below; `jpegxs` halves them) | As `split`; one build loads a whole NIC, up to 72 x 1080p59. At that load MTL's quota of 12 sessions a scheduler, or 8 for RX without DMA, gives an instance 6 schedulers, or 9, which with the system tasklets' CPU and the slot's first take 8 and 11 of its 28 |

Two instances on one socket take some of each other's capacity, which a load at the
limit of a core shows. Measured on the perf pair with perf-compare's units at 36 x
1080p59 RX with a DSA on one core (`single_core_rx`), perf-pytest's single core limit:

| Instances on node 0 | L3 of a slot | Result |
| --- | --- | --- |
| One | 5 of 16 ways | 59.94 fps, no drop, 16 us loops |
| Two, twice | 5 of 16 ways | 3 of the 4 below 1 fps, a third of the packets dropped by the NIC |
| Two | 5 ways and the 2 the NIC writes into | 6 and 40 fps |
| Two | 7 of 16 ways | 53 and 55 fps, up to millions of drops a dump |
| Two, three times | All 16, no CAT | Once both at rate; twice one or both just under 99% |

`split` gives each build what the lone instance had, a socket of its own, and carries
that load: in CI from 2026-10-27 to 2026-11-03, 36 x 1080p59 single core RX with a DSA
failed all 5 runs with `single`, every session of both builds near 0 fps, and passed its
2 runs with `split`. So a single core comparison at a core's limit takes `split`,
`redundant` or `whole`; below it `single` keeps both builds on one NIC.

`dma` gives the RX instances of A, B or both `--dma_dev`: a DSA of the slot's node, the
i-th slot of a node its i-th DSA, bound to vfio-pci, so the DSA follows the
slot when the builds swap. vfio-pci must already allow DSA (`disable_denylist`, set by
the session's `host_dma_devices`): lifting it in the case would reload the module and
unbind the VFs. MTL falls back to the CPU copy for a session it gives no DMA, so a
round in which a session of a DMA instance copied nothing by DMA is invalid. DMA also
changes MTL's scheduler quota (12 rather than 8 1080p sessions a scheduler), so its
scheduler rows move with it.

### 3.6. The network

The eight perf ports are access ports of a VLAN of their own on the switch the fleet
shares, since 2026-11-05; before that of the fleet's VLAN. An RX VF sends nothing but
its ARP replies at its start, so the switch's MAC aging forgets it 300 to 600 s later and
floods the unicast to it over the VLAN. Each flooded stream then keeps its share of a
port's 100G, 25 Gb/s of four in `whole`, while the RX drops nothing: in November 2026
that hit every round longer than 600 s and some shorter ones.

So through each round the companion's keepalive, `mtl-perf-arp`, sends an ARP probe
(RFC 5227: sender IP 0, so no host caches it) for the IP of each RX port every 10 s,
from its PF of the same port (`keep_learned()`; the round keeps its `arp.log`). MTL
answers any ARP request for its IP, `v26.01` too, and its reply teaches the switch the
RX VF's MAC again.

Each window then counts what every RX instance got, plus what its NIC dropped, against
what the TX of its slot sent, from both instances' stats dumps (`delivered()`). Clean
windows got 1.0000 to 1.0002 of it, the floods 0.26 to 0.96; under 0.99 the round is
invalid. The later windows, which the two hosts start seconds apart, leave their first
and last dump out. The PFs' MAC counters cannot tell it: a flooded port counts the other
streams too.

A switch that forgets nothing on these ports, or a cable from host to host, makes the
keepalive moot; see [the perf pair needs a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own).

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
| `scheduler`, `application`, `other` and `all threads (CPUs)`, `utilization (%)`, `per frame (ms)` | `/proc/<pid>/task/*/stat`, `schedstat` | CPU by stage: the schedulers (the `t_pid` of each `SCH` line) send or receive the packets, the application threads (`tx_st2[02]p_N`, `rx_st2[02]p_N`) fill or read the frames and convert them: the CPUs a kind ran on, its CPU time over them, and per frame |
| `sch preemptions (/s)` | `nonvoluntary_ctxt_switches` of the scheduler threads | Isolation quality; what the swap leaves is a cost of the build |
| `memory bandwidth (MB/s)`, `LLC occupancy (MB)` | `mon_data` of the RDT group | Memory traffic over the window, and the cache footprint at its end |
| `IPC`, `core clock (GHz)` | `perf stat`: `instructions`, `cycles`, `task-clock` | Instructions per cycle, and the clock while the threads run; a clock that differs moves every time row |
| `LLC hits (M/s)`, `LLC misses (M/s)`, `LLC miss ratio (%)` | `perf stat`: `cache-references`, `cache-misses` | L3 requests of the instance's cores that hit or went to memory; a packet read while still in the DDIO ways is a hit |
| `fps (worst session)`, `fps (mean of sessions)`, `RX/TX rate (Gb/s)`, `RX/TX rate, port N (Gb/s)` | `RX/TX_VIDEO_SESSION(...): fps`, for st22p RX `RX_ST22P(...), frame get try ... succ` (§4.7), `DEV(N): Avr rate` | The workload: the lowest median frame rate of a session and the mean of them, and the rate of the instance and, on two ports, of each |
| `incomplete frames`, `hw dropped pkts` | `incomplete frames`, `Status: rx_hw_dropped_packets` | Loss in the receiver |
| `encode fail`, `decode fail`, `busy drop frame` | `TX_ST22P(...)`, `RX_ST22P(...)` lines, printed when they moved | st22p only: frames the codec failed, or that the RX pipeline had no free frame for (§4.7) |
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
| Higher | `fps (worst session)`, `fps (mean of sessions)` |
| Lower | Every other scored row, a cost: loop and call times (a call is also a wait for the tasklets after it), CPU, preemptions, memory traffic, L3 misses, loss and late frames, st22p's stage times |
| No direction | `core clock`; `IPC`, which idle polls raise as much as speed does; LLC occupancy, hits and miss ratio; the port rates, which carry pads and other traffic; the re-plans, which releases count differently; the utilization, whose CPU time the CPU per frame scores; the CPUs of every kind of thread but the schedulers, which follow the CPUs the schedulers take; st22p's `encode to send` |

A change of a row with a better direction is a regression (❌) or an improvement (✅) of
the candidate, of any other row it only differs (➖); the mark, the word and the bold
better value carry it for a reader who cannot tell the tints apart. The summary's
verdict counts them on the key rows, and the avg and p99 of an st22p codec's encode and
decode and of each tasklet's calls follow outside it: the timed window runs slower than
production, and a tasklet on several schedulers would count several times. The rough
noise of a few rounds lets an A/A run change a row now and then.

The case passes or fails on the validity of its rounds (§6), never on a score. A load
the builds cannot carry is a valid round, an overloaded one. For a role whose builds
delivered different frame rates, `fps (mean of sessions)` a change by the rule above,
the verdict scores the frame rates, the costs of a frame or a packet (`scheduler
threads per frame`, `RX work (ns/pkt)`) and the loss and late rows, and says so, as the
costs of a second grow with the frames a build delivers; the tables still mark those.
An RX at the frame rate of its overloaded TX, in both builds, restates it, so its frame
rates are left out. At equal frames those compare, so a session that dips under the
rate changes nothing. A session under a tenth of its rate has collapsed or gets no
flow, which invalidates its round: perf-compare's collapses measured 0 to 0.1 fps a
session, the worst sessions of its overloads 32 fps and more. A round that raises
ends the rounds; the report keeps the earlier ones and names the error.

`perf_compare.html` gives the builds with every input of the run as the form names it
(`sessions` with the share of each port), the rounds, the summary, and each role's rows
in groups, every row shown: CPU by stage and the call times as tables of cells, the others
as tables of rows, the summary's key rows by the size of their change. The setup
follows, open, then each instance's command and RxTxApp configuration, folded. The job
summary keeps no style, so it gets `perf_compare_summary.html`: the same without the
setup and the instances.

### 4.5. Layout effects

A difference can come from where the data sits, not from what the code does. The
schedulers sit in one array in `struct mtl_main_impl`, so a few bytes more in
`struct mtl_sch_impl` or `struct mt_sch_tasklet_impl` moved one RX scheduler 10-15% on
the perf pair with no code change. When a candidate differs from the reference on one
scheduler only, compare the reference with itself padded to the candidate's struct sizes
before blaming the code, over enough rounds to see the slow state that `main` reaches on
its own in about 1 round in 10.

### 4.6. Compliance

With `compliance`, after the rounds, unless one of them failed, EBU LIST judges every
stream of each build: the CI's server, `ebu_server` of `test_config.yaml`. Each build runs alone, its TX and RX in
slot 0, its sessions multicast and a few to a group: as many as a kernel capture takes
with no drop, 950,000 packets a second, which four 1080p59 streams are; a 2160p59
stream, as many packets on one flow, is one a group, which no run has proven. On the
companion, the PF of slot 1's first port, which then sends nothing, joins one group at
a time, and `tcpdump -p` captures a second of it: the NIC's timestamps in nanoseconds,
the first 128 bytes of each packet. Each stream keeps its whole frames, from its second
RTP marker to its last, as libpcap loses packets of the first while it sets the
filter, and LIST gets the pcap. The RX runs only so that the switch keeps every group
registered (IGMP snooping); only the TX has to be steady.

The capture needs no clock common to the hosts. phc2sys holds the clock of the
capturing NIC within 2 µs of the companion's CLOCK_TAI, which RxTxApp's TX paces on,
as the capture fixture of the acceptance tests does on one host. A stream is compliant
when LIST finds it within ST 2110-21 type N, as MTL signals it, and every other check of
LIST's report passes too, from the RTP timestamps and sequence to the multicast
addresses. A group with a gap in a stream's sequence, which is what a packet the
kernel or the NIC dropped leaves, is not judged and says why, as is a group the capture
or LIST failed. A pass whose clock left the 2 µs during the captures, whose TAI-UTC
offset changed, or that judged no stream, fails the case. The verdicts are reported per
build, not scored.

What the pass is not: the scored rounds run unicast with both builds at once, the pass
multicast with one build alone in slot 0, so both are judged on the same NIC and CPUs.
In `single`, where both builds share node 0 in the scored rounds, a build alone has
more of its memory bandwidth. A capture on the PF of a port that sends would disturb
that TX, if promiscuous, or record the wrong timing, so the pass never captures on
one. 4320p is refused, as LIST does not judge 8K. A capture takes about 25 s: 9
captures for 36 x 1080p59 a build, 18 for 72, or for 36 in `redundant`, whose legs are
groups of their own. On the perf pair, main had 33 to 35 of 36 x 1080p59 compliant in
four passes, the others under the VRX's floor by 1 to 4 packets, when its TX counted no
late frame (2026-11-06).

An st22p stream (§4.7) LIST does not detect by itself: it marks it `needs_info`. Each
capture's pcap goes to LIST again with an SDP that names each of its streams
`jxsv/90000`, `TP=2110TPN`, `jxsv_sdp()`, and the case waits, 3 minutes at most, until
none is still ready or under analysis; the SDP is kept in the group's JSON. Such a
stream has no VRX and no TRO, as ST 2110-22 has none: LIST judges its class by its Cinst
alone, and the report shows `-` for the rest. A group takes as many st22p streams as
950,000 packets a second carry, 26 of 1080p59, so 60 a build are 3 captures in `single`
or `split`, 4 in `whole` and 6 in `redundant`. Proven with LIST on a loopback capture of
MTL's st22p TX (2026-11-09): it read 608 packets a frame at 60000/1001 and judged each
stream; the timing of the perf pair's TX is the first dispatch's.

### 4.7. Compressed video

With `codec` `jpegxs` every session is st22p, ST 2110-22: JPEG XS by the SVT-JPEG-XS
plugin, from the planar 4:2:2 10-bit Penguin clip at 3 bits a pixel, the size RxTxApp
gives a codestream, in RFC 9134 packets that MTL paces in software (TSC), as it paces
every st22 stream. Each slot builds its own plugin against its own libmtl (§5.2), from
the SVT-JPEG-XS of the workflow commit's `versions.env`, so both builds run the same
codec, each compiled against its own headers; MTL checks no more of a plugin than its
API version. The SVT plugin asks for MTL 26.09 or later, which the refs job checks by
the ref's `VERSION`: MTL 26.01 has the same st22p API, and the plugin compiles against
it, but meson refuses it.

What changes in the measurement:

- An st22 RX session counts no frames in its own dump line, whose fps reads 0: its
  frame rate is that of the frames its pipeline gave the application,
  `RX_ST22P(<name>), frame get try N succ M` over the time since that session's last
  such line. The TX rate is its session's, which counts a frame when its builder
  finished it.
- `encode fail` and `decode fail` count the frames the codec failed, `busy drop frame`
  those the RX pipeline had no free frame for, as its decoder or application lagged;
  they are loss rows (§4.4).
- The codec's threads, which keep RxTxApp's name as they set none, are the `codec`
  row of CPU by stage, apart from MTL's `other` threads. MTL's CNI thread also has no
  name of its own, so it counts there too; it polls a few control packets.
- In the timed window a bpftrace collector on the st22p pipeline's probes times each
  frame's stages, by session and frame buffer. On TX, `encode` and `encode to send`,
  until the transport takes the frame, which holds the frame's slack before its slot and
  has no better direction. On RX, `decode wait`, `decode` and `receive to app`, until
  the application gets the frame. Each has its average and maximum, exact, and its p50
  and p99, from 0.1 ms buckets, in `st22p stages, timed` ([metrics
  §4.8](perf_compare_metrics.md#48-st22p-stages-timed)). The probes are the same since
  v26.01.
- SVT-JPEG-XS gives its threads SCHED_FIFO 99 as root, so they would starve the
  application threads on the CPUs they share, which CFS schedules: an st22p instance
  runs with `RestrictRealtime=yes`, which refuses it, and SVT then only warns. That is a
  seccomp filter, which before Linux 5.16, by default, also forces the speculative store
  bypass mitigation on the whole instance; the perf pair runs 6.8, whose
  `spec_store_bypass` reads "disabled via prctl" (2026-11-06).
- An st22p RX moves its application threads (§3.1) once every session got frames, as
  its decoder starts its own threads at its first frame, which would otherwise be born
  on one CPU and stay there.
- Each instance's log must name a session on `st22_svt_jpeg_xs_plugin*` for each of
  its sessions. MTL gives a session the first device that takes its formats, so a
  registry that also named another, as the sample plugin that claims JPEG XS and only
  copies the frames, would give it sessions.

A build takes up to 60 sessions, of about 0.4 Gb/s each at 1080p59:

- The plugin codes 8 sessions a direction each time MTL registers it, and MTL registers
  8 plugins, which it tells apart by their paths: a slot's registry names the plugin and
  7 links to it, so 64 sessions find a codec.
- MTL puts all st22 sessions of a TX instance on one scheduler, whose table takes 60.
  Their quota, `st22_frame_bandwidth_bps()`, counts the codestream's bytes where st20's
  counts bits, so a scheduler's quota would take about 640 of them, and MTL picks a
  scheduler by its quota alone: the 61st session fails in `tv_mgr_attach()`. RX's quota
  counts the decoded frame, as RxTxApp sets no `max_codestream_size`: 60 a scheduler at
  1080p59, 52 with `redundant`, whose RX then takes a second. perf-compare measures MTL
  as it is.
- MTL tries each full copy before the next, so a healthy log has `all session slot are
  used` and `fail to create one session at 8` lines.
- Within those 60, the codec's CPU and the companion's hugepages bound a build, which a
  run measures and reports rather than refuses: an overloaded build is a result. A TX
  session holds about 0.27 GB of hugepages, by estimate the clip and its frames, and with
  `single` both builds' TX instances share one node of the companion, 24 GB on the perf
  pair: about 40 sessions a build, beyond which a round would fail, its source not in
  hugepages or a session not created.

Only 1080p has a planar clip short enough for each TX session's hugepages. DMA is
refused, as MTL's st22 RX has none, and `pixel_format` takes `none` or `yuv422p10le`,
the same: the encoder takes the planar clip as it is, and st22p converts nothing.
`whole` puts half of a build's sessions on each port of its NIC, as filling the first
would leave the second idle; `compliance` judges them as §4.6 says.

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
| `codec` | `--perf_compare_codec` | `none` | `none`, st20p, uncompressed, or `jpegxs`, st22p with JPEG XS (§4.7) |
| `sessions` | `--perf_compare_sessions` | `36` | Video sessions of each build. `whole` puts a build on two ports and fills the first before the second: 37 x 1080p59 run as 36 and 1, 20 as 20 and none; `jpegxs` puts half on each (below) |
| `nics` | `--perf_compare_nics` | `single` | `single`, `split`, `redundant` or `whole` (§3.5); each choice of the form says what it does, and the case takes the word before its colon |
| `dma` | `--perf_compare_dma` | `none` | `none`, `A`, `B` or `both`: the builds whose RX instances copy with a DSA (§3.5) |
| `single_core_rx`, `single_core_tx` | `--perf_compare_single_core_rx`, `--perf_compare_single_core_tx` | unchecked | The RX or TX instances run as perf-pytest's single core: `--sch_session_quota 99` without `--dedicated_sys_lcore`, one scheduler with the system tasklets. A core carries 28 RX, 36 with DMA, or 35 TX 1080p59. Unchecked, MTL starts the schedulers its quota needs |
| `test_time` | `test_time` of `test_config.yaml` | `120` | Seconds of the untraced window; at least 30, for two whole stats dumps. The three later windows add half of it each |
| `rounds` | `--perf_compare_rounds` | `4` | Rounds; A and B swap places each round, so keep it even. Fewer than 4 give a rough noise estimate |
| `compliance` | `--perf_compare_compliance` | unchecked | After the rounds, EBU LIST judges every stream of each build (§4.6); needs `ebu_server` in `test_config.yaml`, which `config-perf` writes from the runner's `runner.env` |

Where two inputs depend on each other, one input limits the other, and the refs job,
or the case for what only the case knows, refuses a pair that cannot run before
anything is built or started:

| Inputs | How they interact | Checked by |
| --- | --- | --- |
| `sessions`, `video`, `nics` | A port carries at most 100 Gb/s of sessions, a build on two ports twice that (below) | refs job; the case against each port's own link |
| `sessions`, `single_core_rx`, `single_core_tx` | An MTL scheduler takes 60 video sessions at most | refs job and case |
| `compliance`, `video` | LIST does not judge 8K | refs job and case |
| `codec` and `sessions`, `video`, `dma`, `pixel_format` | `jpegxs` takes up to 60 sessions, 1080p, no DMA, and `pixel_format` `none` or `yuv422p10le` (§4.7) | refs job and case |
| `codec`, `nics` | With `jpegxs`, `whole` puts half of a build's sessions on each port (§4.7) | none needed |
| `codec`, `ref_a`, `ref_b` | `jpegxs` takes builds whose `VERSION` is 26.09 or later, as the SVT plugin asks (§4.7) | refs job |
| `nics`, `single_core_rx`, `single_core_tx` | At a core's limit two instances on one socket fall short of it, both builds alike: `split`, `redundant` and `whole` give a build a socket (§3.5) | none needed |
| `pixel_format`, `sessions`, `nics` | The conversion runs on the application threads, on the slot's first CPU and the lcores the schedulers leave: it moves each frame through the memory of the slot's node several times, which binds first: 36 x 1080p59 in yuv422p10le hold their rate in `split` and `whole`, not in `single`, where both builds share node 0 (below) | none needed |
| `args`, `nics` | With `redundant`, a build's `--sch_session_quota` replaces the quota the case gives (§3.5) | none needed |
| `args`, `single_core_rx`, `single_core_tx` | `--sch_session_quota`, `--sch_data_quota`, `--dedicated_sys_lcore` and `--rx_separate_lcore` would give a single core instance more schedulers | case |
| `args`, `dma`, `test_time`, the slots | `--dma_dev`, `--tasklet_time` and `--lcores` are the case's own; by any prefix RxTxApp takes | case |
| `nics`, hardware | A NIC's node needs 3 cores of one kind a slot, isolated or not, after its first two: two slots with `single`, one with `split`, `redundant` and `whole` (§3.1); `split`, `redundant` and `whole` take two ports of each of two NICs on two nodes, whose cores are isolated alike | case |

A 59 fps session takes 2.76 Gb/s on the wire at 1080p, 11.0 at 2160p and 44.1 at 4320p
(20 bits a pixel, paced over 1125 of 1080 lines, in MTL's packets of 1320 bytes of
pixels, 1406 on the wire); 50, 29 and 25 fps take 0.83, 0.5 and 0.42 of that. 36 x
1080p59, 99.3 Gb/s, is the most a 100G port carries (at 37 no session reached its
rate); 2160p59 fits 9, 4320p59 2. `whole` gives a build two ports: it fills port 0
with as many sessions as 100G carries and puts the rest on port 1, so 37 x
1080p59 run as 36 and 1, and 72 fill both; with no sessions left port 1 is not used.

With a `pixel_format` other than `none` the applications work in that format, as an
application in that format does: MTL converts a frame to the format on the wire in `st20p_tx_put_frame()` and back in
`st20p_rx_get_frame()`, both on the application threads and with its own SIMD
converters, as no converter plugin is registered. Their CPU shows in the application
threads' utilization and CPU per frame in CPU by stage, apart from the schedulers. The TX source stays the perf media in the format
on the wire, read as frames of the chosen one: a conversion does the same work whatever
the pixels hold, and nothing checks them.

The conversion costs the application threads several CPUs. In the slots of 8 CPUs that
`single` had with `isolcpus=4-20,68-84`, 36 x 1080p59 sessions held 33 fps in yuv422p10le
and 43 in v210, the TX application threads using every CPU the schedulers left. These
held their rate:

| `pixel_format` | Sessions at 1080p59 | Cores of the TX application threads, of the 5 free | RX application threads |
| --- | --- | --- | --- |
| `yuv422p10le` | 20 | 4.5 | 2.5 |
| `v210` | 24 | 4.1 | 2.5 |

By hand, one instance a host on a slot of 16 CPUs carried 36 x 1080p59 TX sessions in
yuv422p10le at 59.94 fps, their application threads using 8.6 cores.

With the slots of `isolcpus=4-31,68-95`, 14 CPUs in `single` and 28 in the other modes,
the memory of the slot's node binds before its CPUs: in yuv422p10le TX held 46.1 fps at
36 x 1080p59 in `single`, both builds on node 0, and main 45.3 fps at 72 in `whole`,
moving about 118 GB/s, half what the report reads (§3.3); 36 in `whole` hold their rate.

`y210` was not measured; its frames are as large as yuv422p10le's. The pipelines add frames of that format on the
node of the session, 2 a TX session and 3 an RX one: at 36 x 1080p, 0.6 and 0.9 GB an
instance in planar or Y210, two thirds of that in V210.

At 36 x 1080p59 every session holds its rate, but two `main` TX instances on the
companion count about 210,000 build timeout frames each a round, which the rounds note;
`main` beside `v26.01` counts none. Such late frames follow the load of both TX
instances, so an A/A run of `main` notes them every round.

The refs job rejects a bad ref, a number that is not a positive integer, a `test_time`
under 30, and the pairs of inputs it checks above, said in its log and the job summary, a
bad number first and then every other one at once, before the build job starts, which
waits for it; the
numbers are strings, as a number input reaches a step as `120.0`. A ref takes no option, `^{/regex}`, comma or newline:
it reaches `git`, `GITHUB_OUTPUT` and the labels. The case rejects bad arguments before
it starts an instance, as they reach a root shell script on both hosts.

### 5.2. Jobs

1. `refs` resolves both refs to SHAs (`task ci:workflow -- perf-compare-refs`).
2. `build`, once `refs` passed, runs `build.yml` for the workflow commit, which the framework itself runs:
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
workspace's `.local_install`, the path DPDK bakes in, and kept in `<workspace>/../perf-
compare/{a,b}` on both hosts. With `codec` `jpegxs` a slot also holds the SVT-JPEG-XS
plugin, from the workflow commit's `build_jpegxs.sh`, and the registry that names it,
`conf/kahawai.json`. Only the workflow commit's DPDK and MTL, and with `jpegxs` its JPEG
XS and registry, step aside meanwhile, for the ffmpeg watchdog of
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
`<mtl>/.local_install` and copied to the same path on both hosts; `perf-compare.sh
build` with `SHA_A` and `SHA_B` set to full SHAs builds both in `../perf-compare/{a,b}`,
and with `CODEC=jpegxs` the plugin and registry that `--perf_compare_codec jpegxs`
needs: its registry names the plugin 8 times, where a local install's names it once,
which takes 8 sessions a build. `<mtl>/.local_install` twice is an A/A run of the
current build. The report is
`logs/performance/latest/perf_compare.html`, each round's logs and records in
`perf_compare/round<N>/` next to it.

## 6. Troubleshooting

The Rounds table of the report gives each round's problems, which fail the case, and
notes, which do not.

| Problem, or (note) | Cause | What to do |
| --- | --- | --- |
| `not steady after N s: <instance> (N fps at worst, N of M sessions)` or `(no session yet)` | An instance did not run all its sessions in its last dump | Its application output: the sessions it created |
| `under the rate after N s`, `a session fell to N fps` (notes) | A session stayed under 99% of its rate. The CPUs of a slot, two instances on a socket at a core's limit (§3.5), or the companion's hugepages can limit the sessions before the link | The frame rate rows score it; fewer `sessions` for a comparison at the rate |
| `a session at N fps, collapsed or no flow` | A session delivered under a tenth of its frames: no flow to it, or two instances on a socket at a core's limit (§3.5) | The application output and the wire; `split` for single core at the limit |
| `N schedulers, single core runs one` | A single core instance had more than one scheduler, which none of the options the case allows asks for | Its application output: the `sch_start` lines and the scheduler of each session |
| `exited before steady state`, `exited during the measurement` | An application or a collector stopped | The application's output and its `.log` and `.trace` records in `perf_compare/round<N>/`; a timed or sessions collector's output only stays in `/tmp/mtl-perf-compare/` on its host, until the next round |
| `N untraced, N traced, N timed and N session timed dumps`, `N of M sessions` | A window held fewer than two whole dumps, or not every session reported | A `test_time` of 30 or more; the application and collector logs |
| `its source is not in hugepages` | A TX read its source through `mmap()` | More hugepages on the companion, or fewer sessions |
| `exited before steady state`, and `all session slot are used` then `st22p_tx_create fail` in an st22p instance's output | Its registry names the plugin too few times for its sessions, 8 sessions a name: a local install's names it once | Slots that `perf-compare.sh build` built with `CODEC=jpegxs`, or 8 sessions at most |
| `N of M sessions on st22_svt_jpeg_xs_plugin*` | An st22p instance's log names fewer of its sessions on the plugin under test: another plugin coded them, or the log does not say, as with `args` that log above info or to a file | Its application output: the `st_plugin_register` and `get one session` lines, and the slot's `conf/kahawai.json` |
| `codec jpegxs takes ...` (refs job, every one at once), `--perf_compare_codec jpegxs takes ...` or `runs 1080p` | An input that st22p with JPEG XS cannot run with (§4.7) | The inputs the messages name |
| `no rvs_pkt_rx work in sys:tasklet_stat` | A traced RX instance gave no work counters | Its `.trace` record |
| `failed: <error>` | The round raised, such as when an application died between two `/proc` reads; the rounds stop | The pytest log |
| Incomplete frames, dropped packets, late TX frames (note) | Loss in the receiver, or TX frames sent late | Their rows count them |
| `got N% in the <window> window of what its TX sent ...: lost between the hosts or in its NIC` | The RX got under 99% of what its TX sent and counted no drop of the rest (§3.6) | The round's `arp.log`; [a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own) |
| `a CI job ran on <host>` (note) | Another job on the companion at the round's start or end | Rerun if the round stands out |
| `the builds pace TX differently` (note) | A build's TX fell back from the rate limiter (`pacing way:`, info level) | Read its re-plan row as that, not as a gain |
| `compliance of <build>: LIST judged no stream` | No capture of the pass was whole, or LIST reported none of its streams | `perf_compare/compliance_A/` or `compliance_B/`, `<group>.json`: each capture's gaps and LIST's report; `mtl-perf-tx0.json`: the command and the multicast configuration |
| `compliance of <build>: phc2sys held <PF> within 2000 ns ...`, `phc2sys held <PF> not locked within 2000 ns ...` | The capturing NIC's clock did not follow the companion's CLOCK_TAI | `phc2sys.log` of `perf_compare/compliance_A/` or `compliance_B/`; another phc2sys or ptp4l on that NIC |
| `compliance of <build>: the TAI-UTC offset of <host> changed` | The kernel's offset changed during the pass | [TAI and capture clock requirements](ci_runner_setup.md#tai-and-capture-clock-requirements) |
| Not judged (Compliance) | A gap in a stream's sequence, as a packet the kernel or the NIC dropped leaves, a stream with no whole frame, or a capture or LIST that failed, as the row says | The capture's `<group>.json` |
| `perf stat did not count every event` (note) | The host's perf left an event of the instance uncounted, so it has no PMU rows | Its `.pmu` record in `perf_compare/round<N>/` |
| Placement `perf` no | No perf on the host counts every event: no PMU rows at all | A `linux-tools-<version>` package of a 6.8 kernel |
| Placement `CAT` no | No L3 allocation, or other resctrl groups | Remove the groups, or accept a shared L3 |
| `N x <video> on a port take N Gb/s, and <PF> links at N Gb/s` or `<PF> has no link` | The sessions of a port do not fit its link, or the port has none: `sessions` counts a build's, port 0 of `whole` filled first, or half on each with `jpegxs` | Fewer `sessions`, or the cable and the link of that port |
| `--perf_compare_nics <mode> takes two ports of each of two NICs`, `both NICs are on NUMA node N` | The topology declares fewer than four ports (`PERF_PORTS`), or they are not two ports of each of two cards on two nodes | The second card of both hosts, cabled port to port |
| `single core takes up to 60 sessions ...` | A single core role with more sessions than an MTL scheduler takes; the refs job refuses it before the build | Up to 60 `sessions` |
| `NUMA node N has N isolated free cores and N others, not N of either` | A NIC's node has fewer than 3 cores a slot of either, after its first two: two slots a node with `single`, one with `split`, `redundant` and `whole` (§3.1) | `split`, `redundant` or `whole`, or a host with more cores |
| `vfio-pci still refuses DSA`, `NUMA node N has N DSA, its slots need N`, `DSA <BDF> is not on vfio-pci` | vfio-pci kept its denylist, a node has fewer DSA than slots, or a DSA would not bind | `doc/dma.md`; DSA enabled in the BIOS on every node in use |
| `N of M sessions copied by DMA` | MTL gave a session of a DMA instance no DMA device, or it copied nothing | Its log: `rv_init_dma` and the DMA lines of its dumps |
| A change in an A/A run | The rough noise estimate of a few rounds | More rounds; a low confidence change is a hint |
| The job fails copying to the companion | A host has no bpftrace | Host image content, [ci_runner_setup.md](ci_runner_setup.md) |
| `No perf-compare report found` | The case failed or skipped before a round, as without a SUT or a VF on two PFs | The pytest log |
| `mtl-perf-*` units or `mtl_perf_*` groups left over | A job ended before its cleanup | `task ci:perf-compare -- sweep` on each host; `-- clean` also deletes both slot trees, on the companion only with `SHADOW_HOST_IP` and `SHADOW_HOST_USER` set |
| EAL: "directory path /tmp is world-writable and insecure" | The checkout is below a world-writable directory | Move it out of `/tmp` |
