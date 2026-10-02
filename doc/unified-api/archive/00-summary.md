# Unified MTL session API — executive summary

| | |
|---|---|
| Status | **Draft for maintainer review — revision 4 (reviews C1–C5, then a simplification pass).** Nothing is decided; every choice below is a proposal. Seventeen decisions need the maintainer; everything else is a proposed default open to objection |
| Date | 2026-10-01 |
| Baseline | `main` @ `545a266a`; PR #1610 @ `14a1f80c` (16 commits, based on `74c9af0e`); PR #1770 (open) @ `74b9991d` |
| Inputs | the maintainer's PR #1610 review, 13 research notes in [research/](research/), 5 reviews in [reviews/](reviews/) and the designer's [response to C5](reviews/C5-response.md), 9 simplification studies and 2 reviews in [simplification/](simplification/) |

## Recommendation

**Fix the engines first, on the legacy API, and ship the A/V answer there within weeks.
Then build the unified API — one opaque session for every essence, media-specific
creation, common lifecycle, waiting and statistics — as a new internal session core over
the existing pipelines, reached through one internal slot interface. Do not rebase
PR #1610.**

Research confirmed every claim in the maintainer's review of PR #1610, and three are
worse than the review stated. The worst: TX pacing timestamps and user metadata never
reach the wire, because the transport overwrites them. The PR is 283 commits behind
`main`, and `main` has since hardened the pipeline logic the PR duplicates `[R01 §0]`.

Revision 3 answers review C5, which found the model right but the header uncompilable,
the RX half unspecified, the plan too big and its payoff too late `[C5 §0]`. The
incremental alternative is now costed ([02 §2.3](02-architecture.md)): about 6–9
engineer-months (EM) on the legacy API reach most of what users asked for, and 4–6 EM of
that is work the unified API needs anyway. The unified API is justified only by what the
legacy API cannot reach: one verb set and one metadata struct for every essence (GO-1),
versioned structs, no tasklet callbacks, and one lease/result contract for imported
memory. Q-CORE-1 asks the maintainer to decide with that costing in view.

## Revision 4 in brief

The maintainer found the diagrams and samples too complicated and asked for the leanest possible
include file, while keeping every use case MTL covers today, making RTP passthrough part of the
high-level API, and hiding `include/st20_api.h` ([REVISION-4.md](REVISION-4.md)).

- **A program includes `mtl.h`: 32 functions** (revision 3: 203 in one header). 16 optional
  headers add one job each; 125 functions in all, plus 14 reserved for later phases, against 217.
- **One config, one unit struct.** `mtl_session_open(mt, &sc, &s)` creates and starts a stream from one typed config; `struct mtl_unit` is what acquire and dequeue lend
  and submit reads. The minimal sender is 34 lines (63), zero-copy TX 60 (139).
- **Rare knobs are named options**, absent unless set; `MTL_INIT(&s)` replaces 37 init
  functions; buffers are slots by index; one queue type; one stats registry.
- **RTP passthrough is a unit kind** (`MTL_UNIT_PACKETS`) with the same verbs, no mbufs and no
  callbacks on tasklets; a generic RTP essence carries ST 2022-6, which cannot be sent today.
- **The legacy headers become opt-in, then internal**, over three releases after the freeze;
  every capability only they offer gets a home first, or an explicit removal you approve.

## The addenda: Kubernetes, NMOS and IPMX

- **Safe in a pod** ([16](16-kubernetes-and-crash-safety.md), M16).
  - `mtl_instance_close(mt, timeout)` shuts down within the grace period, network first, and
    never touches freed memory or a recycled descriptor. It returns 0 retired, 1 quiesced,
    or `-MTL_EIO` when a port could not be stopped.
  - A SIGKILL leaves nothing that blocks a restart (R8).
  - `mtl_instance_get_health` serves the probes, and liveness never depends on links or PTP.
  - CPUs and time come from the pod, and the pod runs as non-root on a prepared node.
- **Fit for NMOS and IPMX** ([17](17-nmos-and-ipmx.md), M17).
  - One `mtl_session_update` per IS-05 PATCH, switching at the slot boundary by the clock,
    with its planned and applied instants.
  - `mtl_sdp.h` renders and parses SDP.
  - `mtl_rtcp.h` sends and receives IPMX sender reports.
  - A free-running clock and an async media mode cover IPMX without PTP.
  - `mtl_crypto.h` adds PEP encryption.

## The design in eight points

Revision-3 names below are mapped to revision 4 in [REVISION-4.md §6](REVISION-4.md#6-revision-3--revision-4-names).

1. **App-driven data path.** TX is `acquire → fill → submit`, RX is
   `dequeue → read → release`, and access moves only through a typed **`mtl_lease_h`**.
   The library no longer pulls frames through callbacks on pinned cores. This is the
   libfabric/Rivermax shape. Framework-friendly additions:
   - `mtl_tx_acquire_slot` sends one named imported buffer;
   - `mtl_session_interrupt(s, 1 / 0)` implements GStreamer `unlock` / `unlock_stop`;
   - `mtl_session_discard` (with optional rebase) handles seeks;
   - a portable wait handle (fd or `HANDLE`) covers epoll/asyncio; a call that finds nothing
     arms it;
   - `mtl_session_close` is deferred while downstream still holds frames;
   - `mtl_session_open(mt, &sc, &s)` with one typed config gives the first program in one call; the enums follow the legacy ones (D-97).
   [02](02-architecture.md), [03](03-object-model-and-lifecycle.md), [10](10-api-sketch.md)
2. **Nothing runs on the pinned cores except packet work.** No public API call executes
   in a tasklet. On the DPDK backend, tasklets never run application code, make syscalls,
   allocate, format logs or take a lock an application thread can hold.
   - A completion is one release store plus a fence.
   - A deadline-driven waker thread does the wake-up syscall. For unit periods below 1 ms
     the designer recommends a direct, non-blocking write by the tasklet (W2); that one
     exception to "no syscalls" is the maintainer's decision M6.
   - In `MTL_FLAG_TASKLET_THREAD` mode scheduler threads are not pinned and write the
     wake-up themselves.
   - Commands are immediate (stop, flush, interrupt) or boundary (`mtl_session_update` of
     flows and legs), and every command is acked or times out into ERROR.
   - A small inline-safe DP subset may be called from a user tasklet; everything else
     returns `-MTL_EDEADLK` there.
   [04](04-threading-and-execution.md)
3. **Every unit gets exactly one outcome, and results cannot be lost.** Completing
   contexts only mark a lease DONE; the application-side reader turns DONE leases into
   results in submission order.
   - A TX result says ON_TIME, LATE, DROPPED (with a reason), FLUSHED/WITHDRAWN or
     FAILED (`UNSET = 0` is never terminal), with the deadline, the scheduled, enqueued
     and observed times, and signed margins.
   - The core result record is 96 bytes; the full timing record is chosen by its size.
   - Results are off by default for library pools (so a minimal loop cannot stall) and
     always on for imported memory.
   - One error vocabulary (`MTL_E*`): `-MTL_ESHUTDOWN` only when the app stopped the
     session, `-MTL_EIO` for ERROR, `-MTL_ENODEV` for a device gone; `mtl_last_error`
     says why.
   [07](07-completions-events-and-errors.md)
4. **One buffer contract.** Library pools, `mtl_mem_alloc` and imported application memory
   become the same pool slots with the same lease state machine; `mtl_session_attach` lays a
   framework's arena out as slots in one call.
   - Regions are refcounted, page-aligned and mapped into every device on the path
     (shmem, memfd and hugetlbfs included).
   - Zero-copy is a policy (`MTL_SESSION_REQUIRE_DIRECT`) independent of where memory came
     from; `stride ≥ row_bytes` stays DIRECT; the path used is reported.
   - RX supports index-addressed placement (MXL), and one RX unit can feed N TX sessions
     zero-copy through hold counts.
   [05](05-memory-and-buffers.md)
5. **Two times, not one.**
   - **Media time** says what the unit is; RTP is always `floor(M × rate)` from it, for
     every essence and every field.
   - **Launch time** says when packets leave. It is derived from the ST 2110 transmission
     models and from the session's declared **source kind** (playback; live capture with a
     minimum transmit delay; SDI→IP gateway).
   - Live sinks use TAI media time with CAPTURE and nearest-slot snapping, and declare
     their latency with `media_time_offset_ns`.
   - Late handling is explicit and bounded, so nothing slides. ANC and fast metadata keep
     their keep-alive packets.
   [06](06-timing-pacing-and-sync.md)
6. **Multi-essence sync by construction, on TX and RX.** Sessions share a **timeline**
   whose anchor lies on the common grid of their rates for every start index. One
   `mtl_session_start` of several sessions arms them atomically; separate processes share the epoch timeline through an index
   helper. Receivers share timelines too, and get a media index per unit. This
   answers the maintainer's A/V question for sender and receiver — and Phase 0.5 answers
   it on the legacy API first. [06 §10](06-timing-pacing-and-sync.md)
7. **Observable by default, and operable.**
   - One registry of named, cumulative values (no reset, no epochs), read in bulk without a
     lock; gauges, windowed maxima and histograms; a TX ST 2110-21 self-check; the RX
     timing-parser results per frame.
   - Stable, unique session names; session enumeration; granted values in
     `mtl_session_get_info` and "info.*" keys; a capacity query for controllers.
   - Atomic multi-leg `mtl_session_update` at a TAI instant, leg admin state, a link monitor.
   [08](08-observability.md), [09](09-media-modes-and-backends.md)
8. **A lifecycle, ABI and test substrate that hold.**
   - Explicit states, start at a time or media index with preroll, drain versus flush,
     update instead of re-create, deferred close; typed handles with ID 0 = null.
   - A versioned `mtl_instance_params` and a libmtl soname in Phase 0; the new API in its
     own experimental DSO until the freeze; headers that compile with size checks and no
     implicit padding; `MTL_INIT` and zero defaults.
   - A null backend, a test clock and fault injection ship with Phase 1.
   [03](03-object-model-and-lifecycle.md), [11](11-abi-compatibility-and-migration.md),
   [13](13-guarantees-and-tests.md), [15](15-security-and-deployment.md)

## What the reviews changed

| Review | Most important finding | Change |
|---|---|---|
| [C1 real-time feasibility](reviews/C1-realtime-feasibility.md) | ordered exactly-once results cannot be built over unmodified pipelines¹; the draft's wake protocol lost wake-ups | consumer-side result materialisation; explicit L0 hooks; idle descriptor cleanup; seq_cst fences; deadline-driven waker; per-writer stats |
| [C2 timing and standards](reviews/C2-timing-standards.md) | the defaults dropped every live camera frame; the headline A/V example started in the past; unbounded "send late" would slide the stream² | source kinds + `min_tx_delay`; lazy timeline anchors; bounded SEND_LATE; corrected grids; keep-alive underrun defaults; clock-step policies; RX RTP offsets |
| [C3 usability](reviews/C3-usability-personas.md) | the recommended default stalled the minimal loop silently; cancel could not implement GStreamer `unlock`; imported framework buffers could not be sent by name; MXL could not place unit *k* in grain *k* | NONE default for library pools + `blocked_on`; sticky interrupt; `acquire_buffer`; `BY_INDEX`; deferred destroy; discard; dry-run query³ |
| [C4 consistency](reviews/C4-consistency-audit.md) | 44 cross-document inconsistencies, 6 MUST requirements without a contract test, dropped research questions, silently missing modes, working-tree citations | fixed; guarantee map (13 §0); new questions; mode dispositions (09 §8); citations re-pinned to `545a266a` |
| [Simplification pass](simplification/) (r4) | the samples were ≈ 82 % API ceremony with 115 hidden rules; one 3029-line header; RTP passthrough and session-only features would be lost by hiding `st20_api.h` | a lean `mtl.h` + optional headers; one config and one unit struct; options; packet units; the hiding plan and its pre-hide gap list ([REVISION-4.md](REVISION-4.md)) |
| [C5 adversarial user](reviews/C5-adversarial-user-review.md) | the header did not compile; RX was designed by omission; the plan was ≈ 50–75 EM with its payoff last; no NIC-less test substrate; no operator surface | a compiled header set in `sketch/`; the RX timing model; **Phase 0.5, engines first**; the test substrate in Phase 1; the operator surface; ten decisions⁴ ([response](reviews/C5-response.md)) |

¹ st20p frees the slot before calling back; converting sessions get no transport-done
callback; the chain path completes ≈ 512 packets late and never while idle.

² Also grid table errors, missing ANC/FMD keep-alive, and ST22 rate modes.

³ Also named timelines, `mtl_tx_write`, ANC packet tables, and a normative sketch.

⁴ In detail: `mtl_lease_h`, null handles, `MTL_E*`, stride-explicit array reads and the
zero-default rule; RX hooks and T0 on the grid for every start index; the incremental
alternative costed; null backend, test clock and `mtl_debug_inject`; numeric performance
budgets and P/BE marking of guarantees; `update_flows`, leg admin state, the link
monitor, names, enumeration and capacity; the security and deployment document.

## Why this is needed (evidence)

Revision 2 led with "475 public functions". That count is honest only with its breakdown
— ≈ 154 of them are colour-conversion helpers, 104 are pipeline verbs — and the plan
grows the public surface during the transition ([01 §1.1](01-goals-and-requirements.md))
`[C5 §1.2]`. The case rests on these instead:

| Area | Today (verified in code) | Note |
|---|---|---|
| Visibility | "done" is not "on time"; late reporting has 3 units and names no frame; recovery reports lost frames as COMPLETE; PTP `locked` never clears; runtime link loss is not detected | R05, R06 |
| Timing | one field drives pacing and RTP; default video RTP is the TX cursor (ST20 ≠ ST40 for the same frame); UTC labelled TAI; invalid user times silently re-interpreted; up to half a frame of A/V error; #1653 | R05, R12 |
| Memory | no region object; `mtl_dma_map` maps port P only, holds no references; five different "done" points; silent zero-copy→copy downgrades | R04 |
| Pinned cores | user callbacks run on tasklets under the session spinlock; BLOCK_GET does mutex + condvar on the pinned core; recovery rebuilds mempools there; `update_destination` holds the spinlock across a 60 s ARP wait; #1622, #1620 | R03 |
| Lifecycle / ABI | no per-session start/stop; NULL on every failure; no soname, no struct versioning; fields already moved silently; `mtl_init` after `mtl_uninit` fails (#1341) | R13 |
| Users | every framework re-implements condvar wake-ups, instance singletons and audio re-framing; FFmpeg never achieved zero-copy; timing is the largest cluster of external issues | R08 |
| Asymmetry | ST22 has no stats, ST30P no user timestamp, ST41 no frame RX, only video emits events | R02, R07 |

## What we would build first

| Phase | Content | Effort (EM) |
|---|---|---|
| 0 | the ten decisions; versioned instance params and a libmtl soname; PR #1770's fixes landed; the header compiling in CI; spikes S0 (baseline) … S8 (queue stop/start); the test-substrate design; **external design review** (exit) | 3–5 |
| **0.5** | on the **legacy** API: exact-math `st_timeline_*` helper, `ST30P_TX_FLAG_USER_TIMESTAMP`, the ST40P flag fix, a sample-accurate audio start rule, the pipelines' tasklet mutex replaced (SF-15). Exit: the maintainer's A/V/ANC example with exact RTP | 1.5–2.5 |
| engines | E1–E13 in the engines; bugfixes on for legacy users, wire-visible changes behind legacy opt-in flags; a legacy gate every phase | 6–10 |
| 1 | core (typed handles, states, leases, reader-side results, queues, waker, errors, options) + **null backend, test clock, fault injection** + ST20 over st20p + a reference Python wrapper | 11–16 |
| 2 | all essences; link monitor, leg admin state, atomic `mtl_session_update`, capacity query, manager reconnect; plugin ABI v2, `mtl_convert.h`; G-27 measured; go/no-go on the Phase 6 re-base | 7–10 |
| **2P** | packet units (RTP passthrough) for every essence and the generic RTP essence (ST 2022-6), in three steps; hiding the legacy headers depends on it | 4–7 |
| 3 | L2 timing core: media modes, timelines and groups (TX and RX), RX timing model | 4–6 |
| 4 | memory: regions, imports, holds, `MTL_POOL_DYNAMIC`, early `SOURCE_RELEASED`; FFmpeg RX zero-copy | 6–10 |
| 5 | robustness at the I tier: VF reset, recovery on workers, the fault matrix in CI | 4–7 |
| 6 | progressive rows, plugin rewrites, legacy pipelines re-based on L2 (committed), ABI freeze; then the legacy headers deprecated (F), opt-in (F+1), internal (≥ F+2) | 11–18 |

Total ≈ 59–92 EM with packet mode: 3.3–5.1 years at 1.5 FTE, 20–31 months at 3 FTE. The first user-visible
result (Phase 0.5) comes 6–10 weeks after Q-TIME-0 is answered, independent of the rest.
Details, exit criteria and success criteria in user terms:
[14-implementation-roadmap.md](14-implementation-roadmap.md). Contract tests:
[13-guarantees-and-tests.md](13-guarantees-and-tests.md).

## What needs the maintainer

Seventeen decisions. Every other question in [OPEN-QUESTIONS.md](OPEN-QUESTIONS.md) now carries
a **proposed default** that stands unless the maintainer objects during review
([DECISIONS.md](DECISIONS.md)) `[C5 §1.7]`.

| # | Questions | Decision | Designer recommendation |
|---|---|---|---|
| M1 | Q-CORE-1 | the core choices (push model, CQ/EQ, nothing on tasklets, one buffer contract, new core over existing engines), with the incremental costing of 02 §2.3 in view | confirm all five; engines first |
| M2 | Q-ARCH-1, Q-ARCH-1a | wrap the pipelines through one internal slot interface, with hooks in all four pipelines in Phases 1–2 | yes; the slot interface *is* the extracted core |
| M3 | Q-TIME-0, Q-TIME-24 | media time primary with derived or overridable launch; source kinds for live timing | yes and yes (also unblocks Phase 0.5) |
| M4 | Q-TIME-18 | the ST 2110-40 window for live ANC when video leaves L frames late | the window of the frame the video unit is sent in |
| M5 | Q-THR-1 | no user code on tasklets in v1 | yes |
| M6 | Q-THR-2 | may a pinned tasklet make the non-blocking wake-up write (W2) | the waker thread (W3) by default; W2 below 1 ms unit periods and in `MTL_FLAG_TASKLET_THREAD` mode (pinned since addendum K, 04 §5.2) |
| M7 | Q-TIME-15, Q-TIME-16 | the legacy wire-change policy | wire-visible engine changes opt-in on the legacy API, default in the unified API |
| M8 | Q-ABI-1 | the ABI promise | libmtl soname + version script in Phase 0; the new API in its own experimental DSO until the freeze |
| M9 | Q-MIG-3 | PR #1610's fate | keep open as a reference until the new header lands, then close with credit; salvage `mt_session_event.c` |
| M10 | Q-MIG-1, Q-PLAN-1 | external design review and survey; sequencing and staffing | review as the Phase 0 exit; start Phase 0.5 + engines + Phases 1–2 now |
| M11 | Q-R4-1 | the revision-4 shape | accept as one package |
| M12 | Q-R4-2 | legacy capabilities proposed for removal | remove, unless the survey finds a user |
| M13 | Q-R4-3 | wire-visible defaults of the unified API | accept |
| M14 | Q-R4-4 | RTP passthrough defaults and phase | accept; phase 2P |
| M15 | Q-R4-5 | hiding the legacy headers | three tiers, F / F+1 / F+2 |
| M16 | Q-R4-6 | running in Kubernetes pods (16) | accept as one package |
| M17 | Q-R4-7 | NMOS and IPMX (17) | accept; `mtl_crypto.h` after its cost spike |

## Document map

See [README.md](README.md).
