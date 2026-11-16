# perf-compare metrics

This page describes every row of a perf-compare report (`perf_compare.html` and the
job summary): its source, its calculation, its unit and its better direction.
[perf_compare.md](perf_compare.md) describes the benchmark itself. The code is
`tests/acceptance/mtl_engine/perf_compare.py`.

Every number starts as a counter that MTL, the kernel or the CPU keeps anyway. The
case reads it in a window where nothing else measures, and turns it into one value for
each instance and round.

## 1. Data sources

| Source | What it gives | How it is read | Resolution |
| --- | --- | --- | --- |
| MTL stats dump | A block of lines from each instance every 10 s (`MT_STAT_INTERVAL_S_DEFAULT`, `mt_stat.c`), from `M T    D E V   S T A T E` to `E N D    S T A T E` | The USDT probe `sys:log_msg`, which fires for every log line at any log level while attached, so every release is read alike | Loop times 1 ns, call times 0.01 us |
| Tasklet counters | For each scheduler and tasklet: calls, their ns, the calls that did work and their ns, cumulative | The USDT probe `sys:tasklet_stat`, where the build has it, counted within each dump | 1 ns |
| `/proc` | For each thread: CPU ticks and its last CPU (`stat`), time on a CPU (`schedstat`), involuntary switches (`status`); the host's CLOCK_MONOTONIC (`/proc/timer_list`) | At both ends of the untraced window | Ticks of `CLK_TCK`; `schedstat` 1 ns |
| RDT (resctrl) | `mbm_total_bytes` and `llc_occupancy` of the instance's group, summed over its L3 domains | With `/proc` | Bytes |
| `perf stat` | `task-clock`, `cycles`, `instructions`, `cache-references` and `cache-misses` of the process | `perf stat -x, -p <pid>`, over the untraced window | Exact counts |

The `/proc` reads and the bpftrace timestamps share the host's CLOCK_MONOTONIC, so the
case can tell whether a dump's whole period lay inside a window.

## 2. Rows

The groups are those of the report, in its order. Δ is a later reading minus an
earlier one, and Δt the seconds between the two `/proc` reads. The median of dumps is
the median over a window's dumps, which one bad dump cannot move. ↓ means lower is
better (a cost), ↑ higher is better, and ⊘ no better direction: such a row is marked
➖ when it differs, but never regresses or improves. Key marks a row of the verdict
([perf_compare.md §4.4](perf_compare.md#44-reading-the-verdict)). A row comes from the
untraced window unless its group says otherwise.

### 2.1. Delivery

| Row | Source | Calculation | Unit | Better |
| --- | --- | --- | --- | --- |
| `fps (worst session)` | `RX/TX_VIDEO_SESSION(m,s:name): fps X`; for an st22p RX session, `RX_ST22P(name), frame get try N succ M` | Each session's median of dumps, a dump without it counting 0; then the lowest. An st22p RX session's fps is M over the time since its last such line | frames/s | ↑ |
| `fps (mean of sessions)` | The same | The mean of the sessions' medians | frames/s | ↑ |
| `RX rate (Gb/s)`, `TX rate (Gb/s)` | `DEV(N): Avr rate, tx: X Mb/s, rx: Y Mb/s` | For each dump the sum over the instance's ports, then the median of dumps | Gb/s | ⊘ |
| `RX rate, port N (Gb/s)`, `TX rate, port N (Gb/s)` | The same | For an instance on two ports, the median of each; with `redundant`, port 0 is the primary leg | Gb/s | ⊘ |
| `incomplete frames` (RX) | `RX_VIDEO_SESSION(m,s): incomplete frames N` | The sum over the dumps | frames | ↓ |
| `hw dropped pkts` (RX) | `DEV(N): Status: rx_hw_dropped_packets N` | The sum | packets | ↓ |
| `build timeout frames`, `epoch drop`, `transmitter mismatch troffset` (TX) | `TX_VIDEO_SESSION(m,s): <row> N` | The sum | frames; frame periods for `epoch drop` | ↓ |
| `transmitter recalculate warmup (/s)` (TX) | The same | The sum ÷ (dumps × 10 s) | re-plans/s | ⊘ |
| `encode fail` (TX), `decode fail`, `busy drop frame` (RX) | `TX_ST22P(name), encode fail N`, `RX_ST22P(name), decode fail N`, `RX_ST22P(name), busy drop frame N` | st22p only: the sum | frames | ↓ |

How MTL counts them:

- fps is a session's frames since its last dump line over the measured time since
  then (`rv_stat()`, `tv_stat_collect()`). RX counts only complete frames. TX counts a
  frame once its builder built the last packet, so lateness shows in the late rows.
- A port rate is the port's bytes since the last dump × 8 over the dump period of 10 s
  (`dev_inf_stat()`): video, pads, ARP and PTP alike.
- `incomplete frames` were handed on with fewer bytes than a frame
  (`rv_frame_notify()`). `hw dropped pkts` is the NIC's `imissed`: packets dropped for
  want of a free RX descriptor, when the host polled too late.
- `build timeout frames` were built after their planned time (`tv_tasklet_frame()`).
  `epoch drop` counts frame periods that the builder found past and skipped.
  `transmitter mismatch troffset` counts frames whose first packet reached the rate
  limiter too late or too early to warm up for. MTL prints each only when it moved.
- A re-plan is the rate limiter re-planning its pads before a frame, after the clock
  outran its plan. Up to `v26.09` MTL counts only the warm-up re-plan, `main` more.

The loss and late rows join the verdict once a build counts any; a count from 0 reads
`from 0` (§3).

### 2.2. CPU by stage

One table, with a row for each kind of thread:

| Row | Calculation | Unit | Better |
| --- | --- | --- | --- |
| `<kind> threads (CPUs)` | The CPUs that the kind's threads last ran on at the second `/proc` read, of threads at both reads that ran between them (their `schedstat` grew). `all` counts a shared CPU once | CPUs | ↓ for `scheduler`, else ⊘ |
| `<kind> threads utilization (%)` | 100 × Δ(utime + stime) ÷ `CLK_TCK` ÷ Δt ÷ the kind's CPUs; n/a if none of its threads ran | % | ⊘ |
| `<kind> threads per frame (ms)` | Δ(utime + stime) ÷ `CLK_TCK` ÷ Δt × 1000 ÷ the frames per second of all sessions, the sum of their medians | ms of CPU | ↓; key for `scheduler` |

| Kind | Threads, by `comm` |
| --- | --- |
| `scheduler` | Those that the `t_pid` of the `SCH` lines names, each polling one of the slot's `--lcores` |
| `application` | RxTxApp's frame thread of each session: `tx_st20p_N`, `rx_st20p_N`, `tx_st22p_N` or `rx_st22p_N`. They fill or read the frames, and MTL converts a frame of another pixel format on them |
| `codec` | st22p only: those named `RxTxApp`, the codec plugin's, which set no name, and MTL's CNI thread (`mt_cni.c`), which polls a few control packets. With st20p the CNI thread is `other` |
| `other` | The rest: MTL's control threads such as `mtl_stat` and `mtl_admin`, EAL's threads, RxTxApp's main thread |
| `all` | Every thread |

The CPUs are whole, because the case pins each thread to one CPU before the warm-up
([perf_compare.md §3.1](perf_compare.md#31-cpu)). `schedstat` counts a run of 0.2 ms
that the ticks miss, and EAL's thread of an unused lcore sleeps and holds no CPU. The
other kinds share the CPUs the schedulers leave, so their counts are not scored. The
utilization reads about 100% for a scheduler that polls all the time; the CPU per
frame scores the same time. Application threads wait without a frame, so their CPU per
frame is the cost of the frame path, which a faster converter lowers.

### 2.3. st22p stages, timed

The rows `st22p <stage> avg | p50 | p99 | max, timed (ms)` of an st22p instance come
from a bpftrace collector on the pipeline's `st22p` probes (`mt_usdt_provider.d`,
unchanged since `v26.01`), which runs during the timed window.

| Stage | From | To | Better |
| --- | --- | --- | --- |
| TX `encode` | `tx_encode_get`: the encoder takes the frame | `tx_encode_put`: the codec's put call after encoding | ↓ |
| TX `encode to send` | `tx_encode_get` | `tx_frame_next`: the transport takes the frame, with its slack before its slot | ⊘ |
| RX `decode wait` | `rx_frame_available`: received whole | `rx_decode_get`: the decoder takes it | ↓ |
| RX `decode` | `rx_decode_get` | `rx_decode_put`: the codec's put call after decoding | ↓ |
| RX `receive to app` | `rx_frame_available` | `rx_frame_get`: the application gets it | ↓ |

- Each frame is timed by its session and frame buffer, the first two arguments of each
  probe, from a stage's first probe to its second.
- A stage starts at a probe that fires before the frame's state lets the thread of its
  end take the frame, so its end reads the same pass. `tx_frame_put`, `tx_encode_put`
  and `rx_decode_put` fire only after that state, so they start no stage, and TX has
  no stage for the wait before its encoder. The latter two still end `encode` and
  `decode`, which so hold the put thread's delay too: under load, a frame's `encode`
  can exceed its `encode to send`. A stage's end deletes its start's time, because
  bpftrace detaches its probes one by one as it stops.
- bpftrace prints each stage's count in 0.1 ms buckets, and its exact count, sum and
  maximum, as it stops. It keeps only 4,096 keys in a map by default and silently drops
  the rest, so the collector runs with `BPFTRACE_MAP_KEYS_MAX=65536` and also counts
  each stage exactly (`@st22p_n`).
- avg and max are always exact. p50 and p99 appear only when the buckets hold all of a
  stage's frames. They are read linearly within their bucket, the last bucket ending
  at the maximum, as Prometheus' `histogram_quantile()` does, and their noise is at
  least one bucket (§3). A stage without its count, sum or maximum has no rows, and an
  instance with no stage notes `no st22p stage times`.
- Two of the probes run on the schedulers: `rx_frame_available` in the RX packet path
  and `tx_frame_next` in the TX builder. They fire once per frame, in the timed window
  that already carries the cost of timing every call, in both builds alike. The
  summary shows `encode` and `decode` outside the verdict.

### 2.4. Scheduler loops

| Row | Source | Calculation | Unit | Better |
| --- | --- | --- | --- | --- |
| `sch_N avg loop (ns)` | `SCH(N:name): tasklets K, lcore L(t_pid: T), avg loop X ns` | The median of dumps, for each scheduler | ns | ↓, key |

Each time more than 2 s have passed, the scheduler sets `avg loop` to the time since
then over its loops (`sch_tasklet_func()`). A loop is a pass over all its tasklets,
with the sleep of an idle scheduler. More work per packet lengthens it at the same
load. `t_pid` is the scheduler thread's TID, by which the CPU rows find it.

### 2.5. CPU

| Row | Source | Calculation | Unit | Better |
| --- | --- | --- | --- | --- |
| `video cpu busy (%)` | `RX/TX_VIDEO_SESSION(m,s): throughput ..., cpu busy Y` | For each dump the mean of the sessions, then the median of dumps | score, % | ↓, key |
| `sch preemptions (/s)` | `nonvoluntary_ctxt_switches` of each scheduler thread | Δ ÷ Δt | /s | ↓ |
| `IPC` | `perf stat`: `instructions`, `cycles` | instructions ÷ cycles | ratio | ⊘ |
| `core clock (GHz)` | `perf stat`: `cycles`, `task-clock` | cycles ÷ task-clock | GHz | ⊘ |

`video cpu busy` is MTL's load score, not a CPU time. MTL's admin thread computes it
every 6 s from the loop: the loop ÷ 3 packet intervals × 100 for RX, the loop ÷ (bulk
× packet interval) × 100 for TX (`rx_video_session_cal_cpu_busy()`,
`tx_video_session_cal_cpu_busy()`). TX forces it to 100 after more than 10 build
timeouts in an admin period. It follows the loop rows. MTL migrates sessions by it
above 95, but the case runs `--disable_migrate`, so migration is off here.

A scheduler that keeps its CPU is never switched out involuntarily, so the preemptions
measure isolation. IPC and the clock only explain the other rows: idle polls raise IPC
as much as speed does, and a different clock moves every time row.

### 2.6. Memory and cache

| Row | Source | Calculation | Unit | Better |
| --- | --- | --- | --- | --- |
| `memory bandwidth (MB/s)` | `mbm_total_bytes` | Δ bytes ÷ Δt ÷ 10⁶ | MB/s | ↓, key |
| `LLC occupancy (MB)` | `llc_occupancy` | At the end of the window ÷ 10⁶ | MB | ⊘ |
| `LLC hits (M/s)` | `cache-references`, `cache-misses` | (references − misses) ÷ 10⁶ ÷ Δt | M/s | ⊘ |
| `LLC misses (M/s)` | `cache-misses` | misses ÷ 10⁶ ÷ Δt | M/s | ↓, key |
| `LLC miss ratio (%)` | Both | 100 × misses ÷ references | % | ⊘ |

The instance joins its resctrl group before it starts, so the counters hold only its
threads. perf's rates divide by Δt, not by its `duration_time`, which sums the threads
of a `-p` process; perf runs about 1 s longer, in both builds. A packet that the NIC
wrote into the L3 (DDIO) and that is read in time is a hit, and a miss goes to memory,
so only misses are scored. An event that perf could not count gives no PMU rows, and a
note.

### 2.7. Tasklets, 1 loop in 64 timed

The traced window attaches `sys:tasklet_stat` where the build has it
([usdt.md](usdt.md#21-sys-tracing) describes the `sys` probes). Attached, it makes MTL
time 1 loop in 64 on average, at random gaps of 1 to 127 loops, and count each timed
loop as many times as its gap. No release up to `v26.09` has it, nor `main` before it
was added; such a build is not traced, and a note names it.

| Row | Calculation | Unit | Better |
| --- | --- | --- | --- |
| `sch_N avg loop, traced (ns)` | The median of `avg loop` over the traced dumps after the first | ns | ↓ |
| `<tasklet> avg (ns/call)` | Δ ns ÷ Δ calls between the first and last traced dump, summed over the tasklet's schedulers | ns/call | ↓ |
| `rvs_pkt_rx work (ns/call)` | Δ work ns ÷ Δ work calls | ns/call | ↓ |
| `rvs_pkt_rx work (% of a core)` | 100 × Δ work ns ÷ the ns between the two dumps, summed over the schedulers, so it can pass 100 | % | ↓ |
| `RX work (ns/pkt)` | Δ work ns of `rvs_pkt_rx` ÷ the packets the ports received in the same dumps (`rx` of the `DEV` lines) | ns/pkt | ↓, key |

Work is a call that did not return `MTL_TASKLET_ALL_DONE`. A polling tasklet is nearly
always idle, so its average call hardly moves with the load, while its work does. Only
`rvs_pkt_rx` tells work by its return value: pending when it received packets. A paced
`tx_video_sessions_mgr` builds in calls that return all done while its ring is full,
and `video_transmitter` returns pending while a packet waits for the pacer, so TX has
no work rows. `avg loop, traced` against `avg loop` is the cost of the sampling.

### 2.8. Tasklet call times, every call timed

The rows `sch_N <tasklet> avg | p90 | p95 | p99 | min | max, timed (us)` come from
`SCH(N,k): tasklet <name>, avg Xus max Yus min Zus`. MTL reads the TSC around each
tasklet call and keeps the min, max, sum and count, which it resets after each dump.

| Stat | For each dump (MTL) | Over the window |
| --- | --- | --- |
| avg | Sum ÷ count since the previous dump | The median of dumps |
| min, max | The shortest and longest call since the previous dump | The least min and the largest max |
| p90, p95, p99 | Read from `, p90 Pus p95 Qus p99 Rus` after the min, if a build prints it | The median of dumps |

No release up to `v26.09` prints the percentiles, nor `main` before they were added:
a build without them has no such columns, and a note names it. The summary shows each tasklet's avg
and p99 outside the verdict: the window runs slower than production, and a tasklet on
several schedulers would count several times.

### 2.9. Video session call times, every session timed

The rows `sch_N RX|TX video session avg | min | max, session timed (us)` come from
`RX/TX_VIDEO_SESSION(m,s): tasklet time avg Xus max Yus min Zus`. MTL times the part of
a tasklet call that serves one session: for RX, in `rvs_pkt_rx_tasklet_handler()`, the
RX burst on each port, the packet handling and copy, and the DMA submit; for TX, in
`tvs_tasklet_handler()`, the builder of its packets. The transmitter has no session
time. For each scheduler, over the sessions of the manager with its index and over the
dumps, the rows give the median of avg, the least min and the largest max.

## 3. From rounds to a verdict

| Step | Rule |
| --- | --- |
| Build value | The mean of the build's valid rounds that have the row |
| Change | Δ = 100 × (B̄ − Ā) ÷ Ā |
| Noise | Twice the standard error of a build's mean, 100 × √(4 s² ÷ n), in % of Ā (of B̄ when Ā is 0), the scale of the change; the larger of A's and B's. For two rounds it is their difference, and more rounds narrow it. As both are in % of Ā, the ratio of a change to its noise does not depend on which build is A |
| Print-step floor | The noise is at least one step of what MTL prints, in % of Ā as well: 1 ns for a loop, 0.01 us for a call time, and the 0.1 ms bucket for an st22p p50 or p99 |
| A change | \|Δ\| ≥ 2% of Ā and \|Δ\| ≥ 2 × noise. The factor keeps the rough noise of a few rounds from making a change |
| Outcome | A change is a regression (❌) or an improvement (✅) in a row with a better direction, and differs (➖) in one without. Otherwise the row reads `~` and its difference |
| Confidence | \|Δ\| ÷ noise: high from 5, medium from 3, low below |
| From 0 | Ā at 0 and B̄ above it reads `from 0`, and scores as the mirror of a fall to 0, 100% |
| One round | No noise: a row 2% or more apart reads n/a, a closer one unchanged |
| A missing row | n/a, and the summary leaves it out |
| Rounds column | 100 × (Bᵣ − Aᵣ) ÷ Aᵣ for each round, such as `+10% / +8.5%`. A and B ran at once, so each round's difference is paired, and the swap alternates the sign of a slot effect |

## 4. When a round counts

A round enters the comparison only when every instance passes every gate. A round
that fails one shows its problem in the Rounds table, is left out, and fails the case.
The gates, in the order the case checks them:

| Gate | Rule |
| --- | --- |
| Steady | Every session of every instance in one dump at 99% of its frame rate within `120 + 3 × TX queues` s. If all run but some are slower, the round is noted as overloaded |
| Alive | No application, collector or ARP keepalive exited |
| Whole windows | Two dumps or more in each window, as [perf_compare.md §4.1](perf_compare.md#41-one-round) counts them: in the traced one, only with `sys:tasklet_stat`, the base and a whole one |
| Delivered | In every window, each RX got, or dropped at its NIC, 99% of what its slot's TX sent ([perf_compare.md §3.6](perf_compare.md#36-the-network)) |
| Every session | Every session in the untraced window |
| Not collapsed | The worst session at a tenth of its frame rate or more |
| One scheduler | With single core, one scheduler in every instance of that role |
| DMA engaged | With `dma`, every session of a DMA instance copied by DMA |
| Source in hugepages | No TX read its source through `mmap()` |
| Codec under test | With `jpegxs`, a session on `st22_svt_jpeg_xs_plugin*` in each log for each session |
| Work counters | `rvs_pkt_rx` work in a traced RX instance |

Before the rounds, the case also refuses a port whose link cannot carry its sessions.
Notes, such as an overload, a loss count or another CI job on the companion, put a
fact beside a round and do not invalidate it
([troubleshooting](perf_compare.md#6-troubleshooting)).

## 5. Known limits

They hit A and B alike, but limit what a row means on its own.

| Limit | What it means |
| --- | --- |
| On SNC2 hosts such as the perf pair, kernel 6.8's resctrl does not scale MBM and occupancy by the sub-NUMA clusters of an L3 (6.11 does), so both read twice what the instance moves and holds | A ratio of two builds holds; an A/A run cannot show a factor both share |
| Per-scheduler rows pair up only while both builds place their tasklets alike | A build with more schedulers has rows the other lacks (n/a) |
| A size change of a scheduler struct can move one scheduler's rows with no code change | See [layout effects](perf_compare.md#45-layout-effects) |
| A session whose stats lock stays busy past the stats thread's wait prints nothing in that dump. RX waits 10 us; TX 10 us up to `v26.09` and 1 ms in `main` | Its next counts cover two periods. Sums hold, and the median of fps absorbs the dump where it reads 0 |
| `RX work (ns/pkt)` divides by every packet the ports received, video or not | The few other packets lower it a little |
| At 36 x 1080p59, two `main` TX instances on the companion count build timeouts every round, while every session holds its rate | They follow the load of both TX instances, not a counter bug: an A/A run of `main` notes them every round, and fewer sessions give a clean baseline |

## 6. Raw files of a round

Each round keeps its raw material in `logs/performance/<time>/perf_compare/round<N>/`,
where `<label>_<role><slot>` names an instance, such as `A_rx0`:

| File | Holds |
| --- | --- |
| `<label>_<role><slot>` | The application's output |
| `<label>_<role><slot>.log` | The `sys:log_msg` records: `<ns> L <log line>` |
| `<label>_<role><slot>.trace` | The `sys:tasklet_stat` records, `<ns> T <sch> <tasklet> <calls> <ns> <work calls> <work ns>`; empty without the probe |
| `<label>_<role><slot>.pmu` | The output of `perf stat -x,` |
| `<label>_<role><slot>.st22p` | st22p only: the stage collector's maps |
| `arp.log` | The ARP keepalive's output: each interface and frame, and any send error |

Only the report keeps the `/proc` and RDT values. A compliance pass keeps
`perf_compare/compliance_A/` and `compliance_B/`: the instance files,
`mtl-perf-rx0.json` and `mtl-perf-tx0.json` with each command and configuration,
`phc2sys.log`, and a `<group>.json` for each capture with its gaps, its SDP and LIST's
report.
