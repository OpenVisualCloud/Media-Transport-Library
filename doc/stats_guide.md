# Session Statistics Guide

How to interpret the statistics counters exposed by MTL session APIs. For the full list of
fields and their descriptions, see the doxygen comments in the header files (`st_api.h`,
`st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`).

## Table of Contents

- [Overview](#overview)
- [RX Packet Processing Pipeline](#rx-packet-processing-pipeline)
- [Understanding OOO and Loss Counters](#understanding-ooo-and-loss-counters)
- [Cross-Session Differences](#cross-session-differences)
- [Troubleshooting with Stats](#troubleshooting-with-stats)

## Overview

Each RX/TX session exposes statistics via a pair of functions:

- `st*_get_session_stats()` — copies the current counters to a user-provided struct
- `st*_reset_session_stats()` — zeros all counters

All counters are `uint64_t` and monotonically increase until reset. The library never
decreases a counter.

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

```
NIC queue
  │
  ├─① Early validation (payload type, SSRC, packet length, interlace F-bits)
  │     └── FAIL → err_packets++
  │                 stat_pkts_wrong_pt_dropped++ or stat_pkts_wrong_ssrc_dropped++
  │                 (packet discarded)
  │
  ├─② Per-port sequence check
  │     └── GAP  → port[].out_of_order_packets++
  │                 stat_pkts_out_of_order++
  │
  ├─③ port[].packets++, port[].bytes++          ← "pre-redundancy" counters
  │
  ├─④ Redundancy filter (is this packet's timestamp already seen?)
  │     └── STALE → stat_pkts_redundant++
  │                  (packet discarded)
  │
  ├─⑤ Post-redundancy loss detection
  │     Audio/anc/fmd: session_seq_id gap → stat_pkts_unrecovered += gap_size
  │     Video: frame completion check     → stat_pkts_unrecovered += (total - received)
  │
  ├─⑥ stat_pkts_received++                      ← "post-redundancy" counter
  │
  └─⑦ Deliver to frame buffer / RTP ring
```

**Key insight**: `port[].packets` counts everything that passes validation (step ③),
including packets that the redundancy filter will later discard (step ④).
`stat_pkts_received` counts only the packets that make it through all filters (step ⑥).

**Video note**: Video (ST20) performs step ⑤ at frame completion time rather than per-packet.
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

The library provides three levels of out-of-order / loss tracking:

### The Three Counters

| Counter | Scope | Stage | What it detects |
|---|---|---|---|
| `port[i].out_of_order_packets` | Per-port | Pre-redundancy | Gaps in RTP sequence on a single port |
| `stat_pkts_out_of_order` | Session | Pre-redundancy | Sum of all per-port gaps |
| `stat_pkts_unrecovered` | Session | Post-redundancy | Actual missing packets that redundancy could not cover |

**Invariants** (always true):

```
stat_pkts_out_of_order == port[0].out_of_order_packets + port[1].out_of_order_packets
stat_pkts_unrecovered <= stat_pkts_out_of_order
```

### Interpreting OOO Scenarios

| Scenario | `port[P].ooo` | `port[R].ooo` | `stat_pkts_out_of_order` | `stat_pkts_unrecovered` | Interpretation |
|---|---:|---:|---:|---:|---|
| Perfect stream | 0 | 0 | 0 | 0 | All good |
| Port P lossy, R covers | 50 | 0 | 50 | 0 | Redundancy working |
| Both ports lossy, some gaps | 50 | 30 | 80 | 5 | 5 uncoverable gaps — real data loss |
| Single port, some loss | 10 | n/a | 10 | 10 | No redundancy to help |
| Network reordering | 3 | 2 | 5 | 0 | Packets reordered but none lost |

Key takeaways:
- `stat_pkts_out_of_order > 0` with `stat_pkts_unrecovered == 0` → redundancy is
  covering the gaps, the stream is healthy at session level.
- `stat_pkts_unrecovered > 0` → real data loss that redundancy could not cover. Check
  network path diversity.
- With a single port, `stat_pkts_unrecovered` always equals `stat_pkts_out_of_order`
  because there is no redundant port to fill gaps.

### Video-Specific OOO Behavior

Video (ST20) works differently from audio/anc/fmd:

- Video detects OOO using **frame-internal `pkt_idx`** (packet index within the current
  frame), not RTP sequence numbers
- When a packet arrives with `pkt_idx != last_pkt_idx + 1`, both
  `port[].out_of_order_packets` and `stat_pkts_out_of_order` are incremented together
- Video has **no `session_seq_id`** — frames are identified by RTP timestamp, and each
  frame resets the packet index tracking
- `stat_pkts_unrecovered` for video counts exact missing packets in corrupted frames
  (`st20_total_pkts - pkts_received`)

**Practical implication**: For video, `stat_pkts_unrecovered` and `stat_frames_dropped`
are the primary loss indicators. `common.port[].incomplete_frames` shows per-port
contribution.

---

## Cross-Session Differences

### Redundancy Counter Names

Different session types use different counter names for the redundancy concept:

| Session | Counter name | Detection mechanism |
|---|---|---|
| Video (ST20) | `stat_pkts_redundant_dropped` | Same-frame packet already seen at slot level |
| Audio (ST30) | `stat_pkts_redundant` | Stale RTP timestamp |
| Ancillary (ST40) | `stat_pkts_redundant` | Stale timestamp or stale seq_id |
| Metadata (ST41) | `stat_pkts_redundant` | Stale timestamp or stale seq_id |

### OOO Counters at a Glance

| Counter | Scope | Stage | Video (ST20) | Audio/Anc/FMD |
|---|---|---|---|---|
| `port[].out_of_order_packets` | Per-port | Pre-redundancy | `pkt_idx` gap in frame | RTP `seq_number` gap |
| `stat_pkts_out_of_order` | Session | Pre-redundancy | Sum of port OOO | Sum of port OOO |
| `stat_pkts_unrecovered` | Session | Post-redundancy | Missing pkts in corrupted frames | Missing pkts from `session_seq_id` gaps |

### How `stat_pkts_unrecovered` Is Computed

| Session type | Source | Computation | Exact? |
|---|---|---|---|
| Video (ST20) | Corrupted frame at slot completion | `(frame_size - recv_size) / avg_pkt_size` | Estimated |
| Video (ST22) | Corrupted frame at slot completion | `(expect_size - recv_size) / avg_pkt_size` | Estimated |
| Audio (ST30) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |
| Ancillary (ST40) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |
| Metadata (ST41) | Post-redundancy `session_seq_id` gap | `(uint16_t)(seq_id - session_seq_id - 1)` per gap | Exact |

---

## Troubleshooting with Stats

| Symptom | Counter to check | Likely cause |
|---|---|---|
| No packets at all | `port[0].packets == 0` | Wrong IP/port, multicast not joined, NIC link down |
| Packets arrive but `stat_pkts_received` is 0 | `stat_pkts_wrong_pt_dropped`, `stat_pkts_wrong_ssrc_dropped`, `port[].err_packets` | PT or SSRC mismatch between sender and receiver |
| `stat_pkts_out_of_order` increasing | `port[P].out_of_order_packets`, `port[R].out_of_order_packets` | Network congestion, packet loss, or RSS misconfiguration |
| `stat_pkts_unrecovered` increasing | Compare with `stat_pkts_out_of_order` | Real data loss — redundancy could not cover; check path diversity |
| `stat_pkts_redundant` is 0 on 2-port session | `port[1].packets` | Redundant port not receiving data |
| `stat_pkts_redundant ≈ stat_pkts_received` on 2-port session | — | Normal — each packet arrives on both ports, one copy filtered |
| Video `stat_frames_dropped` increasing | `stat_pkts_unrecovered`, `stat_pkts_no_slot` | Incomplete frames due to packet loss or frame buffers exhausted |
| TX `stat_epoch_drop` increasing | `stat_epoch_onward` | Application providing frames too late; callback blocking |
| `stat_slot_get_frame_fail` / `stat_pkts_enqueue_fail` increasing | — | Receiver too slow: return frame buffers faster or increase ring size |
