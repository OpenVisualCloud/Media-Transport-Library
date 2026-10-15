# Guarantees

| | |
|---|---|
| Status | Reference for the unified API. Nothing is implemented; the headers in [sketch/](sketch/) are normative, and where this page and a header disagree, the header wins |

This file is for implementers and testers. It holds every guarantee: the testable promise, its
tier, P or BE, the milestone whose exit needs it, and how it is tested. The rules in
[contract.md](contract.md) and [timing.md](timing.md) carry the G-ID; the requirement each
guarantee closes is in [requirements.md §3.1](requirements.md#31-the-catalogue).

**Start here.**

| Question | Where |
|---|---|
| What does G-n promise, and how is it tested? | its entry in §2, or §3 for G-PKT-n |
| What do the tiers, P, BE and M mean? | §1 |
| Which guarantees close a requirement, and when? | [requirements.md §3.1](requirements.md#31-the-catalogue), columns "Closed by" and "When" |
| Which guarantees does a milestone's exit need? | [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-of-ms1ms3), the index by milestone |
| Which numbers are retired? | §4 |
| The timing oracle | [timing.md §16.2](timing.md#162-oracle-and-contract) |

## 1. How to read this file

A guarantee is a testable promise; IDs are stable and a retired one keeps its number (§4). A
behaviour without a test is not promised. Each entry gives, in this order:

| Field | Meaning |
|---|---|
| ID | G-01…G-142 (§2), G-PKT-1…7 (§3); an entry with two IDs states both with one text |
| Tier | B build, U unit (the core over the null backend and the test clock), UB unit at the engine boundary (the real engine and its packets, through the bindings, without a NIC), I integration on VFs, A acceptance, M measured with a capture (HW timestamps, EBU LIST); where each runs: [implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence) |
| P or BE | **P**: promised (pass/fail); evidence exists or is funded in the named milestone, and a red P test blocks its milestone. **BE**: best effort until the named evidence exists; it never blocks an exit, and its entry says what evidence would make it P ([implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence)). An entry that names neither states none in its sources |
| M | the milestone whose exit needs it |
| Guarantee | the rule, after the ID, the tier, P or BE, and M |
| Test | how it is tested ("Test:"), with the notes behind the method |

The timing guarantees (G-17…G-28, G-53, G-59, G-61…G-63, G-66…G-68, G-75, G-76, G-82, G-85,
G-86, G-94, G-104…G-106) are P unless marked; when an entry names no milestone for one, the
ST 2110-20 subset is MS3 and the rest MS6.

The fault matrix is [implementation-plan.md §8.3](implementation-plan.md#83-fault-injection), the
budgets [§8.4](implementation-plan.md#84-performance-budgets), test isolation
[§8.1](implementation-plan.md#81-tiers-and-evidence), the oracle
[timing.md §16.2](timing.md#162-oracle-and-contract).

## 2. Guarantees G-01…G-142

- **G-01** U, UB, I; MS1. Every accepted TX submission has exactly one terminal outcome: a result
  when results are on, else a counter. Test: count outcomes against accepted submits over
  randomised submit, release, stop and close sequences (discard from MS2a) on `null:1`, also under
  the faults of [implementation-plan.md §8.3](implementation-plan.md#83-fault-injection).
- **G-02** U; MS1. A failed first submit gives no result and returns the slot to the pool, except
  on `-MTL_EBADF` and `-MTL_ESTALE`, which change no state (D-88). Test: submit with each
  invalid-argument class: no result, and the slot is free again.
- **G-03** I; MS1. An accepted unit progresses without another call, the last one before idle
  included. Test: submit one unit, call nothing else, observe its result (I1's
  `tx_idle_last_result` on a VF). It pins a defect of today, chain-mode results that never arrive
  while the application is idle; the core needs the idle descriptor cleanup of MS1 task E1b.
- **G-04** U; MS1. Results are never lost: unread results make acquire return `-MTL_EAGAIN` with
  `blocked_on = MTL_BLOCKED_RESULTS`. Test: `MTL_SESSION_RESULTS` and no reap: acquire ends with
  `-MTL_EAGAIN` and `MTL_BLOCKED_RESULTS`; then every result is readable.
- **G-05** U, UB; MS1. A slot is not re-acquirable before its outcome is recorded. Test: race
  acquire against completion in a stress loop, through the video binding over the real engine.
  Today st20p stores FREE before it calls back, so the race is real.
- **G-06** UB, I; MS2b. "Reusable" means no reader or writer remains (poisoned after the result,
  never on the wire). Test: poison the slot right after its result; a capture shows no poisoned
  packet.
- **G-07** U; MS1. Foreign or stale handles fail with `-MTL_EBADF`, returned leases with
  `-MTL_ESTALE`, without changing state. A lease of another session fails deterministically, on the
  session index in the lease. Test: fuzz handle values; double submit and release; release a lease
  on another session.
- **G-08** UB; MS1. Transmit order is submit order. Test: acquire A and B, submit B then A before
  pick-up: B is sent first. Today st20p numbers frames at `get_frame`
  (`st20_pipeline_tx.c:806-808`) and the builder picks the lowest number
  (`tx_st20p_newest_available`, `:62-77`, SF-44), so it would send A first; the core assigns `seq`
  at submit.
- **G-09** UB; MS1. Results are in submission order, each with its `seq`. A unit that completes
  before an older in-flight unit frees its slot at completion; only its result waits for its
  predecessors. Test: completions forced from another thread and out of order: results still in
  `seq` order.
- **G-10** UB, I; P; MS2b for video, the other essences MS4. A region outlives every slot,
  conversion, packet and DMA reference. Test: `mtl_mem_close` at every stage of a unit's life.
- **G-11** U; P; MS2b for video, the other essences MS4. `mtl_mem_close` on a referenced region
  consumes the handle and returns 1; `MTL_EVENT_REGION_RELEASED` follows the last reference. Test:
  close with slots attached and units in flight.
- **G-12** UB; P; MS2b for video, the other essences MS4. No access outside the declared plane
  spans or the published rows. Test: guard pages (`mprotect`) around the spans; rows units publish
  progressively with the unpublished rows poisoned.
- **G-13** UB, I; P; MS2b for video, the other essences MS4. `MTL_SESSION_REQUIRE_DIRECT` never
  silently copies or converts. Test: force every downgrade condition (no multi-segment TX, pool
  below `min_count_direct`, ST 2110-22, PA mode, conversion): query, create or start fails with a
  reason; conversion is `-MTL_ENOTSUP`, `DIRECT_IMPOSSIBLE`, at query and create alike; a small pool
  `POOL_TOO_SMALL`.
- **G-14** UB; P; granted values MS1; MS2b for video, the other essences MS4. The selected data path
  is queryable and matches what happens. Test: compare `MTL_INFO_DIRECT` and the `path` of
  `mtl_tx_result_full` and `mtl_rx_detail` with the per-unit `pkts_dma` and `pkts_copied_partial`.
  GPU ingest stand-in: import `mmap(MAP_ANONYMOUS | MAP_LOCKED)` memory, run RX direct into it and
  TX direct from it.
- **G-15** UB; P; MS2b for video, the other essences MS4. Library-pool and attached slots obey the
  same timing and outcome accounting. Test: identical submissions through both; diff the results.
- **G-16** I; BE; MS2b for video, the other essences MS4. An import maps into every device on the
  session's path (both 2022-7 ports, the DMA engine), or fails. Test: a 2022-7 session on two ports
  plus a DMA engine under an IOMMU fault monitor (DMAR or AMD-Vi lines in `dmesg`); P once the
  I-tier job scans for IOMMU faults.
- **G-17, G-18** U; P; the ST20 fields MS1–MS3, the rest MS6. Every time field names its clock,
  unit and validity (zero is valid when flagged); requested, resolved, scheduled, enqueued and
  observed times are separate fields. Test: a schema test over every output struct: each time field
  has a validity flag and a clock.
- **G-19** U, UB; P; MS3. RTP = `floor(M × R) mod 2^32`, exact, for every essence (second fields
  by [timing.md §4.2](timing.md#42-derived-rtp-exact-and-its-exceptions)), with no cumulative drift
  and across the 2^32 wrap. Test: an exact 128-bit rational oracle
  ([timing.md §16.2](timing.md#162-oracle-and-contract)) at every rate including 1001 families;
  10^7 units at today's TAI, across the 2^32 wrap (13.26 h at 90 kHz, 27.05 h at 44.1 kHz),
  44.1 kHz with 1001 rates, ST41 at 90 kHz with rational unit rates, TAI past 2^61 ns. Negative
  indices (audio priming under `MTL_WHEN_ORIGIN`) are checked in the oracle at every rate, and in
  the engine as `FLUSHED/BEFORE_START`. The six split formulas of
  [timing.md §3.5](timing.md#35-exact-arithmetic) at the domain edges (index rate 2^29/s,
  num × den = 2^33, clock 2^29 Hz, t = 2^63 − 1 ns) with every product checked for overflow.
- **G-20** UB; P; MS1; the ST40 half MS4a. Default video media time is the frame epoch; ST20 and
  ST40 of one frame carry equal RTP. Test: the RTP of frame N equals the epoch RTP of index N with
  today's engine rounding (`tai_from_frame_count`, then `st10_tai_to_media_clk`, round to nearest;
  [timing.md §6.8](timing.md#68-the-launch-decision)), with no TR offset term;
  `floor(N × TFRAME × 90 kHz)` from E2 (MS3, or MS2a with the stretch task X); the equal-RTP check
  comes with ANC on the core (MS4a). Today ST20 and ST40 of one frame differ by +54.4…+55.7 ticks
  at 1080p59.94, depending on the granted VRX0.
- **G-21** UB; P; MS6. Audio RTP identifies each packet's first sample; submission boundaries change
  no packet. Test: random submission sizes (1024, 800/801 and 1601/1602 samples among them) against
  one large submission: identical packets.
- **G-22** UB; P; MS6 (with TLINE/2, E5b). Interlaced: each field its own M,
  `M(second) = M(first) + TFIELD`, `RTP(second) = RTP(first) + floor(TFRAME × 90000 / 2)`
  ([timing.md §4.2](timing.md#42-derived-rtp-exact-and-its-exceptions)); launch includes TLINE/2.
  Test, for G-22 and G-68: the field increments of timing.md §4.2; starts at k = 0…100 and 20 random
  `now`/lead/preroll per configuration (59.94p + 48 kHz + ANC; 50p + 48 kHz + ANC;
  29.97p + 48 + 96 kHz; 1080i59.94 + 48 kHz + ANC; 1080i50 + 48 kHz + ANC; 59.94p + 29.97p + 48 kHz;
  audio only; 59.94p + 48 + 44.1 kHz; 1199/20 + 48 kHz: 90 kHz and 48 kHz excluded, T0 on the frame
  grid): T0 on the grid, `S ≥ A`, and no field-parity flip.
- **G-23** U, I; P; MS6. A start array starts all or none. Test: fail one member's validation:
  nothing starts.
- **G-24** UB; P; MS6. Under DROP, one unit's lateness shifts no later unit of any session. Test:
  lateness injected into video only leaves audio RTP and launch unchanged; the bounded
  `MTL_LATE_SEND_LATE` part (it never overlaps the next unit) comes with Phase 7.
- **G-25, G-26** UB; P; G-26: with EXACT (B3, else MS2a), the checks beyond the binding's MS3; G-25
  MS6. Arrival time never replaces media time; an invalid exact launch fails explicitly and never
  falls back to another pacing mode. Test: G-25: RX units with shifted arrival keep
  `media_tai_ns`. G-26: exact launches in the past, closer than the lead and beyond the horizon:
  `-MTL_ERANGE` with the reason, never another pacing mode; it pins today's silent fallback of
  invalid exact requests (`st_tx_video_session.c:1796-1805`).
- **G-27** M; BE; an exit criterion of MS4 (MS4a, MS4b). The launch schedule matches ST 2110-21 for
  the granted sender type within the accuracy profile. Test: EBU LIST or the timing parser with NIC
  timestamps, per NIC × pacing class; P per class once measured.
- **G-28** UB; P; MS3. A late unit gets its policy's outcome and a result with margins. Test: delay
  a submission across its deadline for each late policy and media mode.
- **G-29** U, UB; MS1 (ASan); TSan from MS2a. Close cannot race an active call or a completion into
  freed memory. Test: N threads in DP and WT calls while another thread closes.
- **G-30** U; MS1. Stop and close wake blocked calls with `-MTL_ESHUTDOWN`, interrupt with
  `-MTL_ECANCELED`, ERROR with `-MTL_EIO`. Test: blocked acquire, dequeue and reap (event reads from
  MS3) return within a bound with the documented code on stop, interrupt, close and ERROR; WH11.
  Today `wake_block` does not return early (SF-16; fixed in open PR #1770).
- **G-31** U, UB; MS1 (discard MS2a). Discard, DRAIN and FLUSH leave every accepted unit with a
  terminal outcome. Test: discard, DRAIN and FLUSH with units at every stage; stop racing submit.
- **G-32** U, I; MS1. A failed create leaves no session, flow, quota or handle. Test: inject a
  failure at each create step: `capacity.*` and `port.free_*` return to baseline.
- **G-33** U; MS1. The library reads `min(struct_size, known)` and rejects unknown non-zero bytes
  (`NONZERO_TAIL`). `struct_size` is input only and never rewritten; 0 or a too-small size is
  `NONZERO_TAIL` too; a non-zero `reserved` field of an input struct is `-MTL_EINVAL`. Test: compile
  against an older and a newer header copy; a non-zero tail or reserved field is `NONZERO_TAIL`.
- **G-34** I; BE until the per-NIC capability job runs; MS4b. Every advertised capability
  (`caps.*`) passes its conformance test.
- **G-35** U; P; MS2b for video, the other essences MS4. A session cannot start until its pool is
  complete and validated. Test: start with fewer attached slots than `pool_count`: `-MTL_EINVAL`,
  `POOL_TOO_SMALL`.
- **G-36** UB; P; MS2b for video, the other essences MS4. Attach, detach and mapping never happen in
  the packet path; an attach to a device the region is not yet mapped to maps it before the attach
  returns. Test: no allocation and no map or unmap call while RUNNING.
- **G-37** UB; P; MS2b for video, the other essences MS4. Once a close retires, no application code
  is called, no imported memory is touched, the handle reads `MTL_STATE_RETIRED`. Test: unmap the
  region right after `mtl_session_close` returns 0; no fault.
- **G-38** UB; P; measured in MS1, P at MS4b. No public function executes on a library tasklet or
  library loop; from a library thread that close would join (codec, or the log thread while it runs
  a sink), instance close and shutdown are `-MTL_EDEADLK` (`LIBRARY_THREAD`). Test: a debug-build
  assert on the tasklet threads catches any public entry; close and shutdown from each library
  thread.
- **G-39** UB; P; measured in MS1, P at MS4b. On the DPDK PMD backend the tasklet side of every
  hand-off makes no syscall, takes no lock an application thread can hold and never allocates (no
  `malloc`, `rte_malloc*`, pool or ring create); a completion for an armed waiter marks the object,
  and the library loop, after its handler loop and outside every tasklet, wakes a bounded number of
  marked objects and queues per iteration, one futex wake per object and one `write()` per queue
  (G-142). A data call makes only the syscalls of R6, on its own object. Test: syscall counting and
  a `malloc`/`rte_malloc` hook on the tasklets in a stress run, per `MTL_INSTANCE_TASKLET_THREAD`
  mode; the idle sleep and the kernel-socket and AF_XDP backends' own syscalls are reported, not
  forbidden.
- **G-40** UB; MS3. Reading stats never blocks or delays a tasklet. Test: lock instrumentation: no
  stats read takes a lock a tasklet takes.
- **G-41** U; MS3. Every state event has a getter that is right after an event overflow. State
  events only; notices (`MTL_EVENT_EPOCH_TICK`, `MTL_EVENT_OVERFLOW`) need no getter. The getter of
  `MTL_EVENT_RX_TIMEBASE_SUSPECT` is `MTL_STATUS_TIMEBASE_SUSPECT`. Test: overflow a session's
  events, then compare every getter with the true state.
- **G-42** U; MS2a (A2b). Every counter is monotonic for the life of the session; no call resets
  it. Test: two readers with different periods over a randomised session life.
- **G-43** U, UB; MS3. Gauges satisfy entries − exits = gauge at quiescent points. Test: exact at
  ≥ 10^4 quiescent points (test clock paused, application threads parked); between them
  `abs(entries − exits − gauge)` ≤ the number of completing contexts.
- **G-44** UB; P; MS6, at I. In release builds no tasklet or library loop prints a log line or calls
  the sink at any level: tasklets write formatted, length-capped lines into the per-scheduler log
  ring, which drops when full and counts the drops, and a library thread hands them to the sink.
  Test: a sink hook asserts it never runs on a tasklet thread; a flood fills the ring and the drops
  are counted.
- **G-45** U, UB; P; video MS1; every essence MS4b (video only until then). The same verb
  sequence passes G-01…G-09 for every essence × direction. Test: the suites parameterised over
  `MTL_VIDEO`, `MTL_CVIDEO`, `MTL_AUDIO`, `MTL_ANC`, `MTL_FASTMETA` (and `MTL_RTP` packet units at
  MS5).
- **G-46** U; MS1. A TX verb on an RX session (and the reverse) is `-MTL_EINVAL` and changes
  nothing. Test: every TX verb on an RX session and the reverse.
- **G-47** U; MS1. The cookie comes back verbatim; a cookie without results is
  `COOKIE_WITHOUT_RESULTS`; RX units of a provide session carry the destination's cookie (G-137).
  Test: random cookies, and a cookie on a session without results.
- **G-48** U; P; MS2b for video, the other essences MS4. A slot's planes never change between uses;
  per-use values live only in `struct mtl_unit` and the result. Test: hash `mtl_session_get_slot`
  output before and after 10^4 uses.
- **G-49** U; MS1. Every call returns the documented code in every state; a closing handle accepts
  only close (which polls), `mtl_session_get_status`, the release of a lease taken before the close
  and interrupt (a no-op), and a retired one reads `MTL_STATE_RETIRED`. Test: the call table of
  `st_core_states.def` on `null:1`, the closed-by-instance column included; ERROR by
  `MTL_FAULT_FORCE_ERROR`. The state × call table runs inside one long-lived instance, because the
  last `mtl_uninit` of today's default instance happens inside the library and a clean re-init per
  case is impossible (#1341).
- **G-50** B, U; MS3. Every function has one call class, enforced in debug builds. Test: header
  lint; the debug asserts under the U suite; a signal raised inside every CP call under `malloc` and
  `pthread_mutex_lock` interposers.
- **G-51** B; MS1, MS3, MS7. One node per milestone part, `MTL_UNIFIED_EXPERIMENTAL_<rev>_MSn` and
  `_MSn.k`, the bridge in `_BRIDGE`; a sealed node never changes; a node the library lacks fails at
  load, never at a call; from MS3 a soname, `MTL_LEGACY`, `local: *`. In full: milestones ≤
  `MTL_LEVEL` are exported in their nodes, none above `MTL_LEVEL` + 1, none unversioned; a sealed
  node equals its list, which never changes; every node of a release tag of the revision is sealed
  at HEAD with the tag's names, and a `.REL` tree has no open node. Test: `unified_exports`,
  `check_frozen_lists.sh`, `unified_seal_probe` and their mutations
  ([implementation-plan.md §5.2](implementation-plan.md#52-tasks) H1b). Each mutation fails: a name
  added to a tagged node, an edited list, an open node in a `.REL` tree, an MS2 function in an MS1
  part, the bridge in `_MS1`.
- **G-52** U; MS1 (calls with a timeout), MS2a (queues). No lost wake-up and no busy loop, with any
  number of threads on one object and on one queue (R2). In full: a thread asleep in a call with a
  timeout is woken once its target is ready, it is interrupted or its state ends it, whatever other
  threads do on the object. From MS2a a sleeper on a queue is woken once an armed object changes;
  sleep only after `-MTL_EAGAIN`; any thread may make one call. Test: the model job (`wait_model`)
  and the pause-hook tests WH2, WH3, WH10–WH12, WC1–WC3, WC5 (`WaitHook.*`, UnifiedUnitTest,
  TestBinding and NullBinding), from MS2a QW1–QW22 (`QueueHook.*`); no random test counts as
  evidence. The model job, the hook tests and what each pins: "The wait evidence" of
  [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-of-ms1ms3).
- **G-53** U, UB; P; MS3. Submitting before the cursor's deadline is ON_TIME at the cursor's index;
  submitting before `M − min_submit_lead_ns` is never `TOO_LATE`. G-53 holds for ANC as stated: the
  hint's deadline is [timing.md §9.2](timing.md#92-the-anc-transmit-window-and-live-anc)'s. Test:
  submit just before `submit_deadline_tai_ns` of `mtl_tx_get_next`: `ON_TIME` at that index; ANC
  with the deadline of timing.md §9.2 (`Anc.DeadlineCapacity`).
- **G-54** UB with `MTL_FAULT_TIME_LOST`; MS6, at I. The time state leaves LOCKED on sync loss and
  `MTL_EVENT_TIME_STATE` is posted (pins SF-20).
- **G-55** U; P; MS6, at I. `queue.queued_media_ns` equals the sum of the queued units' durations; each
  histogram's `count` equals the number of published results. Test: null-backend runs on the test
  clock, compared at quiescent points.
- **G-56** U; MS1. RX leases released out of order or from other threads return to the pool. Test:
  release RX leases out of order and from other threads.
- **G-57** U, UB; MS3. Each status, reason and code is produced by its documented trigger. Test: the
  trigger list built from `reasons.def` (every row with `ms` ≤ `MTL_LEVEL`) on `null:1`, through
  `mtl_debug_inject` where no natural trigger exists.
- **G-58** U, I; P; MS3, I MS6. Event coalescing never loses the first or last state of a transition
  and never blocks a producer; every reader of a session's or the instance's events receives each
  state event, independently of other readers. Test: a tasklet producer and a reader under stress.
- **G-59** UB, M; P at UB, BE at M; MS1, the M tier MS6. No packet before its media time;
  `NOT_BEFORE t` never before t. Test: a wide sender with maximum pre-fill; UB checks the scheduled
  times, M the wire.
- **G-60** U; P; MS2b for video, the other essences MS4. `mtl_tx_acquire_slot` leases exactly the
  named slot; `MTL_SESSION_RX_BY_INDEX` places unit k in slot k mod `pool_count`. Test: named
  acquires over every slot; an RX index sweep.
- **G-61** UB; P; MS4a (ST40 MS4a2, ST41 MS4a1). ST40 keep-alive per
  frame or field without ANC; ST41 at least every 500 ms. Test: no ANC submitted for 10 frames gives
  one empty packet per frame, field or PsF segment, each inside its window (a PsF unit with ANC only
  in one segment also gets an empty packet of its RTP in the other); ST41 at least every 500 ms.
- **G-62** U, UB; P; MS6. A step keeps every media time and posts `TIME_STEP`; no session drops
  forever after a forward step. Test: `MTL_FAULT_TIME_STEP` with `step_ns` = +37 s (today's UTC →
  PHC switch), or `MTL_FAULT_CLOCK_ADVANCE`; the published time base never steps except at a
  declared `MTL_EVENT_TIME_STEP`.
- **G-63** P; created timelines (`MTL_LATER`), not in v1. Test: sessions created slowly (test clock
  advanced between creates), then started: index 0 of a lazily anchored timeline is never late
  because of creation time.
- **G-64** U; MS1. Interrupt is sticky until cleared (`mtl_interrupt(o, MTL_INTR_OFF, 0)`), never
  wakes another session, and with a target mask wakes only those targets.
  `mtl_instance_interrupt(mt, 0)` undoes the instance interrupt but leaves a session interrupted on
  its own still interrupted; an interrupt limited to some wait targets (the `targets` argument,
  `MTL_WAIT_*`) leaves the other targets' waits alone; interrupting a CLOSING session is a no-op.
  Test: the GStreamer `unlock`/`unlock_stop` sequence, the reaper not spinning; session and instance
  interrupts combined; the `EINVAL` and `ENOTSUP` cases of contract.md §7.3 without
  `mtl_last_error()`; WH10 and WC3.
- **G-65** U; MS1. Close with leases out returns 1; releases still work; a repeated close polls (0
  or 1) and, with a timeout, waits for retirement. Test: close with leases out, close again with
  timeout 0 (1), release them later, close again (0).
- **G-66** UB; P; MS4b. Cvideo follows
  `rate_mode`; oversize is `-MTL_ENOSPC` at submit. Test: variable codestream sizes in both rate
  modes; an oversize codestream is rejected at submit with `-MTL_ENOSPC`, `CODESTREAM_OVERSIZE`, and
  the slot returns to the pool without a result (D-88); `MTL_CVIDEO_VBR_MAX` sets
  `MTL_INFO_NON_COMPLIANT`.
- **G-67** UB; P; MS6. RX honours `rx.rtp_offset`; large offsets post `RX_TIMEBASE_SUSPECT`. Test:
  an AES67 sender with a non-zero `rx.rtp_offset`; a direct stream with a large offset posts
  `MTL_EVENT_RX_TIMEBASE_SUSPECT`.
- **G-68** U; P; MS6. The T0 formula: `T0·R` integer for every rate kept in G, whole frames for
  video/ANC/grid fastmeta, unit k at ≥ `now + lead + preroll`. Test: the method of G-22, which
  covers both. Interlaced members contribute TFRAME, not TFIELD, to the grid, so T0 is a first-field
  instant (at 59.94 with k = 3, 7 and 13, `T0·90000` is an integer); `MTL_AT_INDEX` on one session
  gets the same checks.
- **G-70** U; MS1. Each `MTL_E*` code has one meaning; RX dequeue with nothing ready returns
  `-MTL_EAGAIN` in CREATED, ARMED, RUNNING and STOPPED, never `-MTL_ESHUTDOWN`. Test: each code from
  its triggering call and state, including dequeue on a session never started; the P1
  forgotten-start case. `-MTL_EAGAIN` sets only code and reason in `mtl_last_error`; `detail` stays
  empty for DP failures.
- **G-71** U; MS1. The null handle fails with `-MTL_EBADF`, except every close, which returns 0.
  Test: the null handle to every function, table-driven over the headers.
- **G-72** U; MS1. `mtl_reap` writes `min(rec_size, native)` per record at pitch `rec_size`; the
  typed wrappers pass `sizeof(*r)`. Nothing is written beyond `max × rec_size`; the same for event
  reads with `ev_size`. Test: record sizes larger and smaller than the library's, with `MTL_INIT`
  hoisted out of the loop.
- **G-73** B, U; MS1. An input struct from `MTL_INIT` (zero in every field but `struct_size`) is the
  default configuration. Test: a zero-filled struct opens with the documented defaults; a lint lists
  every field whose zero means something else. The lint covers the fields whose zero is replaced by
  another value and the zero-valued enumerators that name a mode; each `MTL_INIT` output equals a
  zeroed struct except `struct_size`.
- **G-74** B; MS1. Headers and examples compile (`check.sh`). Every hole is a named `reserved`
  field; every symbol named in any document is declared; `check.sh` moves with the headers (MS1 task
  H1b). Test: `check.sh` in CI, with `-pedantic` and clang, plus a compile-only Windows job; the
  examples to object code at their `Needs:` level; the availability probes (OI-64); candidate flags
  `-Wconversion -Wsign-conversion -Wcast-qual` (`-Wcast-qual` catches `mtl_pkt_tx_table()`
  returning a writable table).
- **G-75** U, I; P; MS5. Flow updates at a media index or instant switch on every leg at that unit.
  Test: both legs move to a new multicast pair at one TAI instant; a capture of both legs shows no
  unit mixing old and new destinations. TX `MTL_AT_INDEX k`: unit k is the first on the new flows on
  every leg; RX `MTL_AT_TAI t`: the new rule takes units with media time ≥ t, the old rule goes
  after. An unresolved neighbour does not fail the update (the leg waits in
  `MTL_FLOW_WAITING_NEIGHBOUR`); a capacity shortage is `-MTL_ENOSPC` and changes nothing; a port
  change is `-MTL_EBUSY`, `PORT_CHANGE_NEEDS_STOP`, until make-before-break (Phase 7).
- **G-76** U, UB; P; video MS3; every rate MS6. RX reports `media_index` as the exact inverse of the
  TX rule on the epoch timeline; the RX members of a start array arm together. Test: a sender on the
  grid at every video rate (1001 families, fields) across the 2^32 wrap, and a sender with a phase
  below one period, which maps to the slot it falls in; checked against the oracle. Also: every
  member of an RX start array started at t delivers its first unit with media ≥ t; start at
  `MTL_AT_TAI t` with packets flowing before t, stop and start without an IGMP leave, the
  latest-only recipe (`pool_count` 2, `MTL_SESSION_RX_LATEST`), video, audio and ANC aligned with
  `mtl_rx_align`. At MS6 the inverse is checked at every rate (1001 families, fields, floor-aligned
  44.1 kHz).
- **G-77** U; MS1 (exclusive open); MS2a (SHARED). Closing a shared instance's reference that is not
  the last only drops it (returns 0); the last one shuts down; a CLOSING session stays readable
  through `mtl_session_get_status`. It also covers the framework teardown order (release the
  instance before the last lease returns); close never fails because objects are still live;
  process exit without a close is supported. Test: MS1: a second open without SHARED is `-MTL_EEXIST` (OI-49);
  close polls with a session in CLOSING. MS2a: two SHARED opens, closed in both orders; the joins
  `Instance.shared_subset_join` (`null:1,null:2`, then a SHARED open of `null:2` joins, and
  `mtl_port_find` gives index 1) and `Instance.shared_missing_port_detail`.
- **G-78** U; P; MS2b for video, the other essences MS4. Export pool: a framework pool with one
  wrapper per slot, over `mtl_tx_acquire_slot`, submit and `mtl_tx_release`, recycles every wrapper
  exactly once, with neither `MTL_SESSION_RESULTS` nor `MTL_SESSION_MT_SUBMIT` (migration.md §12.4).
  An unsubmitted buffer's release releases its lease (`mtl_tx_release` on a submitted one is
  `-MTL_ESTALE` and changes nothing); a slot is leased again only after its wrapper came back and
  the slot has left; `set_active(FALSE)` is `mtl_session_stop(MTL_STOP_FLUSH)`. G-78 also serves
  R-CMP-1 and R-MEM-8 for framework pools. Test: the GstBufferPool state machine replayed on
  `null:1`, including a buffer kept as the last sample.
- **G-80** U, UB; MS2a, with command acks. Every command is acked; a missing ack puts the session in
  ERROR with `CMD_TIMEOUT`. Immediate commands (stop, discard, detach, interrupt) are acked within
  one tasklet iteration even with no unit and no packet; the CP applies a command itself for a
  detached session; the ack timeout is fixed (D-48); with `MTL_INSTANCE_TASKLET_SLEEP` one iteration
  plus the wake-up. Test: a TX unit waiting 1 s for its launch: stop is immediate and the unit
  `FLUSHED`; RX with no packets; with `MTL_INSTANCE_TASKLET_SLEEP`; a stalled scheduler.
- **G-81** UB (P), I (BE); MS3. Close on a stalled queue completes (reset or quarantine). After the
  bounded idle-cleanup wait a worker resets the queue (dedicated) or quarantines it (shared); mbuf
  free callbacks then run on that worker; the session retires (close returns 0) only after; a failed
  reset escalates to port-reset handling; then no descriptor references imported memory and
  `mtl_mem_close` returns 0. Test: `MTL_FAULT_TX_QUEUE_HANG`, then close: after retirement no
  descriptor references the session's memory. The I part is P once a hang can be forced on a VF.
- **G-82** UB; P; MS2a. An RX unit past its due time is force-completed within one scheduler
  iteration; with no packets the due time is still checked within 1 ms. The due time is the
  first-packet arrival on the earliest leg + the unit period + `rx.flush_offset_ns`, capped at
  `presentation_tai_ns` with a link offset; the unit is delivered or discarded per `rx.incomplete`;
  `mtl_session_stop(MTL_STOP_DRAIN)` on RX delivers the partial unit. Test: feed half a frame, then
  stop sending.
- **G-83** U, UB; P; MS2b for video, the other essences MS4. An RX lease held by N TX units
  (`unit.hold`) returns to free exactly once, when its hold count reaches 0 **and** the application
  released it, in any order; the N TX units may be in flight together; MTL never writes a slot that
  is held or being read; a TX unit whose planes lie outside the held RX slot is `-MTL_EINVAL`,
  `LAYOUT_MISMATCH`. Test: one RX unit to four TX sessions attached over the same pool
  (`mtl_session_get_pool_region`); release the RX lease, complete the TX units in random order: the
  slot is never reused early and never leaks.
- **G-84** UB; P; MS2b for video, the other essences MS4. A layout with stride ≥ row bytes (a
  sub-rectangle, an interleaved field) over a direct-capable region is direct for ST 2110-20 TX and
  RX: the stride reaches the engine as its line size and no whole-unit copy happens; only a packet
  that crosses row padding (non-GPM_SL) or a PA-mode page boundary is copied, counted in
  `pkts_copied_partial`; slots of one pool with different strides are `-MTL_EINVAL`,
  `STRIDE_MISMATCH`. Test: split-forward quarter frames and 2 × row-bytes field buffers with
  `MTL_SESSION_REQUIRE_DIRECT`: no copy where the engine needs none.
- **G-85** U, UB; P; MS3. TAI snapping NEAREST: collisions are `SNAP_COLLISION`, skips follow the
  underrun policy, never a permanent drop. Test: a 60.0 Hz source into a 59.94 session for a
  simulated 24 h on the test clock, and any source within ±1 %: the fraction of units sent is
  ≥ 1 − the rate error and every sent RTP is on the grid.
- **G-86** UB; P; MS6. Audio forward gaps keep the packet grid and are padded with silence
  (`samples_padded`), overlaps are dropped (`samples_dropped`); only DISCONTINUITY re-phases. Test:
  drop one video frame's audio (800.8 samples) repeatedly; every later packet's RTP stays on the
  original grid; packet p covers `[p·S, (p+1)·S)` with RTP `floor(T0·Fs) + p·S`; no submitted
  sample outside an overlap is dropped.
- **G-87** U; P; MS3. `mtl_session_update` with `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` in STOPPED
  keeps the handle, name, flows, SSRC and counters; library pools are re-created; a validation
  failure changes nothing: attached slots that no longer fit are `-MTL_EINVAL`, `LAYOUT_MISMATCH`.
  Test: 1080p → 720p → 1080p with counters counting on; each failure class.
- **G-88** U; MS1 (the listing: MS2a, A2b). Names are copied, unique per instance, and listed
  exactly once. The name is copied (64 B) and unique per instance: `-MTL_EEXIST`, `NAME_EXISTS`, on a
  clash with a live or CLOSING session; it appears in `mtl_session_info`, the stats, the log prefix
  and every event (`origin_name`). Test: create and close churn with a concurrent
  `mtl_instance_list_sessions`; free the config's name buffer right after create.
- **G-89** U, I; P; MS5. If `mtl_session_query` with `MTL_QUERY_CHECK_CAPACITY` succeeds and nothing
  else changes the host, the create succeeds; if it fails, the reason names the limiting resource
  (`CAPACITY_*`); `capacity.*` and `port.free_*` return to baseline after every close. Test: fill a
  scheduler to its quota and session limit on `null:1` (U); RL queues on a VF (I).
- **G-90** U, I; P; MS5. Create and start never wait for ARP or IGMP: an unresolved TX leg is
  `MTL_FLOW_WAITING_NEIGHBOUR` and its units are not sent on it (DROPPED, `WAITING_NEIGHBOUR`, when
  every leg is unresolved); resolution resumes sending without an API call; `MTL_EVENT_FLOW_STATE`
  and `mtl_leg_status.flow_state` report it per leg. Test: a destination that never answers ARP;
  create returns within its CP bound.
- **G-91** U, I; P; MS5. Resources are reserved for every configured leg regardless of link and a
  configured leg is never pruned; disabling or enabling a leg (`legs_disabled`, `MTL_UPDATE_LEGS`)
  takes effect at a unit boundary; a down leg does not starve the pool (units complete with that leg
  marked not sent, `leg_reason[]`) and resumes at the next unit boundary after its link returns;
  admin and oper state are in `mtl_session_status.leg[]` and `MTL_EVENT_LEG_STATE`. Test: `null:1`
  with `MTL_FAULT_LEG_DOWN` (U); `nicctl.sh vf_link … down|up` while RUNNING (I).
- **G-92** U; MS1. The null backend is deterministic with the test clock. Test: completions run
  synchronously inside `MTL_FAULT_CLOCK_ADVANCE` on the caller's thread; run the suites twice and
  diff the result records byte by byte; WH17 (the wake order under ADVANCE).
- **G-93** U, B; MS1. The debug API exists only with `-Denable_debug_api=true`; otherwise
  `-MTL_ENOTSUP`. Test: every fault reaches its outcome; in a release build `mtl_debug_inject`
  returns `-MTL_ENOTSUP` and `nm` shows no fault code.
- **G-94** U; P; MS3. Preroll extends the lead; the horizon counts from the start instant S; a start
  that would strand queued units beyond the horizon fails atomically (`-MTL_ERANGE`,
  `BEYOND_HORIZON`). Test: preroll a full pool, then start with a horizon one unit too short; queued
  units with M < S are FLUSHED with `BEFORE_START`.
- **G-95** U; MS1. Waiters are per target and never steal each other's wake-up: any number of threads
  may block on one session, on one target or on different ones, and a wake for one target never ends
  another target's wait. Test: WH3 (woken counts per lane), WC5; the model job ("The wait evidence"
  of [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-of-ms1ms3)).
- **G-96** U, I; P; MS2b for video, the other essences MS4. An import whose `va` or `length` is not
  page-aligned (hugepage-aligned for hugetlbfs) is `-MTL_EINVAL`, `UNALIGNED`, and maps nothing; an
  aligned import maps exactly `[va, va + length)`; one spanning VMAs with different backings is
  `MIXED_BACKING`. Test: every misalignment class (U); the device's IOVA table shows no page outside
  the range (I, [deployment.md §1.1](deployment.md#11-imported-memory-and-the-iommu)).
- **G-97** MS1, each later milestone. Every example compiles, and the ST20 ones run on `null:1`
  (ex01, ex02 and ex20 from MS1; ex03 and ex19 from MS2a; ex10–ex12 from MS2b; ex04–ex08, ex16 and
  ex21 from MS3).
- **G-98** U, I; P; MS5. MtlManager loss never kills or stalls the process: no SIGPIPE (manager sends
  use `MSG_NOSIGNAL`); running sessions continue (G-110); a create that needs the manager is
  `-MTL_EBUSY`, `MANAGER_LOST`; after reconnection the instance re-registers and re-announces its
  CPUs. Test: `MTL_FAULT_MANAGER_LOST`; at I `gtest.sh` kills MtlManager, runs creates, restarts it.
- **G-99** UB, I; every engine change. With legacy opt-in flags off, legacy wire output is identical
  to the baseline; on, it matches the oracle. Test: per engine change, a pcap diff against the
  baseline inside the legacy gate.
- **G-100** U, UB; P; MS2b for video, the other essences MS4. MTL never writes a TX slot: after
  `mtl_tx_submit` its bytes are unchanged and it stays mapped until acquired again. Test: hash the
  slot before submit and after the result on the direct, copy and convert paths.
- **G-101** U; P; MS2b for video, the other essences MS4. Attach is `-MTL_EINVAL`,
  `ACCESS_MISMATCH`, when an RX session gets a region without write access or a TX session one
  without read access (`MTL_MEM_WRITE`, `MTL_MEM_READ`). Test: every direction × access combination.
- **G-102** UB; P; MS2b for video, the other essences MS4. An import beyond the region budget
  (`caps.max_regions`) is `-MTL_ENOSPC`, `REGION_BUDGET`, never an opaque DPDK error;
  `instance.regions_used` and `instance.regions_free` are exact. Test: import until the budget is
  exhausted, close one region, import again.
- **G-103** U; MS1. `pool_count` above `max_count` fails at query and create with `-MTL_ERANGE`,
  `POOL_COUNT_MAX`. Test: `max_count` and `max_count + 1`, at query and at create.
- **G-104** UB; P; MS6. With video launch delay L_v, ANC k has RTP_v(k) and lies in the window of
  frame `E⁻¹(M(k)) + L_v`, keep-alive included. Test: a start array of capture producers whose
  `min_tx_delay_ns` gives the video launch delay L_v = 1 and 2; with video units dropped the empty
  ANC keep-alive still lies in the window.
- **G-105** U, UB; P; MS6. Two processes on the epoch with a grid-aligned `t_start` emit the RTP of
  one process. Test: both set-ups (two processes on the epoch with `mtl_epoch_index_at`, one process
  with a start array) on the null backend; packet headers diffed.
- **G-106** UB; P; MS3. A legacy session with E2's opt-in flag puts `floor(M × R)` of the
  timestamps it is given on the wire, equal to the oracle; without the flag its wire is unchanged
  (G-99).
- **G-107** U, UB, I; MS1, MS3. `mtl_instance_close(mt, timeout_ns)` returns within `timeout_ns` and
  only 0, `MTL_RETIRING` or `-MTL_EIO` (`QUEUE_QUARANTINED`); `mtl_session_close` 0 or
  `MTL_RETIRING`; only a DRAIN stop past its deadline returns `-MTL_ETIMEDOUT`. Test: close with
  RUNNING sessions and leases out at timeouts 0, 10 ms, 1 s (`MTL_FAULT_TX_QUEUE_HANG` from MS3);
  elapsed time, return set.
- **G-108** U, I; MS3. Network first: TX stops at a unit boundary and RX leaves its groups before any
  port stops; `mtl_shutdown_report.groups_left` counts the leaves. Test: `mtl_instance_shutdown` with
  a report on `null:1`, the step order from the trace; at I a capture shows the IGMP leave before the
  device stops.
- **G-109** U; MS3. Liveness never depends on packets or time lock: `MTL_FAULT_LEG_DOWN` and
  `MTL_FAULT_TIME_LOST` never clear `MTL_HEALTH_LIVENESS`; readiness reports the time loss
  (`MTL_HEALTH_TIME_UNLOCKED`); `mtl_instance_get_health` takes no lock. Test: inject each fault while
  another thread polls health in a tight loop under lock instrumentation.
- **G-110** U; MS3. `MTL_FAULT_MANAGER_LOST` posts `MTL_EVENT_MANAGER_LOST` and sets
  `MTL_HEALTH_MANAGER_LOST`; running sessions continue and no call blocks (the create half is G-98,
  MS5). Test: inject the fault with sessions RUNNING; results keep arriving ON_TIME; every call
  returns within its bound.
- **G-111** U; MS1 (the AS calls), MS3 (the fork rule). The AS calls (`mtl_instance_interrupt`,
  `mtl_instance_abort`) are safe at any time, during and after close, and never write a recycled
  descriptor; in a forked child every call but close fails with `-MTL_EBADF` (`FORKED`). Test: a
  signal storm of the AS calls across open, close and re-open; `fork()`, then each function in the
  child; also WH15 and WH21 (the latter in the non-ASan job).
- **G-112** U, I; MS1 (U), MS3 (I, wave 4). Re-open: after a close, `mtl_instance_open` works again
  in the same process (EAL stays initialised); after a SIGKILL mid-stream, a new process opens the
  same VF without manual cleanup. Test: 200 open and close cycles on `null:1` under ASan; a NoCtx
  case kills its child mid-stream and re-opens the VF.
- **G-113** UB, I; MS2a. Rows: a TX rows unit re-submitted with a growing `used` sends each row once
  it is ready, never beyond `used`; RX reports rows in order through `mtl_rx_wait_rows`; a complete
  rows unit equals the frame unit. Test: the slice cases of `st20_digest.cpp` ported: rows released
  in steps, SHA-256 of the received frame, no packet of a row before its submit.
- **G-114** B; MS1, each later milestone. The samples call every unified function libmtl exports
  (`mtl_debug.h` exempt), as do the examples; every essence and direction has a TX and an RX sample
  once it is on the new API. Test: `nm -u` of the samples against libmtl's unified symbols, of the
  examples against `check.sh --tags`
  ([implementation-plan.md §4.2](implementation-plan.md#42-samples)); every sample runs 10 s on
  `null:1` (`from_legacy` on `kernel:lo`).
- **G-115** U (helpers), UB (engine); P; MS4a2 (the helper part from MS1). Every RFC 8331 field of
  contract.md §5.7 is written as specified, and RAW re-encodes what RX delivered bit-exactly, errors
  included. Test: random units (8- and 10-bit words and RAW, 0–255 entries, every line and offset
  code) through the TX binding and engine into a capture queue; the legacy
  `st40_rfc8331_decode_packet` (8-bit) and `mtl_anc_rfc8331_decode` both return the table; parity,
  Data_Count, checksum, word_align, Length, ANC_Count and F checked per RTP packet; both legs
  byte-identical.
- **G-116** UB; P; MS4a2. Every entry is sent once and in table order; no RTP packet carries more
  than 255 ANC packets or `max_udp_payload` − 20 bytes of ANC data; a new RTP packet starts exactly
  at `MTL_ANCF_NEW_RTP` and at a PsF segment change; a unit with no entry gives one empty RTP packet;
  the marker is on the unit's last RTP packet and on every empty one; at most 16 RTP packets per
  tasklet call, no per-word work on a tasklet. Test: `Anc.Packing`, `Anc.Burst16`.
- **G-117** UB; P (scheduled times), BE until N8 (wire, M); MS4a2. Every RTP packet of a feasible
  ANC unit ([timing.md §9.2](timing.md#92-the-anc-transmit-window-and-live-anc)) is inside its
  ST 2110-40 window at any capacity create admits, including when it follows any earlier unit of the
  same session. An infeasible unit is never early, its late packets are counted, and it never delays
  the next unit. Entries with `MTL_ANCF_AS_IS` are timed as unlocated and can be late against the
  window an analyzer derives from their wire line. Assumed: the port rate is available to the
  session (other sessions on the port and concurrent large ANC bursts are counted, not covered).
  Test: `Anc.Window*`, `Anc.CrossUnit{Line1125,Line1100,Empty}{Full,Tiny}`, `Anc.Infeasible`, EBU
  LIST (N8).
- **G-118** UB, U; P; MS4a2. RX skips and counts each corrupt ANC packet, one total per unit and per
  cause in the stats (DID or SDID parity, checksum, DID 00h, Type 2 SDID 00h, 10-bit protected
  words, 8-bit words that break the parity convention) and keeps the rest of its RTP packet (a
  Data_Count parity error or a packet past Length ends it); RAW skips only a packet past Length and
  flags the others. The entry after skipped, truncated or missing packets carries
  `MTL_ANCF_GAP_BEFORE`, and no COMPLETE unit carries it; entries out of raster order or outside the
  unit's field carry `MTL_ANCF_AS_IS`. RX delivers entries in wire order across ST 2022-7 legs with
  `rtp_index` (the sequence distance from the packet after the previous unit's marker), truncates
  beyond capacity with a count and `MTL_RX_INCOMPLETE`, and every delivered entry passes submit.
  Test: `Anc.RxSkipCauses`, `Anc.RxGapBeforeAfterFill2022_7` at 64, 65 and 4 096 RTP packets per
  unit and at the largest admitted P (records by sequence, O(1) per packet), `Anc.RxAsIs`,
  `Anc.RxTruncate`, `Anc.RxRelayAccepts`.
- **G-119** UB (ASan), U; P; MS4a2. A unit is refused at submit with nothing queued, or sent whole; a
  write to the planes after submit never reaches the wire; offsets are checked before any word is
  read (ASan clean on corrupted tables); the session-owned allocations are fixed at create (TX 9 + L
  + 1, RX 9 + 3L for L legs, attached pools one fewer), none at attach or after; a stale or second
  submit writes nothing (`Anc.SubmitStaleLeaseWritesNothing`); `mtl_session_update` MEDIA/POOL is a
  re-create. Test: `Anc.SubmitOffsetsFirst`, `Anc.SubmitRefusedQueuesNothing`,
  `Anc.WriteAfterSubmit`, `Anc.NoAllocAfterCreate` (the allocation hook, counted with the scheduler
  objects already present).
- **G-120** U; P; MS4a2. Every submit check of contract.md §5.7 returns `-MTL_EINVAL` naming its
  field, and the slot returns to the pool without a result. Test: `Anc.SubmitChecks`.
- **G-121** UB, I; P; MS4a2. Legacy ST40 TX frames whose RTP count the estimate undercounted (SF-87)
  send every ANC packet, and a frame with an entry the encoder refuses (SF-88) sends nothing instead
  of a partial frame; every other legacy frame is byte-identical to the baseline. Test:
  `St40Legacy.Planner` over the 5 120 equal-size shapes, `St40Legacy.NoPartial`; the G-99 pcap diff
  of the legacy gate.
- **G-122** UB, I; P; MS4a2. The legacy ANC wire and reports are unchanged for legacy sessions and
  st40p: DID 00h, line 0, any order, the legacy RX parse; only SF-87 and SF-88 frames differ. Test:
  G-99's pcap diff with the GStreamer defaults (`tx-did` 0, line 0) and the noctx `st40p_handler`
  tables; the frozen `St40*` and `St40p*` cases.
- **G-123** U; MS1. A raster rate is validated in one order (contract.md §3.3); `info.raster`
  carries the reduced value. Test: `RateRules.zero_terms` (each use, also DETECT_MAX),
  `RateRules.reduce_then_limit`, `RateRules.field_rate_guard`, `RateRules.engine_range` (frame rate
  1–120 every scan; interlaced 1000/1001 refused), `RateRules.info_reduced`.
- **G-124** U; MS1. `mtl_raster_from_legacy` maps every legacy pair as migration.md §5.1. Test:
  `Legacy.raster_from_legacy`: 22 pairs, `ST_FPS_MAX`, 12.
- **G-125** U, UB; MS1 (the flag), MS2a. Sender types are granted by D-143 and every grant passes its
  ST 2110-21 model. Test: MS1: `Grant.ms1_non_compliant_flag` (NL, a 1920×1200 N and a 1080i N set
  the flag; W and 1080p N do not). MS2a: `Grant.table`, `Grant.w_bound_vrx_model` and
  `Grant.n_model_interlaced`; SD rows flagged. `Grant.w_bound_vrx_model` runs the engine's TRS, VRX0
  and TRO through the ST 2110-21 §6.6.1 and §6.6.2 models at 1080p59.94, 1920×1200p60, 720p119.88,
  2160p59.94, 4096×2160p59.94 at ≥ 900 k pps with NL's CMAX, and 1080p 1 fps, plus the 9 000- and
  9 718-packet floor cases; 4320p12 is refused. `Grant.n_model_interlaced`: 1080i59.94, 1080i50 and
  1080PsF23.98 on TSC and RL against §6.3.3 with TLINE/2: peak ≤ VRXFULL, no underflow.
- **G-126** U, I; MS1 (direct clocks), MS2a (snapshot). On a wrapper `mtl_time_now` is DP with the
  legacy clock's flags and state. Test: MS1: `Bridge.time_direct_clocks`,
  `Bridge.time_enotsup_user_ptp`, `Bridge.tai_refresh_step_bound` (the step at a refresh within its
  bound against a fake legacy clock drifting at 500 ppm); I with and without `--ptp`. MS2a:
  `Bridge.time_flags_per_clock` (a fake legacy impl per row, a UTC user fn),
  `Bridge.holdover_threshold` (stop `master_results`; a 1 s result period gives 4 s),
  `Bridge.snapshot_two_records`, `Bridge.stale_estimated`, `Bridge.monotonic_never_backward`. The
  MS1 direct-clock rows are exercised by the U tests and the `from_legacy` sample only, because both
  harnesses install user functions.
- **G-127** U; MS1, MS2a. Lifetime and options on a wrapper. Test: MS1:
  `Bridge.uninit_with_open_wrapper` (a close that returns 1, then 0, with `mtl_uninit` returning
  `-EBUSY`, then 0), `Bridge.c_key_ebusy`. MS2a: `Bridge.option_classes` (a C key read back;
  `instance.sched_sleep_us` reaches the legacy schedulers; a session default from the legacy
  `pacing` and the typed `max_udp_payload` default), `Bridge.second_wrap_reference` (two references,
  `mtl_uninit` `-EBUSY` until the last closes), `Bridge.shared_open_joins_wrapper` (a SHARED open
  with a subset of the legacy ports joins; one naming a time source or another port is
  `INSTANCE_MISMATCH` with its detail).
- **G-128** U; MS1 (helper, parity), MS4a1 (A/V). `mtl_grid_offset` is in [0, TFRAME) and exact; one
  δ keeps two TAI sessions in phase; interlaced first fields land on even indices. Test:
  `Sync.grid_offset`, `Tai.interlaced_parity`, `Sync.av_phase_two_sinks` (snap error ≤ 1 ns, audio
  < 1 sample).
- **G-129** UB; MS3. RX rate detection. Test: `Detect.candidates`, `Detect.skipped_frames` (15 %
  skips, and 30 % in bursts), `Detect.approx` (1199/20 with the flag over ≥ 32 frames; 2997/50
  consistent with 60000/1001).
- **G-130** U; MS1. RX library slots: planes packed at the stride, each slot 64 B aligned,
  `pool_slot_pitch` a multiple of 64 and ≥ `unit_bytes` + 64; the 64 B after every dequeued unit read
  zero, also after the application wrote them during an earlier lease; V210 rows of 128 B per 48
  pixels; V210 at 1280, 2048 and 4096 wide `-MTL_ENOTSUP` until E15. Test:
  `UnifiedRx.LibraryPoolLayoutPacked` (UYVY 1920 and 1278, YUV422P10LE 1920 and 1282, V210 720 and
  1920; V210 1280, 2048 and 4096 → ENOTSUP); `UnifiedRx.TailZeroAtDequeue`: poison every slot's tail
  with 0xA5 after `mtl_session_get_slot`, run 100 units with each lease's tail rewritten before
  release, and assert zeros at each dequeue.
- **G-131** U; MS1. A wrapped unit released after its session's close, from another thread, retires
  the session with no further call; released after the instance's close it returns 0,
  `mtl_session_get_status` reads RETIRED, ASan clean. Test: `UnifiedRx.ReleaseAfterCloseRetires`;
  `UnifiedRx.ReleaseAfterInstanceClose`, `CoreClose.orphan_freed_at_next_cp`; ex20, the element
  freed before release (ASan).
- **G-132** U; MS1. The latency fields equal the formulas exactly and are never negative; recomputed
  after an S option; `MTL_INFO_LATENCY_INFEASIBLE` if and only if max < min, and `ceil(min / U) + 1`
  clears it; `convert_ns` > 0 with conversion, > 0 on TX (the copy calibration), 0 on RX without
  conversion; query matches get_info except `convert_ns`. Test: `UnifiedInfo.LatencyFormulas` (one
  and two legs, flush offset, link offset set and absent, pools 2–8, `RX_LATEST`, TX RL and TSC,
  59.94p and 119.88p); `UnifiedInfo.InfeasibleRemedy`.
- **G-133** U, UB; MS2a. The RX max bound: a consumer that releases each unit just before media time
  + max misses none, with one leg, two legs and skew; one that holds a unit one period longer misses
  units; with `TIMEBASE_SUSPECT` the test only checks that the status is set. Test:
  `UnifiedRx.LatencyMaxBound` on `null:1` with the test clock and skew injection; over the binding
  with `FAULT_DROP_PKTS`.
- **G-134** U; MS2a. Log sinks: each sink gets exactly its level and filter; `fn` never runs on a
  tasklet or two at once; remove and close from `fn` are safe; lines produced inside `fn` are dropped
  and counted; the last close hands its lines over first; without a sink, CP lines are written
  synchronously and are byte-identical to legacy; in a forked child add is `-MTL_EBADF`. It also
  requires: `dropped` is exact for an instance's rings; remove returns 0 and `fn` is never called
  again; remove from inside `fn` returns `MTL_RETIRING` without deadlock; instance close from `fn`
  is `-MTL_EDEADLK`; on a bridged instance a DEBUG sink makes legacy sessions' DEBUG lines reach it.
  Test: `UnifiedLog.TwoSinksLevels`, `.InstanceFilterShared`, `.FloodDropsCounted`,
  `.RemoveRacesDispatch` (10^5 cycles, TSan), `.RemoveSelfInCallback`, `.CloseFromCallbackDeadlk`,
  `.CallbackLinesDropped`, `.CloseDrainsLines`, `.StderrLegacyFormat`, `.BridgeLegacyLevel`,
  `.ForkedChild`, `.RateLimitBeforeFormat` (10^5 lines a second from one tasklet site: at most about
  10 formatted a second, the rest in `dropped`).
- **G-135** U; MS1 (the `enum_names` format, in H1b's .def test), MS2a (parse). Option text: every
  key of `mtl_option_list` round-trips through `mtl_option_parse`; every `enum_names` matches
  `^[a-z0-9_]+=-?[0-9]+(,[a-z0-9_]+=-?[0-9]+)*$` with values in range; "/N" stores `MTL_INDEX(N)`;
  STR keeps ',', '=' and ':'; errors carry reason and field. Test: `UnifiedOptions.EnumNamesPairs`
  (MS1), `.ParseRoundTrip`, `.ParseErrors`, `.ParseReturnsKind`.
- **G-136** U; MS1 (through open), MS2a (export). The `MTL_PORTS` grammar: gateway, IPv6 in
  brackets, no address, `null:` IDs, `env:`, whitespace, normalised duplicates (`null:1` with
  `null:01`; `AF:01.0` with `0000:af:01.0`), prefix 0 rejected, a trailing comma, a 64-byte name;
  `mtl_instance_open(NULL)` with `MTL_PORTS` gives the same specs as `mtl_port_parse`. Test:
  `UnifiedPorts.Grammar` (table-driven), `.Normalise`, `.OpenMatchesParse`.
- **G-137** U, UB; MS2b. Provide: oldest-first assignment; reception-order delivery with the right
  cookies; a dropped unit's destination taken first; none held → missed and counted, never library
  memory; after a hand-back every cookie with g < `provide_gen` not dequeued is never written again
  and the others are; a destination held by a TX unit comes back only after that unit's result. It
  also requires: a provide session starts with no slot, and a session with neither slots nor
  provides fails `POOL_TOO_SMALL`. Test: `UnifiedProvide.Order`; `.MissedWhenEmpty`; `.StartRules`;
  `.HandBackExact`: two threads, one providing in a loop, the other cycling through stop and start,
  discard in RUNNING, detach in STOPPED, and no-op stops, 10^4 times each, every returned destination
  poisoned (ASan on `null:1`; UB with DMA) and the ledger compared; `.GenerationWrap` (start at
  2^31 − 3); `.HoldDelaysReturn`; `.TemplateCookie`.
- **G-138** U, UB; MS3. Publication: every format within the maximum is published, each format's
  first unit flagged with the right `raster` and `format_seq` (1 at each start); nothing before the
  first (`RX_DETECTING`) and nothing above the maximum until it fits again; a skipped frame, one stray
  packet, one lost unit or lost bottom rows never re-detect; two violating units do. It also
  requires: above the maximum nothing is published (`FORMAT_CHANGED`, `RASTER_MISMATCH`); a rate
  that fell to half and a smaller raster at the same rate re-detect; E14 measures width from SRD
  (4096 × 2160 → 4096); no allocation on the tasklet. Test: **U** on `null:1`: the null binding
  reports the TX unit's raster as the detection, so `UnifiedDetect.Publication` tests the
  publication rules (send 1280 × 720p50, 1920 × 1080i29.97, 3840 × 2160p25 above a
  1920 × 1080p59.94 maximum, then back to 720p). **UB** over the binding with the engine's packets:
  `UnifiedDetect.Dci4096`, `.SkippedFrameNoChange`, `.StrayPacketNoChange`,
  `.TwoViolationsRedetect`, `.RateHalvedRedetects` (59.94 → 29.97 and 50 → 25),
  `.SmallerRasterRedetects` (1080p50 → 720p50), `.BottomLossNoChange`, `.NoTaskletAlloc` (G-38's
  malloc hook); the rate cases are G-129's.
- **G-139** A; MS3. FFmpeg: a 720p → 1080p change within the maximum decodes through rawvideo and
  the CLI with frames of both sizes, zero-copy where the fallback allows. Test:
  `test_ffmpeg_rx_format_change`, plus a unit test of the PARAM_CHANGE packet.
- **G-140** A; P; MS6. GStreamer: caps renegotiate in-band; LATENCY answered live; two legs with
  pool 5 configure; `compositor` with 2 inputs and pool 5 loses no unit, and with pool 3 it copies
  (the `copies` count rises) and still loses none. Test: `test_gst_rx_caps_renegotiate`,
  `test_gst_rx_latency_query`, `test_gst_rx_compositor`.
- **G-141** U; MS1 (MS2a part with G-134). `mtl_instance_params.log_level` DEBUG makes an unbridged
  MS1 instance print DEBUG lines; 0 leaves INFO; a second open with another non-zero level is
  `-MTL_EEXIST`; with a bridged instance open, an unbridged open with 0 leaves the legacy level and
  one with another level fails; from MS2a, removing the last sink restores the level. The mismatch is
  `INSTANCE_MISMATCH`, field `log_level`. Test: `UnifiedLog.InstanceLogLevel`, `.BridgedLevelKept`,
  `.LastSinkRestores`.
- **G-142** U, I; MS1 (U), MS2a (I, S1b). The wake flush of one loop iteration wakes at most the
  bound of D-142 of marked objects and queues; those beyond it are woken in later iterations, round
  robin; a loop never sleeps while an entry is marked (D-142). Test: WH16 `WaitFlush.count_bound`
  (UnitTest, `mt_sch_harness.c`); the flush model; S1b's ST2 and ST3; the model job ("The wait
  evidence" of [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-of-ms1ms3)).

## 3. Packet-unit guarantees G-PKT-1…7

MS5, on the null backend: a TX chunk completes at its scheduled instant, a loopback RX session on
the same port receives the packets with synthetic arrival times.

| ID | Guarantee | Tier | How it is tested |
|---|---|---|---|
| G-PKT-1 | verbatim: with `packet.set_fields = 0` every byte from the RTP header on reaches the wire unchanged, on every leg | U, I | `null:1` loopback without `MTL_PKT_RX_INCLUDE_L2`, byte compare; I: capture compare |
| G-PKT-2 | stamping writes exactly the declared fields, identical on both legs | U | loopback, field by field |
| G-PKT-3 | one result per chunk; accepted = published + suppressed under `MTL_STOP_FLUSH`, discard, leg down and close | U | the identity check under fault injection |
| G-PKT-4 | more packets than `packets_per_unit` are rejected at submit (`PKT_COUNT`), short units flagged (`MTL_TXR_PKT_SHORT`), the grid kept | U | loopback with scripted counts |
| G-PKT-5 | RX never holds more than `packet.rx_ring_packets` NIC buffers; a full ring on one leg is filled by the other | U, I | `MTL_FAULT_DROP_PKTS` per leg plus a stalled reader |
| G-PKT-6 | dedup by sequence number: one delivery per sequence, `MTL_PKT_RX_NO_DEDUP` delivers both (`MTL_PKTE_REDUNDANT`), `gap` exact | U | loopback with injected loss and reorder |
| G-PKT-7 | `MTL_PKT_PACE_UNIT` passes the ST 2110-21 narrow check for video when the declared count is sent; `MTL_PKT_TIME_FROM_RTP` keeps a fixed RTP-to-launch offset | I | the timing oracle, EBU LIST |

Further tests: pcap replay (I): RxTxApp on the unified path replays a reference ST 2110-20 pcap and
`tests/tools/RxTxApp/script/loop_json/st22p_pcap.json` through packet sessions with verbatim headers
and `MTL_PKT_TIME_FROM_RTP` into frame RX sessions; the digest must match and the TX timing must
pass the oracle. The out-of-order and truncated-frame cases of
`tests/integration_tests/st20/st20_digest.cpp:567` and `st20_meta.cpp:256` move to the U tier
through packet units; the ST40 RTP fuzz target is re-pointed at the packet RX path.

## 4. Retired IDs

A retired number is not reused.

| ID | Note |
|---|---|
| G-69 | retired; its rule is G-52 |
| G-79 | retired |
| G-PKT-8 | retired; the packet-unit guarantees are G-PKT-1…7 (§3) |
