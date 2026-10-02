# 03 — Object model, handles and lifecycle

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-OBJ-*, R-LIFE-*, R-MEM-3, R-CMP-1 |
| Research | [R13 lifecycle](research/13-lifecycle-errors-abi.md), [R01 PR analysis](research/01-pr1610-analysis.md), [R00 review §7–9](research/00-pr1610-design-review.md); reviews [C1](reviews/C1-realtime-feasibility.md), [C3](reviews/C3-usability-personas.md), [C4](reviews/C4-consistency-audit.md), [C5](reviews/C5-adversarial-user-review.md) and the [C5 response](reviews/C5-response.md) (Part A is normative for names) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

Revision 3 changes, in short: typed handles with a real null and a separate lease type
(§2); the error-code split and a RETIRED getter in the state × call table (§3.2); an ERROR
column in the stop table (§3.4); RX start semantics (§3.5); RX hold counts and stride-aware
DIRECT for shared buffers (§4.3); the exported-pool rule (§4.4); a refcounted instance that
outlives destroying sessions, with a merge table for the default instance (§7). Items
marked **r3** changed in this revision.

## 1. Objects and ownership

```text
                         ┌──────────────── mtl_instance_h ───────────────┐
                         │ time source · ports · schedulers · refcount    │
                         └───┬───────────┬──────────┬──────────┬─────────┘
                             │           │          │          │
                        mtl_region   mtl_timeline  mtl_cq    mtl_eq          mtl_group
                        (bytes+maps) (T0, grid;   (results) (events,        (arm/start set)
                             │        may be named)  ▲       subscriptions)      │
                             ▼           │          │          ▲                 │
                        mtl_buffer ◀─────┼──────────┼──────────┼──── pool ──┐    │
                        (layout)         │          │          │            │    │
                             │ lease     ▼          │          │            │    ▼
                             └──────▶ mtl_session ──┴── bound ─┴────────────┘  member
                          mtl_lease_h  (media × direction, flows, state, lease table)
```

| Object | Purpose | Created by | Destroyed by | References it holds | Destroy blocked while |
|---|---|---|---|---|---|
| instance | owns NIC ports, schedulers, time source, instance EQ | `mtl_instance_acquire_default` (refcounted), `mtl_instance_open` (a private instance), or `mtl_instance_from_legacy(mtl_handle)` (§7) | **r3** `mtl_instance_release` drops one reference; teardown follows the last reference *and* the last DESTROYING session (§7.1) `[C5 §7.1]` | — | never refused (refcount only) |
| memory region | retained bytes + per-device mappings | `mtl_mem_alloc`, `mtl_mem_import`; a library pool also exposes its region (`mtl_session_get_pool_region`, 05) | `mtl_mem_destroy` | instance | any buffer or in-flight operation references it (`-MTL_EBUSY`) |
| buffer | immutable layout over one or more regions | session pool (implicit) or `mtl_buffer_create` | pool teardown or `mtl_buffer_destroy` | regions | attached to a session, leased, or held (§4.3) (`-MTL_EBUSY`) |
| session | one essence × direction stream | `mtl_<media>_session_create` (`video`, `cvideo`, `audio`, `anc`, `fastmeta`) | `mtl_session_destroy` | instance, buffers, CQ, EQ, timeline, group | never refused; with leases outstanding it is *deferred* (§6.3) |
| CQ | results and RX deliveries | private per session (implicit), or `mtl_cq_create` (shared poll set) | session retire / `mtl_cq_destroy` | — | a session is bound to it (`-MTL_EBUSY`) |
| EQ | bounded events with a subscription mask | private per session and one per instance (implicit), or `mtl_eq_create` | owner retire / `mtl_eq_destroy`; **r3** refcounted: a private EQ obtained with `mtl_session_get_eq` stays readable after the session retires until the app drops it with `mtl_eq_destroy` (07 §3.4) | — | never refused (refcount) |
| timeline | exact rational media-time anchor | `mtl_timeline_epoch(mt)` (the epoch timeline has a real handle), `mtl_timeline_create`, or `mtl_timeline_open(name)` (refcounted) | `mtl_timeline_destroy` / last close | instance | a session uses it |
| group | all-or-nothing arm/start/stop | `mtl_group_create` | `mtl_group_destroy` | timeline, sessions | while armed or running |

The rule that makes teardown predictable: **an object can be destroyed only after
everything that references it has released it**, and the failure is an error code, not
undefined behaviour. Two objects are exceptions by design: destroying a *session* always
succeeds (it defers while the application holds leases, §6), and releasing the
*instance* only drops a reference (§7.1).

## 2. Handles

### 2.1 Types, null and equality (r3)

All handles are small value types that wrap a 64-bit identifier, not pointers
`[C5 §2.5, §2.8, §2.9]`:

```c
typedef struct mtl_instance_h { uint64_t id; } mtl_instance_h;   /* replaces mtl_handle in every new prototype */
typedef struct mtl_session_h  { uint64_t id; } mtl_session_h;
typedef struct mtl_buffer_h   { uint64_t id; } mtl_buffer_h;     /* base buffer only */
typedef struct mtl_lease_h    { uint64_t id; } mtl_lease_h;      /* one access grant on one pool slot */
typedef struct mtl_region_h   { uint64_t id; } mtl_region_h;
typedef struct mtl_cq_h       { uint64_t id; } mtl_cq_h;
typedef struct mtl_eq_h       { uint64_t id; } mtl_eq_h;
typedef struct mtl_timeline_h { uint64_t id; } mtl_timeline_h;
typedef struct mtl_group_h    { uint64_t id; } mtl_group_h;
```

- **ID 0 is the null handle of every type.** Every call rejects a null handle with
  `-MTL_EBADF`. Each type has `MTL_<TYPE>_NULL`, `mtl_<type>_is_null()` and
  `mtl_<type>_eq()` (static inline, with exported twins for bindings). A null handle is
  never a live object: every live ID has a non-zero generation (§2.2).
- **In a config struct a null handle means "the documented default"** (for example a null
  `timing.timeline` means the epoch timeline). Zero-filled configs therefore never name a
  live object by accident, which was the `{0}` ambiguity of revision 2 `[C5 §2.8]`.
- **Leases are their own type.** Calls that move access (`mtl_tx_submit`,
  `mtl_tx_publish`, `mtl_tx_release`, `mtl_rx_release`, `mtl_rx_transfer`) take and
  return only `mtl_lease_h`; passing a base buffer does not compile.
  `mtl_lease_buffer(lease)` gives the base buffer, `mtl_lease_slot(lease)` the pool slot
  index (the "cache by index" key, 10). A TX lease goes stale at a successful submit,
  except with `MTL_UNIT_ROWS`, where it stays current for `mtl_tx_publish` until the FINAL
  publish (06 §9). Calls that do not move access, or that act on a unit whose lease is
  already stale (`attach`, `get_view`, `mtl_tx_acquire_buffer`'s target,
  `mtl_tx_withdraw(s, buffer)`), take the base `mtl_buffer_h`.
- `mtl_instance_from_legacy(mtl_handle)` bridges an instance made by today's `mtl_init`;
  `mtl_instance_to_legacy()` goes the other way, so legacy and unified sessions share one
  instance (GO-9).
- Typed getters replace the untyped `mtl_stat_get_u64(uint64_t object, …)`:
  `mtl_session_stat_get`, `mtl_port_stat_get`, `mtl_instance_stat_get` (08).

### 2.2 Encoding and tables (r3)

> **Addendum K (2026-10-01).** The object tables are process-wide, grow-only and never
> freed, not per instance (R4 in `mtl.h`, EK20): an object closed by its instance's shutdown
> keeps a slot in the CLOSED_BY_INSTANCE state, so its handle stays safe to pass after the
> instance is gone ([16 §2.1](16-kubernetes-and-crash-safety.md)).

```text
object handle = | type:8 | reserved:8 | index:16 | generation:32 |   instance, session, buffer, region, cq, eq, timeline, group
lease handle  = | session index:16 | slot:16 | generation:32 |         mtl_lease_h (the type is the C type)
```

- **Object tables are grow-only chunked arrays**, one per type and per instance: a
  top-level array of 256 chunk pointers (2 KiB) × 256 entries per chunk = 65 536 objects per
  type. A chunk is allocated on the control plane when the previous one is full and is
  never freed before instance teardown, so a stale handle always indexes valid memory
  and fails on the generation compare with `-MTL_EBADF` instead of reading freed memory.
  Nothing is preallocated at 65 536, and nothing is capped at 256 `[C5 §4.11 "Handle tables"]`.
  The largest per-instance count the engine allows today is
  18 schedulers × (60 + 60 video + 512 + 1024 audio) = 29 808 sessions
  (`st_header.h:33-34`, `:48`, `:50`; `MT_MAX_SCH_NUM = 18`), well inside 16 bits.
- **Generations** start from a random seed per table slot (object handles) and per session
  (leases), skip 0, and advance on every reuse. Revision 2's 8-bit session tag in the lease
  made a foreign lease a 1-in-256 check; a lease now carries the full 16-bit session index,
  so a lease of another session fails **deterministically** on the index with
  `-MTL_EBADF`, and an old lease of the right session fails on the generation with
  `-MTL_ESTALE` `[C5 §2.9]`.
- **Free-slot reuse is FIFO**, so a retired slot is reused as late as possible. Until
  reuse the slot is a *tombstone*: `mtl_session_get_state()` on the retired handle returns
  `MTL_STATE_RETIRED`, and every other call returns `-MTL_EBADF` (§6.1) `[C5 §7.1]`.
- This is the stale-safety PR #1610's `GRACEFUL_SHUTDOWN.md` promised but never
  implemented, and that today's `mt_handle_guard.h` cannot give because it reads
  `impl->type` through the raw pointer (`mt_handle_guard.h:75-95`) `[R01 §5 #5, R13 §1.3]`.
- Submitting or releasing an old lease (double submit, submit after release, release
  twice) fails with `-MTL_ESTALE` and cannot corrupt another lease — the fix for PR #1610
  defect D4 (double `buffer_put` re-queued an in-flight frame) `[R01 D4, R00 §14 #7]`.

### 2.3 Cost and destroy protection

A lookup is a chunk index, an array index and a 32-bit compare. Data-plane calls also need
protection against a concurrent destroy. Review C1 showed that a "state gate + QSBR grace
period" is not the cheap relaxed load the draft claimed: a preempted reader must announce
itself, and `rte_rcu_qsbr` with arbitrary application threads costs a fence per call
`[C1 #5]`. Proposal (Q-THR-5):

| Mechanism | Per-call cost | Notes |
|---|---|---|
| Today's guard: SEQ_CST RMW on `lc_refcnt` in the pipeline context | one RMW on a line the tasklet reads | the measurable cost is false sharing with `impl`/`idx`/`type` (`st20_pipeline_tx.h:35-42`) `[R03 §8]` |
| **Per-object in-flight counter on its own cache line + generation-tagged handles + type-stable slots** | one RMW on a line shared only by application threads (never by a tasklet); the cost is a spike S3 output, not a promise (revision 2's "~20 ns on an app-owned line" was unmeasured, `[C5 §4.11]`) | recommended; destroy waits for the counter to drain |
| QSBR / epoch per thread | fence per call for arbitrary app threads | revisit only if a packet-rate DP API appears (Q-MODE-1) |

**r3 elision** `[C5 §4.11 "RMW count"]`: a session created with
`MTL_SESSION_SINGLE_READER` and without `MTL_SESSION_MT_SUBMIT` declares that its
acquire/submit/reap/dequeue calls come from one context and are never concurrent with its
`mtl_session_destroy`. For those calls the in-flight counter and the reaper lock are
elided (04 §4.1). `mtl_tx_release` and `mtl_rx_release` keep the counter, because they
are legal from any thread during a deferred destroy.

## 3. Session lifecycle

### 3.1 States

```c
enum mtl_session_state {
  MTL_STATE_UNSET = 0,    /* never returned; a zeroed status record reads UNSET, not CREATED (r3) */
  MTL_STATE_CREATED = 1,  /* configurable; resources reserved (Q-LIFE-9); nothing on a tasklet yet */
  MTL_STATE_ARMED,        /* start instant in the future; TX preroll accepted; RX joined and discarding (§3.5) */
  MTL_STATE_RUNNING,
  MTL_STATE_DRAINING,     /* stop(DRAIN): queued units still being sent / received units being completed */
  MTL_STATE_FLUSHING,     /* stop(FLUSH), drain timeout, or ERROR entry: queued units being flushed */
  MTL_STATE_STOPPED,      /* like CREATED, with history: buffers attached, flows joined, stats kept; can start again */
  MTL_STATE_ERROR,        /* the library cannot continue without the app: only stop/destroy, getters, release, reads */
  MTL_STATE_DESTROYING,   /* destroy called; deferred while app leases are out (§6.3) */
  MTL_STATE_RETIRED,      /* r3: returned by mtl_session_get_state() for a retired handle until its slot is reused */
};
int mtl_session_get_state(mtl_session_h s);   /* DP; r3: returns the state (>= 1) or a negative code */
```

`mtl_session_get_state` is the cheap data-plane getter and returns the state as its
non-negative result; `mtl_session_get_status()` is the full control-plane copy, including
`reason` (`enum mtl_state_reason`, 07 §5.4) (A6).

```text
             create()
  (none) ────────────▶ CREATED ──────────────────────────────────────────────────┐
                        │  attach buffers, bind CQ/EQ, join group, set timeline,   │
                        │  reconfigure, update_flows, set_leg_enabled               │
                        │ start(now | at T | at media index)                        │
                        ▼                                                           │
                      ARMED ── start instant reached ──▶ RUNNING                   │
                        │                         │   │ discard_queued() (stays)    │
                        │ stop()                  │   │ stop(DRAIN)                 │
                        │                         │   ▼                             │
                        │                         │ DRAINING ── all accepted ───────┤
                        │                         │   │ timeout      units final    │
                        │                         │   ▼                             ▼
                        └────────────────────────▶ FLUSHING ──────────────────▶ STOPPED ── start() ─▶ ARMED/RUNNING
                                     stop(FLUSH)  ▲                               ▲       (re-reserves after ERROR, §3.6)
                                                  │ ERROR entry flushes           │
                 fatal fault (any started state) ─┴──▶ ERROR ── stop() ───────────┘
 destroy() from any state ─▶ DESTROYING ─▶ (device references gone, app leases returned) ─▶ RETIRED (tombstone)
```

PR #1610 auto-started at create and its `stop()` only toggled an application-side flag
while TX kept transmitting `[R01 §1.4]`; here the state is real.

### 3.2 What each state allows (r3)

Codes are the `MTL_E*` constants of 07 §5; "yes" means the call does its normal work. CQ
and EQ calls on a bound shared queue follow the queue, not the session. The DP rows use
the A3/A5 conventions: array reads return a count ≥ 0 (0 only with `timeout == 0`),
single-object verbs return 0 `[C5 §3.2, §8.3]`.

| Call | CREATED / STOPPED | ARMED | RUNNING | DRAINING | FLUSHING | ERROR | DESTROYING | RETIRED |
|---|---|---|---|---|---|---|---|---|
| attach / detach buffers | yes | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` (stop first) | `-ESHUTDOWN` | `-EBADF` |
| bind CQ/EQ, join group, set timeline, set option | yes | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-ESHUTDOWN` | `-EBADF` |
| **r3** `mtl_session_reconfigure` (media config, pool count, legs) | yes: keeps handle, name, flows, SSRC, stats, timeline, group, CQ/EQ bindings, RX rules and joins; library pools are re-created, attached pools that no longer fit are `-EINVAL` (`RECONFIGURE_INCOMPATIBLE`) (05 §5, 09 §7.3) `[C5 §7.4]` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-ESHUTDOWN` | `-EBADF` |
| `mtl_session_start` | yes; from STOPPED after ERROR it re-reserves device resources or fails `-ENODEV` (§3.6) | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EIO` (stop first) | `-ESHUTDOWN` | `-EBADF` |
| `mtl_session_stop` | 0 (no-op) | yes | yes | FLUSH escalates; a second DRAIN waits for the first | waits for the flush | yes → STOPPED | `-ESHUTDOWN` | `-EBADF` |
| **r3** `mtl_session_discard_queued` | yes: held preroll units → `FLUSHED/DISCARD` | yes | yes | `-EBUSY` | 0 (no-op) | 0 (no-op: already flushed) | `-ESHUTDOWN` | `-EBADF` |
| **r3** `mtl_session_update_flows` | yes, applied by the CP at once with NOW semantics | yes: boundary command (04 §3.2) | yes: boundary command | yes: boundary command | yes: boundary command | `-EIO` | `-ESHUTDOWN` | `-EBADF` |
| **r3** `mtl_session_set_leg_enabled` | yes (configuration) | yes: boundary command | yes: boundary command | yes: boundary command | yes: boundary command | `-EIO` | `-ESHUTDOWN` | `-EBADF` |
| TX `mtl_tx_acquire` / `mtl_tx_acquire_buffer` | yes (preroll, Q-LIFE-5) | yes | yes | `-ESHUTDOWN` | `-ESHUTDOWN` | `-EIO` | `-ESHUTDOWN` | `-EBADF` |
| TX `mtl_tx_submit` / `mtl_tx_publish` | yes → held until start | yes → held | yes | `-ESHUTDOWN` | `-ESHUTDOWN` | `-EIO` | `-ESHUTDOWN` | `-EBADF` |
| TX `mtl_tx_release` (own unsubmitted lease) | yes | yes | yes | yes | yes | yes | **yes** | `-EBADF` |
| TX `mtl_tx_withdraw` (a queued unit) | yes | yes | yes | yes | `-EBUSY` (being flushed) | `-EBUSY` (already flushed) | `-ESHUTDOWN` | `-EBADF` |
| TX `mtl_tx_reap`, `mtl_cq_read` on the private CQ | yes | yes | yes | yes | yes | yes | yes (entries left at retire are counted and discarded) | `-EBADF` |
| RX `mtl_rx_dequeue` | STOPPED: remaining READY units, then `-EAGAIN` (timeout 0) or a normal wait ending `-ETIMEDOUT`; CREATED: same | `-EAGAIN` / waits | yes | force-completed and READY units, then `-ESHUTDOWN` | READY units, then `-ESHUTDOWN` | READY units, then `-EIO` | `-ESHUTDOWN` | `-EBADF` |
| RX `mtl_rx_release` | yes | yes | yes | yes | yes | yes | **yes** | `-EBADF` |
| `mtl_session_get_eq`, `mtl_eq_read` / `mtl_eq_trywait` / `mtl_eq_get_wait_object` on the private EQ | yes | yes | yes | yes | yes | yes | **yes** `[C5 §7.1]` | `get_eq` `-EBADF`; an EQ handle obtained earlier stays valid (07 §3.4) |
| `mtl_session_interrupt` / `uninterrupt` | yes | yes | yes | yes | yes | yes | 0 (no-op: destroy wins, 04 §5.4) | `-EBADF` |
| `mtl_session_get_state` | state | state | state | state | state | state | state | **`MTL_STATE_RETIRED`** |
| `get_status` / `get_stats` / `get_info` | yes | yes | yes | yes | yes | yes | yes | `-EBADF` |
| `mtl_session_destroy` | yes | yes | yes | yes | yes | yes | `-EBUSY` (in progress); with `MTL_DESTROY_FORCE` it escalates (§6.3) | `-EBADF` |

In the table the prefix `MTL_` is dropped for width: `-EBUSY` is `-MTL_EBUSY`. A WT call
that returns `-MTL_ESHUTDOWN` or `-MTL_EIO` in the table does so immediately; one that is already
blocked when the session enters DRAINING/FLUSHING/ERROR/DESTROYING is woken with the same
code once nothing remains for it. A WT call that *starts* in CREATED, ARMED or STOPPED
waits normally (a unit can become available after start) and ends with `-MTL_ETIMEDOUT`, so a
concurrent `dequeue` during a stop/reconfigure/start cycle never reads as end of stream
`[C5 §8.3]`.

Rules that drive the table:

1. **Results are always drainable** until the session retires. PR #1610's `event_poll`
   returned `-EAGAIN` after `stop()` before reading the ring, which made its own shutdown
   recipe impossible `[R01 R4]`.
2. **A lease the app holds stays valid across stop and destroy.** Stop never yanks memory
   the app is reading or writing; destroy defers until the leases come back (§6.3).
3. **`-MTL_ESHUTDOWN` only ever means "you asked"** (stop, discard, destroy by some
   thread of the app); `-MTL_EIO` means "the session failed" and the reason is in
   `mtl_session_get_status().reason`; `-MTL_EAGAIN`/`-MTL_ETIMEDOUT` mean "nothing yet"
   (A5) `[C5 §8.3, §2.10]`.

### 3.3 Create and start

- **Create reserves** queues, scheduler quota, the lease table, and flow-rule capacity
  (validated with `rte_flow_validate`, installed at the first start, §3.5), so failures
  surface early and start is fast and predictable (Q-LIFE-9 (a)); nothing runs on a
  tasklet yet.
- **r3: create and start never wait for ARP or IGMP** `[C5 §7.6]`. Today `tv_init_hdr`
  resolves the destination MAC at create (`st_tx_video_session.c:903-931`, `:926`) and
  `arp_get_result` sleeps in 500 ms steps up to the ARP timeout, 60 s by default
  (`mt_arp.c:171-199`), which is why the GStreamer sink grew `async-session-create`. In the
  unified API create posts ARP resolution to a worker and returns; an unresolved TX leg
  leaves the session ARMED/RUNNING with the leg's flow state `WAITING_NEIGHBOUR`, and its
  units are `DROPPED/NO_NEIGHBOUR` on that leg until resolution (DROPPED overall when
  every leg is unresolved). The `FLOW_STATE` event and `mtl_session_get_status()` report
  it (07 §3.2). `MTL_FLOW_USER_MAC` bypasses ARP entirely. A group start therefore never
  blocks on one unicast member.
- A **dry run** `mtl_<media>_session_query(mt, &sc, &mc, &info, &req)` validates a
  configuration and returns granted values and buffer requirements without allocating
  anything — what framework caps negotiation needs, and the true `fi_getinfo` counterpart
  `[C3 P2-7, P7-1]`. With `MTL_QUERY_CHECK_CAPACITY` it also checks free capacity (08).

```c
struct mtl_start_params {
  uint32_t struct_size;
  uint32_t mode;            /* MTL_START_NOW (0) | MTL_START_AT_TAI | MTL_START_AT_MEDIA_INDEX */
  int64_t  media_index;     /* AT_MEDIA_INDEX: first unit index on the session's timeline */
  int64_t  tai_ns;          /* AT_TAI */
  int64_t  preroll_ns;      /* extra lead added when the start instant is resolved (06 §3.1) */
  uint64_t reserved[2];
};
int mtl_session_start(mtl_session_h s, const struct mtl_start_params* p);   /* p may be NULL = NOW */
```

The shape is illustrative; the header (10) is normative.

- **Start re-checks** what can have changed since create (pool complete, layouts accepted
  by the granted data path, unread-results room, device resources still valid after an
  ERROR, §3.6) and resolves the timeline anchor if it is lazy (06 §3.1); it either reaches
  ARMED/RUNNING or fails with nothing changed `[R00 §14 #29]`. If any preroll unit lies
  beyond the horizon measured from the resolved start instant, start fails atomically with
  `-MTL_ERANGE`, reason `BEYOND_HORIZON`, and nothing starts (06 §7.4). `preroll_ns` is
  the extra lead added when that instant is resolved.
- **Lazy anchors are TX-only.** An RX session on an AT_START or NEXT_GRID timeline is
  `-MTL_EINVAL`, reason `RX_TIMELINE_LAZY`: a receiver has no lead and no start instant
  to resolve the anchor with (06 §11.1).
- A session in a group is started only through the group (`-MTL_EBUSY`, reason
  `GROUP_MEMBER`, otherwise), except the rejoin case of §5.
- Start never runs on a tasklet. It posts the attach to the scheduler as a command
  (04 §3.2); tasklet-type consumers use the register/ack handshake in
  `mt_sch.c:925-966` `[C1 #19]`.

### 3.4 Stop, discard, drain, flush and ERROR entry (r3)

> **Addendum K (2026-10-01).** stop(FLUSH) and instance close also finish the unit whose
> first packet already left: it is sent to its end at its pace and gets its normal status
> (rows units end by `tx.rows_late`, STALL as TRUNCATE). `mtl_instance_abort` cuts it:
> `FLUSHED/ABORTED` with `MTL_TXR_PKT_SHORT` ([16 §2.2](16-kubernetes-and-crash-safety.md)).

| | `discard_queued()` | stop(DRAIN, timeout) | stop(FLUSH) | **ERROR entered** `[C5 §8.3]` |
|---|---|---|---|---|
| State after | stays (RUNNING/ARMED/CREATED/STOPPED) | STOPPED | STOPPED | ERROR (then `stop` → STOPPED) |
| New TX submissions | accepted after it returns; the next is an implicit DISCONTINUITY; with `MTL_DISCARD_REBASE` the session's `media_index_offset` is rebased so `first_index` maps to the next feasible slot (06 §10.8) | refused (`-ESHUTDOWN`) | refused (`-ESHUTDOWN`) | refused (`-EIO`) |
| Queued, not picked up | `FLUSHED/DISCARD` | sent in order at their slots | `FLUSHED/STOP_FLUSH` | `FLUSHED/SESSION_ERROR` |
| Picked up, waiting for its launch (no packet handed to the NIC yet) | `FLUSHED/DISCARD` | sent | `FLUSHED/STOP_FLUSH`: the stop is an immediate command, so a TX waiting up to 1 s for its launch does not delay it (04 §3.2) `[C5 §4.2]` | `FLUSHED/SESSION_ERROR` |
| Packets already handed to the NIC | completes | completes | completes (cannot be recalled) | `FAILED/<reason>` (for example `TX_QUEUE_FATAL`) once every device reference is gone (queue reset, 04 §4.5) |
| Timeout reached | — | remaining queued → `FLUSHED/STOP_TIMEOUT` | — | — |
| RX units being assembled | force-completed and delivered or discarded per `rx_incomplete`, as at the due time (06 §11.7) | force-completed and delivered with their completeness status (04 §4.4 RX deadline hook) | discarded, counted | discarded, counted |
| RX READY, not dequeued | discarded | stay dequeuable | stay dequeuable | stay dequeuable |
| Blocked callers | unaffected | woken with `-ESHUTDOWN` once nothing remains for them | woken with `-ESHUTDOWN` | woken with `-EIO` |
| Returns | 0 | 0 when every accepted unit has a terminal outcome; `-ETIMEDOUT` if the drain timed out (FLUSH semantics applied) | 0 | — (entry is a library transition; `SESSION_STATE` event with the reason) |

Codes in this table drop the `MTL_` prefix, as in §3.2. `discard_queued` is what GStreamer `FLUSH_STOP` and a seek need `[C3 P2-8]`. After stop
returns, `accepted == terminal outcomes` for the session (07 §6). Taking back queued units
needs the pipeline reclaim hook (04 §4.4); an in-progress plugin conversion bounds the
flush `[C1 #20]`.

Interrupting blocked calls is separate and sticky: `mtl_session_interrupt` (AS) makes
every WT call on the session return `-MTL_ECANCELED` until `mtl_session_uninterrupt`
([04 §5.4](04-threading-and-execution.md)).

### 3.5 RX start and arm semantics (r3)

`[C5 §8.4, §4.9]`; the RX timing model (timeline binding, media time from RTP, groups) is
[06 §11](06-timing-pacing-and-sync.md).

| Step | What happens |
|---|---|
| create | queue and scheduler quota reserved; flow-rule capacity validated; nothing joined |
| **first** `start` | flow rule installed and IGMP join sent (on a worker; the call does not wait for either, `FLOW_STATE` reports `JOINED` / `JOIN_FAILED`). The rule and the membership are **kept across stop** until destroy or `update_flows` (Q-LIFE-1 (a) as the default; a release-on-stop option stays Q-LIFE-1 (c)) |
| ARMED (`start(AT_TAI t)` or `AT_MEDIA_INDEX k`) | joined and receiving; every unit whose media time (06 §11) is < t is discarded and counted (`units_before_start`), never delivered. When the stream's media time is not valid (a SENDER stream, 06 §11) the arrival time is compared instead, and the first delivered unit says so in `time_valid` |
| RUNNING | from the first unit whose media time is ≥ t |
| stop | the tasklet stops assembling (immediate command); membership and rule stay, so a later start is instant; packets arriving while STOPPED are dropped by the queue |
| `update_flows` | new rule and membership prepared beside the old ones, all-or-nothing across legs; with `AT_TAI t` packets are accepted by the new rule for units whose media time ≥ t, then the old rule is removed (09 §7.1) `[C5 §8.1]` |
| unit completion | a unit completes when full, when evicted by a newer timestamp, or — **r3** — at its deadline: first-packet arrival + unit period + `rx_flush_offset`, checked by the tasklet every iteration, or on a command (stop DRAIN, discard). This is the RX hook row of 04 §4.4, in Phase 1 |

**Latest-only recipe** (monitoring, multiviewers): `pool.count = 2`,
`pool.rx_overflow = MTL_RX_RECLAIM_OLDEST_READY`, completion NONE. The one READY unit is
always the newest complete one; a consumer that holds a unit while it dequeues the next
(double-buffered display) uses `pool.count = 3`. Reclaim needs the tasklet-side CAS
READY → RECEIVING of 04 §4.4.

### 3.6 Errors and ERROR state (r3)

A session enters ERROR only for faults it cannot recover from without the application.
Every entry carries one `enum mtl_state_reason` (07 §5.4):

| Reason | Trigger |
|---|---|
| `TX_QUEUE_FATAL` | TX hang recovery failed to get a new queue or mempool (today `stat_unrecoverable_error` + `ST_EVENT_FATAL_ERROR`, `st_tx_video_session.c:4274-4280`, `:4306-4322`) |
| `DEVICE_GONE` | NIC removal, or a port reset after which queues and flows cannot be re-reserved |
| `CMD_TIMEOUT` | a command was not acknowledged by the tasklet within the instance's `cmd_ack_timeout_ns` (default 100 ms, 04 §3.2) |
| `BACKEND_FAILED` | a backend (kernel socket, AF_XDP) failure after retries |
| `FORCED` | `mtl_debug_inject(…, MTL_FAULT_FORCE_ERROR, …)` (debug builds, 13) |

On entry the library, in this order: marks the state (waking blocked callers with
`-MTL_EIO`); detaches the session from its scheduler with an immediate command (or, after
`CMD_TIMEOUT`, with today's lock-based detach, which succeeds as soon as the tasklet is
outside the session because tasklets only `try_get`, `st_tx_video_session.c:2682`);
resets or quarantines the TX queue so that no descriptor references application memory
(04 §4.5); drops every device reference; and emits the outcomes of the ERROR column in
§3.4. `SESSION_STATE {old → ERROR, reason}` goes to the EQs.

Leaving ERROR: `mtl_session_stop` → STOPPED (the reason stays readable as
`get_status().last_error_reason`). `mtl_session_start` from there re-reserves what the
fault invalidated — queues, flow rules, IGMP membership, rate-limiter shaper — or fails
with `-MTL_ENODEV` and a reason (`DEVICE_GONE`, `PORT_RESET`) and nothing changed. The app
can also `mtl_session_reconfigure` (for example to move `flow.port`, which only
reconfigure may change, 09 §7.3) before starting again.

Recoverable incidents do *not* enter ERROR; they are events plus per-unit results
`[R13 §2, R06 §3]`:

| Incident | Outcome |
|---|---|
| TX hang fixed by recovery | `SESSION_RECOVERY` begin/end; units in the gap `DROPPED/RECOVERY` |
| link down on one leg of 2022-7, or single-leg link loss (Q-LIFE-7 (a)) | stays RUNNING; `LEG_STATE`/`PORT_LINK`; units `DROPPED/LINK_DOWN` on that leg; resumes on link up |
| VF reset where the backend can restore queues and flows (Q-LIFE-10 (a)) | `PORT_RESET` event; units in the gap `DROPPED/RECOVERY` |
| PTP holdover or loss | `TIME_STATE`; timing flags in results |
| MtlManager lost | running sessions continue; new creates follow 07 §7 |
| unresolved neighbour, failed IGMP join | `FLOW_STATE` (§3.3) |

## 4. Buffer leases

Allocation ownership never changes; access ownership moves through one state machine for
every provisioning path (review §9). The states are per *pool slot*; the lease generation
increments on every transition out of FREE, written by the acquirer (an app thread), so
the tasklet never writes the generation (04 §4.1, split slot).

### 4.1 TX

```text
            acquire() / acquire_buffer(b)          submit()                engine picks up
   FREE ─────────────────────────────▶ APP_WRITABLE ──────────▶ QUEUED ─────────────────▶ IN_FLIGHT
    ▲                                    │   │ publish() (progressive:     │ withdraw()    (converting,
    │                                    │   │  rows > ready stay the      ▼               packetising,
    │         release()                  │   │  app's to write, 06 §9)   FLUSHED/WITHDRAWN  held by NIC)
    ├────────────────────────────────────┘   │                                               │ last reference
    │                                        │ mtl_rx_transfer(): sugar for acquire_buffer   ▼ gone
    │                                        │ + .hold (§4.3)                                DONE
    │   NONE: acquire takes a DONE slot directly; ALL/EXCEPTIONS: the reader copies the     │
    │   result into the unread ring, then frees the slot (out of order, §4.1 "Order")       │
    └────────────────────────────────────────────────────────────────────────────────────┘
```

| Transition | Who | Guarantees |
|---|---|---|
| FREE → APP_WRITABLE | `mtl_tx_acquire` (any free slot) or `mtl_tx_acquire_buffer(s, b)` (that named attached buffer; `-MTL_EBUSY` if not FREE); MP-safe by default (a CAS on the slot, A10) | unique lease; slot hint returned |
| APP_WRITABLE → FREE | **r3** `mtl_tx_release` (was `mtl_tx_abort`) | no result will be produced |
| APP_WRITABLE → QUEUED | `mtl_tx_submit` success; one submitting context unless `MTL_SESSION_MT_SUBMIT` | access transfers to MTL; exactly one terminal outcome will follow; the lease goes stale (`MTL_UNIT_ROWS`: current for `mtl_tx_publish` until the FINAL publish); **r3** the pick-up sequence number is assigned here, at submit, not at acquire (04 §4.4) |
| APP_WRITABLE stays | `mtl_tx_submit` failure | access stays with the app; no result |
| QUEUED → FLUSHED | **r3** `mtl_tx_withdraw(s, buffer)` (was `mtl_tx_cancel`; it names the base buffer because the lease is stale after submit) | a queued unit (playlist edit) becomes `FLUSHED/WITHDRAWN`; `-MTL_EBUSY` if already picked up |
| QUEUED → IN_FLIGHT | engine | admission decision (on time / late policy) taken here, reported in the result |
| IN_FLIGHT → DONE | the completing context (04 §4.1.1 lists them all) | no converter, encoder, mbuf, DMA descriptor or NIC can access the storage any more; with `.hold`, the held RX lease's count is decremented here (§4.3) |
| DONE → FREE | NONE: the next `acquire` takes the slot; ALL/EXCEPTIONS: the reader, after copying the result into the unread ring | the result (if the mode keeps it) is in the unread ring *before* the slot can be acquired again (review §9 rule 6) |

`mtl_tx_acquire_buffer` exists because a framework sink receives one specific GstBuffer
or AVFrame that maps to one specific imported buffer, and must send *that* buffer; picking
"any free slot" would force a copy `[C3 P2-2]`.

**Order (r3).** The engine picks up submissions in submission order (FIFO), never by
slot index — PR #1610 could transmit `A B D C` because both sides scanned for the lowest
slot `[R01 R6]`; G-08 holds. Result *publication* is decoupled from slot reuse
`[C5 §4.11 "Head-of-line"]`: a unit that completes early frees its slot at once instead
of waiting for its predecessors. Units complete out of order only in exceptional cases
(`mtl_tx_withdraw`; a late drop of a converted unit while an older one is in flight,
`tx_st20p_if_frame_late`, `st20_pipeline_tx.c:137-178`; recovery, which completes by
index, `st_tx_video_session.c:4293-4301`). Chain mode with two legs is *not* one of them:
`sh_info` counts the mbufs of both queues (`st_tx_video_session.c:235-237`, `:1295-1298`),
so the frame completes once, in order. Completed results are copied
into the unread-results ring as they complete, and the ring publishes them **in submission
order**: a result waits there, its slot already FREE, until every earlier submission has
its result. The ring's capacity is the pool size, so the wait never blocks a producer or
the engine, and G-09 holds. `hdr.seq` is in every result.

### 4.2 RX

```text
          engine assigns slot            unit complete, evicted, or deadline      dequeue()
   FREE ─────────────────────▶ RECEIVING ─────────────────────────────────▶ READY ─────────▶ APP_READING
    ▲                              │ stop(FLUSH) / discard / reclaim              │ (may be held across
    │                              ▼                                               │  threads; release in
    ├──────────────────────────── (discarded, counted) ◀── RECLAIM_OLDEST_READY ──┤  any order)
    │                                                                              │ release()
    └───────────── hold count == 0 ◀──── HELD (released, still read by TX) ◀───────┘  (§4.3)
```

- **Slot selection** (`pool.rx_slot_select`): `ANY_FREE` (default) or `BY_INDEX` — slot =
  `media_index mod count`, so unit *k* lands in buffer *k mod N*. The MXL bridge needs
  this: an MXL flow is a ring where grain `k mod N` holds media index *k*, and today the
  POC picks the grain in a tasklet callback (`query_ext_frame`) `[C3 P5-1]`. `BY_INDEX`
  is an index lookup, not today's scan in `rv_get_frame` (04 §4.4). If the target slot is
  not FREE, the overflow policy applies to that slot only (sizing rule in 05 §5).
- **Pool exhaustion** (`pool.rx_overflow`): `MTL_RX_DROP_NEW` (default: the new unit is
  not received; the next delivered result carries `units_missed_before`) or
  `MTL_RX_RECLAIM_OLDEST_READY` (latest-wins: the oldest READY-but-not-dequeued unit is
  discarded and counted; with `BY_INDEX`, only the target slot, and only if READY). Slots
  the app holds, and HELD slots, are never reclaimed. The reclaim is a tasklet-side CAS
  READY → RECEIVING, so a concurrent `dequeue` (CAS READY → APP_READING) either wins or
  finds the slot gone. `[R08 §3.3 bobi "serve_newest", R08 Q4]`
- **Release** is MP-safe (a CAS on the slot, or a decrement of the hold count), from any
  thread, in any order. The MXL bridge had to release MTL slots immediately after dequeue
  because holding them starved the RX pool `[R08 §1.2]`; leases held across threads plus
  an explicit overflow policy make that unnecessary. Today's release is already an atomic
  decrement, not a mempool operation (`rv_put_frame`, `st_rx_video_session.c:222-232`;
  `st20_rx_put_framebuff`, `:4692-4715`), and stays one.

### 4.3 Buffers shared between sessions (r3)

Zero-copy forwarding (RX session A → TX session B, including the split-forward sample
that sends each quadrant of a 4K RX frame as a 1080p stream) needs RX bytes readable by
several TX sessions in the same frame period `[C3 P5-3, C4 §4.1 #7, C5 §6.1, §6.2]`.
A buffer belongs to one session's pool. Sessions share bytes by creating their own
buffers over one region, and a TX submission may hold an RX slot
(`mtl_tx_submission.hold`, 05 §5.4) until its terminal outcome. `mtl_rx_transfer` is
sugar for the 1 → 1 case. A sub-rectangle TX (stride > row) is DIRECT (05 §4.2).

**Stride-aware DIRECT.** The ST20 builder already knows strides: `st20_tx_ops.linesize`
is a session parameter (`include/st20_api.h:1215-1218`); the chain builder computes each
packet's offset as `line1_number * st20_linesize + …` (`st_tx_video_session.c:1120`,
`:1225`, `:1272`), attaches the external buffer at that offset (`:1295-1298`), and copies
only the rare packet that crosses the row padding (`:1274-1288`). The split-forward sample
relies on it today: `ops_tx.width = ctx.width / 2` with the full-width linesize
(`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:263-265`) and one buffer offset per quadrant
(`:284-287`). So:

- for ST20 TX and RX, a plane with `stride ≥ row_bytes` is a **DIRECT-capable** layout:
  `plane[0].stride` maps to `ops.linesize`, and a packet that straddles row padding is
  copied and counted in `pkts_copied_partial` (05 §6);
- the same rule makes an interleaved interlaced frame two field buffers over one region
  (stride = 2 × row_bytes, plane offset = one row for the second field) (05 §4.3, 09).

**RX hold count.** A TX submission may hold an RX lease:

```c
struct mtl_tx_submission {
  /* … 06 §4.1 fields … */
  mtl_lease_h hold;          /* optional: an RX lease this unit reads; null = none */
};
```

- Each accepted submission with `.hold` increments the RX lease's hold count; the TX
  completion (IN_FLIGHT → DONE, or any terminal outcome) decrements it.
- The RX slot returns to FREE only when **the app has released it and its hold count is
  0**; in between it is HELD (released by the app, still read by TX). The RX session's
  destroy defers while any slot is HELD, as for app leases (§6.3).
- One RX frame → four TX sessions: each TX session has its own attached buffer — its own
  layout (offset + half-width rows, full stride) over the same region — and submits with
  `.hold = rx_lease`. The four quadrants go out in the same frame period; nothing is
  serialised on the lease.
- MTL does not track byte overlap between different buffers. `.hold` is how the app
  tells MTL which RX unit a TX submission reads; submitting a TX buffer that overlaps an
  RX slot the app has not held is an app error (torn frames), not a memory-safety issue,
  because regions stay mapped while any buffer references them.
- `mtl_rx_transfer(rx, rx_lease, tx, &tx_lease)` leases the TX session's attached buffer
  whose planes equal the RX buffer's (`-MTL_EINVAL`, reason `HOLD_MISMATCH`, if none),
  records an implicit hold on `rx_lease`, and releases `rx_lease` when the TX lease is
  submitted (05 §5.4).
- Library RX pools are regions too: `mtl_session_get_pool_region(rx)` returns an
  `mtl_region_h`, so TX buffers can be created over a library RX pool (the
  `rx_st20p_tx_st20p_fwd` sample pattern) (05 §5.3).
- Access flags are checked at attach: RX needs `MTL_MEM_WRITE`, TX needs `MTL_MEM_READ`
  (05) `[C5 §6.10]`.

### 4.4 Exported library pools (r3)

A framework that exports the session's library pool as its own buffer pool (GStreamer
`propose_allocation`, FFmpeg `get_buffer2`) must follow this lease-level rule; revision
2's `release_buffer → mtl_tx_abort` returned `-ESTALE` on every submitted buffer and
leaked the pool `[C5 §7.2, §7.3]`. The GStreamer recipe is in 11 §6.2.

| Framework event | MTL call | Rule |
|---|---|---|
| pool `acquire_buffer` (upstream thread) | `mtl_tx_acquire` | MP-safe by default (A10) |
| sink `render` (streaming thread) | `mtl_tx_submit`, then mark the wrapper *in flight* | submit needs one context, or `MTL_SESSION_MT_SUBMIT` |
| pool `release_buffer` | `mtl_tx_release` **only if the wrapper was never submitted**; an in-flight wrapper waits for its result | a submitted lease is MTL's until its result |
| `TX_RESULT` read (sink thread, `mtl_tx_reap`) | return the wrapper to the framework pool | the only way a submitted buffer comes back |
| `set_active(FALSE)` | `mtl_session_stop(s, MTL_STOP_FLUSH, …)` first, then reap until every in-flight wrapper has its result | otherwise `set_active` waits forever for buffers MTL still holds |

Exported pools are declared with `MTL_SESSION_EXPORT_POOL`, which forces
`MTL_COMPLETE_ALL` (as for attached pools, because the result is the recycling signal) and
implies `MTL_SESSION_MT_SUBMIT`. Combining it with completion NONE is `-MTL_EINVAL`.

## 5. Groups

A group arms and starts several sessions together, on one timeline, so no required stream
starts alone (review §14 #20). The start formula, the common grid and RX groups are 06
§3.1, §3.3 and §11; this section is the lifecycle.

```text
 mtl_group_create(mt, timeline, &cfg) → MTL_GROUP_CREATED
                       (r3: cfg.direction TX or RX, required; mixing directions is -MTL_EINVAL;
                        an RX group has one cfg.link_offset_ns for every member and needs an
                        EPOCH or AT_TAI timeline, RX_TIMELINE_LAZY otherwise; cfg NULL = TX, no offset)
 mtl_group_add(g, s)   (s must be CREATED/STOPPED and use the group's timeline; a TX member needs
                        media_mode INDEX or TAI — AUTO cannot be synchronised; else -MTL_EINVAL.
                        r3: legal on a RUNNING group; s then joins at the next feasible index
                        with the timeline's offset, START_NOW semantics)
 mtl_group_remove(g, s) (a STOPPED member)
 mtl_group_start(g, {AT_MEDIA_INDEX k | AT_TAI t | NOW} + preroll_ns, &chosen)
     1. validate every member (as session start would; ANC rate/raster = its video's) ── any failure → nothing changes
     2. resolve the timeline anchor if lazy with the 06 §3.1 formula (T0 on the common grid by
        construction; k in the units of the first video member, else of the first member in add order)
     3. ARM every member with that instant (TX: preroll accepted; RX: flow rules and joins of every
        member installed, filters armed together to discard media time < t, 06 §11.4) → MTL_GROUP_ARMED
     4. members go RUNNING at the instant; the instant is returned in `chosen`             → MTL_GROUP_RUNNING
 mtl_group_stop(g, MTL_STOP_DRAIN | MTL_STOP_FLUSH, timeout) → stops all members; returns when all have finished → MTL_GROUP_STOPPED
 mtl_group_get_state(g)  (r3: returns the state, like mtl_session_get_state)
```

```c
enum mtl_group_state {
  MTL_GROUP_UNSET = 0,
  MTL_GROUP_CREATED = 1,      /* members may be added */
  MTL_GROUP_ARMED,            /* every member ARMED */
  MTL_GROUP_RUNNING,          /* every member RUNNING */
  MTL_GROUP_STOPPED,          /* every member STOPPED; can start again */
  MTL_GROUP_PARTIAL_FAILURE,  /* at least one member in ERROR; the others keep running (Q-TIME-8) */
};
```

A member that later fails (ERROR) emits its own `SESSION_STATE`; the group emits
`GROUP_MEMBER_FAILED` and does *not* stop the others. **r3 restart sequence**
`[C5 §5.10]` (06 §10.6): `mtl_session_stop(member)` → fix (reconfigure, `update_flows`) →
`mtl_session_start(member, NULL)`, which for a group member is legal only in
`MTL_GROUP_RUNNING`/`PARTIAL_FAILURE` and rejoins at the next feasible index with the
offset the timeline kept from its first start (06). The group returns to RUNNING when every
member is RUNNING again; it is never stopped for one member.

Framework elements that each own one session share a *named* timeline instead and start
independently (06 §10.3).

## 6. Destroy

### 6.1 Guarantees

After the `SESSION_RETIRED` event, which is posted in both cases — when
`mtl_session_destroy(s, flags)` returns 0 with nothing outstanding (immediate case), and
when the last lease or hold comes back (deferred case):

1. no library thread will call application code on behalf of `s`;
2. no library thread or device will read or write memory of any region imported by the
   application for `s` — including after a stalled or hung TX queue (04 §4.5);
3. every accepted TX submission has a terminal outcome (in a shared CQ if one is bound;
   entries left unread in the private CQ are counted and discarded at retire);
4. every handle derived from `s` returns `-MTL_EBADF`, except the leases still out during
   a deferred destroy (which accept `mtl_tx_release` / `mtl_rx_release`) and the session
   handle itself, for which `mtl_session_get_state()` returns `MTL_STATE_RETIRED` until
   the slot is reused (§2.2);
5. threads that were blocked in calls on `s` have returned `-MTL_ESHUTDOWN`;
6. **r3** the session no longer pins the instance (§7.1).

`[R13 R-L4, R00 §14 #23–26]`

### 6.2 Sequence (r3)

```text
destroy(s):
  CAS state → DESTROYING       (a concurrent destroy gets -MTL_EBUSY; data calls get -MTL_ESHUTDOWN,
                                release and private-EQ reads keep working)
  if ARMED/RUNNING/DRAINING: stop(FLUSH)            ── immediate command (04 §3.2)
  wake waiters (-MTL_ESHUTDOWN); interrupt on s becomes a no-op
  detach from the scheduler (command + ack)         ── no tasklet touches s after the ack
  wait for DP callers to leave (in-flight counter, §2.3)
  wait for the last NIC/DMA reference on every buffer:
      idle descriptor cleanup (rte_eth_tx_done_cleanup), bounded
      on timeout: a worker resets the queue (stop/start, dedicated queue) or quarantines it (shared
      queue: marked, reset when its last user is gone); mbuf free callbacks then run on that
      worker; if the reset fails, escalate to PORT_RESET handling (04 §4.5)
  if app leases or HELD slots are outstanding: return 0 now (deferred); continue on the last return
  retire: release region, timeline, group, CQ references; drop the session's reference on its EQs
  bump the handle generation, tombstone the slot (§2.2); post SESSION_RETIRED (both cases)
  drop the session's instance reference (§7.1)
```

The busy flush of today's destroy path — `tv_uinit_hw` pushes pad packets through
`mt_txq_flush` (`st_tx_video_session.c:2722-2728`, `dev/mt_dev.c:1782-1799`), and those
bursts run `tv_frame_free_cb` on the destroying app thread — is removed in favour of the
bounded cleanup and queue reset above `[C5 §4.3, §4.11 "Completing contexts"]`.

The last `mtl_rx_release` / `mtl_tx_release` of a deferred destroy is a DP call and cannot
free memory; it posts the retire to a library worker, which runs the tail of the sequence.

### 6.3 Leases the application still holds

Frameworks cannot finish `GstBaseSrc::stop` or FFmpeg `read_close` while downstream still
holds RX buffers, and must not have library-pool memory freed under them `[C3 P2-4]`:

| Option | Behaviour |
|---|---|
| (a) refuse | `destroy` returns `-MTL_EBUSY` until every lease is returned |
| (b) invalidate: `MTL_DESTROY_FORCE` | destroy returns once every *device* reference is gone (the queue path of §6.2 still runs, so guarantee 2 holds); outstanding leases go stale (`-MTL_EBADF`), library-pool memory is freed under the app. `MTL_DESTROY_FORCE` on a session already DESTROYING (deferred) escalates it this way instead of `-MTL_EBUSY` |
| **(c) defer (default)** | destroy stops the session and retires the handle for every call except `mtl_rx_release` / `mtl_tx_release` on the outstanding leases and the private-EQ reads; the pool is freed when the last lease returns and the last hold drops; `SESSION_RETIRED` reports the final free — how GstBufferPool and AVBufferPool behave |

(Q-LIFE-4.) `SESSION_RETIRED` is delivered on the session's private EQ **and** to every EQ
subscribed with `MTL_EQ_SUB_SESSION`, including the instance EQ (07 §3.4), and
`mtl_instance_get_status().sessions_destroying` counts deferred destroys `[C5 §7.1]`.

### 6.4 Imported memory at teardown (r3)

`[C5 §6.12]`: during a deferred destroy the buffers stay attached until `SESSION_RETIRED`,
so `mtl_buffer_destroy` returns `-MTL_EBUSY` and `mtl_mem_destroy` after it too. The rule
for applications:

- wait for `SESSION_RETIRED` (or `mtl_session_get_state() == MTL_STATE_RETIRED`), or use
  `MTL_DESTROY_FORCE`, before `mtl_buffer_destroy` × N and `mtl_mem_destroy`;
- **imported memory must not be unmapped or freed before `mtl_mem_destroy` returns 0**;
  a framework that frees its arena earlier leaves a live IOMMU mapping to freed pages.

`mtl_mem_destroy` stays `-MTL_EBUSY` while any device reference exists; after a hung
queue it becomes 0 once the queue reset of 04 §4.5 has run. Only process exit bypasses
it. The recipe is in 05 §3.7 and 11 §6.2.

## 7. Instance (r3)

`[C5 §7.1, §7.8, §8.5, §8.6]`, A11. Versioned `struct mtl_instance_params` (`struct_size`,
`api_version`, port specs, queue counts, lcores, time source, flags, `cmd_ack_timeout_ns`)
lands **before** the new API, as Phase 0 work (Q-ABI-2 answered yes).

| Call | Class | Purpose |
|---|---|---|
| `mtl_instance_acquire_default(&params, &mt)` | CP | the refcounted process-wide instance; the first call opens it, later calls are merged per §7.2 (Q-LIFE-2) |
| `mtl_instance_open(&params, &mt)` | CP | a private instance, never merged; released with `mtl_instance_release` |
| `mtl_instance_release(mt)` | CP | drops one reference; never fails for live objects (§7.1) |
| `mtl_instance_from_legacy(mtl_handle)` / `mtl_instance_to_legacy` | CP | bridge to an instance made by today's `mtl_init` |
| `mtl_instance_get_info(mt, &info)` | CP | granted `api_version` and `time_source`, `lib_version`, port and scheduler counts, effective `flags`, and `ignored_fields`: which fields of *this caller's* params were ignored (§7.2) |
| `mtl_instance_get_status(mt, &st)` | DP | `manager` (cached, 07 §7), `refcount`, `sessions`, `sessions_destroying`, `regions`; the time state is `mtl_time_get_status` (06 §2.4) |
| `mtl_instance_get_mem_status(mt, numa, &st)` | CP | hugepage and region use on one NUMA node (05 §6.3) |
| `mtl_instance_get_eq(mt, &eq)` | DP | the instance EQ (subscribed to `PORT \| TIME \| INSTANCE \| SESSION`, 07 §3.4) |
| `mtl_instance_list_sessions(mt, handles[], cap, &n)` | CP | enumerate live sessions (08) |
| `mtl_instance_interrupt_all(mt)` / `mtl_instance_uninterrupt_all(mt)` | AS / CP | instance-wide sticky interrupt (04 §5.4) `[C5 §8.12]` |
| `mtl_time_now(mt, &t)` | DP | TAI now with source, lock state and accuracy (06 §2) |
| `mtl_port_count`, `mtl_port_find(mt, name_bdf_or_ip, &port)` | CP | ports by name, BDF or local IP (Rivermax users select by IP `[R10 §1.3]`) |
| `mtl_port_get_caps` / `get_status` / `get_capacity` | CP / DP / CP | capabilities, live status, free capacity ([09](09-media-modes-and-backends.md), [08](08-observability.md)) |

L4 adds `mtl_instance_open_simple("0000:af:01.0=192.168.1.10,…", &mt)` and the
`mtl_simple_*` layer (10) `[C3 P1-2]`.

### 7.1 Lifetime: the instance outlives destroying sessions

> **Addendum K (2026-10-01).** The instance's last close now shuts it down within a deadline
> and closes the sessions, queues and timelines still open ([16 §2](16-kubernetes-and-crash-safety.md)).
> EAL stays initialised, so a later open in the same process works on the same ports and a
> subset of the first open's CPUs; a port quarantined by an earlier close is `-MTL_EBUSY`
> until exit.

Revision 2 blocked instance destroy while "any unified object exists", so a
`GstBaseSrc::stop` or FFmpeg `read_close` that destroyed its session with leases still held
downstream and then released its instance reference got `-EBUSY` and leaked the instance
for the process lifetime (`gst_mtl_common.c`, `ecosystem/ffmpeg_plugin/mtl_common.c:220-239`)
`[C5 §7.1]`. Now:

- the instance has one reference per `acquire_default` caller plus one per session that
  exists (including DESTROYING ones);
- `mtl_instance_release` only drops the caller's reference and returns 0;
- teardown runs on a library worker when the count reaches 0, in dependency order
  (groups, timelines, CQs, EQs, buffers, regions, ports, EAL is kept, Q-LIFE-2 (a)); an
  app-created object still alive then (a region the app forgot) is destroyed with it and
  counted in the log, which is the `MTL_UNINIT_DESTROY_ALL` behaviour of Q-LIFE-3 (c);
- **process exit without release is supported**: the library registers no `atexit`
  teardown that could deadlock, and the kernel reclaims hugepages, VFIO mappings and the
  MtlManager connection (the manager treats a closed socket as a client exit,
  `manager/mtl_instance.hpp:66-68`).

Re-initialisation in one process — today `mtl_uninit` calls `rte_eal_cleanup()` and a
second EAL init is rejected (`dev/mt_dev.c:324`, `:499-502`, #1341) — is solved by keeping
EAL alive across the last release (Q-LIFE-2 (a)); the next `acquire_default` reopens ports.

### 7.2 The default-instance merge table

Today FFmpeg shares an instance only when a `memcmp` of the whole `mtl_init_params` (minus
PTP bits) matches (`ecosystem/ffmpeg_plugin/mtl_common.c:166-181`), and GStreamer ignores
every later element's parameters silently
(`ecosystem/gstreamer_plugin/gst_mtl_common.c:657-686`); neither is a contract. The
default instance publishes one. Each field of the second and later caller's params is:

- **invariant** — it must match the open instance, else `-MTL_EINVAL` with reason
  `INSTANCE_PARAM_MISMATCH` and the field named in `mtl_last_error`;
- **mergeable** — combined as stated;
- **ignored on second acquire** — the open instance's value stays; the field is listed in
  `mtl_instance_get_info(…).ignored_fields` for that caller, and one `warn` is logged.

Two general rules make the table usable for plugins: a **zero field always matches**
(zero means "don't care" for every class), and for flag bits only the bits the caller
*sets* are requests (a clear bit never conflicts).

**The `mtl_instance_params` fields**, in header order. `Bit` is the field's bit in
`mtl_instance_info.ignored_fields` (per-port fields have one bit for all ports; each
`MTL_INSTANCE_*` flag has its own). "Zero means" is the value a zero field takes at first
open, the row 09 §9.1 points to.

| Bit | Field | Zero means | Class | Rule |
|---|---|---|---|---|
| 0 | `api_version` | the header's `MTL_UNIFIED_API_VERSION` | mergeable | each caller's structs are read at that caller's version; `info.api_version` is the granted one |
| 1 | `port_count` | first open: required (`FIELD_REQUIRED`), 1…`MTL_INSTANCE_MAX_PORTS`; later: use the open ports | mergeable | as `ports[].name` |
| 2 | `time_source` | `AUTO`: built-in PTP if enabled, else PHC, else `SYSTEM_TAI` (06 §2.2) | invariant if non-zero | one time source per instance |
| 3 | `ports[].name` | first open: required | mergeable | in v1 every port the caller names must already be open, with the same backend; a port that is not open → `-MTL_ENODEV`, reason `PORT_NOT_OPEN` (A11). The caller finds its ports with `mtl_port_find`; indices are the instance's |
| 4 | `ports[].ip_family` | IPv4 | invariant per open port | — |
| 5 | `ports[].prefix_len` | 24 | invariant per open port | — |
| 6 | `ports[].sip` | DHCP on kernel backends | invariant per open port | a different address for an open port is a mismatch |
| 7 | `ports[].gateway` | none | invariant per open port | — |
| 8 | `ports[].tx_queues` | automatic | mergeable at first open only | queues are configured once (`rte_eth_dev_configure`, `dev/mt_dev.c:993`), so the first open takes the hardware maximum, capped by `max_queues`; later counts are admission-checked and reported, never an error; sessions still fail at create with `-MTL_ENOSPC` when queues run out. OBS depends on this (09 §8.2) |
| 9 | `ports[].rx_queues` | automatic | mergeable at first open only | as `tx_queues` |
| 10 | `ports[].numa` | the device's socket | invariant per open port | device probe argument |
| 11 | `ports[].port_flags` | — (reserved, must be 0) | — | non-zero is `-MTL_EINVAL` (`UNKNOWN_BITS`) |
| 12 | `lcores` | MtlManager, or automatic without it | ignored on second acquire | EAL's lcore set is fixed at init; a later caller's list is reported and its sessions use the instance's schedulers. OBS passes per-source `lcores` (09 §8.2) and must not fail a scene switch over it |
| 13 | flag `MTL_INSTANCE_MANAGER_OPTIONAL` | clear | invariant (set bit) | removed by addendum K (D-92): no fallback flag; without a manager the affinity mask or OFD locks arbitrate |
| 14 | flag `MTL_INSTANCE_TASKLET_THREAD` | clear | invariant (set bit) | thread model (04 §7.1) |
| 15 | flag `MTL_INSTANCE_TASKLET_SLEEP` | clear | invariant (set bit) | thread model |
| 16 | flag `MTL_INSTANCE_PTP_BUILTIN` | clear | invariant (set bit) | time source |
| 17 | flag `MTL_INSTANCE_HW_TIMESTAMP` | clear | invariant (set bit) | time source |
| 18 | flag `MTL_INSTANCE_BIND_NUMA` | clear | invariant (set bit) | placement |
| 19 | flag `MTL_INSTANCE_RX_SEPARATE_VIDEO_LCORE` | clear | ignored on second acquire | legacy scheduler policy |
| 20 | flag `MTL_INSTANCE_TX_VIDEO_MIGRATE` | clear | ignored on second acquire | legacy scheduler policy; ported as the opt-in options `instance.*_video_migrate` and `session.migrate` (Q-THR-6, revised to (b)) |
| 21 | flag `MTL_INSTANCE_RX_VIDEO_MIGRATE` | clear | ignored on second acquire | as above |
| 22 | flag `MTL_INSTANCE_TASKLET_TIME_MEASURE` | clear | ignored on second acquire | diagnostics are the first caller's |
| 23 | `sched_max` | automatic, at most 18 (`MT_MAX_SCH_NUM`) | ignored on second acquire | scheduler sizing is the first caller's |
| 24 | `sched_quota_mbs` | 12 × the 1080p59.94 4:2:2 10-bit bandwidth, as today (`dev/mt_dev.c:1940-1943`, `st_header.h:23`) | ignored on second acquire | the capacity query reports what is left |
| 25 | `ptp_domain` | the value 0 | invariant if non-zero | built-in PTP only |
| 26 | `log_level` | the library default (INFO) | ignored on second acquire | process-global |
| 27 | `max_queues` | the hardware maximum | ignored on second acquire | applies when the ports are opened |
| 28 | `cmd_ack_timeout_ns` | 100 ms (04 §3.2) | ignored on second acquire | — |

A port whose link is down at open is opened anyway, with operational state DOWN, because
unified sessions reserve every leg whatever its link state (09 §7.2).

**Legacy `mtl_init_params` fields.** A plugin ports its current parameter set field by
field, so the fields of `struct mtl_init_params` at `545a266a`
(`include/mtl_api.h:564-761`; flags `:338-502`; `mtl_port_init_params` `:542-558`) have
classes too. The unified field a legacy one maps to follows the same class; a legacy field
without one is per-session in the unified API or has no unified equivalent, and its row
says so:

| Field | Class | Rule |
|---|---|---|
| `port[]`, `num_ports`, `pmd[]` | mergeable | in v1 every port the caller names must already be open, with the same PMD; a port that is not open → `-MTL_ENODEV`, reason `PORT_NOT_OPEN` (A11). The caller finds its ports with `mtl_port_find`; indices are the instance's |
| `net_proto[]`, `sip_addr[]`, `netmask[]`, `gateway[]` | invariant per open port | a different address or protocol for an open port is a mismatch |
| `tx_queues_cnt[]`, `rx_queues_cnt[]` | mergeable at first open only | queues are configured once (`rte_eth_dev_configure`, `dev/mt_dev.c:993`), so the default instance opens each port with the hardware maximum (capped by `max_queues` if set); a later caller's counts are admission-checked and reported, never an error. Sessions still fail at create with `-MTL_ENOSPC` when queues run out |
| `port_params[]` (`flags` `FORCE_NUMA` / `ALLOW_DOWN_INITIALIZATION`, `socket_id`, `rl_burst_size`) | invariant per open port | device probe arguments |
| `lcores` | ignored on second acquire | EAL's lcore set is fixed at init; a later caller's list is reported and its sessions use the instance's schedulers. OBS passes per-source `lcores` and one queue (`ecosystem/obs_mtl/linux-mtl/mtl-input.c:330-332`, `mtl-output.c:139-141`) and must not fail a scene switch over it `[C5 §7.8]` |
| `main_lcore`, `iova_mode`, `memzone_max` | invariant if non-zero | EAL parameters |
| `dma_dev_port[]`, `num_dma_dev_port` | ignored on second acquire | DMA devices are probed at init; sessions that request DMA get `DIRECT_CPU` granted when none is available (05) |
| `rss_mode`, `rss_sch_nb[]`, `nb_rx_hdr_split_queues`, `rx_pool_data_size`, `nb_tx_desc`, `nb_rx_desc` | invariant if non-zero | queue and classifier setup |
| `pacing` | invariant if non-zero | the per-session `pacing_class` request is reconciled with it at create/query: an unsatisfiable request is `-MTL_ENOTSUP`, a satisfiable one is reported as granted (06 §6) |
| `data_quota_mbs_per_sch`, `tx_audio_sessions_max_per_sch`, `rx_audio_sessions_max_per_sch`, `tasklets_nb_per_sch` | ignored on second acquire | scheduler sizing is the first caller's; the capacity query reports what is left |
| `pkt_udp_suggest_max_size` | ignored on second acquire | per-session in the unified API (09) |
| `ptp_get_time_fn`, `priv`, `kp`, `ki`, and the time-source part of `flags` | invariant | the instance has one time source; legacy `ptp_get_time_fn` maps to time source USER, compared by pointer (FFmpeg's `CLOCK_TAI` function against another caller's is a mismatch) |
| `ptp_sync_notify`, `stat_dump_cb_fn`, `dump_period_s` | ignored on second acquire | unified observers use getters and EQs |
| `log_level` | ignored on second acquire | process-global; `mtl_set_log_level` still works |
| `arp_timeout_s` | ignored for unified sessions | ARP never blocks a unified call (§3.3); legacy sessions keep the instance value |
| `port_packet_loss[]`, `tx_sessions_cnt_max`, `rx_sessions_cnt_max` | ignored on second acquire | debug (moves to `mtl_debug_inject`) and deprecated fields |
| flags `PTP_ENABLE`, `PTP_PI`, `PTP_UNICAST_ADDR`, `PHC2SYS_ENABLE`, `PTP_SOURCE_TSC`, `ENABLE_HW_TIMESTAMP` | invariant (set bits) | time source |
| flags `TASKLET_THREAD`, `TASKLET_SLEEP`, `DEDICATED_SYS_LCORE`, `UDP_LCORE`, `CNI_THREAD`, `CNI_TASKLET`, `BIND_NUMA`, `NOT_BIND_NUMA`, `NOT_BIND_PROCESS_NUMA`, `ALLOW_ACROSS_NUMA_CORE` | invariant (set bits) | thread model and placement |
| flags `SHARED_TX_QUEUE`, `SHARED_RX_QUEUE`, `RX_USE_CNI`, `RX_UDP_PORT_ONLY`, `DISABLE_SYSTEM_RX_QUEUES`, `NIC_RX_PROMISCUOUS`, `NO_MULTICAST`, `VIRTIO_USER`, `AF_XDP_ZC_DISABLE`, `RX_MONO_POOL`, `TX_MONO_POOL`, `TX_NO_CHAIN`, `TX_NO_BURST_CHK`, `RANDOM_SRC_PORT`, `MULTI_SRC_PORT` | invariant (set bits) | port data-path setup (`SHARED_TX_QUEUE` forces software pacing for the whole port) |
| flags `RX_SEPARATE_VIDEO_LCORE`, `TX_VIDEO_MIGRATE`, `RX_VIDEO_MIGRATE` | ignored on second acquire | legacy scheduler policies; unified sessions never migrate (Q-THR-6 (a)). FFmpeg sets all three by default (`mtl_common.c`), so making them invariant would break FFmpeg + GStreamer in one process |
| flag `ALLOW_DOWN_PORTS` | ignored for unified sessions | the unified API never prunes legs (09 §7.2) `[C5 §8.2]`; legacy sessions keep it |
| flags `DEV_AUTO_START_STOP`, `RXTX_SIMD_512`, `TASKLET_TIME_MEASURE`, `REDUNDANT_SIMULATE_PACKET_LOSS` | ignored on second acquire | lifecycle of the default instance is refcounted; diagnostics and SIMD preference are the first caller's |

Runtime `mtl_port_open` leaves v1 and moves to Phase 6, where it takes a
`struct mtl_port_params` (name, PMD, IP, netmask, gateway, queue counts, port flags) and
brings up what `mtl_init` brings up per port today: the EAL device (probe or
`rte_eal_hotplug_add`, `dev/mt_dev.c:1558`), `rte_eth_dev_configure` and queue setup,
mempools, the flow manager, the CNI/PTP tasklets on `main_sch`, and the ARP and multicast
tables `[C5 §8.5]`.
