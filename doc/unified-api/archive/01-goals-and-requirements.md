# 01 — Goals, personas and requirements

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5); nothing here is decided |
| Date | 2026-09-30 |
| Baseline | `main` @ `545a266a`; PR #1610 @ `14a1f80c`; PR #1770 (open) @ `74b9991d` |
| Inputs | [research/](research/) notes 00–13; reviews C1–C5; [C5 response](reviews/C5-response.md). Each requirement cites the note that motivates it as `[Rnn §x]` |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

This document states *what* the new API must achieve and *for whom*. The rest of the
document set describes *how*. Every requirement has an ID so the design, the contract
tests ([13-guarantees-and-tests.md](13-guarantees-and-tests.md)) and the open questions
([OPEN-QUESTIONS.md](OPEN-QUESTIONS.md)) can point at it. Items changed in revision 3 are
marked **r3**.

## 1. Why a new API at all

**r3.** Revision 2 opened with "475 functions, ~207 flag bits, 4 exchange models" as the
first reason for the work. Review C5 showed that count mostly describes surface the plan
keeps, and that the plan grows the public surface during the transition `[C5 §1.2]`. The
count is therefore no longer a premise. §1.1 counts the surface honestly, and the case
rests on the four facts below plus the named open issues.

Four facts from the research drive this effort. Each is verified in code with
`path:line` evidence in the cited note.

1. **An application cannot learn what happened to a frame.** TX "done" means "the NIC
   released an mbuf" (or "the frame was copied"), not "sent on time". Late reporting uses
   three different units and names no frame. Recovery reports lost frames as `COMPLETE`.
   Silent downgrades (RL→TSC, zero-copy→copy, DMA→CPU) are visible only in logs.
   `[R02 §5, R04 §6, R05 §9, R06 §2]`
2. **Timing is the largest source of external bug reports.** One `timestamp` field
   drives both pacing and RTP. Default video RTP is the packet-0 TX time, so MTL's own
   ST20 and ST40 streams disagree for the same frame. The default clock is UTC labelled
   TAI. Invalid user times are counted and then silently re-interpreted. Open issue
   #1653 ("unexpected variable latency in interlaced mode") is in this class.
   `[R05 §0, R08 §3.2, R12 §10.7]`
3. **External memory has no object behind it.** `mtl_dma_map` maps only port P, keeps
   no reference count, and nothing in the data path consults it. Only ST20/ST22 video
   supports external frames. The "safe to reuse" point differs by path. `[R04 §0, §6]`
4. **Lifecycle, threading and ABI are fragile.** No per-session start/stop. Create returns
   `NULL` without a reason. `get_frame` returns `NULL` for timeout, stop and destroy alike.
   No soname, no symbol versioning, no struct versioning, and fields have already moved
   silently. Tasklets take a `pthread_mutex` on pinned cores when BLOCK_GET is set, and
   application threads contend for locks the tasklets take. The open issues in this class:
   #1622 (UHD performance / lock contention), #1620 (existing audio/ANC sessions lose
   packets when another stream connects or disconnects) and #1341 (`mtl_init` fails after
   `mtl_uninit`). `[R02 §4, R03, R13 §1–4, R08 §2]`

A fifth fact is real but secondary: features are asymmetric by accident. ST22 has no
stats, ST30P has no user timestamp, only video emits events, and ST41 has no frame RX
`[R02 §2–3, R07 §3]`. The asymmetry is a symptom of four exchange models, and part of it
can be fixed on the legacy API ([02 §2.3](02-architecture.md)).

PR #1610 set out to fix the asymmetry with a unified session. The maintainer's review and
the PR analysis show the direction is right, but the buffer, memory, completion, timing
and lifecycle contract must be redesigned before anything is stabilised
`[R00, R01 §0]`. Most of facts 1–4 are engine problems, so the engine fixes come first and
reach legacy users too ([14 §2](14-implementation-roadmap.md)). The unified API is
justified only by what the legacy API cannot reach ([02 §2.3](02-architecture.md)).

### 1.1 The public surface, counted honestly (r3)

Counted at `545a266a` with a grep over `include/` `[C5 §1.2]`, re-run for this revision:

| Surface | Today | Unified API (sketch) |
|---|---|---|
| Public functions, all headers | 475 | 217 new, as counted by `sketch/check.sh`: 203 in `mtl_unified.h` (of which 37 are `*_init()` initialisers and 23 exported twins of the inline null/equality helpers), 10 in `mtl_simple.h`, 4 in `mtl_debug.h`; by call class 87 CP, 77 DP, 4 DPC, 12 WT, 37 AS; 5 more are reserved for later (`MTL_UNIFIED_LATER`); revision 2's prose sketch had ≈ 101 |
| of which colour-conversion helpers (`st_convert_api.h`, `st_convert_internal.h`) | ≈154 | 0 (conversion is a session property, not a function family) |
| of which pipeline verbs (`st20p_`, `st22p_`, `st30p_`, `st40p_`) | 104 (34 + 24 + 23 + 26) | — |
| Distinct `*FLAG*` `#define`s | 75 (the "207 bits" of revision 2 counted enumerators) | a few flag words; most choices are typed enum fields |
| Frame-exchange models | 4 (session callbacks, pipeline get/put, RTP level, slice) | 1 (lease + result), plus the L4 simple layer over it |
| Decisions exposed per video TX session | ≈44 (`st20p_tx_ops` ≈29 fields + ≈15 flag bits) | ≈90 typed knobs, nearly all with a zero default |

Stated plainly:

- the **name** surface per essence drops: five essences share one verb set, but the
  unified header alone is about half the size of today's whole surface once initialisers,
  typed getters and handle helpers are counted;
- the number of **decisions per session** roughly doubles, because choices move from
  flag bits into typed fields. Usability rests on the zero-default rule and the defaults
  table ([09](09-media-modes-and-backends.md)), and on the L4 simple layer for P1;
- the **total** public surface grows during the transition, because legacy APIs stay
  (D-24) until the deprecation policy removes them
  ([14 §7](14-implementation-roadmap.md)).

## 2. Personas

The API must serve all of these. Each persona has a primary requirement that must be
excellent, not merely possible.

| ID | Persona | Today uses | Must be excellent at |
|---|---|---|---|
| P1 | **Simple generator / player** (sample apps, test tools) | st20p get/put, blocking | 10-line TX/RX loop through the L4 simple layer (`mtl_simple.h`, r3); library allocates everything; sensible defaults |
| P2 | **Framework integrator** (FFmpeg, GStreamer, OBS, Python, Rust) | pipeline + blocking get + copy | Interruptible waits, portable wait object, buffer import/export, clean errors, latency report, caps query before create `[R08 §4]` |
| P3 | **Broadcast playout** (file → ST 2110, A/V/ANC together) | user timestamp/pacing, 2022-7 | Exact RTP for every essence from one timeline, group start, deterministic late policy `[R05 §6, R12 §10.6]` |
| P4 | **Live capture / contribution** (camera, SDI→IP, low latency) | ext frames, slice mode, callbacks | Latency visibility, "next slot" hint, progressive (line) submission, repeat/drop policy `[R08 §3.3]` |
| P5 | **Zero-copy forwarder / GPU pipeline** (MXL bridge, split-forward) | `query_ext_frame`, `put_ext_frame`, DMA map | Imported memory with defined lifetime, RX lending with out-of-order release and fan-out holds, direct-path guarantee `[R08 §1.2 MXL]` |
| P6 | **Rivermax migrant** | `rmx_output_media_*`, `rmx_input_*` | App-driven loop (no library callbacks), acquire/commit with time, chunk≈slice, memory regions, non-blocking backpressure statuses `[R10 §12]` |
| P7 | **libfabric-familiar developer** | `fi_mr`, `fi_cq`, `fi_eq`, `fi_getinfo` | Handles + CQ/EQ + context cookie + requested-vs-granted capabilities + `-MTL_EAGAIN` `[R09 §11]` |
| P8 | **Validation / compliance** (KahawaiTest, pytest, EBU LIST users) | everything | Observable scheduled vs actual times, per-unit status, stats that do not lie, exact arithmetic, a NIC-less backend and fault injection (r3) `[R12 §3.4, C5 §3.3]` |
| P9 | **Operator / NMOS integrator** (r3) | `update_destination`, stats dump | Atomic multi-leg activation at a TAI instant, leg admin state, stable session names, enumeration, capacity query `[C5 §8.1–8.2, §8.7, §8.11]` |

P1–P8 come from code archaeology, not interviews `[C5 §1.9]`. The Phase 0 exit
criterion "design review with external consumers" ([14 §1](14-implementation-roadmap.md))
is where they are validated.

## 3. Goals

| ID | Goal |
|---|---|
| GO-1 | **One session model** for ST 2110-20/-22/-30/-40/-41, TX and RX. Media-specific configuration exists only at creation; runtime verbs are media-polymorphic. |
| GO-2 | **Familiar shape.** App-driven *acquire → fill → submit → completion* on TX and *dequeue → read → release* on RX, completion and event queues, memory regions, a context cookie per operation, capability query with requested-vs-granted reporting. Recognisable to libfabric and Rivermax users without copying their baggage. |
| GO-3 | **One buffer contract, many provisioning paths.** Library pool, imported application memory and (later) device memory become the same buffer handle with the same lifecycle. Allocation origin never implies a data path. `[R00 §2]` |
| GO-4 | **Timing that is correct by construction.** Media time and launch time are separate. RTP is derived from media time only. One exact rational timeline can be shared by several essences, on TX and on RX (r3). Late handling is an explicit policy with a per-unit result, never a silent fallback. `[R05 §10, R12 §10]` |
| GO-5 | **Nothing happens silently.** Every accepted unit gets one terminal result with status, reason and timing. Every downgrade is requested-vs-granted. Every state change is an event with a matching state getter. `[R06 §5]` |
| GO-6 | **Real-time safety.** No public API call ever runs on a pinned tasklet core. Tasklets never block, never allocate, never take a lock an application thread can hold, and by default never run application code. `[R03]` |
| GO-7 | **Robust lifecycle.** Explicit per-session states, drain vs flush, interruptible waits, stale-handle safety, no callbacks or memory access after destroy returns, errors reported as codes. `[R13 §6]` |
| GO-8 | **Evolvable ABI.** Versioned structs, opaque handles, soname and symbol versioning, an experimental tier. `[R13 §7]` |
| GO-9 | **Incremental delivery, engines first (r3).** The first implementation reuses today's packet builders and RX reassembly. Engine fixes land first and reach legacy users; bugfixes are on by default, wire-visible changes are opt-in on the legacy API. Legacy APIs keep working during the transition. `[R01 §6, R08 §5, C5 §1.4]` |

## 4. Non-goals for the first version

These are deliberately out of v1. Several are "reserve the API shape now, implement
later" so they can be added without an ABI break. Each has an open question if the
maintainer may disagree.

| ID | Non-goal | Why | Question |
|---|---|---|---|
| NG1 | **r3, rewritten:** replacing the **packet builders and RX reassembly** — they are kept. Pacing admission, RTP derivation, the time source and the completion plumbing **are** re-plumbed (E1–E13, [06 §14](06-timing-pacing-and-sync.md)); see the note below the table | revision 2's "not replacing the pacing engines" was not honest given E1–E10 `[C5 §1.4]` | — |
| NG2 | App-built RTP packets (today's `*_TYPE_RTP_LEVEL`) inside the unified API | Different ownership model (mbufs); the only route to ST 2022-6; legacy API keeps it | Q-MODE-1 |
| NG3 | SDP parsing inside `lib/` | Parser edge cases; NMOS users already have tooling. `mtl_session_get_info` returns every SDP-relevant value ([08 §3.1](08-observability.md)) | Q-MODE-6 |
| NG4 | DMA-BUF, CUDA, Level Zero *direct* NIC access | No such path exists today `[R04 §5]`; GPU pinned *host* memory is a supported import (r3) | Q-MEM-8 |
| NG5 | Multi-process session sharing / DPDK secondary processes | EAL runs `--in-memory` `[R07 §5.2]`; cross-process sync is by the epoch timeline ([06](06-timing-pacing-and-sync.md)) | Q-LIFE-8 |
| NG6 | Header split | Compiled out on the pinned DPDK `[R04 §5]` | Q-MEM-9 |
| NG7 | Library-side scheduled RX delivery ("present at T") | Report presentation time first; scheduling is later | Q-TIME-12 |
| NG8 | **r3:** runtime `mtl_port_open` | Needs `struct mtl_port_params` and a list of subsystems it brings up; moved to Phase 6 ([03 §7](03-object-model-and-lifecycle.md)) | — |

**NG1 and legacy users (r3).** Per engine change, [14 §2](14-implementation-roadmap.md)
states whether legacy users get it: bugfixes default on; wire-visible changes (RTP ±1
tick, default video RTP, ANC RTP, ST22 CBR, the linear read schedule) default off on the
legacy API behind a new opt-in flag, and on in the unified API.

## 5. Requirements

Levels: **MUST** — v1 blocker. **SHOULD** — v1 unless it costs a phase. **MAY** — reserve
the shape, implement later. IDs are stable; new ones are appended.

### 5.1 Object model and verbs

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-OBJ-1 | MUST | One opaque session type for all essences and directions; media-specific create/config only. | R00 exec, R01 §2 #1 |
| R-OBJ-2 | MUST | Direction-specific verbs: TX `acquire/submit/release/withdraw`, RX `dequeue/release` (r3 names). No verb means opposite ownership transitions in the two directions. | R00 §13, C5 §2.17 |
| R-OBJ-3 | MUST | A buffer is an opaque handle with an immutable layout (planes, offsets, spans, strides). Per-use information (media time, cookie, overrides) lives in a separate submission, never in the buffer. | R00 §7 |
| R-OBJ-4 | MUST | Every submission and every RX delivery carries a 64-bit user cookie returned verbatim in its result. | R09 §4.2, R11 §10 #6 |
| R-OBJ-5 | MUST | Handles (instance, session, region, queue, timeline, group, buffer, lease) are validated without dereferencing freed memory; stale, foreign and duplicate handles fail with an error; ID 0 is the null handle of every type (r3). A lease has its own C type. | R13 R-L5, R00 §14 #7, C5 §2.5, §2.8 |
| R-OBJ-6 | SHOULD | Completion queues and event queues are objects that several sessions may share, so one application thread can serve many sessions; EQs take a subscription mask (r3). | R09 §5, C5 §7.5 |

### 5.2 Memory and buffers

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-MEM-1 | MUST | The library-allocated pool is the default and needs no memory knowledge from the app (NUMA, hugepages, IOVA). | R00 §3 |
| R-MEM-2 | MUST | Application memory is imported once as a retained *memory region*; buffers reference `{region, offset}`, never raw IOVA in the ordinary contract. Import requires page-aligned `va`/`length` (hugepage-aligned for hugetlbfs), so no neighbouring memory is ever mapped for DMA (r3). | R00 §7, R04 §7, C5 §6.5 |
| R-MEM-3 | MUST | A region outlives every buffer, conversion, packet and DMA reference; destroy returns `-MTL_EBUSY` while referenced. | R00 §14 #9–10 |
| R-MEM-4 | MUST | A region import maps into every device on the session's path (both 2022-7 ports and the DMA engine) or fails. | R04 §4.2 |
| R-MEM-5 | MUST | Data-path policy `REQUIRE_DIRECT / PREFER_DIRECT / ALLOW_COPY` is independent of allocation origin; the selected path is queryable per session and reported per unit when it can vary. `stride ≥ row_bytes` is DIRECT-capable (r3). | R00 §10, R04 §6 #4, C5 §6.1 |
| R-MEM-6 | MUST | The first implementation supports fixed pools attached before start (library-filled or imported). | R00 §8.4 |
| R-MEM-7 | SHOULD | Imported buffers for every essence (ST30/40/41 are copy-only by construction, which is fine). | R04 §7 #10 |
| R-MEM-8 | SHOULD | RX buffers may be held by the app across threads and released in any order; one RX lease may be held by N TX submissions (hold count, r3); the pool-exhaustion policy is explicit. | R08 §4, Q4, C5 §6.2 |
| R-MEM-9 | MAY | Mixed-origin pools; device memory domains. (Dynamic per-acquire layouts, `MTL_POOL_DYNAMIC`, move to Phase 4, r3.) | R00 §8.3, C5 §6.3 |

### 5.3 Timing and synchronisation

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-TIME-1 | MUST | Every timestamp field names one clock, unit, epoch and measurement point; validity is carried by flags, never by zero. Inside CQ records every time is TAI nanoseconds or invalid (r3). | R00 §14 #15–16, R11 §10 #3, C5 §2.1 |
| R-TIME-2 | MUST | Media time (what the unit represents) and launch time (when packets leave) are separate inputs; launch is derived from media time and the session's declared source kind (playback, capture, gateway) unless explicitly overridden. | R05 §10 #1, R12 §10.2, C2 #1 |
| R-TIME-3 | MUST | RTP = `floor(media_time × clock_rate) mod 2^32`, offset zero, exact integer/rational arithmetic, one rule for all essences and for both fields of an interlaced frame (r3). | R12 §1.3, §10.3, C5 §5.3 |
| R-TIME-4 | MUST | Default media time for generated video is the frame epoch `N × TFRAME`, not the TX cursor. | R12 Q1, R05 Q1 |
| R-TIME-5 | MUST | Late and underrun handling are explicit per-session policies producing per-unit outcomes; no invalid request silently changes pacing mode; essences with keep-alive rules (ST 2110-40, -41) keep them by default. | R05 §2.5, R00 §11, C2 #8 |
| R-TIME-6 | MUST | The instance exposes its time source, lock state and offset, and emits events on lock change and clock step. | R06 Q7, R05 §9 F1–F2 |
| R-TIME-7 | SHOULD | A timeline object with an exact rational anchor that several sessions share, plus an atomic group start, so video/audio/ANC timestamped from one file are aligned exactly. T0 is on the common grid for every start index (r3). | R12 §10.6, C5 §5.1 |
| R-TIME-8 | SHOULD | Audio submission is sample-accurate (a submission identifies its first sample; buffer boundaries need not match video frames). | R05 §5, R12 §8.2 |
| R-TIME-9 | SHOULD | `acquire` reports the next slot the buffer would occupy and the latest submit time that still meets it. | R11 §10 #8, R08 §3.3 |
| R-TIME-10 | SHOULD | RX reports unwrapped media time, per-leg first/last arrival, and an optional configured link offset (presentation time). | R12 §9, R05 Q14 |
| R-TIME-11 | MAY | Progressive (line) submission within one timed frame/field; RX progressive delivery. | R00 §12, R07 §7 #1 |
| R-TIME-12 | SHOULD | **r3.** RX sessions bind a timeline (EPOCH or AT_TAI anchor), join groups, and report `media_index`; a receiver can align video, audio and ANC units without app arithmetic. | C5 §5.7 |
| R-TIME-13 | SHOULD | **r3.** Separate processes (one per essence) share a timeline without IPC through the epoch timeline plus an index helper. | C5 §5.6 |

### 5.4 Completion, events and errors

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-CMP-1 | MUST | Every accepted TX submission produces exactly one terminal outcome — a CQ result, or a counter increment when the session's completion mode suppresses that result; a rejected submission produces none and leaves ownership with the caller. | R00 §9, C4 C-32 |
| R-CMP-2 | MUST | Results cannot be lost to queue overflow; they are materialised by the reader from the lease table, and unread results are bounded by the pool size. | R00 §4.4, R11 §10 #4, C1 #1 |
| R-CMP-3 | MUST | TX terminal status distinguishes on-time, late-but-sent, dropped (with reason), flushed and failed; `*_UNSET = 0` is never a terminal status (r3). | R11 §1.2, §10 #1, C5 §2.20 |
| R-CMP-4 | MUST | A "reusable" result means no converter, encoder, packet reference, DMA descriptor or NIC can still access the storage. | R00 §9 #5, R04 §2.1 |
| R-CMP-5 | MUST | Informational events (state changes, loss notices, detection) go to a separate bounded event queue with coalescing, overflow counting and a state getter for each *state* event kind (notices exempt, 07 §3.2). | R06 §5, Q5 |
| R-CMP-6 | MUST | **r3.** One error vocabulary: `MTL_E*` constants with fixed values (Linux errno where Linux has it), one meaning per code — `-MTL_ESHUTDOWN` only when the app stopped or destroys the session, `-MTL_EIO` for ERROR, `-MTL_ENODEV` for a device gone (07 §5). Create returns a code; `mtl_last_error` copies the reason into caller memory. | R13 R-L10, C5 §2.13, §2.16, §8.3 |
| R-CMP-7 | SHOULD | Every queue exposes a portable wait object (fd on Linux, `HANDLE` on Windows) plus a race-free try-wait whose return is 1 = ready, 0 = armed, < 0 = error (r3). | R09 §5.2, R08 Q2, C5 §2.10 |

### 5.5 Observability

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-OBS-1 | MUST | One stats schema for all essences: counters cumulative for the life of the session (no epochs, no reset, r3), separate gauges, snapshot reads that never take a lock the tasklet uses. | R06 §5, R11 §10 #12, C5 §8.8 |
| R-OBS-2 | MUST | Queue-depth gauges (per buffer state) in count and in time, from a single-pass scan of the slot states (r3). | R11 §10 #7, C5 §8.9 |
| R-OBS-3 | MUST | PTP, link, redundancy-leg (admin and oper) and scheduler status getters; each has a change event. | R06 Q6–7, C5 §8.2 |
| R-OBS-4 | SHOULD | Lateness/margin histograms per session (log2 default, linear by option); windowed maxima (last 1 s and 60 s) instead of max-since-reset (r3). | R06 §5, C5 §8.8, §8.12 |
| R-OBS-5 | SHOULD | Logs carry instance/session/module identity and the session name; in release builds tasklets never format or print a log line at any level — they write binary records to the log ring (04 §8); no fact is only in a log. | R06 F9–F10, C5 §8.7 |

### 5.6 Threading and real-time safety

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-THR-1 | MUST | No public API function executes on a scheduler tasklet, and tasklets never call application code unless the app registers an explicitly data-plane hook. The inline-safe DP subset from a busy-loop thread is defined and every other call returns `-MTL_EDEADLK` (r3). | maintainer, R03, C5 §4.1 |
| R-THR-2 | MUST | Tasklet ↔ application hand-off is lock-free; no tasklet waits on a lock, futex or syscall that an application thread can delay. | R02 §4.2 #10 |
| R-THR-3 | MUST | Every public function is classified (control / data / data with caller-context work / wait / async-signal-safe) with a documented thread-safety promise, enforced dynamically in debug builds (r3). | R13 R-L9, C5 §2.15 |
| R-THR-4 | MUST | Heavy per-frame work (pixel conversion, RX copy, codec) never runs on a tasklet; where it runs is policy and is reported. | R01 D2, R07 Q7 |
| R-THR-5 | SHOULD | Waking a sleeping application thread costs the tasklet at most one non-blocking signal per sleep, and can be delegated off the pinned core. | R09 §11 |

### 5.7 Lifecycle, capability, ABI

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-LIFE-1 | MUST | Session states CREATED → (ARMED) → RUNNING → DRAINING / FLUSHING → STOPPED (≡ CREATED with history), plus ERROR and a transient DESTROYING; start/stop reversible; discard queued units without stopping; destroy from any state. | R13 §6.1, C4 C-01 |
| R-LIFE-2 | MUST | Stop has DRAIN and FLUSH modes (r3 name); both leave every accepted unit with a terminal result. | R00 §14 #25 |
| R-LIFE-3 | MUST | Interrupt/stop wakes blocked callers immediately with a distinct code. | R13 R-L3 |
| R-LIFE-4 | MUST | After destroy returns: no callback, no access to imported memory, handle invalid. | R13 R-L4 |
| R-LIFE-5 | SHOULD | Instance re-initialisation within a process (#1341). | R08 B5 |
| R-CAP-1 | MUST | Capability query per port and backend; session create uses REQUIRE/PREFER/OFF and reports what was granted (pacing way, data path, conversion, DMA, timestamps). | R09 §2.1, R07 Q4 |
| R-ABI-1 | MUST | Every public input struct starts with `struct_size` (input only, never rewritten, r3); handles are opaque; flags are plain integer literals; unknown flags are rejected. | R13 §7, R02 §4.4, C5 §2.6 |
| R-ABI-2 | MUST | The new API has a soname, hidden default visibility and a version script; during the experimental period it ships as its own DSO whose soname changes on every incompatible change (r3). libmtl itself gets a soname and version script in Phase 0 (Q-ABI-1). | R13 §4, C5 §2.18 |
| R-ABI-3 | SHOULD | An experimental tier, so pieces can land before they are frozen. | R13 Q13 |
| R-ABI-4 | MUST | **r3.** A versioned `struct mtl_instance_params` lands before the new API; the default instance publishes a merge table (invariant / mergeable / ignored) for every field. | C5 §2.9, §8.5 |
| R-ABI-5 | MUST | **r3.** The header compiles as C99 and C++17 with `-Wpadded -Werror`; every public struct has a size check; no input field has a non-zero default; every input struct has an exported `*_init()`. | C5 §2.1–2.4, §2.7 |

### 5.8 Operations (r3)

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-OPS-1 | MUST | Flow changes on all legs of a session commit atomically at one activation (`NOW`, `AT_TAI`, `AT_MEDIA_INDEX`); on failure nothing changes. | C5 §8.1 |
| R-OPS-2 | MUST | Resources are reserved for every configured 2022-7 leg regardless of link; each leg has an admin state settable while RUNNING and an oper state from a link monitor. | C5 §8.2 |
| R-OPS-3 | MUST | A session has a stable, per-instance unique name, copied at create and carried in info, stats, events and logs; sessions of an instance can be enumerated. | C5 §8.7 |
| R-OPS-4 | SHOULD | A controller can ask for remaining capacity (queues, RL queues, scheduler quota, lcores, sessions) and dry-run a create against it. | C5 §8.11 |
| R-OPS-5 | SHOULD | A STOPPED session can be reconfigured (format change) keeping handle, name, flows, SSRC, stats, timeline, group and EQ bindings. | C5 §7.4 |
| R-OPS-6 | MUST | Loss of MtlManager never kills or stalls running sessions; new creates fail with a distinct reason or fall back when the instance allows it. | C5 §8.6 |
| R-OPS-7 | MUST | Create and start never block on ARP or IGMP; flow resolution is a per-leg state with an event and a getter. | C5 §7.6 |

### 5.9 Testability and usability (r3)

| ID | Level | Requirement | Source |
|---|---|---|---|
| R-TEST-1 | MUST | A NIC-less **null backend** ships in the library (experimental): port spec `null:<n>`, completes units at their scheduled time from the instance clock, needs no NIC, root or hugepages. | C5 §3.3 |
| R-TEST-2 | MUST | A **test time source** (`mtl_time_test_source`, `mtl_time_test_advance`) that the published time base honours, so time-relative rules are testable deterministically. | C5 §3.3 |
| R-TEST-3 | MUST | A **fault-injection** debug API (`mtl_debug_inject`) that can force every fault of the 13 §6 matrix, including ERROR; built only with `-Denable_debug_api=true`, never in release builds by default. | C5 §8.10, §1.8 |
| R-TEST-4 | SHOULD | The I tier can pull a link and reset a VF from `gtest.sh` (`nicctl.sh` steps). | C5 §8.10 |
| R-USE-1 | SHOULD | An L4 simple layer gives P1 a started session with a library pool and no results in a ≤ 10-line loop. | C5 §3.1 |
| R-USE-2 | SHOULD | Bindings get stride-explicit array reads, copy-in/out helpers and a reference Python wrapper. | C5 §3.5 |
| R-MIG-1 | MUST | Every engine fix reaches legacy users: bugfixes on by default, wire-visible changes behind a legacy opt-in flag; every phase passes a legacy KahawaiTest and acceptance gate. | C5 §1.4 |
| R-PERF-1 | MUST | Numeric performance budgets exist before Phase 1 (13 §8) and are confirmed or revised by the Phase 0 baseline spike S0. | C5 §1.8 |
| R-SEC-1 | MUST | Security properties of the new surfaces are stated and tested: import alignment, access enforcement, debug-API build gating, wait-object ownership ([15](15-security-and-deployment.md)). | C5 §1.10 |

## 6. Quality bar

The API is "done" for a phase only when every MUST in scope has a contract test at the
cheapest tier that can observe it (the requirement → guarantee map is in
[13 §0](13-guarantees-and-tests.md)). Behaviour that cannot be tested is not promised.

**r3.** Each guarantee is marked **P** (promised: B/U/UB evidence exists, or I-tier
evidence exists or is funded in the named phase) or **BE** (best effort until the named
evidence exists) `[C5 §1.8]`. A BE guarantee never blocks a phase exit; a P guarantee
does.

## 7. Success criteria in user terms (r3)

Revision 2 had no definition of done that a user would recognise `[C5 §1.9]`. Each phase
now ends with a user-visible result. The details, and the engineering exit criteria, are
in [14](14-implementation-roadmap.md).

| Phase | Done when a user can … |
|---|---|
| 0 | read a header that compiles, and see the objections of the FFmpeg/GStreamer plugin owners, the MXL team and the external engine team answered in the design (design review held) |
| 0.5 | send video, audio and ANC from one file with RxTxApp on the **legacy** API and get exact RTP on every packet, verified by the 13 §7 oracle |
| engines | turn on each engine fix on the legacy API with one flag, and see no wire change with the flag off |
| 1 | run a sample and a GStreamer st20 sink prototype on the new API with latency no worse than today; run the doc examples and the Python wrapper on the null backend, with no NIC |
| 2 | send and receive all five essences; activate new flows on both 2022-7 legs at one TAI instant; disable and re-enable a leg; ask "can this host take 12 more 1080p59 streams?" and get the answer admission then confirms |
| 3 | send the maintainer's A/V/ANC example through a group on the new API with exact RTP, and pass EBU LIST narrow on at least one NIC × pacing class |
| 4 | receive with FFmpeg zero-copy (`av_buffer_create` over leases) and send from a GStreamer export pool with zero copies, shown by the path counters |
| 5 | pull a link, reset a VF, step PTP and kill MtlManager in CI and see the documented outcome each time, from events and getters, not logs |
| 6 | use the FFmpeg and GStreamer plugins on the new API in the acceptance smoke suite; build against a frozen ABI |
