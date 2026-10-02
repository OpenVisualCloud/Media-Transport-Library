# Decisions

| | |
|---|---|
| Status | Maintained. Seventeen maintainer decisions (M1–M17) are open; nothing is **Accepted** yet. Every other decision is a **Proposed default** that stands unless the maintainer objects |
| Date | 2026-10-02 |
| Sources | [archive/DECISIONS.md](archive/DECISIONS.md), [archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md), [archive/REVISION-4.md §3, §5, §8, §9](archive/REVISION-4.md), [archive/14-implementation-roadmap.md §3.7](archive/14-implementation-roadmap.md) |

This file replaces `DECISIONS.md` and `OPEN-QUESTIONS.md` for daily use. It holds every
decision of the design (D-01…D-98) in one line each, the open maintainer decisions with an
answer line, the proposed defaults that matter for the ST 2110-20 port, and the open issues
for the implementation (OI-1…OI-47). The full
questions, with options, research references and review history, stay in
[archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md).

## 1. How to use this file

**Statuses.**

| Status | Meaning | What the maintainer does |
|---|---|---|
| **Awaiting M#** | the decision depends on maintainer decision M# (§2) | answer M# in its **Your answer** line |
| **Proposed default** | the designer answered it and applied the answer in the headers and documents | nothing; to object, name the ID in §6 |
| **Proposed default (maintainer's direction)** | the designer wrote down a direction the maintainer gave (D-97, D-98) | confirm by not objecting |
| **Accepted** | the maintainer confirmed it | none so far |
| **Superseded by D-nn** | replaced; listed once in §3.3 | — |

**Rules for keeping this file.**

- IDs are stable. A new decision takes the next free number (D-99, then up); a new
  maintainer decision takes M18; a new open issue the next OI number (§5). Nothing is
  renumbered.
- When the maintainer answers M#, write the answer in its line, then change the status of
  every D-xx listed under it to **Accepted** (or **Rejected**, with the replacement).
- A change to a decision changes the header in `sketch/` and the maintained document named
  in the **Where** column in the same commit.
- "M1"…"M17" in this file always mean maintainer decisions. The milestones of
  [implementation-plan.md](implementation-plan.md) are always written with the word
  "milestone" and their name: milestone M0 (skeleton), milestone M1 (ST20 TX), milestone M2
  (ST20 RX), milestone M3 (results, events, stats), milestone M4 (RxTxApp), milestone M5
  (gtests), milestone M6 (nightly).

**Question IDs.** `Q-xxx-n` are the question IDs of
[archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md); each M# names the questions it
decides. `Q-K8S-1…11` are in [archive/16 §13](archive/16-kubernetes-and-crash-safety.md),
`Q-NI-1…10` in [archive/17 §7](archive/17-nmos-and-ipmx.md), `Q-PKT-1…9` in
[archive/simplification/S8 §9](archive/simplification/S8-rtp-passthrough.md).

## 2. Open maintainer decisions

### 2.1 What each decision blocks

**B** = the milestone cannot start (or cannot be merged) without the answer. **S** = the
answer changes the milestone's scope or its expected results, but work can start on the
recommendation. Empty = no effect on milestones M0–M6.

| Decision | Topic | Milestone M0 skeleton | Milestone M1 ST20 TX | Milestone M2 ST20 RX | Milestone M3 results, events, stats | Milestone M4 RxTxApp | Milestone M5 gtests | Milestone M6 nightly |
|---|---|---|---|---|---|---|---|---|
| M1 | core direction | B | B | B | B | S | S | |
| M2 | wrap the pipelines through a slot interface | S | B | B | S | | | |
| M3 | media time primary; source kinds | | B | S | | | S | |
| M4 | ANC window when video has a slot delay | | | | | | | |
| M5 | no user code on tasklets | B | S | S | | S | S | |
| M6 | wake-up syscall on a tasklet (W2) | S | | | | | S | S |
| M7 | legacy wire-change policy | | B | | | | S | B |
| M8 | ABI promise and the experimental DSO | B | | | | S | | S |
| M9 | PR #1610 | | | | | | | |
| M10 | external review, survey, sequencing | S | | | | S | S | |
| M11 | the revision-4 shape | B | B | B | B | S | S | |
| M12 | legacy capabilities proposed for removal | | | | | B | B | S |
| M13 | wire-visible defaults of the unified API | | S | S | | | S | S |
| M14 | RTP passthrough defaults and phase | | | | | S | S | |
| M15 | hiding the legacy headers | S | | | | S | S | |
| M16 | Kubernetes pods and crash safety | B | | | S | | S | |
| M17 | NMOS and IPMX (Phase 7, later) | | | | | | | |

The shortest path to milestone M0 is an answer to **M1, M5, M8, M11 and M16**; milestones
M1 and M2 also need **M2, M3 and M7**; milestones M4 and M5 need **M12**.

### 2.2 M1 — the core direction

**Question.** Confirm the five choices the whole design rests on: app-driven push on TX
(`mtl_tx_acquire` → fill → `mtl_tx_submit`), lossless per-unit results apart from bounded
events, no application code on tasklets, one slot and lease contract for library and
application memory, and a new internal session core (L2) over today's engines.
(Q-CORE-1)

- **Options:** (a) confirm all five; (b) change one (name it), which changes most documents;
  (c) the incremental alternative on the legacy API, ≈ 6–9 EM, 4–6 EM of it shared with this
  plan ([archive/02 §2.3](archive/02-architecture.md)).
- **What (c) cannot reach:** one verb set and one metadata struct for every essence (four
  exchange models and twelve metadata structs are the legacy ABI); `struct_size` on the ops
  structs (it needs a `*_create2` per family); removing the tasklet callbacks (they can be made
  optional only); one lease and result contract for imported memory (ext frames are raw
  `{addr, iova}` with five different done points). If these four are not worth ≈ 40–60
  further EM, the engines track and Phase 0.5 still stand on their own.
- **Recommendation:** (a). The unified API is justified by one lease and result contract for
  imported memory, versioned structs and no tasklet callbacks; do the engine work first.
- **Blocks:** milestone M0 and everything after it.
- **Decisions waiting:** D-01, D-02, D-03.

**Your answer:**

### 2.3 M2 — what v1 wraps

**Question.** Does v1 wrap the pipelines (`st20p`, `st22p`, `st30p`, `st40p`, plus the `st41`
session) through one internal slot interface, with L0 hooks in all four pipelines as
first-phase scope? (Q-ARCH-1, Q-ARCH-1a)

- **Options:** (a) wrap the pipelines with the hooks (held slot, once-only completion,
  last-packet hook, rejected-at-pick-up, flush reclaim, idle descriptor cleanup, RX deadline
  force-complete, reclaim, by-index lookup, the submit-time `seq`); (b) wrap the session
  layer as PR #1610 did, which needs the same hooks in `tv_*`/`rv_*`; (c) extract a common
  core first.
- **Recommendation:** (a). The slot interface is (c)'s extracted core; the re-base of the
  legacy pipelines on it is committed, with a go/no-go after all essences are ported. Spike S3
  sizes the hooks.
- **Blocks:** milestones M1 and M2 (the st20p slot interface). For the ST20 branch only the
  st20p hooks are needed; the other three pipelines follow with their essences.
- **Decisions waiting:** D-06.

**Your answer:**

### 2.4 M3 — media time and source kinds

**Question.** Confirm the two-timestamp model (media time is primary and always drives RTP;
launch time is derived from it, with an optional override) and the source kinds for live
timing (`MTL_SOURCE_PLAYBACK`, `MTL_SOURCE_CAPTURE`, `MTL_SOURCE_GATEWAY` with
`min_tx_delay_ns`; the slot delay L is reported, not configured). (Q-TIME-0, Q-TIME-24)

- **Options:** (a) media time primary plus a launch override, and source kinds; (b) two
  independent required fields, and an integer slot delay only; (c) one timestamp plus flags.
- **Recommendation:** (a) and (a). The CAPTURE default `min_tx_delay_ns` is one unit period
  plus the pick-up lead (L = 2 at 1080p59.94 for frame-based capture).
- **Blocks:** milestone M1 (the timing fields of `struct mtl_unit` and the RTP rule); shapes
  milestone M2 (`media_index` on RX) and the timing cases of milestone M5. It also unblocks
  Phase 0.5, the legacy `st_timeline_*` helper.
- **Decisions waiting:** D-09, D-39.

**Your answer:**

### 2.5 M4 — the ANC window when video has a slot delay

**Question.** With a video slot delay L ≥ 1, which frame's ST 2110-40 window does live ANC
use? It is a reading of ST 2110-40. (Q-TIME-18)

- **Options:** (a) ANC always L = 0 relative to its RTP, and video L ≥ 1 only without ANC in
  the same start; (b) ANC follows video L and is flagged non-compliant; (c) live ANC requires
  the gateway model; (d) ANC keeps the video RTP and is sent in the -40 window of the frame in
  which its video unit is transmitted (N = E⁻¹(M) + L_v).
- **Recommendation:** (d); strict anchoring on the media frame is the opt-in option
  `MTL_ANC_WINDOW_MEDIA`.
- **Blocks:** nothing in milestones M0–M6 (ANC is ported after ST20).
- **Decisions waiting:** D-67.

**Your answer:**

### 2.6 M5 — user code on tasklets

**Question.** May any application code run on a tasklet in v1? (Q-THR-1)

- **Options:** (a) never; (b) opt-in inline hooks with a wait-free contract, an allow-list,
  a measured budget and automatic disable; (c) only in a manual-progress mode.
- **Recommendation:** (a). Measure whether busy polling (W0) closes the latency gap; add (b)
  only if it does not. A user busy loop may call the inline-safe DP subset, and
  `MTL_SESSION_RX_BY_INDEX` covers the MXL `query_ext_frame` use without a callback.
- **Blocks:** milestone M0 (call classes, the threading model). Shapes milestone M4 and M5:
  RxTxApp and the gtests that use session callbacks move to application-thread loops.
- **Decisions waiting:** D-04 (with M6).

**Your answer:**

### 2.7 M6 — the wake-up syscall on a pinned tasklet

**Question.** May a pinned tasklet make the wake-up syscall (W2: a non-blocking eventfd write,
only when a waiter is armed) for sessions with a unit period below 1 ms? (Q-THR-2)

- **Options:** (a) never; an unpinned waker thread wakes (W3), W2 per-session opt-in; (b)
  never, and no blocking waits (poll only); (c) always W2.
- **Recommendation:** W3 by default in lcore mode; W2 automatically below a 1 ms unit period;
  W2 always in `MTL_FLAG_TASKLET_THREAD` mode (pinned but preemptible, so the write costs no
  more). A completing context that is not a tasklet wakes directly. Spike S1 measures it.
- **Blocks:** nothing for ST20 at common rates (unit periods above 1 ms). Shapes the waker
  of milestone M0 and the thread-mode runs of milestones M5 and M6.
- **Decisions waiting:** D-04 (with M5), D-68.

**Your answer:**

### 2.8 M7 — the legacy wire-change policy

**Question.** The unified API uses `floor` for RTP and the frame epoch for the default video
RTP. Do the wire-visible engine changes (±1 tick rounding, default video RTP, ANC RTP,
ST 2110-22 CBR, the linear read schedule) also reach the legacy API, and how?
(Q-TIME-15, Q-TIME-16)

- **Options:** (a) the new API only, legacy unchanged; (b) everywhere, with the old
  behaviour behind a flag; (c) everywhere, no flag.
- **Recommendation:** off by default on the legacy API behind opt-in flags (for example
  `ST20_TX_FLAG_RTP_FROM_MEDIA_TIME`, `ST20_TX_FLAG_LINEAR_SCHEDULE`), on by default in the
  unified API; a pcap-diff gate (G-99) proves the legacy wire is unchanged. Revisit the legacy
  default at the ABI freeze.
- **Blocks:** milestone M1 (the engine change and its flag) and milestone M6 (the legacy
  nightly must stay green with an unchanged wire). Shapes the RTP expectations of milestone M5.
- **Decisions waiting:** D-10, D-11.

**Your answer:**

### 2.9 M8 — the ABI promise

**Question.** What ABI does the library promise, and where does the new API ship before the
freeze? (Q-ABI-1)

- **Options:** (a) API stability only, soname bumped every release; (b) a stable ABI for the
  new API (soname, version script, hidden visibility), legacy unversioned; (c) symbol
  versions for everything.
- **Recommendation:** (b). libmtl gets a soname and version script first; the unified API
  ships as its own DSO `libmtl_unified.so.0.<rev>` (soname bumped on every incompatible
  change, node `MTL_UNIFIED_EXPERIMENTAL`) and moves to `MTL_1.0` at the freeze.
- **Blocks:** milestone M0 (meson targets, DSO, symbol nodes). Shapes milestone M4 (RxTxApp
  links the new DSO) and the install steps of milestone M6.
- **Decisions waiting:** D-23.

**Your answer:**

### 2.10 M9 — PR #1610

**Question.** What happens to PR #1610, the earlier session-API attempt? (Q-MIG-3)

- **Options:** (a) close as superseded, with credit; (b) keep open as a reference until the
  new header lands; (c) rebase pieces.
- **Recommendation:** (b), then (a). Salvage `lib/src/new_api/mt_session_event.c` for SF-15,
  merged with credit to its authors.
- **Blocks:** no milestone. Answer before milestone M0 merges, so two session APIs are never
  in review at once.
- **Decisions waiting:** D-27.

**Your answer:**

### 2.11 M10 — external review, survey and sequencing

**Question.** Is an external design review with a user survey an exit criterion before the
header freezes, and which sequencing and staffing does the plan follow? (Q-MIG-1, Q-PLAN-1)

- **Options:** for the review: yes, with at least three named consumers (FFmpeg and
  GStreamer plugin owners, the MXL team, the external engine team) and the private-user
  questionnaire; or no. For the plan: (a) the full plan at 3 FTE (18–28 months); (b) at
  1.5 FTE (3.1–4.7 years); (c) Phase 0.5, the engines track and Phases 1–2 first, then decide.
- **Recommendation:** the review, as the gate before the header freezes; start (c) now and
  choose (a) or (b) at the go/no-go after Phase 2. Total ≈ 55–85 EM, plus ≈ 5–8 EM for the
  Kubernetes work.
- **Blocks:** shapes milestone M0 (header review gate) and the order of milestones M4–M5. The
  survey also feeds M12.
- **Decisions waiting:** D-66.

**Your answer:**

### 2.12 M11 — the revision-4 shape

**Question.** Accept the revision-4 simplification as one package (D-71…D-80, D-84, D-85,
D-88). It reverses rules that earlier reviews set:

| Earlier rule | Revision 4 |
|---|---|
| every knob a typed field | typed core fields; named options (`mtl_options.h`) for the long tail |
| an exported `*_init()` per struct | `MTL_INIT(&s)` |
| distinct buffer and lease handles | slots by index (`unit.slot`); leases stay typed (`mtl_lease_h`) |
| completion modes NONE / EXCEPTIONS / ALL | results on or off: always for application memory, `MTL_SESSION_RESULTS` for library pools |
| separate CQ and EQ objects, an instance EQ, user posts | one queue type (`mtl_queue.h`); events per session |
| group objects | `mtl_session_start` / `mtl_session_stop` over an array |
| fixed stats structs | a registry of named values (`mtl_stat_*`) |
| `-MTL_EAGAIN` for timeout 0, `-MTL_ETIMEDOUT` for an expired data wait | `-MTL_EAGAIN` for both; a miss arms the wait handle |
| a failed submit leaves the lease with the caller | the slot returns to the pool (except `-MTL_EAGAIN`) |
| destroy, force flag and waiting for retirement | `mtl_session_close(s, timeout)` |
| `mtl_call_seq()` | the errno contract of `mtl_last_error()` |

- **Options:** (a) accept the package; (b) accept it but keep one revision-3 rule (name it);
  (c) keep revision 3.
- **Recommendation:** (a).
- **Blocks:** milestones M0–M3 (the header set they implement). Shapes milestones M4 and M5.
- **Decisions waiting:** D-71, D-72, D-73, D-74, D-76, D-77, D-79, D-80, D-84, D-85, D-88.
- **Detail:** [archive/REVISION-4.md §5](archive/REVISION-4.md),
  [archive/simplification/](archive/simplification/) S2–S7.

**Your answer:**

### 2.13 M12 — legacy capabilities proposed for removal

**Question.** No current use case is cut without approval. Approve or reject each row; none is
used by an in-tree consumer beyond tests, a sample or RxTxApp. (Q-R4-2)

| Candidate | Evidence | If kept |
|---|---|---|
| header split RX | needs a DPDK patch absent for the pinned 26.07; one sample, one test; RxTxApp option | a later phase with the patch |
| `uframe_pg_callback` (app code per pixel group on the tasklet) | violates the no-app-code rule; RxTxApp and two tests | per-packet conversion as `MTL_OPT_RX_CONVERT_PER_PACKET` (`rx.convert_per_packet`) |
| `st20rc` redundant-combined RX | two-leg sessions do the same; one sample, RxTxApp `rx_st20r_app.c` | — |
| public per-pair converters (≈ 105) | replaced by `mtl_convert()`; per-pair stay internal for tests | — |
| public user DMA (`mtl_udma_*`), public lcore borrowing | perf tools and tests only; sessions use DMA through the session's DMA option | an advanced header later |
| user schedulers and tasklets (`mtl_sch_*`) | one integration test (`sch_test.cpp`) | an advanced header with the inline-safe DP subset |
| `ST22_*_FLAG_DISABLE_BOXES`, RTCP flags on audio, ANC, fastmeta | no consumer; the RTCP flags do nothing today | — |
| queue meta, `DATA_PATH_ONLY` | tests only; a suspected NULL dereference (SP-02) | an extension table after a verification test |
| `st_draw_logo`, `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_get_if_ip`, `mtl_udma_fill` | thin wrappers, zero or one consumer | — |
| dead fields (`tx/rx_sessions_cnt_max`, `sample_size/num`, `sip_addr`, RX `pacing/packing`, `MTL_FLAG_BIND_NUMA`, ST40P `FORCE_NUMA`, `st40p_rx_ops.rtp_ring_size`, UDP remnants) | documented as unused | — |
| DPDK AF_XDP and AF_PACKET PMD backends | native AF_XDP and kernel sockets cover them | an experimental backend name |
| `st22_tx_ops.fmt`, `st22_rx_ops.fmt` | ignored by today's library | a `cvideo` sampling field |

- **Options:** per row: remove, or keep (the last column says where it would go).
- **Recommendation:** remove every row unless the M10 survey finds an external user.
- **Blocks:** milestones M4 and M5: whether RxTxApp's header-split, `st20r` and
  `uframe_pg_callback` paths and the user-scheduler gtests are ported, left on the legacy API,
  or deleted. Shapes the nightly suite list (milestone M6).
- **Decisions waiting:** D-87.
- **Detail:** [archive/simplification/R4-coverage-check.md](archive/simplification/R4-coverage-check.md).

**Your answer:**

### 2.14 M13 — wire-visible defaults of the unified API

**Question.** Accept four defaults of the unified API that differ on the wire from today's
legacy defaults (the legacy API keeps today's behaviour under M7): the default video RTP from
the frame epoch; incomplete RX frames delivered with `MTL_RX_INCOMPLETE` instead of dropped
(`rx.incomplete` = `MTL_RX_DELIVER`); ST 2110-22 constant bitrate (`MTL_CVIDEO_CBR`); packet
mode verbatim by default. (Q-R4-3)

- **Options:** (a) accept the four; (b) keep today's behaviour for some (name them).
- **Recommendation:** (a).
- **Blocks:** shapes milestone M1 (default RTP), milestone M2 (incomplete frames), and the
  expectations of milestones M5 and M6.
- **Decisions affected:** D-10, D-32, D-82.

**Your answer:**

### 2.15 M14 — RTP passthrough defaults and phase

**Question.** Accept the packet-unit defaults (`MTL_UNIT_PACKETS`, Q-PKT-1…9) and the phase.
(Q-R4-4)

- Verbatim by default: of the RTP header the library writes only the fields in
  `packet.set_fields`.
- RX chunks are formed at dequeue by count or timeout; no RX reordering; duplicates of the two
  legs removed by sequence.
- The generic RTP essence (`MTL_RTP`) carries RTP only (no raw UDP yet); per-packet launch
  offsets later.
- RX copies in the caller by default, with a bounded zero-copy lend.
- ST 2022-6 headers are written by the application; tests inject faults through packet mode.
- Packet mode is phase 2P, right after Phase 2, because hiding the session headers depends on
  it.
- **Options:** accept, or change single items.
- **Recommendation:** accept.
- **Blocks:** shapes milestones M4 and M5: the RTP-level ST20 modes of RxTxApp and the
  RTP-level ST20 gtests stay on the legacy API until packet mode is ported.
- **Decisions waiting:** D-82.
- **Detail:** [archive/simplification/S8-rtp-passthrough.md](archive/simplification/S8-rtp-passthrough.md).

**Your answer:**

### 2.16 M15 — hiding the legacy headers

**Question.** Accept the three tiers (public, legacy opt-in, internal) and the stages: 0
(symbol nodes `MTL_LEGACY_SESSION`, `MTL_LEGACY_PIPELINE`, `MTL_LEGACY_CORE`, `MTL_INTERNAL`,
hidden visibility, a soname), 1 (headers move unchanged to `include/mtl/legacy/` with
forwarding stubs), F (deprecated; a warning unless `MTL_LEGACY_API`), F+1 (an error without
`MTL_LEGACY_API`; `pkg-config mtl-legacy`), ≥ F+2 (not installed; libmtl soname bump).
`libmtl_unified.so.1` stays a separate library at the freeze; plugin ABI v2 (`mtl_plugin.h`)
and `mtl_convert.h` land before F. (Q-R4-5)

- **Options:** accept; or keep the legacy headers public and frozen.
- **Recommendation:** accept; set the pipeline headers' removal date at the go/no-go after
  Phase 2.
- **Blocks:** shapes milestone M0 (stage 0) and milestones M4 and M5 (legacy tests keep
  testing the engine through an internal dependency).
- **Decisions waiting:** D-83.
- **Detail:** [archive/simplification/S9-hiding-session-headers.md](archive/simplification/S9-hiding-session-headers.md).

**Your answer:**

### 2.17 M16 — Kubernetes pods and crash safety

**Question.** Accept as one package (Q-R4-6, sub-questions Q-K8S-1…11):

- `mtl_instance_close(mt, timeout_ns)` shuts the instance down on its last reference, network
  first; 0 retired, 1 quiesced, `-MTL_EIO` when a port could not be stopped (exit the
  process). `mtl_instance_shutdown` adds flags and a report. Handles closed by the instance
  stay safe to pass (R4).
- R8: no signal handler and no `atexit` in the library, close-on-exec, a fork rule, nothing
  found by PID; a SIGKILL leaves nothing that blocks a restart.
- `mtl_instance_get_health` for probes; liveness never depends on links, PTP lock or a
  surviving ST 2022-7 leg.
- Open fails fast with one named reason (environment reasons 600–614) and waits for no link
  or time lock.
- CPUs from the affinity mask; the SysV lcore table and the manager-optional flag go; no-IOMMU
  refused unless `instance.allow_noiommu`; time read-only in pods; files only in
  `instance.runtime_dir`.
- **Options:** accept; or the shutdown and R8 only, keeping today's lcore table, no-IOMMU
  behaviour and blocking open.
- **Recommendation:** accept. Q-K8S-6 changes behaviour for no-IOMMU users, who must set
  `instance.allow_noiommu`.
- **Blocks:** milestone M0 (instance open and close, handle slots, R8). Shapes milestone M3
  (health) and the shutdown cases of milestone M5.
- **Decisions waiting:** D-89, D-90, D-91, D-92.
- **Detail:** [deployment.md](deployment.md), [archive/16](archive/16-kubernetes-and-crash-safety.md).

The sub-questions ([archive/16 §13](archive/16-kubernetes-and-crash-safety.md)); accepting
M16 accepts each recommendation:

| ID | Question | Recommendation |
|---|---|---|
| Q-K8S-1 | `mtl_instance_close(mt, timeout_ns)` with 0 / 1 / `-MTL_EIO` (`QUEUE_QUARANTINED`), and `mtl_instance_shutdown` with a report | yes |
| Q-K8S-2 | Leases still out at shutdown: free each slot at the next control-plane call after its last lease returns, never earlier | yes |
| Q-K8S-3 | CPUs: the affinity mask in pods, MtlManager otherwise, OFD locks (`MTL_CPUARB_LOCKS`) without a manager; the SysV table and the manager-optional flag removed | yes |
| Q-K8S-4 | Device removal: sessions enter ERROR and the application closes them; liveness fails only when a session has no leg left | yes |
| Q-K8S-5 | DPDK's SIGBUS hotplug handler only with `instance.hotplug` | yes |
| Q-K8S-6 | Refuse no-IOMMU unless `instance.allow_noiommu` | yes; changes behaviour for no-IOMMU users (the legacy API only warns) |
| Q-K8S-7 | IGMP after SIGKILL: document the window (≈ 260 s) in v1; leaves sent by the manager later | yes |
| Q-K8S-8 | "A node daemon disciplines the PHC": the start-time agreement test plus the application's `locked` flag (`time.phc_trust`) | yes, after a spike on the bound |
| Q-K8S-9 | Default of `instance.cpu_shared`: `MTL_CPU_SHARED_WARN` or `MTL_CPU_SHARED_REFUSE` in lcore mode | WARN in v1; the health detail shows it |
| Q-K8S-10 | Shutdown default "finish the unit on the wire, flush the rest" rather than drain | yes; `MTL_SHUTDOWN_DRAIN` exists |
| Q-K8S-11 | Recommend set-up A (non-root, node prepared) in the documentation | yes; B as the fallback |

**Your answer:**

### 2.18 M17 — NMOS and IPMX (Phase 7, later)

**Question.** Accept the NMOS and IPMX design (Q-R4-7, sub-questions Q-NI-1…10): the IS-05
activation contract of `mtl_session_update`, `mtl_sdp.h`, `mtl_rtcp.h`, `mtl_crypto.h`, the
IPMX profile and timing without PTP. Under D-98 everything except the atomic update of
today's destination and source change is Phase 7; the declarations are under `MTL_LATER`.

- **Options:** accept; or NMOS only (the IS-05 contract and SDP), IPMX later.
- **Recommendation:** accept as the design; implement in Phase 7; `mtl_crypto.h` after its
  cost spike. IPMX cannot be met without sender reports and timing without PTP.
- **Blocks:** nothing in milestones M0–M6. The update rule (the switch at the slot boundary
  by the clock) shapes the Phase 2 twins of the update-destination gtests
  (`st20_update_source.cpp`), which the branch does not port ([implementation-plan.md](implementation-plan.md) §5.6).
- **Decisions waiting:** D-93, D-94, D-95, D-96.
- **Detail:** [nmos-ipmx.md](nmos-ipmx.md).

The sub-questions ([archive/17 §7](archive/17-nmos-and-ipmx.md)); under D-98 every "v1" of
the archive reads "Phase 7, declared under `MTL_LATER`":

| ID | Question | Recommendation |
|---|---|---|
| Q-NI-1 | Ship `mtl_sdp.h` (render and parse), replacing Q-MODE-6's "SDP values only" | yes (Phase 7) |
| Q-NI-2 | Ship `mtl_rtcp.h`; IPMX cannot be met without sender reports | yes, Phase 7 with timing without PTP (the archive's "phase 3" predates D-98) |
| Q-NI-3 | `mtl_crypto.h` now, or after a cost spike | the header as a proposal; implementation after the spike |
| Q-NI-4 | Rename the NACK options `rtcp.*` to `rtx.*` (`MTL_OPT_RTX`, `rtx.enable`) | yes; the legacy names stay in the bridge |
| Q-NI-5 | Mute (every leg disabled) instead of a scheduled stop | yes |
| Q-NI-6 | Both legs on one port | yes, with an explicit `flows[1].port` |
| Q-NI-7 | Audio updates at a packet boundary | yes |
| Q-NI-8 | `session.profile` (`MTL_OPT_PROFILE`) as the one IPMX switch; profiles change zero defaults and labels only | yes |
| Q-NI-9 | No separate IPMX sender type: under the profile, N uses TR-10-1's CMAX and VRX | yes, unless the VSF requires a distinct TP value |
| Q-NI-10 | RX `MTL_UPDATE_REAPPLY` without a leave (reports re-sent) instead of a leave and join | yes; leaving and joining both legs at once is a guaranteed hit |

**Your answer:**

## 3. Decision log

### 3.1 Decisions in force

Names are the revision-4 header names. "r4 shape" means a later decision changed the form
and the row states the current form. **Where** names the maintained document that
specifies it. **Answers** names the question of
[archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md) the decision answers (its options and
research are there), with the study (S1–S9, K1–K3, N1, I1) in brackets; "C5 §x" is a finding
of the adversarial user review C5 (not in the repository; its findings and answers are in [archive/reviews/C5-response.md](archive/reviews/C5-response.md)); "—" is a review
finding without a question, or the maintainer's direction.

| ID | Decision | Status | Where | Answers |
|---|---|---|---|---|
| D-01 | One slot and lease state machine and one submit/dequeue contract for every provisioning path (library pool, attached, exported); no "library-owned vs user-owned" mode | Awaiting M1 | contract.md | Q-CORE-1 |
| D-02 | App-driven push on TX: `mtl_tx_acquire` → fill → `mtl_tx_submit`; the library never pulls frames through callbacks | Awaiting M1 | contract.md, engine.md | Q-CORE-1 |
| D-03 | Results are materialised by the reader from a per-session lease table; a slot is freed once its result is in the unread-results ring (capacity = pool), so reuse never waits for predecessors; results publish in submission order; state changes go to session events | Awaiting M1 | engine.md, contract.md | Q-CMP-1 |
| D-04 | No application code on tasklets; the tasklet side of every hand-off is a wait-free store plus a fence; wake-ups through an armed-waiter bit and a deadline-driven waker thread; no syscalls on PMD tasklets | Awaiting M5, M6 | engine.md | Q-THR-1, Q-THR-2, Q-THR-2a |
| D-05 | Every public function has a call class (`MTL_API_CP`, `_DP`, `_DPC`, `_WT`, `_AS`) in the header, enforced in debug builds | Proposed default | contract.md | — |
| D-06 | A new internal session core (L2) owns the contracts; L1 adapters wrap the pipelines and the `st41` session through one slot interface with L0 hooks | Awaiting M2 | engine.md | Q-ARCH-1, Q-ARCH-1a |
| D-07 | Typed value handles (`mtl_instance_h`, `mtl_session_h`, `mtl_lease_h`, `mtl_region_h`, `mtl_timeline_h`) over grow-only chunked tables; 0 is null; a lease is session index:16, slot:16, generation:32 with random generations; stale → `-MTL_ESTALE`, foreign → `-MTL_EBADF`; an in-flight counter on its own cache line guards destroy | Proposed default; r4 shape (D-76) | engine.md | Q-THR-5 |
| D-08 | Explicit states `MTL_STATE_CREATED`, `ARMED`, `RUNNING`, `DRAINING`, `FLUSHING`, `STOPPED`, `ERROR`, `CLOSING`, `RETIRED`; start now, at TAI or at an index with `preroll_ns`; `mtl_session_discard`; stop `MTL_STOP_DRAIN` or `MTL_STOP_FLUSH`; results drainable until retirement | Proposed default | contract.md | Q-LIFE-1, Q-LIFE-9 |
| D-09 | Media time and launch time are separate; RTP = `floor(M × R) mod 2^32` from media time only, one rule for every essence (exception: `MTL_MEDIA_SENDER`, D-95) | Awaiting M3 | timing.md | Q-TIME-0, Q-TIME-15 |
| D-10 | The default video media time in the unified API is the frame epoch `N × TFRAME`, not the TX cursor | Awaiting M7 | timing.md | Q-TIME-16 |
| D-11 | Exact rational arithmetic (128-bit intermediates) for all media-time math; computed from the anchor, never accumulated | Awaiting M7 | timing.md | Q-TIME-15 |
| D-13 | Explicit late policy (`tx.late_policy`: `MTL_LATE_DROP`, bounded `MTL_LATE_SEND_LATE`, `MTL_LATE_RESLOT`), default by media mode (AUTO → RESLOT, INDEX/TAI → DROP); underrun policy by essence (skip; empty ANC; fastmeta keep-alive; silence option; repeat-last later); an invalid exact request fails, never falls back | Proposed default | timing.md | Q-TIME-4, Q-TIME-23 |
| D-14 | TX results carry deadline, sent time and a signed `margin_ns`; `mtl_tx_next_slot` returns the next slot and its deadline; `MTL_TX_ON_TIME` is an admission verdict | Proposed default; r4 shape (D-75, D-77) | timing.md, contract.md | Q-TIME-10, Q-CMP-6 |
| D-15 | Every time is `int64_t` TAI ns whose validity is a flag (`MTL_UNITF_*_VALID`, `MTL_TXR_*`), never zero | Proposed default; r4 shape (D-80) | timing.md | — |
| D-16 | Memory regions (`mtl_mem_import`) are refcounted, page-aligned (never rounded outward), mapped lazily at first attach or eagerly into named ports; shmem, memfd, hugetlbfs and GPU-pinned host memory direct; regular files copy only; `mtl_mem_close` retires at the last reference | Proposed default | contract.md | Q-MEM-2, Q-MEM-12 |
| D-17 | The data path is independent of allocation origin: direct when possible, `MTL_SESSION_REQUIRE_DIRECT` fails at create otherwise; the path taken is reported per session and per unit (`MTL_TXR_COPIED`) | Proposed default | contract.md | Q-MEM-7 |
| D-18 | The terminal TX result comes after both storage access and the transport outcome are finished (needs the L0 completion hooks) | Proposed default | engine.md | Q-MEM-1 |
| D-19 | Capability query per port, a dry-run `mtl_session_query`, REQUIRE/PREFER/OFF requests with granted values reported; runtime downgrades are events (`MTL_EVENT_PACING_CHANGED`) | Proposed default | contract.md | Q-TIME-7 |
| D-21 | One error vocabulary of `MTL_E*` constants with Linux errno values and one meaning each: `-MTL_ESHUTDOWN` only for app-initiated stop or close, `-MTL_EIO` for ERROR, `-MTL_ENODEV` for a device gone, `-MTL_EAGAIN` for "nothing now" | Proposed default | contract.md | Q-CMP-3 |
| D-23 | The new API ships in its own experimental DSO `libmtl_unified.so.0.<rev>` (node `MTL_UNIFIED_EXPERIMENTAL`) until the ABI freeze; libmtl gets a soname and version script first | Awaiting M8 | migration.md | Q-ABI-1 |
| D-24 | Engines first: legacy APIs keep working and receive every engine fix; bugfixes on by default, wire-visible changes behind legacy opt-in flags; every phase passes a legacy KahawaiTest and acceptance gate | Proposed default | migration.md, engine.md | Q-ABI-4, Q-TIME-16 |
| D-26 | One flow descriptor (`struct mtl_flow`, `mtl_flow_ipv4()`) for every essence; flow changes apply on every leg atomically at one `struct mtl_when`, prepared before commit, never holding a tasklet lock across ARP or IGMP | Proposed default; r4 shape (D-72) | contract.md | Q-MODE-2 |
| D-27 | PR #1610 is superseded, not rebased: kept as a reference until the new header lands, then closed with credit | Awaiting M9 | migration.md | Q-MIG-3 |
| D-30 | Async-signal-safe, sticky interruption: `mtl_session_interrupt(s, on)`, `mtl_instance_interrupt(mt, on)`; waits return `-MTL_ECANCELED`; close wins | Proposed default | contract.md | Q-LIFE-6 |
| D-31 | Explicit time sources (`enum mtl_time_source`: built-in PTP, PHC, `CLOCK_TAI`, system TAI, user); one published time base for tasklets; a per-timeline clock-step policy | Proposed default | timing.md | Q-TIME-1, Q-TIME-2, Q-THR-7a |
| D-32 | ST 2110-22 is its own essence (`MTL_CVIDEO`); `MTL_CVIDEO_CBR` by default, `MTL_CVIDEO_VBR_MAX` opt-in and flagged non-compliant; an oversize codestream fails at submit | Proposed default | timing.md, contract.md | Q-MODE-3 |
| D-33 | `mtl_session_config` reserves room for extension; unknown or non-zero reserved fields are rejected | Proposed default; r4 shape (D-72, D-74) | contract.md | Q-ABI-3 |
| D-34 | Names: essences `MTL_VIDEO`, `MTL_CVIDEO`, `MTL_AUDIO`, `MTL_ANC`, `MTL_FASTMETA`, `MTL_RTP`; verbs `mtl_tx_*` / `mtl_rx_*`; `MTL_TX` / `MTL_RX`; timeout last; headers `include/mtl/experimental/`; source `lib/src/unified/` | Proposed default | contract.md, migration.md | Q-ABI-5 |
| D-36 | Close with outstanding leases is deferred: release still works, `mtl_session_close` returns 1, `MTL_EVENT_SESSION_RETIRED` when done | Proposed default; r4 shape (D-88) | contract.md | Q-LIFE-4 |
| D-37 | RX pool policies: a full pool drops new units, or reclaims the oldest unread with `MTL_SESSION_RX_LATEST`; `MTL_SESSION_RX_BY_INDEX`; zero fill by default for library pools, done in dequeue (`MTL_SESSION_RX_NO_FILL` opts out) | Proposed default | contract.md | Q-MEM-6 |
| D-38 | The media mode (`MTL_MEDIA_AUTO`, `INDEX`, `TAI`) is a session property; TAI snapping `MTL_SNAP_NEAREST` with hysteresis for every source kind, `MTL_SNAP_LOCKED_PHASE` opt-in; duplicates are results (`DUPLICATE_SLOT`, `DUPLICATE_INDEX`), never `-MTL_EINVAL` | Proposed default | timing.md | Q-TIME-25 |
| D-39 | Source kinds (`sc.source_kind`) with `min_tx_delay_ns`; the slot delay L is reported, not configured | Awaiting M3 | timing.md | Q-TIME-24, Q-TIME-18 |
| D-41 | Audio: a carry buffer, integer samples per packet, ±one packet absorbed for CAPTURE and TAI, forward gaps keep the packet grid, only a discontinuity re-phases; `mtl_tx_write` for copy essences | Proposed default | timing.md | Q-TIME-6 |
| D-42 | Array reads (`mtl_tx_reap`, `mtl_queue_reap`, `mtl_queue_read_events`) take the caller's record size, never rewrite a stride, and return a count ≥ 0 | Proposed default; r4 shape (D-77, D-79) | contract.md | — (C5 §2.1, §2.6) |
| D-43 | Wait handles are portable: a Linux eventfd or a Windows event `HANDLE` (`mtl_session_get_wait_handle`) | Proposed default; r4 shape (D-88) | contract.md | — (C5 §2.10, §2.13) |
| D-44 | Typed `mtl_instance_h` over a versioned `struct mtl_instance_params`; `MTL_INSTANCE_SHARED` is the refcounted process-wide instance; a later open with differing ports, lcores, time source or options is `-MTL_EEXIST` (`INSTANCE_MISMATCH`, no merge table); it outlives closing sessions; runtime port open later | Proposed default; r4 (D-71, D-89) | contract.md | Q-ARCH-2, Q-LIFE-2, Q-ABI-2 |
| D-45 | The header set in `sketch/` is normative; it compiles as C99 and C++17 with `-Wpadded -Werror`; every public struct has a size check; `sketch/check.sh` runs in CI | Proposed default | contract.md | — (C5 §2.2) |
| D-47 | An inline-safe DP subset may be called from user busy loops (trylock only, no eventfd drain); every other call there returns `-MTL_EDEADLK` | Proposed default | engine.md, contract.md | Q-THR-1 |
| D-48 | Commands are immediate (stop, flush, discard, interrupt: every tasklet iteration) or boundary (flow change, grid-changing update, leg enable); the control plane applies them itself for a detached session; an unacknowledged command (100 ms) puts the session in ERROR (`CMD_TIMEOUT`) | Proposed default | engine.md | — (C5 §4.2) |
| D-49 | Close or ERROR on a stalled queue: bounded idle cleanup, then a worker restarts a dedicated queue or quarantines a shared one, escalating to port reset; retirement only after the device references are gone | Proposed default | engine.md | Q-CMP-7 |
| D-50 | Concurrency: acquire and releases MP-safe; submit one context at a time unless `MTL_SESSION_MT_SUBMIT` (required for exported pools); readers under the reaper lock unless `MTL_SESSION_SINGLE_READER`; one waiter set per wait target | Proposed default | contract.md | Q-THR-8 |
| D-51 | RX timing: RX sessions bind the epoch or a resolved timeline and report an exact `media_index`; RX starts in one array arm every filter together with one link offset; units complete at a due time (RX hook); the join is made at the first start and kept across stop | Proposed default | timing.md | — (C5 §5.7, §8.4) |
| D-52 | `preroll_ns` in `struct mtl_when`; the horizon (`tx.horizon_ns`, 1 s) counts from the resolved start; a start that would strand queued units beyond it fails atomically (`BEYOND_HORIZON`) | Proposed default | timing.md | Q-LIFE-5, Q-TIME-3 |
| D-53 | `min_submit_lead_ns` = media time − pick-up deadline; `media_time_offset_ns` declares a just-in-time producer's latency in TAI mode and replaces `rtp_timestamp_delta_us`; live framework sinks use TAI + NEAREST, INDEX is for whole-timeline owners | Proposed default | timing.md | Q-TIME-5 (C5 §5.4) |
| D-54 | A plane `stride` ≥ row bytes is direct for ST20 TX and RX; an interlaced unit is a field with rows = height/2; a woven frame is two field slots over one region | Proposed default | contract.md | — (C5 §6.1, §6.9) |
| D-55 | A slot belongs to one pool; sessions share bytes through slots over one region; `unit.hold` keeps an RX slot until the TX unit is done, so one RX unit feeds N TX sessions zero-copy (`mtl_tx_send_slot`) | Proposed default; r4 shape (D-76) | contract.md | Q-MEM-11 |
| D-56 | Moving-cursor TX from application memory, Phase 4: `mtl_tx_acquire_layout` binds a slot to a new layout per acquire (`MTL_LATER`); the revision-3 dynamic pool (`MTL_POOL_DYNAMIC`, `mtl_tx_acquire_dynamic`) has no header. Opt-in early source release for copy paths, Phase 4, has no header symbol yet | Proposed default; r4 shape | contract.md | Q-MEM-10, Q-MEM-1 |
| D-57 | The ST20/ST22 frame-count cap is lifted in the engine (E11); until then `max_count` reports it | Proposed default | engine.md | Q-MEM-5 |
| D-58 | `mtl_session_update` with `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` in CREATED or STOPPED keeps handle, name, SSRC, stats and bindings; the only way to change the port or the format without close and create | Proposed default; r4 shape (D-72, D-88) | contract.md | — (C5 §7.4) |
| D-59 | Every configured ST 2022-7 leg is reserved regardless of link and never pruned; `legs_disabled` via `MTL_UPDATE_LEGS`; admin × oper state per leg; a link monitor | Proposed default; r4 shape (D-72) | contract.md | Q-LIFE-7 |
| D-60 | Session names copied (64 B), unique per instance, `""` = generated; `mtl_instance_list_sessions`; free capacity by `MTL_QUERY_CHECK_CAPACITY` (`-MTL_ENOSPC`, `CAPACITY_*`) and the `capacity.*`, `port.free_*` stats (no `mtl_port_get_capacity`); `mtl_session_get_info` has every granted value (SDP: Phase 7) | Proposed default; r4 shape (D-80) | contract.md | — (C5 §8.7, §8.11) |
| D-62 | Create and start never wait for ARP or IGMP; flow resolution is a per-leg state (`MTL_EVENT_FLOW_STATE`, `status.leg[]`); units on an unresolved leg drop with `WAITING_NEIGHBOUR` | Proposed default | contract.md | Q-LIFE-9 |
| D-63 | MtlManager loss never kills or stalls the process; creates after loss return `-MTL_EAGAIN` (`MANAGER_LOST`); reconnect re-announces held lcores; with no manager present none is used | Proposed default; r4 shape (D-92) | deployment.md | — (C5 §8.6) |
| D-64 | Test substrate first: the null backend (`null:<n>`), the test clock (`mtl_test_clock`), `mtl_debug_inject` in debug builds, `nicctl.sh` fault steps; guarantees marked P or BE; budgets confirmed by spike S0 | Proposed default | implementation-plan.md | — (C5 §3.3, §8.10, §1.8) |
| D-66 | Phase 0.5 (legacy `st_timeline_*` helper, ST30P/ST40P flag fixes, SF-15) and an engines-first track before the unified phases; ≈ 55–85 EM | Awaiting M10 | implementation-plan.md | Q-PLAN-1 |
| D-67 | Live ANC and fastmeta keep the video RTP and are sent in the ST 2110-40 window of the frame their video unit is transmitted in; strict media-frame anchoring opt-in (`MTL_ANC_WINDOW_MEDIA`) | Awaiting M4 | timing.md | Q-TIME-18 |
| D-68 | Waker W3 by default in lcore mode; W2 below a 1 ms unit period and always in `MTL_FLAG_TASKLET_THREAD` mode | Awaiting M6 | engine.md | Q-THR-2 |
| D-69 | Security posture of the new surfaces (import alignment and access, wait-handle ownership, waker scheduling, handle randomness, debug API build gating); deprecation no earlier than two `vYY.MM` releases after notice | Proposed default | deployment.md, migration.md | — (C5 §1.10) |
| D-70 | Small defaults: ANC `total_lines` 0 = the video's raster, else 1125; fastmeta rate 0 outside a video start is `FIELD_REQUIRED`; linear histograms one option key each | Proposed default | contract.md | — |
| D-71 | A lean core `mtl.h` (C library includes only, 32 functions) and 16 optional headers with one job each (17 headers in all) | Awaiting M11 | contract.md | Q-R4-1 (S2 P10, S9) |
| D-72 | One `struct mtl_session_config` (direction, essence, one member per essence, others zero); `mtl_session_create`, dry-run `mtl_session_query`, `mtl_session_open` (create and start); `mtl_session_update(s, &sc, parts, &when, &planned)` replaces reconfigure, flow change and leg enable | Awaiting M11 | contract.md | Q-R4-1 (S2 P4/P9, S3 P3/P7/P8, S6, S7 F-02) |
| D-73 | Typed fields for what most applications and the timing recipes set; every other knob is an option key (`mtl_options.h`) with presence semantics, a string name and descriptors | Awaiting M11 | contract.md | Q-R4-1 (S3 P1/P2) |
| D-74 | Inputs carry `struct_size` and are initialised by `MTL_INIT(&s)`; outputs carry none and take a size argument | Awaiting M11 | contract.md | Q-R4-1 (S5 P8) |
| D-75 | One `struct mtl_unit` (208 B) lent by acquire and dequeue and read by submit; the slot hint is `mtl_tx_next_slot` | Proposed default | contract.md | Q-R4-1 (S4 P1/P2) |
| D-76 | Buffers are pool slots named by index; `mtl_session_attach` imports and lays out N slots; leases stay typed | Awaiting M11 | contract.md | Q-R4-1 (S4 P4) |
| D-77 | A 96-byte core TX result (`struct mtl_tx_result`), the full timing record by size; results always for application memory, opt-in (`MTL_SESSION_RESULTS`) for library pools; a cookie without results is `COOKIE_WITHOUT_RESULTS` | Awaiting M11 | contract.md | Q-R4-1 (S4 P3/P6) |
| D-78 | No group object: `mtl_session_start` / `mtl_session_stop` take arrays; a start array is one timeline and one direction, all or none; ANC and fastmeta take their raster from the first video of their start | Proposed default | timing.md, contract.md | Q-R4-1 (S2 P5, S3 P5) |
| D-79 | Events per session and one shared queue type (`mtl_queue.h`) for results, RX readiness and events; instance event queue, user posts and per-port subscription removed | Awaiting M11 | contract.md | Q-R4-1 (S2 P7, S5 P6/P7) |
| D-80 | A stats registry of named values (`mtl_stat_list`, `mtl_stat_read`) with bulk DP reads; counters cumulative for the session's life; lean status (`struct mtl_session_status`, 104 B) and info (256 B) | Awaiting M11 | contract.md | Q-R4-1 (S5 P1–P4) |
| D-81 | One reason vocabulary (`enum mtl_reason`, `mtl_reasons.h`) for errors, states, events and TX results | Proposed default | contract.md | Q-R4-1 (S5 P5) |
| D-82 | Packet units (`MTL_UNIT_PACKETS`) on every essence and a generic RTP essence; a chunk of slots is one unit with one result; L2–L4 and declared RTP fields only; essence pacing, ST 2022-7 duplication, RX dedup by sequence; no mbufs, no app code on tasklets. Answers Q-MODE-1 (maintainer, 2026-10-01) | Awaiting M14 | contract.md | Q-MODE-1, Q-R4-4 (S8) |
| D-83 | Legacy headers non-public in three tiers, staged 0 → 1 → F → F+1 → ≥ F+2; plugin ABI v2 (`mtl_plugin.h`); `mtl_convert.h` | Awaiting M15 | migration.md | Q-R4-5 (S9) |
| D-84 | `mtl_last_error()` follows the errno contract (valid until the next non-AS call); no call sequence | Awaiting M11 | contract.md | Q-R4-1 (S5 P9) |
| D-85 | `MTL_TIME_SOURCE_AUTO` = a disciplined NIC PHC, else `CLOCK_TAI`, else `MTL_TIME_SOURCE_SYSTEM_TAI` (estimated), chosen once at open; the built-in PTP client runs only when named; the FREERUN fallback and re-evaluation while running are Phase 7 (D-95) | Awaiting M11 | timing.md | Q-R4-1 (S3 P9) |
| D-86 | ANC packet tables and user meta live in the slot's meta area on TX and RX, validated and snapshotted at submit | Proposed default | contract.md | Q-R4-1 (S4 P2) |
| D-87 | Coverage gate: every legacy capability has a revision-4 home, a later phase, or an approved cut | Awaiting M12 | migration.md | Q-R4-2 (S1, S9) |
| D-88 | Submit consumes the lease on failure (except `-MTL_EAGAIN`); `-MTL_EAGAIN` for every "nothing now", and a miss arms the wait handle; interrupts cancel data waits only; `mtl_session_close`; one plain wait handle; `MTL_PORTS` | Awaiting M11 | contract.md | Q-R4-1 (S7) |
| D-89 | `mtl_instance_close(mt, timeout_ns)` shuts down on the last reference, network first, MtlManager grants returned last; 0 / 1 / `-MTL_EIO` (`QUEUE_QUARANTINED`); `mtl_instance_shutdown` adds flags and a report; handle slots process-wide and never freed (R4) | Awaiting M16 | deployment.md, contract.md | Q-R4-6 (K1, K3) |
| D-90 | R8: no signal handler or `atexit` in the library (DPDK's SIGBUS handler during heap growth excepted); AS calls safe at any time; close-on-exec; a fork closes MTL's descriptors in the child; nothing found by PID | Awaiting M16 | deployment.md | Q-R4-6 (K3 P-1, P-9, P-10, P-11) |
| D-91 | Lock-free health (`mtl_instance_get_health`: liveness, readiness, phase), liveness independent of links, PTP and a surviving leg; open never waits for links, neighbours or lock; environment reasons 600–614 | Awaiting M16 | deployment.md, contract.md | Q-R4-6 (K1 K-REQ-6, -7) |
| D-92 | CPUs from the affinity mask with MtlManager, OFD locks or no arbitration (SysV table removed); every thread pinned, non-scheduler threads on `instance.main_lcore`; no-IOMMU refused unless `instance.allow_noiommu`; files only in `instance.runtime_dir`; time read-only in pods | Awaiting M16 | deployment.md | Q-R4-6 (K1, K2) |
| D-93 | IS-05 activation: the switch at the slot boundary by the clock; `planned_tai_ns` from `mtl_session_update`, `status.update_state`, `MTL_EVENT_UPDATE`; `MTL_UPDATE_REAPPLY`, `MTL_UPDATE_DRY_RUN`, cancel; mute; reserved legs; port changes while running (the atomic update of today's destination change in Phase 2, the rest Phase 7) | Awaiting M17 | nmos-ipmx.md, contract.md | Q-R4-7 (N1) |
| D-94 | `mtl_sdp.h` (render and parse) on public calls only (Phase 7, later; `MTL_LATER`) | Awaiting M17 | nmos-ipmx.md | Q-R4-7 (N1, I1) |
| D-95 | IPMX: `session.profile` changes zero defaults and labels only; RTCP sender reports and the Info Block (`mtl_rtcp.h`); NACK options `rtx.*`; `MTL_MEDIA_SENDER`, `MTL_SUBMIT_SENDER_TIME`; `MTL_TIME_SOURCE_FREERUN` and AUTO re-evaluated while running; IGMPv2; RX header extensions (Phase 7, later) | Awaiting M17 | nmos-ipmx.md | Q-R4-7 (I1) |
| D-96 | PEP encryption with options `crypto.*` (`mtl_crypto.h`); keys only through `mtl_crypto_set_key`; no IV and counter reuse under one key; HDCP keys never in MTL (Phase 7, later) | Awaiting M17 | nmos-ipmx.md | Q-R4-7 (I1) |
| D-97 | Typed configuration only, no spec strings or string parsers; `mtl_session_open(mt, &sc, &s)`; enums in the legacy order (legacy + 1 where 0 means "not set", same values where 0 is a real default); `mtl_flow_ipv4()`, `raster.rate` (`MTL_FPS_*`), `audio.ptime` (`MTL_PTIME_*`); a legacy field map; `MTL_PORTS` stays | Proposed default (maintainer's direction) | contract.md, migration.md | — |
| D-98 | Port first: Phases 1–6 port today's functionality plus the Kubernetes lifecycle; the NMOS contract extras, SDP, IPMX and PEP are Phase 7, declared under `MTL_LATER` | Proposed default (maintainer's direction) | implementation-plan.md | — |

### 3.2 Why: prior art

The defaults that M1, M3 and M7 rest on come from other media and I/O APIs. Details:
[archive/research/11 §10–§11](archive/research/11-media-io-prior-art.md),
[archive/research/09 §10](archive/research/09-libfabric.md),
[archive/research/10 §12](archive/research/10-rivermax.md).

| Rule | Decision | Taken from | Rejected |
|---|---|---|---|
| TX statuses `MTL_TX_ON_TIME`, `LATE`, `DROPPED`, `FLUSHED`, `FAILED`, with a reason | D-14 | DeckLink {Completed, DisplayedLate, Dropped, Flushed} | JACK's xrun callback without arguments; AJA's aggregate-only drop count |
| A results ring that cannot overflow: acquire reserves the entry | D-03 | Vulkan `QUEUE_FULL`, DeckLink `E_OUTOFMEMORY` | io_uring's overflow list (it allocates) |
| Signed `margin_ns` in every TX result | D-14 | Vulkan `presentMargin` | a boolean on-time flag |
| `mtl_tx_next_slot` names the slot being filled and its deadline | Q-CMP-4 | OpenXR `predictedDisplayTime`, CoreAudio `inOutputTime` | — |
| Validity flags on every time, one coherent clock pair (`mtl_time_cross`) | D-15 | CoreAudio `mFlags`, ALSA `actual_type` | sentinel values (NDI `INT64_MAX`, Vulkan "0 = unavailable") |
| Cumulative counters, never reset | Q-OBS-1 | WebRTC stats, OpenTelemetry cumulative temporality | SRT and ALSA reset-on-read |
| `struct_size` first in every input struct | D-74 | AJA `NTV2_HEADER`, Win32 `cbSize` | — |
| Inline status per result, no mode bits, every buffer flushed back at close, refcounted memory close | D-03, D-16, D-36 | libfabric's pain points (separate error queue, silent discard on close, undefined MR close) | — |
| Lifetime ends at an explicit result | D-01 | — | NDI's positional "valid until the next call" |
| MTL compares lateness against the deadline itself | D-13 (E3) | DPDK says past launch times "should be ignored" | trusting the NIC to report lateness |

### 3.3 Superseded

Only the replacement is in force; the original text is in
[archive/DECISIONS.md](archive/DECISIONS.md).

- D-12 (timelines with a group object and lazy anchors) by D-78.
- D-20 (stats structs, cumulative counters) by D-80.
- D-22 (exported `*_init()` per struct, zero-default rule) by D-74.
- D-25 (RTP level stays legacy-only) by D-82.
- D-28 (reuse today's `mtl_handle`) by D-44.
- D-29 (one submitting context, one reader) by D-50.
- D-35 (completion modes NONE / EXCEPTIONS / ALL) by D-77.
- D-40 (one buffer attached to several sessions) by D-55.
- D-46 (where each knob lives) by D-73.
- D-61 (EQ subscription masks, user posts) by D-79.
- D-65 (the simple layer `mtl_simple.h`) by D-72.

Partly superseded, kept above in their current form: D-72's spec strings and
`mtl_session_config_parse` by D-97; D-63's manager-optional flag by D-92; D-43's try-wait
return codes by D-88; Q-MODE-6's "SDP values only" by D-94.

## 4. Proposed defaults for the ST20 port

These are the proposed defaults that milestones M0–M6 implement or test for ST 2110-20. They
stand unless the maintainer objects in §6. Names are the header names; the full question,
options and research are under the Q-ID in [archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md).

| Q-ID | Proposed default | Where |
|---|---|---|
| Q-ARCH-2 | A typed `mtl_instance_h` over versioned `struct mtl_instance_params`; `mtl_legacy.h` bridges today's `mtl_handle` | contract.md, migration.md |
| Q-THR-3 | Automatic progress only; no manual `run_once` mode in v1 (the null backend and test clock cover testing) | engine.md |
| Q-THR-4 | Conversion runs in the caller of submit (O(frame) for converting sessions), reported per session | engine.md |
| Q-THR-5 | An in-flight counter on its own cache line plus generation-tagged handles over type-stable tables; elided for `MTL_SESSION_SINGLE_READER` sessions without `MT_SUBMIT` | engine.md |
| Q-THR-6 | Migration is ported (D-98): `instance.tx_video_migrate`, `instance.rx_video_migrate` (off, as today) and `session.migrate`, with a quiesce and acknowledge handshake | engine.md, migration.md |
| Q-THR-7, Q-THR-7a | Tasklets read a published time base (`{tsc_base, tai_base, ratio, seq}`, refreshed ≤ 100 ms, slewed) for every source, built-in PTP included; legacy keeps `ptp_get_time_fn`; spike S7 bounds the error | timing.md, engine.md |
| Q-THR-9 | TX-hang recovery and RX auto-detect run on a per-instance worker with a quiesce handshake; recovery never touches `sh_info` (SF-41) | engine.md |
| Q-THR-10 | `MTL_FLAG_TASKLET_THREAD` and `TASKLET_SLEEP` keep their meaning; the sleep path performs pending wakes; scheduler threads `rte_thread_register` | engine.md |
| Q-LIFE-1 | Stop pauses: the RX join and flow rule are kept until close or a flow update, so restart is instant | contract.md |
| Q-LIFE-2 | EAL stays alive across the last close, so open can run again (#1341); `MTL_INSTANCE_SHARED` is refcounted; a later open with differing settings is `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | contract.md |
| Q-LIFE-3 | The shared instance tears down in dependency order on its last reference; legacy `mtl_uninit` with unified objects returns `-EBUSY` | contract.md, migration.md |
| Q-LIFE-4 | Close with leases out is deferred (`mtl_session_close` returns 1, then `MTL_EVENT_SESSION_RETIRED`) | contract.md |
| Q-LIFE-5 | TX acquire and submit are accepted before start (CREATED, STOPPED, ARMED), with `preroll_ns` | contract.md, timing.md |
| Q-LIFE-6 | Sticky, async-signal-safe interrupts per session and instance (`mtl_session_interrupt`, `mtl_instance_interrupt`) | contract.md |
| Q-LIFE-7 | Single-leg link loss: the session stays RUNNING, units `MTL_TX_DROPPED` with `LINK_DOWN`, `MTL_EVENT_PORT_LINK`, resume on link up | contract.md |
| Q-LIFE-8 | One process per instance; no DPDK secondary processes | deployment.md |
| Q-LIFE-9 | Create reserves queues, quota and flow-rule capacity but never waits for ARP or IGMP; rules and joins at the first start | contract.md |
| Q-LIFE-10 | VF reset or port restart: pause and resume with `MTL_EVENT_PORT_RESET` where queues and flows can be restored (gap units dropped with `RECOVERY`; open: OI-13); otherwise ERROR with `PORT_RESET` or `DEVICE_GONE` | contract.md, engine.md |
| Q-CMP-1 | Library pools produce no results unless `MTL_SESSION_RESULTS`; application memory always; `status.blocked_on` explains every stall (`MTL_BLOCKED_BUFFERS`, `_RESULTS`, `_APP_LEASES`, `_APP_PINS`) | contract.md |
| Q-CMP-3 | `mtl_last_error(struct mtl_error_info*, size)` copies code, reason and a detail string; one frozen `enum mtl_reason` | contract.md |
| Q-CMP-4 | The slot hint (`mtl_tx_next_slot`) plus an opt-in lossy `MTL_EVENT_EPOCH_TICK` (`session.epoch_tick`), replacing `ST_EVENT_VSYNC` | contract.md |
| Q-CMP-6 | `MTL_TX_ON_TIME` is an admission verdict; a wire verdict is reported separately when NIC timestamps exist | timing.md |
| Q-CMP-7 | On idle sessions with frames in flight the transmitter calls the non-blocking `rte_eth_tx_done_cleanup`, rate-limited, dedicated queues only; a stalled queue is restarted by a worker (spikes S6, S8) | engine.md |
| Q-MEM-5 | The 8-frame cap is lifted in the engine (E11); until then `max_count = 8` is reported and a larger `pool_count` fails at query and create | contract.md, engine.md |
| Q-MEM-6 | RX gaps are zero-filled by default for library pools, in dequeue, never on the tasklet; `MTL_SESSION_RX_NO_FILL` opts out; a loss map is reported too | contract.md |
| Q-MEM-7 | `MTL_SESSION_REQUIRE_DIRECT` with too few slots fails (`POOL_TOO_SMALL`); the minimum is 2 for common formats, 3 under 512 packets per unit | contract.md |
| Q-MEM-9 | Header split is not in the unified API (see M12) | migration.md |
| Q-TIME-1 | Timed sessions without a locked TAI source run on `MTL_TIME_SOURCE_SYSTEM_TAI`, labelled estimated, with a warning event | timing.md |
| Q-TIME-2 | Clock steps: a per-timeline `step_policy` (re-anchor for created timelines, keep for the epoch); AUTO never emits a smaller index; always `MTL_EVENT_TIME_STEP` | timing.md |
| Q-TIME-3 | The horizon is configurable (`tx.horizon_ns`), 1 s from max(now, resolved start) | timing.md |
| Q-TIME-5 | `tx.index_offset` (whole units, lip-sync) and `media_time_offset_ns` replace `rtp_timestamp_delta_us`; legacy keeps it | timing.md, migration.md |
| Q-TIME-7 | The pacing engine is per port; sessions request a class and see the granted value | timing.md |
| Q-TIME-9 | FIFO submission with strictly increasing media time; no media-ordered submission in v1 | timing.md |
| Q-TIME-10 | Sent times are SW estimates first (flagged), NIC timestamps after spike S5 (`MTL_TXR_SENT_HW`) | timing.md |
| Q-TIME-13 | Rows units not published by their packet deadline: `MTL_ROWS_TRUNCATE` by default, `PAD` opt-in, `STALL` legacy only | timing.md |
| Q-TIME-14 | ST 2022-7 RX reports the tolerated path differential; `rx.skew_budget_ns` defaults to class A (10 ms) where memory allows | timing.md |
| Q-TIME-17 | Sender type W uses the linear read schedule of ST 2110-21:2022 in the unified API; legacy behind a flag (M7); `MTL_SENDER_NL` explicit | timing.md |
| Q-TIME-21 | Legacy user pacing keeps nearest-epoch in the shim; the unified equivalent is a not-before launch (TX moves by ≈ 604–619 µs at 1080p59.94) | timing.md, migration.md |
| Q-TIME-23 | The media mode sets the late policy (AUTO → RESLOT, INDEX/TAI → DROP); a CAPTURE session raises `MTL_EVENT_TIMING_INFEASIBLE` on its first late unit | timing.md |
| Q-TIME-25 | TAI snapping `MTL_SNAP_NEAREST` with a TFRAME/8 hysteresis band for every source kind; `MTL_SNAP_LOCKED_PHASE` opt-in, relock after 3 off-grid units | timing.md |
| Q-TIME-27 | RX relocks onto a restarted sender after 3 distinct increasing stale RTP values in a row | timing.md |
| Q-OBS-1 | Counters cumulative for the session's life (no reset); windowed maxima (1 s, 60 s) and histograms | contract.md |
| Q-OBS-3 | The ST 2110-21 timing parser is opt-in (`rx.timing_parser`), integer math, NIC-timestamped packets only (open: OI-29, today's parser also runs on SW time) | contract.md, timing.md |
| Q-MODE-2 | `struct mtl_flow` carries DSCP (0 = the profile's; ST 2110: CS0) and TTL (0 = 64); VLAN and IPv6 reserved | contract.md |
| Q-MODE-4 | The codec and converter plugin ABI is frozen for v1, redesigned as ABI v2 (`mtl_plugin.h`) before stage F | migration.md |
| Q-ABI-2 | The versioned instance parameters land first, before the new API | contract.md |
| Q-ABI-6 | One ABI on every OS (portable wait handles, `MTL_E*` codes); a compile-only Windows CI job; runtime support follows the legacy library | deployment.md |
| Q-ABI-7 | C11 atomics are allowed in `lib/`; public headers stay C99 and C++ clean | engine.md |
| Q-MIG-2 | About six canonical samples (TX and RX × simple, zero-copy, timed A/V) plus an event-loop example, each also run on the null backend in CI (G-97) | examples.md |

The other proposed defaults (audio, ANC, ST 2110-22, memory imports and GPUs, timecode,
groups of essences, out-of-process stats, SDP, Rivermax naming) are not on the ST20 path.
They keep their text and answer lines in [archive/OPEN-QUESTIONS.md](archive/OPEN-QUESTIONS.md);
their decisions are in §3.1.

## 5. Open issues for the implementation

Points the distillation found where two documents, or a document and a header, disagree, or
where a rule is missing. None is decided here. Where the headers were changed on 2026-10-02
the recommendation is the header's text, to be confirmed. Answer in the last column; an
answered issue becomes a D-xx row (§3.1) or a fix of the named document.

### 5.1 Instance, lifecycle, time sources

| ID | Issue | Options | Recommendation | Milestone | Your answer |
|---|---|---|---|---|---|
| OI-1 | Shared instance: archive/03 §7.2 has a merge table; `mtl_instance_open` (`mtl.h`) fails a later open with differing ports, lcores, time source or options with `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | (a) the header; (b) the merge table | (a); D-44 and Q-LIFE-2 follow it | milestone M0 | |
| OI-2 | Bridged instance before the legacy `mtl_start()`: today sessions can be created first and no tasklet runs until it (`dev/mt_dev.c:2113-2125`) | (a) create any time, start before `mtl_start()` is `-MTL_EBUSY` (`WRONG_STATE`); (b) the unified start starts it | (a); RxTxApp and KahawaiTest start it (or `MTL_FLAG_DEV_AUTO_START_STOP`) first | milestones M0, M4, M5 | |
| OI-3 | Legacy teardown: `mt_sch_mrg_uinit` releases lcores before it frees active schedulers (`mt_sch.c:1022-1030`); `mtl_uninit` with live sessions self-deadlocks (SP-01). EK1 fixes the unified order | (a) fix the legacy order too, as a bugfix (D-24); (b) unified only | (a) | milestone M0 | |
| OI-4 | EK6 removes the SysV lcore table: the legacy `mtl_lcore_shm_*` calls, `doc/shm_lcore.md`, `docker/README.md` (`ipc: host`, `/tmp/kahawai_lcore.lock`) and the `mtl_lcore_shm` tool lose it | (a) the calls report nothing and go at the freeze; docs move to `MTL_CPUARB_LOCKS` with a shared `instance.runtime_dir`; (b) keep the table for legacy until the freeze | (a), with M16 | milestone M0 | |
| OI-5 | `MTL_TIME_SOURCE_AUTO` before Phase 7: some prose said FREERUN; the header says a disciplined NIC PHC, else `CLOCK_TAI`, else `SYSTEM_TAI`, chosen once at open | (a) the header; (b) FREERUN in Phases 1–6 | (a); D-85 follows it | milestone M0 | |
| OI-6 | `MTL_TIME_SOURCE_PTP_BUILTIN` on a VF: one text refused it (`CLOCK_NOT_OWNED`); today it runs in software mode (`mt_ptp.c:1390-1393`) for RxTxApp `--ptp` and the nightly pytest `ptp` group | (a) as today: disciplines MTL's own time base, never the VF's PHC (header); (b) refuse, as a listed behaviour change | (a) (port first, D-98) | milestones M4, M6 | |
| OI-7 | Which configurations are "a PHC MTL does not own" (`CLOCK_NOT_OWNED`, 614): on a PF today's client always steers the PHC, and MTL cannot see a node ptp4l on it ([deployment.md §4.8](deployment.md#48-time-in-a-pod)) | (a) `PTP_BUILTIN` with `time.phc_trust` = `MTL_PHC_TRUST_YES`; (b) never on a PF | (a) | milestone M0 | |
| OI-8 | Phase of `mtl_time_set_reference()`: nmos-ipmx.md said Phase 7; pods need it to report a lost grandmaster | (a) Phases 1–2, with the pod time rules (header list of 2026-10-02); (b) Phase 7 | (a) | milestones M0–M3 | |
| OI-9 | The waker's `SCHED_FIFO` priority had no option key (archive/15 §4); the header now has `MTL_OPT_WAKER_PRIORITY` (`instance.waker_priority`, 1–99, needs `CAP_SYS_NICE`) | (a) keep the key, failure reported, never fatal; (b) drop the promise | (a) | milestone M1 | |
| OI-10 | Code of a missed control-plane deadline: prose said `-MTL_ETIMEDOUT` for "stop, close"; `mtl_session_close` returns 0 or 1, `mtl_instance_close` 0, 1 or `-MTL_EIO` | (a) only a DRAIN stop that missed its deadline (header); (b) close too | (a) | milestone M1 | |
| OI-11 | Generated session names: archive/09 and contract.md `<essence>_<tx or rx>_<n>`, archive/08 `<essence>-<dir>-<n>` | (a) underscores; (b) hyphens | (a); state it at `mtl_session_config.name` | milestones M1, M3 (G-88) | |
| OI-12 | Codes per call: the header gives `-MTL_EBADF` for a closed handle and R4's rule, not the state × call table (contract.md §4.2; an update in DRAINING or FLUSHING is `-MTL_EBUSY`) or the code matrix of archive/07 §5.3 | (a) each function's comment lists its codes; (b) contract.md is normative, the header points there | (b), with a G-49 test from the table | milestone M1 | |
| OI-13 | Gap units of a recovered port reset: contract.md (from archive/16) `MTL_TX_FAILED`, `PORT_RESET`; archive/03 and Q-LIFE-10 `MTL_TX_DROPPED`, `RECOVERY` | (a) FAILED / `PORT_RESET`; (b) DROPPED / `RECOVERY` | (a); then fix Q-LIFE-10 | milestones M3, M5 | |

### 5.2 Data path, results, memory

| ID | Issue | Options | Recommendation | Milestone | Your answer |
|---|---|---|---|---|---|
| OI-14 | DP and syscalls: R6 said "no syscall", but a data call drains the armed wait handle with a `read()`; the header now allows exactly that non-blocking read (R2) | (a) the header; busy-loop threads never make it; (b) drain on the waker only | (a); debug-build syscall checks allow that read | milestone M1 | |
| OI-15 | Failed submit of a packet unit (`PKT_COUNT`) or codestream (`CODESTREAM_OVERSIZE`): S8 and archive G-66 keep the lease with the caller; `mtl_tx_submit` (D-88) returns the slot to the pool | (a) the header, restate G-66 and S8; (b) keep the lease for these two | (a) | none (2P, ST 2110-22) | |
| OI-16 | Guarantees in revision-3 terms (archive/13): G-02 (lease kept after a rejected submit), G-11 (`mtl_mem_destroy` `-EBUSY`), G-70 (`-MTL_ETIMEDOUT`, `call_seq`), G-72 (CQ/EQ readers), G-77 (instance release, EQs), G-87 (reconfigure, groups) | (a) restate each in revision-4 terms in implementation-plan.md §8.2; (b) drop and replace | (a) | milestones M0, M1, M3 | |
| OI-17 | `MTL_SESSION_EXPORT_POOL` implies only `MTL_SESSION_RESULTS` (`mtl.h`); D-50 and archive/11 §6.2 say exported pools need `MTL_SESSION_MT_SUBMIT` | (a) it implies MT_SUBMIT too; (b) create without MT_SUBMIT fails `-MTL_EINVAL`; (c) neither, fix D-50 | (a); state it in the header | none (framework pools) | |
| OI-18 | Results ring full: archive/04 §4.1 keeps a done slot until a result entry frees; the header sizes the ring at `pool_count` and reserves an entry at acquire, so a result never waits | (a) the header (G-04); (b) archive/04 | (a); restate engine.md | milestone M3 | |
| OI-19 | Latency fields: archive/04 puts the expected completion and wake latency (`expected_wake_latency_ns`, from S1) in session info; only `mtl_buffer_requirements.completion_latency_ns` exists | (a) add both to `struct mtl_session_info`; (b) wake latency as an `info.*` stats key | (b), keeping the info size | milestone M3 | |
| OI-20 | Moving-cursor TX (D-56): no header has revision 3's `MTL_POOL_DYNAMIC` or `mtl_tx_acquire_dynamic`; `mtl_tx_acquire_layout` is under `MTL_LATER`; early source release has no symbol | (a) `mtl_tx_acquire_layout` replaces the dynamic pool, early release added in Phase 4; (b) restore the dynamic pool | (a) | none (Phase 4) | |
| OI-21 | Import access: deployment.md §1.1 says the mapping uses the matching direction; DPDK maps every region read-write (DPDK 26.07 `lib/eal/linux/eal_vfio.c:1442-1443`) | (a) `MTL_MEM_READ`/`MTL_MEM_WRITE` are a contract check at attach (`ACCESS_MISMATCH`), a read-only CPU mapping is copy only; (b) direction-limited mappings (a DPDK patch) | (a) for v1 | none (Phase 4) | |
| OI-22 | Imports in PA mode (`instance.allow_noiommu`): `mtl_mem_import` and DMA behaviour unstated | (a) copy only (`direct` 0), `-MTL_ENOTSUP` with `MTL_SESSION_REQUIRE_DIRECT`; (b) import fails `-MTL_ENOTSUP` | (a) | none (Phase 4) | |
| OI-23 | AF_XDP queue rate in pods: the library writes sysfs `tx_maxrate` today (`dev/mt_af_xdp.c:867-874`); a pod's `/sys` is read-only ([deployment.md §4.13](deployment.md#413-af_xdp-in-pods)) | (a) the node agent that hands over `port.xsk_map` sets and resets it (a rate request in its protocol); (b) software pacing only in unprivileged pods | (a) for MtlManager, (b) for other agents | none | |

### 5.3 RX

| ID | Issue | Options | Recommendation | Milestone | Your answer |
|---|---|---|---|---|---|
| OI-24 | Discard and ready RX units: contract.md §4.5 keeps them dequeuable; archive/03 §3.4 discards them; `mtl_session_discard` is silent | (a) they stay; (b) discarded and counted (`rx.units_flushed`), so the first dequeue after a seek is a new unit; stop and ERROR keep them | (b), for framework flushes (GStreamer FLUSH_STOP) | milestones M2, M4 | |
| OI-25 | Zero fill of attached RX pools: header and contract.md cover library pools; archive/05 §8 leaves attached pools unfilled; S4 P8 fills every pool | (a) library pools only, attached pools use `mtl_rx_get_missing()`; (b) every pool, `MTL_SESSION_RX_NO_FILL` opts out | (a) (D-37); align the `MTL_SESSION_RX_NO_FILL` comment | milestone M2 | |
| OI-26 | Source of missing ranges: the packet bitmap belongs to the reassembly slot and is cleared at reuse (`st_rx_video_session.c:1298-1299`), so zero fill and `mtl_rx_get_missing()` have no data | (a) copy the bitmap into the lease table at the RX hook (≈ 540 B at 1080p); (b) zero-fill the whole frame before reuse | (a); until then `mtl_rx_get_missing()` is `-MTL_ENOTSUP` | milestone M2 | |
| OI-27 | `rx.threads` = 2 with two legs or rows units: today's engine rejects it (`st_rx_video_session.c:2642-2668`); the header now says so | (a) an explicit 2 fails `-MTL_ENOTSUP`, auto stays 1; (b) silently 1 | (a) (no silent downgrade, D-19) | milestone M2 | |
| OI-28 | Per-packet conversion: `rx.convert_per_packet` (`ST20P_RX_FLAG_PKT_CONVERT`) runs library code on the RX tasklet and disables DMA; contract.md said conversion is never on a tasklet | (a) the stated exception (header); (b) not ported, its twin goes to the M12 list | (a) | milestones M2, M5 | |
| OI-29 | Timing parser without NIC timestamps (Q-OBS-3 says NIC-timestamped packets only); today it uses SW time on VFs and counts in-burst packets as untrusted (`st_rx_video_session.c:1546-1557`) | (a) as today, flagged ESTIMATED, in `tp.untrusted_pkts`; (b) NIC only, a listed behaviour change | (a) (port first) | milestones M2, M5 | |

### 5.4 Timing

| ID | Issue | Options | Recommendation | Milestone | Your answer |
|---|---|---|---|---|---|
| OI-30 | Rounding of `mtl_index_at` / `mtl_epoch_index_at`: the header rounds down (the unit containing t; the first at or after is k or k + 1); archive/06 §10.7, §15 and `st_timeline_index_at` used ceil | (a) the header, the Phase 0.5 helper alike; (b) ceil | (a) | milestone M3 (M3b) | |
| OI-31 | `MTL_STEP_DEFAULT` keeps media times on the epoch and re-anchors every created timeline; archive/06 keeps timelines anchored at a TAI instant | (a) the header; (b) TAI-anchored timelines keep | (a), revisited with created timelines | none (Phase 3) | |
| OI-32 | Audio packet time that is not whole samples: `MTL_PTIME_80US` at 48 kHz is 3.84 samples; today packets go every 80 µs (`st_fmt.c:1111-1112`) with 4 samples each (`:1172-1173`) | (a) reject the pair (`-MTL_EINVAL`); (b) 4 samples paced at 4 / Fs, granted ptime reported; (c) today's behaviour | (b), so RTP and pacing agree (D-41) | none (audio) | |
| OI-33 | Source kind of a processor: ex10 uses `MTL_SOURCE_GATEWAY` with `min_tx_delay_ns` = the budget; archive/06 §10.8 uses CAPTURE; the header calls GATEWAY "line by line: SDI to IP" | (a) GATEWAY; (b) CAPTURE (late units raise `MTL_EVENT_TIMING_INFEASIBLE`) | (b); applied in ex10 and timing.md (object to reverse) | none | |
| OI-34 | Archive/06 statuses missing from the headers: RELOCKED, REPEATED, TIMELINE_CONFIG_MISMATCH, RX_TIMELINE_LAZY, JTNM_DEFAULT_WINDOW_EXCEEDED, an RX_DISCONTINUITY event; DISCONTINUITY and NON_COMPLIANT exist only as submit, unit and info flags | (a) add each with the phase of its feature; (b) drop them | (a); check RELOCKED for RX relock (Q-TIME-27) | milestone M3 | |
| OI-35 | G-86, a non-aligned audio forward gap: archive/13 counts the skipped samples in `samples_dropped`; timing.md §8 pads with silence (`samples_padded`) | (a) pad gaps, drop overlaps (timing.md); (b) archive/13 | (a); restate G-86 | none (audio) | |

### 5.5 Headers

| ID | Issue | Options | Recommendation | Milestone | Your answer |
|---|---|---|---|---|---|
| OI-36 | Generator source of `mtl_reasons.h` and `mtl_options.h`: they named revision-3 tables in archive/07 §5.4 and archive/09 §9 | (a) the tables in contract.md §8.3 and §12 are the source; (b) the headers are the source and the tables are generated from them | (a), applied: the header comments now name contract.md | milestone M0 | |
| OI-37 | Review leftovers RV-49, RV-50: `MTL_US`/`MTL_MS`/`MTL_SEC` take integers (`MTL_MS(1.5)` truncates); `mtl_mem_alloc`'s `void** va` is not `MTL_ADDR`; opposite defaults of `MTL_SESSION_MT_SUBMIT` and `MTL_SESSION_SINGLE_READER` | (a) keep all three, documented; (b) change them | `MTL_ADDR` for `va`; keep the macros and the defaults | milestone M0 | |
| OI-38 | Error codes of the environment reasons 600–614: the header names the reasons but not the code each returns from `mtl_instance_open` (contract.md §8.4 proposes them, and `-MTL_EINVAL` for `PORT_NOT_OPEN`) | (a) contract.md's table; (b) one code for all (`-MTL_ENODEV` or `-MTL_EINVAL`) | (a), and fix them in the header comments | milestone M0 | |
| OI-39 | `MTL_SUBMIT_NOT_BEFORE` / `MTL_SUBMIT_EXACT` on audio and fastmeta before those essences get launch control | (a) `-MTL_ENOTSUP` until their phase; (b) ignored | (a) | none (Phase 3) | |
| OI-40 | Legacy counters with no key (U-402): `stat_epoch_onward`, `stat_error_user_timestamp`, the per-leg `build`, `frames` and `err_packets`, about 20 `st20_rx` fields ([migration.md §4.14](migration.md)) | (a) a key each; (b) "no key", listed | (b) for debug-only counters, (a) for those RxTxApp or the gtests read | milestone M3 (M3a) | |
| OI-41 | Q-MEM-12, the IOVA of an imported non-hugepage region: D-16 records it as answered, engine.md keeps it open (MTL's invented range from `0x10000`, `mt_dma.c:21`, or IOVA = VA) | (a) IOVA = VA where the IOMMU allows; (b) the invented range | (a), restate D-16 | none (Phase 4) | |
| OI-42 | Small legacy helpers with no unified home: U-115 a general fast copy, U-348 pixel-group bit layouts, R-7 a session-level cookie ([migration.md](migration.md)) | (a) homes in `mtl_util.h` / `mtl_format.h`; (b) M12 removal candidates | (b) for U-115, (a) for U-348, R-7 as a result cookie only | none | |
| OI-43 | OBS passes `lcores` and queue counts per source, FFmpeg sets migrate and separate-lcore flags: a second element's shared open then fails with `INSTANCE_MISMATCH` | (a) the plugins leave them zero; (b) these settings become session keys or a mergeable class | (b) for queue counts and migrate (session keys exist), (a) for `lcores` | Phase 6 (plugin rewrites) | |
| OI-44 | Rounding of nanosecond ties in the epoch math (Q-TIME-15): the code rounds ties down (`st_muldiv_u64_round_closest`); the design wanted the later ns | (a) pin today's behaviour; (b) round ties up | (a), pinned by a unit test in the legacy gate | milestone M3 (M3b) | |
| OI-45 | TX hang detection for ANC and fastmeta: today none (no `fatal_error` in those sessions) | (a) the video path's detection for them; (b) none, documented | (a) | none (Phase 5) | |
| OI-46 | Where the null backend lives: an L2 adapter (`lib/src/unified/adapters/null.c`) or a device in `lib/src/dev/` | (a) the adapter: no engine change, enough for the U tier; (b) a device: also exercises the engine | (a) for milestone M0, (b) later for UB tests | milestone M0 | |
| OI-47 | `MTL_PORTS` grammar (contract.md §2.1, proposed): gateways, IPv6, interface names, `env:` ports with `=ip/prefix`, and what happens when `p` names ports too | (a) the proposed grammar; (b) a different one | (a) | milestone M0 | |

## 6. Objections to proposed defaults

To object to a proposed default, name its ID (D-xx or Q-xxx-n) and say what should change.
The designer then turns it into a maintainer decision (M18 and up) or changes the design.

**Your answer:**
