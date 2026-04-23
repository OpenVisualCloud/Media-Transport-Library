# Session Statistics Guide

How to read MTL per-session counters. Field-level docs live in the headers
(`st_api.h`, `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`).

## API

```c
st<NN>_<rx|tx>_get_session_stats(handle, &stats);
st<NN>_<rx|tx>_reset_session_stats(handle);
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
 │     stat_pkts_received++       ◄── "what the app actually got" (per pkt)    │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  7. ENQUEUE to slot / RTP ring  (transport)                                 │
 │                                                                             │
 │     no free slot          ──► stat_pkts_no_slot++          (video)          │
 │     ring full             ──► stat_pkts_rtp_ring_full++    (video RTP)      │
 │     anc enqueue fail      ──► stat_pkts_enqueue_fail++     (anc/fmd)        │
 │     audio offset overflow ──► stat_pkts_dropped++          (audio)          │
 │     no free framebuff     ──► stat_slot_get_frame_fail++   (video/audio)    │
 │                                                                             │
 │     first pkt of a new frame on this port                                   │
 │                           ──► port[i].frames++             (audio/anc/fmd)  │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │ frame complete (last pkt arrived, or timeout closed it)
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  8. FRAME COMPLETION  (transport notify_frame_ready → pipeline)             │
 │                                                                             │
 │     Video: per-port frame accounting                                        │
 │       port P delivered enough pkts ──► port[P].frames++                     │
 │       port P was short             ──► st20_rx_user_stats                   │
 │                                          .frames_partial[P]++               │
 │       (same for port R)                                                     │
 │       gap → estimated missing pkts ──► stat_pkts_unrecovered += est         │
 │                                                                             │
 │     ST20 / ST22 (transport): intra-frame loss detected                       │
 │                                    ──► stat_frames_incomplete++             │
 │                                                                             │
 │     ST20p / ST40p: frame delivered with intra-frame loss             │
 │                                    ──► stat_frames_corrupted++              │
 │                                        (frame still delivered with          │
 │                                         ST_FRAME_STATUS_CORRUPTED)          │
 │                                                                             │
 │     Pipeline (ST20p / ST30p / ST40p):                                       │
 │       no free user framebuff       ──► stat_frames_dropped++                │
 │                                        frame NOT delivered                  │
 │       handed off to user ring      ──► stat_frames_received++               │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  9. APP get_frame()                                                         │
 └──────────────────────────────────────────────────────────────────────────────┘
```

`port[i].packets` = what arrived on wire `i`. `stat_pkts_received` = packets the
session accepted (post-redundancy). `stat_frames_received` = frames the app
actually got.

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

## Frame-level counters

Session-wide, type-agnostic counters living in `st_rx_user_stats` /
`st_tx_user_stats`. Use these (not `port[i].frames`) to answer "how many
frames did the app receive / drop / send".

| Counter | Side | Meaning |
|---|---|---|
| `stat_frames_received`  | RX | Frames delivered to the app via the get-frame / notify path |
| `stat_frames_dropped`   | RX | Frames the pipeline could not deliver (no free user slot) |
| `stat_frames_corrupted` | RX | Frames delivered with `ST_FRAME_STATUS_CORRUPTED` (unrecovered intra-frame loss); the frame is still handed to the app |
| `stat_frames_sent`      | TX | Frames whose final packet was committed to the wire (`notify_frame_done(COMPLETE)`) |
| `stat_frames_dropped`   | TX | Frames the pipeline dropped because the app handed them too late (`notify_frame_done(DROPPED)`); also bumped on `put_frame_abort` |

Populated by pipeline session types (`ST20p`, `ST30p`, `ST40p` for both
RX and TX). For transport-only paths and types with no per-frame
integrity concept (`stat_frames_corrupted` on `ST30p` audio, `ST41` RX),
the relevant counters stay 0.

> **ST20 / ST22 only:** the transport-layer field `stat_frames_incomplete`
> (in `st20_rx_user_stats`) is **not** the same as `stat_frames_corrupted`.
> `stat_frames_incomplete` fires whenever the transport detects intra-frame
> loss, including when the frame is then silently discarded because
> `RECEIVE_INCOMPLETE_FRAME` is not set; `stat_frames_corrupted` only
> counts corrupted frames the app actually consumed via `get_frame()`.
> `stat_frames_incomplete - stat_frames_corrupted` = corrupted frames
> dropped before reaching the app.

## `port[i].frames` — two flavors (per-port, **not** a session total)

| Sessions | `frames++` when… | Partial counter |
|---|---|---|
| Video (ST20/ST22) | This port delivered enough pkts to **complete** the frame | `st20_rx_user_stats::frames_partial[i]` (this port was short) |
| Audio/Anc/FMD     | New frame's **first** packet arrived on this port (race winner) | n/a |

`port[i].frames` is per-port and the two flavors above do not compose: summing
across ports does not yield total frames delivered to the app. For end-to-end
frame accounting, use the session-wide common counters:
`stat_frames_received`, `stat_frames_dropped`, `stat_frames_corrupted` (RX)
and `stat_frames_sent`, `stat_frames_dropped` (TX) — see
[Frame-level counters](#frame-level-counters) above. Use `port[i].frames` /
`st20_rx_user_stats::frames_partial[i]` only for per-port redundancy debugging.

**Video frame-mode invariant** (ST20 RX with frame callbacks, per port):
`port[i].frames + frames_partial[i] == stat_frames_received`.
Each delivered frame (status `COMPLETE` or `RECONSTRUCTED`) bumps exactly one
of the two counters per port. Frames that never complete on either port bump
`stat_frames_incomplete` instead and leave both per-port counters untouched.

> **Does NOT hold for:** ST22 single-port (port R is left at zero, since
> there is no port R to account for) or any RTP-mode session (only the
> winning port's `frames` is bumped; `frames_partial` is unused).
>
> **Example.** A 2-port redundant RX session reports:
>
> ```text
> stat_frames_received = 1000   (TX sent 1000, all delivered to app)
> frames_dropped       = 0
> unrecovered          = 0
>
> port[P].frames = 950          frames_partial[P] =  50
> port[R].frames =  50          frames_partial[R] = 950
> ```
>
> The invariant holds on both ports: 950 + 50 = 1000.
>
> **Reading it:** P completes 95% of frames on its own; R only 5%. Every
> frame still reaches the app because whenever R is short, P fills the gap
> (and vice versa) — that's redundancy doing its job. But **R is missing
> packets on 950 of 1000 frames** (95%). R is one switch hiccup away from
> real loss.
>
> Investigate R: `mtl_get_port_stats(R).rx_hw_dropped_packets`, switch port
> stats, MTU, SFP/cable.

### `err_packets` — not data loss

`port[i].err_packets` = packets that arrived but the session refused (wrong PT, wrong
SSRC, length mismatch, etc.). Does **not** reduce `stat_pkts_received` or cause
`stat_pkts_unrecovered`. Most common benign cause: another stream on the same multicast
group leaks in — confirm via `stat_pkts_wrong_pt_dropped` / `stat_pkts_wrong_ssrc_dropped`.

## TX packet processing pipeline

```text
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  APP put_frame() / put_frame_abort()                                        │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  1. PIPELINE INTAKE  (ST20p / ST30p / ST40p)                                │
 │                                                                             │
 │     put_frame_abort()         ──► stat_frames_dropped++                     │
 │                                   notify_frame_done(DROPPED)                │
 │     put_frame()               ──► hand frame to transport ring              │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │ frame ready for transport
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  2. USER-TIMESTAMP VALIDATION  (only if the app supplied an RTP timestamp)  │
 │                                                                             │
 │     timestamp invalid     ──► stat_error_user_timestamp++                   │
 │                               (the frame is still scheduled)                │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  3. PACING DECISION                                                         │
 │                                                                             │
 │     app handed the frame too late, slots had to be skipped                  │
 │                            ──► stat_epoch_drop += skipped_slots             │
 │                                notify_frame_late() (if app registered)      │
 │                                                                             │
 │     app handed the frame too early, scheduled far in the future             │
 │                            ──► stat_epoch_onward += onward_slots            │
 │                                                                             │
 │     pacing snapped to a different epoch than requested                      │
 │                            ──► stat_epoch_mismatch++  (audio/anc/fmd)       │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │ frame slot assigned
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  4. PER-PORT BUILD  (transport tasklet, P and R independently)              │
 │                                                                             │
 │     packet built and queued ──► port[i].build++                             │
 │     packet enqueued to NIC  ──► port[i].packets++                           │
 │                                 port[i].bytes  += pkt_len                   │
 │     transient build error   ──► stat_recoverable_error++                    │
 │     fatal build/send error  ──► stat_unrecoverable_error++                  │
 │                                 (session needs restart)                     │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  5. FRAME COMPLETION  (transport — last packet of frame committed to wire)  │
 │                                                                             │
 │     all packets sent on this port ──► port[i].frames++                      │
 │                                                                             │
 │     pipeline late-drop watchdog (post-send):                                │
 │       cur_tai > frame_tai + frame_period                                    │
 │                                  ──► stat_frames_dropped++                  │
 │                                      notify_frame_done(DROPPED)             │
 │     otherwise                       ──► stat_frames_sent++                  │
 │                                          notify_frame_done(COMPLETE)        │
 └──────┬───────────────────────────────────────────────────────────────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  6. APP notify_frame_done(status)                                           │
 └──────────────────────────────────────────────────────────────────────────────┘
```

`port[i].packets` = what was put on wire `i`. `stat_frames_sent` = frames the
app successfully delivered end-to-end. `stat_frames_dropped` is the only
counter that reflects `ST_FRAME_STATUS_DROPPED` — `stat_error_user_timestamp`
only counts user-supplied RTP timestamp validation failures and is unrelated
to drops.

## Per-session quirks

**Video (ST20/ST22).** Loss uses **frame-internal `pkt_idx`**.
`stat_pkts_unrecovered` is **estimated** as `(frame_size − recv_size) / avg_pkt_size`.
Cross-frame reorders are not tracked. Watch
`stat_frames_dropped`, `stat_frames_incomplete`, `stat_pkts_rtp_ring_full`,
`stat_pkts_no_slot`, `stat_slot_get_frame_fail`.

**Audio/Anc/FMD (ST30/40/41).** Loss uses post-redundancy `session_seq_id` →
`stat_pkts_unrecovered` is **exact**. ST30 adds `stat_pkts_dropped`,
`stat_pkts_len_mismatch_dropped`, `stat_slot_get_frame_fail`. ST40/41 add
`stat_pkts_wrong_interlace_dropped`, `stat_pkts_enqueue_fail`. ST20p and
ST40p RX mark frames whose constituent packets had unrecoverable gaps as
`ST_FRAME_STATUS_CORRUPTED` and count them in `stat_frames_corrupted`;
the frame is still delivered to the app (the app should consult
`frame->status`).

**TX (any type).** `stat_epoch_drop` = app handed frame too late, slots were
skipped (counter advanced by the number of skipped slots).
`stat_epoch_onward` = system clock ran past `max_onward_epochs` ahead of the
next free slot — the frame was still scheduled, but pacing skipped onward
slots (advanced by the onward delta). `stat_epoch_mismatch` = pacing snapped
to a different epoch than requested. `stat_recoverable_error` /
`stat_unrecoverable_error` = TX faults during build/send.
`stat_error_user_timestamp` counts user-supplied RTP timestamp validation
failures during pacing setup; it does **not** track frames dropped because
the pipeline handed them to TX too late — those are
`stat_frames_dropped` (and surface to the app as
`notify_frame_done(status=ST_FRAME_STATUS_DROPPED)`).

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
