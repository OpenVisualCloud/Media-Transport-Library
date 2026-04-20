# Session Statistics Guide

How to interpret the statistics counters exposed by MTL session APIs. For the full list of
fields and their descriptions, see the doxygen comments in the header files (`st_api.h`,
`st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`).

## Table of Contents

- [Overview](#overview)
- [RX Packet Processing Pipeline](#rx-packet-processing-pipeline)
- [Understanding OOO and Loss Counters](#understanding-ooo-and-loss-counters)
- [Cross-Session Differences](#cross-session-differences)
- [Periodic Stat Log Output](#periodic-stat-log-output)
- [Troubleshooting with Stats](#troubleshooting-with-stats)

## Overview

Each RX/TX session exposes statistics via a pair of functions:

- `st*_get_session_stats()` — copies the current counters to a user-provided struct
- `st*_reset_session_stats()` — zeros all counters

All counters are `uint64_t` and monotonically increase until reset. The library never
decreases a counter.

### Thread Safety

Both `get` and `reset` acquire the per-session spinlock internally, making them safe to
call from any thread while the session is active.

**Data-path impact**: the data-path tasklet holds the same spinlock during packet
processing. A concurrent `get` or `reset` may cause the tasklet to skip one poll cycle
(trylock failure). The lock is held only for a struct memcpy/memset (~200-400 bytes, tens
of nanoseconds), which is negligible compared to the microsecond-scale poll interval.

**Recommended pattern**: call `get` followed by `reset` for interval-based monitoring.

| Session type | Get stats | Reset stats |
|---|---|---|
| Video TX (ST20) | `st20_tx_get_session_stats()` | `st20_tx_reset_session_stats()` |
| Video RX (ST20) | `st20_rx_get_session_stats()` | `st20_rx_reset_session_stats()` |
| Audio TX (ST30) | `st30_tx_get_session_stats()` | `st30_tx_reset_session_stats()` |
| Audio RX (ST30) | `st30_rx_get_session_stats()` | `st30_rx_reset_session_stats()` |
| Ancillary TX (ST40) | `st40_tx_get_session_stats()` | `st40_tx_reset_session_stats()` |
| Ancillary RX (ST40) | `st40_rx_get_session_stats()` | `st40_rx_reset_session_stats()` |
| Metadata TX (ST41) | `st41_tx_get_session_stats()` | `st41_tx_reset_session_stats()` |
| Metadata RX (ST41) | `st41_rx_get_session_stats()` | `st41_rx_reset_session_stats()` |

## RX Packet Processing Pipeline

Every incoming packet goes through the following stages. Understanding this order is key to
interpreting which counters are "pre-redundancy" (counted early) vs "post-redundancy"
(counted after merging redundant streams).

```text
NIC queue
  │
  ├─1. Early validation (payload type, SSRC, packet length, interlace F-bits)
  │     └── FAIL → err_packets++
  │                 stat_pkts_wrong_pt_dropped++ or stat_pkts_wrong_ssrc_dropped++
  │                 (packet discarded)
  │
  ├─2. Per-port sequence check
  │     └── GAP  → port[].lost_packets += gap_size
  │                 stat_lost_packets += gap_size
  │
  ├─3. port[].packets++, port[].bytes++          ← "pre-redundancy" counters
  │
  ├─4. Redundancy filter (is this packet's timestamp already seen?)
  │     └── STALE → stat_pkts_redundant++
  │                  (packet discarded)
  │
  ├─5. Post-redundancy loss detection
  │     Audio/anc/fmd: session_seq_id gap → stat_pkts_unrecovered += gap_size
  │     Video: frame completion check     → stat_pkts_unrecovered += (total - received)
  │
  ├─6. stat_pkts_received++                      ← "post-redundancy" counter
  │
  └─7. Deliver to frame buffer / RTP ring
```

**Key insight**: `port[].packets` counts everything that passes validation (step 3),
including packets that the redundancy filter will later discard (step 4).
`stat_pkts_received` counts only the packets that make it through all filters (step 6).

**Video note**: Video (ST20) performs step 5 at frame completion time rather than per-packet.
It compares expected total packets to received packets and adds the difference to
`stat_pkts_unrecovered`. See [Video-Specific OOO Behavior](#video-specific-ooo-behavior).

### `port[].frames` Semantics

The `frames` counter means different things depending on the session type:

| Session type | What `frames` counts | When it increments |
|---|---|---|
| Video (ST20) | Frame completions | When a frame is notified to app (complete or incomplete) |
| Audio (ST30) | Frame completions | When a frame buffer is filled and delivered to app |
| Ancillary (ST40) | New RTP timestamps | When a new timestamp is first seen (frame **start**, not completion) |
| Metadata (ST41) | New RTP timestamps | When a new timestamp is first seen (frame **start**, not completion) |

**Example**: If an ancillary stream sends 100 frames and you see `port[0].frames = 100`,
that means 100 distinct timestamps were detected — but some of those frames could be
incomplete if packets were lost.

## Understanding OOO and Loss Counters

The library exposes a **uniform set of RX counters across all session types** so a
single frontend can render the same KPIs for ST20 / ST30 / ST40 / FMD without
branching on session type.  Loss is split from "wire weirdness" so you can tell
"network dropped a packet" apart from "a switch duplicated/reordered a packet".

### The Counters

| Counter | Scope | Stage | What it detects |
|---|---|---|---|
| `port[i].lost_packets` | Per-port | Pre-redundancy | Forward gap on one port (gap size, not gap count) |
| `port[i].reordered_packets` | Per-port | Pre-redundancy | Backward arrival on the same port (intra-port reorder) |
| `port[i].duplicates_same_port` | Per-port | Pre-redundancy | Same seq seen twice on the same port (audio/anc/fmd only — see below) |
| `stat_lost_packets` | Session | Pre-redundancy | Sum of per-port `lost_packets` |
| `stat_pkts_redundant` | Session | Post-redundancy | Packet discarded because the redundant port already delivered it |
| `stat_pkts_unrecovered` | Session | Post-redundancy | Packets missing on *both* ports — real data loss |

**Invariants** (always true):

```text
stat_lost_packets == port[0].lost_packets + port[1].lost_packets
stat_pkts_unrecovered <= stat_lost_packets
```

**Reorder vs duplicate — what each counter means**:

- `port[].reordered_packets` is bumped when a packet arrives "behind" the highest
  position already accepted on this port. For audio/anc/fmd that means a backward RTP
  sequence. For video (ST20/ST22) that means an intra-frame `pkt_idx` smaller than
  `last_pkt_idx`. ST20 has no per-port `latest_seq_id`, so cross-frame reorders are
  not tracked — only intra-frame ones.
- `port[].duplicates_same_port` is bumped when the **same** seq is seen twice on the
  **same** port. Tracked for audio/anc/fmd. Always 0 for ST20/ST22 because the
  slot+bitmap completion model cannot distinguish a same-port duplicate from a
  normal cross-port redundant copy — both surface as `stat_pkts_redundant`.

**Distinguishing duplicates**: `stat_pkts_redundant` counts the *expected* cross-port
duplicate (same packet delivered on both P and R, filtered post-redundancy).
`port[].duplicates_same_port` counts the *unexpected* case where the **same port** sees
the same seq twice — strongly suggests a switch loop, cable fault, LAG misconfig,
or a tcpreplay loop upstream of MTL.

### Interpreting OOO Scenarios

| Scenario | `port[P].lost` | `port[R].lost` | `stat_lost_packets` | `stat_pkts_unrecovered` | Interpretation |
|---|---:|---:|---:|---:|---|
| Perfect stream | 0 | 0 | 0 | 0 | All good |
| Port P lossy, R covers | 50 | 0 | 50 | 0 | Redundancy working |
| Both ports lossy, some overlap | 50 | 30 | 80 | 5 | 5 packets unrecoverable — real data loss |
| Single port, some loss | 10 | n/a | 10 | 10 | No redundancy to help |
| Same-port duplicate (switch loop) | see `duplicates_same_port` | — | 0 | 0 | Not loss, but a network-fabric anomaly (audio/anc/fmd only) |
| Intra-port reorder | see `reordered_packets` | — | may be 0 or >0 | 0 | Forward gap counted when the "high" seq arrives first, then the late packet lands in `reordered_packets` |

Key takeaways:

- `stat_lost_packets > 0` with `stat_pkts_unrecovered == 0` → redundancy is covering the
  gaps, the stream is healthy at session level.
- `stat_pkts_unrecovered > 0` → real data loss that redundancy could not cover. Check
  network path diversity.
- With a single port, `stat_pkts_unrecovered` equals `stat_lost_packets` because there is
  no redundant port to fill gaps.
- Redundancy save rate: `save_rate = lost_packets / (lost_packets + unrecovered)` — the
  fraction of per-port gaps that redundancy covered.

### Same-Port Duplicates vs Cross-Port Redundant

Two very different "I saw that packet already" conditions:

| Condition | Counter | Normal? | Cause |
|---|---|---|---|
| Same packet delivered on both P and R | `stat_pkts_redundant` | **Yes**, expected on 2-port sessions | Sender transmits on both ports; RX filters one copy |
| Same packet delivered **twice on the same port** | `port[].duplicates_same_port` (audio/anc/fmd) | **No** | Switch loop, cable fault, LAG misconfiguration, tcpreplay loop |

A non-zero `duplicates_same_port` (or `reordered_packets`) means a fabric problem
upstream of MTL — MTL correctly absorbs the anomaly but you should investigate the
switch path.

For video (ST20/ST22), `port[].duplicates_same_port` stays 0 because the
slot+bitmap completion model cannot distinguish a same-port duplicate from a
normal cross-port redundant copy — both surface as `stat_pkts_redundant`.
Intra-frame reorders **are** tracked in `port[].reordered_packets`.

### Video-Specific OOO Behavior

Video (ST20) works differently from audio/anc/fmd:

- Video detects OOO using **frame-internal `pkt_idx`** (packet index within the current
  frame), not RTP sequence numbers
- When a packet arrives with `pkt_idx != last_pkt_idx + 1`, the gap size
  (`pkt_idx - last_pkt_idx - 1`) is added to both `port[].lost_packets`
  and `stat_lost_packets`
- Video has **no `session_seq_id`** — frames are identified by RTP timestamp, and each
  frame resets the packet index tracking
- `stat_pkts_unrecovered` for video counts missing packets in corrupted frames
  (estimated from `(frame_size - recv_size) / avg_pkt_size`)

**Practical implication**: For video, `stat_pkts_unrecovered` and `stat_frames_dropped`
are the primary loss indicators. `common.port[].incomplete_frames` shows per-port
contribution.

---

## Cross-Session Differences

### OOO Counters at a Glance

| Counter | Scope | Stage | Video (ST20) | Audio/Anc/FMD |
|---|---|---|---|---|
| `port[].lost_packets` | Per-port | Pre-redundancy | Missing pkts from `pkt_idx` gaps | Missing pkts from RTP `seq_number` gaps |
| `port[].reordered_packets` | Per-port | Pre-redundancy | Intra-frame `pkt_idx < last_pkt_idx` (no cross-frame tracking) | Backward RTP seq on same port |
| `port[].duplicates_same_port` | Per-port | Pre-redundancy | **Always 0** (same-port dup indistinguishable from cross-port redundant copy, see `stat_pkts_redundant`) | Same RTP seq seen twice on same port |
| `stat_lost_packets` | Session | Pre-redundancy | Sum of port missing pkts | Sum of port missing pkts |
| `stat_pkts_unrecovered` | Session | Post-redundancy | Missing pkts in corrupted frames | Missing pkts from `session_seq_id` gaps |

The public schema is identical for every session type: a frontend that reads
`port[].{packets, bytes, frames, err_packets, lost_packets, reordered_packets, duplicates_same_port}`
and `{stat_pkts_received, stat_pkts_redundant, stat_lost_packets, stat_pkts_unrecovered}`
works the same way for ST20, ST30, ST40, and FMD — with the documented caveat that
`duplicates_same_port` is always 0 for video.

Video (ST20/ST22) uses a slot+bitmap completion model that does not maintain a
per-port `latest_seq_id`. Same-port duplicates within a frame are therefore
indistinguishable from cross-port redundant copies and surface as
`stat_pkts_redundant` (`pkt_idx` already set in the bitmap) or
`stat_pkts_idx_oo_bitmap` (`pkt_idx` out of range). Intra-frame reorders
(a not-yet-seen `pkt_idx` arriving below `last_pkt_idx`) are tracked in
`port[].reordered_packets`.

### How `stat_pkts_unrecovered` Is Computed

| Session type | Source | Computation | Exact? |
|---|---|---|---|
| Video (ST20) | Corrupted frame at slot completion | `(frame_size - recv_size) / avg_pkt_size` | Estimated |
| Video (ST22) | Corrupted frame at slot completion | `(expect_size - recv_size) / avg_pkt_size` | Estimated |
| Audio (ST30) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |
| Ancillary (ST40) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |
| Metadata (ST41) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |

---

## Periodic Stat Log Output

Each RX session logs a delta-based status snapshot at the library stat interval
(controlled by `stat_dump_period_s`, default 10s). The format is uniform across ST20,
ST30, ST40, and FMD. Only lines whose counter delta is non-zero are emitted.

### Line Forms

```text
RX_<TYPE>_SESSION(<idx>): fps <f> frames <n> pkts <N> [(redundant <R>)]
RX_<TYPE>_SESSION(<idx>): port stats P=<N> pkts/<F> frames, R=<N> pkts/<F> frames
RX_<TYPE>_SESSION(<idx>): per-port loss covered by redundancy: <L> of <T> pkts (P:<d_p>=<pct>%, R:<d_r>=<pct>%, save_rate=<z>%)
RX_<TYPE>_SESSION(<idx>): per-port lost pkts <L>                                      (single-port form)
RX_<TYPE>_SESSION(<idx>): unrecovered pkts (lost on all ports) <U>                    (err() level)
```

### Log Level by Severity

| Condition | Level | Why |
|---|---|---|
| `per-port lost pkts` with redundancy covering | `warn()` | Network degradation signal; session still healthy |
| `per-port lost pkts` on single-port session | `warn()` | Real data loss (no redundancy to help) |
| `unrecovered pkts (lost on all ports)` | `err()` | Real data loss even with redundancy |
| Wrong PT / SSRC / len dropped | `notice()` | Configuration mismatch, filtered by MTL |

### `save_rate` Formula

```text
save_rate = 100 * lost_packets / (lost_packets + unrecovered)
```

- `100%` → every per-port loss was covered by the redundant port.
- `< 100%` → some packets were missing on **both** ports.
- `0%` → no redundancy benefit for this interval (single-port or both ports lost the same
  packets).

---

## Troubleshooting with Stats

| Symptom | Counter to check | Likely cause |
|---|---|---|
| No packets at all | `port[0].packets == 0` | Wrong IP/port, multicast not joined, NIC link down |
| Packets arrive but `stat_pkts_received` is 0 | `stat_pkts_wrong_pt_dropped`, `stat_pkts_wrong_ssrc_dropped`, `port[].err_packets` | PT or SSRC mismatch between sender and receiver |
| `stat_lost_packets` increasing | `port[P].lost_packets`, `port[R].lost_packets` | Network congestion, packet loss, or RSS misconfiguration |
| `stat_pkts_unrecovered` increasing | Compare with `stat_lost_packets` | Real data loss — redundancy could not cover; check path diversity |
| `port[].duplicates_same_port > 0` (audio/anc/fmd) | Upstream switch / path | Switch loop, cable fault, LAG misconfig, or tcpreplay loop |
| `port[].reordered_packets > 0` | Upstream switch / path | ECMP/QoS reorder; for ST20 limited to intra-frame reorders |
| `stat_pkts_redundant` is 0 on 2-port session | `port[1].packets` | Redundant port not receiving data |
| `stat_pkts_redundant ≈ stat_pkts_received` on 2-port session | — | Normal — each packet arrives on both ports, one copy filtered |
| Video `stat_frames_dropped` increasing | `stat_pkts_unrecovered`, `stat_pkts_no_slot` | Incomplete frames due to packet loss or frame buffers exhausted |
| TX `stat_epoch_drop` increasing | `stat_epoch_onward` | Application providing frames too late; callback blocking |
| `stat_slot_get_frame_fail` / `stat_pkts_enqueue_fail` increasing | — | Receiver too slow: return frame buffers faster or increase ring size |
