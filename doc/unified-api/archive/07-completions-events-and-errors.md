# 07 — Completions, events and errors

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-CMP-1…7, R-OBJ-4, R-OBJ-6, R-LIFE-3 |
| Research | [R09 libfabric §5–8](research/09-libfabric.md), [R11 prior art §1, §4, §5](research/11-media-io-prior-art.md), [R06 observability](research/06-observability.md), [R13 §3](research/13-lifecycle-errors-abi.md), [R01 R4](research/01-pr1610-analysis.md); reviews [C1](reviews/C1-realtime-feasibility.md), [C3](reviews/C3-usability-personas.md), [C4](reviews/C4-consistency-audit.md), [C5](reviews/C5-adversarial-user-review.md) and the [C5 response](reviews/C5-response.md) (A2, A3, A5 normative) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

Revision 3 changes, in short: CQ records hold `int64_t` TAI times with a validity mask and
fit the 256-byte entry (§1); array reads take the caller's record size and a timeout, and
`hdr.size` is output only (§2.4); status enums start at `UNSET = 0` (§1); EQs have a
subscription mask, shared EQs are in Phase 1, `USER` posts have their own ring, and every
event carries the origin's name (§3); `FLOW_STATE` and `RX_DISCONTINUITY` are new (§3.2); the error vocabulary is
split so `-MTL_ESHUTDOWN` means only "you asked" (§5); `enum mtl_state_reason` is written
in full (§5.4); MtlManager loss has a defined outcome (§7). Items marked **r3** changed in
this revision.

Two channels, deliberately different:

| | Completion queue (CQ) | Event queue (EQ) |
|---|---|---|
| Carries | per-unit results (TX terminal results, RX deliveries, RX progress) | state changes and notices (link, PTP, session state, flows, back-pressure, …) |
| Loss | **never**: results are materialised by the reader from the lease table ([04 §4.1](04-threading-and-execution.md)) | bounded; coalesces; counts overflow per producer class; every *state* event kind has a state getter for resync |
| Order | submission order (the unread-results ring waits for predecessors; slots are freed as units complete, 03 §4.1) | per source |
| Ownership | a CQ entry may return a buffer lease to the app | never carries ownership |
| Readers | one reader lock per queue (MP-safe), elided with `MTL_SESSION_SINGLE_READER` | one reader lock per EQ; every subscriber EQ gets its own copy (§3.4) |
| libfabric analogue | `fi_cq` | `fi_eq` |

PR #1610 put both into one 64-entry drop-on-full ring, so an application that did not
drain informational events lost ownership completions `[R01 R4]`. Separating them is
the fix.

## 1. Completion records (r3)

`[C5 §2.1, §2.6]`, A2. Every CQ entry is one fixed-size union, so a shared CQ can mix kinds
and bindings need one type `[C3 P7-2]`. Revision 2's primary kinds did not fit its own
256-byte entry once `struct mtl_time` (16 B) was used per time field; revision 3 stores
every time in a CQ record as **`int64_t` TAI nanoseconds** with one `uint32_t time_valid`
bit per time field. The `time_valid` word lives in the record header, so the embedded
timing structs (`mtl_tx_timing`, 120 B; `mtl_rx_timing`, 80 B; 06) carry none of their own.
"Every time in a CQ record is TAI or invalid" is a header rule; the self-describing
`struct mtl_time` stays for API arguments and getters, and
`mtl_time_diff_ns(a, b, time_valid, required, &out)` compares two record times, returning
`-MTL_EINVAL` when a bit of `required` is missing from `time_valid`, so an unset field never
compares.

```c
#define MTL_CQ_ENTRY_SIZE 256            /* frozen */
#define MTL_CQ_RECORD_MAX 240            /* every kind is <= 240 B, leaving >= 16 B of growth */

struct mtl_cq_entry_hdr {                /* 48 B */
  uint16_t kind;          /* MTL_CQE_TX_RESULT | RX_UNIT | RX_PROGRESS | TX_SOURCE_RELEASED | RX_MISSING */
  uint16_t size;          /* OUT only: bytes the library wrote into this record (A3) */
  uint32_t status;        /* per kind; 0 = *_STATUS_UNSET, never a success value */
  uint32_t flags;         /* MTL_CQE_MORE, MTL_CQE_FINAL, MTL_RX_F_USED_REDUNDANCY, … */
  uint32_t time_valid;    /* one bit per int64 time field of this kind */
  mtl_session_h session;
  mtl_lease_h   lease;    /* RX: the lease the app now holds; TX: the submitted lease, stale by
                             now (mtl_lease_buffer() still resolves it to its buffer) */
  uint64_t user_cookie;   /* TX: the submission's cookie if set, else the buffer's; RX: the buffer's */
  uint64_t seq;           /* per-session submission (TX) / delivery (RX) sequence number */
};
union mtl_cq_entry {
  struct mtl_cq_entry_hdr        hdr;
  struct mtl_tx_result           tx;        /* 208 B */
  struct mtl_rx_unit             rx;        /* 240 B */
  struct mtl_rx_progress         progress;
  struct mtl_tx_source_released  released;  /* 56 B */
  struct mtl_rx_missing          missing;
  uint8_t raw[MTL_CQ_ENTRY_SIZE];
};
```

The shapes in this section are illustrative; the header (10, `sketch/include/…`) is
normative, and each struct there has a C99 size check and no implicit padding (A1).

### 1.1 TX result

```c
enum mtl_tx_status {
  MTL_TX_STATUS_UNSET = 0,  /* a half-filled record never reads as success (A5) */
  MTL_TX_ON_TIME = 1, MTL_TX_LATE, MTL_TX_DROPPED, MTL_TX_FLUSHED, MTL_TX_FAILED,
};
struct mtl_tx_result {
  struct mtl_cq_entry_hdr hdr;      /* hdr.status = enum mtl_tx_status */
  uint32_t reason;                  /* enum mtl_tx_reason (§1.1.1); NONE when ON_TIME */
  int32_t  error;                   /* negative MTL_E* code for FAILED */
  struct mtl_tx_timing timing;      /* 06 §7.5, 120 B: int64 TAI times (media, submitted, deadline,
                                       scheduled, enqueued, observed per leg), margins, rtp, and the
                                       audio sample counts (padded, dropped, inserted); validity in
                                       hdr.time_valid */
  uint32_t leg_count;
  uint32_t pkts_sent[MTL_MAX_LEGS];
  uint8_t  leg_reason[MTL_MAX_LEGS];/* enum mtl_tx_reason per leg: NONE, or why that leg sent nothing
                                       (LINK_DOWN, NO_NEIGHBOUR, LEG_DISABLED) */
  uint16_t reserved0;
  uint32_t path;                    /* enum mtl_path_kind: the session's granted path (05 §6) */
  uint32_t pkts_dma, pkts_copied_partial;   /* per unit (05 §6) */
  uint32_t reserved1;
};                                  /* 208 B */
```

`ON_TIME` is an **admission verdict**: the unit was picked up no later than its pick-up
deadline, so the engine could schedule it in its slot. Wire accuracy
(`observed_first − scheduled_first`, and per-packet lateness against TPRj once the TX
self-check exists) is *reported* in `timing` but does not change the status, because a SW
observation at `tx_burst` is enqueue time, not wire time, under RL pacing `[C1 #18]`
(Q-CMP-6).

| Status | Sent? | Meaning | Reasons |
|---|---|---|---|
| `ON_TIME` | yes (on at least one leg) | picked up by its deadline and scheduled in its slot | `NONE` (an `EXACT` launch that meets its deadline is `ON_TIME` with `timing.flags & MTL_TX_TIMING_NON_COMPLIANT`); per leg `leg_reason` |
| `LATE` | yes | sent under the bounded `SEND_LATE` policy after its deadline | `TOO_LATE` |
| `DROPPED` | no | not sent; its RTP slot is empty | `TOO_LATE`, `WOULD_OVERLAP`, `DUPLICATE_SLOT`, `DUPLICATE_INDEX`, `BEHIND`, `OFF_GRID`, `LINK_DOWN` / `NO_NEIGHBOUR` / `LEG_DISABLED` (every leg), `RECOVERY`, `INVALID_PAYLOAD`, `ENCODER_FAILED`, `REJECTED_AT_PICKUP` |
| `FLUSHED` | no | removed before it started, by an application action or by ERROR entry | `STOP_FLUSH`, `STOP_TIMEOUT`, `DISCARD`, `WITHDRAWN`, `DESTROY`, `BEFORE_START`, `SESSION_ERROR` |
| `FAILED` | partial / unknown | an error while sending; `error` has the code | `TX_QUEUE_FATAL`, `DEVICE_GONE`, `SESSION_ERROR` |

This is DeckLink's four-way outcome {Completed, DisplayedLate, Dropped, Flushed} plus an
explicit failure `[R11 §1.2]`. Today frames in flight during TX recovery are reported as
`COMPLETE` and counted as sent `[R06 F2]`; in the new API they are `DROPPED/RECOVERY` or
`FAILED`.

#### 1.1.1 `enum mtl_tx_reason` (r3)

A compact enum (< `MTL_TX_REASON_SLOTS` = 32) so per-reason counters are fixed arrays (08).

| Value | Name | Status | Trigger |
|---|---|---|---|
| 0 | `NONE` | ON_TIME | — |
| 1 | `TOO_LATE` | LATE, DROPPED | picked up after its deadline (06 §7.2) |
| 2 | `WOULD_OVERLAP` | DROPPED | bounded SEND_LATE could not fit before the next unit |
| 3 | `DUPLICATE_SLOT` | DROPPED | CAPTURE NEAREST snapped onto a slot already submitted (06 §4.3) |
| 4 | `DUPLICATE_INDEX` | DROPPED | INDEX mode: an index equal to the last one submitted (06 §4.4) |
| 5 | `OFF_GRID` | DROPPED | LOCKED_PHASE: media time off the grid (06 §4.3) |
| 6 | `LINK_DOWN` | DROPPED / per leg | the leg's link is down |
| 7 | `NO_NEIGHBOUR` | DROPPED / per leg | the leg's destination MAC is unresolved (`WAITING_NEIGHBOUR`, 03 §3.3) |
| 8 | `LEG_DISABLED` | DROPPED / per leg | `mtl_session_set_leg_enabled(s, leg, false)` |
| 9 | `RECOVERY` | DROPPED | lost in a TX recovery or a port reset gap |
| 10 | `INVALID_PAYLOAD` | DROPPED | a payload the builder cannot packetise at pick-up. Not used for an ST22 codestream over the granted size: that is a synchronous `-MTL_ENOSPC` from `mtl_tx_submit`, reason `CODESTREAM_OVERSIZE` (06) |
| 11 | `ENCODER_FAILED` | DROPPED | the ST22 encoder plugin failed |
| 12 | `REJECTED_AT_PICKUP` | DROPPED | the builder refused the unit (04 §4.4) |
| 13 | `STOP_FLUSH` | FLUSHED | `mtl_session_stop(MTL_STOP_FLUSH)` |
| 14 | `STOP_TIMEOUT` | FLUSHED | the drain timed out |
| 15 | `DISCARD` | FLUSHED | `mtl_session_discard_queued` |
| 16 | `WITHDRAWN` | FLUSHED | `mtl_tx_withdraw` |
| 17 | `DESTROY` | FLUSHED | destroy of a running session |
| 18 | `BEFORE_START` | FLUSHED | a preroll unit whose media time precedes the start instant |
| 19 | `SESSION_ERROR` | FLUSHED, FAILED | the session entered ERROR (03 §3.4) |
| 20 | `TX_QUEUE_FATAL` | FAILED | the unit was in a queue that had to be reset (04 §4.5) |
| 21 | `DEVICE_GONE` | FAILED | the device disappeared with the unit in flight |
| 22 | `BEHIND` | DROPPED | INDEX mode: an index smaller than the last one submitted (06 §4.4) |

### 1.2 RX delivery

```c
enum mtl_rx_status { MTL_RX_STATUS_UNSET = 0, MTL_RX_COMPLETE = 1, MTL_RX_INCOMPLETE, MTL_RX_DISCARDED };
struct mtl_rx_unit {
  struct mtl_cq_entry_hdr hdr;      /* hdr.status = enum mtl_rx_status */
  struct mtl_rx_timing timing;      /* 06 §11, 80 B: rtp, int64 TAI media/arrival per leg/presentation,
                                       flags, units_missing_before; validity in hdr.time_valid */
  uint32_t leg_count;
  uint32_t pkts_expected;
  uint32_t pkts_received[MTL_MAX_LEGS];
  uint32_t pkts_recovered;          /* filled by the other leg (2022-7) */
  uint32_t units_missed_before;     /* units the pool could not receive since the previous delivery */
  uint32_t units_before_start;      /* r3: units discarded while ARMED (03 §3.5), since the previous delivery */
  uint32_t valid_bytes;             /* audio/ANC/fastmeta/cvideo */
  uint32_t sample_count;            /* audio */
  uint32_t ready_rows;              /* progressive */
  uint32_t format_changed;          /* bitmask: which detected properties differ from the config */
  uint32_t meta_bytes;              /* sender user meta / ANC packet table in the buffer's meta area (05 §4) */
  uint32_t anc_count;
  uint32_t missing_count;           /* ranges in total; above MTL_RX_MISSING_RANGES see RX_MISSING */
  uint32_t path;                    /* r3: enum mtl_path_kind, the session's granted path (05 §6) */
  uint32_t pkts_dma, pkts_copied_partial;     /* r3: per unit (05 §6) */
  uint32_t user_meta_type;          /* r3: MTL_META_UNTAGGED or the sender's tag (05 §4) */
  uint32_t user_meta_version;
  uint32_t reserved0;
  struct mtl_rx_missing_range missing[MTL_RX_MISSING_RANGES];   /* 4 × {first_pkt, pkt_count} */
};                                  /* 240 B */
```

Two gap counters answer different questions: `units_missed_before` counts units this pool
could not receive, and `timing.units_missing_before` counts units absent from the stream by
media-index difference (06 §11.5). `timing.flags` carries `MTL_RXT_DISCONTINUITY` (the
sender re-anchored; an `RX_DISCONTINUITY` event follows, §3.2), `MTL_RXT_LATE_FOR_PRESENTATION`
(delivered after `presentation_tai_ns`), `MTL_RXT_INCOMPLETE_BY_DUE` (force-completed at the
due time, 04 §4.4) and `MTL_RXT_HW_ARRIVAL`.

| Status | Meaning |
|---|---|
| `COMPLETE` | every packet received (possibly via either leg) |
| `INCOMPLETE` | packets missing after the deadline (04 §4.4) or at a forced completion (stop DRAIN); `pkts_*` and `missing[]` say how much |
| `DISCARDED` | (dynamic provide only, later) the buffer returns without a unit |

`RECONSTRUCTED` in today's enum means "complete thanks to the second leg"; it becomes the
flag `MTL_RX_F_USED_REDUNDANCY` plus `pkts_recovered`, because a unit can be complete
*and* rescued. Whether incomplete units are delivered or discarded is the session option
`rx_incomplete = DELIVER | DISCARD` (default DELIVER); L2 always enables incomplete delivery
in the transport and applies this option itself (04 §4.4).

### 1.3 Other kinds

- `MTL_CQE_RX_PROGRESS` — a progressive RX unit gained rows (06 §9). It is the **only**
  progress mechanism; `mtl_rx_wait_progress` is a convenience over it. **r3:** at most one
  pending progress entry per unit is kept and updated in place (the latest `ready_rows`),
  so progress can never exceed one unread entry per slot; the last entry of a unit carries
  `MTL_CQE_FINAL` and the unit's terminal status `[C3 P4-4, C5 §4.11]`.
- `MTL_CQE_TX_SOURCE_RELEASED` — optional early release of the source buffer, followed
  later by the terminal `TX_RESULT` (`hdr.flags` has `MORE`). **r3:** a Phase 4 opt-in
  (`completion.optional_kinds |= MTL_CQE_ENABLE_SOURCE_RELEASED`) for COPY and CONVERT paths
  only; on a session whose granted path is DIRECT, enabling it is `-MTL_ENOTSUP`, because the
  NIC reads the source until the terminal result. Off by default (05 §7, Q-MEM-1).
- `MTL_CQE_RX_MISSING` — **r3** optional follow-on record with the full missing-range table
  of an RX unit (`hdr.flags & MTL_CQE_MORE` on the unit), enabled per session; detail that
  does not fit the unit record goes here (A2).

## 2. Completion modes and back-pressure

### 2.1 Capacity by construction

Results are generated on the reader's side from the lease table (04 §4.1), so the
tasklet never writes into a queue that could be full. What remains bounded is how many
*unread* results the library keeps for the app:

```text
per session:  unread results ≤ pool size      (the unread-results ring; + 1 per unit when SOURCE_RELEASED
                                              or RX_MISSING is enabled; + 1 coalesced progress entry per unit)
acquire needs: a FREE slot, or (NONE) a DONE slot. In ALL/EXCEPTIONS a unit's slot is freed as soon as
               the unit completes (its result moves into the unread ring), without waiting for
               earlier units; the ring publishes in submission order (03 §4.1). Only a ring the
               app has not read (pool size unread results) leaves a slot DONE; acquire then
               reports RESULTS
shared CQ:     a poll set over members with a ready summary (04 §4.3); no extra capacity rule
```

**Configured capacities.** `struct mtl_cq_config` has no capacity field: a CQ's capacity
follows from its members' pools, as above. `struct mtl_eq_config.capacity` is the size of
each library, app and USER ring of that EQ (§3.3), in records; 0 means 64. Tasklet-class
events need no ring (per-source pending state, §3.3), so the value bounds only library,
app and USER records between two reads.

### 2.2 Modes

| Mode | Results kept for the app | Use | Default for |
|---|---|---|---|
| `MTL_COMPLETE_NONE` | none; DONE slots are recycled by acquire/dequeue; outcomes go to counters and coalesced events | simplest apps; Rivermax-style loops; the legacy pipeline shim | **library pools** |
| `MTL_COMPLETE_EXCEPTIONS` | only results whose status is not `ON_TIME` | apps that only react to problems | — |
| `MTL_COMPLETE_ALL` | every terminal result | anything using cookies or per-unit timing | **attached (imported) pools and exported pools (`MTL_SESSION_EXPORT_POOL`, 03 §4.4) — forced**, because the result is how the app learns its buffer is free |

This follows usability review C3: with "ALL" as a universal default, the minimal loop that
never reads results stalls silently after `pool.count` frames `[C3 P1-1]` (Q-CMP-1).
"Exactly one terminal outcome" (R-CMP-1) holds in every mode: a result entry, or — when
the mode suppresses that result — a counter increment.

**r3: no silent cookie drop** `[C5 §3.6]`. A submission with a non-zero `user_cookie` on a
session in completion NONE fails with `-MTL_EINVAL`, reason `COOKIE_WITHOUT_RESULTS`: a
cookie is only useful in a result, so asking for one where no result exists is a
configuration bug and fails the first time, not silently forever. `pool.count = 0` means the
library default (video: `max(min_count_direct, 3)`; audio, ANC, fastmeta: 4; 09 defaults
table).

### 2.3 Telling the app why it is blocked

`mtl_tx_acquire` returns `-MTL_EAGAIN` (or `-MTL_ETIMEDOUT` when waiting) with the reason
available as `mtl_session_get_status(…).blocked_on`:

| `blocked_on` | Meaning | What to do |
|---|---|---|
| `MTL_BLOCKED_NONE` | not blocked | — |
| `MTL_BLOCKED_BUFFERS` | every slot is queued or in flight — normal back-pressure | wait |
| `MTL_BLOCKED_RESULTS` | the unread-results ring is full | read your CQ |
| `MTL_BLOCKED_RING` | the submission hand-off is full | wait |
| **r3** `MTL_BLOCKED_APP_LEASES` | every slot is APP_WRITABLE: the app holds them all (a lease leaked after a failed submit) `[C5 §3.2]` | submit or `mtl_tx_release` what you hold |
| **r3** `MTL_BLOCKED_APP_HOLDS` | the FREE slots are all excluded from any-free acquire by `mtl_buffer_hold` (05 §4.5) | `mtl_buffer_unhold`, or `mtl_tx_acquire_buffer` on a held buffer |

Rivermax separates `RMX_NO_FREE_CHUNK` from `RMX_HW_SEND_QUEUE_IS_FULL` for the same reason.
An EQ `BACKPRESSURE` event is posted on the first transition, and the admin thread logs one
warning if a session stays blocked on RESULTS or APP_LEASES for more than 1 s.

### 2.4 Reading (r3)

`[C5 §2.6, §3.5]`, A3. Array readers take the caller's record size explicitly and write
`min(record_size, native)` bytes per record at pitch `record_size`; they never modify a
caller-set stride, and `hdr.size` in each written record is an output (the bytes written).
Hoisting an initialiser out of a loop can no longer corrupt an array read, and a binding
passes a plain byte buffer.

```c
int mtl_session_get_cq(mtl_session_h s, mtl_cq_h* cq);                            /* DP: the private CQ */
int mtl_cq_create(mtl_instance_h mt, const struct mtl_cq_config* c, mtl_cq_h* out); /* CP: shared CQ */
int mtl_session_bind_cq(mtl_session_h s, mtl_cq_h cq);                            /* CP; CREATED/STOPPED only */
int mtl_cq_read(mtl_cq_h cq, void* buf, size_t record_size, uint32_t max, int64_t timeout_ns);  /* WT */
int mtl_cq_trywait(mtl_cq_h cq);                                                  /* DP: 1 ready, 0 armed, < 0 error */
int mtl_cq_get_wait_object(mtl_cq_h cq, struct mtl_wait_object* out);             /* CP */
int mtl_cq_interrupt(mtl_cq_h cq);   int mtl_cq_uninterrupt(mtl_cq_h cq);          /* AS / CP, sticky */
int mtl_cq_destroy(mtl_cq_h cq);                                                  /* CP; -MTL_EBUSY while sessions are bound */
```

- **Return convention** (A3): an array read returns the count ≥ 0. `0` means "empty" and
  occurs only with `timeout_ns == 0`; with a timeout > 0 and nothing ready the call returns
  `-MTL_ETIMEDOUT`. A read with a timeout is the wait; there is no separate `mtl_cq_wait`.
- Single-object verbs (`mtl_tx_acquire`, `mtl_rx_dequeue`) return 0 or a negative code.
- `record_size` smaller than the record header (48 B) is `-MTL_EINVAL`; exactly 48 B yields a
  header-only fill (10).

Convenience verbs are thin wrappers on the session's private CQ (timeout always last):

- `mtl_rx_dequeue(s, &lease, &view, &unit, unit_size, timeout)` = the next `RX_UNIT` of this
  session; `view` and `unit` are nullable (10);
- `mtl_tx_reap(s, res, res_size, max, timeout)` = the next `TX_RESULT`s of this session.

With a shared CQ the app uses `mtl_cq_read` and dispatches on `hdr.session`. A small
dispatcher helper (a thread reading the CQ with a timeout that calls an app function per
entry) ships in L4 in v1 for apps that prefer callbacks (Q-CMP-2).

## 3. Event queue (r3)

### 3.1 Record

`[C5 §7.5, §8.7]`. Every event carries the origin's **name** — the 64-byte name a session
was given at create (copied, unique per instance, 08) or the port name — because the handle
alone is useless after the session retires (a `SESSION_RETIRED` reader cannot call
`get_info` on a retired handle) and changes on every recreate:

```c
struct mtl_event {
  uint16_t size;            /* OUT: bytes written (A3) */
  uint16_t severity;        /* INFO | WARNING | ERROR | FATAL */
  uint32_t type;            /* enum mtl_event_type */
  uint32_t coalesced;       /* how many occurrences this record stands for (>= 1) */
  uint32_t reason;          /* enum mtl_state_reason (§5.4), NONE if not applicable */
  uint64_t seq;             /* per EQ; a gap means overflow happened */
  int64_t  time_tai_ns;     /* of the (last) occurrence */
  uint32_t time_valid;
  uint32_t origin_kind;     /* INSTANCE | PORT | SESSION | GROUP | REGION | TIMELINE | EQ | APP */
  uint64_t origin;          /* the typed handle's id (session, group, region, timeline, eq) or the port index */
  char     origin_name[64]; /* MTL_NAME_MAX. SESSION: the session name (08); PORT: the port name;
                               GROUP/TIMELINE: their name; else "" */
  uint32_t producer_class;  /* OVERFLOW: which class lost records (§3.3) */
  uint32_t reserved;
  union mtl_event_payload u;  /* MTL_EVENT_PAYLOAD = 64 B: typed payloads (state, port, time_step,
                                 flow, timing, overflow, backpressure, epoch_tick, rx_format,
                                 rx_discontinuity, recovery) and user[32] */
};                          /* 184 B */
int mtl_eq_read(mtl_eq_h eq, struct mtl_event* ev, size_t ev_size, uint32_t max, int64_t timeout_ns);  /* WT, A3 */
```

The record size and payload layout are fixed in the header with a size check (A1). The
name is carried inline rather than as an index into an instance name table, so a reader
needs no lookup that could itself race with retirement.

### 3.2 Event types

> **Addenda K and N (2026-10-01).** `mtl_queue.h` adds events 23–30: `PORT_REMOVED`,
> `SCHED_STALLED`, `HEALTH` ([16](16-kubernetes-and-crash-safety.md)); `UPDATE`,
> `GRANDMASTER`, `PORT_ADDRESS`, `RTCP_INFO`, `KEY_NEEDED` ([17](17-nmos-and-ipmx.md)). The
> header is normative for their payloads.

"State" events have a state getter for resync; "notice" events do not need one, but the
two notices a framework must not miss — the timing warning and retirement — have getters
too `[C5 §7.5]`. `#` is the `enum mtl_event_type` value (`MTL_EVENT_<type>`; 0 is `UNSET`).
The subscription bit (§3.4) that delivers each type is in the last column.

| # | Type | Kind | Origin | Payload | State getter | Subscription |
|---|---|---|---|---|---|---|
| 1 | `TIME_STATE` | state | instance/port | old → new state, offset | `mtl_time_get_status` | `TIME` |
| 2 | `TIME_STEP` | notice | instance | step ns, policy applied per timeline | — | `TIME` |
| 3 | `PORT_LINK` | state | port | up/down, speed | `mtl_port_get_status` | `PORT` |
| 4 | `PORT_RESET` | state | port | reason (VF reset, recovery) | same | `PORT` |
| 5 | `SESSION_STATE` | state | session | old → new, `reason` | `mtl_session_get_state` / `get_status().reason` | `SESSION` |
| 6 | `SESSION_RETIRED` | notice | session | destroy finished, immediate or deferred (03 §6.2); `origin_name` identifies it | **r3** `mtl_session_get_state() == MTL_STATE_RETIRED` during the grace period; `mtl_instance_get_status().sessions_destroying` | `SESSION` (also the private EQ) |
| 7 | `SESSION_RECOVERY` | state | session | begin/end, units dropped; `recoveries_ok` is the count of record (coalesced records keep first and last) | `mtl_session_get_status` | `SESSION` |
| 8 | `LEG_STATE` | state | session | leg, **r3** admin (ENABLED/DISABLED) × oper (UP/DOWN/UNKNOWN) (TX link from the link monitor, 09; RX no packets on the leg for X ms) | `get_status().leg[i]` | `SESSION` |
| 9 | **r3** `FLOW_STATE` | state | session | leg, `RESOLVED` / `WAITING_NEIGHBOUR` / `JOINED` / `JOIN_FAILED`, admin state; after an `update_flows` activation `first_index` = the first unit on the new flows (TX: its media index; RX: the RTP of the first unit accepted, 09 §7.1); IGMP reports are a stats counter `[C5 §7.6]` | `get_status().leg[i].flow_state` | `SESSION` |
| 10 | `RX_SIGNAL` | state | session | lost / restored (no unit for `rx_signal_timeout`) | `mtl_session_get_status` | `SESSION` |
| 11 | `RX_FORMAT` | state | session | detected / changed / mismatch; bitmask of changed properties + new values | same | `SESSION` |
| 12 | `RX_TIMEBASE_SUSPECT` | state | session | \|arrival − media\| > 1 s on a DIRECT stream (06 §11) | same | `SESSION` |
| 13 | **r3** `RX_DISCONTINUITY` | notice | session | `{old_rtp, new_rtp}`: the receiver relocked onto a sender re-anchor (06 §11.5) | the unit's `timing.flags & MTL_RXT_DISCONTINUITY` (§1.2) | `SESSION` |
| 14 | `PACING_CHANGED` | state | session/port | requested, old granted, new granted | `mtl_session_get_info` | `SESSION` |
| 15 | `TIMING_INFEASIBLE` | notice | session | a CAPTURE unit arrived after its deadline: measured shortfall, suggested `min_tx_delay_ns` (06 §5.1) | **r3** `get_status().timing_warning` {reason, shortfall_ns, suggested_min_tx_delay_ns} (also set for `link_offset_budget_ns` excess and an ESTIMATED time source, 06) | `SESSION` |
| 16 | `BACKPRESSURE` | state | session | begin/end; `blocked_on` (§2.3) | `mtl_session_get_status` | `SESSION` |
| 17 | `TX_UNDERRUN` | state | session | begin/end; slots empty | stats | `SESSION` |
| 18 / 19 | `SCHED_OVERLOAD` / `SESSION_MIGRATED` (only if Q-THR-6 (b)) | state / notice | instance/session | scheduler index, busy % | `mtl_sched_get_status` | `INSTANCE` |
| 20 | `INLINE_HOOK_DISABLED` | state | session | budget exceeded (04 §6) | `mtl_session_get_status` | `SESSION` |
| 21 | `MANAGER_LOST` | state | instance | lost / reconnected (§7) | **r3** `mtl_instance_get_status().manager` (cached; replaces `mtl_is_manager_alive`) | `INSTANCE` |
| 22 | `GROUP_MEMBER_FAILED` | notice | group | session handle + name | `mtl_group_get_state` | `SESSION` |
| 23 | `REGION_RELEASED` | notice | region | (deferred-release mode) | `mtl_mem_get_info` | `INSTANCE` |
| 24 | `EPOCH_TICK` | notice | session | epoch, TAI (today's `ST_EVENT_VSYNC`; lossy; opt-in) | — | `EPOCH_TICK` only |
| 25 | `OVERFLOW` | notice | EQ | events lost since the last read, **r3** with `producer_class` | re-read all state getters | always |
| 26 | `USER` | notice | app (`mtl_eq_post`) | 32 bytes | — | `USER` |

Detection and loss events follow DeckLink's "flag per frame + event with a bitmask of
what changed" pattern `[R11 §1.5]`; coalescing follows GStreamer's jitter-buffer
"drop-msg" with counts since the last message `[R11 §3.5]`.

### 3.3 Producers, coalescing and overflow

Review C1 showed that merging duplicates in a plain SPSC ring races with the reader, and
that the EQ has more producers than the draft assumed `[C1 #8]`. The construction:

| Producer class (`producer_class`) | Mechanism | Why |
|---|---|---|
| `TASKLET` (session events, PTP-on-tasklet) | **per-source pending state**, not a ring: a pending-type bitmask, a seqlocked latest payload and a count per type, written wait-free by the one producing tasklet, plus a per-scheduler event counter bumped on each new pending source | coalescing is free; nothing overflows; the tasklet never waits, and its cost is independent of the subscriber count |
| `LIBRARY` (admin, CNI thread, PTP servo, waker, workers) | one small ring per producer class **per subscribed EQ**, filled at post time | a preempted producer can only delay its own class, never a tasklet |
| `APP` (`BACKPRESSURE` posted from acquire) | one ring per EQ for app-thread library notices, with a lock between app threads | app threads never share a structure with tasklets |
| **r3** `USER` (`mtl_eq_post`) | its **own** ring per EQ, with its own `lost` counter | an app that floods `USER` can never evict library notices such as `BACKPRESSURE` `[C5 §7.5]` |

An EQ is a poll set over those sources. `mtl_eq_read` materialises records from pending
state (first and last state of a transition are kept), visiting only the schedulers whose
event counter moved since its last read, and from the rings. If a ring overflows, the
producer increments that ring's `lost` and the next read returns `OVERFLOW(lost = n,
producer_class)` first; `seq` also jumps. Nothing ever blocks a producer.

### 3.4 Subscriptions and routing (r3)

`[C5 §7.5, §7.1]`. Revision 2 gave the instance EQ one reader and let only sessions be bound
to a created EQ, so twelve elements that each want `PORT_LINK` had to elect a reader and fan
out themselves. Now every EQ has a **subscription mask**, and each subscribing EQ gets its
own copy of an event:

```c
#define MTL_EQ_SUB_PORT        0x1u    /* PORT_LINK, PORT_RESET (all ports, or one via subscribe) */
#define MTL_EQ_SUB_TIME        0x2u    /* TIME_STATE, TIME_STEP */
#define MTL_EQ_SUB_INSTANCE    0x4u    /* MANAGER_LOST, SCHED_OVERLOAD, REGION_RELEASED */
#define MTL_EQ_SUB_SESSION     0x8u    /* every session event incl. SESSION_RETIRED (all sessions, or one via subscribe) */
#define MTL_EQ_SUB_EPOCH_TICK  0x10u   /* EPOCH_TICK, only for sessions that enable it */
#define MTL_EQ_SUB_USER        0x20u   /* USER posts to this EQ */
struct mtl_eq_config { uint32_t struct_size; uint32_t capacity; uint64_t mask; uint64_t reserved[4]; };  /* 48 B; capacity 0 = 64 (§2.1) */
int mtl_eq_create(mtl_instance_h mt, const struct mtl_eq_config* cfg, mtl_eq_h* out);   /* CP; cfg->mask = instance-wide subscriptions */
int mtl_eq_subscribe(mtl_eq_h eq, struct mtl_object obj, uint64_t mask);                /* CP; one session, port, group; mask 0 = unsubscribe */
int mtl_eq_trywait(mtl_eq_h eq);                                                        /* DP: 1 / 0 / < 0 */
int mtl_eq_get_wait_object(mtl_eq_h eq, struct mtl_wait_object* out);                   /* CP */
int mtl_eq_interrupt(mtl_eq_h eq);   int mtl_eq_uninterrupt(mtl_eq_h eq);                /* AS / CP */
int mtl_eq_post(mtl_eq_h eq, const struct mtl_event* ev);                               /* DP; USER ring */
int mtl_eq_destroy(mtl_eq_h eq);                                                        /* CP; drops a reference */
```

`struct mtl_object` is the typed-object reference shared with `mtl_debug_inject` (10).

| EQ | Default subscriptions | Notes |
|---|---|---|
| a session's **private EQ** (`mtl_session_get_eq`) | that session's events | the default, so two framework elements never read each other's events `[C3 P2-6]`; `mtl_session_bind_eq(s, eq)` routes the session's events to `eq` *instead* (CREATED/STOPPED) |
| the **instance EQ** (`mtl_instance_get_eq`) | `PORT \| TIME \| INSTANCE \| SESSION` | an operator app with 40 sessions reads one EQ instead of 41 |
| an app-created EQ | `cfg.mask` (instance-wide), plus `mtl_eq_subscribe` per object | twelve elements can each have their own EQ subscribed to `PORT \| TIME` |

- **Shared EQs are Phase 1** (they use the private EQ's poll-set construction); revision 2
  had them in no phase.
- **Fan-out is bounded per EQ.** Tasklet-class events fan out at read time (each EQ keeps its
  own cursor over the per-source pending state), so the tasklet's cost is independent of the
  number of subscribers; library-class events are copied into each subscribed EQ's ring at
  post time, and an EQ whose ring is full overflows alone.
- **Retirement is always readable** `[C5 §7.1]`: `SESSION_RETIRED` goes to the private EQ
  **and** to every EQ subscribed with `MTL_EQ_SUB_SESSION`, including the instance EQ;
  `mtl_session_get_eq`, `mtl_eq_read` and `mtl_eq_get_wait_object` stay legal in DESTROYING
  (03 §3.2).
- **EQs are refcounted.** `mtl_session_get_eq` takes an app reference on the private EQ, so it
  stays readable after the session retires, until the app calls `mtl_eq_destroy` on it (which
  only drops that reference while the session is live). An app that never calls
  `get_eq` pays nothing: the private EQ is freed at retire. Any EQ left is freed at instance
  teardown.

## 4. Callbacks

None in tasklet context by default (R-THR-1). The optional inline hook is described in
[04 §6](04-threading-and-execution.md). There is no library-thread callback delivery in the
core; the L4 dispatcher helper (§2.4) covers apps that want callbacks.

## 5. Errors (r3)

`[C5 §2.10, §3.2, §7.10, §8.3]`, A5.

### 5.1 Codes

One convention for the whole new API: `0` (or a count, or a state) on success, a negative
`MTL_E*` code on failure. Creation functions return an `int` and write the handle through an
out-parameter `[R13 R-L10]`. The codes are constants with fixed values, equal to the Linux
errno value, on every OS, so the header is one ABI; Windows UCRT's `errno.h` numbers some of
them differently (for example its `ETIMEDOUT` is 138) and lacks `ESHUTDOWN`, so apps compare
with `MTL_E*`, never with `errno.h`. A code Linux lacks, if one is ever needed, gets a value
≥ 1000.

| Code | Value | Meaning, and only this | Application action |
|---|---|---|---|
| `-MTL_EAGAIN` | 11 | nothing now and `timeout == 0`: no buffer / result / data / room (`blocked_on` says which); includes RX `dequeue` in CREATED/ARMED/STOPPED with nothing READY; a timeline anchor not yet resolved; an inline-safe call that found the reader lock contended | retry later |
| `-MTL_ETIMEDOUT` | 110 | the timeout expired with nothing available | as EAGAIN |
| `-MTL_ECANCELED` | 125 | waiters interrupted by `*_interrupt` / `mtl_instance_interrupt_all`; sticky until the matching `*_uninterrupt` | the framework's flushing path (GStreamer `GST_FLOW_FLUSHING`); call again after uninterrupt |
| `-MTL_ESHUTDOWN` | 108 | **app-initiated only**: the session is DRAINING, FLUSHING or DESTROYING because some thread of the app asked | leave the loop; this is the stop you (or your framework) requested |
| `-MTL_EIO` | 5 | the session is in **ERROR**; the reason is in `mtl_session_get_status().reason` and `mtl_last_error` | report; `stop`, then `start` (re-reserves) or destroy |
| `-MTL_ENODEV` | 19 | the device behind the session or port is gone or was reset, and resources cannot be re-reserved (`start` after ERROR; a port that is not open, reason `PORT_NOT_OPEN`) | reconfigure onto another port, or give up |
| `-MTL_EBADF` | 9 | null, foreign (another session's lease) or destroyed handle; a retired handle for every call except `get_state` | bug |
| `-MTL_ESTALE` | 116 | a lease of the right session whose generation moved on: already submitted, released or returned | bug |
| `-MTL_EINVAL` | 22 | invalid argument; the reason names which (§5.4) | fix the call |
| `-MTL_ERANGE` | 34 | timing outside the accepted window (beyond the horizon, start instant or exact launch in the past) | fix the time |
| `-MTL_ENOSPC` | 28 | capacity exhausted: queues, RL queues, scheduler quota, lcores, sessions per scheduler, region budget, an oversized cvideo codestream; the reason names the resource | free capacity or reduce the request |
| `-MTL_ENOMEM` | 12 | memory allocation failed (hugepages; `mtl_instance_get_mem_status` says where) | free memory |
| `-MTL_EBUSY` | 16 | the object is in use or the call is not allowed in this state (03 §3.2): destroy while referenced, attach while running, a second destroy, `acquire_buffer` on a buffer that is not FREE, `withdraw` of a picked-up unit, a group member started directly | later, or change state first |
| `-MTL_ENOTSUP` | 95 | a capability is absent (REQUIRE_DIRECT impossible, pacing class, backend feature, Windows wait objects in a build without them) | choose another option |
| `-MTL_EEXIST` | 17 | a name already used: session name (08), a named timeline opened with a different config (06) | pick another name / match the config |
| `-MTL_EDEADLK` | 35 | a WT (timeout ≠ 0), DPC-work or CP call from a library busy-loop thread (04 §3.3); AS calls are exempt | bug |

Rules:

- `-MTL_EIO` stops being the catch-all it is today (368 of ~890 `return -E*` sites, and the
  handle guard's "wrong type / dying" code) `[R13 §3]`: it means ERROR and nothing else.
- Unknown flag bits, and bytes beyond the library's known `struct_size` that are
  non-zero, return `-MTL_EINVAL` (reasons `UNKNOWN_BITS`, `NONZERO_TAIL`); they are never
  silently ignored (today no ops check masks flags `[R02 §4.4 #19]`). This is the
  compatibility policy of 11 §2, with `mtl_struct_known_size` for feature detection.
- `*_trywait` returns **1** (ready), **0** (armed, block now) or a negative code; never
  `-MTL_EAGAIN` (04 §5.3).
- `-MTL_EAGAIN` vs `-MTL_ETIMEDOUT` differ only by whether a timeout was passed and are
  handled alike; `-MTL_EBADF` vs `-MTL_ESTALE` are both bugs and differ in which part of the
  lease is wrong `[C5 §7.10]`.

### 5.2 Last error

`[C5 §2.16]`, replacing `mtl_last_error_detail()`:

```c
struct mtl_error_info {   /* 152 B */
  uint32_t struct_size;   /* set by the caller (mtl_error_info_init); the library writes at most that */
  int32_t  code;          /* the negative MTL_E* the failing call returned; 0 = none recorded */
  uint32_t reason;        /* enum mtl_state_reason */
  uint32_t reserved0;
  uint64_t call_seq;      /* per-thread sequence number of the failing call */
  char     detail[128];   /* MTL_ERROR_DETAIL_MAX. CP failures: human-readable; DP failures: empty */
};
int mtl_last_error(struct mtl_error_info* out);   /* DP; copies the calling thread's last error */
uint64_t mtl_call_seq(void);                      /* DP; the calling thread's call counter */
```

- every failing call sets it on the calling thread; a successful call does not clear it,
  so the caller compares `call_seq` with `mtl_call_seq()` taken before the call to know
  whether that call is the one that failed;
- DP failures set `code`, `reason` and `call_seq` only (no formatting on the data path);
- the copy goes into caller memory, so there is no lifetime or staleness question.

### 5.3 Per-call errors

Codes each call can return (✓), in addition to `-MTL_EBADF` for a null, foreign or retired
handle, which every call can return. `EINVAL` includes every argument check; `—` means never.
The state-dependent codes are those of 03 §3.2; "W" means only with `timeout ≠ 0`, and "DPC" only when the call would do caller-context work (from a busy-loop thread, 04 §3.3).

| Call | AGAIN | TIMEDOUT | CANCELED | SHUTDOWN | IO | NODEV | STALE | INVAL | RANGE | NOSPC | NOMEM | BUSY | NOTSUP | EXIST | DEADLK |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `mtl_instance_acquire_default` | — | — | — | — | — | ✓ `PORT_NOT_OPEN` | — | ✓ `INSTANCE_PARAM_MISMATCH` | — | ✓ | ✓ | — | ✓ | — | ✓ |
| `mtl_instance_release` | — | — | — | — | — | — | — | — | — | — | — | — | — | — | ✓ |
| `mtl_<media>_session_create` / `_query` | ✓ `MANAGER_LOST` (§7) | — | — | — | — | ✓ | — | ✓ | — | ✓ | ✓ | — | ✓ | ✓ name | ✓ |
| `mtl_session_attach_buffers` / `detach` | — | — | — | ✓ | — | — | — | ✓ layout, access | — | — | — | ✓ | ✓ | — | ✓ |
| `mtl_session_reconfigure` | — | — | — | ✓ | — | — | — | ✓ | — | ✓ | ✓ | ✓ | ✓ | — | ✓ |
| `mtl_session_start` | — | — | — | ✓ | ✓ | ✓ | — | ✓ | ✓ `BEYOND_HORIZON` | ✓ | ✓ | ✓ | ✓ | — | ✓ |
| `mtl_session_stop` | — | ✓ drain timeout | — | ✓ | — | — | — | ✓ | — | — | — | — | — | — | ✓ |
| `mtl_session_discard_queued` | — | — | — | ✓ | — | — | — | ✓ | — | — | — | ✓ | — | — | ✓ |
| `mtl_session_destroy` | — | — | — | — | — | — | — | ✓ flags | — | — | — | ✓ | — | — | ✓ |
| `mtl_session_update_flows` | — | ✓ ack | — | ✓ | ✓ | ✓ | — | ✓ | ✓ activation past | ✓ | ✓ | ✓ | ✓ | — | ✓ |
| `mtl_session_set_leg_enabled` | — | ✓ ack | — | ✓ | ✓ | — | — | ✓ leg | — | — | — | ✓ | — | — | ✓ |
| `mtl_tx_acquire` / `acquire_buffer` | ✓ | W | W | ✓ | ✓ | — | — | ✓ | — | — | — | ✓ `acquire_buffer` | — | — | W |
| `mtl_tx_submit` | — | — | — | ✓ | ✓ | — | ✓ | ✓ incl. `COOKIE_WITHOUT_RESULTS`, `HOLD_MISMATCH` | ✓ | ✓ `CODESTREAM_OVERSIZE` | — | ✓ 256th hold on one RX slot | — | — | DPC |
| `mtl_tx_publish` | — | — | — | ✓ | ✓ | — | ✓ | ✓ | — | — | — | — | — | — | — |
| `mtl_tx_release` | — | — | — | — | — | — | ✓ | ✓ | — | — | — | — | — | — | — |
| `mtl_tx_withdraw` (takes the base buffer) | — | — | — | ✓ | — | — | — | ✓ | — | — | — | ✓ | — | — | — |
| `mtl_tx_write` | ✓ | W | W | ✓ | ✓ | — | — | ✓ | ✓ | ✓ | — | — | — | — | ✓ |
| `mtl_tx_reap` / `mtl_cq_read` | ✓ contended (inline-safe only) | W | W | — | — | — | — | ✓ `record_size` | — | — | — | — | — | — | W |
| `mtl_rx_dequeue` | ✓ | W | W | ✓ | ✓ | — | — | ✓ | — | — | — | — | — | — | W, DPC |
| `mtl_rx_release` | — | — | — | — | — | — | ✓ | ✓ | — | — | — | — | — | — | — |
| `mtl_rx_transfer` | — | — | — | ✓ | ✓ | — | ✓ | ✓ `HOLD_MISMATCH` | — | — | — | ✓ | — | — | — |
| `mtl_eq_read` | ✓ | W | W | — | — | — | — | ✓ `ev_size` | — | — | — | — | — | — | W |
| `mtl_*_trywait` (returns 1 / 0) | — | — | ✓ | ✓ | ✓ | — | — | ✓ mask | — | — | — | — | — | — | — |
| `mtl_*_interrupt` / `uninterrupt` | — | — | — | — | — | — | — | — | — | — | — | — | — | — | `uninterrupt` only (CP); `interrupt` is AS and legal on any thread |
| `mtl_mem_import` / `alloc` | — | — | — | — | — | — | — | ✓ `UNALIGNED` | — | ✓ `REGION_BUDGET` | ✓ | — | ✓ | — | ✓ |
| `mtl_mem_destroy`, `mtl_buffer_destroy` | — | — | — | — | — | — | — | — | — | — | — | ✓ | — | — | ✓ |
| `mtl_group_start` | — | — | — | ✓ | ✓ | ✓ | — | ✓ | ✓ | ✓ | — | ✓ | ✓ | — | ✓ |
| `mtl_timeline_open` | — | — | — | — | — | — | — | ✓ | — | — | ✓ | — | — | ✓ config mismatch | ✓ |

### 5.4 `enum mtl_state_reason` (r3)

One enum carries every reason: in `SESSION_STATE` and other events (`mtl_event.reason`),
in `mtl_session_get_status().reason` / `last_error_reason`, and in `mtl_error_info.reason`
`[C5 §8.3]`. Values are grouped by hundreds so each group can grow without renumbering;
the header freezes them.

| Value | Name | Accompanies | Meaning / trigger |
|---|---|---|---|
| 0 | `NONE` | — | no reason |
| **1–99: lifecycle** | | | |
| 1 | `APP_REQUEST` | `SESSION_STATE` | a transition the app asked for (start, stop, destroy) |
| 2 | `START_INSTANT` | `SESSION_STATE` | ARMED → RUNNING at the start instant |
| 3 | `DRAIN_COMPLETE` | `SESSION_STATE` | DRAINING → STOPPED |
| 4 | `DRAIN_TIMEOUT` | `SESSION_STATE`, `-MTL_ETIMEDOUT` from stop | DRAINING → FLUSHING |
| 5 | `CMD_TIMEOUT` | ERROR, `-MTL_ETIMEDOUT` | a command was not acknowledged within `cmd_ack_timeout_ns` (04 §3.2) |
| 6 | `FORCED` | ERROR | `mtl_debug_inject(FORCE_ERROR)` |
| 7 | `WRONG_STATE` | `-MTL_EBUSY`, `-MTL_EIO`, `-MTL_ESHUTDOWN` | the call is not allowed in the current state (03 §3.2) |
| 8 | `GROUP_MEMBER` | `-MTL_EBUSY` | a group member started, stopped or reconfigured directly |
| 9 | `DESTROY_IN_PROGRESS` | `-MTL_EBUSY` | a second destroy |
| **100–199: device, port, network** | | | |
| 100 | `TX_QUEUE_FATAL` | ERROR, `FAILED` results | TX recovery could not get a new queue or mempool |
| 101 | `TX_QUEUE_HANG` | `SESSION_RECOVERY` | a TX hang recovery started (recoverable) |
| 102 | `LINK_DOWN` | `PORT_LINK`, `LEG_STATE` | the link of a port / leg is down (recoverable, Q-LIFE-7) |
| 103 | `PORT_RESET` | `PORT_RESET`, `-MTL_ENODEV` | VF reset or port restart; with `-MTL_ENODEV` when resources could not be restored |
| 104 | `DEVICE_GONE` | ERROR, `-MTL_ENODEV` | the device was removed |
| 105 | `BACKEND_FAILED` | ERROR | a kernel-socket or AF_XDP backend failure after retries |
| 106 | `PORT_NOT_OPEN` | `-MTL_ENODEV` | the port is not open in the default instance (v1, 03 §7.2) |
| 107 | `WAITING_NEIGHBOUR` | `FLOW_STATE`, `NO_NEIGHBOUR` results | the destination MAC is not resolved yet |
| 108 | `JOIN_FAILED` | `FLOW_STATE` | the IGMP join or the flow rule failed |
| 109 | `LEG_DISABLED` | `LEG_STATE` | the leg is administratively disabled |
| 110 | `MANAGER_LOST` | `MANAGER_LOST`, `-MTL_EAGAIN` from create | MtlManager is unreachable (§7) |
| 111 | `SCHED_OVERLOAD` | `SCHED_OVERLOAD` | a scheduler exceeded its busy threshold |
| **200–299: configuration and arguments** | | | |
| 200 | `INVALID_ARGUMENT` | `-MTL_EINVAL` | generic; `detail` names the field (CP) |
| 201 | `UNKNOWN_BITS` | `-MTL_EINVAL` | unknown flag bits |
| 202 | `NONZERO_TAIL` | `-MTL_EINVAL` | non-zero bytes beyond the known `struct_size`, or `struct_size` 0 / too small |
| 203 | `WRONG_DIRECTION` | `-MTL_EINVAL` | a TX verb on an RX session or vice versa |
| 204 | `INSTANCE_PARAM_MISMATCH` | `-MTL_EINVAL` | an invariant field of the default instance differs (03 §7.2) |
| 205 | `COOKIE_WITHOUT_RESULTS` | `-MTL_EINVAL` | a non-zero `user_cookie` in completion NONE (§2.2) |
| 206 | `MEDIA_TIME_BACKWARDS` | `-MTL_EINVAL` | media time not increasing without DISCONTINUITY, or a backwards DISCONTINUITY while RUNNING |
| 207 | `PASSTHROUGH_AUTO` | `-MTL_EINVAL` | RTP passthrough with media mode AUTO (06 §5.4) |
| 208 | `LAYOUT_MISMATCH` | `-MTL_EINVAL` | a buffer layout does not satisfy the requirements |
| 209 | `ACCESS_MISMATCH` | `-MTL_EINVAL` | a region's access flags do not allow the direction (RX needs WRITE, TX needs READ) |
| 210 | `UNALIGNED` | `-MTL_EINVAL` | an import's `va`/`length` is not page-aligned (hugepage-aligned for hugetlbfs) (05) |
| 211 | `POOL_INCOMPLETE` | `-MTL_EINVAL` | start with fewer attached buffers than `pool.count` |
| 212 | `POOL_TOO_SMALL` | `-MTL_EINVAL`, `-MTL_ENOTSUP` | `pool.count` below `min_count` (or below `min_count_direct` with REQUIRE_DIRECT) |
| 213 | `RECONFIGURE_INCOMPATIBLE` | `-MTL_EINVAL` | an attached pool does not fit the reconfigured media (05 §5) |
| 214 | `NAME_EXISTS` | `-MTL_EEXIST` | a session name is already used in the instance |
| 215 | `TIMELINE_CONFIG_MISMATCH` | `-MTL_EEXIST` | a named timeline opened with a different config |
| 216 | `BUSY_LOOP_THREAD` | `-MTL_EDEADLK` | a forbidden call from a library busy-loop thread |
| 217 | `RX_TIMELINE_LAZY` | `-MTL_EINVAL` | an RX session on an AT_START or NEXT_GRID timeline (06 §11.1) |
| 218 | `FIELD_REQUIRED` | `-MTL_EINVAL` | a required field is zero (09 §9.3); `detail` names it |
| 219 | `STRIDE_MISMATCH` | `-MTL_EINVAL` | attached buffers of one pool with different strides (05 §4.2) |
| 220 | `HOLD_MISMATCH` | `-MTL_EINVAL` | a TX plane outside the held RX buffer, or no matching buffer for `mtl_rx_transfer` (05 §5.4) |
| 221 | `SPAN` | `-MTL_EINVAL` | a buffer outside one library-pool slot or outside its region (05 §5.3) |
| 222 | `REGION_NOT_MAPPED` | `-MTL_EINVAL` | a dynamic acquire over a region not yet mapped into the session's devices (05 §5.5) |
| 223 | `MIXED_BACKING` | `-MTL_EINVAL` | an import spanning VMAs with different backings (05 §3.2) |
| 224 | `PORT_CHANGE_NEEDS_RECONFIGURE` | `-MTL_EINVAL` | `update_flows` tried to change `flow.port` (09 §7.1) |
| 225 | `GRID_MISMATCH` | `-MTL_EINVAL` | a reconfigured unit period no longer fits the group grid (09 §7.3) |
| **300–399: capacity and memory** | | | |
| 300 | `CAPACITY_TX_QUEUES` | `-MTL_ENOSPC` | no free TX queue |
| 301 | `CAPACITY_RX_QUEUES` | `-MTL_ENOSPC` | no free RX queue / flow rule |
| 302 | `CAPACITY_RL_QUEUES` | `-MTL_ENOSPC`, `-MTL_ENOTSUP` | no free rate-limited queue |
| 303 | `CAPACITY_SCHED_QUOTA` | `-MTL_ENOSPC` | no scheduler has quota left |
| 304 | `CAPACITY_LCORES` | `-MTL_ENOSPC` | no free lcore |
| 305 | `CAPACITY_SESSIONS` | `-MTL_ENOSPC` | the per-scheduler session table is full |
| 306 | `REGION_BUDGET` | `-MTL_ENOSPC` | the device's region (memseg) budget is exhausted (05) |
| 307 | `CODESTREAM_OVERSIZE` | `-MTL_ENOSPC` | an ST22 `valid_bytes` larger than the granted size |
| 308 | `HUGEPAGES` | `-MTL_ENOMEM` | hugepage allocation failed |
| 309 | `NUMA_MISMATCH` | `-MTL_EINVAL` (with `MTL_MEM_NUMA_REQUIRED`), status warning otherwise | the memory is not on the ports' socket |
| 310 | `DIRECT_IMPOSSIBLE` | `-MTL_ENOTSUP` | REQUIRE_DIRECT cannot be granted (CONVERT, ST22, PA mode, …) |
| 311 | `PACING_UNAVAILABLE` | `-MTL_ENOTSUP` | the requested pacing class cannot be granted |
| 312 | `POOL_COUNT_MAX` | `-MTL_ERANGE` | `pool.count` above `max_count` (8 for video until E11, 05 §5.2) |
| **400–499: timing** | | | |
| 400 | `BEYOND_HORIZON` | `-MTL_ERANGE` | a unit, or at start a preroll unit, beyond the horizon measured from the resolved start (06) |
| 401 | `START_IN_PAST` | `-MTL_ERANGE` | an `AT_MEDIA_INDEX` / `AT_TAI` start or activation whose instant has passed |
| 402 | `LAUNCH_IN_PAST` | `-MTL_ERANGE` | an exact launch in the past |
| 403 | `TIMING_SHORTFALL` | `timing_warning`, `TIMING_INFEASIBLE` | CAPTURE units arrive after their deadline |
| 404 | `LINK_OFFSET_BUDGET` | `timing_warning` | `link_offset_budget_ns` exceeded (06) |
| 405 | `TIME_ESTIMATED` | `timing_warning`, `TIME_STATE` | the time source is ESTIMATED (e.g. `CLOCK_TAI` with a zero kernel TAI offset, 06 §2.2) |
| 406 | `TIME_STEP` | `TIME_STEP` | a declared clock step |

G-57 is table-driven over this enum: every value has a documented trigger that a test can
produce, through `mtl_debug_inject` where no natural trigger exists (13).

## 6. Exactly-once, stated as testable identities

For every session, at every instant:

```text
accepted TX submissions = results published + results suppressed by the completion mode + units not yet terminal
at quiescence (STOPPED):  accepted = published + suppressed;   rejected ∩ (published ∪ suppressed) = ∅
RX slots = FREE + RECEIVING + READY + APP_READING + HELD        (r3: HELD = released, hold count > 0)
```

Gauges come from one scan of the slot state words, so the slot identity holds by
construction; the testable form is the **transition identity** per state (entries − exits =
gauge, checked at quiescence and under stress), which is how 13 rewrites G-43 `[C5 §8.9]`.
These identities become contract tests ([13](13-guarantees-and-tests.md)), including under
fault injection (drop the link, force recovery, destroy mid-stream). Issue #1147
(`notify_frame_done` called twice with ext frames) and the downstream watchdog patches for
frames abandoned without any completion `[R08 §3.3]` are exactly what these tests catch.

## 7. MtlManager loss (r3)

`[C5 §8.6]`. Lcore acquisition goes through the MtlManager socket whenever the instance
connected to it at init (`mt_sch_get_lcore`, `mt_sch.c:713-731`). Today:

- `send()` on the manager socket has no `MSG_NOSIGNAL` (`mt_instance.c:17`, `:69`) and the
  library never ignores SIGPIPE, so after the manager dies the next request raises SIGPIPE,
  which terminates an app that has not ignored it; otherwise it returns `-EIO` with no
  fallback. The manager's own `send` has the same gap (`manager/mtl_instance.hpp:58`; its
  signalfd covers SIGINT only, `manager/mtl_manager.cpp:57-69`) (side findings; SP-08);
- there is no reconnect, and on a client disconnect the manager frees that client's lcores
  (`manager/mtl_instance.hpp:66-68`), so a restarted manager can grant lcores that live
  processes still use;
- `mtl_is_manager_alive()` opens a new connection per call, is process-global, and logs at
  `err` when no manager runs (`mt_instance.c:255-273`, `:266`) — the normal single-instance
  mode, where init itself only warns (`:199-204`).

The unified contract:

| Situation | Outcome |
|---|---|
| manager lost while sessions run | running sessions continue (13 §6); `MANAGER_LOST` event; `mtl_instance_get_status().manager = LOST` (a cached field, no connection per call) |
| create (or anything needing an lcore) after the loss | `-MTL_EAGAIN` with reason `MANAGER_LOST` — retry once it reconnects (addendum K: the r3 fallback flag `MTL_INSTANCE_MANAGER_OPTIONAL` and its shared-memory lcore allocator are removed, D-92) |
| manager restarts | the instance reconnects in the background (admin thread, back-off), re-registers, and **re-announces every lcore it holds**, so the new manager does not grant them again; `MANAGER_LOST` (reconnected) event; status `CONNECTED` |
| no manager configured | status `NOT_CONFIGURED`; nothing is logged at `err` |
| every manager write | uses `MSG_NOSIGNAL`; a broken pipe is `MANAGER_LOST`, never a signal |
