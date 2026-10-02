# Questions

| | |
|---|---|
| Status | Maintained. Every question the design raised (170 IDs in 21 families), with its options, the proposed default or answer, its status and where the answer lives now |
| Date | 2026-10-02 |
| Folded from | the archived OPEN-QUESTIONS (107 IDs) and DECISIONS; design documents 00, 03, 04, 06, 11, 13, 14, 16 §13, 17 §7; interop studies N1 §9 and I1 §9; Kubernetes study K3 §6; simplification studies S8 §9 and S9 §9; reviews C1, C3, C4, C5-response, R2; the open questions of research notes 01–05 and 08 |

This file is the question register of the unified API. [decisions.md](decisions.md) holds the
decisions (D-xx), the seventeen open maintainer decisions (M1–M17) and the open issues
(OI-x); this file holds the questions those decisions answer, one row each, so the reasoning
behind a default is still at hand when the maintainer answers or objects. Read it by family
(§2–§21). Each row gives the question, the options in a few words, the proposed default or the
answer, the status, and the maintained section where the answer is applied. The line under a
table keeps the research reason a default rests on ("why"). Names are the header names of
`sketch/include/mtl/experimental/`; a revision-3 name appears only as "was X". Under D-98 every
"v1" of an NMOS or IPMX question reads "Phase 7, declared under `MTL_LATER`".

## 1. How to read the status column

| Status | Meaning |
|---|---|
| **M#** | a maintainer decision of [decisions.md §2](decisions.md#2-open-maintainer-decisions); the row's default is the designer's recommendation |
| **Proposed default (D-xx)** | answered by the designer and applied; stands unless the maintainer objects in [decisions.md §6](decisions.md#6-objections-to-proposed-defaults) |
| **Proposed default, under M#** | a sub-question that is accepted with the maintainer decision M# |
| **Answered** | answered by the maintainer (only Q-MODE-1 so far) |
| **Open (OI-x)** | the answer is disputed between two documents; [decisions.md §5](decisions.md#5-open-issues-for-the-implementation) holds the issue |
| **Open, action** | needs an outside party (a vendor, a standards body, a measurement) |
| **Superseded by …** | replaced by a later question or decision; the replacement is the answer |

Priority, where the archive gave one: **B** blocked the Phase 1 header, **P** needed before its
phase, **L** later.

## 2. Maintainer decisions and the questions they decide

| Decision | Questions | Topic |
|---|---|---|
| M1 | Q-CORE-1 | the core direction |
| M2 | Q-ARCH-1, Q-ARCH-1a | wrap the pipelines through one slot interface; L0 hooks |
| M3 | Q-TIME-0, Q-TIME-24 | media time primary; source kinds |
| M4 | Q-TIME-18 | the ANC window when video has a slot delay |
| M5 | Q-THR-1 | no user code on tasklets |
| M6 | Q-THR-2 | the wake-up syscall on a pinned tasklet (W2) |
| M7 | Q-TIME-15, Q-TIME-16 | the legacy wire-change policy |
| M8 | Q-ABI-1 | the ABI promise and the experimental DSO |
| M9 | Q-MIG-3 | PR #1610 |
| M10 | Q-MIG-1, Q-PLAN-1 | external review, survey, sequencing and staffing |
| M11 | Q-R4-1 | the revision-4 shape |
| M12 | Q-R4-2, Q-HIDE-1, Q-HIDE-4 | legacy capabilities proposed for removal |
| M13 | Q-R4-3 | wire-visible defaults of the unified API |
| M14 | Q-R4-4, Q-PKT-1…9 | RTP passthrough defaults and phase |
| M15 | Q-R4-5, Q-HIDE-2, Q-HIDE-3 | hiding the legacy headers |
| M16 | Q-R4-6, Q-K8S-1…11 (and Q-K3-1…8 behind them) | Kubernetes pods and crash safety |
| M17 | Q-R4-7, Q-NI-1…10 (and Q-NMOS-1…11, Q-I-1…12 behind them) | NMOS and IPMX (Phase 7, later) |

## 3. Core and architecture (Q-CORE, Q-ARCH)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-CORE-1 (B) | Confirm the five core choices: app-driven push on TX, lossless per-unit results apart from bounded events, no app code on tasklets, one slot and lease contract for library and app memory, a new internal session core (L2) over today's engines | (a) confirm all five; (b) change one; (c) the incremental alternative on the legacy API (≈ 6–9 EM, 4–6 EM shared) | (a); do the engine work first | M1 (D-01, D-02, D-03) | [concepts.md §2](concepts.md#2-the-mental-model), [contract.md §5](contract.md#5-data-path), [engine.md §1](engine.md#1-the-layer-picture) |
| Q-ARCH-1 (B) | What does v1 wrap: the pipelines, the session layer, or a new core? | (a) wrap `st20p`/`st22p`/`st30p`/`st40p` plus the `st41` session; (b) the session layer, as PR #1610; (c) extract a common core first | (a), through one internal slot interface, which is (c)'s extracted core; the re-base of the legacy pipelines on it is committed with a go/no-go after all essences are ported; spike S3 sizes it | M2 (D-06) | [engine.md §3](engine.md#3-the-slot-interface) |
| Q-ARCH-1a (B) | Accept L0 engine hooks in all four pipelines as first-phase scope? | (a) accept (held slot, once-only completion, last-packet hook, rejected-at-pick-up, flush reclaim, idle descriptor cleanup; RX deadline force-complete, reclaim, by-index lookup; submit-time `seq`); (b) wrap the session layer, which needs the same hooks in `tv_*`/`rv_*` | (a), sized by spike S3; for the ST20 branch only the st20p hooks | M2 (D-06) | [engine.md §3](engine.md#3-the-slot-interface), [engine.md §4](engine.md#4-st20-first-where-to-start-in-lib) |
| Q-ARCH-2 (B) | Does the new API reuse today's `mtl_handle` or a new instance object? | (a) reuse `mtl_handle`, versioned init later; (b) a new instance now | (b), changed from (a) after C5 §2.9: typed `mtl_instance_h` over versioned `struct mtl_instance_params`, landed before the new API; `mtl_instance_from_legacy` (`mtl_legacy.h`) bridges | Proposed default (D-44) | [contract.md §2](contract.md#2-instance), [contract.md §2.8](contract.md#28-legacy-bridge), [migration.md §6.2](migration.md#62-the-legacy-bridge-mtl_legacyh) |

Why. Q-CORE-1: what the incremental path cannot reach is one verb set and one metadata struct
for every essence (the legacy ABI has four exchange models and twelve metadata structs),
`struct_size` on the ops structs (a `*_create2` per family), removing the tasklet callbacks
(only optional), and one lease and result contract for imported memory (ext frames are raw
`{addr, iova}` with five different done points). Q-ARCH-1: PR #1610 wrapped the session layer,
re-implemented the pipeline on top and wrote into transport internals, losing the
application's timing metadata; `main`'s pipelines carry the hardened logic. Q-ARCH-1a: review
C1 showed ordered, exactly-once results cannot be built over unmodified pipelines: st20p
stores FREE before calling `notify_frame_done` (`st20_pipeline_tx.c:270` vs `:286`),
converting sessions never get a transport-done callback, copy paths report "done" at the end
of build, and the builder can claim a frame and never complete it (C1 #1, #2, #12, #20).

## 4. Threading (Q-THR)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-THR-1 (B) | May any application code run on a tasklet? | (a) never; (b) opt-in inline hooks with a wait-free contract, an allow-list, a measured budget and automatic disable; (c) only in a manual-progress mode | (a); measure whether busy polling (W0) closes the latency gap, add (b) only if not. A user busy loop may call the inline-safe DP subset; `MTL_SESSION_RX_BY_INDEX` covers MXL's `query_ext_frame` without a callback. (b) is pre-declared as `mtl_session_set_inline_notify` under `MTL_LATER` | M5 (D-04, D-47) | [engine.md §2](engine.md#2-the-pinned-core-rules), `mtl_queue.h` |
| Q-THR-2 (B) | May a pinned tasklet make the wake-up syscall (W2, a non-blocking eventfd write only when a waiter is armed)? | (a) never: an unpinned waker wakes (W3), W2 per-session opt-in; (b) never, and no blocking waits (poll only); (c) always W2 | W3 by default in lcore mode; W2 automatically below a 1 ms unit period; W2 always in `MTL_FLAG_TASKLET_THREAD` mode; a completing context that is not a tasklet wakes directly; spike S1 measures | M6 (D-04, D-68) | [engine.md §7](engine.md#7-waking) |
| Q-THR-2a (P) | Waker scheduling contract: CPU budget, affinity, `SCHED_FIFO`, deadline-driven? | budget; affinity from MtlManager, a housekeeping cpuset or inherited; `SCHED_FIFO` or not; deadline-driven or fixed poll; `rte_power_monitor` | deadline-driven sleep (at most 1 ms with no due time), wake words per scheduler and per RX packet lcore, `PR_SET_TIMERSLACK` = 1, explicit housekeeping affinity, `SCHED_OTHER` by default, `SCHED_FIFO` only as `instance.waker_priority` (`MTL_OPT_WAKER_PRIORITY`, 1–99, needs `CAP_SYS_NICE`); W0 required for 125 µs audio and line mode | Proposed default, under M5 and M6 (D-04); option key: OI-9 | [engine.md §7.2](engine.md#72-the-waker), [deployment.md §1.3](deployment.md#13-threads-priorities-and-affinity) |
| Q-THR-3 (L) | A manual-progress mode (the application owns a scheduler loop and calls `run_once`)? | (a) automatic only; (b) also manual for non-video or app-pinned loops; (c) manual as a peer | (a) in v1; the null backend and the test clock cover testing | Proposed default | [engine.md §2](engine.md#2-the-pinned-core-rules) |
| Q-THR-4 (P) | Where does pixel conversion run? | (a) the caller of submit (today); (b) a library worker pool; (c) per session | (a), reported per session (O(frame) submit for converting sessions); (c) later | Proposed default | [engine.md §2.2](engine.md#22-execution-contexts) |
| Q-THR-5 (B) | How are DP calls protected against a concurrent close? | (a) per-call refcount (today); (b) state gate plus grace period (QSBR); (c) CP-only guard | an in-flight counter on its own cache line plus generation-tagged handles over type-stable tables; elided for `MTL_SESSION_SINGLE_READER` without `MTL_SESSION_MT_SUBMIT` (except releases); QSBR only for a packet-rate DP API | Proposed default (D-07) | [engine.md §5.7](engine.md#57-handle-table) |
| Q-THR-6 (P) | Keep runtime session migration (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`)? | (a) drop for unified sessions; (b) keep with a quiesce and acknowledge handshake; (c) keep as is | (b), changed from (a) by D-98 (no use case cut without approval): `instance.tx_video_migrate`, `instance.rx_video_migrate` (off, as today), `session.migrate` (`MTL_OPT_MIGRATE`, home of `ST20_RX_FLAG_DISABLE_MIGRATE`) | Proposed default; no event reports a move: OI-50 | [engine.md §4.8](engine.md#48-placement-and-quota-today), [migration.md §4.3](migration.md#43-enum-mtl_init_flag-bit-by-bit) |
| Q-THR-7 (P) | Replace the per-read `ptp_get_time_fn` user callback with a published time base? | (a) keep, DP class; (b) a `{tsc_base, tai_base, ratio, seq}` record updated from a normal thread, read wait-free; (c) remove user time | (b) for the unified API (refresh ≤ 100 ms, least-squares ratio servo, slewed, one record per socket; spike S7 bounds the error); legacy keeps (a) | Proposed default (D-31) | [timing.md §2.4](timing.md#24-the-published-time-base), [engine.md §8](engine.md#8-the-published-time-base) |
| Q-THR-7a (P) | Also replace built-in-PTP PHC reads on tasklets with the time base? | (a) yes, for every source; (b) user sources only | (a), gated by spike S7 | Proposed default (D-31) | [timing.md §2.4](timing.md#24-the-published-time-base) |
| Q-THR-8 (B) | Default DP concurrency contract? | (a) single submitter and consumer, debug-asserted, MP option; (b) always MP-safe; (c) per-thread queues | acquire and releases MP-safe; submit one context at a time unless `MTL_SESSION_MT_SUBMIT` (required for exported pools, OI-17); readers under the reaper lock unless `MTL_SESSION_SINGLE_READER`; debug builds detect overlap with a CAS owner flag, not a thread ID | Proposed default (D-50) | [contract.md §5.6](contract.md#56-concurrency), [engine.md §5.5](engine.md#55-concurrency-contracts) |
| Q-THR-9 (P) | Where do TX-hang recovery and RX auto-detect run? | (a) the admin thread; (b) a per-instance worker with a quiesce handshake; (c) split | (b); recovery never touches `sh_info` (SF-41); a worker resets a stalled queue on close or ERROR | Proposed default (D-49) | [engine.md §10](engine.md#10-recovery) |
| Q-THR-10 (P) | What do `MTL_FLAG_TASKLET_THREAD` and `TASKLET_SLEEP` mean for unified sessions? | (a) keep both; (b) keep, and the sleep path performs pending wakes; (c) fold into manual progress | (b); W2 by default in thread mode; scheduler threads call `rte_thread_register`; G-39 measured per mode | Proposed default | [engine.md §2.5](engine.md#25-thread-mode-and-sleep) |

Why. Q-THR-1: slice producers, zero-copy forwarders and the MXL bridge chose tasklet callbacks
for zero-hop latency; they are also why a slow callback stalls every session on a core and why
a stats call from a callback deadlocks. Q-THR-2: today the pipeline does a mutex and condvar on
the pinned core and can sleep behind an application thread. Q-THR-2a: a fixed 20–50 µs poll
costs 5–15 % of a core, timer slack turns 20 µs into about 70 µs, and a waker that inherits
the main lcore's affinity shares a CPU with the application (C1 #4). Q-THR-5: today's guard is a
SEQ_CST RMW per call on a cache line the tasklet reads (`lc_refcnt`, `st20_pipeline_tx.h:35-42`)
and cannot protect a call after free; a state gate alone is unsafe and `rte_rcu_qsbr` with
arbitrary application threads costs a fence per call, so QSBR saves nothing in v1 (C1 #5).
Q-THR-6: migration moves sessions every 6 s check and takes blocking session spinlocks, so it
must quiesce. Q-THR-7a: with built-in PTP every tasklet time read is
`rte_eth_timesync_read_time`, a register read (possibly a PF round trip on iavf);
`clock_gettime` on `/dev/ptpN` is a syscall. Q-THR-8: today's rule ("one app thread per
session and direction") is only in `doc/design.md` and corrupts silently; a GstBufferPool
export acquires in one thread and submits in another, so the rule is "one context at a time".
Q-THR-9: both paths do control-plane work on tasklets today (queue re-acquire, mempool
re-create); the hang threshold is 1 s, so a worker hop adds nothing that matters. Q-THR-3: an
application-owned loop calling `mtl_sched_run_once` (`MTL_LATER`; libfabric `FI_PROGRESS_MANUAL`)
would let GStreamer- and OBS-style applications that own their threads, and non-video media, drive
progress, and could replace `mtl_sch_api.h`; it is unsafe for hardware-paced video, because a late
application loop breaks ST 2110-21, so v1 has automatic progress only (`FI_PROGRESS_AUTO`).

## 5. Lifecycle (Q-LIFE)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-LIFE-1 (B) | Does stop release NIC resources (queues, flows, multicast, RL shaper) or only pause? | (a) pause; (b) release everything; (c) pause by default, a flag to release | pause: the RX join and flow rule are kept until close or a flow update, so restart is instant; a release-on-stop flag is (c), later | Proposed default (D-08) | [contract.md §4.5](contract.md#45-stop) |
| Q-LIFE-2 (B) | Must re-initialisation in one process work (#1341), with a refcounted default instance? | (a) keep EAL alive across uninit; (b) a library-owned refcounted instance; (c) "one init per process" | (a) + (b): EAL stays alive across the last close, so open runs again; `MTL_INSTANCE_SHARED` is refcounted; a later open with differing ports, lcores, time source or options is `-MTL_EEXIST` (`INSTANCE_MISMATCH`), no merge table; runtime port open later | Proposed default (D-44); merge table dropped: OI-1; a second open without `MTL_INSTANCE_SHARED`: OI-49 | [contract.md §2.3](contract.md#23-shared-instance), [implementation-plan.md §8.0](implementation-plan.md#80-requirements-and-the-guarantees-that-close-them) (R-LIFE-5, G-112) |
| Q-LIFE-3 (P) | `mtl_uninit` while unified objects exist? | (a) `-EBUSY`; (b) destroy all in dependency order; (c) (a) plus a destroy-all flag on a versioned uninit | the shared instance tears down in dependency order on its last reference (`mtl_instance_close`); legacy `mtl_uninit` with unified objects returns `-EBUSY` and stops nothing. The archive's destroy-all flag on a versioned uninit has no header | Proposed default; legacy order: OI-3 | [contract.md §2.4](contract.md#24-close-and-shutdown), [contract.md §2.8](contract.md#28-legacy-bridge) |
| Q-LIFE-4 (B) | Close while the application holds leases? | (a) `-EBUSY`, a force flag; (b) succeed and invalidate; (c) defer | (c): `mtl_session_close` stops the session, release still works, it returns 1, `MTL_EVENT_SESSION_RETIRED` when the last lease returns. The revision-3 force flag for (b) is gone (D-88) | Proposed default (D-36) | [contract.md §4.9](contract.md#49-close) |
| Q-LIFE-5 (P) | Accept TX acquire and submit before start (preroll)? | (a) in CREATED, STOPPED, ARMED; (b) ARMED only; (c) no | (a), with `mtl_when.preroll_ns`; the horizon counts from the resolved start; a start that would strand queued units beyond it fails atomically (`BEYOND_HORIZON`) | Proposed default (D-52) | [contract.md §4.2](contract.md#42-what-each-state-allows), [timing.md §6.4](timing.md#64-early-too-far-ahead-and-preroll) |
| Q-LIFE-6 (B) | An async-signal-safe way to unblock waiters? | (a) library calls (atomics plus one write); (b) no, applications use a self-pipe | (a), sticky: `mtl_session_interrupt(s, on)`, `mtl_instance_interrupt(mt, on)`, `mtl_queue_interrupt(q, on)` (per queue, so cancelling one element wakes no other); waits return `-MTL_ECANCELED`; close wins; interrupts cancel data waits only (D-88) | Proposed default (D-30) | [contract.md §7.4](contract.md#74-interrupts) |
| Q-LIFE-6a | Is cancel edge-triggered or sticky, and what clears it? | edge (races with a call about to start); sticky (needs a clear) | sticky; `on = 0` clears it (GStreamer `unlock` / `unlock_stop`) | Proposed default (D-30) | [contract.md §7.4](contract.md#74-interrupts) |
| Q-LIFE-7 (P) | A single-leg session whose link goes down? | (a) stay RUNNING, units dropped, link event, resume; (b) ERROR; (c) configurable | (a): units `MTL_TX_DROPPED` with `LINK_DOWN`, `MTL_EVENT_PORT_LINK`, resume on link up; a link monitor; the dead leg's queue reset so the pool never starves | Proposed default (D-59) | [contract.md §4.10](contract.md#410-status-and-recoverable-incidents), [contract.md §14](contract.md#14-legs-and-st-2022-7) |
| Q-LIFE-8 (L) | Multi-process expectations? | (a) "one process per instance" as a non-goal; (b) a manager-mediated stats channel; (c) DPDK secondary processes | (a); processes share time, not state: the epoch timeline and `mtl_epoch_index_at` | Proposed default | [concepts.md §1.4](concepts.md#14-non-goals-of-the-first-version) (NG5), [deployment.md §2](deployment.md#2-processes-and-instances), [timing.md §10.4](timing.md#104-separate-processes) |
| Q-LIFE-9 (B) | Does create allocate NIC resources, or does start? | (a) create reserves, start attaches and joins; (b) create is cheap, start allocates | (a), refined: create reserves queues, quota and flow-rule capacity but never waits for ARP or IGMP; rules and joins at the first start; flow resolution is a per-leg state (`MTL_EVENT_FLOW_STATE`, `status.leg[]`), units on an unresolved leg drop with `WAITING_NEIGHBOUR` | Proposed default (D-08, D-62) | [contract.md §4.3](contract.md#43-create-and-open), [contract.md §14.2](contract.md#142-admin-oper-and-flow-state) |
| Q-LIFE-10 (P) | On VF reset or port restart: survive, ERROR, or re-create? | (a) pause and resume with `MTL_EVENT_PORT_RESET`; (b) ERROR; (c) the application re-creates | (a) where the backend restores queues and flows, else ERROR with `PORT_RESET` or `DEVICE_GONE`; leaving ERROR is stop, then start (re-reserves or `-MTL_ENODEV`). The gap units' status is disputed: contract.md `MTL_TX_FAILED` / `PORT_RESET`, the archive `MTL_TX_DROPPED` / `RECOVERY` | Proposed default; gap status open (OI-13) | [contract.md §2.6](contract.md#26-device-removal-and-reset), [contract.md §4.8](contract.md#48-error), [engine.md §10](engine.md#10-recovery) |

Why. Q-LIFE-1: releasing lets other sessions reuse queues but makes start fallible and slow;
pausing keeps multicast joined. Q-LIFE-2: FFmpeg, GStreamer and two external projects
re-implement a fragile refcounted singleton because `mtl_uninit` calls `rte_eal_cleanup()` and
a second EAL init is rejected; raised to B because framework plugins cannot meet persona P2
without it (C3 P2-6). Q-LIFE-3: today `mtl_uninit` with live sessions self-deadlocks (SP-01).
Q-LIFE-4: deferral is how GstBufferPool and AVBufferPool behave; `GstBaseSrc::stop` and FFmpeg
`read_close` must return while downstream holds buffers (C3 P2-4). Q-LIFE-5: DeckLink-style
preroll lets playout fill the queue and start at an exact instant. Q-LIFE-6: every sample stops
from a SIGINT handler; today `mtl_abort` wakes nothing and `wake_block` does not return early
(C3 P2-1). Q-LIFE-7: today runtime link loss is not detected; TX loops through hang recovery,
RX simply stops. Q-LIFE-8: the knowledge base claimed secondary-process stats access, but EAL
runs `--in-memory`; only SR-IOV plus MtlManager multi-instance works. Q-LIFE-9: early failure at
create against cheap CREATED sessions that can be configured before committing resources;
reserving at create makes failures surface there and keeps start fast and predictable, which a
start of several sessions at once needs. Q-LIFE-10: the plan scheduled VF-reset handling with no
defined outcome, and Q-LIFE-7 covers only link loss.

## 6. Completions and errors (Q-CMP)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-CMP-1 (B) | Default completion mode, and the back-pressure it implies? | (a) every result; (b) exceptions only; (c) none for library pools, all for imported pools | (c) in r4 form: library pools give no results unless `MTL_SESSION_RESULTS`; application memory always; a cookie without results is `COOKIE_WITHOUT_RESULTS`; `status.blocked_on` (`MTL_BLOCKED_BUFFERS`, `_RESULTS`, `_APP_LEASES`, `_APP_PINS`) explains every stall. Was modes NONE / EXCEPTIONS / ALL (D-35) | Proposed default, under M1 and M11 (D-03, D-77) | [contract.md §6.1](contract.md#61-when-there-are-results), [contract.md §5.5](contract.md#55-back-pressure) |
| Q-CMP-1a | Should acquire return a different code for result starvation than for slot starvation (Rivermax separates `RMX_NO_FREE_CHUNK` from a full send queue)? | a distinct code; one code plus a reason | one code, `-MTL_EAGAIN` for every "nothing now" (D-88); the cause is `status.blocked_on` | Proposed default, under M11 (D-88) | [contract.md §5.5](contract.md#55-back-pressure) |
| Q-CMP-2 (L) | Ship a library-thread callback helper (a thread that reads a queue and calls the application)? | (a) no; (b) a small helper; (c) a first-class callback mode | (b), shipped: `mtl_queue_dispatch_start` / `mtl_queue_dispatch_stop`; the dispatch thread is never a tasklet | Proposed default | [contract.md §10.4](contract.md#104-library-threads-that-call-application-code), `mtl_queue.h` |
| Q-CMP-3 (P) | Error detail for calls with many causes (create, attach, start)? | (a) a thread-local string; (b) a reason enum; (c) both | (c): `mtl_last_error(struct mtl_error_info*, size)` copies code, reason and a detail string; one frozen `enum mtl_reason` (`mtl_reasons.h`); errno contract, no call sequence number (D-84) | Proposed default, under M11 (D-21, D-81, D-84) | [contract.md §8.2](contract.md#82-last-error), [contract.md §8.3](contract.md#83-reasons) |
| Q-CMP-4 (P) | The epoch tick (today's `ST_EVENT_VSYNC`)? | (a) the slot hint only; (b) a lossy opt-in event; (c) both | (c): `mtl_tx_next_slot` plus `MTL_EVENT_EPOCH_TICK`, opt-in through `session.epoch_tick` (was an EQ subscription bit) | Proposed default (D-14) | [contract.md §10.2](contract.md#102-event-types), [timing.md §6.5](timing.md#65-the-slot-hint) |
| Q-CMP-5 (P) | Shared completion queues (one queue for many sessions) in v1? | (a) yes, as poll sets over the member sessions' lease tables; (b) later | (a), in r4 form: one queue type (`mtl_queue.h`) shared by many sessions for results, RX readiness and events, with a per-queue ready summary (one word per scheduler) and one armed word per queue, so a read or arm is O(active) | Proposed default, under M11 (D-79) | [contract.md §10.3](contract.md#103-shared-queues), [engine.md §5.6](engine.md#56-shared-queues-mtl_queueh) |
| Q-CMP-6 (P) | What makes a TX unit `MTL_TX_ON_TIME`? | (a) admission verdict; (b) wire verdict within a tolerance; (c) status from (a), wire error reported separately | (c): admission verdict; a wire verdict is reported separately when NIC timestamps exist | Proposed default (D-14) | [contract.md §6.3](contract.md#63-statuses-and-reasons), [timing.md §6.6](timing.md#66-result-timing-fields) |
| Q-CMP-7 (P) | Idle descriptor recycling with `rte_eth_tx_done_cleanup`? | (a) on idle sessions with frames in flight, rate-limited, dedicated queues only; (b) also shared queues; (c) pad bursts | (a); after the bounded cleanup a worker restarts a stalled dedicated queue or quarantines a shared one (spikes S6, S8) | Proposed default (D-49) | [engine.md §9](engine.md#9-close-and-error-on-a-stalled-queue) |

Why. Q-CMP-1: with every result as a universal default the minimal loop that never reads results
stalls silently after `pool_count` frames (C3 P1-1). Q-CMP-4: events are not a fast path; the slot
hint covers most uses. Q-CMP-6: an admission verdict is always available, while software
observation at `tx_burst` is enqueue time under RL pacing. Q-CMP-7: in chain mode a frame's result
arrives only when a later burst recycles its descriptors (about 512 packets later) and never while
the session is idle, so DRAIN cannot finish (C1 #2; `mt_txq_done_cleanup`).

## 7. Memory (Q-MEM)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-MEM-1 (P) | One terminal result, or an early source-released result plus the terminal one on copy paths? | (a) one terminal result after both; (b) opt-in early release (io_uring zero-copy model) | (a) in v1: the terminal result comes after storage access and transport are both done; (b) opt-in for copy and convert paths in Phase 4 (no header symbol yet), rejected for direct paths | Proposed default (D-18, D-56) | [contract.md §6.2](contract.md#62-lossless-ordered-exactly-once), [contract.md §9.11](contract.md#911-later) |
| Q-MEM-2 (P) | Is IOVA-PA mode in scope for imported memory? | (a) VA only, PA imports copy only; (b) PA for hugepage imports through page tables; (c) drop PA for direct paths | archive: (b) for hugepage memory, copy only otherwise (spike S4). decisions.md OI-22 now recommends copy only for every import in PA mode (`instance.allow_noiommu`), `-MTL_ENOTSUP` with `MTL_SESSION_REQUIRE_DIRECT` | Proposed default for regions (D-16); PA mode open (OI-22) | [deployment.md §4.11](deployment.md#411-memory-and-the-iommu) |
| Q-MEM-3 (L) | Keep a raw-IOVA "pre-mapped" import for today's `mtl_dma_map` users? | (a) yes, validated against the map table; (b) regions only | (a) later, marked expert; no header has it; `mtl_dma_map` maps to `mtl_mem_import`, and `mtl_mem_iova` gives a region's IOVA | Proposed default (later) | [migration.md §2.1](migration.md#21-instance-and-core-utilities-mtl_apih-mtl_sch_apih) |
| Q-MEM-4 (L) | Region close while referenced: `-EBUSY` only, or deferred release with an event? | `-EBUSY`; deferred release | superseded by the r4 header: `mtl_mem_close` always consumes the handle, returns 1 while still referenced, and `MTL_EVENT_REGION_RELEASED` reports the end (the archive had `-MTL_EBUSY` in v1, deferral later) | Superseded by D-16 | [contract.md §9.2](contract.md#92-regions), `mtl_mem.h` |
| Q-MEM-5 (P) | Raise the 8-frame cap for video pools? | (a) keep 8; (b) raise to about 64; (c) no fixed cap | (c): lift it in the engine (E11: dynamic frame arrays, bounded by memory and the 16-bit slot field); until then `max_count = 8` is reported and a larger `pool_count` fails at query and create | Proposed default (D-57) | [contract.md §9.4](contract.md#94-requirements), [engine.md §11](engine.md#11-the-engine-change-list) |
| Q-MEM-6 (P) | RX missing data: zero fill, loss map, or both? | (a) a missing-range map; (b) zero-fill library pools; (c) both | (c): zero fill by default for library pools, in dequeue, never on the tasklet; `MTL_SESSION_RX_NO_FILL` opts out; a loss map (`mtl_rx_get_missing()`). Attached pools and the bitmap source are open | Proposed default (D-37); OI-25, OI-26 | [contract.md §9.8](contract.md#98-rx-placement), [contract.md §6.5](contract.md#65-rx-outcomes) |
| Q-MEM-7 (P) | `MTL_SESSION_REQUIRE_DIRECT` with a pool below the direct minimum? | (a) fail; (b) copy with a report; (c) raise the count | (a): `POOL_TOO_SMALL` (`-MTL_ENOTSUP` at create); the minimum (`min_count_direct` = 1 + ceil(512 / packets per unit) for 512 TX descriptors) is 2 for 512 packets per unit or more, 3 for 256–511, more below 256 (5 at 128), reported by the buffer requirements | Proposed default (D-17) | [contract.md §9.4](contract.md#94-requirements), [contract.md §9.7](contract.md#97-direct-and-copy) |
| Q-MEM-8 (L) | Device memory (GPU, DMA-BUF) and what "GPU direct" means | (a) a copy-only device domain until a direct path exists; (b) remove the flag, import as host memory; (c) real device DMA now | (a); GPU pinned host memory (`cudaHostAlloc`, `zeMemAllocHost`) is a plain host import and the supported GPU ingest path; device memory is `mtl_mem_import_device` (`MTL_LATER`, Phases 4–6) | Proposed default | [contract.md §9.11](contract.md#911-later), `mtl_mem.h` |
| Q-MEM-9 (L) | Header split: drop, keep internal, or port the DPDK patch? | (a) drop; (b) internal RX optimisation; (c) port the patch and a region-pool import | (a) for the unified API; the legacy flag is a removal candidate (CUT-1) | Proposed default; legacy row under M12 | [concepts.md §1.4](concepts.md#14-non-goals-of-the-first-version) (NG6), [migration.md §4.7](migration.md#47-st20_rx_ops-st20_rx_flag_) |
| Q-MEM-10 (P) | Dynamic (unattached) buffers in v1? | (a) later; (b) v1 | moving-cursor TX in Phase 4 through `mtl_tx_acquire_layout` (`MTL_LATER`); RX per-unit destination `mtl_rx_provide` (`MTL_LATER`, Phase 4); the revision-3 dynamic pool has no header | Proposed default (D-56); OI-20 | [contract.md §9.11](contract.md#911-later) |
| Q-MEM-11 (P) | Buffers shared by two sessions, RX → TX lease transfer? | (a) one buffer attached to several sessions and an RX-to-TX transfer; (b) later | a slot belongs to one pool; sessions share bytes through slots over one region; `unit.hold` keeps an RX slot until the TX unit is done, so one RX unit feeds N TX sessions zero-copy through `mtl_tx_send_slot`; the revision-3 lease-transfer call is gone | Proposed default (D-55) | [contract.md §9.6](contract.md#96-holds-and-forwarding) |
| Q-MEM-12 (P) | IOVA of imported non-hugepage memory: MTL's invented range (`mt_dma.c:21`, from `0x10000`) or IOVA = VA? | (a) IOVA = VA; (b) the invented range, made collision-safe | archive: (b) unless spike S4 shows (a) is safe with `rte_extmem_register`; decisions.md OI-41 recommends (a) where the IOMMU allows. D-16 records it as answered, engine.md keeps it open | Open (OI-41) | [engine.md §5.8](engine.md#58-regions-and-dma-today) |

Why. Q-MEM-1: on copy paths MTL stops reading the application buffer long before the transport
finishes, and holding the lease costs pool size; the framework-pool starvation case needs (b).
Q-MEM-10: codecs and frameworks that cannot
pre-register every surface need a TX unit bound to new memory per acquire; fixed attached pools
come first (the maintainer's review baseline). Q-MEM-5: deep preroll playout, frameworks holding many RX buffers and direct TX minimum counts
all exceed 8; preroll is 133 ms at 59.94 with 8 frames. Q-MEM-6: recycled RX frames are not
re-zeroed today, so lost packets show the previous frame; the repository rule is that gaps read
as zeros (C3 P1-7, C1 #16). Q-MEM-8: today's "GPU direct" is a CPU copy into Level Zero shared
memory, RX only; no DMA-BUF, CUDA or gpudev path exists. Q-MEM-9: header split is compiled out
on the pinned DPDK 26.07 (no `patches/dpdk/26.07/hdr_split/`), single port, and its pool layout
does not fit per-slot leases. Q-MEM-12: IOVA = VA matches DPDK VA mode but can collide with
DPDK's own mappings; the invented range avoids that but is unchecked against the hugepage space.

## 8. Time (Q-TIME)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-TIME-0 (B) | Confirm the two-timestamp model asked for in the PR review ("one timestamp for user pacing, one for user timestamp") | (a) media time primary, launch derived, optional launch override (`MTL_SUBMIT_NOT_BEFORE`, `MTL_SUBMIT_EXACT`); (b) two independent required fields; (c) one timestamp plus flags | (a); answering it also unblocks Phase 0.5 (the legacy `st_timeline_*` helper) | M3 (D-09) | [timing.md §4](timing.md#4-media-time-on-tx), [timing.md §5](timing.md#5-launch-time) |
| Q-TIME-1 (B) | Time source default; refuse timed sessions without a locked TAI source? | (a) refuse INDEX/TAI without lock; (b) allow with a labelled estimated clock and a warning event; (c) status quo | (b): such sessions run on `MTL_TIME_SOURCE_SYSTEM_TAI`, flagged ESTIMATED, with a warning; external PHC and `CLOCK_TAI` sources added. FFmpeg's `CLOCK_TAI` on a host without ptp4l becomes estimated (a listed behaviour change) | Proposed default, under M11 (D-31, D-85) | [timing.md §2.2](timing.md#22-time-sources) |
| Q-TIME-2 (P) | How to surface and handle clock steps (UTC → PHC at first sync; grandmaster change)? | (a) an event, sessions keep indices; (b) a stream reset; (c) session error | a per-timeline step policy: `MTL_STEP_REANCHOR` for created timelines (T0 += step, implicit DISCONTINUITY), `MTL_STEP_KEEP` for the epoch; AUTO never emits a smaller index after a backward step; always `MTL_EVENT_TIME_STEP` | Proposed default (D-31); `MTL_STEP_DEFAULT` scope: OI-31; `MTL_ANCHOR_AT_TAI` timelines: OI-52 | [timing.md §12](timing.md#12-time-steps-and-step-policies) |
| Q-TIME-3 (P) | Horizon: how far ahead may a unit be submitted? | (a) fixed 1 s; (b) configurable, 1 s; (c) from pool depth | (b): `tx.horizon_ns`, 1 s from max(now, resolved start) | Proposed default (D-52) | [timing.md §6.4](timing.md#64-early-too-far-ahead-and-preroll) |
| Q-TIME-4 (P) | Underrun repeat-last (resend the previous unit when none came in time)? | (a) later; (b) v1; (c) never | (a), later (Phase 6); v1 underrun policies are skip, empty ANC, fastmeta keep-alive and a silence option (`tx.underrun_policy`) | Proposed default (D-13) | [timing.md §6.3](timing.md#63-underrun-policy-no-unit-for-a-slot) |
| Q-TIME-5 (P) | Replace `rtp_timestamp_delta_us` (a non-zero RTP offset, forbidden by ST 2110-10 §7.3)? | (a) a whole-unit index offset plus a TROFFSET knob; (b) keep, documented non-compliant; (c) drop | (a): `tx.index_offset` (whole units, lip-sync) and `media_time_offset_ns` (ns; the compliance trim and a just-in-time producer's latency in TAI mode); legacy keeps the delta | Proposed default (D-53) | [timing.md §4.5](timing.md#45-media-time-offsets), [timing.md §10.3](timing.md#103-lip-sync-trims-and-offset-inheritance) |
| Q-TIME-6 (P) | Audio continuity: strict, tolerance, or explicit discontinuity? | (a) strict plus `MTL_SUBMIT_DISCONTINUITY`; (b) a tolerance keeping the grid; (c) re-anchor every submit (today) | (a) + (b): CAPTURE and TAI absorb ±one packet; forward gaps keep the packet grid with silence fill; overlaps are trimmed; only a DISCONTINUITY re-phases; `mtl_tx_write` for copy essences | Proposed default (D-41) | [timing.md §8](timing.md#8-audio) |
| Q-TIME-7 (P) | Pacing engine per port (today) or per session? | (a) per port, sessions request a class and see the grant; (b) per session; (c) status quo | (a); runtime downgrades are `MTL_EVENT_PACING_CHANGED` | Proposed default (D-19) | [timing.md §5.5](timing.md#55-pacing-classes) |
| Q-TIME-8 (P) | When one group member fails, do the others continue? | (a) continue, event only; (b) stop the group; (c) per-group policy | (a), with groups gone (start arrays, D-78): a session in ERROR posts `MTL_EVENT_SESSION_STATE`, the others continue; restart is stop, fix, `mtl_session_start(&s, 1, NULL, NULL)`, which rejoins at the next feasible index with the inherited offset | Proposed default (D-78) | [timing.md §7.1](timing.md#71-starting-sessions-together) |
| Q-TIME-9 (L) | Media-time-ordered submission instead of FIFO? | yes; no | no in v1: FIFO with strictly increasing media time | Proposed default | [timing.md §4.4](timing.md#44-ordering-gaps-and-discontinuities) |
| Q-TIME-10 (P) | Which observed TX times can E810/E830 give, and how accurately? | TSC at `tx_burst`; per-packet NIC TX timestamps; TSN launch confirmation on E830 | software estimates first, flagged (`MTL_TXR_ESTIMATED`); NIC timestamps (`MTL_TXR_SENT_HW`) after spike S5 | Proposed default (D-14) | [timing.md §6.6](timing.md#66-result-timing-fields) |
| Q-TIME-11 (P) | Audio launch offset default (today packets leave exactly at their RTP instant) | (a) one packet time; (b) configurable, 0; (c) configurable, ptime/2; (d) from the granted pacing profile's worst early error | (d) for PLAYBACK, clamped to ptime/2; CAPTURE uses `min_tx_delay_ns`; reported as TSDELAY; key `audio.launch_offset_ns` | Proposed default | [timing.md §5.3](timing.md#53-derived-launch-per-essence), [timing.md §8](timing.md#8-audio) |
| Q-TIME-12 (L) | RX scheduled delivery (hold units until media time + link offset)? | v1; later | later; v1 reports the presentation time and takes `rx.link_offset_ns` (presentation = media + link offset; `MTL_LINK_OFFSET_AUTO`) | Proposed default | [timing.md §11.3](timing.md#113-presentation-and-link-offset), [concepts.md §1.4](concepts.md#14-non-goals-of-the-first-version) (NG7) |
| Q-TIME-13 (L) | Row units: rows not published by their packet deadline? | STALL (today's slice behaviour), PAD, TRUNCATE | `MTL_ROWS_TRUNCATE` by default with a result, `MTL_ROWS_PAD` opt-in (`tx.rows_late`), STALL legacy only | Proposed default | [timing.md §6.7](timing.md#67-row-units-progressive-submission) |
| Q-TIME-14 (P) | Which ST 2022-7 skew class does dual-leg RX claim? | (a) class D (150 µs); (b) a configurable window; (c) class A (10 ms) | report the tolerated path differential; `rx.skew_budget_ns` defaults to class A (10 ms) where memory allows; warn when `rx.link_offset_ns` is smaller; the RX due time uses the budget | Proposed default | [timing.md §11.7](timing.md#117-due-time-completion-and-2022-7-skew) |
| Q-TIME-15 (B) | RTP rounding: `floor` everywhere or only in the new API? Also the ns tie rule | (a) floor in the new API, legacy unchanged; (b) everywhere; (c) keep rounding | `floor(M × R) mod 2^32` with exact rational math in the unified API; for legacy, M7 recommends opt-in flags and a pcap-diff gate (G-99). The tie rule is open: the code rounds ties down (`st_muldiv_u64_round_closest`) | M7 (D-09, D-11); ties: OI-44 | [timing.md §4.2](timing.md#42-derived-rtp-exact-and-its-exceptions), [timing.md §3.5](timing.md#35-exact-arithmetic) |
| Q-TIME-16 (P) | Flip the legacy default video RTP to the frame epoch too? | (a) new API only; (b) legacy too, old behaviour behind a flag; (c) never | unified: the frame epoch `N × TFRAME` (D-10); legacy: (b) with the flag off by default (`ST20_TX_FLAG_RTP_FROM_MEDIA_TIME`), revisited at the ABI freeze | M7 (D-10, D-24) | [timing.md §4.2](timing.md#42-derived-rtp-exact-and-its-exceptions), [migration.md §6.3](migration.md#63-shared-engines-separate-wire-defaults) |
| Q-TIME-17 (P) | Sender type W: linear read schedule (ST 2110-21:2022 §7.1.4) for TX and the RX parser, and an explicit NL? | yes; after measurement | yes in the unified API; legacy behind `ST20_TX_FLAG_LINEAR_SCHEDULE` (M7); `MTL_SENDER_NL` explicit | Proposed default | [timing.md §5.1](timing.md#51-the-st-2110-21-model) |
| Q-TIME-18 (B) | Live ANC with a video slot delay L ≥ 1: which frame anchors the ST 2110-40 window? | (a) ANC always L = 0, video L ≥ 1 only without ANC in the start; (b) ANC follows video and is flagged non-compliant; (c) live ANC needs the gateway model; (d) ANC keeps the video RTP and is sent in the window of the frame its video unit is transmitted in (N = E⁻¹(M) + L_v) | (d); strict media-frame anchoring opt-in (`MTL_ANC_WINDOW_MEDIA`, `anc.window`) | M4 (D-67; the slot delay L from D-39) | [timing.md §9.2](timing.md#92-the-anc-transmit-window-and-live-anc) |
| Q-TIME-19 (P) | Common grid: exact for every member, or video/ANC only with audio floor-aligned; a cap | (a) exact for every member; (b) G over video/ANC frame starts, 90 kHz and every audio rate, except audio rates that would push G above the 1 s cap (excluded, floor-aligned, sub-sample phase reported); ST41 excluded; a video/ANC grid above the cap is `-MTL_EINVAL` | (b); interlaced members contribute TFRAME; grid fastmeta joins G | Proposed default | [timing.md §3.4](timing.md#34-the-common-grid) |
| Q-TIME-20 (L) | Timecode (ST 12-1 from media time, RP 188 / ST 12-2 into ST40) and 1001 audio-cadence helpers? | (a) helpers later; (b) v1; (c) out of scope | (a) | Proposed default (later) | [timing.md §8](timing.md#8-audio) (cadence), [timing.md §17](timing.md#17-open-items) |
| Q-TIME-21 (P) | Legacy USER_PACING mapping: nearest epoch, nearest slot, or not-before? | (a) the shim keeps nearest epoch; (b) nearest slot; (c) `MTL_SUBMIT_NOT_BEFORE` | (a) for the legacy shim, (c) as the documented unified equivalent; migrated applications' TX moves by TROFFSET − VRX0·TRS (604–619 µs at 1080p59.94, by VRX0) | Proposed default | [timing.md §14.1](timing.md#141-today--unified), [migration.md §1.6](migration.md#16-behaviour-a-migrating-application-notices) |
| Q-TIME-22 (P) | CTM/LLTM-conformant ANC packet scheduling (a wire change for multi-packet ANC)? | (a) v1 with a deterministic target delay; (b) later | (a) in the engines track (E7): on by default in the unified API (`anc.timing_model` `MTL_ANC_CTM` / `MTL_ANC_LLTM`, `anc.target_delay_ns`), opt-in on the legacy API | Proposed default | [timing.md §9.1](timing.md#91-anc-units-rtp-and-limits) |
| Q-TIME-23 (B) | Default late policy per source kind; reject or drop too-late requests? | (a) DROP for PLAYBACK/CAPTURE, RESLOT for AUTO, synchronous `-MTL_ERANGE` only when knowable; (b) DROP everywhere; (c) bounded SEND_LATE for live | the media mode wins: AUTO → `MTL_LATE_RESLOT`, INDEX/TAI → `MTL_LATE_DROP` for every source kind; a CAPTURE session raises `MTL_EVENT_TIMING_INFEASIBLE` on its first late unit | Proposed default (D-13) | [timing.md §6.2](timing.md#62-late-policy) |
| Q-TIME-24 (B) | Confirm source kinds for live timing | (a) `MTL_SOURCE_PLAYBACK` / `MTL_SOURCE_CAPTURE` / `MTL_SOURCE_GATEWAY` with `min_tx_delay_ns`, slot = first N whose first-packet time ≥ M + `min_tx_delay_ns`, L reported; (b) an integer slot delay only | (a); CAPTURE default `min_tx_delay_ns` = one unit period plus the pick-up lead (L = 2 at 1080p59.94 for frame capture); the ST 2110-10 §7.6.3 bound replaced by `tx.link_offset_budget_ns` warnings | M3 (D-39) | [timing.md §5.2](timing.md#52-source-kinds-min_tx_delay_ns-and-the-slot-rule) |
| Q-TIME-25 (P) | TAI-mode snapping modes and tolerances? | (a) nearest for PLAYBACK, locked phase (TFRAME/4) for CAPTURE; (b) nearest only | changed to nearest for every source kind: `MTL_SNAP_NEAREST` with a TFRAME/8 hysteresis band (a free-running source adapts by drop and gap, never locks out); `MTL_SNAP_LOCKED_PHASE` opt-in, relock after 3 off-grid units (`MTL_OFF_GRID_RELOCK`); duplicates are results (`DUPLICATE_SLOT`) | Proposed default (D-38) | [timing.md §4.3](timing.md#43-snapping-tai-mode-times) |
| Q-TIME-26 (B) | ST 2110-22 rate mode: constant bitrate by default? | (a) CBR with a per-unit ceiling and synchronous oversize rejection, VBR opt-in; (b) VBR default | (a): `MTL_CVIDEO_CBR` default; `MTL_CVIDEO_VBR_MAX` (today) opt-in, flagged `MTL_INFO_NON_COMPLIANT`; an oversize codestream fails at submit `-MTL_ENOSPC` (`CODESTREAM_OVERSIZE`); the lease outcome on failure: OI-15 | Proposed default (D-32); wire default under M13 | [timing.md §5.3](timing.md#53-derived-launch-per-essence) |
| Q-TIME-27 (P) | After how many stale units does RX relock onto a restarted sender? | (a) 3 distinct increasing stale RTP values in a row; (b) configurable from v1 | (a); configurable later if field data asks; status RELOCKED: OI-34 | Proposed default | [timing.md §11.5](timing.md#115-duplicates-stale-units-gaps-and-relock) |

Why. Q-TIME-0: the answer decides every timing field of `struct mtl_unit` and the answer to the
A/V question; issue #1211 asks to "pass both". Q-TIME-1: the default today is `CLOCK_REALTIME`
(UTC) labelled TAI (`dev/mt_dev.c:2288`). Q-TIME-4: live broadcast often prefers a freeze to a
gap, but repeating the last unit needs buffer-retention rules (the slot must stay out of the
pool), so it waits for a later phase.
Q-TIME-2: without re-anchoring, a file timeline drops forever after the +37 s UTC → PHC step
(C2). Q-TIME-5: one downstream engine uses the delta to fake a TROFF shift. Q-TIME-6: a live
sound card drifts against PTP, and "equal the grid point or be rejected" forces resets. Q-TIME-7:
ST22 silently drops to software pacing, a shared TX queue forces the whole port to software, and
a runtime RL failure flips the port while sessions still believe RL. Q-TIME-11: one ptime would
exceed JT-NM's 1 ms limit at Level A; RL and TSN early errors are tens of µs. Q-TIME-15: code and
unit tests pin round-to-nearest; at 59.94 and 23.976 half the frames differ by one tick; spike S2
diffs the wire. Q-TIME-16: today MTL's own ST20 and ST40 streams carry different RTP for the same
frame (about 57 ticks at 1080p59.94). Q-TIME-18: a window anchored on the frame the ANC RTP
denotes closes ≈ 15.7 ms (CTM) before a CAPTURE frame with L = 1 can leave, and ANC RTP ≠ video
RTP would break ST 2110-40 §5.4. Q-TIME-19: 1001 families with 44.1 kHz need a 3.34 s grid, mixed
video families 20–40 s, ST41 rates are unbounded. Q-TIME-22: today ANC packets spread over the
whole frame (`st_tx_ancillary_session.c:1108`). Q-TIME-23: lateness is only known at pick-up, so
the unit is accepted and dropped there (its RTP slot consumed) rather than rejected. Q-TIME-24:
with a slot delay of 0 every captured frame is late (it exists only after its sampling instant).
Q-TIME-25: nearest-unit snapping of jittery capture timestamps creates duplicates and gaps; a
locked phase by default lets a 50 ppm crystal drift out. Q-TIME-26: ST 2110-22 requires constant
bytes and packets per frame; C5 §5.12 found strict CBR with deferred drops hostile to encoders
that are not byte-exact, hence the synchronous ceiling check. Q-TIME-27: today's implicit rule is
20 consecutive redundant errors per packet on every port.

## 9. Observability (Q-OBS)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-OBS-1 (P) | Stats epochs instead of reset? | (a) cumulative only; (b) cumulative plus an explicit "new epoch" with a start time; (c) keep reset | changed to (a): counters cumulative for the session's life, no reset call and no stats epoch; `*_max` values become windowed maxima (1 s, 60 s) plus histograms (`hist.*` keys); legacy reset unchanged | Proposed default, under M11 (D-80) | [contract.md §11.1](contract.md#111-rules) |
| Q-OBS-2 (L) | An out-of-process stats reader (Rivermax has only this kind)? | v1; later | later, possibly through MtlManager shared memory; an in-process exporter needs only `mtl_instance_list_sessions`, `mtl_session_get_info` and `mtl_stat_read` | Proposed default (later) | [migration.md §11.8](migration.md#118-stats-exporters) |
| Q-OBS-3 (P) | ST 2110-21 timing-parser summary always on? | (a) a cheap summary always on, per-unit detail opt-in; (b) fully opt-in | (b): `rx.timing_parser` opt-in, integer math; "NIC-timestamped packets only" conflicts with port first, since today's parser also runs on software time on VFs | Proposed default; open (OI-29) | [timing.md §11.8](timing.md#118-rx-timing-fields), [timing.md §17](timing.md#17-open-items) |

Why. Q-OBS-1: reset-on-demand breaks as soon as two readers exist (WebRTC stats and
OpenTelemetry cumulative temporality; SRT and ALSA reset-on-read rejected). Q-OBS-3: today the
parser does double-precision divisions per packet (C1).

## 10. Modes and surfaces (Q-MODE)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-MODE-1 (P) | The RTP / packet level in the unified API? | (a) legacy only in v1, reserve a packet unit; (b) a separate `mtl_rtp_*` API; (c) drop | maintainer, 2026-10-01: RTP passthrough must stay supported in a coherent API and `include/st20_api.h` should stop being public, so the unified API provides it: packet units (`MTL_UNIT_PACKETS`) on every essence plus the generic `MTL_RTP` essence. The remaining choices are Q-PKT-1…9 | Answered (D-82); details under M14 | [contract.md §13](contract.md#13-packet-units) |
| Q-MODE-2 (P) | Network header controls: DSCP, TTL, VLAN, IPv6 | DSCP and TTL now; VLAN, IPv6 reserved | `struct mtl_flow` carries `dscp` (0 = the profile's; ST 2110: CS0, today's value; `MTL_FLOWF_DSCP_LITERAL`) and `ttl` (0 = 64); VLAN and IPv6 reserved | Proposed default (D-26) | [contract.md §3.2](contract.md#32-common-fields) |
| Q-MODE-3 (B) | ST 2110-22: its own create, or a codec sub-struct in the video config? | own create; sub-struct | its own essence `MTL_CVIDEO` in the one `struct mtl_session_config` (revision 3 had a separate create function) | Proposed default, under M11 (D-32, D-72) | [contract.md §3.3](contract.md#33-essence-members) |
| Q-MODE-4 (L) | Codec and converter plugin ABI (`st_plugin_*`, `st22_encoder/decoder_*`, `st20_converter_*`): freeze, redesign, replace? And the ≈ 105 conversion functions? | freeze; redesign on acquire/submit; replace | freeze ABI v1; plugin ABI v2 (`mtl_plugin.h`: host dispatch table, no libmtl link, no tasklet callback) before stage F, v1 loaded until F+2; standalone conversion through `mtl_convert.h`, per-pair converters internal (CUT-4) | Proposed default, under M15 (D-83) | [migration.md §9](migration.md#9-codec-plugin-abi-v2-mtl_pluginh) |
| Q-MODE-5 (L) | `DATA_PATH_ONLY` (application-managed flows): keep, fix, drop? | (a) drop; (b) fix with a test; (c) an "external steering" port mode | verify with a test first; not in the unified API; with queue meta a removal candidate (M12); if kept, an extension table (`mtl_open_ext`) after the verification test (H-17) | Proposed default; removal under M12 | [migration.md §4.1](migration.md#41-fields-and-flags-every-session-ops-shares) |
| Q-MODE-6 (P) | SDP: a helper in the library, values only, or nothing? | (a) render and parse in `lib/`; (b) values in `mtl_session_get_info`, text in `ecosystem/` or `app/`; (c) nothing | was (b) plus a parse helper; replaced: `mtl_sdp.h` renders and parses on public calls only (Phase 7, `MTL_LATER`), because every NMOS and IPMX sender publishes an SDP per activation | Superseded by D-94 (Q-NI-1, under M17) | [nmos-ipmx.md §5](nmos-ipmx.md#5-sdp-helper-mtl_sdph-mtl_later) |
| Q-MODE-7 (L) | Fate of legacy and experimental surfaces: `st20rc`, DPDK AF_XDP and AF_PACKET PMDs, the SysV lcore allocator, UDP remnants, non-RFC 4175 formats, `st_convert_internal.h` installed | remove; freeze; per release | remove the UDP remnants now; `st20rc` and the two DPDK PMDs are removal candidates (M12); the SysV allocator is replaced (D-92); non-RFC 4175 formats kept with `MTL_INFO_NON_COMPLIANT`; `st_convert_internal.h` goes internal | Proposed default; rows under M12 | [migration.md §4.16](migration.md#416-coverage-of-todays-modes), [contract.md §3.3](contract.md#33-essence-members) |
| Q-MODE-8 (L) | Scope of MTL's NACK retransmission (`rtcp` today) | (a) video only, remove the no-op flags elsewhere; (b) generalise; (c) later | (a): ST20 and ST22 only, renamed `MTL_OPT_RTX` (`rtx.*`, Q-NI-4); the ST30/ST40/ST41 flags are removal candidates (CUT-7, M12) | Proposed default | [migration.md §4.1](migration.md#41-fields-and-flags-every-session-ops-shares), [contract.md §12.6](contract.md#126-key-groups) |

Why. Q-MODE-1: packet mode is the only route to ST 2022-6 and what Rivermax users expect; today
it hands out mbufs. Q-MODE-2: broadcast plants mark DSCP; today TOS = 0 and TTL = 64 are
hard-coded, no VLAN, IPv4 only. Q-MODE-3: PR #1610's `bool compressed` silently created an
ST20 session. Q-MODE-5: it appears to dereference a NULL flow on non-socket backends (SP-02).
Q-MODE-6: an SDP parse helper was the largest single migration win for Rivermax users (C3 P6-3).
Q-MODE-8: the retransmission is non-standard and implemented only for ST20/ST22; the
ST30/40/41 flags do nothing today.

## 11. ABI (Q-ABI)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-ABI-1 (B) | ABI promise level (soname, version script, visibility) | (a) API stability, soname bump every release; (b) stable ABI for the new API, legacy unversioned; (c) symbol versions everywhere | (b): libmtl gets a soname and version script first; the unified API ships as `libmtl_unified.so.0.<rev>` (node `MTL_UNIFIED_EXPERIMENTAL`, soname bumped on each incompatible change) and moves to `MTL_1.0` at the freeze | M8 (D-23) | [migration.md §7](migration.md#7-library-and-abi-plan) |
| Q-ABI-2 (P) | A versioned `mtl_init` replacement (`struct_size`, API version, no mutation of the caller's params, no 64-bit enum)? | yes; no | yes, and earlier: before the new API (Phase 0): `mtl_instance_open` over `struct mtl_instance_params` | Proposed default (D-44) | [contract.md §2.1](contract.md#21-open) |
| Q-ABI-3 (B) | Reserve an extension chain in create-time configs? | (a) `struct_size` only; (b) an extension-chain pointer, NULL in v1; (c) named, versioned backend-extension tables (`fi_open_ops` style) | r4 form: `struct_size` plus reserved fields; unknown or non-zero reserved fields are rejected; option keys carry the long tail; (c) as `mtl_open_ext` (`mtl_util.h`) for backend extensions | Proposed default, under M11 (D-33, D-73, D-74) | [contract.md §1](contract.md#1-the-rules-r1r8) (R3, R7), [contract.md §12](contract.md#12-options) |
| Q-ABI-4 (L) | Re-base the legacy pipeline APIs on the new core later? | yes; no | yes for the pipelines (after all essences are ported, with a go/no-go); in r4 the legacy headers then go non-public in stages (M15) instead of staying public and frozen | Proposed default (D-24); hiding under M15 | [migration.md §8](migration.md#8-hiding-the-legacy-headers) |
| Q-ABI-5 (B) | Names: header, prefix, source directory | `include/mtl_session_api.h` (PR #1610); a new `include/mtl/` family | headers in `include/mtl/experimental/` until the freeze (17 headers, core `mtl.h`; revision 3's single header and simple layer are gone, D-65 superseded), then `include/mtl/`; prefix `mtl_`, verbs `mtl_tx_*` / `mtl_rx_*`; essences `MTL_VIDEO`, `MTL_CVIDEO`, `MTL_AUDIO`, `MTL_ANC`, `MTL_FASTMETA`, `MTL_RTP`; source `lib/src/unified/`; DSO `libmtl_unified`; no PR #1610 names | Proposed default, under M11 (D-34, D-71) | [contract.md](contract.md), [migration.md §14](migration.md#14-where-the-code-goes-and-pr-1610) |
| Q-ABI-6 (P) | Windows parity for the new API | (a) first-class (event `HANDLE` waits); (b) builds, fd calls `-MTL_ENOTSUP`; (c) Linux only | (a)-lite: one ABI on every OS (portable wait handles, `MTL_E*` codes); a compile-only Windows CI job; runtime support follows the legacy library | Proposed default (D-43) | [deployment.md §6](deployment.md#6-windows), [migration.md §7.4](migration.md#74-windows) |
| Q-ABI-7 (P) | Formalise C11 in `lib/` (atomics are in 13 files) with public headers C99 and C++ clean? | yes; no | yes; update the coding instructions and `CLAUDE.md` (DD-12) | Proposed default | [engine.md §12.2](engine.md#122-documentation-drift-dd) |

Why. Q-ABI-6: wait objects cannot be eventfd-based on Windows, and 64-bit flag enums must go.
Q-ABI-4: fixes then land once; the shim needs a binary-identical `st_frame` adapter, and session
callbacks and RTP and slice modes cannot be shimmed without a thread hop.

## 12. Migration, plan and outside parties (Q-MIG, Q-PLAN, Q-EXT)

| ID | Question | Options | Proposed default / answer | Status | Applied in |
|---|---|---|---|---|---|
| Q-MIG-1 (P) | Survey private and external users before freezing? | yes; no | yes, as the gate before the header freezes: a design review with at least three named consumers (FFmpeg and GStreamer plugin owners, the MXL team, the external engine team) plus a questionnaire (layer, flags, ext frames, callbacks or blocking, timing) | M10 | [migration.md §8.4](migration.md#84-pre-hide-gate), [implementation-plan.md §3.1](implementation-plan.md#31-maintainer-decisions) |
| Q-MIG-2 (L) | The canonical sample set | — | about six examples (TX and RX × simple, zero-copy, timed A/V) plus an event-loop example; each also runs on the null backend in CI (G-97); old samples to `legacy/` | Proposed default | [examples.md](examples.md), [migration.md §11.6](migration.md#116-other-in-tree-consumers) |
| Q-MIG-3 (P) | What happens to PR #1610? | (a) close as superseded, with credit; (b) keep as a reference until the new header lands; (c) rebase pieces | (b), then (a); salvage `lib/src/new_api/mt_session_event.c` for SF-15 in Phase 0.5, with credit | M9 (D-27) | [migration.md §14](migration.md#14-where-the-code-goes-and-pr-1610) |
| Q-PLAN-1 (B) | Phase 0.5 and the engines-first track first, and which staffing? | (a) full plan at 3 FTE (18–28 months); (b) at ≈ 1.5 FTE (3.1–4.7 years); (c) Phase 0.5, engines and Phases 1–2, then decide | start (c) now, choose (a) or (b) at the go/no-go after Phase 2; ≈ 55–85 EM plus ≈ 5–8 EM for the Kubernetes work; "stop after Phase 2" is a coherent product | M10 (D-66) | [implementation-plan.md §10](implementation-plan.md#10-effort) |
| Q-EXT-1 (L) | Can someone with a Rivermax developer account confirm what public sources leave unknown: the full `rmx_status` enum, the commit-time failure code, completion moderation (DOCA and the Dev Kit disagree), thread safety, which SDP attributes drive pacing? | — | an action, not a decision: wanted before Rivermax-like names freeze | Open, action | [migration.md §13](migration.md#13-if-you-know-libfabric-or-rivermax) |

Why. Q-MIG-1: about ten private repositories (media proxy and mesh, NMOS integration, a vendor
origin service, an LED-wall receiver, validation frameworks) and the team behind the
issues numbered 11xx–13xx are the real ABI users, and their usage was not inspected. Q-PLAN-1: C5 §1.1 measured
the revision-2 plan at ≈ 50–75 EM with nothing user-visible before Phase 1; revision 3 delivers the
A/V answer on the legacy API in Phase 0.5 (6–10 weeks after Q-TIME-0 is answered).

## 13. Revision 4 (Q-R4)

| ID | Question | Proposed default | Status | Applied in |
|---|---|---|---|---|
| Q-R4-1 (B) | Accept the revision-4 shape as one package: lean `mtl.h` (32 functions) plus optional headers, one config, option keys, `MTL_INIT`, one `struct mtl_unit`, slots by index, results on or off, start arrays, one queue type, a stats registry, `-MTL_EAGAIN` for every "nothing now", submit consuming the lease on failure, `mtl_session_close`, the errno contract | accept (a); options (b) keep one revision-3 rule, (c) keep revision 3 | M11 (D-71…D-81, D-84…D-86, D-88) | [decisions.md §2.12](decisions.md#212-m11--the-revision-4-shape) |
| Q-R4-2 (P) | Approve or reject each legacy capability proposed for removal | remove each row unless the M10 survey finds an external user | M12 (D-87) | [decisions.md §2.13](decisions.md#213-m12--legacy-capabilities-proposed-for-removal) |
| Q-R4-3 (P) | Accept four wire-visible defaults: video RTP from the frame epoch; incomplete RX frames delivered (`rx.incomplete` = `MTL_RX_DELIVER`, flagged `MTL_RX_INCOMPLETE`); `MTL_CVIDEO_CBR`; packet mode verbatim | accept the four | M13 (D-10, D-32, D-82) | [decisions.md §2.14](decisions.md#214-m13--wire-visible-defaults-of-the-unified-api) |
| Q-R4-4 (P) | RTP passthrough defaults (Q-PKT-1…9) and phase 2P | accept | M14 (D-82) | §14 below |
| Q-R4-5 (P) | Hiding the legacy headers: three tiers, stages 0, 1, F, F+1, ≥ F+2; `libmtl_unified.so.1` separate; plugin ABI v2 and `mtl_convert.h` before F | accept; else keep the legacy headers public and frozen | M15 (D-83) | §15 below, [migration.md §8](migration.md#8-hiding-the-legacy-headers) |
| Q-R4-6 (P) | Kubernetes pods and crash safety as one package | accept; else shutdown and R8 only | M16 (D-89…D-92) | §16 below, [deployment.md §4](deployment.md#4-kubernetes) |
| Q-R4-7 (P) | NMOS and IPMX: the IS-05 activation contract, `mtl_sdp.h`, `mtl_rtcp.h`, `mtl_crypto.h`, the profile, timing without PTP | accept as the design, implement in Phase 7; `mtl_crypto.h` after its cost spike | M17 (D-93…D-96) | §17 below, [nmos-ipmx.md](nmos-ipmx.md) |

## 14. RTP passthrough (Q-PKT, under M14)

Accepting M14 accepts each recommendation. The answers live in
[contract.md §13](contract.md#13-packet-units) and `mtl_packet.h`.

| ID | Question | Options | Proposed default | Applied in |
|---|---|---|---|---|
| Q-PKT-1 | Default RTP stamping of application packets | (a) verbatim; (b) timestamp derived (today's default) | (a): the library writes only the fields in `packet.set_fields` (0 = verbatim); today's behaviour maps to one bit | contract.md §13.4 |
| Q-PKT-2 | How RX chunks are formed | (a) at dequeue by count or timeout (moderation); (b) per unit by the library | (a): up to `packets_per_chunk`, at least `packet.rx_min_packets` (1), or what arrived within `packet.rx_max_wait_ns` (one ptime, or 1 ms); `MTL_PKT_RX_UNIT_ALIGNED` for assemblers | contract.md §13.7 |
| Q-PKT-3 | Scope of the generic essence | (a) RTP only; (b) also raw UDP (no sequence, no dedup) | (a) in 2P; raw UDP only on demand | contract.md §13.6 |
| Q-PKT-4 | Per-packet launch offsets (pcap inter-packet fidelity, test shaping) | (a) later, through reserved room in `struct mtl_pkt_tx`; (b) never | (a); software pacing only | contract.md §13.5 |
| Q-PKT-5 | Default RX path | (a) copy in the caller; (b) lend NIC buffers | (a): NIC buffers held only briefly by construction; `MTL_PKT_RX_LEND` is the bounded zero-copy opt-in | contract.md §13.7 |
| Q-PKT-6 | RX reordering window | (a) none, arrival order with a gap flag; (b) an optional window | (a) | contract.md §13.7 |
| Q-PKT-7 | ST 2022-6 validation | (a) generic RTP only, the application writes the HBRMT headers; (b) a 2022-6 helper | (a), plus a helper later if asked; the 2P-b exit needs a 2022-6 receiver or analyser | contract.md §13.6 |
| Q-PKT-8 | Test fault injection (out of order, truncated units) | (a) through public packet mode; (b) only `mtl_debug_inject` | (a) for tests; (b) stays for mutating library-built streams (`MTL_FAULT_TX_MUTATE`) | contract.md §13, `mtl_debug.h` |
| Q-PKT-9 | Phase | (a) 2P right after Phase 2; (b) Phase 6, as in revision 3 | (a): hiding the session headers depends on it | [implementation-plan.md §2.1](implementation-plan.md#21-scope-map-what-must-be-ported-and-when) |

Related open issue: a failed submit of a packet unit (`PKT_COUNT`) returns the slot to the pool
under D-88, while the packet-mode study kept the lease with the caller (OI-15). Spike SP-PKT
measures the per-packet tasklet cost of extbuf attach plus a header mbuf at 1080p59.94 and
2160p59.94 with two legs, the RX copy cost on the application thread, and a chunk-size sweep
(8–128) against pacing jitter.

## 15. Hiding the legacy headers (Q-HIDE, under M12 and M15)

| ID | Question | Proposed default | Status | Applied in |
|---|---|---|---|---|
| Q-HIDE-1 | Approve each cut individually: CUT-1 header split; CUT-2 `uframe_pg_callback`; CUT-3 `st20rc`; CUT-4 public per-pair converters; CUT-5 public user DMA (`mtl_udma_*`, `*_simd_dma`); CUT-5b public lcore borrowing; CUT-6 `ST22_*_FLAG_DISABLE_BOXES`; CUT-7 RTCP flags on ST30/40/41; CUT-8 thin wrappers (`st_draw_logo`, `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align`); CUT-? queue meta and `DATA_PATH_ONLY` | remove each unless the M10 survey finds an external user; re-check against the survey with a `readelf -V` audit once the Phase 0 symbol nodes exist | M12 (one row each in decisions.md §2.13) | [decisions.md §2.13](decisions.md#213-m12--legacy-capabilities-proposed-for-removal), [migration.md §8.4](migration.md#84-pre-hide-gate) |
| Q-HIDE-2 | Keep `libmtl_unified.so.1` a separate library at the freeze, so the legacy removal bumps only libmtl's soname? | yes | M15 (D-83) | [migration.md §7.2](migration.md#72-the-unified-library) |
| Q-HIDE-3 | Same removal date for `MTL_LEGACY_PIPELINE` as for `MTL_LEGACY_SESSION`, or set at the Phase 2 go/no-go? | set the pipeline headers' date at the go/no-go after Phase 2 | M15 (D-83) | [migration.md §8.3](migration.md#83-stages) |
| Q-HIDE-4 | User schedulers and tasklets (`mtl_sch_*`, H-08): an advanced opt-in surface, or cut for D-04's purity? | cut (one integration test, `sch_test.cpp`, uses them); if kept, an advanced header with the inline-safe DP subset | M12 | [decisions.md §2.13](decisions.md#213-m12--legacy-capabilities-proposed-for-removal), [engine.md §2](engine.md#2-the-pinned-core-rules) |

Why. Q-HIDE-1: no row is used by an in-tree consumer beyond tests, a sample or RxTxApp; the
private users (Q-MIG-1) may depend on session-only features no in-tree consumer shows (slice
mode, `DATA_PATH_ONLY`, user DMA). Q-HIDE-2: the legacy tier is bug-fix-only from F and moves to
the internal tier rather than being deleted; a separate DSO keeps unified applications' soname
stable across that move.

## 16. Kubernetes and crash safety (Q-K8S, Q-K3, under M16)

Q-K8S-1…11 are the questions of the Kubernetes design; accepting M16 accepts each
recommendation ([decisions.md §2.17](decisions.md#217-m16--kubernetes-pods-and-crash-safety)
lists them in one line each). The shutdown study's own questions Q-K3-1…8 came first; each is
answered by a Q-K8S row, and where the two differ the Q-K8S row (and the header) wins.

| ID | Question | Proposed default | Status | Applied in |
|---|---|---|---|---|
| Q-K8S-1 | `mtl_instance_close(mt, timeout_ns)` returning 0 retired, 1 quiesced, `-MTL_EIO` (`QUEUE_QUARANTINED`, exit the process), and `mtl_instance_shutdown` with flags and a report? | yes | Proposed default, under M16 (D-89) | [deployment.md §4.2](deployment.md#42-shutdown), [deployment.md §4.3](deployment.md#43-outcomes-and-the-report), [contract.md §2.4](contract.md#24-close-and-shutdown) |
| Q-K8S-2 | Leases still out at shutdown: free each slot at the next control-plane call after its last lease returns, never earlier? | yes | Proposed default, under M16 | [contract.md §2.5](contract.md#25-what-holds-after-each-kind-of-ending), [deployment.md §4.5](deployment.md#45-the-crash-contract) |
| Q-K8S-3 | CPU arbitration: the affinity mask in pods, MtlManager otherwise, OFD locks (`MTL_CPUARB_LOCKS`) without a manager; the SysV table and the manager-optional flag removed? | yes | Proposed default, under M16 (D-92); legacy fallout OI-4; auto with a mounted manager socket in a pod: OI-48 | [deployment.md §4.7](deployment.md#47-cpus) |
| Q-K8S-4 | Device removal: sessions enter ERROR and the application closes them; liveness fails only when a session has no leg left? | yes | Proposed default, under M16 | [deployment.md §4.6](deployment.md#46-device-removal-and-reset), [contract.md §2.6](contract.md#26-device-removal-and-reset) |
| Q-K8S-5 | DPDK's SIGBUS hotplug handler only with `instance.hotplug`? | yes (default off) | Proposed default, under M16 (D-90) | [deployment.md §4.4](deployment.md#44-signals-and-fork-r8) |
| Q-K8S-6 | Refuse no-IOMMU unless `instance.allow_noiommu`? | yes, with a startup warning naming the crash risk; changes behaviour for no-IOMMU users (the legacy API only warns) | Proposed default, under M16 (D-92) | [deployment.md §4.11](deployment.md#411-memory-and-the-iommu) |
| Q-K8S-7 | IGMP after SIGKILL: document the window (≈ 260 s) in v1, leaves sent by MtlManager later? | yes | Proposed default, under M16 | [deployment.md §4.5](deployment.md#45-the-crash-contract) |
| Q-K8S-8 | Detecting "a node daemon disciplines the PHC": the start-time agreement test plus the application's `locked` flag (`time.phc_trust`)? | yes, after a spike on the bound | Proposed default, under M16; `CLOCK_NOT_OWNED` scope: OI-7 | [deployment.md §4.8](deployment.md#48-time-in-a-pod) |
| Q-K8S-9 | Default of `instance.cpu_shared`: `MTL_CPU_SHARED_WARN` or `MTL_CPU_SHARED_REFUSE` in lcore mode? | WARN in v1; the health detail shows it | Proposed default, under M16 | [deployment.md §4.7](deployment.md#47-cpus) |
| Q-K8S-10 | Shutdown default "finish the unit on the wire, flush the rest" rather than drain? | yes: a pod is being replaced and its queued frames are not wanted; `MTL_SHUTDOWN_DRAIN` exists | Proposed default, under M16 | [deployment.md §4.2](deployment.md#42-shutdown) |
| Q-K8S-11 | Recommend set-up A (non-root, node prepared) in the documentation? | yes; set-up B as the fallback | Proposed default, under M16 | [deployment.md §4.14](deployment.md#414-files-and-privileges), [deployment.md §4.15](deployment.md#415-a-pod-spec-set-up-a) |

| ID | Question | Study's recommendation | Status now |
|---|---|---|---|
| Q-K3-1 | Adopt `mtl_instance_shutdown` with a 0 / 1 / timeout report? | yes; it is the safe closing the owner asked for; per-object closes stay | Superseded by Q-K8S-1: the third outcome is `-MTL_EIO` (a quarantined queue), not `-MTL_ETIMEDOUT` |
| Q-K3-2 | Forced retire with stale leases: keep library-pool memory mapped until exit (leaks) or free it (use-after-free risk)? | keep mapped; report the bytes leaked until exit | Superseded by Q-K8S-2: a slot is freed after its last lease returns, never earlier; handles stay safe because handle slots are never freed (R4); only a quarantined queue's memory is kept until exit |
| Q-K3-3 | Manager-less CPU arbitration: OFD lockfiles, affinity only, or MtlManager required in pods? | affinity only for Guaranteed pods; MtlManager otherwise; OFD files as the fallback; remove SysV | Answered by Q-K8S-3 |
| Q-K3-4 | Device removal: a session with no surviving leg goes ERROR (closeable) or is closed by the library? | ERROR; the application closes it (Vulkan, uverbs) | Answered by Q-K8S-4 |
| Q-K3-5 | Should the library ever install DPDK's SIGBUS hotplug handler? | only by option, default off | Answered by Q-K8S-5 |
| Q-K3-6 | Refuse no-IOMMU in pods unless opted in? | yes, `instance.allow_noiommu` | Answered by Q-K8S-6 (for every process, not only pods) |
| Q-K3-7 | IGMP after SIGKILL: accept the querier timeout, or MtlManager sends leaves for dead clients? | accept and document in v1; manager leaves later (SF-K3-7) | Answered by Q-K8S-7 |
| Q-K3-8 | Liveness: stats keys only, or also a health getter? | both; the getter is one call a probe can make | Answered by D-91: `mtl_instance_get_health` (liveness, readiness, phase) plus the stats keys |

Why. Q-K8S-1: a pod gets SIGTERM and a grace period, then SIGKILL; the network goes first so
peers see a clean end, MtlManager grants are returned last. Q-K8S-2: freeing early turns
application bugs into use-after-free. Q-K8S-3: the SysV table arbitrates nothing across PID
namespaces and finds owners by PID (`kill(pid, 0)`). Q-K8S-6: in no-IOMMU mode nothing is pinned,
so after a crash DMA may hit freed pages. Q-K8S-7: the switch forwards for the group membership
interval, 2 × 125 s + 10 s = 260 s at RFC 3376 defaults.

## 17. NMOS and IPMX (Q-NI, Q-NMOS, Q-I, under M17; Phase 7, later)

Q-NI-1…10 are the questions of the NMOS and IPMX design; accepting M17 accepts each
recommendation. Under D-98 everything except the atomic update of today's destination and
source change (Phase 2) is Phase 7, declared under `MTL_LATER`.

| ID | Question | Proposed default | Status | Applied in |
|---|---|---|---|---|
| Q-NI-1 | Ship `mtl_sdp.h` (render and parse), replacing Q-MODE-6's "values only"? | yes (Phase 7) | Proposed default, under M17 (D-94) | [nmos-ipmx.md §5](nmos-ipmx.md#5-sdp-helper-mtl_sdph-mtl_later) |
| Q-NI-2 | Ship `mtl_rtcp.h`? IPMX cannot be met without sender reports | yes, Phase 7 with timing without PTP (the design's "phase 3" predates D-98) | Proposed default, under M17 (D-95) | [nmos-ipmx.md §10](nmos-ipmx.md#10-rtcp-sender-reports-mtl_rtcph-mtl_later) |
| Q-NI-3 | `mtl_crypto.h` now, or after a cost spike? | the header as a proposal; implementation after the spike (cycles per byte with ipsec-mb or OpenSSL on the target CPUs) | Proposed default, under M17 (D-96) | [nmos-ipmx.md §13](nmos-ipmx.md#13-pep-encryption-and-hdcp-mtl_cryptoh-mtl_later) |
| Q-NI-4 | Rename the NACK options `rtcp.*` to `rtx.*` (`MTL_OPT_RTX`, `rtx.enable`)? | yes; the legacy names stay in the bridge | Proposed default, under M17 (D-95) | [migration.md §4.1](migration.md#41-fields-and-flags-every-session-ops-shares) |
| Q-NI-5 | Mute (every leg disabled) instead of a scheduled stop? | yes (Q-NMOS-1) | Proposed default, under M17 (D-93) | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |
| Q-NI-6 | Both legs on one port? | yes, with an explicit `flows[1].port` (Q-NMOS-3) | Proposed default, under M17 | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |
| Q-NI-7 | Audio updates at a packet boundary? | yes (Q-NMOS-4) | Proposed default, under M17 | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |
| Q-NI-8 | `session.profile` (`MTL_OPT_PROFILE`) as the one IPMX switch; profiles change zero defaults and labels only? | yes | Proposed default, under M17 (D-95) | [nmos-ipmx.md §9](nmos-ipmx.md#9-the-ipmx-profile) |
| Q-NI-9 | No separate IPMX sender type: under the profile, N uses TR-10-1's CMAX and VRX? | yes, unless the VSF requires a distinct TP value | Proposed default, under M17 | [nmos-ipmx.md §9](nmos-ipmx.md#9-the-ipmx-profile) |
| Q-NI-10 | RX `MTL_UPDATE_REAPPLY` without a leave (reports re-sent) instead of a leave and join? | yes; leaving and joining both legs at once is a guaranteed hit | Proposed default, under M17 (D-93) | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |

The NMOS study's questions (Q-NMOS) and the IPMX study's (Q-I) are adopted as the studies
recommended, except where a row says otherwise.

| ID | Question | Recommendation, as adopted | Status | Applied in |
|---|---|---|---|---|
| Q-NMOS-1 | Mute (G-N3) or a scheduled `mtl_session_stop` at a time? | mute: it composes with `MTL_UPDATE_FLOWS` in one atomic update and keeps the session's resources, so re-enable is a boundary command; stop stays for format changes. Muted units count in `tx.units_muted`, not `tx.units_dropped` (which BCP-008 reads as unhealthy) | Answered by Q-NI-5; the result status of a muted unit: OI-56 | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl), [nmos-ipmx.md §14.1](nmos-ipmx.md#141-nmos-gaps-n1) |
| Q-NMOS-2 | Is TX `planned_tai_ns` the media time of the first switched unit, or its first packet's launch (media time + TROFFSET)? | the media time (the unit boundary): IS-05 compares devices by it, and launch differs per sender type | Proposed default, under M17 (D-93) | [nmos-ipmx.md §4.1](nmos-ipmx.md#41-one-patch-is-one-update) |
| Q-NMOS-3 | Both legs on one port (`interface_bindings` listing one interface twice; RFC 7104 separate addresses on one NIC)? | allow with an explicit `flows[1].port` = 1 (port index 0 + 1); ST 2110-10 §8.5 still forbids identical source and destination | Answered by Q-NI-6 | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |
| Q-NMOS-4 | Audio updates at a unit (default 10 ms) or a packet (1 ms) boundary? | packet boundary, so a salvo of video and audio to one t lands within one packet time | Answered by Q-NI-7 | [nmos-ipmx.md §4.2](nmos-ipmx.md#42-is-05-to-mtl) |
| Q-NMOS-5 | Colorimetry, TCS and range as typed fields (G-N10) or options? | typed (`video.colorimetry`, `tcs`, `range`, also in `mtl_cvideo_config`): SDP-mandatory, IS-04-required, they select conversion matrices; three bytes of reserved space; 0 = UNSPECIFIED, SDR, narrow | Proposed default (applied in `mtl.h`) | [nmos-ipmx.md §3](nmos-ipmx.md#3-is-04-values-only-the-transport-knows) |
| Q-NMOS-6 | ST 2110-41 has no NMOS media type or flow schema: publish fast metadata as `urn:x-nmos:format:data` with an unregistered `media_type`? | yes, as a vendor value until AMWA registers one; flag it to AMWA (still unregistered, checked 2026-09-25) | Open, action (outside MTL) | [nmos-ipmx.md §15.1](nmos-ipmx.md#151-nmos-n-req-160), [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |
| Q-NMOS-7 | `mtl_sdp.h` render and parse, or values only (Q-MODE-6)? | render and parse, an optional helper over public calls; NMOS and IPMX Nodes both need it | Answered by Q-NI-1 (D-94) | [nmos-ipmx.md §5](nmos-ipmx.md#5-sdp-helper-mtl_sdph-mtl_later) |
| Q-NMOS-8 | LLDP on DPDK ports (G-N14): an MTL feature or the Node's (raw packets through packet mode)? | MAY; deferred: `chassis_id = null` is allowed and topology tools mostly read the switch | Proposed default (later) | [nmos-ipmx.md §14.1](nmos-ipmx.md#141-nmos-gaps-n1) |
| Q-NMOS-9 | Should MTL count BCP-008 status transitions itself? | no: hysteresis, reporting delay and messages are Node policy; MTL gives counters and events | Proposed default | [nmos-ipmx.md §7](nmos-ipmx.md#7-bcp-008-monitoring) |
| Q-NMOS-10 | The A/B swap closes the old session on a Node timer; add a timed stop (`MTL_AT_TAI`) for RX? | only if G-N6 (an RX format change at an instant) is rejected; with G-N6 the swap disappears. G-N6 is later, so the A/B swap stays, and `mtl_session_stop` has no `when` | Proposed default (later, with G-N6) | [nmos-ipmx.md §4.5](nmos-ipmx.md#45-a-node-on-phase-2-only), [nmos-ipmx.md §14.1](nmos-ipmx.md#141-nmos-gaps-n1) |
| Q-NMOS-11 | NMOS treats RTCP (RFC 3550) as optional, IPMX needs sender reports: one RTCP design for both? | decided in the IPMX design: `mtl_rtcp.h` (D-95); today's NACK retransmission kept separate in naming (`rtx.*`, Q-NI-4) | Answered by Q-NI-2, Q-NI-4 | [nmos-ipmx.md §10](nmos-ipmx.md#10-rtcp-sender-reports-mtl_rtcph-mtl_later) |
| Q-I-1 | Library-built Media Info Blocks (needing colorimetry, TCS, range, PAR in the config) or application bytes? | changed after review RN: the library writes the essence's block from the config and `struct mtl_rtcp_info` (PAR, measured rate, channel order) and appends the application's blocks unread (`MTL_RTCP_MIB_APP_ONLY` sends only those); no builder function. The study had preferred application bytes with builders | Proposed default, under M17 (D-95) | [nmos-ipmx.md §10](nmos-ipmx.md#10-rtcp-sender-reports-mtl_rtcph-mtl_later) |
| Q-I-2 | Do TR-10-10 InfoFrame and TR-10-6 FEC streams need their own sender reports? (TR-10-1 §8.7 says "IPMX Senders"; TR-10-10 is silent) | ask the VSF; default: no report for FEC (FEC is not adopted), a report without a block for InfoFrames under the profile | Open, action | [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |
| Q-I-3 | `MTL_MEDIA_SENDER` after a source dropout: keep counting RTP or re-anchor RTP0? | `MTL_SUBMIT_DISCONTINUITY` re-anchors RTP0 from the next M; otherwise k advances by max(1, round((M − M_prev) / period)), so a missed VSYNC skips one period without drift (fixes RN-24) | Proposed default, under M17 (D-95) | [nmos-ipmx.md §12](nmos-ipmx.md#12-timing-without-ptp-and-sender-mode) |
| Q-I-4 | May MTL hold HDCP keys at all? | no (DCP licensing and robustness rules); a vendor cipher plugin or packet units (GI-11), later | Proposed default, under M17 (D-96) | [nmos-ipmx.md §13](nmos-ipmx.md#13-pep-encryption-and-hdcp-mtl_cryptoh-mtl_later) |
| Q-I-5 | A per-unit block (HDR) and user meta on one frame? | the meta area holds records back to back, at most one per (kind, tag) (C-I10); `MTL_META_RTCP_MIB` appends to that unit's report only and never changes the block version | Proposed default, under M17 (D-86, D-95) | [nmos-ipmx.md §10](nmos-ipmx.md#10-rtcp-sender-reports-mtl_rtcph-mtl_later), [contract.md §9.9](contract.md#99-the-meta-area) |
| Q-I-6 | Link offset without a shared clock (presentation = sender time + link offset needs a local mapping) | map sender time to local time by the minimum observed (arrival − sender time) and report it; sample `MTL_LINK_OFFSET_AUTO` at activation and hold it | Proposed default, under M17 (D-95) | [nmos-ipmx.md §12](nmos-ipmx.md#12-timing-without-ptp-and-sender-mode), [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |
| Q-I-7 | Add BMCA to the built-in PTP client, or require ptp4l for IPMX (TR-10-1 §7.2 MUST)? | require PHC plus ptp4l (`MTL_TIME_SOURCE_PHC`); BMCA later; `MTL_TIME_SOURCE_PTP_BUILTIN` is labelled non-compliant under the profile (it takes the first Announce, `mt_ptp.c:1032`) | Proposed default | [nmos-ipmx.md §12](nmos-ipmx.md#12-timing-without-ptp-and-sender-mode) |
| Q-I-8 | RTCP receive path: a flow rule per session, or the system queue? | the system (CNI) queue by default; a session queue when NIC arrival times of reports are needed (the flow-rule budget would double per RX session) | Proposed default | [nmos-ipmx.md §10](nmos-ipmx.md#10-rtcp-sender-reports-mtl_rtcph-mtl_later) |
| Q-I-9 | In-band control on a DPDK port: is `port.virtio_user` enough for TCP, mDNS and DHCP (TR-10-9 §18–19)? | verify with an IPMX controller; mDNS needs 224.0.0.251 forwarded to the kernel; in a pod the primary network carries them | Open, action | [nmos-ipmx.md §8](nmos-ipmx.md#8-nmos-in-a-pod), [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |
| Q-I-10 | Key derivation helpers for PEP (CMAC, HMAC, ECDH need a crypto dependency)? | no: the application uses OpenSSL; MTL takes derived keys through `mtl_crypto_set_key` only | Proposed default, under M17 (D-96) | [nmos-ipmx.md §13](nmos-ipmx.md#13-pep-encryption-and-hdcp-mtl_cryptoh-mtl_later) |
| Q-I-11 | IS-05 `rtcp_destination_port` other than media port + 1 (IPMX fixes + 1)? | allow `rtcp.dst_port`, default + 1, warn under the IPMX profile; it changes only in STOPPED | Proposed default | [nmos-ipmx.md §4.4](nmos-ipmx.md#44-details-that-make-a-salvo-land-together) |
| Q-I-12 | Does a partial slice consume a PEP counter (TR-10-13; frame-wide vs per-packet equivalence)? | ask the TR-10-13 authors; the library encrypts per packet region anyway | Open, action | [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |

## 18. Where each question came from

The research references of each question, so a reader of [research.md](research.md),
[prior-art.md](prior-art.md) and [history.md](history.md) can find the evidence. Codes: `Rnn` is
research note nn (00 PR #1610 design review, 01 PR #1610 analysis, 02 current API survey, 03
scheduler and threading, 04 memory and buffers, 05 timing and pacing, 06 observability, 07 modes
matrix, 08 consumers and ecosystem, 09 libfabric, 10 Rivermax, 11 media-IO prior art, 12 ST 2110
timing standards, 13 lifecycle, errors and ABI); `Qn` is that note's question n, `§x` its
section, and `B`, `D`, `F`, `H`, `R-L` its own finding labels. C1–C5 are the reviews (`#n` a
finding, `Px-y` a persona finding, `C-nn` a consistency finding), S1–S9 the simplification
studies, K1–K3 the Kubernetes studies, N1 and I1 the interop studies, RA, RK and RN the
revision-4 reviews.

| ID | Raised by |
|---|---|
| Q-CORE-1 | R00, R01 §2, R03 §7.1, R09 Q1–Q2, R11 §10 |
| Q-ARCH-1 | R01 Q1, §6; R00 §15 phase 1 |
| Q-ARCH-1a | C1 #1, #2, #12, #20 |
| Q-ARCH-2 | R13 §1.1 |
| Q-THR-1 | R03 Q1, H1–H2; R08 Q1 |
| Q-THR-2 | R03 Q2, §7.3; R08 Q2 |
| Q-THR-2a | C1 #4 |
| Q-THR-3 | R03 Q3; R09 Q9 |
| Q-THR-4 | R01 Q12; R07 Q7 |
| Q-THR-5 | R03 Q11, §8; R13 §1.3 |
| Q-THR-6 | R03 Q7, §4.4; R13 §1.5 |
| Q-THR-7 | R03 Q9, H7 |
| Q-THR-7a | C1 #7, C2 #10 |
| Q-THR-8 | R03 Q4, §5; R09 Q10 |
| Q-THR-9 | R03 Q6; C1 #11 |
| Q-THR-10 | R03 Q10 |
| Q-LIFE-1 | R13 Q3 |
| Q-LIFE-2 | R08 Q3, B5; R13 §1.1 |
| Q-LIFE-3 | R13 Q9 |
| Q-LIFE-4 | C3 P2-4 |
| Q-LIFE-5 | R11 §1.1, §10 #9 |
| Q-LIFE-6 | R13 R-L8; R01 §5 #5 |
| Q-LIFE-7 | R13 §2; R06 Q6 |
| Q-LIFE-8 | R07 Q16 |
| Q-LIFE-9 | R13 R-L1 |
| Q-LIFE-10 | R06 Q6, R13 Q8 |
| Q-CMP-1 | R01 Q10; R11 Q4 |
| Q-CMP-3 | R13 R-L11; R06 Q13 |
| Q-CMP-4 | R09 Q11 |
| Q-CMP-5 | R03 §7.2, Q5; R09 §5 |
| Q-CMP-6 | R11 Q1, R06 Q3, R05 Q9, C1 #18, C4 C-05 |
| Q-CMP-7 | C1 #2 |
| Q-MEM-1 | R00 §9; R04 Q1; R11 Q3 |
| Q-MEM-2 | R04 Q4 |
| Q-MEM-3 | R04 Q6 |
| Q-MEM-4 | R11 §4.5, §10 #18 |
| Q-MEM-5 | R02 §4.7 |
| Q-MEM-6 | R04 Q12 |
| Q-MEM-7 | R04 Q2 |
| Q-MEM-8 | R04 Q11 |
| Q-MEM-9 | R04 Q10; R07 Q8 |
| Q-MEM-10 | R00 §8.3 |
| Q-MEM-11 | C3 P5-3, C4 §4.1 #7 |
| Q-MEM-12 | R04 Q5 |
| Q-TIME-0 | R12 §10.2; R05 Q3; R08 Q6 (#1211 asks for "pass both"); R01 Q7 |
| Q-TIME-24 | C2 #1 |
| Q-TIME-1 | R05 Q8 |
| Q-TIME-2 | R05 Q8, F2 |
| Q-TIME-3 | R05 Q13 |
| Q-TIME-4 | R11 Q2; R10 §2.6 |
| Q-TIME-5 | R12 Q5; R08 §3.3 |
| Q-TIME-6 | R05 Q7, §8.2 |
| Q-TIME-7 | R05 Q10; R07 Q3 |
| Q-TIME-9 | R01 Q11 |
| Q-TIME-10 | R11 Q6; R05 Q11 |
| Q-TIME-11 | R12 Q11 |
| Q-TIME-12 | R12 Q13; R05 Q14 |
| Q-TIME-13 | R00 §12 |
| Q-TIME-14 | R12 Q14 |
| Q-TIME-15 | R05 Q2; R12 Q8 |
| Q-TIME-16 | R05 Q1; R12 Q1 |
| Q-TIME-17 | R12 Q7 |
| Q-TIME-18 | C2 #1 |
| Q-TIME-19 | C2 #3 |
| Q-TIME-20 | C2 #27 |
| Q-TIME-21 | R05 Q6, C2 #13, C4 C-44 |
| Q-TIME-22 | R05 Q12 |
| Q-TIME-23 | R05 Q5, R10 Q4, R11 Q2, C2 #13 |
| Q-TIME-25 | C2 #5 |
| Q-TIME-26 | R12 §6; C5 §5.12 |
| Q-TIME-27 | C5 §5 note |
| Q-OBS-1 | R06 Q4; R11 Q8; R03 Q8 |
| Q-OBS-2 | R10 Q13 |
| Q-OBS-3 | R06 Q11 |
| Q-MODE-1 | R07 Q1; R10 Q2 |
| Q-MODE-2 | R07 Q11 |
| Q-MODE-3 | R01 Q13, D5 |
| Q-MODE-4 | R08 Q8 |
| Q-MODE-5 | R07 Q9 |
| Q-MODE-6 | R10 Q1; R12 Q12 |
| Q-MODE-7 | R07 Q14; R03 Q12 |
| Q-MODE-8 | R07 Q10 |
| Q-ABI-1 | R13 Q1; R02 Q1 |
| Q-ABI-2 | R09 Q8; R13 §4 |
| Q-ABI-3 | R11 Q9 |
| Q-ABI-4 | R08 §5.2; R13 Q14 |
| Q-ABI-6 | R13 Q12 |
| Q-ABI-7 | R13 Q11 |
| Q-MIG-1 | R08 Q10 |
| Q-MIG-2 | R08 Q13 |
| Q-MIG-3 | R01 Q14, §6 |
| Q-PLAN-1 | C5 §1.1, §1.5 |
| Q-R4-1 | S2, S3, S4, S5, S6, S7 |
| Q-R4-2 | R4 coverage check |
| Q-R4-3 | S1 §6 (U-155, U-202, U-233), S8 §4.5 |
| Q-R4-5 | S9 |
| Q-R4-6 | K1, K2, K3; reviews RK, RA and the response |
| Q-R4-7 | N1, I1; reviews RN, RA and the response |
| Q-EXT-1 | R10 Q14 |
| Q-CMP-1a | C3 P1-1 |
| Q-CMP-2 | C3 P2-5, B14 |
| Q-LIFE-6a | C3 P2-1 |
| Q-TIME-8 | design of the group object (revision 2); R05 Q4 |
| Q-ABI-5 | R00 §13 (API evolution and common verbs) |
| Q-R4-4 | S8 §9 |

The families outside the old register: Q-PKT-1…9 from S8 §9; Q-HIDE-1…4 from S9 §9; Q-K8S-1…11
from the Kubernetes design (K1, K2, K3, review RK); Q-K3-1…8 from K3 §6; Q-NI-1…10 from the NMOS
and IPMX design (N1, I1, reviews RN, RA); Q-NMOS-1…11 from N1 §9; Q-I-1…12 from I1 §9.

The research notes' own questions that were answered by the design without becoming a Q-ID
(the rest map to the rows above). Answers in header terms:

| Note Q | Question | Answer |
|---|---|---|
| R01 Q1 | backend to wrap | the pipelines through the slot interface (Q-ARCH-1, M2) |
| R01 Q2 | fate of frame, slice, RTP sessions | rows units and packet units in the unified API; the legacy APIs stay (Q-MODE-1, D-82) |
| R01 Q3 | does create start; what stop does | CREATED sends nothing; `mtl_session_start`; stop DRAIN or FLUSH (Q-LIFE-1) |
| R01 Q4 | completions after stop | results stay drainable until close ([contract.md](contract.md) §4) |
| R01 Q5 | handle validation strength | generation-tagged handles, never reissued (R4, Q-THR-5) |
| R01 Q6 | buffer struct or opaque handle | slot index plus `struct mtl_unit` with `struct_size` (M11) |
| R01 Q7 | one or two timestamps | media time and launch time, TAI (Q-TIME-0) |
| R01 Q8 | A/V alignment in v1 | yes: timelines and start arrays (D-78) |
| R01 Q9 | where imported memory lives | instance regions mapped into every port, refcounted (`mtl_mem.h` MEM1) |
| R01 Q10 | event policy | results that cannot be lost, coalescing events (Q-CMP-1) |
| R01 Q11 | FIFO or timestamp order | submission order (Q-TIME-9, G-08) |
| R01 Q12 | where conversion runs | never on a tasklet except `rx.convert_per_packet` (OI-28); path reported (Q-THR-4) |
| R01 Q13 | ST22 as a flag | its own essence `MTL_CVIDEO` (Q-MODE-3) |
| R01 Q14 | the PR itself | a reference, then closed with credit (Q-MIG-3, M9) |
| R01 Q15 | the "stopped" code | `-MTL_ESHUTDOWN`; `-MTL_EAGAIN` only for "nothing now" (D-88) |
| R02 Q1 | ABI policy | soname and version script for the new library, `struct_size` inputs (Q-ABI-1, R3) |
| R02 Q2 | four frame-exchange models | one: acquire/submit/reap and dequeue/release for every essence; callbacks only on dispatch threads (`mtl_queue_dispatch_start`) |
| R02 Q3 | which thread may call what | call classes CP/DP/DPC/WT/AS; no application code on a tasklet (R6, M5); the tasklet mutex goes (SF-15) |
| R02 Q4 | what TX done reports | `struct mtl_tx_result`: status, reason, media time, `margin_ns`, `sent_tai_ns` (`MTL_TXR_SENT_HW`) (Q-CMP-6) |
| R02 Q5 | one timestamp representation | `int64_t` TAI ns valid by flag plus a separate `rtp`; no `tfmt`; `epoch` replaced by the media index (R5) |
| R02 Q6 | MEDIA_CLK for user pacing | launch times are TAI only (`launch_tai_ns`); an RTP value goes with `MTL_SUBMIT_RTP_TS` |
| R02 Q7 | flags or typed fields | typed fields plus options (`mtl_options.h`); unknown bits `-MTL_EINVAL` (`UNKNOWN_BITS`); `mtl_session_query` reports what is granted |
| R02 Q8 | external memory scope | regions for every essence (`mtl_mem.h`); results always for application memory |
| R02 Q9 | per-session start/stop | yes (CREATED, start, stop); `mtl_session_update` applies at a boundary |
| R02 Q10 | latency knobs | `pool_count` up to `max_count` (Q-MEM-5), `min_tx_delay_ns`, `media_time_offset_ns`, `mtl_tx_next_slot`, `mtl_tx_row_deadline`, `info.min_submit_lead_ns` |
| R02 Q11 | conversion and plugins | in the session by `app_format`; optional `mtl_convert.h` and `mtl_plugin.h` |
| R02 Q12 | error reporting | `int` + out handle, `mtl_last_error()` with reason and detail; distinct `-MTL_EAGAIN`, `-MTL_ESHUTDOWN`, `-MTL_ECANCELED` |
| R02 Q13 | health events | `MTL_EVENT_*` (LEG_STATE, FLOW_STATE, TIME_STATE, PORT_RESET, RX_FORMAT) for every essence; `mtl_instance_get_health()` |
| R02 Q14 | debug knobs in production structs | `mtl_debug.h`, tests only |
| R03 Q5 | event-queue overflow policy (size the CQ ≥ outstanding frames and coalesce; drop with a counter and an overflow event; back-pressure) | results are materialised by the reader from the lease table and cannot overflow; events coalesce into per-source pending state, `MTL_EVENT_OVERFLOW` ([engine.md](engine.md) §5, §7.5) |
| R03 Q8 | stats guarantee: seqlock snapshot, per-field relaxed atomics, or periodic events | per-writer counter blocks summed by the reader, seqlocks only for grouped single-writer values ([engine.md](engine.md) §2.8), cumulative without reset (Q-OBS-1, D-80) |
| R04 Q3 | which devices a region maps into: every port and DMA device, or one IOMMU domain | per-device `rte_dev_dma_map` at import ([engine.md](engine.md) §5.8, MF2; G-16) |
| R04 Q7 | unmap or destroy while in flight; force-complete with ABORTED first | `mtl_mem_close` returns 1 while referenced, `MTL_EVENT_REGION_RELEASED` at the end (D-16, Q-MEM-4); close completes leases as FLUSHED |
| R04 Q8 | a terminal result for every provided RX buffer, dropped ones included | MF4, [contract.md](contract.md) §6.5 |
| R04 Q9 | can the extbuf count hit 0 mid-frame (+1 builder reference) | MF7 ([engine.md](engine.md) §11) |
| R04 Q13 | imported memory for ST30/40/41 in v1 | copy only by construction, the same API ([research.md](research.md) §6.2) |
| R05 Q4 | multi-essence sync: a library sync group, or helpers | timelines with a shared T0 and start arrays; groups gone (D-78; [timing.md](timing.md) §3, §7.1) |
| R05 Q14 | RX presentation contract: unwrapped TAI media time, per-path first and last arrival, flush deadline | `media_tai_ns`, `arrival_first_tai_ns[leg]`, the due time ([timing.md](timing.md) §11.2, §11.7, §11.8; Q-TIME-12) |
| R08 Q2 | a pollable wait object | Q-THR-2; `mtl_session_get_wait_handle` (D-43) |
| R08 Q4 | buffer lending, out-of-order release, an all-held policy | D-01, D-76; `mtl_rx_release` in any order, `MTL_SESSION_RX_LATEST` |
| R08 Q5 | import or export pool | both: `mtl_mem_import` + `mtl_session_attach`, `MTL_SESSION_EXPORT_POOL` (D-76, [migration.md](migration.md) §12.4, §12.5) |
| R08 Q7 | error, state and completion semantics | D-21, D-81, D-88; `enum mtl_state`; results (D-03) |
| R08 Q9 | fate of the session and RTP/slice APIs | M15 (D-83); `MTL_UNIT_ROWS`, `MTL_UNIT_PACKETS`; inline callbacks `MTL_LATER` (M5, U-176) |
| R08 Q11 | configuration ABI style | D-74, D-97 (typed, `struct_size`, no key-value strings) |
| R08 Q12 | audio granularity | D-41 (`mtl_tx_write`) |

## 19. How the register grew, and renamed IDs

- The first register: 176 questions raised by the 13 research notes, de-duplicated and merged;
  reviews C1–C4 added their own.
- Revision 2 added Q-ARCH-1a, Q-THR-2a, Q-THR-7a, Q-THR-9, Q-THR-10, Q-LIFE-10, Q-CMP-6,
  Q-CMP-7, Q-MEM-11, Q-MEM-12, Q-TIME-18…25, Q-MODE-8, and extended Q-MODE-4 (the ≈ 105
  conversion functions) and Q-ABI-3 (option (c), backend-extension tables).
- Revision 3 answered review C5 and added Q-TIME-26, Q-TIME-27 and Q-PLAN-1; every question
  without a maintainer decision got a proposed default.
- Revision 4 added Q-R4-1…7 (M11–M17) and the study families Q-PKT, Q-HIDE, Q-K3, Q-K8S, Q-NI,
  Q-NMOS, Q-I; the maintainer answered Q-MODE-1 on 2026-10-01.

Reviews C1 and C4 proposed questions under IDs that were renumbered when they entered the
register. A reader of those reviews maps them so:

| Proposed as | Topic | Now |
|---|---|---|
| C1's Q-CMP-6 | idle descriptor recycling | Q-CMP-7 |
| C4's Q-TIME-18 | default late policy; reject or drop | Q-TIME-23 |
| C4's Q-TIME-19 | legacy USER_PACING mapping (fixes C-44) | Q-TIME-21 |
| C4's Q-TIME-20 | CTM/LLTM ANC scheduling | Q-TIME-22 |
| C4's Q-MEM-11 | IOVA of imported non-hugepage memory | Q-MEM-12 |
| C3's Q-CMP-1a, Q-LIFE-6a | result starvation code; sticky cancel | kept, answered in §6 and §5 |

C4 also found seven B questions without a decision row (Q-ARCH-2, Q-THR-8, Q-LIFE-6, Q-TIME-1,
Q-MODE-3, Q-ABI-3, Q-ABI-5); they became D-28…D-34 (D-28 and D-29 since superseded by D-44 and
D-50).

## 20. Where the archive and the maintained set disagree

decisions.md and the headers are newer and win; the rows above follow them. The differences, so
nobody re-applies an archive answer:

| ID | Archive answer | Current answer |
|---|---|---|
| Q-LIFE-2 | a refcounted default instance with a merge table for later opens | a later open with differing settings is `-MTL_EEXIST` (`INSTANCE_MISMATCH`); no merge table (OI-1) |
| Q-LIFE-3 | (c), a versioned uninit with a destroy-all flag | dependency-order teardown on the last `mtl_instance_close`; no destroy-all flag in any header |
| Q-LIFE-4 | deferred close plus a force flag | deferred close only; `mtl_session_close(s, timeout_ns)` returns 1 (D-88) |
| Q-LIFE-10 | gap units `MTL_TX_DROPPED` / `RECOVERY` | contract.md has `MTL_TX_FAILED` / `PORT_RESET`; open (OI-13) |
| Q-MEM-2 | PA mode direct for hugepage imports | OI-22 recommends copy only for every import in PA mode; open |
| Q-MEM-4 | `-MTL_EBUSY` in v1, deferred release later | `mtl_mem_close` returns 1 and `MTL_EVENT_REGION_RELEASED` now (D-16) |
| Q-MEM-12 | the invented IOVA range unless spike S4 clears IOVA = VA | OI-41 recommends IOVA = VA; D-16 calls it answered, engine.md open |
| Q-OBS-3 | the parser on NIC-timestamped packets only | port first keeps today's software-time parsing; open (OI-29) |
| Q-THR-6 | (a), unified sessions never migrate | (b), migration ported with a handshake (D-98) |
| Q-K3-1 | third shutdown outcome `-MTL_ETIMEDOUT` | `-MTL_EIO` (Q-K8S-1) |
| Q-K3-2 | keep stale-lease memory mapped until exit | free each slot after its last lease returns (Q-K8S-2) |
| Q-I-1 | application-supplied Media Info Blocks with builders | the library writes the essence block, appends the application's; no builder (review RN) |
| Q-NI-2 | `mtl_rtcp.h` in phase 3 | Phase 7 (D-98) |
| Q-MODE-6, Q-NMOS-7 | SDP values only / helper in v1 | `mtl_sdp.h` in Phase 7, `MTL_LATER` (D-94) |
| Q-MEM-10 | a dynamic pool with its own acquire call in Phase 4 | `mtl_tx_acquire_layout` (`MTL_LATER`); OI-20 |
