# Performance comparison of two MTL builds (perf-compare)

perf-compare runs one end-to-end workload on two MTL builds at once and reports how
the second differs from the first: `main` against yesterday's, a branch against
`main`, `v26.09` against `v26.01`, or two values of an RxTxApp option. It does not
search for the most sessions a host carries; `performance.yml` does that.

A is the baseline and B the candidate: every change is B's, `(B - A) / A`. An A/A
run, one build twice, measures the noise of the rig.
[perf_compare_metrics.md](perf_compare_metrics.md) describes every number of the
report.

## 1. Terms

| Term | Meaning |
| --- | --- |
| perf pair | The two CI hosts. Each has two E830 NICs of two ports: NIC 0 on NUMA node 0, NIC 1 on node 2 |
| SUT | The host of both RX applications, runner label `sut`, where the compare job runs |
| companion | The host of both TX applications, the ARP keepalive and the compliance captures |
| build | A or B: DPDK, libmtl and RxTxApp built from one ref |
| instance | One build's TX application and its RX application |
| slot | A fixed place for an instance on each host: CPUs, one or two ports, L3 ways and, with `dma`, a DSA. Each host has slots 0 and 1 |
| round | Both instances at once, one in each slot. The builds swap slots every round |
| window | A part of a round with one set of probes attached (§4.1) |
| dump | MTL's stats dump, every 10 s |

## 2. Running it

### 2.1. Quick start

Run the `perf-compare` workflow from the Actions tab, or:

```bash
gh workflow run perf-compare.yml -f ref_a=main -f ref_b=my-branch
```

By default each build runs 36 sessions of 1080p59 for 4 rounds. A round takes about 7
to 10 minutes at the default `test_time` of 120 s, plus the builds; the compare job
stops after 180 minutes. The job summary shows the verdict, and the artifact
`perf-compare-<runner>-<date>` holds the full report, `perf_compare.html`, and the
logs.

By hand, on a dual-host acceptance setup:

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml \
  tests/dual/performance/test_perf_compare.py -k "1080p and 59fps" \
  --perf_compare_slots=/srv/mtl/a,/srv/mtl/b --perf_compare_labels=main,my-branch
```

`-k` must select one case, because each case writes the run's one report. Without
`--perf_compare_slots`, pytest deselects the cases. A slot is a tree built at
`<mtl>/.local_install` and copied to the same path on both hosts.
`perf-compare.sh build`, with `SHA_A` and `SHA_B` set, builds both in
`../perf-compare/`, and with `CODEC=jpegxs` also the plugin and registry that
`--perf_compare_codec jpegxs` needs. `<mtl>/.local_install` twice is an A/A run of the
current build. The report is `logs/performance/latest/perf_compare.html`, with each
round's logs in `perf_compare/round<N>/`.

### 2.2. Requirements

- Both hosts in one acceptance topology, with `sudo -n`, systemd and cgroup v2, and
  Python 3 on the companion.
- On each host, a VF on both ports of NIC 0. With `nics` other than `single`, also
  both ports of a second NIC on another node, whose cores are isolated alike
  ([ci_runner_setup.md](ci_runner_setup.md#the-st-2022-7-leg-needs-both-ports-cabled-too)).
- On the node of each NIC in use, 3 cores for each slot after the first two (§3.1).
- `bpftrace` on both hosts; optionally a `perf` that counts every event (§4.2) and
  resctrl with L3 allocation (§3.3).
- Hugepages on the companion, and the perf media (`MEDIA_ASSET_SET=perf`), with
  `jpegxs` also the planar 1080p clip.
- With `dma`, a DSA on each node in use (§3.5). With `compliance`, `ebu_server` in
  `test_config.yaml`, and `phc2sys` and `tcpdump` on the companion (§4.6).

### 2.3. Inputs

| Input | pytest | Default | Meaning |
| --- | --- | --- | --- |
| `ref_a`, `ref_b` | `--perf_compare_slots`, `--perf_compare_labels` | `main` | A and B: a branch, tag or commit, with an optional `~n` or `^`. By hand, the two trees and their labels |
| `args_a`, `args_b` | `--perf_compare_args_a`, `--perf_compare_args_b` | empty | Extra RxTxApp options of each build, in letters, digits, spaces and `_./:=,+-` |
| `video` | `-k "1080p and 59fps"` | `1080p59` | 1080p, 2160p or 4320p, at 59, 50, 29 or 25 fps |
| `pixel_format` | `--perf_compare_pixel_format` | `none` | `none`, `yuv422p10le`, `v210` or `y210` (§2.5) |
| `codec` | `--perf_compare_codec` | `none` | `none` for st20p, `jpegxs` for st22p (§4.7) |
| `sessions` | `--perf_compare_sessions` | `36` | Video sessions of each build (§2.5) |
| `nics` | `--perf_compare_nics` | `single` | `single`, `split`, `redundant` or `whole` (§3.4) |
| `dma` | `--perf_compare_dma` | `none` | `none`, `A`, `B` or `both`: whose RX copies with a DSA (§3.5) |
| `single_core_rx`, `single_core_tx` | `--perf_compare_single_core_rx`, `--perf_compare_single_core_tx` | off | That role runs one scheduler, with the system tasklets (`--sch_session_quota 99`, no `--dedicated_sys_lcore`). A core carries about 28 x 1080p59 RX, 36 with DMA, or 35 TX |
| `test_time` | `test_time` of `test_config.yaml` | `120` | Seconds of the untraced window, at least 30. Each later window adds half |
| `rounds` | `--perf_compare_rounds` | `4` | Keep it even, as the slots swap every round. Under 4, the noise is rough |
| `compliance` | `--perf_compare_compliance` | off | EBU LIST judges every stream after the rounds (§4.6) |

### 2.4. Inputs that limit each other

The refs job refuses what cannot run before anything is built: a number that is not
one first, then every other input at once. The case refuses the rest before it starts
an instance.

| Inputs | Limit | Checked by |
| --- | --- | --- |
| `sessions`, `video`, `nics` | At most 100 Gb/s of sessions on a port; `whole` gives a build two (§2.5) | Refs job for st20p; the case against each link |
| `sessions`, single core | At most 60, the most an MTL scheduler takes | Refs job, case |
| `compliance`, `video` | No 4320p: LIST does not judge 8K | Refs job, case |
| `codec` `jpegxs` | At most 60 sessions, 1080p, no `dma`, `pixel_format` `none` or `yuv422p10le` (§4.7) | Refs job, case |
| `codec` `jpegxs`, refs | A `VERSION` of 26.09 or later | Refs job |
| `compliance` | `ebu_server` in `test_config.yaml`, which CI's `config-perf` writes only with `compliance` | Case |
| `args` | No `--tasklet_time`, `--dma_dev` or `--lcores`, by any prefix RxTxApp takes; with single core, no `--sch_session_quota`, `--sch_data_quota`, `--dedicated_sys_lcore` or `--rx_separate_lcore` | Case |
| `nics` other than `single` | Two ports of each of two NICs on two nodes, isolated alike | Case |

The refs job also refuses a ref that does not resolve, a number that is not a positive
integer, and a `test_time` under 30. A ref takes no option, `^{/regex}`, comma or
newline. The numbers are string inputs, because a number input reaches a step as
`120.0`.

### 2.5. Sessions and pixel formats

MTL sends 20 bits per pixel in packets of 1320 bytes of pixels, 1406 on the wire,
paced over 1125 of 1080 lines: 2.76 Gb/s for a 1080p59 session. A 100G port carries:

| Video | 59 fps | 50 fps | 29 fps | 25 fps |
| --- | --- | --- | --- | --- |
| 1080p | 36 | 43 | 72 | 86 |
| 2160p | 9 | 10 | 18 | 21 |
| 4320p | 2 | 2 | 4 | 5 |

`whole` fills port 0 first and puts the rest on port 1: 37 x 1080p59 run as 36 on
port 0 and one on port 1. With `jpegxs`, `whole` puts half on each port instead. A
JPEG XS session takes about 0.4 Gb/s.

With a `pixel_format`, MTL converts each frame between it and the format on the wire,
in `st20p_tx_put_frame()` and `st20p_rx_get_frame()`, on the application threads. The
TX still reads the perf clip, as frames of that format; nothing checks the pixels. The
conversion moves each frame through the slot node's memory several times, which runs
out first: 36 x 1080p59 in `yuv422p10le` hold their rate with `split` or `whole`, not
with `single`, where both builds share node 0.

## 3. Topology and isolation

Both hosts are measured. Each RX receives only its own instance's TX, so a change on
either side can show on the other, and `args_b` reaches both applications of B. Both
instances get the same RxTxApp configuration but
for interfaces and IPs, the same number of CPUs, and `--disable_migrate`; unless
single core, also `--dedicated_sys_lcore`.

With `nics` `single` on the perf pair:

```text
 companion (NIC 0's node 0): TX                           SUT (NIC 0's node 0): RX
┌─────────────────────────────────────────┐              ┌─────────────────────────────────────────┐
│ CPU 0: every instance's main lcore      │              │ CPU 0: every instance's main lcore      │
│ CPU 1: collectors, ARP keepalive        │              │ CPU 1: collectors                       │
│                                         │              │                                         │
│ slot 0  unit mtl-perf-tx0               │  port 0 VF   │ slot 0  unit mtl-perf-rx0               │
│         CPUs 4-17, L3 group mtl_perf_0  │ ──────────▶  │         CPUs 4-17, L3 group mtl_perf_0  │
│         A in round 1, B in round 2      │  subnet 0    │         A in round 1, B in round 2      │
│                                         │              │                                         │
│ slot 1  unit mtl-perf-tx1               │  port 1 VF   │ slot 1  unit mtl-perf-rx1               │
│         CPUs 18-31, L3 group mtl_perf_1 │ ──────────▶  │         CPUs 18-31, L3 group mtl_perf_1 │
│         B in round 1, A in round 2      │  subnet 1    │         B in round 1, A in round 2      │
└─────────────────────────────────────────┘              └─────────────────────────────────────────┘
```

Slot 0 uses a VF of port 0 (`host.vfs[0]`), slot 1 a VF of port 1 (`host.vfs_r[0]`)
on the subnet of the redundant `ip_pools`, so no stream of one slot reaches the other.
One instance, from the outside in:

```text
systemd-run --unit mtl-perf-rx0      AllowedCPUs=0,4-17 AllowedMemoryNodes=0 Nice=-20 (cgroup v2)
└─ unshare --mount --ipc --propagation private sh /tmp/mtl-perf-compare/A_rx0.sh
   ├─ mount --bind <slot tree> <mtl>/.local_install    its build, at the path DPDK expects (§3.2)
   ├─ mount -t tmpfs tmpfs /var/run/imtl               no MtlManager: a private lcore table
   ├─ echo $$ > /sys/fs/resctrl/mtl_perf_0/tasks       its L3 group (§3.3)
   └─ cd <mtl> && exec env LD_LIBRARY_PATH=<its libs> RxTxApp ... --lcores 5-17 ...
                                   schedulers on 5-17; application and EAL control threads on 4
mtl-perf-rx0-{log,trace,timed,st22p,sessions}   bpftrace -p <pid>, the probes of each window, on CPU 1
mtl-perf-rx0-pmu                                perf stat -p <pid>, the untraced window, on CPU 1
mtl-perf-arp (companion)                        an ARP probe for each RX port every 10 s, on CPU 1 (§3.6)
```

A cgroup cpuset bounds the instance. `taskset` would not, because `mtl_init()` calls
`numa_bind()`, which widens the CPU mask to the whole node.

### 3.1. CPU

`cpu_layout()` takes one hardware thread of each physical core of the NIC's node, so
no slot holds an SMT sibling of another's core. It leaves the node's first two cores to
the OS and splits the rest into equal slots: two with `single`, one on each NIC's node
otherwise. It takes the isolated cores if they give each slot 8 CPUs or are at least
as many as the others, else the others. A slot needs 3 CPUs: the application threads',
the system tasklets' and a scheduler's. With two NICs, both slots get the smaller CPU
count and must be isolated alike. On the perf pair (`isolcpus=4-31,68-95`), `single`
has slots 4-17 and 18-31 on node 0; the other modes have 4-31 on node 0 and 68-95 on
node 2.

The first CPU of a slot runs the application and EAL control threads. The others are
the `--lcores`, which the schedulers poll and the application threads may share. The
cpuset also holds CPU 0 and its node's memory:

- MTL passes `-l 0,<lcores>` to EAL, and RxTxApp has no main-lcore option, so CPU 0
  is every instance's main lcore.
- DPDK allocates on the main lcore's node for a thread that is no lcore and names no
  node, and its dmadev data only there; without node 0, a DSA on node 2 fails to probe. MTL still
  puts queues, buffers and frames on the port's node.
- Once an instance's sessions and schedulers run, each thread that may run on several
  CPUs gets one free CPU of the slot, in turn. The kernel never balances tasks across
  isolated CPUs, so they would stay where they started.

### 3.2. Software state

DPDK bakes `RTE_EAL_PMD_PATH` into `librte_eal` as an absolute path
([ci_runner_setup.md](ci_runner_setup.md#a-dpdk-artifact-remembers-the-path-it-was-built-at)).
So every slot is built at `<mtl>/.local_install`, and each instance binds its tree over
that path in its own mount namespace, with an `LD_LIBRARY_PATH` for its libraries. A
tmpfs over `/var/run/imtl` hides MtlManager, so MTL keeps its lcore table in SysV
memory private to the instance's IPC namespace. DPDK runs `--in-memory`. So a
`v26.01` build on DPDK 25.11 runs beside a `main` build on DPDK 26.07.

### 3.3. Cache and memory bandwidth

With `single`, both slots share one L3. If resctrl offers L3 allocation and has no
other groups, `mtl_perf_0` and `mtl_perf_1` each get a third of the ways for
themselves, at the end of the mask away from the `shareable_bits`. The
`shareable_bits` are the ways the NIC writes into (DDIO), and the default group keeps
them: with 12 ways and
`shareable_bits` `c00`, the slots get `00f` and `0f0`, the default group `f00`. The
other modes share no L3, so both groups keep every way and only monitor; Placement
then always reads CAT `no`.

Each instance joins its group before `exec`, and the group counts its memory traffic
(MBM) and LLC occupancy (CMT). MBA is not used, because its delay would change the
latency it protects. The case restores the default mask, saved in
`/run/mtl-perf-compare.schemata`, at its end; after an aborted job,
`task ci:perf-compare -- sweep` does.

### 3.4. Two NICs

| `nics` | Slot 0 | Slot 1 | Sessions of a build |
| --- | --- | --- | --- |
| `single` | Port 0 of NIC 0 | Port 1 of NIC 0 | On its port; both slots on node 0, with CAT |
| `split` | Port 0 of NIC 0 | Port 0 of NIC 1 | On its port, a socket for each build |
| `redundant` | Ports 0 and 1 of NIC 0 | Ports 0 and 1 of NIC 1 | Each on both ports, ST 2022-7; the port rate, `hw dropped pkts` and `RX work` rows hold both legs |
| `whole` | Ports 0 and 1 of NIC 0 | Ports 0 and 1 of NIC 1 | Port 0 first, or half on each with `jpegxs` (§2.5) |

All modes but `single` declare four ports (`PERF_PORTS=4`); the case makes a VF on
each port of NIC 1 and puts them on subnets 100 above NIC 0's. MTL charges a redundant
session on both ports, so with `redundant`, instances not on single core take the
quotas of the redundant performance cases: `--sch_session_quota` 36 for TX, 21 for RX
and 18 for RX with DMA. A `--sch_session_quota` in `args` overrides it.

At a core's limit, two instances on one socket take capacity from each other, so a
single core comparison at that load needs `split`, `redundant` or `whole`.

### 3.5. DMA

`dma` gives `--dma_dev` to the RX instances of the chosen builds; MTL has no TX DMA.
MTL gives a session only a DMA device on its port's node, and a vfio device serves
one process, so each slot takes its own DSA on its node: the i-th slot of a node the
i-th DSA by address, which stays with the slot when the builds swap. vfio-pci must
already allow DSA (`disable_denylist`), which the session fixture `host_dma_devices`
sets; lifting it in the case would reload the module and unbind the VFs. MTL
silently copies by CPU for a session without DMA, so such a round is invalid. DMA also
raises MTL's RX quota from 8 to 12 1080p sessions for each scheduler.

### 3.6. The network

The perf ports are in a VLAN of their own on a switch the fleet shares. An RX VF sends
nothing after its first ARP replies, so the switch forgets its MAC 300 to 600 s later
and floods its unicast over the VLAN, which loses packets. So the companion's
keepalive, unit `mtl-perf-arp`, sends an ARP probe for each RX port's IP every 10 s,
from its own PF of that port. A probe has the sender IP 0 (RFC 5227), so no host
caches it, and MTL's reply teaches the switch the MAC again. The round keeps
`arp.log`; a dead keepalive ends it as `exited during the measurement: mtl-perf-arp`.

In each window, every RX must have got, or dropped at its NIC, 99% of what its slot's
TX sent, by both instances' stats dumps; the PFs' MAC counters cannot tell, because a
flooded port counts the other streams too. The hosts start the later windows seconds
apart, so those leave out their first and last dump. A switch that ages no MAC there,
or a direct cable, makes the keepalive moot
([a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own)).

### 3.7. What still differs

| Residual effect | Handling |
| --- | --- |
| The two ports and core sets of a host differ | The slot swap |
| With two NICs, socket 0 also carries the OS, CPU 0 and the collectors | The slot swap |
| The two hosts differ | Never compared |
| The memory controller, the DDIO ways, the power budget | Common mode; an A/A run measures the rest |
| Another CI job on the companion | A note (§6) |
| Host tasks and IRQs on slot cores or their SMT siblings | Not prevented: they show in `sch preemptions (/s)`, longer loops and wider noise |

## 4. Measurement and the report

### 4.1. One round

```text
                           start steady  warm-up      untraced         traced        timed      sessions     stop
                           │      │      ├──────┤├─────────────────┤├───────────┤├───────────┤├───────────┤   │
RX A, B                    ┼══════════════════════════════════════════════════════════════════════════════════┼
TX A, B                      ┼══════════════════════════════════════════════════════════════════════════════┼
sys:log_msg                  ├────────────────────────────────────────────────────────────────────────────────┤  every dump
/proc, RDT, CLOCK_MONOTONIC                      ▲ read 1          ▲ read 2
perf stat                                        ├─────────────────┤
sys:tasklet_stat                                                    ├───────────┤  where the build has it
sys:tasklet_time_measure                                                         ├───────────┤
st22p stage collector                                                            ├───────────┤  st22p only
sys:sessions_time_measure                                                                     ├───────────┤
                                  ▲ the app threads leave the scheduler CPUs
```

The RX applications start first, because a unicast TX session waits for its RX's ARP
reply; the TX applications and the keepalive follow. Every session must then reach
99% of its frame rate within `120 + 3 × TX queues` s, a TX queue for each session and
port. If all sessions run but some stay under that, the round is overloaded, measured
and noted (§4.4); if one is missing, the round is invalid. A 30 s warm-up follows.

| Window | Length | Attached besides `sys:log_msg` | Gives | Dumps that count |
| --- | --- | --- | --- | --- |
| Untraced | `test_time` | Nothing in MTL; `perf stat` from outside | The primary rows | Those whose whole period lies between the `/proc` reads |
| Traced | `test_time / 2` and a dump period at each end | `sys:tasklet_stat`, if the build has it, which makes MTL time a sample of its loops | Tasklet and work rows | Those with tasklet records; the first is only the base |
| Timed | As traced | `sys:tasklet_time_measure`: every tasklet call timed; st22p's stage collector | Call times, st22p stages | Those with call times, but the first |
| Sessions | As traced | `sys:sessions_time_measure`: every video session timed | Session times | Those with session times, but the first |

Each window has its probes to itself: full timing would add to the sampled counters,
and session timing to the call times. Timing every call costs far more than sampling,
so those windows come last, change no primary row, and compare the builds rather than
production. The last probe detaches a dump period early, so each application prints
the dump that closes its window; then every unit gets SIGINT, each host on its own, so
an SSH error on one keeps the other's records. Each host places its dumps by its own
CLOCK_MONOTONIC, the clock of bpftrace and `/proc/timer_list`, so the hosts need no
common clock.

### 4.2. Collectors

The collectors attach by PID on CPU 1, once `Instance.start()` has seen the
application map libmtl. bpftrace resolves `usdt::sys:<probe>` through the process's
own libraries, so it reads the instance's tree ([usdt.md](usdt.md#21-sys-tracing)).
A build without `sys:tasklet_stat`, as every release up to v26.09, is not traced: its
tasklet rows read n/a, or are left out when neither build has them, and a note names
it.

`perf stat -x, -p <pid>` counts each process over the untraced window, sampling
nothing. The case takes the first of `/usr/bin/perf` and
`/usr/lib/linux-tools/*/perf`, newest first, that counts every event, because Ubuntu's
`/usr/bin/perf` refuses to run without the tools of the running kernel.

### 4.3. The report

`perf_compare.html` gives the builds and inputs, the rounds with their problems and
notes, the summary (§4.4), the compliance pass, and the rows of each role in groups.
Then come the test information, the Placement table (each slot's CPUs, whether they
are isolated, whether the host has CAT and perf, and its PFs and DSA) and, folded,
each instance's command and configuration. The case rewrites the report after every
round, so a job that times out keeps what it measured. The job summary, which renders
no style, gets `perf_compare_summary.html`: the report without the setup.

### 4.4. Reading the verdict

The verdict at the top of the summary reads:

```text
❌ <B> regresses <n> of the <N> key metrics and improves <m, or none>.
Worst: <role> · <row> <change> (<confidence>). Best: <role> · <row> <change> (<confidence>).
No loss or late frames; the worst session at RX <A> → <B>; TX <A> → <B> fps.
<k> valid rounds, the slots swapped each round. A change is at least 2% and 2 times the noise,
twice the standard error of a build's mean over its rounds.
```

The first line can also be `✅ <B> improves <m> of the <N> key metrics and regresses
none.` or `<B> changes none of the <N> key metrics.`, and with one valid round `One
valid round gives no noise estimate, so no change is scored.` With none, the summary
reads `No valid round: the builds are not compared.` Losses follow `⚠️ Loss
or late frames:`. With 2 or 3 valid rounds the verdict adds `Under 4 rounds the noise
is a rough estimate: a change of any confidence is a hint.`

A row changes from 2% and twice its noise
([the scoring rules](perf_compare_metrics.md#3-from-rounds-to-a-verdict)). Each row
has a better direction or none:

| Better | Rows |
| --- | --- |
| Higher (↑) | `fps (worst session)`, `fps (mean of sessions)` |
| None | `core clock`, `IPC`, LLC occupancy, hits and miss ratio, the port rates, the re-plans, the utilization, the CPUs of every kind of thread but the schedulers, and st22p's `encode to send` |
| Lower (↓) | Every other row: a cost |

A change is a regression (❌) or an improvement (✅) in a row with a direction, and
only differs (➖) in one without; no change reads `~`. The mark, the word and the bold
better value show the outcome without color. Confidence is high from 5 times the
noise, medium from 3, and low below.

The verdict counts the key rows that both builds have: each scheduler's `avg loop`,
`video cpu busy`, `scheduler threads per frame`, `memory bandwidth`, `LLC misses` and
`RX work (ns/pkt)`, and the loss and late rows once a build counts any. They are not
independent, so the counts summarize; they do not vote.

A role whose `fps (mean of sessions)` changed is overloaded. Its costs per second grow
with the frames it delivers, so the verdict scores its frame rates, its costs per
frame or packet (`scheduler threads per frame`, `RX work (ns/pkt)`) and its loss and
late rows instead, and says so. An RX at its overloaded TX's frame rate in both builds
only restates the TX, so its frame rates are left out.

The case passes or fails on the validity of its rounds, never on a score
([the validity gates](perf_compare_metrics.md#4-when-a-round-counts)). An overloaded
round is valid; a session under a tenth of its rate has collapsed, which invalidates
the round. A round that raises ends the rounds, and the report keeps the earlier ones.
With few rounds, an A/A run can show a change now and then.

### 4.5. Layout effects

The schedulers sit in one array in `struct mtl_main_impl`, so a size change of
`struct mtl_sch_impl` moves other schedulers' data, and one of
`struct mt_sch_tasklet_impl`, allocated one by one, moves the DPDK heap. Either can
move one scheduler's rows with no change to its code. When a candidate differs on
one scheduler only, first compare the reference with itself padded to the candidate's
struct sizes, over enough rounds: `main` reaches such a slow state on its own now and
then.

### 4.6. Compliance pass

With `compliance`, EBU LIST (`ebu_server`) judges every stream of each build after
the rounds. Only a round that raised stops the pass; an invalid round does not.

Each build runs alone, TX and RX in slot 0, its sessions multicast. A group holds as
many streams as a kernel capture takes without drops, 950,000 packets per second:
four 1080p59, one 2160p59, or 26 st22p 1080p59 streams. On the companion, the PF of
slot 1's first port, idle during the pass, joins each group in turn, and `tcpdump -p`
captures 1 s with the NIC's nanosecond timestamps and 128 bytes of each packet. Each
stream keeps its whole frames, from its second RTP marker to its last, for LIST. The
RX runs only so that IGMP snooping keeps the groups; only the TX must be steady. A
capture and its analysis take about 25 s: 9 captures for 36 x 1080p59, 18 for 72 or
for 36 with `redundant`.

phc2sys holds the capturing NIC's clock within 2 µs of the companion's CLOCK_TAI,
which the TX paces on, so the hosts need no common clock. A stream is compliant when
LIST finds it within ST 2110-21 type N, as MTL signals it, and every other check
passes. A group with a gap in a stream's RTP sequence, or a failed capture or
analysis, is not judged, and the report says why. The case fails when the clock left
the 2 µs during the captures, the TAI-UTC offset changed, or LIST judged no stream.
The verdicts are reported, not scored; `-` marks a value that LIST did not give.

LIST does not detect st22p streams, so for each st22p capture the case gives it an
SDP that names each stream `jxsv/90000`, `TP=2110TPN` (`jxsv_sdp()`), and waits up to
3 minutes for the new analysis. ST 2110-22 has no VRX or TRO, so LIST judges such a
stream by its Cinst alone.

Unlike the rounds, the pass runs one build alone, multicast, so that both builds are
judged on the same NIC and CPUs; with `single`, a lone build has more memory
bandwidth. It never captures on a sending port's PF, which would disturb that TX or
record the wrong timing.

### 4.7. JPEG XS

With `codec` `jpegxs`, every session is st22p (ST 2110-22): the SVT-JPEG-XS plugin
codes the planar 4:2:2 10-bit 1080p clip at RxTxApp's 3 bits per pixel, into RFC 9134
packets that MTL paces in software (TSC). Each slot builds the plugin of the workflow
commit's `versions.env` against its own libmtl (§5.2), so both builds run one codec.

- At most 60 sessions: MTL puts all st22 sessions of a TX instance on one scheduler,
  because their quota counts bytes, not bits.
- 1080p only: no other planar clip fits the hugepages of each TX session.
- `pixel_format` `none` or `yuv422p10le`, the same here: st22p converts nothing.
- No `dma`: MTL's st22 RX has none.
- MTL 26.09 or later, by each ref's `VERSION`: the SVT plugin's meson asks for it,
  although 26.01 has the same st22p API.
- The plugin codes 8 sessions of a direction per registration, and MTL takes up to 8
  registrations by distinct paths, so a slot's `conf/kahawai.json` names the plugin
  and 7 links to it. The `fail to create one session at 8` lines of a healthy log are
  MTL passing over a full registration.
- SVT gives its threads SCHED_FIFO 99 as root, which would starve the application
  threads; an st22p instance runs with `RestrictRealtime=yes`, and SVT only warns.
- Each log must name a session on `st22_svt_jpeg_xs_plugin*` for each session: MTL
  gives a session the first plugin that takes its formats, and the sample plugin also
  claims JPEG XS.
- An st22p RX moves its application threads (§3.1) only once every session got frames,
  because its decoder starts its threads at its first frame.
- With `single`, both TX instances share the companion's node 0 hugepages; a TX that
  runs out reads its source through `mmap()`, which invalidates the round.

An st22 RX session's own dump line reads 0 fps, so its frame rate is that of the
frames its pipeline gave the application. `encode fail`, `decode fail` and `busy drop
frame` are loss rows. The codec's threads keep RxTxApp's name and, with MTL's unnamed
CNI thread, make the Codec row of CPU by stage. The timed window adds the
[st22p stage rows](perf_compare_metrics.md#23-st22p-stages-timed).

## 5. Jobs and nightly

### 5.1. Jobs

1. `refs`, on a hosted runner, checks out without persisting credentials, installs
   Task and runs `task ci:perf-compare -- refs`, which resolves both refs and refuses
   the inputs that cannot run (§2.4).
2. `build`, after `refs`, is `performance.yml`'s build job: `checkout-clean`, then
   `.github/actions/build-artifacts`, which restores or builds the workflow commit's
   cached install trees and uploads them as `mtl-build`. The framework runs on it.
3. `compare` runs on `[self-hosted, perf, sut]` for up to 180 minutes; the pair has
   one runner of that label, so runs queue for it. It checks out the workflow commit
   without persisting credentials and restores the build through `validate-host`,
   which installs Task.
   Then:
   1. `task ci:perf-compare -- build` builds A and B (§5.2), and `-- sync` copies them
      to the companion and exports `PERF_COMPARE_SLOTS`;
   2. `task ci:pytest-setup` prepares the virtualenv and configuration, and
      `task ci:media-assets -- verify` checks the media;
   3. `task ci:pytest-run -- perf-compare` runs the case;
   4. always, and first, as a cancelled job leaves these steps 5 minutes,
      `task ci:perf-compare -- clean` removes the units, RDT groups and slot trees on
      both hosts;
   5. if the case ran, `task ci:report -- perf-compare` writes the job summary; then
      the report and logs are uploaded, and `task cleanup` runs.

### 5.2. The builds of A and B

The compare job builds both on the SUT, with one compiler and set of flags, so a
difference is the code's. A slot comes from `git archive <sha>`, built with the
workflow commit's `build_dpdk.sh` but the ref's own `versions.env` and DPDK patches, at
the workspace's `.local_install`, the path DPDK bakes in. It then moves to
`<workspace>/../perf-compare/a` or `b` on both hosts; equal SHAs build once. With
`jpegxs`, a slot also holds the plugin, from the workflow commit's `build_jpegxs.sh`,
and `conf/kahawai.json`. Meanwhile the workflow commit's DPDK and MTL, and with
`jpegxs` its JPEG XS and registry, step aside; its `ffmpeg` stays, as the fleet's
watchdog deletes `/usr/local/bin/ffmpeg` when it does not lead into the workspace's
`.local_install`, during a job too.
The ICE driver is the workflow commit's, because a kernel module cannot differ by
process.

The workflow ref ("Use workflow from") gives the tests and CI scripts, so it must have
this workflow. `ref_a` and `ref_b` can be any ref with `tests/tools/RxTxApp`, a
`versions.env` and the DPDK patches it names, such as `v26.01`, `v26.09` and `main`.

### 5.3. Nightly

The schedule runs at 16:00 UTC. It compares the `main` of the run (`GITHUB_SHA`), as
B, with the B of the last passed nightly, as A, so every commit falls in one night
however late GitHub starts it:

```text
main   ──●───●──●────────●───●──────●───────●──▶
night 1  ├──── A₁ … B₁ ──┤
night 2                  ├── A₂ = B₁ … B₂ ──┤
```

A failed night widens the next. A falls back to the `main` of 24 hours ago when no
night passed yet, the Actions API read fails, or the last night's B is not an
ancestor of this one. When `main` did not move, the night is an A/A run.

## 6. Troubleshooting

The Rounds table gives each round's problems, which fail the case, and its notes,
which do not. A compliance problem starts with `compliance of <build>:`. `<who>` is an
instance's unit and build, such as `mtl-perf-rx0 (main 1a2b3c4d5)`.

| Message, or (note) | Cause | What to do |
| --- | --- | --- |
| `perf-compare refused the inputs:` (refs job) | Inputs that cannot run (§2.4), a ref that does not resolve, or a bad number | Fix what it lists |
| `N x <video> take more than a 100G port carries ...` (refs job) | More st20p sessions than a port carries (§2.5) | Fewer `sessions`, or `nics` `whole` |
| `This host or the companion has no bpftrace, or the companion is unreachable` (sync) | No bpftrace, or no SSH to the companion | Install bpftrace; check `runner.env` |
| `one perf-compare case per run: select it with -k` | `-k` selected more than one case | Select one video |
| `--perf_compare_compliance needs ebu_server in test_config` | No LIST account | `ebu_server` in `test_config.yaml`; in CI, `runner.env` ([EBU LIST](ci_runner_setup.md#the-ebu-list-compliance-analyser)) |
| `--tasklet_time`, `--dma_dev` or `--lcores would ...`, `--<option> would give a single core instance more schedulers` | An option in `args` that the case sets | Remove it |
| `--perf_compare_codec jpegxs takes ...`, `... runs 1080p` | An input that JPEG XS cannot run with (§4.7) | Fix what it names |
| `<host> needs a VF on each of two PFs` | No VF on one of NIC 0's ports | The PCI devices of the topology |
| `<host>: N x <video> on a port take N Gb/s, and <PF> links at N Gb/s`, or `<PF> has no link` | A port's sessions exceed its link, or it has none | Fewer `sessions`, or fix the link |
| `<host>: --perf_compare_nics <mode> takes two ports of each of two NICs ...`, `both NICs are on NUMA node N` | Not two ports of each of two cards on two nodes | The second card on both hosts, cabled port to port |
| `<host>: <mode> takes the cores of both NICs' nodes isolated alike ...` | One node's slot cores are isolated and the other's are not (§3.1) | Isolate as many cores on both nodes, or use `single` |
| `NUMA node N has N isolated free cores and N others, not N of either` | Fewer than 3 cores for each slot on a NIC's node (§3.1) | A mode with one slot on each node, or more cores |
| `vfio-pci still refuses DSA ...`, `NUMA node N has N DSA, its slots need N`, `DSA <BDF> is not on vfio-pci` | The denylist, too few DSA on a node, or a failed bind | [dma.md](dma.md); DSA on in the BIOS of every node in use |
| `not steady after N s: <unit> (...)` | An instance did not run all its sessions | Its application output: the sessions it created |
| `exited before steady state: <units>` | An application or a collector stopped | The application output, `<label>_<role><slot>`, and its `.log` in `perf_compare/round<N>/` |
| The same, with `st22p_tx_create fail` in an st22p output | The registry names the plugin too few times, 8 sessions each | Slots built with `CODEC=jpegxs` (§2.1), or at most 8 sessions |
| `exited during the measurement: <units>` | An application, a collector or the keepalive `mtl-perf-arp` stopped | As above, and `arp.log`. The output of a timed or sessions collector stays in `/tmp/mtl-perf-compare/` until the next round |
| `<who>: N untraced, N traced, N timed and N session timed dumps` | A window held fewer than two of its dumps (§4.1) | A `test_time` of 30 or more; the logs |
| `<who>: got N% in the <window> window of what its TX sent ...` | The RX got under 99% of it (§3.6) | `arp.log`; [a link of its own](ci_runner_setup.md#the-perf-pair-needs-a-link-of-its-own) |
| `<who>: N of M sessions` | A session did not report in the untraced window | The application output |
| `<who>: a session at N fps, collapsed or no flow` | No flow, or two instances on a socket at a core's limit (§3.4) | The output and the wire; `split` for single core |
| `<who>: N schedulers, single core runs one` | A single core instance ran more schedulers | Its `sch_start` lines |
| `<who>: N of M sessions copied by DMA` | A session got no DMA device, or copied nothing | Its `rv_init_dma` and `by dma copy` lines |
| `<who>: its source is not in hugepages` | A TX read its source through `mmap()` | More hugepages on the companion, or fewer sessions |
| `<who>: N of M sessions on st22_svt_jpeg_xs_plugin*` | Another plugin coded sessions, or `args` log above info or to a file | Its `st_plugin_register` and `get one session` lines, and `conf/kahawai.json` |
| `<who>: no rvs_pkt_rx work in sys:tasklet_stat` | A traced RX gave no work counters | Its `.trace` record |
| `failed: <error>` | The round raised; the rounds stop | The pytest log |
| `under the rate after N s: ...`, `<who>: a session fell to N fps` (notes) | Overload: the slot's CPUs, two instances on a socket, or the companion's hugepages | The fps rows score it; fewer `sessions` to compare at the rate |
| `<who>: N incomplete frames`, and the other loss and late counts (notes) | Loss in the receiver, or late TX frames | Their rows count them |
| `<who>: app threads not moved, <why>` (note) | The threads could not leave the scheduler CPUs | Its schedulers and their CPUs |
| `a CI job ran on <host> during the round` (note) | Another job ran on the companion | Rerun if the round stands out |
| `the builds pace TX differently: ...` (note) | A TX fell back from the rate limiter (`pacing way:`) | Its re-plan row is that, not a gain |
| `<who>: perf stat did not count every event` (note) | No PMU rows for that instance | Its `.pmu` record |
| `<who>: no st22p stage times` (note) | The stage collector was cut short | Its `.st22p` record |
| `... LIST judged no stream`, and the reason of each capture | No capture was whole, or LIST reported none of its streams | Each `<group>.json` in `perf_compare/compliance_<A or B>/` |
| `... the upload failed: <error>` | `Connection refused`: LIST's proxy is down; 404: another server on port 80; 401: the account | `task ci:ebu-list -- verify` on the companion |
| `... exited during the captures: <units>` | An application or phc2sys stopped | The pass's logs and `phc2sys.log` |
| `... phc2sys held <PF> within 2000 ns of CLOCK_TAI in no three samples ...`, `... not locked within 2000 ns ... during the captures ...` | The capturing NIC's clock did not follow CLOCK_TAI | `phc2sys.log`; another phc2sys or ptp4l on that NIC |
| `... the TAI-UTC offset of <host> changed` | The kernel's offset changed during the pass | [TAI and capture clock requirements](ci_runner_setup.md#tai-and-capture-clock-requirements) |
| Not judged, in the Compliance table | A gap in the RTP sequence, no whole frame, or a failed capture or analysis | The row's reason and `<group>.json` |
| Placement `perf` no | No perf on the host counts every event | A `linux-tools-<version>` package that counts there |
| Placement `CAT` no | With `single`: no L3 allocation, or other resctrl groups. Always no with the other modes (§3.3) | With `single`, remove the other groups |
| A change in an A/A run | The rough noise of a few rounds | More rounds |
| `No perf-compare report found` (Summarize) | The case failed or skipped before a round | The pytest log |
| `mtl-perf-*` units or `mtl_perf_*` groups left over | A job ended before its cleanup | `task ci:perf-compare -- sweep` on each host |
| EAL: "directory path /tmp is world-writable and insecure" | The checkout is below a world-writable directory | Move it out of `/tmp` |
