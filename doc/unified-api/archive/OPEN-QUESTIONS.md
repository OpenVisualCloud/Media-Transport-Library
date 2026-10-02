# Open questions for the maintainer

| | |
|---|---|
| Status | **Revision 4** (the simplification pass, [REVISION-4.md](REVISION-4.md)). **Seventeen decisions need the maintainer**: M1–M10 from revision 3, M11–M15 for revision 4, and M16–M17 for its Kubernetes and NMOS/IPMX addenda (§The decisions); every other question carries a **proposed default** that stands unless the maintainer objects |
| Date | 2026-10-01 |
| Source | 176 questions raised by the 13 research notes and the questions raised by reviews C1–C4, de-duplicated and merged into the list below; each links back to its source; revision 3 adds the answers to review C5 ([response](reviews/C5-response.md)) and three new questions (Q-TIME-26, Q-TIME-27, Q-PLAN-1) |

How to use this file:

- **Maintainer decisions (M1–M17)** are the only questions that still need you. Each has
  options, a recommendation and a **Your answer** line.
- **Proposed defaults.** Every other question has a "Proposed default (r3)" line: the
  designer answered it, applied the answer in the design documents and logged it in
  [DECISIONS.md](DECISIONS.md). It stands unless you object; to object, write in its
  **Your answer** line.
- **Priority** (kept from revision 2) — **B** blocks the Phase 1 header and core; **P**
  needed before its phase starts; **L** later.

## The decisions

| # | Questions | Decision | Designer recommendation |
|---|---|---|---|
| M1 | [Q-CORE-1](#q-core-1) | the core direction, with the incremental alternative costed | confirm all five choices; do the engine work first |
| M2 | [Q-ARCH-1](#q-arch-1), [Q-ARCH-1a](#q-arch-1a) | wrap the pipelines through one internal slot interface; hooks in all four pipelines | (a); the slot interface is the extracted core; Phase 6 re-base committed |
| M3 | [Q-TIME-0](#q-time-0), [Q-TIME-24](#q-time-24) | media time primary + derived launch; source kinds for live timing | (a) and (a) |
| M4 | [Q-TIME-18](#q-time-18) | which frame's ST 2110-40 window live ANC uses when video has L ≥ 1 | (d): the window of the frame video unit k is transmitted in |
| M5 | [Q-THR-1](#q-thr-1) | no user code on tasklets in v1 | (a) |
| M6 | [Q-THR-2](#q-thr-2) | may a pinned tasklet make the wake-up syscall (W2) for sub-ms sessions | W3 by default, W2 below 1 ms and in thread mode |
| M7 | [Q-TIME-15](#q-time-15), [Q-TIME-16](#q-time-16) | the legacy wire-change policy | wire-visible engine changes opt-in on the legacy API, default in the unified API |
| M8 | [Q-ABI-1](#q-abi-1) | the ABI promise | libmtl soname in Phase 0; the new API in its own experimental DSO until the freeze |
| M9 | [Q-MIG-3](#q-mig-3) | PR #1610's fate | keep open as a reference, then close with credit; salvage `mt_session_event.c` |
| M10 | [Q-MIG-1](#q-mig-1), [Q-PLAN-1](#q-plan-1) | external design review and survey; sequencing and staffing | review as the Phase 0 exit; start Phase 0.5 + engines + Phases 1–2 |
| M11 | [Q-R4-1](#q-r4-1) | the revision-4 shape: lean core + optional headers, one config, options, `MTL_INIT`, one unit struct, slots instead of buffer handles, results on/off, start arrays, one queue type, stats registry, the samples-audit rules | accept as one package |
| M12 | [Q-R4-2](#q-r4-2) | legacy capabilities proposed for removal (each yes/no) | remove all except those an external user still needs (M10 survey) |
| M13 | [Q-R4-3](#q-r4-3) | wire-visible defaults of the unified API | accept the four defaults |
| M14 | [Q-R4-4](#q-r4-4) | RTP passthrough: defaults (Q-PKT-1…9) and phase 2P | accept S8's recommendations; phase 2P after Phase 2 |
| M15 | [Q-R4-5](#q-r4-5) | hiding the legacy headers: three tiers, F / F+1 / F+2, separate `libmtl_unified.so.1` | accept |
| M16 | [Q-R4-6](#q-r4-6) | running in Kubernetes pods: bounded network-first shutdown, crash-only rules (R8), health for probes, CPUs and time from the pod, fail-fast open, no-IOMMU refused by default | accept as one package |
| M17 | [Q-R4-7](#q-r4-7) | NMOS and IPMX: the IS-05 activation contract, `mtl_sdp.h`, `mtl_rtcp.h`, `mtl_crypto.h`, the IPMX profile and timing without PTP | accept; `mtl_crypto.h` implemented after its cost spike |

## Index

| ID | Question | Pri | State (r3) |
|---|---|---|---|
| [Q-CORE-1](#q-core-1) | Confirm the core direction (push model, CQ/EQ, no app code on tasklets, one buffer contract, new L2 core over existing engines) | B | **M1** |
| [Q-ARCH-1](#q-arch-1) | What does v1 wrap: pipelines, the session layer, or a new core? | B | **M2** |
| [Q-ARCH-1a](#q-arch-1a) | Accept L0 engine hooks in all four pipelines as Phase 1 scope? *(new in r2)* | B | **M2** |
| [Q-ARCH-2](#q-arch-2) | Does the new API use today's `mtl_handle`, or a new instance object? | B | proposed default |
| [Q-THR-1](#q-thr-1) | May any user code run on a tasklet (opt-in inline hooks)? | B | **M5** |
| [Q-THR-2](#q-thr-2) | May a pinned tasklet ever make the wake-up syscall? | B | **M6** |
| [Q-THR-2a](#q-thr-2a) | Waker scheduling contract: CPU budget, affinity, SCHED_FIFO, deadline-driven *(new in r2)* | P | proposed default |
| [Q-THR-3](#q-thr-3) | Manual-progress mode (app-owned scheduler loop)? | L | proposed default |
| [Q-THR-4](#q-thr-4) | Where does conversion run: caller thread or worker pool? | P | proposed default |
| [Q-THR-5](#q-thr-5) | Destroy protection on data-plane calls: refcount RMW or state gate + grace period? | B | proposed default |
| [Q-THR-6](#q-thr-6) | Keep runtime session migration? | P | proposed default |
| [Q-THR-7](#q-thr-7) | Replace `ptp_get_time_fn` by a published time base? | P | proposed default |
| [Q-THR-7a](#q-thr-7a) | Also replace built-in-PTP PHC reads on tasklets with the published time base? *(new in r2)* | P | proposed default |
| [Q-THR-8](#q-thr-8) | Default data-plane threading contract: single submitter/consumer or MP-safe? | B | proposed default |
| [Q-THR-9](#q-thr-9) | Where do recovery and auto-detect run, and what recovery latency is acceptable? *(new in r2)* | P | proposed default |
| [Q-THR-10](#q-thr-10) | What do `MTL_FLAG_TASKLET_THREAD` / `TASKLET_SLEEP` mean for unified sessions? *(new in r2)* | P | proposed default |
| [Q-LIFE-1](#q-life-1) | Does stop release NIC resources (queues, flows, multicast) or only pause? | B | proposed default |
| [Q-LIFE-2](#q-life-2) | Re-init in one process (#1341) and a refcounted default instance | B | proposed default |
| [Q-LIFE-3](#q-life-3) | `mtl_uninit` with live objects: refuse or destroy all? | P | proposed default |
| [Q-LIFE-4](#q-life-4) | Destroy while the app holds leases: `-EBUSY`, invalidate, or defer? | B | proposed default |
| [Q-LIFE-5](#q-life-5) | Accept TX submissions before start (preroll)? | P | proposed default |
| [Q-LIFE-6](#q-life-6) | Async-signal-safe, sticky `cancel_waiters` with `resume_waiters` | B | proposed default |
| [Q-LIFE-7](#q-life-7) | Single-leg link loss: keep RUNNING with DROPPED results, or ERROR? | P | proposed default |
| [Q-LIFE-8](#q-life-8) | Multi-process expectations | L | proposed default |
| [Q-LIFE-9](#q-life-9) | Does create allocate NIC resources, or does start? | B | proposed default |
| [Q-LIFE-10](#q-life-10) | On VF reset or port restart: survive, ERROR, or re-create? *(new in r2)* | P | proposed default |
| [Q-CMP-1](#q-cmp-1) | Default completion mode (ALL / EXCEPTIONS / NONE) and the credit back-pressure it implies | B | proposed default |
| [Q-CMP-2](#q-cmp-2) | Ship a library-thread callback helper? | L | proposed default |
| [Q-CMP-3](#q-cmp-3) | Error detail: thread-local string, reason enum, or both? | P | proposed default |
| [Q-CMP-4](#q-cmp-4) | Epoch tick (today's VSYNC): EQ event, slot hint only, or both? | P | proposed default |
| [Q-CMP-5](#q-cmp-5) | Shared CQs (poll sets) in v1? | P | proposed default |
| [Q-CMP-6](#q-cmp-6) | What makes a TX unit `ON_TIME`: admission verdict or wire verdict? *(new in r2)* | P | proposed default |
| [Q-CMP-7](#q-cmp-7) | Idle descriptor recycling (`rte_eth_tx_done_cleanup`): allowed, rate, TSQ policy *(new in r2)* | P | proposed default |
| [Q-MEM-1](#q-mem-1) | One terminal result, or early `SOURCE_RELEASED` + terminal? | P | proposed default |
| [Q-MEM-2](#q-mem-2) | Is IOVA-PA mode in scope for imported memory? | P | proposed default |
| [Q-MEM-3](#q-mem-3) | Keep a raw-IOVA "pre-mapped" import for compatibility? | L | proposed default |
| [Q-MEM-4](#q-mem-4) | Region destroy: `-EBUSY` only, or also deferred release with an event? | L | proposed default |
| [Q-MEM-5](#q-mem-5) | Raise the 8-buffer cap for video pools? | P | proposed default |
| [Q-MEM-6](#q-mem-6) | RX missing data: zero-fill, loss map, or both? | P | proposed default |
| [Q-MEM-7](#q-mem-7) | `REQUIRE_DIRECT` with too few buffers: fail, or auto-raise the count? | P | proposed default |
| [Q-MEM-8](#q-mem-8) | Device memory (GPU, DMA-BUF) scope and what "GPU direct" means | L | proposed default |
| [Q-MEM-9](#q-mem-9) | Header split: drop, keep internal, or port the DPDK patch? | L | proposed default |
| [Q-MEM-10](#q-mem-10) | Dynamic (unattached) buffers in v1? | P | proposed default |
| [Q-MEM-11](#q-mem-11) | Buffers shared by two sessions and RX→TX lease transfer in v1? *(new in r2)* | P | proposed default |
| [Q-MEM-12](#q-mem-12) | Imported non-hugepage memory: invented IOVA range or IOVA == VA? *(new in r2)* | P | proposed default |
| [Q-TIME-0](#q-time-0) | Confirm the two-timestamp model: media time primary + launch override | B | **M3** |
| [Q-TIME-24](#q-time-24) | Confirm source kinds (PLAYBACK / CAPTURE / GATEWAY) and `min_tx_delay` for live timing *(new in r2)* | B | **M3** |
| [Q-TIME-1](#q-time-1) | Time source default; refuse timed sessions without locked TAI? | B | proposed default |
| [Q-TIME-2](#q-time-2) | How to surface and handle clock steps | P | proposed default |
| [Q-TIME-3](#q-time-3) | Horizon: fixed 1 s or configurable? | P | proposed default |
| [Q-TIME-4](#q-time-4) | Underrun `REPEAT_LAST` — needed, when? | P | proposed default |
| [Q-TIME-5](#q-time-5) | Replace `rtp_timestamp_delta_us` by a media-index offset? | P | proposed default |
| [Q-TIME-6](#q-time-6) | Audio continuity: strict, tolerance, or explicit discontinuity? | P | proposed default |
| [Q-TIME-7](#q-time-7) | Pacing engine: per port, or requested per session? | P | proposed default |
| [Q-TIME-8](#q-time-8) | Group member failure: others continue or stop? | P | proposed default |
| [Q-TIME-9](#q-time-9) | Allow media-time-ordered (not FIFO) submission? | L | proposed default |
| [Q-TIME-10](#q-time-10) | Which observed TX times can E810/E830 give, and how accurate? | P | proposed default |
| [Q-TIME-11](#q-time-11) | Audio launch offset default | P | proposed default |
| [Q-TIME-12](#q-time-12) | RX scheduled delivery at presentation time | L | proposed default |
| [Q-TIME-13](#q-time-13) | Progressive late-row policy (STALL / PAD / TRUNCATE) | L | proposed default |
| [Q-TIME-14](#q-time-14) | Which ST 2022-7 skew class does MTL claim? | P | proposed default |
| [Q-TIME-15](#q-time-15) | Rounding: `floor` everywhere, or only in the new API? | B | **M7** |
| [Q-TIME-16](#q-time-16) | Flip the legacy default video RTP to the epoch too? | P | **M7** |
| [Q-TIME-17](#q-time-17) | Sender type W: switch to the linear read schedule? | P | proposed default |
| [Q-TIME-18](#q-time-18) | Live ANC with video slot delay ≥ 1: which frame anchors the ST 2110-40 window? *(new in r2)* | B | **M4** |
| [Q-TIME-19](#q-time-19) | Common grid: exact for every member, or video/ANC only with audio floor-aligned; cap *(new in r2)* | P | proposed default |
| [Q-TIME-20](#q-time-20) | Timecode (ST 12 / RP 188) and 1001 audio-cadence helpers *(new in r2)* | L | proposed default |
| [Q-TIME-21](#q-time-21) | Legacy USER_PACING mapping: nearest epoch, nearest slot, or NOT_BEFORE? *(new in r2)* | P | proposed default |
| [Q-TIME-22](#q-time-22) | CTM/LLTM-conformant ANC packet scheduling in v1 (a wire change)? *(new in r2)* | P | proposed default |
| [Q-TIME-23](#q-time-23) | Default late policy per source kind; reject-vs-drop for too-late requests *(new in r2)* | B | proposed default |
| [Q-TIME-25](#q-time-25) | TAI-mode snap modes and tolerance defaults *(new in r2)* | P | proposed default |
| [Q-TIME-26](#q-time-26) | ST 2110-22 rate mode: CBR default? *(new in r3)* | B | proposed default |
| [Q-TIME-27](#q-time-27) | RX relock threshold *(new in r3)* | P | proposed default |
| [Q-OBS-1](#q-obs-1) | Stats epochs instead of reset | P | proposed default |
| [Q-OBS-2](#q-obs-2) | Out-of-process stats reader (like `rmx_stats`)? | L | proposed default |
| [Q-OBS-3](#q-obs-3) | Timing-parser summary always on? | P | proposed default |
| [Q-MODE-1](#q-mode-1) | RTP/packet level in the unified API | P | **answered by the maintainer (r4)** |
| [Q-MODE-2](#q-mode-2) | Network header controls (DSCP, TTL, VLAN, IPv6) | P | proposed default |
| [Q-MODE-3](#q-mode-3) | ST22: separate create or codec sub-struct? | B | proposed default |
| [Q-MODE-4](#q-mode-4) | Codec/converter plugin ABI: freeze, redesign, or drop? | L | proposed default |
| [Q-MODE-5](#q-mode-5) | `DATA_PATH_ONLY` (app-managed flows): keep, fix, drop? | L | proposed default |
| [Q-MODE-6](#q-mode-6) | SDP: helper in lib, values only, or nothing? | P | superseded by M17 |
| [Q-MODE-7](#q-mode-7) | Fate of legacy/experimental surfaces | L | proposed default |
| [Q-MODE-8](#q-mode-8) | RTCP retransmission scope *(new in r2)* | L | proposed default |
| [Q-ABI-1](#q-abi-1) | ABI promise level (soname, version script, visibility) | B | **M8** |
| [Q-ABI-2](#q-abi-2) | Versioned `mtl_init` replacement | P | proposed default |
| [Q-ABI-3](#q-abi-3) | Reserve a `next` extension chain in create configs? | B | proposed default |
| [Q-ABI-4](#q-abi-4) | Re-base legacy pipeline APIs on the new core later? | L | proposed default |
| [Q-ABI-5](#q-abi-5) | Names: header, prefix, source directory | B | proposed default |
| [Q-ABI-6](#q-abi-6) | Windows parity for the new API | P | proposed default |
| [Q-ABI-7](#q-abi-7) | Formalise C11 in `lib/` | P | proposed default |
| [Q-MIG-1](#q-mig-1) | Survey private/external users before freezing? | P | **M10** |
| [Q-MIG-2](#q-mig-2) | Canonical sample set | L | proposed default |
| [Q-MIG-3](#q-mig-3) | What happens to PR #1610 | P | **M9** |
| [Q-EXT-1](#q-ext-1) | Can someone with a Rivermax developer login confirm the unknowns? | L | proposed default |
| [Q-PLAN-1](#q-plan-1) | Sequencing and staffing *(new in r3)* | B | **M10** |
| [Q-R4-1](#q-r4-1) | Accept the revision-4 shape | B | **M11** |
| [Q-R4-2](#q-r4-2) | Legacy capabilities proposed for removal | P | **M12** |
| [Q-R4-3](#q-r4-3) | Wire-visible defaults of the unified API | P | **M13** |
| [Q-R4-4](#q-r4-4) | RTP passthrough defaults and phase | P | **M14** |
| [Q-R4-5](#q-r4-5) | Hiding the legacy headers | P | **M15** |
| [Q-R4-6](#q-r4-6) | Kubernetes pods and crash safety | P | **M16** |
| [Q-R4-7](#q-r4-7) | NMOS and IPMX | P | **M17** |

---

## Core

### Q-CORE-1

> **Maintainer decision M1.** Recommendation unchanged (confirm all five). Decide with the
> incremental costing of [02 §2.3](02-architecture.md) in view: ≈ 6–9 EM on the legacy API reach
> most user asks, and 4–6 EM of that is shared with the unified plan; the unified API is justified
> only by GO-1, versioned structs, no tasklet callbacks and one lease/result contract for imported
> memory.

**Confirm the core direction.** The design rests on five choices; if any is wrong, most
documents change.

1. App-driven push on TX (`acquire → fill → submit`) replaces the library pulling frames
   through `get_next_frame` callbacks. [02 §6](02-architecture.md)
2. Per-unit results go to a reliable completion queue; state changes go to a separate
   bounded event queue. [07](07-completions-events-and-errors.md)
3. No application code runs on tasklets by default; tasklets never make syscalls or take
   locks an app thread can hold. [04](04-threading-and-execution.md)
4. One buffer handle and lease state machine for library and imported memory (your
   review's decision). [05](05-memory-and-buffers.md)
5. A new internal "session core" (L2) owns the contracts; v1 adapters wrap existing
   engines. [02 §2](02-architecture.md)

- **Recommendation:** confirm all five.
- **Research:** R00, R01 §2, R03 §7.1, R09 Q1–Q2, R11 §10.
- **Your answer:**

## Architecture

### Q-ARCH-1

> **Maintainer decision M2.** r3: (a), and the L0 hooks are specified as one internal **slot
> interface** ([02 §2.2](02-architecture.md)), which *is* option (c)'s extracted core. The Phase 6
> re-base of the legacy pipelines is a commitment with a go/no-go at the Phase 2 exit (S3 costs
> it).

**What does the v1 implementation wrap?**

- **Why it matters:** PR #1610 wrapped the low-level session layer, re-implemented the
  pipeline on top and wrote into transport internals, which lost the app's timing
  metadata. `main`'s pipelines now carry the hardened logic.
- **Options:** (a) wrap `st20p/st22p/st30p/st40p` (plus `st41` session); (b) wrap the
  session layer as PR #1610 did; (c) extract a common core first and move both old and
  new APIs onto it.
- **Recommendation:** (a), validated by spike S3, with the L0 hooks of Q-ARCH-1a; (c) later as Q-ABI-4.
- **Research:** R01 Q1, §6; R00 §15 phase 1; [02 §2.2](02-architecture.md).
- **Your answer:**

### Q-ARCH-1a

> **Maintainer decision M2.** r3: (a), decided together with Q-ARCH-1; the slot interface adds RX
> rows (deadline force-complete, reclaim, `BY_INDEX` lookup) and the submit-time `seq` hook ([04
> §4.4](04-threading-and-execution.md)).

**Accept L0 engine hooks in all four pipelines as Phase 1 scope?** *(new in r2)*

- **Why it matters:** review C1 showed that ordered, exactly-once results cannot be built over unmodified pipelines: st20p stores FREE before calling `notify_frame_done` (`st20_pipeline_tx.c:270` vs `:286`), converting sessions never get a transport-done callback, copy paths report "done" at the end of build, and the builder can claim a frame and never complete it.
- **Options:** (a) accept the hooks (held slot state, once-only completion hook, last-packet hook, rejected-at-pick-up callback, flush reclaim, idle descriptor cleanup) as Phase 1 scope; (b) wrap the session layer instead (Q-ARCH-1 b), where the same hooks are needed in `tv_*`/`rv_*` anyway.
- **Recommendation:** (a), sized by spike S3.
- **Research:** C1 #1, #2, #12, #20; [04 §4.4](04-threading-and-execution.md).
- **Your answer:**

### Q-ARCH-2

> **Proposed default (r3):** (b), changed from (a): a typed `mtl_instance_h` over a versioned
> `struct mtl_instance_params`, landed in Phase 0 before the new API; `mtl_instance_from_legacy`
> bridges today's `mtl_handle` ([03 §7](03-object-model-and-lifecycle.md)) `[C5 §2.9]`. Stands
> unless you object.

**Does the new API use today's `mtl_handle` (from `mtl_init`) or a new instance object?**

- **Why it matters:** coexistence with legacy sessions in one process vs a clean
  versioned instance.
- **Options:** (a) reuse `mtl_handle` in v1, add a versioned init later (Q-ABI-2);
  (b) new `mtl_instance_create` now.
- **Recommendation:** (a).
- **Research:** R13 §1.1; [03 §7](03-object-model-and-lifecycle.md).
- **Your answer:**

## Threading

### Q-THR-1

> **Maintainer decision M5.** Recommendation unchanged (a). r3 adds a zero-hop path that needs no
> hook: a user tasklet may call the inline-safe DP subset ([04
> §3.3](04-threading-and-execution.md)).

**May any user code still run on a tasklet?**

- **Why it matters:** slice producers, zero-copy forwarders and the MXL bridge chose
  tasklet callbacks for zero-hop latency; they are also why a slow callback stalls every
  session on a core, and why a stats call from a callback deadlocks.
- **Options:** (a) never; (b) opt-in inline hooks with a wait-free contract, an
  allow-list of DP functions, a measured budget and automatic disable; (c) only in a
  manual-progress mode (Q-THR-3).
- **Recommendation:** (a) in v1; measure whether W0 busy polling closes the gap; add (b)
  only if it does not. `rx_slot_select = BY_INDEX` covers the MXL `query_ext_frame` use without a callback.
- **Research:** R03 Q1, H1–H2; R08 Q1; [04 §6](04-threading-and-execution.md).
- **Your answer:**

### Q-THR-2

> **Maintainer decision M6.** r3 designer recommendation: W3 (waker) by default in lcore mode,
> **W2 (a non-blocking eventfd write from the tasklet, only when a waiter is armed) automatically
> for sessions with a unit period below 1 ms**, and W2 always in `MTL_FLAG_TASKLET_THREAD` mode
> (pinned since addendum K, but preemptible, so the write costs no more there: 04 §5.2). This is a syscall on a pinned core, so it needs your acceptance; the alternatives
> are W0 polling for sub-ms sessions or W3 everywhere with a stated latency ([04
> §5.2](04-threading-and-execution.md)).

**May a pinned tasklet ever make the wake-up syscall?**

- **Why it matters:** this is your hard requirement. Today the pipeline does mutex +
  condvar on the pinned core and can sleep behind an app thread.
- **Options:** (a) never: an unpinned waker thread does it (W3, default), with W2
  (direct eventfd write when a waiter is armed) as a per-session opt-in; (b) never, and
  no blocking waits at all (poll/back-off only); (c) always direct (W2).
- **Recommendation:** (a), with a deadline-driven waker (Q-THR-2a); a completing context that is *not* a tasklet (free callback on an app thread, plugin thread) wakes directly. Spike S1 measures latency and CPU.
- **Research:** R03 Q2, §7.3; [04 §5](04-threading-and-execution.md).
- **Your answer:**

### Q-THR-2a

> **Proposed default (r3):** As recommended, plus: with no due time the waker sleeps at most 1 ms;
> wake words per scheduler **and** per RX packet lcore; `SCHED_OTHER` by default, `SCHED_FIFO`
> only as an explicit instance setting ([04 §5.2](04-threading-and-execution.md), [15
> §4](15-security-and-deployment.md)). Stands unless you object.

**Waker scheduling contract.** *(new in r2)*

- **Why it matters:** a fixed 20–50 µs poll costs 5–15 % of a core and timer slack turns 20 µs into ~70 µs; a waker inheriting the main lcore's affinity shares a CPU with the app.
- **Options:** CPU budget; affinity source (MtlManager, a configured housekeeping cpuset, or inherited); `SCHED_FIFO` allowed or not; deadline-driven vs fixed-interval polling; `rte_power_monitor` where available.
- **Recommendation:** deadline-driven sleep, one wake word per scheduler, `PR_SET_TIMERSLACK = 1`, explicit housekeeping affinity from config/MtlManager, `SCHED_FIFO` optional; W0 documented as required for 125 µs audio and line mode.
- **Research:** C1 #4; [04 §5.2](04-threading-and-execution.md).
- **Your answer:**

### Q-THR-3

> **Proposed default (r3):** (a) auto only in v1. The null backend with the test clock covers the
> testing need that manual progress was sometimes wanted for. Stands unless you object.

**Offer a manual-progress mode (the app owns a scheduler thread and calls
`run_once`)?**

- **Why it matters:** gives GStreamer/OBS-style apps a way to own their threads and could
  replace `mtl_sch_api.h`; unsafe for hardware-paced video.
- **Options:** (a) auto only; (b) auto + manual for non-video or app-pinned loops; (c)
  manual as a first-class peer.
- **Recommendation:** (a) for v1, revisit.
- **Research:** R03 Q3; R09 Q9.
- **Your answer:**

### Q-THR-4

> **Proposed default (r3):** (a) caller thread in v1, reported as `convert_context = CALLER` with
> `caller_work_ns`; (c) later. Stands unless you object.

**Where does pixel conversion run?**

- **Why it matters:** in v1 over the pipelines, internal conversion runs in the caller
  of `submit` (app thread), which makes `submit` O(frame) for converting sessions.
- **Options:** (a) caller thread (today's behaviour, reported); (b) a library worker
  pool; (c) configurable per session.
- **Recommendation:** (a) in v1, (c) later.
- **Research:** R01 Q12; R07 Q7; [05 §6.3](05-memory-and-buffers.md).
- **Your answer:**

### Q-THR-5

> **Proposed default (r3):** As recommended (in-flight counter on its own cache line +
> generation-tagged handles over grow-only chunked tables); r3: elided together with the reaper
> lock for `MTL_SESSION_SINGLE_READER` sessions without `MT_SUBMIT`, except releases ([03
> §2.3](03-object-model-and-lifecycle.md)). Stands unless you object.

**How are data-plane calls protected against a concurrent destroy?**

- **Why it matters:** today's guard does a SEQ_CST RMW per call on a cache line the
  tasklet reads (false sharing, measurable at packet rate); it also cannot protect calls
  made after free.
- **Options:** (a) keep the per-call refcount; (b) session-state gate (relaxed load) +
  grace period (QSBR/epoch) in destroy; (c) CP-only guard, DP calls valid only between
  start and destroy by contract.
- **Recommendation:** revised after review C1: a per-object in-flight counter on its own cache line, plus generation-tagged handles over type-stable slot memory (a stale handle never dereferences freed memory). A state gate alone is not safe, and `rte_rcu_qsbr` with arbitrary app threads costs a fence per call, so (b) saves nothing in v1; revisit QSBR only for a packet-rate DP API (C1 #5).
- **Research:** R03 Q11, §8; R13 §1.3; [03 §2.2](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-THR-6

> **Proposed default (r3):** (a): unified sessions never migrate; legacy migration flags are
> "ignored on second acquire" in the merge table. Stands unless you object.
>
> **Revision 4, port first (D-98):** changed to (b). Migration is an opt-in feature today
> (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`), and no current use case is cut without approval. So it is
> ported, with a quiesce and acknowledge handshake: `instance.tx_video_migrate` and
> `instance.rx_video_migrate` (off by default, as today) and the per-session `session.migrate`,
> the home of `ST20_RX_FLAG_DISABLE_MIGRATE` ([11 §R4.15](11-abi-compatibility-and-migration.md)).

**Keep runtime session migration (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`)?**

- **Why it matters:** it moves sessions between lcores every 6 s check, takes blocking
  session spinlocks, and breaks single-producer assumptions unless quiesced.
- **Options:** (a) drop for unified sessions; (b) keep with an explicit quiesce/ack
  handshake; (c) keep as is.
- **Recommendation:** (a) in v1, (b) if users need it.
- **Research:** R03 Q7, §4.4; R13 §1.5.
- **Your answer:**

### Q-THR-7

> **Proposed default (r3):** (b), with the r3 time-base contract: refresh ≤ 100 ms, least-squares
> ratio servo, slewed publication, one record per socket; spike S7 bounds the error ([04
> §8.1](04-threading-and-execution.md)). Stands unless you object.

**Replace the per-read `ptp_get_time_fn` user callback with a published time base?**

- **Why it matters:** the callback runs on every tasklet pacing computation — user code on
  pinned cores.
- **Options:** (a) keep, documented as DP-class; (b) the app (or MTL's PTP) updates a
  `{tsc_base, tai_base, ratio, seq}` time base from a normal thread; tasklets read it wait-free;
  (c) remove the user time source.
- **Recommendation:** (b) for the new API; legacy keeps (a).
- **Research:** R03 Q9, H7; [04 §8](04-threading-and-execution.md).
- **Your answer:**

### Q-THR-7a

> **Proposed default (r3):** (a), gated by spike S7. Stands unless you object.

**Should the published time base also replace built-in-PTP PHC reads on tasklets?** *(new in r2)*

- **Why it matters:** with built-in PTP every tasklet time read is `rte_eth_timesync_read_time`, a PMD register read (possibly a PF round-trip on iavf); `clock_gettime` on `/dev/ptpN` is a syscall.
- **Options:** (a) yes, for every source, accepting TSC-extrapolation error between servo updates; (b) only for user sources.
- **Recommendation:** (a).
- **Research:** C1 #7, C2 #10; [06 §2.3](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-THR-8

> **Proposed default (r3):** Superseded by the r3 contract table ([04
> §4.2](04-threading-and-execution.md)): acquire and releases are MP-safe by default;
> submit/publish need one context unless `MTL_SESSION_MT_SUBMIT` (required for exported pools);
> readers take the reaper lock unless `MTL_SESSION_SINGLE_READER`. Stands unless you object.

**Default concurrency contract for data-plane calls.**

- **Why it matters:** single-producer/consumer rings are lock-free and cheap; MP-safe
  rings cost CAS contention and preemption hazards. Today's rule ("one app thread per
  session and direction") is only in `doc/design.md` and silently corrupts when violated.
- **Options:** (a) single submitter + single consumer per session by default, stated in
  the header, debug-asserted, with an MP-submit option; (b) always MP-safe; (c) per-thread
  queues bound to a poll set.
- **Recommendation:** (a), stated as "externally serialised" (one context at a time, not one thread: a GstBufferPool export acquires in one thread and submits in another); debug builds detect *overlap* with a CAS owner flag, not a different thread ID; `rx_release` is always MP-safe.
- **Research:** R03 Q4, §5; R09 Q10; [04 §4](04-threading-and-execution.md).
- **Your answer:**

### Q-THR-9

> **Proposed default (r3):** (b), plus: recovery never touches `sh_info` (SF-41), and a worker
> resets a stalled queue on destroy or ERROR ([04 §4.5](04-threading-and-execution.md)). Stands
> unless you object.

**Where do TX-hang recovery and RX auto-detect re-init run, and what added recovery latency is acceptable?** *(new in r2)*

- **Why it matters:** both do control-plane work on tasklets today (queue re-acquire, mempool re-create, allocation).
- **Options:** (a) the admin thread, event-driven; (b) a per-instance worker with a quiesce handshake (tasklet raises a flag, worker takes the session spinlock, swaps, releases; completion by CAS); (c) split.
- **Recommendation:** (b); the hang threshold is 1 s, so a worker hop adds nothing that matters.
- **Research:** R03 Q6; C1 #11.
- **Your answer:**

### Q-THR-10

> **Proposed default (r3):** (b) for Phase 1, plus: W2 by default in thread mode and
> `rte_thread_register` for scheduler threads; G-39 measured per mode ([04
> §7.1](04-threading-and-execution.md)). Stands unless you object.

**What do `MTL_FLAG_TASKLET_THREAD` and `MTL_FLAG_TASKLET_SLEEP` mean for unified sessions?** *(new in r2)*

- **Why it matters:** the sleep path uses condvars, alarms and `nanosleep` on scheduler threads, which interacts with the waker (its idle path can perform pending wakes) and with G-39.
- **Options:** (a) keep both as they are (only idle paths enter the kernel); (b) keep, and use the sleep path to perform pending wakes; (c) fold into a later manual-progress mode.
- **Recommendation:** (b).
- **Research:** R03 Q10; [04 §5.2](04-threading-and-execution.md).
- **Your answer:**

## Lifecycle

### Q-LIFE-1

> **Proposed default (r3):** (a) by default for the RX join and flow rule (kept across stop until
> destroy or `update_flows`), so restart is instant; a release-on-stop flag is (c), later ([03
> §3.5](03-object-model-and-lifecycle.md)). Stands unless you object.

**Does `stop` release NIC resources or only pause?**

- **Why it matters:** releasing (queues, flow rules, multicast membership, RL shaper)
  lets other sessions reuse them but makes `start` fallible and slower; pausing keeps
  multicast joined and start instant.
- **Options:** (a) pause only; (b) release everything; (c) pause by default, a flag to
  release.
- **Recommendation:** (c).
- **Research:** R13 Q3; [03 §3.4](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-LIFE-2

> **Proposed default (r3):** (a) + (b): EAL stays alive across the last release;
> `mtl_instance_acquire_default` is refcounted with a published merge table; ports must already be
> open in v1, runtime `mtl_port_open` is Phase 6 ([03 §7](03-object-model-and-lifecycle.md)).
> Stands unless you object.

**Must re-initialisation within one process work?**

- **Why it matters:** FFmpeg, GStreamer and two external projects re-implement a fragile
  refcounted singleton because `mtl_uninit` calls `rte_eal_cleanup()` and a second EAL
  init is rejected (#1341, open).
- **Options:** (a) keep EAL alive across uninit (release ports only) so init can run
  again; (b) a library-owned refcounted default instance; (c) document "one init per
  process".
- **Recommendation:** (a), plus (b) as `mtl_instance_acquire_default` (refcounted, merges port lists or opens ports later) — raised to **B** because framework plugins cannot meet persona P2 without it (C3 P2-6).
- **Research:** R08 Q3, B5; R13 §1.1.
- **Your answer:**

### Q-LIFE-3

> **Proposed default (r3):** (c): the default instance tears down in dependency order when its
> last reference goes; legacy `mtl_uninit` with unified objects returns `-EBUSY`, and the
> versioned uninit takes `DESTROY_ALL`. Stands unless you object.

**`mtl_uninit` while unified objects still exist.**

- **Why it matters:** today it probably self-deadlocks (SP-01).
- **Options:** (a) `-EBUSY`; (b) destroy everything in dependency order; (c) (a) with a
  `MTL_UNINIT_DESTROY_ALL` flag, taken by a versioned uninit (Q-ABI-2) because today's
  `mtl_uninit(mt)` has no flags argument.
- **Recommendation:** (c).
- **Research:** R13 Q9; [03 §6.4](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-LIFE-4

> **Proposed default (r3):** (c) deferred by default, `MTL_DESTROY_FORCE` = (b); r3: FORCE still
> waits for device references, and `mtl_instance_release` never fails because a session is
> DESTROYING ([03 §6–7](03-object-model-and-lifecycle.md)). Stands unless you object.

**Destroy while the app holds leases (acquired TX buffers, dequeued RX buffers).**

- **Options:** (a) `-EBUSY` until returned, `MTL_DESTROY_FORCE` to override; (b) succeed
  and invalidate (library-pool memory freed under the app); (c) *defer*: destroy stops the session and retires the handle for every call except release/abort on the outstanding leases, frees the pool when the last returns, and posts `SESSION_RETIRED`.
- **Recommendation:** (c) as the default (how GstBufferPool and AVBufferPool behave; `GstBaseSrc::stop` and FFmpeg `read_close` must return while downstream holds buffers), with `MTL_DESTROY_FORCE` for (b) (C3 P2-4).
- **Research:** [03 §6.3](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-LIFE-5

> **Proposed default (r3):** (a), with r3 `preroll_ns` and the horizon measured from the resolved
> start instant; a start that would strand queued units beyond the horizon fails atomically
> (`BEYOND_HORIZON`) ([06 §7.4](06-timing-pacing-and-sync.md)). Stands unless you object.

**Accept TX `acquire`/`submit` before `start` (preroll)?**

- **Why it matters:** DeckLink-style preroll lets playout fill the queue and start at an
  exact instant with no first-frame lateness.
- **Options:** (a) yes, in CREATED/STOPPED/ARMED; (b) only in ARMED; (c) no.
- **Recommendation:** (a).
- **Research:** R11 §1.1, §10 #9; [03 §3.2](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-LIFE-6

> **Proposed default (r3):** (a) sticky, renamed `mtl_*_interrupt` / `mtl_*_uninterrupt`, with an
> instance-wide flag (`mtl_instance_interrupt_all` / `uninterrupt_all`); destroy wins ([04
> §5.4](04-threading-and-execution.md)). Stands unless you object.

**Is an async-signal-safe way to unblock waiters required?**

- **Why it matters:** every sample stops from a SIGINT handler; today `mtl_abort` wakes
  nothing and `wake_block` does not return early.
- **Options:** (a) `mtl_session_cancel_waiters` / `mtl_instance_cancel_all_waiters`
  (atomics + one `write`); (b) no; apps use their own self-pipe.
- **Also:** is cancel edge-triggered or sticky? Edge-triggered races with a call about to start (today's GStreamer bug in a new form); sticky needs a clear operation.
- **Recommendation:** (a), **sticky** until `mtl_session_resume_waiters` (GStreamer `unlock` / `unlock_stop`), plus per-queue `mtl_cq/eq_cancel_waiters` so cancelling one element never wakes another (C3 P2-1).
- **Research:** R13 R-L8; R01 §5 #5; [04 §5.4](04-threading-and-execution.md).
- **Your answer:**

### Q-LIFE-7

> **Proposed default (r3):** (a), with per-leg `LINK_DOWN` results, a link monitor in Phase 2 and
> the dead leg's queue reset so the pool never starves ([09
> §7.2](09-media-modes-and-backends.md)). Stands unless you object.

**What happens to a single-leg session when its link goes down at runtime?**

- **Why it matters:** today runtime link loss is not detected at all; TX loops through
  hang recovery; RX simply stops.
- **Options:** (a) stay RUNNING, results `DROPPED/LINK_DOWN`, `PORT_LINK` event, resume
  on link up; (b) enter ERROR; (c) configurable.
- **Recommendation:** (a).
- **Research:** R13 §2; R06 Q6.
- **Your answer:**

### Q-LIFE-8

> **Proposed default (r3):** (a) for v1. Separate processes share time, not state: the epoch
> timeline and `mtl_rational_index_at` ([06 §10.7](06-timing-pacing-and-sync.md)). Stands unless
> you object.

**Multi-process expectations.**

- **Why it matters:** the KB claims secondary-process stats access, but EAL runs
  `--in-memory`; only SR-IOV + MtlManager multi-instance works.
- **Options:** (a) state "one process per instance" as a non-goal; (b) manager-mediated
  stats channel; (c) support DPDK secondary processes.
- **Recommendation:** (a) for v1.
- **Research:** R07 Q16.
- **Your answer:**

### Q-LIFE-9

> **Proposed default (r3):** (a), refined: create reserves queues, quota and flow-rule capacity
> but never waits for ARP or IGMP; flow rules and joins are installed at the first start; flow
> resolution is a per-leg state with an event and a getter ([03
> §3.3](03-object-model-and-lifecycle.md)). Stands unless you object.

**Does `create` allocate NIC resources (queues, flows, scheduler quota), or does `start`?**

- **Why it matters:** early failure at create vs cheap CREATED sessions that can be
  configured and grouped before committing resources.
- **Options:** (a) create validates and reserves everything, start only attaches the
  tasklet and joins multicast; (b) create is cheap, start allocates.
- **Recommendation:** (a): failures surface at create, start is fast and predictable
  (important for group start).
- **Research:** R13 R-L1; [03 §3](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-LIFE-10

> **Proposed default (r3):** (a) where the backend can restore queues and flows, otherwise ERROR
> with `DEVICE_GONE`/`PORT_RESET` and a defined restart path (`stop`, then `start` re-reserves or
> fails with `-MTL_ENODEV`) ([03 §3.6](03-object-model-and-lifecycle.md)). Stands unless you
> object.

**On VF reset or port restart, do sessions survive, go to ERROR, or need re-creation?** *(new in r2)*

- **Why it matters:** Phase 5 schedules VF-reset handling with no defined outcome; Q-LIFE-7 covers only link loss.
- **Options:** (a) pause and resume with `PORT_RESET` events, units in the gap `DROPPED/RECOVERY`; (b) ERROR; (c) the app re-creates.
- **Recommendation:** (a) where the backend can restore queues and flows; (b) otherwise.
- **Research:** R06 Q6, R13 Q8.
- **Your answer:**

## Completions and errors

### Q-CMP-1

> **Proposed default (r3):** (c) — C5 §1.7 counted it as argued at length, so it is a designer
> decision: NONE for library pools, ALL forced for attached, dynamic and exported pools. r3 adds
> `MTL_BLOCKED_APP_LEASES` and fails a non-zero `user_cookie` in NONE with
> `COOKIE_WITHOUT_RESULTS` ([07 §2](07-completions-events-and-errors.md)). Stands unless you
> object.

**Default completion mode, and the back-pressure it implies.**

- **Why it matters:** reliable results need reserved capacity; an app that never reads
  its CQ eventually cannot acquire. That is correct but surprising for the simplest apps.
- **Options:** (a) default ALL (every result); (b) default EXCEPTIONS; (c) default NONE
  for library pools, ALL for imported pools.
- **Recommendation:** revised after review C3: (c) — NONE by default for library pools, ALL forced for attached pools — because with ALL as a universal default the minimal loop that never reads results stalls silently after `pool.count` frames. `acquire` reports `blocked_on` (BUFFERS / RESULTS / RING) so a stall is never silent (C3 P1-1).
- **Research:** R01 Q10; R11 Q4; [07 §2](07-completions-events-and-errors.md).
- **Your answer:**

### Q-CMP-2

> **Proposed default (r3):** (b), shipped in v1 as the L4 dispatcher. Stands unless you object.

**Ship a library-thread callback helper** (a thread that waits on a CQ/EQ and calls the
app's function)?

- **Options:** (a) no; (b) a small L4 helper; (c) a first-class callback mode.
- **Recommendation:** (b), shipped in v1 (`mtl_cq_dispatch_start`).
- **Your answer:**

### Q-CMP-3

> **Proposed default (r3):** (c): `mtl_last_error(struct mtl_error_info*)` copies `{code, reason,
> call_seq, detail[128]}` into caller memory; one frozen `enum mtl_state_reason` ([07
> §5](07-completions-events-and-errors.md)). Stands unless you object.

**Error detail for failures with many causes (create, attach, start).**

- **Options:** (a) thread-local string `mtl_last_error_detail()`; (b) a reason enum
  out-parameter; (c) both.
- **Recommendation:** (a) now, (b) for the most common reasons later.
- **Research:** R13 R-L11; R06 Q13.
- **Your answer:**

### Q-CMP-4

> **Proposed default (r3):** (c): the slot hint at acquire, and an opt-in lossy `EPOCH_TICK` via
> the EQ subscription bit `MTL_EQ_SUB_EPOCH_TICK`. Stands unless you object.

**Epoch tick (today's `ST_EVENT_VSYNC`).**

- **Why it matters:** time-critical, but EQs are not a fast path; the slot hint at
  acquire covers most uses.
- **Options:** (a) slot hint only; (b) lossy opt-in EQ event; (c) both.
- **Recommendation:** (c).
- **Research:** R09 Q11; [07 §3.2](07-completions-events-and-errors.md).
- **Your answer:**

### Q-CMP-5

> **Proposed default (r3):** (a), with a per-CQ ready summary (one word per scheduler) and one
> armed word per CQ, so a read or an arm is O(active), not O(members) ([04
> §4.3](04-threading-and-execution.md)). Shared **EQs** move to Phase 1. Stands unless you object.

**Shared completion queues (one CQ for many sessions) in v1?**

- **Options:** (a) yes, as poll sets over the member sessions' lease tables; (b) later; private CQs only.
- **Recommendation:** (a): cheap with the poll-set construction and valued by
  libfabric-style apps.
- **Research:** R03 §7.2; R09 §5; [04 §4.3](04-threading-and-execution.md).
- **Your answer:**

### Q-CMP-6

> **Proposed default (r3):** (c), as recommended. Stands unless you object.

**What makes a TX unit `ON_TIME`?** *(new in r2)*

- **Why it matters:** an admission verdict (picked up by its deadline) is always available; a wire verdict needs HW TX timestamps, and SW observation at `tx_burst` is enqueue time under RL pacing.
- **Options:** (a) admission verdict; (b) wire verdict within a tolerance of `scheduled_first`; (c) status from (a), wire error reported separately.
- **Recommendation:** (c), with the tolerance reported in `get_info`.
- **Research:** R11 Q1, R06 Q3, R05 Q9, C1 #18, C4 C-05.
- **Your answer:**

### Q-CMP-7

> **Proposed default (r3):** (a), plus the stalled-queue branch: after the bounded cleanup a
> worker stops and restarts a dedicated queue (spike S8 checks the PMDs release chained mbufs)
> ([04 §4.5](04-threading-and-execution.md)). Stands unless you object.

**Idle descriptor recycling.** *(new in r2)*

- **Why it matters:** in chain mode a frame's result arrives only when a later burst recycles its descriptors (≈ 512 packets later) and never while the session is idle, so the last result never arrives and DRAIN cannot finish.
- **Options:** (a) the transmitter calls the non-blocking `rte_eth_tx_done_cleanup` on idle sessions with frames in flight, rate-limited, dedicated queues only in v1; (b) also on shared queues (TSQ takes a shared spinlock); (c) pad bursts instead.
- **Recommendation:** (a), measured by spike S6.
- **Research:** C1 #2; [04 §4.4](04-threading-and-execution.md).
- **Your answer:**

## Memory

### Q-MEM-1

> **Proposed default (r3):** (b) in **Phase 4**, opt-in, for COPY/CONVERT paths only (the
> framework-pool starvation of 05 §5.6 needs it); DIRECT rejects it. Stands unless you object.

**One terminal result, or an early `SOURCE_RELEASED` for copy/convert paths?**

- **Why it matters:** on copy/convert paths MTL stops reading the app buffer long before
  the transport finishes; holding the lease costs pool size.
- **Options:** (a) one terminal result after both (v1); (b) opt-in `SOURCE_RELEASED` +
  terminal (io_uring zero-copy model).
- **Recommendation:** (a) now, (b) later.
- **Research:** R00 §9; R04 Q1; R11 Q3; [05 §7](05-memory-and-buffers.md).
- **Your answer:**

### Q-MEM-2

> **Proposed default (r3):** (b) for hugepage memory, COPY-only otherwise; spike S4. Stands unless
> you object.

**Is IOVA-PA mode in scope for imported memory?**

- **Options:** (a) VA only; PA imports are COPY-only; (b) PA for hugepage imports via page
  tables; (c) drop PA for direct paths.
- **Recommendation:** (b) for hugepage memory, COPY-only otherwise.
- **Research:** R04 Q4; [05 §3.2](05-memory-and-buffers.md).
- **Your answer:**

### Q-MEM-3

> **Proposed default (r3):** (a) later, marked expert. Stands unless you object.

**Keep a raw-IOVA "pre-mapped" import** for today's `mtl_dma_map` users?

- **Options:** (a) yes, validated against the map table; (b) no, regions only.
- **Recommendation:** (a) later, marked expert.
- **Research:** R04 Q6.
- **Your answer:**

### Q-MEM-4

> **Proposed default (r3):** `-MTL_EBUSY` in v1; deferred release (`REGION_RELEASED`) later.
> Stands unless you object.

**Region destroy while referenced: `-EBUSY` only, or also a deferred-release mode**
that posts an event when the last reference drops?

- **Recommendation:** `-EBUSY` now; deferred release later.
- **Research:** R11 §4.5, §10 #18.
- **Your answer:**

### Q-MEM-5

> **Proposed default (r3):** Lift the cap in the engine (E11, Phase 2: dynamic frame arrays,
> bounded by memory and the 16-bit slot field); until then `max_count = 8` is reported and a
> larger count fails at query and create ([05 §5.2](05-memory-and-buffers.md)). Stands unless you
> object.

**Raise the 8-buffer cap for video pools?**

- **Why it matters:** deep pre-roll playout, frameworks holding many RX buffers, direct TX
  minimum counts.
- **Options:** (a) keep 8; (b) raise to e.g. 64 for the unified API; (c) no fixed cap,
  bounded by memory.
- **Recommendation:** (b).
- **Research:** R02 §4.7; [05 §5](05-memory-and-buffers.md).
- **Your answer:**

### Q-MEM-6

> **Proposed default (r3):** (c) as recommended. Stands unless you object.

**RX missing data: zero-fill, loss map, or both?**

- **Why it matters:** recycled RX frames are not re-zeroed today, so lost packets show the
  previous frame.
- **Options:** (a) report a coarse missing-range map only; (b) zero-fill library pools;
  (c) both, zero-fill opt-in.
- **Recommendation:** (c), with zero-fill the **default for library pools** (the repository rule is that gaps read as zeros) done in the consumer's `dequeue`, never on the RX tasklet; NONE default for attached pools (C3 P1-7, C1 #16).
- **Research:** R04 Q12; [05 §8](05-memory-and-buffers.md).
- **Your answer:**

### Q-MEM-7

> **Proposed default (r3):** (a): `-MTL_ENOTSUP`, reason `POOL_TOO_SMALL`; the minimum is 2 for
> common formats, 3 under 512 packets per unit ([05 §4.1](05-memory-and-buffers.md)). Stands
> unless you object.

**`REQUIRE_DIRECT` with a pool below the direct-TX minimum count.**

- **Options:** (a) fail; (b) succeed as COPY with a report; (c) MTL raises the count.
- **Recommendation:** (a) — report the minimum in `get_buffer_requirements` so apps can
  size correctly.
- **Research:** R04 Q2.
- **Your answer:**

### Q-MEM-8

> **Proposed default (r3):** (a); r3: GPU *pinned host* memory (`cudaHostAlloc`, `zeMemAllocHost`)
> is a plain host import in v1 and the supported GPU ingest path ([05
> §10](05-memory-and-buffers.md)). Stands unless you object.

**Device memory scope and "GPU direct".**

- **Why it matters:** today's "GPU direct" is CPU copy into Level Zero shared memory, RX
  only; no DMA-BUF, CUDA or gpudev exists.
- **Options:** (a) model as a COPY-only device domain until a direct path exists; (b)
  remove the flag, import USM as host memory; (c) scope real device DMA now.
- **Recommendation:** (a).
- **Research:** R04 Q11; [05 §10](05-memory-and-buffers.md).
- **Your answer:**

### Q-MEM-9

> **Proposed default (r3):** (a) for the unified API. Stands unless you object.

**Header split.**

- **Why it matters:** compiled out on the pinned DPDK 26.07, single port, pool layout
  incompatible with per-buffer handles.
- **Options:** (a) drop; (b) keep as an internal RX optimisation for library pools; (c)
  port the DPDK patch and design a region-pool import.
- **Recommendation:** (a) for the unified API; decide separately for legacy.
- **Research:** R04 Q10; R07 Q8.
- **Your answer:**

### Q-MEM-10

> **Proposed default (r3):** Changed: TX `MTL_POOL_DYNAMIC` with `mtl_tx_acquire_dynamic` in
> **Phase 4** (moved from later), completion forced ALL; RX `mtl_rx_provide` stays later ([05
> §5.5](05-memory-and-buffers.md)). Stands unless you object.

**Dynamic (unattached) buffer submit/provide in v1?**

- **Why it matters:** codecs and frameworks that cannot pre-register every surface.
- **Options:** (a) later capability; (b) v1.
- **Recommendation:** (a); fixed attached pools first (your review's baseline).
- **Research:** R00 §8.3.
- **Your answer:**

### Q-MEM-11

> **Proposed default (r3):** Superseded: a buffer belongs to one pool; sessions share bytes
> through buffers over one region, and RX → TX sharing uses hold counts
> (`mtl_tx_submission.hold`), so one RX unit can feed N TX sessions in the same frame period;
> `mtl_rx_transfer` stays as sugar (Phase 4) ([05 §5.4](05-memory-and-buffers.md)). Stands unless
> you object.

**Buffers shared by two sessions and RX→TX lease transfer in v1?** *(new in r2)*

- **Why it matters:** zero-copy forwarding (including the split-forward sample) needs one buffer visible to an RX and a TX session.
- **Options:** (a) a buffer may be attached to several sessions with one exclusive lease; `mtl_rx_transfer` turns an RX lease into a TX lease; (b) later.
- **Recommendation:** (a) in Phase 4.
- **Research:** C3 P5-3, C4 §4.1 #7; [03 §4.3](03-object-model-and-lifecycle.md).
- **Your answer:**

### Q-MEM-12

> **Proposed default (r3):** (b) unless spike S4 shows (a) is safe. Stands unless you object.

**For imported non-hugepage memory, keep MTL's invented-IOVA allocator (`mt_dma.c:21`, from `0x10000`) or require IOVA == VA?** *(new in r2)*

- **Why it matters:** IOVA == VA is simpler and matches DPDK VA mode but can collide with DPDK's own mappings; the invented range avoids that but is unchecked against the hugepage space.
- **Options:** (a) IOVA == VA; (b) invented range, made collision-safe.
- **Recommendation:** (b) unless spike S4 shows (a) is safe with `rte_extmem_register`.
- **Research:** R04 Q5.
- **Your answer:**

## Time

### Q-TIME-0

> **Maintainer decision M3.** Recommendation unchanged (a). Answering it unblocks Phase 0.5 (the
> legacy `st_timeline_*` helper, [06 §15](06-timing-pacing-and-sync.md)).

**Confirm the two-timestamp model.** Your PR comment asked for "one timestamp for user
pacing, one for user timestamp". The design interprets that as **media time** (what the
unit represents; always drives RTP) plus an optional **launch override** (NOT_BEFORE /
EXACT), with launch *derived* from media time by default.

- **Why it matters:** it decides every timing field and the answer to the A/V question.
- **Options:** (a) media time primary + optional launch override (standards model); (b)
  two independent required fields (pacing time, RTP/media time); (c) keep one timestamp +
  flags.
- **Recommendation:** (a).
- **Research:** R12 §10.2; R05 Q3; R08 Q6 (#1211 asks for "pass both"); [06 §4–5](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-24

> **Maintainer decision M3.** Recommendation (a), with the r3 CAPTURE default `min_tx_delay` = one
> unit period + the pick-up lead (L = 2 at 1080p59.94 for frame-based capture) and the §7.6.3
> bound replaced by `link_offset_budget_ns` warnings ([06 §5.1](06-timing-pacing-and-sync.md)).

**Confirm the source-kind model for live timing.** *(new in r2)*

- **Why it matters:** with a fixed slot delay of 0 every captured frame is late (it exists only after its sampling instant); with L ≥ 1 JT-NM default windows flag the stream; playback with L ≥ 2 breaks ST 2110-10 §7.6.3.
- **Options:** (a) `source_kind` = PLAYBACK / CAPTURE / GATEWAY with `min_tx_delay_ns` (slot = first N whose first-packet time ≥ M + min_tx_delay), L reported; (b) an integer slot delay only.
- **Recommendation:** (a).
- **Research:** C2 #1; [06 §5.1](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-1

> **Proposed default (r3):** (b), plus: FFmpeg's `CLOCK_TAI` on a host without ptp4l runs as
> `SYSTEM_TAI` labelled ESTIMATED with a `timing_warning` (a documented behaviour change). Stands
> unless you object.

**Time source default, and timed sessions without a locked TAI source.**

- **Why it matters:** the default today is `CLOCK_REALTIME` (UTC) labelled TAI.
- **Options:** (a) refuse `INDEX`/`TAI` media modes without a locked TAI source; (b)
  allow with an explicitly labelled estimated clock (configured UTC–TAI offset) and a
  warning event; (c) status quo.
- **Recommendation:** (b), and add external PHC / `CLOCK_TAI` sources.
- **Research:** R05 Q8; [06 §2.2](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-2

> **Proposed default (r3):** As recommended, plus the r3 AUTO-cursor rule for backward steps on
> the epoch timeline ([06 §2.4](06-timing-pacing-and-sync.md)). Stands unless you object.

**Clock steps** (UTC→PHC at first PTP sync; grandmaster change).

- **Options:** (a) `TIME_STEP` event, sessions keep media indices and re-derive launch;
  (b) sessions go through a stream reset (units in flight dropped with a reason); (c)
  session error.
- **Recommendation:** revised after review C2: a per-timeline `step_policy` — REANCHOR (T0 += step, implicit DISCONTINUITY, the default for non-epoch timelines, so a file timeline does not drop forever after the +37 s UTC→PHC step) or KEEP (the epoch timeline); AUTO never emits a smaller index after a backward step; always evented.
- **Research:** R05 Q8, F2.
- **Your answer:**

### Q-TIME-3

> **Proposed default (r3):** (b), measured from max(now, resolved start) ([06
> §7.4](06-timing-pacing-and-sync.md)). Stands unless you object.

**Horizon (how far ahead a unit may be submitted).**

- **Options:** (a) fixed 1 s; (b) configurable, default 1 s; (c) derived from pool depth.
- **Recommendation:** (b).
- **Research:** R05 Q13.
- **Your answer:**

### Q-TIME-4

> **Proposed default (r3):** (a) later (Phase 6). Stands unless you object.

**Underrun `REPEAT_LAST`** (resend the previous unit when nothing is submitted in time).

- **Why it matters:** live broadcast often prefers a freeze to a gap; needs buffer
  retention rules.
- **Options:** (a) later capability; (b) v1; (c) never.
- **Recommendation:** (a).
- **Research:** R11 Q2; R10 §2.6; [06 §7.3](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-5

> **Proposed default (r3):** (a): whole-unit `media_index_offset` for lip-sync, and r3
> `media_time_offset_ns` (ns) for the compliance trim and the framework latency; legacy keeps
> `rtp_timestamp_delta_us`. Stands unless you object.

**`rtp_timestamp_delta_us`** (in effect a non-zero RTP clock offset, forbidden by ST
2110-10 §7.3, used as a sampling-instant correction and by one downstream engine to fake
a TROFF shift).

- **Options:** (a) replace by an integer media-index offset (compliant) + an explicit
  TROFFSET knob; (b) keep, documented as non-compliant; (c) drop.
- **Recommendation:** (a) in the new API; legacy keeps (b).
- **Research:** R12 Q5; R08 §3.3.
- **Your answer:**

### Q-TIME-6

> **Proposed default (r3):** (a) + (b) made concrete: CAPTURE and TAI absorb ±one packet by
> default; forward gaps keep the packet grid with silence fill; overlaps are trimmed; only an
> explicit DISCONTINUITY re-phases ([06 §8](06-timing-pacing-and-sync.md)). Stands unless you
> object.

**Audio continuity.**

- **Why it matters:** live sound-card capture drifts against PTP; the contract's strict
  "must equal the grid point or be rejected" forces resets.
- **Options:** (a) strict contiguity + explicit `DISCONTINUITY` to re-anchor; (b) a
  tolerance within which MTL keeps the grid and reports drift; (c) re-anchor every
  submission (today).
- **Recommendation:** (a) + (b).
- **Research:** R05 Q7, §8.2; [06 §8](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-7

> **Proposed default (r3):** (a). Stands unless you object.

**Pacing engine: per port (today) or requested per session?**

- **Why it matters:** ST22 silently drops to SW pacing; a shared TX queue forces the whole
  port to SW; a runtime RL failure flips the port while sessions still believe RL.
- **Options:** (a) per port, sessions request a class and see the granted value; (b) per
  session; (c) status quo.
- **Recommendation:** (a).
- **Research:** R05 Q10; R07 Q3; [06 §6](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-8

> **Proposed default (r3):** (a), with the written restart sequence: stop the member, fix,
> `mtl_session_start(member, START_NOW)` rejoins a RUNNING group ([06
> §10.6](06-timing-pacing-and-sync.md)). Stands unless you object.

**When one group member fails, do the others continue?**

- **Options:** (a) continue, event only; (b) stop the group; (c) per-group policy.
- **Recommendation:** (a) default, (c) later.
- **Your answer:**

### Q-TIME-9

> **Proposed default (r3):** Not in v1. Stands unless you object.

**Allow media-time-ordered submission instead of strict FIFO?**

- **Recommendation:** not in v1; FIFO with strictly increasing media time.
- **Research:** R01 Q11.
- **Your answer:**

### Q-TIME-10

> **Proposed default (r3):** Spike S5; ship SW-estimated (`enqueued_first`) first. Stands unless
> you object.

**Which observed TX times can E810/E830 provide, with what accuracy?**

- **Why it matters:** the result's `observed_first/last` are only honest if the source is
  known; DPDK does not report lateness and sends past launch times immediately.
- **Options to investigate:** TSC at `tx_burst` (SW), per-packet HW TX timestamps,
  TSN launch-time confirmation on E830.
- **Recommendation:** spike S5; ship SW-estimated with `flags = SW | ESTIMATED` first.
- **Research:** R11 Q6; R05 Q11.
- **Your answer:**

### Q-TIME-11

> **Proposed default (r3):** (d) as revised in r2. Stands unless you object.

**Audio launch offset default** (today packets leave exactly at their RTP instant; any
early wire error makes the RTP time "in the future", failing JT-NM).

- **Options:** (a) one packet time; (b) configurable, default 0; (c) configurable,
  default ptime/2; (d) derived from the granted pacing profile's worst early error.
- **Recommendation:** revised after review C2: (d) for PLAYBACK, clamped to ptime/2 (tens of µs on RL/TSN; one ptime would exceed JT-NM's 1 ms limit at Level A); CAPTURE uses `min_tx_delay`; reported as TSDELAY.
- **Research:** R12 Q11.
- **Your answer:**

### Q-TIME-12

> **Proposed default (r3):** Scheduled delivery stays later; r3 makes an RX group's
> `link_offset_ns` v1 configuration (presentation = media + link offset) ([06
> §11](06-timing-pacing-and-sync.md)). Stands unless you object.

**RX scheduled delivery** (hold units until `media + link_offset`).

- **Recommendation:** later; v1 reports the presentation time.
- **Research:** R12 Q13.
- **Your answer:**

### Q-TIME-13

> **Proposed default (r3):** As recommended. Stands unless you object.

**Progressive TX: what happens when rows are not published by their packet deadline?**

- **Options:** STALL (today's slice behaviour, breaks pacing), PAD, TRUNCATE; per session.
- **Recommendation:** default TRUNCATE with a result, PAD opt-in, STALL legacy only.
- **Research:** R00 §12; [06 §9](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-14

> **Proposed default (r3):** As revised in r2; the RX due time uses the tolerated skew budget.
> Stands unless you object.

**ST 2022-7 skew class claimed for dual-port RX.**

- **Options:** (a) class D (150 µs) documented; (b) configurable path-differential
  window; (c) class A (10 ms) default.
- **Recommendation:** revised after review C2: report the *tolerated* path differential; default to class A (10 ms) where memory allows (cheap for audio/ANC); warn when `link_offset` is smaller than the budget.
- **Research:** R12 Q14.
- **Your answer:**

### Q-TIME-15

> **Maintainer decision M7.** r3 decided for the unified API: `floor` (C5 §1.7 called this a
> designer decision). **For you:** the legacy wire policy. Designer recommendation: every
> wire-visible engine change (±1 tick rounding, default video RTP, ANC RTP, ST22 CBR, linear read
> schedule) is off by default on the legacy API behind an opt-in flag, on by default in the
> unified API, with a pcap-diff gate (G-99, [14 §2](14-implementation-roadmap.md)).

**RTP rounding: `floor` (standards, contract) everywhere, or only in the new API?**

- **Why it matters:** code and unit tests pin round-to-nearest; at 59.94/23.976 half the
  frames differ by one tick. Also the ns tie rule (contract: later; code: earlier).
- **Options:** (a) floor in the new API, legacy unchanged; (b) floor everywhere; (c)
  keep rounding.
- **Recommendation:** (a), then (b) with a release note once S2 shows interop impact is
  nil.
- **Research:** R05 Q2; R12 Q8.
- **Your answer:**

### Q-TIME-16

> **Maintainer decision M7.** Decided with Q-TIME-15 as one legacy wire policy: (b) with the flag
> default off (`ST20_TX_FLAG_RTP_FROM_MEDIA_TIME`); revisit the default at the ABI freeze.

**Flip the legacy default video RTP to the frame epoch too?**

- **Why it matters:** today MTL's own ST20 and ST40 streams carry different RTP for the
  same frame (~57 ticks at 1080p59.94).
- **Options:** (a) new API only; (b) legacy too, keeping the old behaviour behind a flag;
  (c) never for legacy.
- **Recommendation:** (a) now, (b) in a later release.
- **Research:** R05 Q1; R12 Q1.
- **Your answer:**

### Q-TIME-17

> **Proposed default (r3):** Yes in the unified API; on the legacy API behind
> `ST20_TX_FLAG_LINEAR_SCHEDULE` (the M7 policy). Stands unless you object.

**Sender type W: switch TX wide pacing and the RX timing parser to the linear read
schedule (ST 2110-21:2022 §7.1.4), and add explicit NL?**

- **Recommendation:** yes, in the new APIs sender-type knob; legacy after measurement.
- **Research:** R12 Q7.
- **Your answer:**

### Q-TIME-18

> **Maintainer decision M4.** r3 designer answer, kept for the maintainer because it is a reading
> of ST 2110-40: **(d)** ANC for video unit k keeps RTP = video RTP and is sent in the -40 window
> of the frame in which video unit k is actually transmitted (N = E⁻¹(M) + L_v); strict
> `MTL_ANC_WINDOW_MEDIA` is opt-in. Rationale in [06 §5.5](06-timing-pacing-and-sync.md).

**Live ANC with video slot delay L ≥ 1: which frame anchors the ST 2110-40 window?** *(new in r2)*

- **Why it matters:** if the window is anchored on the frame the ANC RTP denotes, ANC sent L frames later misses it by ≈ 15 ms at 1080p59.94; ANC RTP ≠ video RTP would break ST 2110-40 §5.4.
- **Options:** (a) ANC is always L = 0 relative to its RTP, and video L ≥ 1 is allowed only without an associated ANC session in a group; (b) ANC follows video L and is flagged non-compliant; (c) live ANC requires the gateway model (L = 0).
- **Recommendation:** re-read ST 2110-40 §6 first; then (a) or (c).
- **Research:** C2 #1.
- **Your answer:**

### Q-TIME-19

> **Proposed default (r3):** (b), plus: interlaced members contribute TFRAME (not TFIELD), and
> grid fastmeta joins G ([06 §3.3](06-timing-pacing-and-sync.md)). Stands unless you object.

**Common grid rules.** *(new in r2)*

- **Why it matters:** 1001 families + 44.1 kHz need a 3.34 s grid; mixed video families need 20–40 s; ST41 rates are unbounded.
- **Options:** (a) exact coincidence for every member; (b) G is the smallest period on which every video/ANC member starts on an integer frame and 90 kHz and every audio rate tick an integer number of times, except audio rates that would push G above the 1 s cap: those are excluded and floor-aligned with the sub-sample phase reported; ST41 excluded; a video/ANC grid above the cap is `-EINVAL`.
- **Recommendation:** (b).
- **Research:** C2 #3; [06 §3.3](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-20

> **Proposed default (r3):** (a). Stands unless you object.

**Timecode and cadence helpers.** *(new in r2)*

- **Why it matters:** broadcast users expect ST 12-1 time address from media time (via `currentUtcOffset`, local offset and daily jam from ST 2059-2 SM TLVs; drop-frame at 1001 rates), RP 188 / ST 12-2 insertion into ST40, and a 1001 audio-cadence helper (Dolby E needs frame-aligned audio).
- **Options:** (a) L4 helpers later; (b) in v1; (c) out of scope.
- **Recommendation:** (a).
- **Research:** C2 #27.
- **Your answer:**

### Q-TIME-21

> **Proposed default (r3):** As recommended ((a) shim, (c) new); the offset is 604–619 µs at
> 1080p59.94 depending on VRX0. Stands unless you object.

**How does legacy USER_PACING map onto the new model?** *(new in r2)*

- **Why it matters:** today it snaps to the nearest *epoch*; the contract snaps to the nearest *slot* (N·T + packet-0 offset); NOT_BEFORE never sends early. Migrated apps' TX moves by ≈ TROFFSET − VRX0·TRS (≈ 604 µs at 1080p59.94) depending on the choice.
- **Options:** (a) shim keeps nearest-epoch; (b) nearest-slot (contract); (c) NOT_BEFORE.
- **Recommendation:** (a) for the legacy shim, (c) as the documented new-API equivalent.
- **Research:** R05 Q6, C2 #13, C4 C-44.
- **Your answer:**

### Q-TIME-22

> **Proposed default (r3):** (a) in the engines track (E7), on by default in the unified API and
> opt-in on the legacy API. Stands unless you object.

**Is CTM/LLTM-conformant ANC packet scheduling in scope for v1?** *(new in r2)*

- **Why it matters:** it is a wire change for multi-packet ANC; today packets are spread over the whole frame (`st_tx_ancillary_session.c:1108`).
- **Options:** (a) v1 with a deterministic target delay (`anc_target_delay_ns`); (b) later.
- **Recommendation:** (a) in Phase 3.
- **Research:** R05 Q12.
- **Your answer:**

### Q-TIME-23

> **Proposed default (r3):** (a), refined: **the media mode wins** — AUTO → RESLOT, INDEX/TAI →
> DROP for every source kind; a CAPTURE session raises `TIMING_INFEASIBLE` on its first late unit.
> Stands unless you object.

**Default late policy, and reject vs drop for too-late requests.** *(new in r2)*

- **Why it matters:** the contract REJECTS too-late requests (no media unit consumed, RTP not advanced); this design accepts and DROPs at pick-up (RTP slot consumed) because lateness is only known then.
- **Options:** (a) DROP default for PLAYBACK/CAPTURE, RESLOT for AUTO, synchronous `-ERANGE` only when knowable at submit; (b) DROP everywhere; (c) SEND_LATE (bounded) for live.
- **Recommendation:** (a).
- **Research:** R05 Q5, R10 Q4, R11 Q2, C2 #13.
- **Your answer:**

### Q-TIME-25

> **Proposed default (r3):** Changed: NEAREST (with a TFRAME/8 hysteresis band) for **every**
> source kind, so a free-running source adapts by drop/gap and never locks out; LOCKED_PHASE
> opt-in with a relock after 3 consecutive OFF_GRID ([06 §4.3](06-timing-pacing-and-sync.md)).
> Stands unless you object.

**TAI-mode snap modes and tolerance defaults.** *(new in r2)*

- **Why it matters:** nearest-unit snapping of jittery capture timestamps creates duplicates and gaps and discards the camera's sampling phase.
- **Options:** (a) NEAREST_EPOCH for PLAYBACK, LOCKED_PHASE (phase locked at the first unit, tolerance TFRAME/4) for CAPTURE; (b) nearest only.
- **Recommendation:** (a).
- **Research:** C2 #5; [06 §4.3](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-26

> **Proposed default (r3):** New in r3 (ST 2110-22 rate mode): **CBR default**, because -22
> requires constant bytes and packets per frame; `VBR_MAX` (today's behaviour) opt-in and flagged
> non-compliant; an oversize codestream fails at submit with `-MTL_ENOSPC` ([06
> §5.2](06-timing-pacing-and-sync.md)). Stands unless you object.

**CBR (constant bytes and packets per frame, padded) or today's variable packet count?**

- **Why it matters:** C5 §5.12 found that a strict CBR with deferred drops is hostile to encoders that are not byte-exact, while ST 2110-22 requires constant bytes and packets per frame `[R12 §6]`.
- **Options:** (a) CBR default with a per-unit ceiling and synchronous oversize rejection, VBR_MAX opt-in; (b) VBR_MAX default.
- **Recommendation:** (a).
- **Research:** R12 §6; C5 §5.12; [06 §5.2](06-timing-pacing-and-sync.md).
- **Your answer:**

### Q-TIME-27

> **Proposed default (r3):** New in r3 (RX relock): relock after 3 distinct increasing stale RTP
> values in a row; configurable later if field data asks for it ([06
> §11.5](06-timing-pacing-and-sync.md)). Stands unless you object.

**After how many stale units does a receiver relock onto a restarted sender?**

- **Why it matters:** a sender restart with a backward RTP produces stale units forever unless the receiver relocks; today's implicit rule is 20 consecutive redundant errors per packet on every port.
- **Options:** (a) 3 distinct increasing stale RTP values in a row, per unit; (b) configurable from v1.
- **Recommendation:** (a).
- **Research:** C5 §5 note; [06 §11.5](06-timing-pacing-and-sync.md).
- **Your answer:**

## Observability

### Q-OBS-1

> **Proposed default (r3):** Changed to (a) cumulative only: `new_epoch` is removed, `*_max`
> values become windowed maxima (1 s, 60 s) plus histograms ([08 §2.3](08-observability.md)).
> Stands unless you object.

**Stats epochs instead of reset.**

- **Why it matters:** reset-on-demand breaks as soon as two readers exist.
- **Options:** (a) cumulative only; (b) cumulative + explicit "new epoch" with a start
  time; (c) keep reset.
- **Recommendation:** (b) for the new API; legacy reset unchanged.
- **Research:** R06 Q4; R11 Q8; [08 §2.3](08-observability.md).
- **Your answer:**

### Q-OBS-2

> **Proposed default (r3):** Later. An in-process exporter needs only `mtl_instance_list_sessions`
> + `get_info` + `get_stats` ([08 §7](08-observability.md)). Stands unless you object.

**Out-of-process stats reader** (Rivermax has only this kind).

- **Recommendation:** later, possibly via MtlManager shared memory.
- **Research:** R10 Q13.
- **Your answer:**

### Q-OBS-3

> **Proposed default (r3):** Opt-in per session; when enabled its summary is part of v1 (integer
> math, HW-timestamped packets only). Stands unless you object.

**Timing-parser summary counters always on?**

- **Options:** (a) cheap summary always on, per-unit detail opt-in; (b) fully opt-in.
- **Recommendation:** revised after review C1: keep the parser **opt-in**; if a summary is always on, compute it only for HW-timestamped packets with integer math (today it does double divisions per packet).
- **Research:** R06 Q11.
- **Your answer:**

## Modes

### Q-MODE-1

> **Answered by the maintainer, 2026-10-01:** RTP passthrough must stay supported in a coherent
> API, and `include/st20_api.h` should stop being public, so the high-level API must provide
> it. Revision 4 makes packet units first class (D-82, [S8](simplification/S8-rtp-passthrough.md));
> the remaining choices are M14.

**RTP/packet level in the unified API.**

- **Why it matters:** the only route to ST 2022-6; what Rivermax users expect; different
  ownership model (mbufs today).
- **Options:** (a) keep in the legacy API for v1, reserve `MTL_UNIT_PACKET_CHUNK`; (b)
  a separate `mtl_rtp_*` API; (c) drop.
- **Recommendation:** (a).
- **Research:** R07 Q1; R10 Q2; [09 §2.1](09-media-modes-and-backends.md).
- **Your answer:**

### Q-MODE-2

> **Proposed default (r3):** DSCP (0 = CS0, today's value) and TTL (0 = 64) in `mtl_flow` for v1;
> VLAN and IPv6 reserved. Stands unless you object.

**Network header controls: DSCP, TTL, VLAN, IPv6.**

- **Why it matters:** broadcast plants mark DSCP; today TOS = 0 and TTL = 64 are
  hard-coded, no VLAN, IPv4 only.
- **Recommendation:** DSCP and TTL in `mtl_flow` for v1; VLAN and IPv6 fields reserved.
- **Research:** R07 Q11.
- **Your answer:**

### Q-MODE-3

> **Proposed default (r3):** Its own create, named `mtl_cvideo_session_create` (r3 naming). Stands
> unless you object.

**ST22: its own create function, or a codec sub-struct in the video config?**

- **Recommendation:** its own create (`mtl_st22_session_create`); PR #1610's
  `bool compressed` silently created ST20.
- **Research:** R01 Q13, D5.
- **Your answer:**

### Q-MODE-4

> **Proposed default (r3):** As recommended. Stands unless you object.

**Codec/converter plugin ABI** (`st_plugin_*`, `st22_encoder/decoder_*`,
`st20_converter_*`): freeze, redesign on the same acquire/submit primitives, or replace?

- **Also:** whether the ~105 colour-conversion functions (`st_convert_api.h`, `st_convert_internal.h`) move out of the core library or headers.
- **Recommendation:** freeze the plugin ABI for v1; redesign later; move the conversion functions to a separate header/library.
- **Research:** R08 Q8.
- **Your answer:**

### Q-MODE-5

> **Proposed default (r3):** As recommended (verify with a test; not in the unified API v1).
> Stands unless you object.

**`DATA_PATH_ONLY` (app-managed flows).**

- **Why it matters:** it appears to dereference a NULL flow on non-socket backends (SP-02).
- **Options:** (a) drop; (b) fix with a test and keep; (c) replace with an "external
  steering" port mode.
- **Recommendation:** verify with a test first; not in the unified API v1.
- **Research:** R07 Q9.
- **Your answer:**

### Q-MODE-6

> **Proposed default (r3):** (b): every SDP value in `mtl_session_get_info` in v1 ([08
> §3.1](08-observability.md)); an L4 parse helper for -20/-30/-40 in Phase 2. Stands unless you
> object.
>
> **Revision 4, addendum N:** superseded by D-94 (M17): `mtl_sdp.h` renders and parses in
> v1 as an optional helper on public calls, because every NMOS and IPMX sender must publish
> an SDP on each activation ([17 §2.3](17-nmos-and-ipmx.md)).

**SDP.**

- **Options:** (a) SDP text helper in `lib/` for 2110-20/-30/-40 (render and parse);
  (b) values only via `get_info` (TP, TROFF, CMAX, TSMODE, mediaclk, ts-refclk), text
  generation in `ecosystem/` or `app/`; (c) nothing.
- **Recommendation:** (b) in v1, plus an L4 *parse* helper for -20/-30/-40 considered for v1 — it is the largest single migration win for Rivermax users (C3 P6-3).
- **Research:** R10 Q1; R12 Q12; [08 §4](08-observability.md).
- **Your answer:**

### Q-MODE-7

> **Proposed default (r3):** As recommended. Stands unless you object.

**Fate of legacy/experimental surfaces:** `st20rc`, DPDK AF_XDP and AF_PACKET PMDs, SysV
shm lcore allocator, UDP remnants, non-RFC 4175 transport formats, `st_convert_internal.h`
installed as public.

- **Recommendation:** remove the UDP remnants now; freeze the rest; decide removal per
  release. *Addendum K:* the SysV shm lcore allocator is replaced, not frozen: it arbitrates
  nothing across PID namespaces (D-92, M16; [16 §5](16-kubernetes-and-crash-safety.md)).
- **Research:** R07 Q14; R03 Q12.
- **Your answer:**

### Q-MODE-8

> **Proposed default (r3):** (a), later. Stands unless you object.

**RTCP retransmission scope.** *(new in r2)*

- **Why it matters:** it is non-standard and implemented only for ST20/ST22; the ST30/40/41 flags are no-ops today.
- **Options:** (a) video only, remove the no-op flags elsewhere; (b) generalise; (c) later.
- **Recommendation:** (a), later.
- **Research:** R07 Q10.
- **Your answer:**

## ABI

### Q-ABI-1

> **Maintainer decision M8.** r3: (b), made concrete: libmtl gets a soname and version script in
> Phase 0; the unified API ships as its own DSO `libmtl_unified.so.0.<rev>` (soname bumped on
> every incompatible pre-freeze change, node `MTL_UNIFIED_EXPERIMENTAL`) and moves to `MTL_1.0` at
> the freeze ([11](11-abi-compatibility-and-migration.md)).

**ABI promise level.**

- **Options:** (a) API-only stability, bump soname every release; (b) stable ABI for the
  new API (soname, version script, hidden visibility), legacy unversioned; (c) full ABI
  with symbol versions for everything.
- **Recommendation:** (b), with the new API experimental until the Phase 6 freeze.
- **Research:** R13 Q1; R02 Q1; [11 §2](11-abi-compatibility-and-migration.md).
- **Your answer:**

### Q-ABI-2

> **Proposed default (r3):** Yes, and earlier: **Phase 0**, before the new API (A11). Stands
> unless you object.

**Versioned `mtl_init` replacement** (`struct_size`, API version argument, no mutation
of the caller's params, no 64-bit enum).

- **Recommendation:** yes, Phase 2 or 3.
- **Research:** R09 Q8; R13 §4.
- **Your answer:**

### Q-ABI-3

> **Proposed default (r3):** (b), must be NULL in v1; (c) later. r3 states where each knob lives
> (11 §2): fields, media configs, `next` blocks, key/value options, flags. Stands unless you
> object.

**Reserve a `next` extension chain in create-time configs?**

- **Options:** (a) `struct_size` only; (b) reserve a `next` chain on create-time configs; (c) named, versioned backend-extension tables (`mtl_open_ext(obj, name, version, &ops)`, libfabric `fi_open_ops`) for DATA_PATH_ONLY, AF_XDP knobs and queue meta.
- **Recommendation:** (b), must be NULL in v1; (c) for backend extensions later.
- **Research:** R11 Q9; [11 §2.2](11-abi-compatibility-and-migration.md).
- **Your answer:**

### Q-ABI-4

> **Proposed default (r3):** Yes for pipelines, as a commitment with a go/no-go at the Phase 2
> exit; session-level APIs frozen at the ABI freeze. Stands unless you object.

**Re-base the legacy pipeline APIs on the new core later?**

- **Why it matters:** fixes then land once; the shim needs a binary-identical `st_frame`
  adapter; session callbacks and RTP/slice modes cannot be shimmed without a thread hop.
- **Recommendation:** yes for pipelines (Phase 6); freeze the session-level APIs.
- **Research:** R08 §5.2; R13 Q14.
- **Your answer:**

### Q-ABI-5

> **Proposed default (r3):** Header `include/mtl/experimental/mtl_unified.h` (plus `mtl_simple.h`,
> `mtl_debug.h`) until the freeze, then `include/mtl/`; prefix `mtl_` with essence names (video,
> cvideo, audio, anc, fastmeta) and `mtl_tx_*`/`mtl_rx_*` verbs; source `lib/src/unified/`; DSO
> `libmtl_unified` (A6). Stands unless you object.

**Names.**

- **Options for the header:** `include/mtl_session_api.h` (PR #1610's name), or a new
  `include/mtl/` family. **Prefix:** `mtl_` + object noun and `mtl_tx_*`/`mtl_rx_*` verbs
  (review §13). **Source:** `lib/src/unified/`.
- **Recommendation:** as listed; avoid reusing PR #1610's names if the PR stays open, to
  avoid confusion.
- **Your answer:**

### Q-ABI-6

> **Proposed default (r3):** Changed to (a)-lite: the header is one ABI on every OS — portable
> wait objects (`MTL_WAIT_WIN_HANDLE`) and `MTL_E*` codes — with a compile-only Windows CI job in
> v1; runtime support follows the legacy library's ([15 §8.4](15-security-and-deployment.md)).
> Stands unless you object.

**Windows parity for the new API.**

- **Why it matters:** wait objects cannot be eventfd-based there; 64-bit flag enums must
  go.
- **Options:** (a) first-class (event `HANDLE` wait objects); (b) build and run, but the
  fd API returns `-ENOTSUP`; (c) Linux only.
- **Recommendation:** (b) for v1.
- **Research:** R13 Q12.
- **Your answer:**

### Q-ABI-7

> **Proposed default (r3):** Yes. Stands unless you object.

**Formalise C11 for `lib/`** (atomics are already used in 13 files) while public headers
stay C99/C++-clean?

- **Recommendation:** yes; update the coding instructions and `CLAUDE.md`.
- **Research:** R13 Q11.
- **Your answer:**

## Migration

### Q-MIG-1

> **Maintainer decision M10.** r3: raised from P to a Phase 0 exit criterion — a design review
> with at least three named external consumers (FFmpeg/GStreamer plugin owners, the MXL team, the
> external engine team) plus the private-user questionnaire ([14
> §1.5](14-implementation-roadmap.md)).

**Survey private and external users before freezing the design?**

- **Why it matters:** about 10 private repositories (media proxy/mesh, NMOS integration, a
  vendor origin service, an LED-wall receiver, validation frameworks) plus the external
  team behind the #11xx–#13xx issues are the real ABI users; their usage was not
  inspected.
- **Recommendation:** yes: a short questionnaire (layer, flags, ext frames, callbacks vs
  blocking, timing) before Phase 1 freezes the header.
- **Research:** R08 Q10.
- **Your answer:**

### Q-MIG-2

> **Proposed default (r3):** As recommended; every canonical sample also runs on the null backend
> in CI (G-97). Stands unless you object.

**Canonical sample set for the new API.**

- **Recommendation:** ~6 samples (TX/RX × simple / zero-copy / timed A/V group) plus a
  framework-style epoll example; move the rest to `legacy/`.
- **Research:** R08 Q13.
- **Your answer:**

### Q-MIG-3

> **Maintainer decision M9.** Recommendation unchanged ((b) then (a)), plus r3: salvage
> `lib/src/new_api/mt_session_event.c` for SF-15 in Phase 0.5, merged with credit to the PR's
> authors.

**What happens to PR #1610?**

- **Options:** (a) close as superseded, with credit, and cherry-pick concepts by hand;
  (b) keep open as a reference until the new header lands; (c) rebase pieces.
- **Recommendation:** (b) then (a).
- **Research:** R01 Q14, §6; [11 §6](11-abi-compatibility-and-migration.md).
- **Your answer:**

## Plan

### Q-PLAN-1

> **Maintainer decision M10.** New in r3. Designer recommendation: accept Phase 0.5 and the
> engines-first track as the first deliverables (they pay legacy users on their own), and choose a
> staffing scenario from [14 §4](14-implementation-roadmap.md) (≈ 55–85 EM total; 1.5 FTE →
> 3.1–4.7 years, 3 FTE → 18–28 months). "Stop after Phase 2" is a coherent product if the plan is
> not staffed at full size.

**Accept Phase 0.5 and the engines-first track as the first deliverables, and pick a staffing scenario.**

- **Why it matters:** C5 §1.1 measured the plan at ≈ 50–75 EM with no user-visible result before Phase 1 exit; revision 3 is ≈ 55–85 EM but delivers the A/V answer on the legacy API in Phase 0.5 and every engine fix to legacy users.
- **Options:** (a) the full plan at 3 FTE (18–28 months); (b) the full plan at ≈ 1.5 FTE (3.1–4.7 years); (c) Phase 0.5 + engines + Phases 1–2 only ("stop after Phase 2"), then decide.
- **Recommendation:** start (c) now, decide (a) or (b) at the Phase 2 go/no-go.
- **Research:** C5 §1.1, §1.5; [14 §4](14-implementation-roadmap.md).
- **Your answer:**

## Revision 4

The detail of each decision is in [REVISION-4.md §5](REVISION-4.md#5-what-needs-your-decision).

### Q-R4-1

> **Maintainer decision M11.** Designer recommendation: accept as one package.

**Accept the revision-4 shape (D-71…D-81, D-84, D-85, D-88).**

- **Why it matters:** it is what makes the include file lean (the core is `mtl.h`, 32 functions)
  and the examples short. It changes rules earlier reviews set:
  - options instead of typed fields for the long tail (A8/D-46);
  - `MTL_INIT` instead of per-struct init functions (D-22);
  - slots by index instead of buffer handles (C5 §2.5 wording);
  - results on/off instead of three completion modes (D-35);
  - one queue type instead of CQ and EQ (D-61), start arrays instead of groups (D-12);
  - a stats registry instead of fixed structs (D-20 shape);
  - `-MTL_EAGAIN` for every "nothing now" (C2), submit consuming the lease on failure (03 §4.1);
  - `mtl_session_close` instead of destroy (D-30 detail).
- **Options:** (a) accept the package; (b) accept it but keep one rule of revision 3 (name it); (c) keep revision 3.
- **Recommendation:** (a).
- **Research:** [S2](simplification/S2-object-model.md), [S3](simplification/S3-configuration.md), [S4](simplification/S4-data-path-and-memory.md), [S5](simplification/S5-observability-and-errors.md), [S6](simplification/S6-minimal-alternative.md), [S7](simplification/S7-samples-friction.md).
- **Your answer:**

### Q-R4-2

> **Maintainer decision M12.** Designer recommendation: remove every row of the table in
> REVISION-4 §5 unless the M10 survey finds an external user.

**Approve or reject each legacy capability proposed for removal.**

- **Why it matters:** the owner's rule is that no current use case is cut without approval.
  These are the rows of the coverage inventory ([S1 §6](simplification/S1-coverage-inventory.md),
  [S9 §7.2](simplification/S9-hiding-session-headers.md)) that revision 4 does not carry over:
  header split, `uframe_pg_callback`, `st20rc`, the public per-pair converters, public user DMA
  and lcore borrowing, user schedulers and tasklets, `DISABLE_BOXES`, the no-op RTCP flags on
  audio/ANC/fastmeta, queue meta / `DATA_PATH_ONLY`, thin wrappers, dead fields, and the DPDK
  AF_XDP/AF_PACKET PMD backends.
- **Options:** per row: remove / keep (the "if kept" column says where it would go).
- **Recommendation:** remove, pending the survey.
- **Research:** [R4 coverage check](simplification/R4-coverage-check.md).
- **Your answer:**

### Q-R4-3

> **Maintainer decision M13.** Designer recommendation: accept.

**Wire-visible defaults of the unified API.** The default video RTP from the frame epoch; incomplete RX frames delivered with their status instead of dropped; ST 2110-22 constant bitrate; packet mode verbatim by default. The legacy API keeps today's behaviour (M7).

- **Options:** (a) accept the four; (b) keep today's behaviour for some (name them).
- **Recommendation:** (a).
- **Research:** [S1 §6](simplification/S1-coverage-inventory.md) (U-155, U-202, U-233), [S8 §4.5](simplification/S8-rtp-passthrough.md).
- **Your answer:**

### Q-R4-4

> **Maintainer decision M14.** Designer recommendation: accept S8's recommendations and phase 2P.

**RTP passthrough defaults and phase.** Q-PKT-1…9 of [S8 §9](simplification/S8-rtp-passthrough.md):
verbatim by default; RX chunks formed at dequeue by count or timeout; generic RTP essence for RTP
only; per-packet launch offsets later; RX copy in the caller by default with a bounded zero-copy
lend; no RX reordering; ST 2022-6 headers written by the application; test fault injection through
packet mode; packet mode as phase 2P right after Phase 2.

- **Options:** accept, or change single items.
- **Recommendation:** accept.
- **Your answer:**

### Q-R4-5

> **Maintainer decision M15.** Designer recommendation: accept.

**Hiding the legacy headers.** Three tiers (public, legacy opt-in, internal); stages Phase 0 (symbol nodes), Phase 1 (files move, forwarding stubs), F (deprecated), F+1 (opt-in only), ≥ F+2 (not installed, libmtl soname bump); `libmtl_unified.so.1` stays a separate library at the freeze; the pipeline headers' removal date is set at the Phase 2 go/no-go; plugin ABI v2 and `mtl_convert.h` before F.

- **Options:** accept; or keep the legacy headers public and frozen (revision 3's Q-ABI-4).
- **Recommendation:** accept.
- **Research:** [S9](simplification/S9-hiding-session-headers.md).
- **Your answer:**

### Q-R4-6

> **Maintainer decision M16.** Designer recommendation: accept as one package.

**Kubernetes pods and crash safety** ([16](16-kubernetes-and-crash-safety.md), D-89…D-92).

- `mtl_instance_close(mt, timeout)` shuts the instance down on its last reference, network
  first: TX finishes the unit on the wire, RX leaves its groups, pending results are
  delivered and the threads that call application code are joined, the devices stop, the
  MtlManager grants are returned last, memory is freed. It returns 0 retired, 1 quiesced
  (memory or a thread still held), or `-MTL_EIO` when a port could not be stopped (exit
  the process). `mtl_instance_shutdown` adds flags and a report whose summary fits a
  termination message. Handles closed by the instance stay safe to pass (R4).
- R8: no signal handlers and no `atexit` in the library, close-on-exec, a fork rule, and
  nothing found by PID, so a SIGKILL leaves nothing that blocks a restart.
- `mtl_instance_get_health` for probes: liveness never depends on links, PTP lock or a
  surviving 2022-7 leg.
- Open fails fast with one named reason, and does not wait for links or time lock.
- CPUs come from the affinity mask; the SysV lcore table and `MANAGER_OPTIONAL` go.
  No-IOMMU is refused unless opted in. Time is read-only in pods, and MTL writes no file
  unless given a runtime directory. The pod recipe runs as non-root on a prepared node.
- Sub-questions Q-K8S-1…11 (16 §13), each with a recommendation.

- **Options:** accept; or accept the shutdown and R8 only, and keep today's lcore table,
  no-IOMMU behaviour and blocking open.
- **Recommendation:** accept. Q-K8S-6 changes behaviour for existing no-IOMMU users, who
  must then set `instance.allow_noiommu`.
- **Research:** [K1](kubernetes/K1-kubernetes-runtime.md), [K2](kubernetes/K2-mtl-code-audit.md), [K3](kubernetes/K3-shutdown-prior-art.md); reviews [RK](reviews/RK-kubernetes-review.md), [RA](reviews/RA-api-coherence-review.md) and the [response](reviews/RKNA-response.md).
- **Your answer:**

### Q-R4-7

> **Maintainer decision M17.** Designer recommendation: accept as the design; implement in
> Phase 7, after today's functionality is ported (D-98), and `mtl_crypto.h` after its cost spike.

**NMOS and IPMX** ([17](17-nmos-and-ipmx.md), D-93…D-96).

- IS-05: an update switches at the slot boundary by the clock, whether media flows or not;
  `mtl_session_update` returns the planned instant, and the status says when it applied.
  Re-apply and dry-run modifiers, and cancel. Every leg may be disabled (mute), legs can be
  reserved without an address, a port can change while running, and per-activation
  options apply at the same boundary.
- `mtl_sdp.h` renders and parses SDP in v1, replacing Q-MODE-6's "values only".
- IPMX: `session.profile`, sender reports and the Info Block (`mtl_rtcp.h`), with the NACK
  options renamed `rtx.*`; the async media mode `SENDER`, the one exception to D-09; a
  free-running time source, with AUTO re-evaluated while running; the IPMX schedule from
  the profile; IGMPv2; RX header extensions.
- PEP encryption through `mtl_crypto.h`, its parameters as options; keys never options;
  HDCP keys never in MTL.
- Sub-questions Q-NI-1…10 (17 §7), each with a recommendation.

- **Options:** accept; or NMOS only (IS-05 contract and SDP) and IPMX later.
- **Recommendation:** accept. IPMX cannot be met without sender reports and timing without
  PTP.
- **Research:** [N1](interop/N1-nmos-requirements.md), [I1](interop/I1-ipmx-requirements.md); reviews [RN](reviews/RN-nmos-ipmx-review.md), [RA](reviews/RA-api-coherence-review.md) and the [response](reviews/RKNA-response.md).
- **Your answer:**

## External

### Q-EXT-1

> **Proposed default (r3):** An action, not a decision: still wanted before Rivermax-like names
> are frozen (Phase 0 exit). Stands unless you object.

**Rivermax licensed documentation.** Several Rivermax semantics are **[unknown]** from
public sources (full `rmx_status` enum, the commit-time failure code, completion
moderation where DOCA and the Dev Kit disagree, thread-safety, which SDP attributes drive
pacing). Can someone with a Rivermax developer account confirm them before we adopt
Rivermax-like naming?

- **Research:** R10 Q14.
- **Your answer:**
