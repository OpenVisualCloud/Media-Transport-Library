# Session Statistics Guide

How to read MTL per-session counters. Field-level docs live in the headers
(`st_api.h`, `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`).

## API

```c
st<NN>_<rx|tx>_get_session_stats(handle, &stats);   /* snapshot */
st<NN>_<rx|tx>_reset_session_stats(handle);         /* zero    */
```

`<NN>` ∈ `20` video, `30` audio, `40` ancillary, `41` fast metadata.
All counters are `uint64_t`, monotonic, thread-safe (per-session spinlock).

## RX packet processing pipeline

```text
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  NIC queue                                                                  │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  1. VALIDATE  (payload type, SSRC, packet length, interlace F-bits)         │
 │                                                                             │
 │     FAIL ──► port[i].err_packets++                                          │
 │              stat_pkts_wrong_{pt,ssrc,len,interlace}_dropped++  (video)     │
 │              stat_pkts_wrong_{pt,ssrc,interlace}_dropped++      (anc/fmd)   │
 │              stat_pkts_{wrong_pt,wrong_ssrc,len_mismatch}_dropped++ (audio) │
 │              packet DISCARDED                                               │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │ pass
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  2. PER-PORT SEQUENCE CHECK                                                 │
 │                                                                             │
 │     forward gap  ──► port[i].lost_packets += gap_size                       │
 │                      stat_lost_packets    += gap_size                       │
 │     backward seq ──► port[i].reordered_packets++                            │
 │     same seq     ──► port[i].duplicates_same_port++  (audio/anc/fmd only)   │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  3. PRE-REDUNDANCY TOTALS                                                   │
 │                                                                             │
 │     port[i].packets++          ◄── "what arrived on this wire"              │
 │     port[i].bytes += pkt_len                                                │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  4. REDUNDANCY FILTER  (timestamp/seq already seen on the other port?)      │
 │                                                                             │
 │     YES ──► stat_pkts_redundant++   (expected on 2-port sessions)           │
 │             packet DISCARDED                                                │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │ unique
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  5. POST-REDUNDANCY LOSS DETECTION                                          │
 │                                                                             │
 │     Audio/Anc/FMD:  session_seq_id gap ──► stat_pkts_unrecovered += gap     │
 │     Video:          frame completion   ──► stat_pkts_unrecovered += missing  │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  6. POST-REDUNDANCY TOTAL                                                   │
 │                                                                             │
 │     stat_pkts_received++       ◄── "what the app actually got"              │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  7. DELIVER to frame buffer / RTP ring                                      │
 └──────────────────────────────────────────────────────────────────────────────┘
```

`port[i].packets` = what arrived on wire `i`. `stat_pkts_received` = what the app got.

## The five counters you monitor

| Counter | Meaning |
|---|---|
| `port[i].lost_packets` | Packets missing on **this** port (pre-redundancy) |
| `port[i].reordered_packets` | Backward arrival on the same port |
| `port[i].duplicates_same_port` | Same seq twice on the same port (audio/anc/fmd; always 0 for video) |
| `stat_pkts_redundant` | Cross-port duplicate filtered — **expected** on 2-port sessions |
| `stat_pkts_unrecovered` | Missing on **all** ports — real loss |

```text
stat_lost_packets     == Σ port[i].lost_packets
stat_pkts_unrecovered <= stat_lost_packets
save_rate (%)         == 100 * lost / (lost + unrecovered)
```

`save_rate` answers: **"is redundancy covering the gaps?"** 100% = yes.

### Reading table

| `port[P].lost` | `port[R].lost` | `unrec` | `redundant` | Verdict |
|---:|---:|---:|---:|---|
| 0 | 0 | 0 | ≈ received | Healthy |
| 50 | 0 | 0 | ≈ received | P degraded, R covers (`save_rate=100%`) |
| 50 | 30 | 5 | < received | 5 pkts lost on both → real loss |
| 10 | n/a | 10 | 0 | Single-port, no redundancy |

## `port[i].frames` — two flavors

| Sessions | `frames++` when… | `incomplete_frames` |
|---|---|---|
| Video (ST20/ST22) | This port delivered enough pkts to **complete** the frame | Bumped when this port was short |
| Audio/Anc/FMD     | New frame's **first** packet arrived on this port (race winner) | Always 0 |

`port[0].frames + port[1].frames` ≠ total frames. Use `stat_pkts_received` and the
type-specific `stat_frames_dropped` / `incomplete_frames_cnt` for end-to-end totals.

**Video invariant** (per port): `frames + incomplete_frames == total complete frames`.

> **Example.** TX=6766, RX `port[P].frames=6465`, `port[R].frames=300`,
> `unrecovered=0`, `frames_dropped=0`. Then `port[P].incomplete_frames≈300` and
> `port[R].incomplete_frames≈6465`: redundancy is healing every frame, but **R is
> dropping ≥1 pkt per frame**. Check `mtl_get_port_stats(R).rx_hw_dropped_packets`,
> switch port stats, MTU, SFP/cable on R.

### `err_packets` — not data loss

`port[i].err_packets` = packets that arrived but the session refused (wrong PT, wrong
SSRC, length mismatch, etc.). Does **not** reduce `stat_pkts_received` or cause
`stat_pkts_unrecovered`. Most common benign cause: another stream on the same multicast
group leaks in — confirm via `stat_pkts_wrong_pt_dropped` / `stat_pkts_wrong_ssrc_dropped`.

## Per-session quirks

**Video (ST20/ST22).** Loss uses **frame-internal `pkt_idx`**.
`stat_pkts_unrecovered` is **estimated** as `(frame_size − recv_size) / avg_pkt_size`.
Cross-frame reorders are not tracked. Watch
`stat_frames_dropped`, `incomplete_frames_cnt`, `stat_pkts_rtp_ring_full`,
`stat_pkts_no_slot`, `stat_slot_get_frame_fail`.

**Audio/Anc/FMD (ST30/40/41).** Loss uses post-redundancy `session_seq_id` →
`stat_pkts_unrecovered` is **exact**. ST30 adds `stat_pkts_dropped`,
`stat_pkts_len_mismatch_dropped`, `stat_slot_get_frame_fail`. ST40/41 add
`stat_pkts_wrong_interlace_dropped`, `stat_pkts_enqueue_fail`.

**TX (any type).** `stat_epoch_drop` = app late, `stat_epoch_onward` = on time,
`stat_epoch_mismatch` = pacing snapped to a different epoch,
`stat_recoverable_error` / `stat_unrecoverable_error` = TX faults.

## Periodic stat log lines

Emitted every `stat_dump_period_s` (default 10s); zero-delta lines are suppressed.

```text
RX_<TYPE>_SESSION(idx): fps F.f frames N pkts M [(redundant R)]
RX_<TYPE>_SESSION(idx): per-port arrivals P=<n> pkts (<f> frames <verb>), R=<n> pkts (<f> frames <verb>)
RX_<TYPE>_SESSION(idx): per-port loss covered by redundancy: <L> of <T> pkts
                        (P:<dp>=<%>, R:<dr>=<%>), unrecovered (lost on both) <U>, save_rate=<z>%
RX_<TYPE>_SESSION(idx): unrecovered pkts <U>                  (single-port form, err)
```

`<verb>` = `complete` (video) or `first` (audio/anc/fmd).

| Line | Level |
|---|---|
| `per-port arrivals` | `notice` |
| `per-port loss … save_rate=100%` | `warn` |
| `per-port loss … save_rate<100%` | `warn` + `err` |
| `unrecovered pkts <U>` (single-port) | `err` |
| `wrong PT/SSRC/len/interlace dropped` | `notice` |

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
