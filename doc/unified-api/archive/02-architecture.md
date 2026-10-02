# 02 — Architecture

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Reads with | [01-goals-and-requirements.md](01-goals-and-requirements.md), [04-threading-and-execution.md](04-threading-and-execution.md), [14-implementation-roadmap.md](14-implementation-roadmap.md) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

This document shows where the new API sits, which objects it introduces, how data and
control flow between application threads and tasklets, how the first implementation
reuses today's engines, and (new in revision 3) what the incremental alternative would
reach and cost. Items changed in revision 3 are marked **r3**.

## 1. The picture in one diagram

```text
 APPLICATION THREADS (any core, may block)                  LIBRARY-OWNED, NOT ON SCHEDULER CORES
 ┌───────────────────────────────────────────────┐         ┌──────────────────────────────────┐
 │ control:  create/query/attach/start/stop       │         │ workers: recovery, auto-detect,  │
 │           discard_queued/update_flows/destroy, │────────▶│ ARP/flow/IGMP for updates, pcap, │
 │           stats, status, info, capacity        │ commands│ stalled-queue reset              │
 │ data:     acquire/submit/publish/release/      │         │ WAKER thread (deadline-driven)   │
 │           withdraw, dequeue/release,           │         │ admin: link monitor, published   │
 │           cq_read, eq_read                     │         │ time base (servo, slewed)        │
 │ wait:     acquire/dequeue/cq/eq with timeout,  │         └───────────────┬──────────────────┘
 │           interrupt/uninterrupt                │                         │ per-class rings
 │ REAPER: walks the lease table, emits results   │                         │
 └───────┬──────────────────────▲─────────────────┘                         │
         │ slot interface        │ lease table: DONE{status, times}         │
         │ (hold/submit/reclaim) │ (release store + seq_cst fence)          │
         │                       │ EQ pending state (tasklet-written, wait-free)
         ▼                       │ armed bit per wait target → waker        ▼
 ┌──────────────────────────────────────────────────────────────────────────────────────┐
 │ PINNED SCHEDULER CORES — tasklets only (never block, no app code, no allocation)     │
 │   builder tasklets: pick up units → admission/pacing decision → build packets        │
 │   transmitter tasklets: pace → rte_eth_tx_burst → idle descriptor cleanup            │
 │   RX tasklets: reassemble → mark READY; deadline check → force-complete              │
 │   every iteration: apply immediate commands (stop, flush, detach, interrupt)         │
 └──────────────────────────────────────────────────────────────────────────────────────┘
                                        │
                  NIC (DPDK PMD / AF_XDP / kernel socket) or the null backend (r3)
```

Three rules make the picture safe:

1. **Arrows into the tasklet world are lock-free hand-offs.** Application threads never
   take a lock that a tasklet also takes. Control operations that must change
   tasklet-owned state are *commands* (r3: two classes, [04 §3.2](04-threading-and-execution.md)):
   - **immediate** commands (stop, flush/discard, detach, interrupt) are checked every
     tasklet iteration outside a packet burst;
   - **boundary** commands (`mtl_session_update_flows`, `mtl_session_set_leg_enabled`,
     timing reconfiguration) apply at a unit boundary.
   The CP applies a command itself when the session is not attached to a scheduler, and
   an ack timeout (default 100 ms) puts the session in ERROR with reason `CMD_TIMEOUT`,
   never an infinite wait `[C5 §4.2]`.
2. **Arrows out of the tasklet world are wait-free stores.** A completion is a release
   store of `DONE` into the lease table plus a fence; an event is a wait-free update of
   pending state. No `pthread_mutex`, no `pthread_cond`, no futex, no allocation, no log
   formatting. If an application thread is asleep waiting, the tasklet only sets a bit;
   the waker thread performs the syscall. A direct, non-blocking eventfd write (W2) is the
   designer's recommendation for sessions with a unit period below 1 ms (maintainer
   decision M6, Q-THR-2) and the default in `MTL_FLAG_TASKLET_THREAD` mode, where
   scheduler threads are not pinned (r3, [04 §5](04-threading-and-execution.md)). Today the
   pipeline BLOCK_GET path does mutex + condvar on the pinned core and can sleep in
   `FUTEX_WAIT` behind an application thread (`st20_pipeline_tx.c:29-45`, `:774-789`)
   `[R03 §3.3, H3]`; Phase 0.5 fixes that on the legacy API (SF-15). The exceptions are
   the I/O syscalls the kernel-socket and AF_XDP backends need by design, which are
   reported per backend ([04 §2.2](04-threading-and-execution.md)).
3. **Heavy per-unit work never runs on a tasklet.** Pixel conversion, RX copies into
   application memory, zero-fill of missing data, codecs and recovery rebuilds run on the
   application thread (reported as caller-context work) or on library workers. Today PR
   #1610 copies whole RX frames inside the tasklet `[R01 D2]`, and TX recovery rebuilds
   mempools there `[R06 E1]`.

Results are **materialised by the reader** (the reaper, under an app-only lock that is
elided with `MTL_SESSION_SINGLE_READER`) from the lease table. That is what makes them
impossible to lose, keeps them in submission order, and removes any need for a new L2
tasklet ([04 §4.1](04-threading-and-execution.md)).

## 2. Layers

```text
 ┌─────────────────────────────────────────────────────────────────────────────────┐
 │ L4  Convenience: mtl_simple.h (r3: open/frame/send/close, started, library pool,│  thin, over L3
 │     completion NONE); SDP-value helpers; format/layout queries; index helpers    │
 │     (mtl_timeline_index_at, mtl_rational_index_at, mtl_rx_align); mtl_flow_parse;│
 │     mtl_fps_parse; CQ dispatcher thread                                          │
 ├─────────────────────────────────────────────────────────────────────────────────┤
 │ L3  Unified session API  (sketch/include/mtl/experimental/mtl_unified.h)         │
 │     instance · port · session · region · buffer · lease · CQ · EQ · timeline ·   │
 │     group · stats/status/info · capability/capacity                              │
 │     + mtl_debug.h (r3: fault injection, only with -Denable_debug_api=true)       │
 ├─────────────────────────────────────────────────────────────────────────────────┤
 │ L2  Session core (new internal): handle table (grow-only chunks, random         │
 │     generations), lifecycle state machine, lease table + reaper, completion      │
 │     modes, EQ pending state + subscription fan-out, waker, timing core (exact    │
 │     rational math, published time base), admission bookkeeping, stats            │
 │     aggregation, command channel                                                 │
 ├─────────────────────────────────────────────────────────────────────────────────┤
 │ L1  Media adapters (internal vtable per essence × direction):                    │
 │     video (st20) · cvideo (st22) · audio (st30) · anc (st40) · fastmeta (st41)   │
 │     v1: over today's st20p/st22p/st30p/st40p pipelines (st41 session),           │
 │     through the slot interface (§2.2)                                             │
 ├─────────────────────────────────────────────────────────────────────────────────┤
 │ L0  Existing engines, extended with the slot interface and hooks (04 §4.4,       │
 │     06 §14 E1–E13): tv_*/rv_* builders & reassembly, transmitters, pacing        │
 │     (RL/TSC/TSN), PTP, datapath queues (PMD/AF_XDP/socket), DMA lender,          │
 │     converters, plugins; null backend (r3: experimental, no NIC, `null:<n>`)     │
 └─────────────────────────────────────────────────────────────────────────────────┘
      Legacy public APIs (st20_*, st20p_*, st30p_*, …) keep sitting on L0 and get every
      engine fix (engines first, 14 §2). In Phase 6 the legacy pipeline functions are
      re-based on L2 as thin wrappers (a commitment; go/no-go at Phase 2 exit, §2.2).
```

**The null backend (r3).** A port spec `null:<n>` gives an instance with `n` ports that
own no NIC. A unit completes at its scheduled launch instant, read from the instance
clock (including the test time source), with a synthetic observed time equal to the
scheduled one; RX sessions on a null port can be fed by a TX session on the same port
(loopback) or stay idle. It needs no root, hugepages or VFIO. It ships in the library as
experimental in Phase 1 and is the substrate of the U tier, the doc examples and the
bindings' tests ([13 §9](13-guarantees-and-tests.md)) `[C5 §3.3]`.

### 2.1 Why a new L2 "session core" and not only an L3 facade

PR #1610 put a facade directly on the low-level session layer and re-implemented pipeline
behaviour there. That produced the metadata-loss bug (`frame->tv_meta = meta` overwrites
the app's timing, `[R01 R7]`) and duplicated logic that `main` has since hardened
(FIFO by sequence number, handle guards, drop-when-late `DROPPED`). `[R01 §6]`

The contracts this design promises — exactly-once outcomes, results that cannot be lost,
stale-handle safety, media-time-derived RTP, per-unit timing — are not properties of any
existing layer. They are owned by one new component (L2) that every essence goes through.

### 2.2 What L1 wraps, and the slot interface L0 must add

Recommendation (review §15 Phase 1, `[R01 Q1 option a]`): **wrap the pipelines** where
they exist, because `main`'s pipelines carry the hardened logic (sequence-number FIFO,
handle guard, `DROPPED` completion, ext-frame release) and already run conversion off the
tasklet in the caller's thread or on plugin threads.

This is **not a thin adapter**. Review C1 showed the pipelines as they are cannot give
ordered, exactly-once results: st20p stores FREE before calling `notify_frame_done`
(`st20_pipeline_tx.c:270` vs `:286`), converting sessions never get a transport-done
callback (`:280-286`), and copy paths report "done" at the end of build
(`st_tx_video_session.c:2130-2134`) `[C1 #1]`.

**r3 — the hooks are the extracted core boundary.** Review C5 read "wrap now, re-base
later, plus a session-layer adapter for progressive" as three passes over the same code
`[C5 §1.6]`. They are not, because the L0 hooks are specified as one internal interface:
the **L2 ↔ engine slot interface**. It is a set of new, internal pipeline entry points
(names indicative; the exact list is S3's output):

| Entry point (per pipeline, TX shown) | Replaces | Contract |
|---|---|---|
| `st20p_tx_hold_slot(ctx, &idx)` / `st20p_tx_release_slot(ctx, idx)` | `get_frame` + L2 writing slot state | a finished slot stays HELD until L2 releases it |
| `st20p_tx_submit_slot(ctx, idx, const struct st20p_slot_submit*)` | `put_frame` + `frame->tv_meta = meta` | media time, launch, cookie and `seq` travel in the call; `seq` is assigned here, at submit, so transmit order is submit order `[C5 §4.11]` |
| `st20p_tx_set_done_hook(ctx, hook, arg)` | `notify_frame_done` + `frame_done_cb_called` | once-only, at transport done, with status and meta, for every path (converting, copy, chain) |
| `st20p_tx_reclaim_queued(ctx, mask)` | `put_frame_abort` (IN_USER only) | CAS CONVERTED/READY → FLUSHED for stop, flush and `withdraw` |
| RX: `st20p_rx_hold_ready` / `st20p_rx_release_slot` / `st20p_rx_force_complete(ctx, deadline)` | `get_frame` / `put_frame`; no RX deadline today | RX lending, the deadline force-complete (Phase 1, [04 §4.4](04-threading-and-execution.md)) |

So "wrap the pipelines now" and "extract a common core" (Q-ARCH-1 option c) are the same
code: L1 calls only these entry points, and never writes another layer's private state.
The consequences:

- the Phase 6 **re-base** of the legacy pipeline public functions onto L2 is a
  **commitment**, not an option. The legacy `st20p_tx_get_frame` becomes `hold_slot` plus
  the st_frame adapter; `put_frame` becomes `submit_slot`. The go/no-go is at the Phase 2
  exit, from S3's costing and the Phase 1–2 measurements;
- the **ST20 progressive session-layer adapter** stays, because st20p has no line mode;
  it is the only L1 adapter that does not go through a pipeline;
- the UB tests target the slot interface, not file-local statics, so they stop rippling
  with every internal refactor ([13](13-guarantees-and-tests.md), `[C5 §1.8]`).

| Essence × dir | v1 adapter wraps | L2 verb → slot interface | Further L0 work |
|---|---|---|---|
| video TX | `st20p_tx` | acquire → `hold_slot`; submit → `submit_slot`; outcome ← done hook | transmitter "last packet handed" hook for no-chain; rejected-at-pick-up callback; idle descriptor cleanup; media time and launch carried separately into `tv_*` (E1) |
| video RX | `st20p_rx` | dequeue → `hold_ready` (conversion in the caller when granted); release → `release_slot` | stop silent recycle of ext frames `[R04 §6 #10]`; deadline force-complete; per-leg arrival times; `BY_INDEX` slot lookup; the `dma_previous_busy` drop counted (SF-46) |
| cvideo TX/RX | `st22p` | same | per-unit status; stats (none today) `[R07 §2.2]`; `rate_mode` CBR (default) / VBR_MAX (E10) |
| audio TX/RX | `st30p` | same | "last packet handed" (done fires at build today, `st_tx_audio_session.c:923-930`); media time from the sample index; carry buffer (06 §8) |
| anc TX/RX | `st40p` | same | ANC RTP from media time (E7); keep-alive on underrun (06 §7.3) |
| fastmeta TX/RX | `st41` session (no pipeline exists) | the adapter owns a small frame pool | frame-level RX (RTP-only today) `[R07 §2.5]`; keep-alive |

The alternative — wrapping the low-level session layer everywhere, as PR #1610 did — needs
the same hooks in `tv_*`/`rv_*` anyway and duplicates pipeline behaviour (Q-ARCH-1).

### 2.3 The incremental alternative, costed (r3)

Every in-tree consumer uses the pipeline layer `[R08 §1.1]`. It is already app-driven
get/put on the caller's thread with BLOCK_GET + timeout + `wake_block`,
`put_frame_abort`, ext frames, `DROP_WHEN_LATE → ST_FRAME_STATUS_DROPPED`, separate
USER_PACING/USER_TIMESTAMP, stats and `update_destination`. Review C5 asked what an
incremental path over it reaches, and at what cost `[C5 §1.3]`. Estimates use C5's
yardstick (one engineer-month ≈ 1.0–1.5 kLOC of library code landed with unit tests).

**What the incremental path reaches:**

| Change on the legacy pipeline API | Effort (EM) | Shared with the unified plan? |
|---|---|---|
| `*_get_frame2(h, &f, timeout)` returning `int` in all four pipelines | 0.5 | no (legacy-only surface) |
| per-frame result fields appended to the library-allocated `st_frame` (status, reason, scheduled/observed times, margin) | 0.5 on top of E3/E4 | E3/E4 yes |
| per-session start/stop | 1–1.5 | yes: the command channel and scheduler detach |
| a timeline object in ops, with exact rational math (Phase 0.5 `st_timeline_*` helper) | 0.5–1 | yes: the E2 math and the 13 §7 oracle |
| `ST30P_TX_FLAG_USER_TIMESTAMP` (missing today, R05 §6) | 0.1 | — |
| ST40P honouring USER_TIMESTAMP without USER_PACING | 0.1–0.2 | — |
| `st41p` by copying the 858-line st40p | 0.5–1 | partly (the L1 fastmeta adapter could wrap it) |
| the armed bit and a non-blocking wake inside the pipelines instead of the tasklet mutex/condvar (SF-15; PR #1610's `mt_session_event.c` salvaged) | 0.5–1 | yes: the same wait-target protocol |
| exactly-once results for legacy users through the slot interface (§2.2) | 2–3 | yes: it *is* the L1 boundary |
| **Total** | **≈ 6–9 EM**, of which ≈ 4–6 EM is work the unified plan needs anyway | |

**What the incremental path cannot reach:**

| Goal | Why the legacy API cannot get there |
|---|---|
| GO-1: one verb set and one metadata struct across essences | four exchange models and twelve metadata structs are the legacy ABI; unifying them *is* a new API |
| `struct_size` on the existing ops structs | the structs have no size field; adding one needs `*_create2` per family, a second API in all but name |
| removing the tasklet `notify_*` callbacks | they can be made optional, not removed, without breaking every legacy consumer |
| the unified lease/result contract for imported memory | ext frames are per-essence raw `{addr, iova}` with five different "done" points `[R04 §6]`; regions with per-device mapping and refcounted destroy need new objects |

**Conclusion: engines first.** The incremental engine work is done **first**
([14 §2](14-implementation-roadmap.md)). It pays legacy users within weeks (Phase 0.5)
and it is the same code L2 needs. The unified API is justified only by the four goals in
the second table; Q-CORE-1 is asked with this costing in view. If the maintainer decides
the four goals are not worth ≈ 40–60 further engineer-months, the engines track and
Phase 0.5 still stand on their own.

## 3. Objects

```text
mtl_instance_h ─┬─ ports[0..n)        capability, capacity, status (link admin/oper, PTP, pacing ways)
                ├─ mtl_region_h[*]    retained memory + per-device mappings + refcount
                ├─ mtl_timeline_h[*]  exact rational anchor T0 (lazy or fixed), optionally named; epoch timeline has a real handle
                ├─ mtl_group_h[*]     atomic arm/start/stop of several sessions (TX and RX)
                ├─ mtl_cq_h[*]        shared completion queues (one armed word per CQ)
                ├─ mtl_eq_h[*]        event queues with a subscription mask; plus the instance EQ
                └─ mtl_session_h[*]   (video | cvideo | audio | anc | fastmeta) × (tx | rx), unique name
                      ├─ flows[1..2]            one per 2022-7 leg; admin + oper state per leg
                      ├─ pool: mtl_buffer_h[k]  immutable layouts over regions (library or attached)
                      ├─ lease table + reaper   mtl_lease_h; results materialised on read
                      ├─ private CQ / private EQ (or bound shared ones)
                      └─ timeline (the epoch timeline by default)
```

| Object | Created by | Lifetime owner | Key property |
|---|---|---|---|
| Instance | the Phase 0 versioned init over `struct mtl_instance_params`, `mtl_instance_acquire_default` (refcounted, merge table), `mtl_instance_open_simple` (L4), or `mtl_instance_from_legacy` (bridge from `mtl_handle`) | app; lives until the last DESTROYING session retires | ports, schedulers, time source; `api_version` per instance |
| Port | instance params (runtime `mtl_port_open` is Phase 6) | instance | per-port capabilities, capacity and live status |
| Memory region | `mtl_mem_alloc` / `mtl_mem_import` | app (explicit destroy) | refcounted; `-MTL_EBUSY` while referenced; page-aligned |
| Buffer | session pool (implicit) or `mtl_buffer_create` | pool or app | immutable layout |
| Lease | `mtl_tx_acquire` / `mtl_rx_dequeue` | app until submitted or released | `session index:16 \| slot:16 \| generation:32`; access moves only through leases |
| Session | `mtl_<essence>_session_create` | app | explicit state machine; starts stopped; deferred destroy; unique name |
| CQ | private per session, or `mtl_cq_create` | session or app | results cannot be lost |
| EQ | private per session + instance EQ, or `mtl_eq_create` with a mask | owner | bounded; coalesces; counts overflow; per-EQ copies |
| Timeline | `mtl_timeline_epoch(mt)`, `mtl_timeline_create` / `open(name)` | app | exact rational origin; RTP/launch derived from it |
| Group | `mtl_group_create` | app | all-or-nothing arm; common start instant |

The full object model, handle representation and state machines are in
[03-object-model-and-lifecycle.md](03-object-model-and-lifecycle.md).

## 4. Data flow

### 4.1 TX, one unit

```text
app thread                               L2                                engine (tasklets and hooks)
──────────                               ──                                ───────────────────────────
mtl_tx_acquire(s,&lease,&view,&hint,t)▶  reap DONE leases (emit results
                                         per completion mode) → FREE
                                         slot FREE→APP_WRITABLE (CAS; MP-safe)
                                         fill slot hint (next media index, submit deadline)
  … app fills planes …
mtl_tx_submit(s,lease,&sub) ─────────▶   validate lease, layout, media index,
                                         horizon; else reject (-MTL_E…), lease stays with app
                                         resolve media time → M, RTP (exact)
                                         lease APP→QUEUED; submit_slot (seq assigned here)
                                         (conversion here if granted = CALLER)
                                                                     ──▶ builder picks up at the slot
                                                                         decision: on time → build+pace;
                                                                         late → late policy
                                                                     ──▶ last packet handed / last mbuf
                                                                         freed (any context):
                                                                         write result into the lease,
                                                                         store DONE (release), fence,
                                                                         if the results target is armed:
                                                                         set wake bit
mtl_cq_read / mtl_tx_reap / next acquire  ◀── the reaper emits the result in submission order
                                              (waker wakes a sleeping waiter)
```

### 4.2 RX, one unit

```text
engine                                   L2                                    app thread
──────                                   ──                                    ──────────
packets for media time M land in a pool buffer (slot RECEIVING; slot by index if configured)
unit complete / deadline (first arrival + unit period + rx_flush_offset) hit
  → store READY {rtp, arrival per leg, pkts, missing}; fence; wake bit if armed
                                         ◀──────────────────────────────────  mtl_rx_dequeue(s,&lease,&view,&unit,t)
                                         build the RX_UNIT record (media unwrap, media_index,
                                         latency, presentation); conversion / zero-fill here
                                         if granted; slot READY→APP_READING
                                                                               … app reads (any thread);
                                                                               TX submissions may hold it …
                                         ◀──────────────────────────────────  mtl_rx_release(s,lease)  (any thread)
                                         slot → FREE when hold count = 0 and the app released it
```

## 5. Control flow

| Operation | Where it runs | How it reaches the tasklet |
|---|---|---|
| create / query / attach / validate | app thread (control) | nothing yet: create reserves resources for every configured leg (never waits for ARP or IGMP), but the session is not attached to a scheduler until start |
| start / arm | app thread | attach the session to its scheduler/manager; it begins at the armed start instant; RX joins at the first start and keeps the join across stop |
| `mtl_session_update_flows` (all legs, one activation) | app thread + worker (ARP, flow, IGMP, queue reservation prepared before commit) | boundary command → tasklet swaps templates or queues at the activation → ack; on failure nothing changes |
| `mtl_session_set_leg_enabled` | app thread | boundary command (applied at the next unit); the leg keeps its reservation |
| `mtl_session_discard_queued` / stop (DRAIN / FLUSH) | app thread | immediate command; the engine stops picking up; queued units are reclaimed (FLUSHED) or sent (DRAIN) |
| `mtl_session_reconfigure` (STOPPED only) | app thread | nothing: the session is detached; pools re-derived, identity kept |
| destroy | app thread | stop(FLUSH) → detach → wait for in-flight callers and NIC references (idle descriptor cleanup; after a bounded wait, a worker resets the stalled queue) → deferred until app leases return |
| stats / status / info read | app thread | per-writer counter blocks, single-pass gauge scans and seqlocked groups; never takes a lock the tasklet uses |
| async faults (link, PTP, recovery) | admin (link monitor) / worker; the tasklet only raises a request flag | recovery work on a worker with a quiesce handshake (04 §2, `[C1 #11]`); recovery never touches `sh_info` |

## 6. Where the design deliberately differs from today

| Topic | Today | New design | Why |
|---|---|---|---|
| Who drives TX | library pulls via `get_next_frame` callback (session) or app get/put (pipeline) | app pushes submissions; the engine picks them up | one model; Rivermax/libfabric-like; no app code on tasklets `[R09 Q1]` |
| Completion | callbacks on tasklet, optional, four possible threads | lease table + reader-side results, one outcome per unit | exactly-once, cannot overflow, no app code on tasklets `[C1 #1]` |
| Timing input | one `timestamp` + flags | media time (session mode) + optional launch override + source kind | standards model `[R12 §10, C2 #1]` |
| RTP | TX-cursor by default (video) | always from media time | ST 2110-10 §7.5/§7.6.3 `[R12 §0]` |
| Memory | raw `{addr, iova}` | region + buffer + lease handles | lifetime, multi-device mapping `[R04 §7]` |
| Session lifecycle | live from create to free | explicit states, start at a time, discard, drain/flush, reconfigure, deferred destroy | preroll, group start, framework teardown `[R13 §6, C3 P2-4]` |
| Downgrades | silent | requested vs granted, reported | `[R07 §5.2]` |
| Handles | raw pointers with an in-memory guard | typed value handles, ID 0 = null, generation-checked | stale safety `[R13 §1.3, C5 §2.8]` |
| Errors | NULL, negative errno overloaded | `MTL_E*` with one meaning each, `mtl_last_error` | `[C5 §2.10, §8.3]` |
| Time on tasklets | callback / PMD read / vDSO per computation | published time base, slewed | no user code or driver calls on pinned cores `[C1 #7, C5 §4.5]` |
| Instance | `mtl_init_params`, 64-bit enum flags, mutated by `mtl_init` | versioned `mtl_instance_params` (Phase 0), merge table for the default instance | `[C5 §8.5]` |

## 7. Architectural risks

| Risk | Consequence | Mitigation |
|---|---|---|
| The slot interface needs work in all four pipelines (held slot, once-only completion, reclaim, rejected-at-pick-up, last-packet hook, RX force-complete) | more engine work than "an adapter" | budget it (14 effort table); UB tests against the real `st*p` code through the slot interface; spike S3 measures it |
| TX completion latency on the chain path is ≈ `nb_tx_desc` packets and unbounded while idle | late results; DRAIN never finishes | idle descriptor cleanup (04 §4.4, Q-CMP-7); report `completion_latency_ns` in `mtl_session_get_info` |
| Lost wake-ups if memory orders are wrong | hangs | seq_cst fences on both sides (04 §5.1); litmus test (G-52) |
| Waker CPU cost and timer slack | CPU burn or high latency | deadline-driven waker, per-scheduler wake words, explicit affinity, W2 below 1 ms if M6 accepts it; S1 reports CPU; budget ≤ 2 % of a core at 100 armed sessions (13 §8) |
| Stats have several writers today; tasklets skip a session whenever someone holds its spinlock | torn stats; pacing hazard | per-writer counter blocks; no spinlock on read paths (08 §2.4) |
| Engine fixes change the legacy wire (RTP ±1 tick, default video RTP, ANC RTP, ST22 CBR) | interop regressions for legacy users | wire-visible changes are opt-in on the legacy API (14 §2); S2 wire diffs; a legacy KahawaiTest and acceptance gate in every phase |
| The legacy/new fork multiplies the pacing test matrix ({RL, TSC, TSN} × {N, NL, W} × {legacy, new} × essence) `[C5 §1.4]` | CI time and flakiness | the fork is tested exhaustively at U/UB against the oracle; the I tier runs only the two default columns (legacy default, unified default) per pacing class |
| The published time base changes pacing accuracy | frame-start error | servo + slewed publication; spike S7 bounds the error versus a direct PHC read |
| Generation-checked handles and in-flight counters add work per call | ~ns per call | array index + compare; counter on its own cache line; DP budget p50 ≤ 150 ns (13 §8) |
