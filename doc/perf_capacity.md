# 100 G session capacity: analysis and perf-oracle fixes

Status: **implemented and validated on hardware** · Baseline run: [perf-pytest 35799748224](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/runs/35799748224)
(mtl-runner-7 as DUT, mtl-runner-8 as companion, both Quanta Grid D55Q-2U, E830-CC 100 G VF) ·
Subject: `tests/acceptance/tests/dual/performance/test_vf_perf_dualhost.py`, 1080p59 ST2110-20

This document answers three capacity questions raised against that run, and specifies the defects
in the test's measurement oracle that the answers uncovered: two that corrupted published numbers
(§6, §7), one instance of the first on the companion host (§6.1), and one hazard the fix for the
first had to handle (§6.2). It is the plan of record for the fix; §9 tracks implementation state
and §10 validation.

---

## 1. The questions

1. TX multi-core reports only 36 sessions while its throughput reads ~65 Gb/s, not the ~100 Gb/s
   the port can carry. What is the bottleneck?
2. Is single-core TX affected by the same bottleneck, and can it be higher?
3. RX multi-core reports 94 Gb/s on a 100 Gb/s port and is not core-limited. Can it carry more
   than 36 sessions?

## 2. Answers in one line each

1. **Two unrelated things were conflated.** The 65 Gb/s is a reporting defect — the port really
   carries **93.76 Gb/s** at 36 sessions. The session count 36 is limited by the **100 G port
   itself**, not by cores: 36 sessions occupy **95.4 % of the wire** once Ethernet framing is
   counted. Only 3 of 17 available lcores were in use.
2. **Yes to the reporting defect, no to the bottleneck.** Single-core TX really carries
   **78.7 Gb/s**, not the published 59.9. But its ceiling is the *host TX path*, at ~80 % of the
   port — it is not wire-limited. Its honest clean capacity is **30 sessions, not 31**; 31 passed
   only because of the second defect (§7).
3. **No.** 36 sessions is already 95.4 % of the wire; 37 needs 98.1 % and 38 needs 100.7 %, which
   is physically impossible. 37 was tried and delivered 30/37. The apparent 6 Gb/s of "headroom"
   is 1.7 % per-packet framing overhead plus the pacing margin ST 2110 narrow pacing needs.

## 3. Evidence base

Everything below is derived from the CI artifact of the baseline run (8 sweep logs, no-DMA and
DMA variants, 90 measured iterations) plus the MTL sources that emit the parsed lines. No number
is modelled without a measurement to check it against.

The per-iteration figures in §5 were **re-derived** from the raw `DEV(n): Avr rate` stat dumps in
each log, averaged over the FPS steady window that the same log already prints. That is precisely
the computation §6 installs in the test, so §5 is also a dry run of the fix against real data.

Key sources:

| Line consumed | Emitted by |
|---|---|
| `DEV(n): Avr rate, tx: F Mb/s, rx: F Mb/s, pkts, tx: N, rx: N` | `dev_inf_stat()`, `lib/src/dev/mt_dev.c` |
| `TX_VIDEO_SESSION(p,i:app_tx_st20p_i): fps F frames N` | `lib/src/st2110/st_tx_video_session.c` |
| stat cadence 10 s | `MT_STAT_INTERVAL_S_DEFAULT`, `lib/src/mt_stat.c` |
| `"fps": "p59"` → `ST_FPS_P59_94` | `parse_st20p_fps()`, `tests/tools/RxTxApp/src/parse_json.c` |

## 4. What one 1080p59 session actually costs on the wire

`p59` is **not** 59 fps. `parse_st20p_fps()` maps it to `ST_FPS_P59_94` = 60000/1001 =
**59.94006 fps**. That single fact drives both §7 and every figure here.

Packet geometry for 1920×1080 YUV 4:2:2 10-bit, GPM packing, from the `st20_total_pkts`
computation in `st_tx_video_session.c`. The packet size is set by MTL's own reserve, not by the
MTU: `MTL_PKT_MAX_RTP_BYTES` is `1460 - 8 - 100` = 1352 B, leaving ~100 B of the 1500 B MTU
deliberately unused; 1352 − 20 (RTP + payload header) − 6 (SRD) = 1326, floored to a whole pixel
group → 1320.

| Quantity | Value |
|---|---|
| Frame payload | 1920 × 1080 × 2.5 = 5 184 000 B |
| RTP payload per packet | 1320 B |
| Packets per frame | 3928 (3927 full + 1 tail of 360 B) |
| Single-SRD packets | 2946 × 1382 B on the wire |
| Dual-SRD packets | 981 × 1388 B |
| Tail packet | 1 × 422 B |

Per-packet non-payload bytes: 14 Ethernet + 20 IPv4 + 8 UDP + 12 RTP + 2 ext-seq + 6 per SRD.

Two different byte counts matter, and confusing them is what makes "94 of 100 Gb/s" look like
headroom:

| Accounting | Bytes/frame | Per-session rate | What it is |
|---|---|---|---|
| **L2** | 5 433 422 | **2605.44 Mb/s** | Ethernet frame octets *excluding* FCS. This is DPDK `obytes`, so it is what MTL's `DEV` line reports. |
| **L1** | 5 527 694 | **2650.64 Mb/s** | Adds 24 B/packet the wire spends but no counter shows: FCS 4 + preamble 7 + SFD 1 + inter-frame gap 12. This is what consumes the 100 G serial link. |

So **L1 = 1.01735 × L2**. A port that could be filled continuously would hold
100 000 / 2650.64 = 37.73 sessions — but ST2110-21 pacing does not let it be filled
continuously, which is §4.1 and the real answer to Q1 and Q3.

Both halves of this model are confirmed by measurement, not assumed:

* **L2 leg, and whether `DEV` includes FCS.** The `DEV` line carries a byte rate *and* a packet
  count, so their ratio is bytes per packet — independent of frame rate, session count, and the
  length of the stat window. Across every steady dump in the run, grouped by direction and session
  count (21 TX groups spanning 16–72 sessions, 19 RX groups spanning 8–72, single- and multi-core,
  plain and redundant), that ratio is **1383.254 B/pkt** against the geometry's
  5 433 422 / 3928 = **1383.2541**: worst case +0.000015 % on TX and +0.062 % on RX, the latter
  being a little non-media traffic (ARP, IGMP, PTP) in the mix. FCS-inclusive accounting would give
  1387.2541, **+0.289 %**. So both `obytes` and `ibytes` exclude FCS — measured from the counters
  themselves, not inferred from iavf's `stats->ibytes -= stats->ipackets * crc_stats_len`.
* **No pad packets reach the wire in this workload.** RL pacing can inject padding frames to soak
  up the hardware's overshoot, and `tv_train_pacing` did train one here: the logs show
  `pad_interval 231.87`, i.e. 3928 / 231.87 = **16.9 pads per frame**, which would be +0.43 % on
  both port counters — enough to matter for a figure quoted to 0.04 %. It does not show up.
  Comparing the port's packet counter against the builder's own `pkts` counter in the same dumps,
  the port sends **0.04 % fewer** packets than the builder enqueues (`dev/build` 0.9995–0.9997) at
  every load from 16 sessions (42 % of the link) to 36 (99 % of the active window), with the
  builder at 3928.0 packets per frame exactly. Pads on the wire would put that ratio at 1.0043.
  The same 0.04 % appears in the byte comparison, so the residual lives in the port's packet
  counter rather than in the byte accounting; its cause is not chased here because nothing in this
  document turns on 0.04 %. Note also that the implementation is immune to this question either
  way: `monitor_dev_rate()` derives the L1 correction from the *measured* packet count on the same
  line, so a pad packet that did reach the wire would be charged its 24 B like any other.
* **L1 leg.** Deliberately oversubscribed probes drive the port to saturation. The highest
  sustained `DEV` rates observed are 98 110 Mb/s L2 (TX MC, 48 sessions) and 97 881 Mb/s L2
  (RX MC + DMA, 48 sessions). Scaled by 1.01735 those are **99 812** and **99 579 Mb/s L1** —
  99.81 % and 99.58 % of line rate. The model lands within 0.2 % of a hard physical limit.

### 4.1 Pacing, not the average rate, is what caps the session count

The averages above are not what the link sees. ST2110-21 narrow pacing confines a frame's packets
to the *active-video* part of the frame period — `st_tx_video_session.c:503,519`:

```c
/* 1080/1125 = 0.96 for 1080p; the annotations below are this document's */
pacing->reactive = 1080.0 / 1125.0;
/* trs is the inter-packet gap the transmitter paces to */
pacing->trs = frame_time * pacing->reactive / s->st20_total_pkts;
```

All 3928 packets go out in **96 %** of the frame period and the port idles for the remaining 4 %
(the vertical blanking interval). Two consequences:

* the **instantaneous** rate a session demands while it is transmitting is `1 / 0.96` = 1.0417×
  its average — **2761.08 Mb/s L1**, not 2650.64;
* the sessions do **not** average out against each other. `pacing->cur_epochs = ptp_time /
  frame_time` derives the epoch from PTP, and `tr_offset` is a function of height only, so every
  session at the same format starts its active window at the same instant. There is no
  per-session stagger. The peaks coincide exactly.

So the usable fraction of the link for paced ST2110-20 traffic is 96 %, and the ceiling is

```text
floor(100 000 Mb/s × 0.96 / 2650.64 Mb/s) = floor(36.22) = 36 sessions
```

which is the published maximum. This is the only mechanism found that predicts both 36 and the
oversubscribed delivery ceiling below; it is not proved to be the *sole* cause.

**The rate limiter is not a hard cap at its setpoint, and the run measured by how much.** At first
reading the hardware setpoint contradicts the model: `tv_rl_bps()` (`st_tx_video_session.c:84-91`)
hardcodes `reactive = 1.0` for progressive video — the interlaced SD branch is the only one below
1 — so `flow.bytes_per_sec` (`:2725`, `:4184`) asks for the frame *average*, 2603 Mb/s L2, not
`1 / 0.96` of it. If that were a hard cap no session could send its active window at the rate
pacing requires, and 37 sessions at a 98.1 % average ought to fit.

`tv_train_pacing()` (`:333-492`) measures what the queue really does. It runs once per process,
port and rate, and later sessions reuse the cached result (`:366-370`): the baseline logs hold 195
training runs against 6005 reuses. A run fills the queue back to back with 71 frames' worth of
packets, times each frame, and averages the middle 60. Its `measured_bps` (`:451`) is therefore the
rate at which the hardware drains a saturated queue, sustained for about a second and scaled by
`reactive`; `measured_bps / tv_rl_bps()` divided by 0.96 is that drain rate in units of the frame
average. All 195 runs take both passes below, with ratios identical to four decimal places:

| Pass | Log line | `measured_bps / rl_bps` | Drain rate ÷ frame average |
|---|---|---|---|
| 1, queue at `tv_rl_bps()` | `too small pad_interval 13.666 pkts_per_frame 4215.418` | 1.0732 | **1.1179** |
| 2, queue at `bps_to_set` | `trained pad_interval 231.872 pkts_per_frame 3944.940` | 1.0043 | **1.0462** |

Pass 1 fails the `pad_interval > 32` test at `:458`, so `:485` recomputes
`bps_to_set = 1.005 × rl_bps² / measured_bps` = **0.9365 × `tv_rl_bps()`**, calls
`mt_txq_set_tx_bps()` and retrains. Pass 2 passes and stores only a `pad_interval`, so that lowered
setpoint is what the queue keeps. Three things follow:

* the queue drains at **1.117×** whatever rate it is programmed to: 1.1179× at `tv_rl_bps()`,
  1.0462 / 0.9365 = 1.1171× at `bps_to_set`. Why the limiter over-delivers by that factor is not
  established here; training exists to measure it, not to explain it;
* after training a session's queue sustains **1.0462×** the frame average, and narrow pacing needs
  **1.0417×** during the active window. The 0.4 % margin means the per-session limiter does not
  bind. The port does: the table below puts 36 sessions at 99.4 % of it and 37 at 102.2 %;
* no session took the `measured_bps < rl_bps` branch at `:467` — 0 occurrences of
  `measured bps … is lower than set bps` in the whole run — so no queue was trained below its
  setpoint.

The trained `pad_interval` is the limiter's other defence against the same overshoot, and §4 shows
it stays unused here: no pads reach the wire at any load from 16 to 36 sessions. Independently,
something holds the port idle about 4 % of the time, because an oversubscribed run delivers
94–95 Gb/s L2 when the same port demonstrably carries 98 Gb/s once pacing collapses (table below).

| Sessions | L1 average | % of 100 G | **L1 during active window** | **% of 100 G** | Outcome in the run |
|---|---|---|---|---|---|
| 30 | 79 519 | 79.5 % | 82 832 | 82.8 % | clean |
| 31 | 82 170 | 82.2 % | 85 594 | 85.6 % | clean on MC |
| 36 | 95 423 | 95.4 % | **99 399** | **99.4 %** | **clean — the published maximum** |
| 37 | 98 074 | 98.1 % | **102 160** | **102.2 %** | impossible while paced; 32/37 TX, 30/37 RX |
| 38 | 100 724 | 100.7 % | 104 921 | 104.9 % | impossible either way; 28/38 TX, 24/38 RX |

This also predicts how much an *over*subscribed run still delivers. While pacing holds, the most
that can leave the port is 96 % of line rate = 96 000 Mb/s L1 = **94 363 Mb/s L2**:

| Sessions | Predicted deliverable L2 | Measured `DEV` L2 | fps |
|---|---|---|---|
| 37 | 94 363 | 94 749 (+0.4 %) | 58.90 |
| 38 | 94 363 | 95 344 (+1.0 %) | 57.72 |
| 48 | 94 363 | 98 110 (+4.0 %) | 47.06 |

The overshoot grows with oversubscription because pacing progressively *stops* holding: packets
that miss their `trs` deadline queue in `inflight[]` and are sent in the blanking interval, so the
traffic becomes less paced and more continuous. At 48 sessions it is barely paced at all (47 fps)
and the port reaches the 99.8 % continuous figure measured in the L1 leg above. That 99.8 % is
therefore *not* evidence that 37 paced sessions would fit — it is the rate of a stream that has
already failed its timing.

## 5. Corrected measurements

`fps` is the mean of the per-session FPS averages over the steady window; nominal is 59.94.
`DEV` is L2 Mb/s over the same window (per port for redundant cases).

### Non-redundant, no DMA

| Case | Sessions | Verdict | fps | DEV L2 | Reading |
|---|---|---|---|---|---|
| TX MC | 32 | 32/32 | 59.90 | 83 343 | demand met |
| TX MC | **36** | **36/36** | **59.90** | **93 761** | **demand met — port at 95.4 % L1** |
| TX MC | 37 | 32/37 | 58.90 | 94 749 | 1.7 % short of the 96 401 demanded |
| TX MC | 38 | 28/38 | 57.72 | 95 344 | 3.7 % short |
| TX MC | 48 | 5/48 | 47.06 | **98 110** | **wire saturated, 99.81 % L1** |
| TX SC | 28 | 28/28 | 59.90 | 72 916 | demand met |
| TX SC | **30** | **30/30** | **59.90** | **78 131** | **demand met — true SC maximum** |
| TX SC | 31 | 31/31 ✗ | 58.50 | 78 749 | **false pass** (§7); supply-limited |
| TX SC | 32 | 0/32 | 56.40 | 78 470 | supply-limited |
| RX MC | **36** | **36/36** | **59.90** | **93 759** | demand met |
| RX MC | 37 | 30/37 | 59.07 | 95 018 | sender 1.4 % short |
| RX MC | 48 | 0/48 | 46.95 | 97 845 | wire saturated |
| RX SC | **21** | **21/21** | **59.90** | **54 693** | demand met |
| RX SC | 22 | 0/22 | 18.44 | 57 301 | **all traffic arrived**; core could not drain it |

The evidence for a single-core TX plateau is the **demand-independence** of the delivered rate. At
30 sessions the link carries exactly what is demanded. At 31 and 32 the demand rises by 2605 Mb/s
and the delivered rate does not move:

| Sessions | L2 demand | L2 delivered | measured fps |
|---|---|---|---|
| 31 | 80 769 | 78 749 | 58.50 |
| 32 | 83 374 | 78 470 | 56.40 |

78 749 vs 78 470 is a 0.35 % spread across a 3.2 % change in demand, so single-core TX plateaus at
**≈78.6 Gb/s L2 = 80.0 Gb/s L1**, i.e. ⌊78 600 / 2605.44⌋ = **30 sessions**. Two points with that
spread support a plateau near 78.6 Gb/s; they do not pin it to three digits.

Note what this table is *not*. Converting the delivered rate back to a frame rate
(`delivered / (N × 2605.44) × 59.94`) reproduces the measured fps to 0.1 — but that is an identity,
not a prediction: a session's byte rate is linear in its frame rate, which is the same model the
rate figures are built on. It is a useful consistency check between two independent counters (DPDK
octets and the per-session frame counter) and nothing more.

### Redundant and DMA ceilings, for completeness

Every rate in this table is L2 per port; a redundant session sends the same stream on both, so its
host-side cost is twice its per-port figure.

| Case | Max clean | fps | DEV L2 per port | Ceiling observed |
|---|---|---|---|---|
| TX redundant MC | 36 | 59.81 | 93 601 | 94 665 at 40 sessions |
| TX redundant SC | 20 | 59.90 | 52 083 | **52 746** — one core builds 105 Gb/s L2 across the two ports |
| RX redundant MC | 36 | 59.70 | 93 389 | 94 196 at 38 sessions |
| RX redundant SC + DMA | 30 | 59.76 | 78 128 | 80 722 at 31 sessions |
| RX redundant SC, no DMA | 15 | 59.90 | 39 067 | — |
| RX SC + DMA | 36 | 59.90 | 93 755 | DMA lifts single-core RX from 21 → 36 |

---

## 6. Defect 1 — rate metrics are diluted by session-teardown stat dumps

**Symptom.** Every TX-measured case understates port throughput, by 12.5–35.8 % (median 27.5 %)
over the run's 26 TX app runs. The 36-session TX MC winner reports `DEV TX: 65 149.48 Mb/s` where
the port carried 93 761. RX-measured cases are correct.

**Cause.** `monitor_dev_rate()` and `monitor_tx/rx_throughput()` average *every* stat dump after a
fixed 60 s wall-clock warmup (`_collect_after_warmup`). `MT_STAT_INTERVAL_S_DEFAULT` is 10 s and
each dump reports the mean rate since the previous one, so dumps emitted while the app is tearing
down report a partial period and then zero. `st20p_tx_free()` costs roughly **1 s per session**, so
a 36-session TX app keeps dumping for 30+ s after traffic stops:

The 11 dumps this run averaged, from the winning 36-session iteration of
`test_tx[multi_core-59fps-1080p-no_dma]` — the first seven are the tail of the steady run, the
eighth onwards are teardown:

```text
TX: MTL: 2026-09-23 00:44:46, DEV(0): Avr rate, tx: 93768.903274
TX: MTL: 2026-09-23 00:44:56, DEV(0): Avr rate, tx: 93759.061955
TX: MTL: 2026-09-23 00:45:06, DEV(0): Avr rate, tx: 93755.228802
TX: MTL: 2026-09-23 00:45:16, DEV(0): Avr rate, tx: 93768.743904
TX: MTL: 2026-09-23 00:45:26, DEV(0): Avr rate, tx: 93752.847874
TX: MTL: 2026-09-23 00:45:36, DEV(0): Avr rate, tx: 93761.313866
TX: MTL: 2026-09-23 00:45:46, DEV(0): Avr rate, tx: 93767.955414
TX: MTL: 2026-09-23 00:45:53, _mt_stop, succ                      <- traffic stops
TX: MTL: 2026-09-23 00:45:56, DEV(0): Avr rate, tx: 60310.175230  <- 7 s of a 10 s period
TX: MTL: 2026-09-23 00:46:06, DEV(0): Avr rate, tx:     0.000000
TX: MTL: 2026-09-23 00:46:16, DEV(0): Avr rate, tx:     0.000000
TX: MTL: 2026-09-23 00:46:26, DEV(0): Avr rate, tx:     0.000000
TX: MTL: 2026-09-23 00:46:29, mtl_uninit, succ                    <- 36 s after _mt_stop
```

Their mean is 65 149.475, which is the 65 149.48 the run published —
`(7 × ~93 760 + 60 310.18 + 3 × 0) / 11`. The 36 s from `_mt_stop` to `mtl_uninit` for 36 sessions
is where the 1 s per session comes from. RX escapes this only by accident: every RX app run frees
all its sessions within a second of `_mt_stop`, well inside one stat period, so it exits before the
stat thread fires again. None of the run's 52 RX app runs prints a dump after `_mt_stop`; each of
its 26 TX app runs prints between 2 and 5.

**Why the fix is not a new heuristic.** The test already computes the correct interval. `_steady_
window()` bounds a window by the first and last stat dump that names every session, takes the
strict interior, and starts after the first dump in which every session is live — exactly the
dumps that cover full-rate traffic for a whole period. The FPS verdict has always used it. The
rate metrics simply never adopted it.

**The change.** Average the rate metrics over that same window:

* extract the `{dump: {session: fps}}` census loop out of `_monitor_fps_generic()` so the
  window can be computed for a log on its own (needed for the companion host, which has no FPS
  verdict of its own);
* replace `_collect_after_warmup(lines, pattern, warmup_seconds)` with
  `_collect_in_window(lines, pattern, window)`, keeping a match only when the stat dump it was
  printed inside is in the window (§6.2 — not when its own timestamp is);
* `monitor_dev_rate(lines, window)` and `monitor_tx/rx_throughput(lines, window)`; drop the
  `num_sessions` parameter both throughput functions accept and never use;
* in the test, pass `fps_details["window"]` for the measured side and
  `companion_steady_window(companion_lines, num_sessions, companion_dir)` for the companion
  (§6.1);
* delete `WARMUP_SECONDS`, now unused.

**The published rate goes up, which is the correction and not a bias.** Dropping zero-rate teardown
dumps necessarily raises the mean; that is the defect being fixed, so the direction of change is not
evidence either way. What has to be shown is that the choice of which dumps to drop cannot be
influenced by the rates themselves. Four properties:

* **the selection criterion never reads a rate, and cuts only at the ends.** `_steady_window()`
  sees only the per-session FPS census — which sessions each dump names, and whether each names a
  non-zero fps. No `DEV` or throughput value is an input to it. Fps and rate are coupled, so a
  zero-fps dump is a low-rate dump, but the census is consulted only to place the two ends of the
  window: every dump between them is kept whatever it reads, and a session stalled mid-run keeps
  its zeros in the window (§6.1). There is no path by which a high interior sample could be
  preferred to a low one;
* **the metric is a plain mean of the survivors, never a rescaling.** A dump's rate is published as
  MTL reported it;
* **where there is nothing to trim it moves the rate both ways, and barely.** The RX app runs have
  no teardown dumps (above), so the only difference from HEAD's 60 s warm-up is where the two cut
  the ends. Over the 49 judged RX app runs the published rate moves by −0.35 % to +0.07 %, median
  −0.00 % — a systematic upward bias would show here, and does not;
* **it cannot move a verdict.** The rate metrics are reported, not asserted on. The pass/fail
  decision is the FPS bar, and the only change to that (§7) makes it strictly stricter.

The cost is that it can also drop one legitimate steady sample at each end: the first all-live
dump after the first full census, and the last dump naming every session. On the test fixture six
steady dumps yield a four-dump window. Against the 4-sample floor that is a deliberate trade: the
first may cover the moment the far end started sending and the last the first session free, and
there is no way to tell from the log which ones do.

### 6.1 The same defect on the companion host — found by the hardware re-run

The first hardware re-run of the 36-session TX MC iteration (§10) fixed the measured side exactly
as predicted, `DEV TX: 93 760.21 Mb/s`, but its companion reported `DEV RX: 73 464.57 Mb/s` — 22 %
below the 93.8 Gb/s it had demonstrably received. The census bound that saves the measured host
does nothing here, and the reason is in the harness, not the library:

* the measured app is *stopped*, so it frees its sessions and its last dumps name fewer and fewer
  of them — which is what `_steady_window()`'s census bound trims;
* the companion app is *killed*. It frees nothing, so **every** dump to the end of its log still
  names all 36 sessions. The census bound finds nothing to trim and the window runs on through the
  partial dump in which the far end stopped plus four fully idle dumps.

Cross-host timestamp intersection is not an option: the two hosts' stat clocks are unrelated (the
companion reads `09:5x` where the DUT reads `07:5x` in the same run), so there is no shared axis.

**The change.** In `companion_steady_window()` — the companion-only entry point — drop a *trailing*
run of dumps in which no session is live, plus the one dump before it, on the same logic
`_steady_window()` already uses at the other end: the dump before the idle run is the one
containing the moment traffic stopped, so its period is partial.

**The bias this introduces, stated plainly.** The trim cannot distinguish the idle tail of a healthy
run from a run that genuinely collapsed at the end and stayed down; it removes both, so the
companion rate it publishes is biased slightly high. Two things bound the cost. A collapse the far
end recovers from is not trailing and is kept, zeros and all — `_steady_window()` retains every
sample from the dump after the first all-live one onward, including zeros. And a companion that
really did stop receiving shows up as the measured side's own FPS verdict failing, which is the
verdict that decides the iteration. So the bias can cost a *rate line* — if the trim takes the
window below `FPS_MIN_STEADY_SAMPLES` the companion rate is suppressed and logged as such — but not
a verdict.

**Why not inside `_steady_window()`.** That function also bounds the FPS verdict. Trimming idle
dumps there would hide a run that transported nothing at the end — exactly the failure the verdict
exists to catch. The companion has no verdict, only a rate, so the trim is safe there and nowhere
else.

**Why only a trailing run.** An all-zero dump in the *middle* of a window is a real collapse and
must stay in the average; a log whose last dump still carries traffic has no idle tail and is left
whole. Both properties are pinned by tests, and both die if the trim is written as "drop every idle
dump" (§10).

### 6.2 A hazard the window fix introduces: MTL timestamps lines, not dumps

The window has to name stat dumps, because a dump's rate comes from its `DEV` line while the census
that decides whether the dump is steady comes from its *session* lines. HEAD's census identifies a
dump by the timestamp its lines carry. That looks obviously right and is not, for two reasons, both
measured across the baseline run:

* **A dump can straddle a second.** `dev_inf_stat()` and the per-session stat functions each stamp
  their own line as it is printed, to one-second resolution. **16 of the 1026 dumps** carrying a
  `DEV` line put it on the opening banner's second and every session line on the next — this one a
  ramp-up dump of `test_tx[multi_core-59fps-1080p-no_dma]`:

  ```text
  TX: MTL: 2026-09-23 00:25:05, * *    M T    D E V   S T A T E   * *
  TX: MTL: 2026-09-23 00:25:05, DEV(0): Avr rate, tx: 39373.499413 Mb/s, …
  TX: MTL: 2026-09-23 00:25:06, TX_VIDEO_SESSION(1,0:app_tx_st20p_0): fps 25.289278 …
  TX: MTL: 2026-09-23 00:25:06, * *    E N D    S T A T E   * *
  ```

  12 of the 16 are in the 16-session iteration of
  `test_rx_redundant[single_core-59fps-1080p-no_dma]`, 4 in the 48-session iteration of
  `test_tx[multi_core-59fps-1080p-no_dma]`. Matched on stamp, such a dump contributes no rate sample
  — all 10 dumps of the first's stamp-keyed window, 2 of 9 of the second's — and
  `_log_throughput()`'s `if not vals: continue` turns that into a silent hole, not an error. That is
  the only way a dump splits: no dump prints its census under more than one stamp (0 of 1025), and
  none of the 514 two-port dumps puts `DEV(0)` and `DEV(1)` on different seconds.
* **Freeing a session prints its stats once more, outside any dump.** `tv_detach()` and
  `rv_detach()` call `tv_stat()` / `rv_stat()` as the session is freed, so **2522 of the 34 384**
  session stat lines in the run fall outside the banners. An app that frees every session within
  one second, as this RX app does, prints what reads, by stamp, as one more dump naming every
  session:

  ```text
  RX: MTL: 2026-09-23 01:29:31, RX_VIDEO_SESSION(1,0:app_rx_st20p_0): fps 59.899893 frames 599 …
  RX: MTL: 2026-09-23 01:29:31, * *    E N D    S T A T E   * *
  RX: MTL: 2026-09-23 01:29:33, _mt_stop, succ
  RX: MTL: 2026-09-23 01:29:33, RX_VIDEO_SESSION(1,0:app_rx_st20p_0): fps 59.175589 frames 139 …
  ```

  That pseudo-dump becomes the last dump naming every session, the interior bound keeps the real
  last one, and the window runs one dump too long. HEAD's FPS verdict does exactly this in **48 of
  the 75** judged app runs. The dump it keeps is usually an ordinary full-period one — its 1533
  session samples have a median of 59.90 fps — so this is not a claim that HEAD judged partial
  periods; the low values among them (606 below the 59.34 bar) are in overloaded iterations that
  fail anyway. At the same bar it moves one pass count: the 32-session iteration of
  `test_rx_redundant[single_core-59fps-1080p-dma]`, 2 of 32 under HEAD against 21 of 32 here, which
  fails either way.

**The change.** Identify a dump by the banner `stat_dump()` prints in front of it
(`mt_stat.c:45-61`), not by the stamps of the lines inside it. `_walk_dumps()` yields
`(_Dump(idx, ts), line)` for every line from the first banner on, assigning each line to the dump
whose opening banner came last before it; `_fps_dumps()` keys the census on that dump and
`_collect_in_window()` admits a match when its dump is in the window. `_stat_dump()` has exactly
one caller and it always brackets the call with the two banners, which each appear 1027 times in
the baseline logs, so none is lost.

That reunites a straddling dump by construction. Free-time lines are assigned to whichever dump
preceded them, and that is harmless by the library's locking, not by luck.
`tv_mgr_detach()` (`st_tx_video_session.c:3715-3730`) and `st_rvs_mgr_detach()`
(`st_rx_video_session.c:4103-4118`) print a session's free-time stats and clear its
`sessions[idx]` slot under one hold of the session lock, and the stat thread takes that lock to
print the session (`:3861`, `:4147`). So every dump naming a session began before that session's
free-time line, and no dump begun after it can name that session. The last dump naming every
session therefore began before every free-time line, which lands in it or later — and
`_steady_window()`'s interior bound excludes exactly those. Skipping the lines between a closing
banner and the next opening one would express the same thing a second time; it changes nothing
reachable, so it is not there.

`_collect_in_window()` yields the window dump alongside each match rather than just the match, for
one reason: **half the perf matrix is redundant, and a redundant config emits one `DEV` line per
port per dump.** A two-port run legitimately produces `2 × len(window)` samples, so a check that
counts matched lines against the window length either warns on every redundant run or, relaxed to
suit them, cannot see a window dump go missing. `monitor_dev_rate()` therefore warns when a window
dump has no `DEV` line at all, which is lost evidence whatever the port count. On the 75 judged app
runs this warning is silent (§10).

Four mutations pin it (§10): delimiting on the closing banner instead of the opening one, never
advancing the dump ordinal, regrouping by timestamp — the defect reintroduced — and counting `DEV`
coverage per line instead of per dump.

**A trap for anyone re-analysing the artifact logs.** `RxTxApp` stdout interleaves at the byte
level, so a worker thread's line can be spliced into the middle of the stat thread's:

```text
RX: app_rx_st20p_frame_thMTL: 2026-09-23 00:48:19, * *    M T    D E V   S T A T E   * *
```

An ad-hoc filter of the form `<DIR>: MTL:` drops those lines, which silently merges physical dumps
and produces nonsense — it is what an earlier draft of this section counted. Filter on the relay
prefix (`INFO\s+<DIR>:` plus a space) instead. Production never sees this: the measured side splits
`result.stdout_text` and the companion side reads the remote log unfiltered. The line's *own*
timestamp is safe to trust either way — 0 of the 34 384 session stat lines in the run carry two
timestamps.

## 7. Defect 2 — the FPS bar is computed from the format token, not the paced rate

**Symptom.** `test_tx[single_core]` publishes 31 sessions. At 31 sessions **every** session ran at
58.50 fps against a 59.94 nominal — a uniform 2.4 % frame-rate deficit, i.e. every stream dropping
frames — and the oracle passed it.

**Cause.** The test parametrizes `fps` as the integer token `59`, formats it into the RxTxApp
config as `"fps": "p59"`, and *also* uses it as the numeric target:

```python
min_required = expected_fps * FPS_TOLERANCE_PCT   # 59 * 0.99 = 58.41
```

But `parse_st20p_fps()` maps `"p59"` to `ST_FPS_P59_94` = **59.94006** fps. The intended 99 %
tolerance is therefore really 58.41 / 59.94006 = **97.45 %**, granting 2.55 % of silent slack.
The same mismatch exists for `p29` (29 vs 29.97, 3.3 % slack); `p25` and `p50` are exact.

**Consequence in the baseline run.** Checked against a correct bar of 59.94006 × 0.99 = 59.34, all
ten published winners hold except one:

| Case | Published | fps achieved | Corrected |
|---|---|---|---|
| `test_tx[single_core]` | 31 | 58.50 | **30** (59.90) |
| `test_tx_redundant[multi_core]` Phase 2 VERIFY at quota 36 | 36 "verified" | 58.90 | VERIFY correctly reported unstable; Phase 1 result of 36 at quota 12 (59.81) stands |
| all other winners | — | 59.67 – 59.90 | unchanged |

The second row is worth noting on its own: re-running the same 36 sessions at quota 36 instead of
12 costs 1.6 % of throughput, and the slack hid it. A VERIFY failure is logged only, so this
changes no test outcome.

**The change.** Score against the rate the transport is actually paced at, taken from the mapping
the suite already carries rather than from a second copy of it:

```python
def paced_fps(fps_token):
    """Rate the transport is paced at for a ``p<token>`` format token → float."""
    return float(Fraction(pformat_to_exact_fps(fps_token)))
```

`mtl_engine/media_files.py` already holds this mapping — `pformat_to_exact_fps()` returns the exact
rational (`"60000/1001"`) for the four labels that truncate by the NTSC convention
(`_TRUNCATED_FPS_LABELS = {23, 29, 59, 119}`) and the label itself for every label that is already
exact. Reusing it rather than adding a second table means a new format token cannot be scored
against a rate this module invented, and a token added to the sweep needs no change here.

**Confirmed on hardware, not only by replay.** Re-running the published 31-session single-core TX
iteration on mtl-runner-7 with the fix in place (`--num_sessions 31`, FIXED mode):

```text
TX Results: 0/31 sessions at 59.00 fps (min: 59.34)
    DEV TX: 79021.30 Mb/s wire 80392.35 Mb/s (9 samples)
E   Failed: Fixed run FAILED: 31 sessions for TX SC @ 59fps / 1080p
1 failed in 584.22s (0:09:44)
```

All 31 sessions ran, none reached the bar, and the port rate matches the ≈78.6 Gb/s plateau §5
records for that iteration. The honest single-core TX capacity is 30.

That headline is quoted as the run printed it, and it shows the *second* half of this defect still
present: `at 59.00 fps (min: 59.34)` states a pass bar **above** its own stated target, which is
impossible and is the token-as-rate confusion surfacing in the report rather than in the verdict.
The verdict was already correct at that point — `min: 59.34` is the corrected bar, which is why the
run fails — but `display_session_results()` was still being handed the raw token. That call site now
passes `paced_fps(fps)`, so the headline reads `at 59.94 fps`. The fix was made after this run, so
no hardware log in this document shows it; it is covered by
`test_fps_bar_is_the_paced_rate_not_the_format_token` instead.

## 8. The three answers in full

### Q1 — why only 36 TX sessions at "65 Gb/s"

Two separate facts were reading as one.

*The 65 Gb/s is not a measurement of the port.* It is Defect 1. The DUT transmitted
**93 761 Mb/s** L2 throughout the steady window; the published figure averaged that together with
one partial dump and three zero dumps from session teardown.

*The 36 is a pacing limit on the wire, not a core limit.* Narrow pacing sends each frame in 96 % of
the frame period and idles through the vertical blanking interval (§4.1), and every session's
active window is aligned to the same PTP epoch, so the peaks add. 36 sessions occupy **99.4 % of
line rate while they are transmitting**; a 37th would need 102.2 %:

```text
floor(100 000 × 0.96 / 2650.64) = 36
```

That arithmetic lands on the observed ceiling and on the 94–95 Gb/s an oversubscribed run still
delivers, which no other mechanism examined here accounts for. §4.1 records why the per-session
rate limiter, programmed near the frame average, does not bind first: after training it sustains
1.0462× the frame average against the 1.0417× the 96 % schedule needs, so the port saturates
before any one session's limiter does.

Cores are not involved: the run used 3 of 17 available lcores, two data cores carried 18 sessions
each, and one core is separately proven to carry 30.

### Q2 — is single-core TX affected, and can it be bigger?

*Affected by the reporting defect: yes.* Published 59 856 Mb/s; actual 78 749 Mb/s — understated
by 24 %.

*Affected by the same bottleneck: no.* Single-core TX saturates at **78.6 Gb/s L2 / 80.0 Gb/s L1 =
80 % of the port**, proven by the demand-independent plateau and the two-point fps prediction in
§5. It stops ~19.5 Gb/s short of the wire, so it is limited by the host TX path, not the link.

*Can it be bigger?* Only by making that path cheaper — there is ~7 sessions of wire headroom
waiting for it. Scheduler `avg loop` time over the sweep is strongly superlinear and then flat
(16 sessions → 1788 ns, 24 → 6112, 28 → 11 743, 30 → 16 988, 31 → 17 447, 32 → 18 096; per session
112 ns → 566 ns, saturating), which points at per-session rather than per-packet cost in the
builder/transmitter pair. **That is a characterization, not a diagnosed defect**, and this change
deliberately does not act on it: publishing a correct 78.6 Gb/s is the prerequisite for
investigating it.

Its honest capacity is **30 sessions**, not the published 31 (Defect 2).

### Q3 — can RX multi-core exceed 36?

No. There is less headroom than 94 of 100 suggests, for two compounding reasons.

**1. The 94 is L2 and the 100 is L1.** 94 Gb/s of `DEV` rate is **95.4 % of the wire**, not 94 %.
The missing 1.7 % is the 24 B/packet of FCS, preamble and inter-frame gap that no counter reports —
3928 packets/frame × 36 sessions × 59.94 fps is 8.5 million packets/s, and at 24 B each that is
1.63 Gb/s.

**2. Only 96 % of the link is available to paced traffic at all.** That 95.4 % average is spent in
96 % of the wall clock, so it is **99.4 % of line rate while transmitting** (§4.1). The remaining
budget is 0.6 %, not 4.6 %. The rate limiter's own training telemetry says the same from the other
direction: after training it sustains 1.0462× the frame average where narrow pacing needs 1.0417×,
a margin of 0.4 % (§4.1).

* 37 sessions need 102.2 % of line rate during the active window. Tried on RX: **30/37** at
  59.07 fps, the sender delivering 95 018 Mb/s L2 against 96 401 demanded — and 94 363 is what §4.1
  predicts a paced port can deliver.
* 38 sessions are impossible on the average alone (100.7 % of L1). Measured on RX: **24/38**.

Both figures are the RX-measured ones, which is why they are lower than the TX-measured 32/37 and
28/38 in §5: at these loads the sender is already missing its pacing deadlines, so the receiver sees
fewer conforming streams than the sender counts.

**Core count is irrelevant** — the run used 3 of 17.

Two caveats on the ceiling. Upstream `doc/performance.md` lists 37 as the 1080p59.94 single-port
maximum for CVL 100 G; 37 does not fit the model above, so either that figure predates a geometry
change, uses a larger UDP payload, or was obtained without narrow pacing. Not resolved here.
Second, the levers are all per-packet overhead and none is attractive: the UDP payload is capped at
1352 B by `MTL_PKT_MAX_RTP_BYTES` (`include/mtl_api.h`), which deliberately leaves ~100 B of the
1500 B MTU unused; raising it via `mtl_init_params.pkt_udp_suggest_max_size` recovers ~0.5 %, well
under one session, and MTU 9000 would gain ~5 % but breaks ST 2110 interoperability.

---

## 9. Implementation

Scope is the acceptance measurement oracle only. No library change: the data plane behaved
correctly throughout, and all three defects are in how the test reads MTL's output.

This does edit `mtl_engine/`, which the acceptance instructions forbid doing "to make a test pass".
That rule is about bending the harness around a failing case; here the harness *is* the defect —
the numbers it published did not describe the run it measured — so fixing it is the only correct
place to fix anything. No test outcome was chased in the permissive direction: the one verdict that
moves, moves to *fail* — `test_tx[single_core]` 31 → 0/31 (§7) — and three iterations lose a rate
line (below).

| # | Change | Files | State |
|---|---|---|---|
| 1 | Average rate metrics over the FPS steady window (§6) | `mtl_engine/performance_monitoring.py`, `tests/dual/performance/test_vf_perf_dualhost.py` | **done** |
| 1b | Trim the companion's idle tail (§6.1) | `mtl_engine/performance_monitoring.py` | **done** |
| 1c | Identify a stat dump by its banner, not by the stamps inside it (§6.2) | `mtl_engine/performance_monitoring.py` | **done** |
| 2 | Derive the FPS bar from the paced rate, in the verdict *and* the headline (§7) | `mtl_engine/performance_monitoring.py`, `tests/dual/performance/test_vf_perf_dualhost.py` | **done** |
| 3 | Report the L1/wire rate beside L2 (§4) | `mtl_engine/performance_monitoring.py`, `common/generate_report.py` | **done** |
| 4 | Correct the hard-coded workload geometry in the report | `common/generate_report.py` | **done** |
| 5 | Regression tests over the captured dump sequences | `mtl_engine/tests/test_oracles.py` | **done** |

Change 3 exists because Q3 was asked at all: a report that prints only L2 invites comparison
against a line rate it is not measured in. It derives the wire rate from the packet counts already
on each `DEV` line, logs it beside the L2 rate, and renders it in the report's throughput cells.
The report's `DEV` regexes take the wire figure as an *optional* group, so a log from before this
change still parses and simply shows no wire column — verified by regenerating the baseline run's
report from its own artifact. The figure is deliberately a rate, not a "% of line rate": the
report does not know the port's speed and should not hardcode 100 G.

Change 4 corrects every numerically wrong row in the report's "Workload Description" table
against §4 — payload per packet (was "~1,200–1,314 bytes", is 1320), packets per frame (was
"~3,945", is 3928), Ethernet frame size (the SRD term was right only for the 2946 single-SRD
packets, not the 981 dual-SRD ones), bandwidth per session (was "~2,589 Mb/s", is 2605 L2 /
2651 wire) — plus the pass-criteria row, which stated the 58.41 fps bar Defect 2 produced, and
the measurement-window row, which named the warmup Change 1 removes.

Known gap, not addressed here: no workflow runs `tests/acceptance/mtl_engine/tests/`, so
Change 5's tests are local-only. Wiring a python unit tier into CI is a separate change.

### Two consequences of Change 1 worth expecting

* **Three already-failing iterations lose their rate line entirely.** `_steady_window()` rejects
  them with "no dump had every session live" — at those session counts no single stat dump ever
  had all sessions running, which is the failure itself. The rate metric now reports nothing
  rather than a number averaged over dumps that were never comparable. This is correct, and it
  means an absent rate in the report is a signal, not a parsing gap.
* **Redundant MC at 36 sessions has thin margin.** It measures 59.67–59.84 fps against the
  corrected 59.34 bar, so it may flip between runs. Deliberately not loosened: loosening the
  tolerance is exactly the mistake Defect 2 was.

## 10. Validation

| Step | State |
|---|---|
| Regression tests fail without the fix (mutation-checked) | **done** — 10 new tests (14 in the file), 10 mutations, none survived |
| `isort` / `black` / `flake8` / `ruff` clean on the four changed files | **done** |
| Replay every captured app run through the fixed production functions | **done** — 78 runs reconstructed from the 8 sweep logs |
| Replay HEAD's stamp-keyed census over the same runs (§6.2) | **done** — 48 of 75 windows one dump too long; 1 pass count moves, same verdict |
| Re-run the 36-session TX MC iteration on mtl-runner-7/-8 | **done** — twice; both sides correct after §6.1 |
| Re-run the published 31-session single-core TX iteration on mtl-runner-7 | **done** — fails, as §7 predicts |

Mutations used, each run against the whole new suite to confirm which tests it kills:

| # | Mutation | Killed |
|---|---|---|
| 1 | `_collect_in_window()` ignores the window (the pre-fix behaviour) | 6 |
| 2 | `ETH_L1_OVERHEAD_BYTES = 0` | 1 — `test_dev_rate_reports_the_wire_rate_beside_the_l2_counter` |
| 3 | `paced_fps()` returns the format token | 2 — `test_fps_bar_is_the_paced_rate_not_the_format_token`, `…_wire_rate…` |
| 4 | `companion_steady_window()` returns `_steady_window()`'s window unmodified (pre-§6.1) | 1 — `test_companion_window_stops_where_the_far_end_stopped_sending` |
| 5 | `companion_steady_window()` drops *every* idle dump, not just a trailing run | 2 — both companion tests |
| 6 | `FPS_MIN_STEADY_SAMPLES = 3` | 1 — `test_a_three_dump_window_is_refused_rather_than_judged` |
| 7 | dumps delimited by the closing banner instead of the opening one (§6.2) | 9 |
| 8 | the dump ordinal never advances, so every line falls in dump 0 | 6 |
| 9 | dumps grouped by timestamp — the §6.2 defect, reintroduced | 3 — both straddle tests, `test_stats_printed_as_sessions_are_freed_are_not_a_dump` |
| 10 | `DEV` coverage counted per matched line rather than per window dump | 1 — `test_a_redundant_dump_straddling_a_second_reads_as_one_dump` |

No mutation survived. Several kill far more than their own test, which is the point: 1, 7 and 8 take
out most of the new tests because dump identity and the window filter are what every rate figure
rests on, and 3 takes out the wire-rate test too because that fixture's packet counts derive from
`paced_fps(59)`.

The mutations are chosen in groups on purpose:

* 4 and 5 — the companion trim is load-bearing (4) and not over-eager (5). 5 is why
  `test_companion_window_keeps_a_log_that_ends_mid_traffic` is not a vacuous guard: it dies the
  moment the trim starts removing interior samples.
* 7 and 8 — the delimiter must be the *opening* banner (7: the closing one puts every line in the
  preceding dump, shifting the whole census by one) and the ordinal must actually distinguish dumps
  (8: collapsing every line into dump 0 makes the window meaningless while still "matching").
* 9 alone reintroduces the §6.2 defect, and the three tests it kills are not paraphrases of each
  other: one dies on a dump straddling a second, one on a two-port dump doing the same, one on
  free-time session lines reading as one more full dump.
* 10 alone pins that coverage is counted per window dump. It is why `_collect_in_window()` yields
  the dump alongside each match instead of just the match; without a test that dies for it, that
  indirection would have been unjustified sophistication and belonged out of the patch.
* 6 alone pins the sample floor, which is what keeps a window from being judged on too few dumps.

One trap worth recording, because it once let a mutation here survive: in
this suite **`caplog.text` does not contain message bodies.** The `pytest_mfd_logging` format string
omits `%(message)s`, so `caplog.text` renders as timestamp/logger/level only and any
`assert "…" not in caplog.text` is vacuously true. Assert over `caplog.messages` or
`caplog.records` instead.

### Adjacent finding, deliberately not changed

`_monitor_fps_generic()` trims the worst samples with
`n_drop = max(1, int(len(hist) * max_drop_pct))`, so it always drops at least one. At the
`FPS_MIN_STEADY_SAMPLES = 4` floor a 10 % setting therefore discards **25 %** of the evidence, and
the sample it discards is the worst one: a session that transported nothing for one whole 10 s
period still passes, judged on the other three. The effective tolerance is wider than 10 %
anywhere below 10 dumps.

This is left as is. Of the baseline run's 75 judged windows, 64 were 9 dumps and 9 were 10, where
the trim is about the intended 10 % (the other two, of 8 and 6 dumps, dropped one in eight and one
in six); narrowing it would move verdicts on real runs, which is outside the scope of a measurement
fix. `test_trimmed_mean_discards_a_quarter_of_a_minimum_window` pins both halves of the behaviour
(untrimmed → 1/2 sessions pass, trimmed → 2/2) so the next person to widen `MAX_DROP_PCT` or lower
the floor meets the interaction first.

The replay imports the *production* functions and re-runs them over the baseline run's captured
logs. Scope, stated precisely because it is easy to overstate: the 8 sweep logs were split at each
`mtl_uninit` into **78 individual app runs** — every binary-search probe and every Phase-2 verify,
not just the winning iteration of each case. Of those, 75 yield a steady window with a `DEV` sample
for every dump in it, and 3 are rejected with the reason now logged —
`no dump had every session live`, which is the correct verdict for a probe whose RX side never
brought all its sessions up. **The new coverage warning is silent on all 75.** Matched by stamp
instead (§6.2), 2 of them would lose samples: the 16-session redundant single-core RX probe all 10
dumps of its stamp-keyed window, so it would publish no `DEV` rate, and the 48-session TX multi-core
probe 2 of 9 — the probe whose window reads 98 111.70 Mb/s, against §4.1's independently tabulated
98 110.

Results: every TX rate corrected (the 36-session MC winner 65 149 → 93 761 Mb/s L2 / 95 387 Mb/s
wire, and the packet-derived wire rate agrees with §4's independent geometry model — 95 387 vs
95 423, 0.04 %); **every RX rate within −0.35 % … +0.07 % of HEAD's**, median −0.00 %, which is the
no-upward-bias property of §6 measured rather than argued. Verdict changes are confined to
`test_tx[single_core]` 31 → 0/31 so the search lands on 30, plus two Phase-2 VERIFY downgrades that
are log-only.

### Hardware re-run

The 36-session TX MC iteration was re-run twice in fixed mode on mtl-runner-7 (DUT) against
mtl-runner-8, with the changed files overlaid on the runner's own checkout of this branch's base
commit. The first run reported **93 760.21 Mb/s L2 / 95 386.99 Mb/s wire** over a 9-dump window and
passed in 278.84 s — the §6 prediction reproduced on hardware, 65 149 → 93 760. It is quoted here as
figures rather than as a log block because the perf runner's workspace is shared and a later job's
checkout has since removed that log; the numbers below are from the surviving one.

The first run is also what exposed §6.1: its companion reported 73 464.57 Mb/s where it had received
93.8 Gb/s. After §6.1 the iteration was re-run, same command, both sides now reading from their own
steady windows (`~/.perf-fix-backup/run36-companion.log` on mtl-runner-7; the two DEV blocks are
labelled here because the log prints one per host and they are otherwise indistinguishable):

```text
TX Results: 36/36 sessions at 59.00 fps (min: 59.34)
  Steady window: 9 stat dumps, 2026-09-23 08:37:02 … 2026-09-23 08:38:22
  measured  DEV TX: 93759.50 Mb/s wire 95386.27 Mb/s (9 samples)
  companion DEV RX: 92750.63 Mb/s wire 94359.89 Mb/s (8 samples)
1 passed in 278.20s (0:04:38)
```

Three things to read off it. The bar is now 59.34, not 58.41, and 36 sessions still clear it — the
headline's `at 59.00` is the report-side half of Defect 2, fixed after this run (§7). The measured
side reproduces to 0.01 % across the two runs (93 760.21 → 93 759.50). And the companion is
22 % → **1.08 %** from the measured side.

The wire rate to compare against §4's independently derived 95 423 Mb/s for 36 sessions is 95 386,
which agrees to 0.04 %.

That residual is real loss, not a residual artefact of the metric. Summing both hosts' packet
counters over the whole run, the companion received 1 012 393 257 of the 1 025 242 084 packets the
DUT sent — **1.25 % lost on receive**, and the per-packet byte figure the two hosts report is
identical, so it is packets missing rather than bytes counted differently. The two windows also
cover different 80 s spans of the run in each host's own clock, which is why 1.08 % and 1.25 %
differ. Receive-side loss at this occupancy does not move the TX verdict — the criterion is the
transmitter's frame rate — but it is worth knowing that a 36-session receiver on this hardware is
not lossless.

Run the regression tests with:

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest mtl_engine/tests/test_oracles.py \
  --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -q
```

They need no NIC, no hugepages and no media, but both config flags are still required — the local
`conftest.py` registers `pytest_mfd_config`, which fails collection without them.
