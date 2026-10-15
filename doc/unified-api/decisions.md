# Decisions

| | |
|---|---|
| Status | Maintained |

The design rationale of the unified API for maintainers: one line per design decision (D-xx),
the prior art behind the main rules, the defaults the ST 2110-20 milestones implement, and the
implementation issues still to apply (OI-xx). The headers in
[sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) are normative; where a row
and a header disagree, the header wins.

## 1. Keeping this file

- A D-xx states one decision in its current form; **Where** names the document that specifies it,
  with its section where the row gives one (`contract.md §5.5`); a new row names its section.
- IDs are stable, never renumbered or reused; the next free numbers are D-245 and OI-85.
- D-12, D-20, D-22, D-25, D-28, D-29, D-35, D-40, D-46, D-61 and D-65 (superseded by D-78,
  D-80, D-74, D-82, D-44, D-50, D-77, D-55, D-73, D-79, D-72) and D-47 were dropped in
  `b7885ddf`; they stay unused.
- OI-82 was resolved by D-215 and D-218 and left §5; its ID stays unused.
- A superseded row stays. It reads "Superseded by D-n (reason). Was: <old text>", so new text
  never sits in the old row; only the new row states the current decision.
- An amended row keeps its text and ends "Amended by D-n: <what changed>"; the new row states the
  change.
- A row too long for the table puts "(continued below the table)" where its text breaks off,
  before any "Amended by"; the list after §2's table holds the rest as "D-n, continued: …".
- The defaults of §4 carry stable IDs, DF-01…DF-29, like D-rows.
- A changed decision changes the header in `sketch/` and the document in **Where** in the same
  commit.

### 1.1 Decisions by topic

Each D-row and OI-row under one topic; the home names the documents that specify most of them.

| Topic | D-rows | OI-rows | Home |
|---|---|---|---|
| Instance, lifecycle and commands | D-08, D-36, D-44, D-48, D-58, D-60, D-62, D-121, D-186, D-192, D-193, D-210, D-211, D-236–D-238 | OI-2, OI-11, OI-49, OI-61, OI-62 | contract.md, core.md |
| Data path and results | D-01, D-02, D-03, D-14, D-42, D-50, D-75, D-77, D-88, D-119, D-120, D-183, D-203, D-207–D-209, D-213 | — | contract.md, core.md |
| Memory, buffers and conversion | D-16, D-17, D-54, D-55, D-56, D-76, D-86, D-100, D-151, D-202, D-214 | OI-41, OI-59, OI-60 | contract.md |
| The core and its bindings | D-06, D-07, D-99, D-103, D-111, D-118, D-124, D-126, D-127, D-171, D-172, D-174, D-176, D-177, D-179, D-180, D-182, D-226 | — | core.md |
| Waiting and queues | D-04, D-30, D-43, D-68, D-79, D-102, D-122, D-141, D-142, D-158–D-170, D-181, D-206 | OI-63, OI-66, OI-80 | core.md, contract.md |
| Time sources and media time | D-09, D-10, D-11, D-15, D-31, D-38, D-41, D-51, D-85, D-132, D-144, D-205, D-215, D-216, D-221, D-229–D-233, D-235 | OI-7, OI-44, OI-68, OI-72 | timing.md |
| Launch, late and underrun policy | D-13, D-39, D-52, D-53, D-78, D-130, D-131, D-173, D-212, D-218–D-220, D-234, D-239, D-241, D-243 | — | timing.md |
| RX | D-37, D-150, D-152, D-155, D-156, D-199, D-204, D-225, D-227, D-228, D-242 | OI-25, OI-26, OI-67, OI-73, OI-74, OI-84 | contract.md |
| Formats and sender types | D-32, D-101, D-143, D-222 | OI-42, OI-71, OI-76, OI-77 | contract.md, timing.md |
| ANC and fast metadata | D-67, D-70, D-145–D-149, D-157, D-201 | OI-45, OI-78 | contract.md, timing.md |
| Packets, RTP and legs | D-26, D-59, D-82, D-115, D-133, D-134, D-198, D-224 | OI-56 | contract.md, timing.md |
| Options, stats, errors and logs | D-19, D-21, D-73, D-80, D-81, D-84, D-128, D-129, D-139, D-153, D-154, D-175, D-244 | OI-40, OI-50, OI-54, OI-65, OI-69 | contract.md, engine.md |
| Headers and structs | D-05, D-33, D-34, D-45, D-71, D-72, D-74, D-97, D-104, D-105, D-137, D-140, D-185, D-187, D-195, D-217, D-223 | OI-70 | the headers, contract.md, README.md |
| Versioning and export | D-23, D-83, D-106, D-108, D-190, D-191, D-194 | OI-64 | migration.md |
| Legacy APIs and engines | D-18, D-24, D-27, D-57, D-112, D-114, D-123, D-125, D-178, D-189, D-197, D-240 | OI-3, OI-81 | migration.md, engine.md |
| Validation and process | D-64, D-66, D-87, D-98, D-107, D-109, D-110, D-113, D-135, D-136, D-138, D-184, D-188, D-200 | — | implementation-plan.md |
| Deployment and pods | D-49, D-63, D-69, D-89, D-90, D-91, D-92, D-116, D-117 | OI-23, OI-43, OI-75 | deployment.md |
| NMOS, SDP and IPMX | D-93, D-94, D-95, D-96, D-196 | — | nmos-ipmx.md |

## 2. Decisions

| ID | Decision | Where |
|---|---|---|
| D-01 | One slot and lease state machine and one submit/dequeue contract for every provisioning path (library pool, attached, exported) | contract.md |
| D-02 | App-driven push on TX (`mtl_tx_acquire` → fill → `mtl_tx_submit`); the library never pulls frames through application callbacks | contract.md |
| D-03 | Results are returned in submission order (`seq`) from the order ring, written in place by the completer (D-119); acquire reserves the result entry (D-120), so neither a result nor slot reuse ever waits; state changes are session events. Amended by D-209: a result never waits; a slot that completes before an older one is reused only after the older ones are done | core.md |
| D-04 | No application code on tasklets: the tasklet side of a hand-off is a release store and one RMW of the event word, wake-ups go through the object's event word (D-158) and `mt_wake()`; an inline notify (`MTL_LATER`) only if busy polling (W0) cannot close the latency gap | core.md, engine.md |
| D-05 | Every public function has a call class and its milestone in one macro (`MTL_API_CP(n)`, `_DP(n)`, `_DPC(n)`, `_WT(n)`, `_AS(n)`, n = 1–7 or `LATER`), the class enforced in debug builds from MS3 | contract.md, mtl.h |
| D-06 | One internal core under every API owns the contracts, with one binding per essence and direction over today's engines (D-99) | core.md |
| D-07 | Typed 64-bit handles over grow-only chunked tables, 0 = null; a lease is session index:16, slot:16, random generation:32; stale lease `-MTL_ESTALE`, foreign or closed `-MTL_EBADF`; an in-flight counter on its own cache line guards destroy. Amended by D-226: generation:32 from the entry's lease base, raised at retire; `-MTL_EBADF` for a foreign lease with a session handle | core.md |
| D-08 | Nine states (CREATED, ARMED, RUNNING, DRAINING, FLUSHING, STOPPED, ERROR, CLOSING, RETIRED); start now, at a TAI or an index with `preroll_ns`; stop DRAIN or FLUSH; `mtl_session_discard`; results readable until close | contract.md |
| D-09 | Media time and launch time are separate; RTP = `floor(M × R) mod 2^32` from media time, one rule for every essence except `MTL_MEDIA_SENDER` (Phase 7, D-95) | timing.md |
| D-10 | The default video media time is the frame epoch `N × TFRAME`, not the TX cursor | timing.md |
| D-11 | Media-time math is exact rational arithmetic from the epoch, or from the start's T0 under `MTL_WHEN_ORIGIN`, never accumulated, in 64-bit integers split by quotient and remainder, every intermediate below 2^63 (timing.md §3.3); a rate is a reduced fraction within the IPMX MIB widths (num ≤ 4194303, den ≤ 1023, TR-10-2 §10) | timing.md, contract.md |
| D-13 | Explicit late policy (`tx.late_policy`: DROP or DEFER; AUTO → DEFER, INDEX/TAI → DROP) and underrun policy per essence; an infeasible exact request fails, never falls back; bounded `MTL_LATE_SEND_LATE` (`tx.late_tolerance_ns`, `WOULD_OVERLAP`) is Phase 7 under `MTL_LATER` | timing.md |
| D-14 | TX results carry deadline, sent time and a signed `margin_ns`; `MTL_TX_ON_TIME` is an admission verdict; `mtl_tx_get_next` names the next index and its submit deadline. Amended by D-215: a signed `margin_ns` where a deadline exists; `MTL_TIME_NONE` otherwise | timing.md |
| D-15 | Superseded by D-215 (one "no value" constant replaces the flags). Was: Every time is `int64_t` TAI ns with a validity flag (`MTL_UNITF_*_VALID`, `MTL_TXR_*`, `MTL_TIMEF_*`), never a sentinel | timing.md |
| D-16 | Regions (`mtl_mem.h`) are refcounted, page-aligned, never rounded outward, mapped at first attach or into named ports; `mtl_session_attach` takes regions, no IOVA call; shmem, memfd, hugetlbfs and pinned host memory direct, files copy only; a non-hugepage import's IOVA is its VA (OI-41) | contract.md |
| D-17 | The data path does not depend on where memory came from: direct when possible, else a copy reported per session (`MTL_INFO_DIRECT`) and unit (`MTL_TXR_COPIED`); `MTL_SESSION_REQUIRE_DIRECT` fails at create | contract.md |
| D-18 | The terminal TX result comes after storage access and the transport outcome are both finished (the engine's done callback after the last buffer reference, MF1) | engine.md |
| D-19 | Capabilities per port, dry-run `mtl_session_query`, REQUIRE/PREFER/OFF requests with granted values; runtime downgrades are events (`MTL_EVENT_PACING_CHANGED`) | contract.md |
| D-21 | One error vocabulary, `MTL_E*` = Linux errno values, one meaning each (`-MTL_ESHUTDOWN` only after the app's own stop or close, `-MTL_EIO` ERROR, `-MTL_ENODEV` device gone, `-MTL_EAGAIN` nothing now); errno values because every caller already maps them (`strerror`, frameworks' errno mappings) and they cross language bindings unchanged | contract.md |
| D-23 | One DSO: libmtl carries the unified API in its own version nodes, per milestone (`MTL_UNIFIED_EXPERIMENTAL_<rev>_MSn` and its parts, each inheriting the previous, sealed at every release, D-191), with the API shell compiled in, so there is no cross-DSO core ABI and pkg-config stays `mtl` (abi.md §2) | migration.md, engine.md |
| D-24 | Engines first: legacy APIs get every engine fix; wire-visible changes (rounding, default RTP, ANC RTP, ST 2110-22 CBR, linear read schedule) only behind legacy opt-in flags, proven by the pcap-diff gate G-99 | migration.md, engine.md |
| D-26 | One flow descriptor (`struct mtl_flow`) for every essence and leg, without RTP identity (`ssrc` and `payload_type` are per session, D-133); flow changes apply on every leg at one `struct mtl_when`, prepared before commit, never holding a tasklet lock across ARP or IGMP | contract.md |
| D-27 | PR #1610 stays a reference until the unified header lands, then is closed with credit; its `mt_session_event.c` seeds `mt_wake` and the SF-15 fix | migration.md |
| D-30 | Async-signal-safe, sticky interruption (`mtl_interrupt(o, mode, targets)` and its wrappers): data waits return `-MTL_ECANCELED` until cleared; `targets` is a `MTL_WAIT_*` mask (0 = every target, OI-70), so an `unlock` cancels acquire without a reaper spinning; stop and close still work | contract.md |
| D-31 | Explicit time sources (`enum mtl_time_source`); tasklets read one published time base; on a clock step media times are kept (one rule, no per-session policy) | timing.md |
| D-32 | ST 2110-22 is its own essence (`MTL_CVIDEO`), CBR by default, `MTL_CVIDEO_VBR_MAX` opt-in and non-compliant; an oversize codestream fails at submit | timing.md |
| D-33 | Input structs grow through `struct_size` and named `reserved` fields; unknown non-zero bytes are `-MTL_EINVAL` | contract.md |
| D-34 | Names: essences `MTL_VIDEO` … `MTL_RTP`, verbs `mtl_tx_*`/`mtl_rx_*` and object verbs (D-104), timeout last; headers `include/mtl/experimental/`, shell `lib/src/unified/`, core `lib/src/st2110/core/`, both in libmtl (D-23) | contract.md |
| D-36 | Close is idempotent and deferred while leases are out: `mtl_close` returns 1 while retiring, 0 once retired, and a repeated close polls, never `-MTL_EBADF`; on a closing handle only status, the release of earlier leases and interrupt (a no-op) are valid. Not `-MTL_ETIMEDOUT`: the close has happened, only the memory waits for leases still held | contract.md |
| D-37 | RX pools: a full pool drops new units, or reclaims the oldest unread (`MTL_SESSION_RX_LATEST`); `MTL_SESSION_RX_BY_INDEX`; library pools zero-filled in dequeue (`MTL_SESSION_RX_NO_FILL` opts out), attached pools not (OI-25). Amended by D-242: library pools filled in dequeue (MS2a): video the format's black; audio, cvideo and fastmeta zero; ANC units are never filled | contract.md |
| D-38 | The media mode (AUTO, INDEX, TAI) is a session property; TAI snaps NEAREST with hysteresis, with no locked-phase snap, off-grid policy or phase-drift gauge in v1; duplicates are results (`SNAP_COLLISION`, `DUPLICATE_INDEX`), not errors; the binding takes the launch decision (D-131) | timing.md |
| D-39 | No source kinds: `min_tx_delay_ns` is the one knob (a camera sets one frame period + the pick-up lead, D-130); the launch delay L is reported, not configured. Amended by D-218: a camera sets one unit period (a field when interlaced); MTL adds its lead | timing.md |
| D-41 | Audio: a carry buffer, whole samples per packet, ±one packet absorbed in TAI mode, gaps keep the packet grid, only a discontinuity re-phases; `mtl_tx_write` for copy essences | timing.md |
| D-42 | Array reads (`mtl_reap`, `mtl_read_events`) take the caller's record size and return a count ≥ 1 or `-MTL_EAGAIN`; the typed `static inline` wrappers (`mtl_tx_reap`, `mtl_tx_reap_full`, `mtl_session_read_events`) pass `sizeof(*r)` | contract.md |
| D-43 | Superseded by D-159 (the portable descriptor is a queue's, not a per-object wait handle's). Was: wait handles are portable (eventfd; a Windows manual-reset event from MS2a), one per session or instance (`mtl_get_wait_handle`) | contract.md |
| D-44 | `mtl_instance_h` over versioned `struct mtl_instance_params`; `MTL_INSTANCE_SHARED` is one refcounted instance per process, a differing later open `-MTL_EEXIST` (`INSTANCE_MISMATCH`), no merge; runtime port open later; a joining open may name a subset of the ports (D-192) | contract.md |
| D-45 | The headers are normative, compile as C99 and C++17 with `-Wpadded -Werror`, size-check every public struct, and `check.sh` runs in CI | contract.md |
| D-48 | Commands are immediate (stop, flush, discard, interrupt) or at a boundary (flow, grid, leg); the control plane applies them for a detached session; one unacknowledged for a fixed 100 ms is ERROR (`CMD_TIMEOUT`); form D-103. Amended by D-236: a boundary command is acknowledged on receipt within this timeout; its APPLIED state follows at the boundary | core.md |
| D-49 | Close or ERROR on a stalled queue: bounded idle cleanup, then a worker restarts a dedicated queue or quarantines a shared one, up to a port reset; retirement after the device references are gone | engine.md |
| D-50 | Acquire and release are MP-safe; one submitter unless `MTL_SESSION_MT_SUBMIT`; readers take the reaper lock; destroy and stop always drain the in-flight counter; no single-reader or export-pool flag: an exported pool binds one framework wrapper to each slot with `mtl_tx_acquire_slot`, so it needs neither results nor `MTL_SESSION_MT_SUBMIT` (frameworks.md §7.1) | contract.md |
| D-51 | RX runs on the epoch timeline and reports an exact `media_index`; an RX start array arms all filters with one link offset; units complete at a due time (MS2); the join is made at the first start, kept across stop | timing.md |
| D-52 | `preroll_ns` in `struct mtl_when`; the horizon (`tx.horizon_ns`, 1 s) counts from the resolved start; a start stranding queued units beyond it fails (`BEYOND_HORIZON`) | timing.md |
| D-53 | `min_submit_lead_ns` = media time − pick-up deadline; `media_time_offset_ns` has one meaning, the producer's latency in TAI mode (it moves the index); the legacy `rtp_timestamp_delta_us` (an RTP trim with the launch fixed, AUTO and INDEX) is `tx.rtp_trim_ns`; live sinks use TAI + NEAREST, INDEX is for whole-timeline owners | timing.md |
| D-54 | A plane `stride` ≥ row bytes is direct for ST20; an interlaced unit is a field; a woven frame is two field slots over one region | contract.md |
| D-55 | A slot belongs to one pool; sessions share bytes through slots over one region; `unit.hold` lets one RX unit feed N TX sessions zero-copy (`mtl_tx_send_slot`) | contract.md |
| D-56 | Moving-cursor TX: `mtl_tx_acquire_layout` gives a slot a new layout per acquire (MS2); no dynamic pool; submit can read caller memory during the call (`MTL_SUBMIT_SRC_PLANES`, `u.plane[]`), the slot staying the admission and back-pressure token; on any session it copies or converts into the slot (`MTL_TXR_COPIED`), `-MTL_EINVAL` only with `MTL_SESSION_REQUIRE_DIRECT` | contract.md |
| D-57 | The ST20/ST22 cap of 8 frames is lifted in the engine (E11, MS2); until then `info.max_count` reports it | engine.md |
| D-58 | `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` in CREATED or STOPPED keeps handle, name, SSRC, stats and bindings | contract.md |
| D-59 | Every configured ST 2022-7 leg is reserved regardless of link, never pruned; `legs_disabled`; admin and oper state per leg; a link monitor (MS5) | contract.md |
| D-60 | Session names are copied, unique per instance, `""` generated; capacity by `MTL_QUERY_CHECK_CAPACITY` and the `capacity.*`, `port.free_*` stats; `mtl_session_get_info` has every granted value, the wire rate in `info.wire_bps` (query fills it too) | contract.md |
| D-62 | Create and start never wait for ARP or IGMP; flow resolution is a per-leg state; units on an unresolved leg drop (`WAITING_NEIGHBOUR`) | contract.md |
| D-63 | Losing MtlManager never kills or stalls the process: creates return `-MTL_EBUSY` (`MANAGER_LOST`), reconnect (MS5) re-announces held lcores | deployment.md |
| D-64 | Test substrate first: the null backend, the test clock, `mtl_debug_inject` (debug builds), `nicctl.sh` fault steps; guarantees marked P or BE; budgets from spike S0 | implementation-plan.md |
| D-66 | An engines track beside the milestones serves both APIs (ST30P/ST40P flag fixes, SF-15, the critical-path engine fixes); exact RTP math lives in the engines and the core, with no legacy helper (timing.md §14.2) | implementation-plan.md |
| D-67 | Live ANC and fastmeta follow the first video of their start array (its raster and launch delay), keep the video RTP and go in the ST 2110-40 window of the frame their video unit is sent in; no media-frame anchoring option | timing.md |
| D-68 | Waking is W2-deferred: a completing context on a library loop never makes a syscall; `mt_wake()` marks the object (`fired` lanes, a bit in the loop's bitmap) and the loop wakes a bounded number of marked objects and queues per iteration after its handler loop (D-142); the RX packet lcore wakes its one session after its handler; other threads wake directly | core.md |
| D-69 | Security posture of the new surfaces (imports, queue descriptors, the wake path, handle randomness, debug API gating); deprecation no earlier than two `vYY.MM` releases after notice and, for the legacy session API, F+2 no earlier than 12 months after F; published no-earlier-than targets, a slip rule and an LTS branch on F+1 (abi.md §7, OI-75) | deployment.md |
| D-70 | Small defaults: ANC `total_lines` 0 = the video's raster, else 1125; fastmeta rate 0 outside a video start is `FIELD_REQUIRED`; histograms are log2, with no layout keys | contract.md |
| D-71 | A lean `mtl.h` (C library includes only) and optional headers, each for one kind of program (their jobs: examples.md §1); the function counts per header and per milestone come from `check.sh` (D-137) | contract.md |
| D-72 | One `struct mtl_session_config` (one member per essence, plus `ssrc` and `payload_type`); `mtl_session_create`, `_query`, `_open`; `mtl_session_update(s, &sc, parts, &when, &planned)` reads only the members `parts` names, so no read-back call. Members, not a union: one `MTL_INIT` serves every essence and a wrong member is caught (`OTHER_ESSENCE`) | contract.md |
| D-73 | Typed fields for what most applications set; every other knob is a listable option key with presence semantics; no key for what another key, a request or a fixed value covers (`video.*` serves cvideo, `session.migrate` on the instance, the 100 ms ack timeout); a requirement on a key is its own key (`caps.pacing_req`); a frozen key may only become a no-op | contract.md |
| D-74 | Inputs carry `struct_size` and are initialised by `MTL_INIT(&s)`; outputs take a size argument | contract.md |
| D-75 | One `struct mtl_unit` (208 B) lent by acquire and dequeue, read by submit; 64-bit flags (TX bits 0–31, RX 32–63) | contract.md |
| D-76 | Buffers are pool slots named by index (`unit.slot`); `mtl_session_attach` lays out N slots; leases stay typed (`mtl_lease_h`) | contract.md |
| D-77 | A 96-byte core TX result, the full record by size; results always for application memory, opt-in (`MTL_SESSION_RESULTS`) for library pools | contract.md |
| D-78 | No group object: start and stop take arrays, because a group would be one more object with its own lifecycle for what is one instant; a start array is one direction on the epoch timeline, all or none; `MTL_WHEN_ORIGIN` in `struct mtl_when.flags` makes media index 0 of every started session the start's T0; created timelines and anchors are `MTL_LATER` | timing.md |
| D-79 | Superseded by D-159 (events are waited on with `mtl_wait`, a call with a timeout or a queue; queues are MS2a, not `MTL_LATER`). Was: events per session and per instance (`MTL_OBJ_INSTANCE`) through `mtl_read_events`, `mtl_wait` and `mtl_get_wait_handle`; `mtl_queue_*` and dispatch threads `MTL_LATER`, their scale measured by spike S1 (OI-66); no user posts | contract.md |
| D-80 | A stats registry of named, typed values (`mtl_stat_*`) with bulk DP reads; counters cumulative, never reset; lean status and info structs (D-128 for the per-scheduler blocks) | contract.md |
| D-81 | One reason vocabulary (`mtl_reasons.h`) for errors, states, events and TX results | contract.md |
| D-82 | Packet units (`MTL_UNIT_PACKETS`) on every essence and the generic `MTL_RTP` essence (MS5): a chunk of slots is one unit with one result; no mbufs, no app code on tasklets; defaults D-115 | contract.md |
| D-83 | Legacy headers leave the public set in three tiers (session, pipeline, core), stages A → B → F → F+1 → ≥ F+2; their symbols sit in `MTL_LEGACY`, are deprecated at MS7 (F) and leave it from F+2, at least 12 months after F; plugin ABI v2 and the conversion calls of `mtl_format.h` before F; pipeline-header removal set after MS4 | migration.md, abi.md §7 |
| D-84 | `mtl_last_error()` follows errno: only a failing call writes it | contract.md |
| D-85 | Superseded by D-229 (AUTO moves up without a step). Was: `MTL_TIME_SOURCE_AUTO` = a disciplined NIC PHC (from MS6, D-132), else `CLOCK_TAI`, else SYSTEM_TAI (estimated), chosen once at open; the built-in PTP client only when named | timing.md |
| D-86 | User meta lives in the slot's meta area, validated and copied at submit; an ANC packet table is plane 0 of its unit (D-145) | contract.md |
| D-87 | Coverage gate: every legacy capability has a unified home and a milestone, or is on the cut list ([coverage.md](coverage.md) §4.5, CUT-1…CUT-13) | coverage.md |
| D-88 | Submit consumes the lease on failure (a failed first submit returns the slot to the pool), except `-MTL_EBADF` and `-MTL_ESTALE`, which change no state; `-MTL_EAGAIN` for every "nothing now", it arms nothing (D-158); interrupts cancel data waits only | contract.md |
| D-89 | The last `mtl_instance_close` shuts down network first, MtlManager grants last: 0, 1 or `-MTL_EIO` (`QUEUE_QUARANTINED`), and a repeated close polls as in D-36; `mtl_instance_shutdown` adds a report; handle slots never freed (R4) | deployment.md |
| D-90 | R8: no signal handler or `atexit` in the library (DPDK's SIGBUS handlers excepted), close-on-exec, a fork closes MTL's descriptors in the child, nothing found by PID | deployment.md |
| D-91 | Lock-free health (`mtl_instance_get_health`); liveness independent of links, PTP and legs; open waits for no link, neighbour or lock and fails with one environment reason (600–614) | deployment.md |
| D-92 | CPUs from the affinity mask (MtlManager, `MTL_CPUARB_LOCKS` or none; no SysV table); threads pinned; no-IOMMU refused without `instance.allow_noiommu`; files only in `instance.runtime_dir` | deployment.md |
| D-93 | IS-05 activation at the index boundary by the clock (`planned_tai_ns`, `status.update_state`, `MTL_EVENT_UPDATE`; both legs may share a port through an explicit `flows[1].port`), the atomic destination and source change in MS5 (continued below the table). Amended by D-236: immediate (`when` NULL or `MTL_NOW`) in MS3; at an instant in MS5 | nmos-ipmx.md |
| D-94 | SDP render and parse (`mtl_sdp.h`) on public calls only, in the companion library `libmtl_sdp` (`ecosystem/sdp/`), outside libmtl's frozen set (D-196); with NX-4, else Phase 7 (`MTL_LATER`) | nmos-ipmx.md |
| D-95 | IPMX (Phase 7, under `MTL_LATER`): `session.profile` changes defaults and labels only (N keeps its Type N CMAX, within the IPMX ceiling, and adds the IPMX VRX check); `rtx.*`; `MTL_MEDIA_SENDER`; AUTO re-evaluation; IGMPv2; RTCP sender reports with the Info Block and FREERUN come earlier (D-134). Amended by D-229: of AUTO, only the FREERUN fallback stays Phase 7 | nmos-ipmx.md |
| D-96 | PEP encryption (`crypto.*`, `mtl_crypto_set_key`), no IV reuse under one key, HDCP keys never in MTL; built after its cost spike (Phase 7) | nmos-ipmx.md |
| D-97 | Typed configuration only, no spec strings in any struct; inline `mtl_flow_parse()`; exported `mtl_port_parse`, `mtl_option_parse` (D-154); enums in legacy order (+1 where 0 is "not set"); `mtl_flow_ipv4()`, `raster.fps`, `audio.ptime`; `MTL_PORTS` is the only string; SDP stream parameters are typed fields (`tsmode`, `max_udp_payload`, `troffset_us`, `timing_model`) | contract.md |
| D-98 | Port first: MS1–MS6 port today's functionality plus the Kubernetes lifecycle; NMOS extras, SDP, IPMX and PEP are Phase 7 under `MTL_LATER` | implementation-plan.md |
| D-99 | One core in libmtl with one binding per essence and direction (plus packet and null): a session is a slot table shaped like a lease table (D-118, seven slot states; a completion claims, writes the result, then publishes); the binding implements the engines' existing callbacks, no new entry point for frames and rows (ANC: D-149); legacy `st*p_*` become wrappers on the core | core.md |
| D-100 | Conversion and codecs are a transform state on the slot (claim, done) shared by the caller, converter plugin threads and ST 2110-22 codec threads | core.md |
| D-101 | Slice mode is required: `MTL_UNIT_ROWS` on video in MS2 through `query_frame_lines_ready` and `notify_slice_ready`; the engine adds a `query_frame_lines_ready` return code that ends a frame early (TRUNCATE) and `sc.video.troffset_us`; interlaced rows are kept | implementation-plan.md, engine.md |
| D-102 | MS1 ships W2-deferred behind one internal `mt_wake(object, lanes)`, bounded by count per iteration (D-142); D-170's patterns, a slack gate, then the notifier thread (D-165), are built only by D-142's rules (S1, MS2a) | core.md |
| D-103 | Commands are one `ctl` and one `ack` word per session, read by the session's tick (D-127: ack, RX due time, idle cleanup, heartbeat); flush and discard are CASes on the slot table; MS2 | core.md |
| D-104 | Object verbs (`mtl_close`, `_interrupt`, `_wait`, `_reap`, `_read_events`) over `struct mtl_object`, and the lease release `mtl_release(s, lease)`, with typed inline wrappers; queues in `mtl.h` (MS2); RTCP and PEP in `mtl_ipmx.h`; SDP in `mtl_sdp.h` of `libmtl_sdp` (D-94) (continued below the table). Amended by D-226: `s` may be null | the headers |
| D-105 | Struct rules: `raster.fps` rational only; TR offset only `troffset_us` (video, rtp; whole µs); 64-bit unit flags; growth room checked by `check.sh` (essence members 128 B, `mtl_when.flags` + `reserved[3]`, `mtl_port_spec` +32 B, `mtl_flow` +16 B); `used` 0 = a whole video frame, rows count rows, bytes and packets literal; acquire resets the TX meta area | the headers |
| D-106 | A function is exported in its milestone's open node, sealed at the exit and every release (D-191); a call above `MTL_TARGET_LEVEL` (default `MTL_LEVEL`; `MTL_LEVEL + 1` in-tree, set in each project's `meson.build`, never in `mtl.pc`) fails to compile naming its milestone (GCC, Clang ≥ 14), else to link; a missing node fails at load; unbuilt values are `-MTL_ENOTSUP` | contract.md |
| D-107 | AI agents write the code; the maintainer's review sets the pace: a commit ≤ 1.5 k lines (mechanical moves exempt), `mtl-reviewer` first, ≤ 3 review units awaiting review and ≤ 2 developer tasks in flight, where a task is one review unit: one commit or a series of at most four, each commit under the cap and each passing the legacy gate | implementation-plan.md §5.8 |
| D-108 | MS1 changes no existing export (the version script holds only the unified nodes, no `local: *`); the soname, `MTL_LEGACY` and hidden internals wait for MS3's `nm` audit of leaked-symbol users, and the legacy symbols are deprecated at MS7 (F) and leave `MTL_LEGACY` from F+2 (D-83; abi.md §2) | implementation-plan.md |
| D-109 | Two validation stacks while both APIs exist: frozen `KahawaiTest` (with NoCtx) and RxTxApp, where harness bugfixes enter like legacy ones, and their new-API copies `UnifiedKahawaiTest`, `UnifiedRxTxApp`; at F+2 the session-level suites join the unified harness, the rest goes, the unified binaries take the old names (F2-1). Amended by D-188: bugfix approval | implementation-plan.md §4.1 |
| D-110 | Acceptance runs RxTxApp and `UnifiedRxTxApp` as two pytest apps (`--app`) on the same configs; the copy keeps RxTxApp's CLI, JSON and output, and `compat_log.c` prints the parsed lines in legacy format (D-177), so the adapters survive; a key only `UnifiedRxTxApp` knows needs `"api": "unified"`, which the `rxtxapp` adapter skips. Amended by D-188: `api` key | implementation-plan.md §4.1 |
| D-111 | The null backend (`null:<n>`) is a binding; nothing changes in `lib/src/dev/`; null loopback delivers a TX unit to every RX session whose flow (destination IP and port) matches | core.md |
| D-112 | Only the cut list of D-87 goes; per-packet conversion, VSYNC (`MTL_EVENT_EPOCH_TICK`), two RX threads, the ST 2110-30 knobs, pcap capture, converter plugins and the conversion calls of `mtl_format.h` stay | coverage.md |
| D-113 | An external review with at least three named consumers (FFmpeg and GStreamer plugin owners, the MXL team, the external engine team) and the private-user questionnaire gates the header freeze (MS7) | implementation-plan.md |
| D-114 | Unified wire defaults differing from legacy: frame-epoch RTP (D-10), incomplete RX frames delivered (`rx.incomplete` = `MTL_RX_DELIVER`), ST 2110-22 CBR (D-32), verbatim packets (D-115); legacy keeps today's (D-24) | contract.md |
| D-115 | Packet defaults: verbatim (only `packet.set_fields` written); RX chunks formed at dequeue, no reordering, legs deduplicated by RTP timestamp and sequence number; `MTL_RTP` is RTP only; RX copies unless `MTL_PKT_RX_LEND`; ST 2022-6 headers are the app's | contract.md |
| D-116 | Crash safety: a slot leased at shutdown is freed at the next control-plane call after its release; device removal is ERROR; SIGBUS hotplug only with `instance.hotplug`; `instance.cpu_shared` WARN; shutdown finishes the unit on the wire | deployment.md |
| D-117 | Pods: the IGMP window after SIGKILL (≈ 260 s) is documented; a node-disciplined PHC is trusted through `time.phc_trust`; set-up A (non-root, node prepared) is recommended, B the fallback | deployment.md |
| D-118 | One 64-bit atomic word per slot (continued below the table). Amended by D-209 and D-212: the layout is `{state:4 \| X:1 \| C:1 \| W:1 \| P:1 \| xs:2 \| holds:8 \| spare:14 \| gen:32}`, W the done-out-of-order bit (D-209), P the pin bit (D-212), `holds` the repeat uses in flight | core.md |
| D-119 | The order ring holds 64-byte submission descriptors `{seq:48 \| slot:16, gen, media time, launch, flags, used, cookie, …}` written by submit in one line; the completer writes the result in place, the reaper returns them in `seq` order (continued below the table). Amended by D-208: size a power of two at least D-208's bound | core.md |
| D-120 | The result reservation is one counter `resv`, CAS-incremented while `resv − reap_seq < N`; releasing an APP lease decrements it | core.md |
| D-121 | Stop against submit: after its QUEUED store, submit re-loads the session state (seq_cst) and, if the session no longer accepts, applies the flush CAS itself, so exactly one of stop and submit wins; stop always drains the in-flight counter | core.md §5.3 |
| D-122 | Superseded by D-158, D-159 (no wait handle: probes arm nothing, event loops arm a queue explicitly). Was: one wait handle per object (an eventfd; a Windows event from MS2a), made by the first `mtl_get_wait_handle`, whose mask it fixes; for event loops only, WT calls sleep on the wake word (D-141); a miss arms its target only when it is in the handle's mask | core.md |
| D-123 | TX recovery (MS1, E1a) records a verdict (DROPPED, `RECOVERY`) in the slot and drops only software-held references; the last PMD reference (the free callback) publishes; two `sh_info` per engine frame alternate per use, a use generation in `fcb_opaque`, so a stale free never completes the next use; attached memory completes after the queue stop/start (S8, MS3) | engine.md |
| D-124 | The core sees an instance-context interface (socket-aware alloc, TSC or test clock, wake flush hook, scheduler ID), never `struct mtl_main_impl*`; a null-only instance skips `mtl_init`, starts EAL itself (`--no-huge --no-pci --in-memory`) or uses a running one, and loops on a library thread; under the test clock completions run inside `MTL_FAULT_CLOCK_ADVANCE` (G-92) | core.md |
| D-125 | `st_engine_core.h` lists every engine entry point the bindings use besides the callbacks: a tasklet-callable RX frame put, the packet bitmap (MS2), "last packet handed" (MS2), the recovery verdict (MS1), per-leg arrival time, an RTP sequence seed (OI-62), an engine frame count ≥ pool_count + slot_max for RX_LATEST and BY_INDEX (E11); `st41_rx_set_unit_sink` (MS4a1, D-174) | engine.md |
| D-126 | RX uses `query_ext_frame` with core-allocated slots from MS1, one RX path; delivery order is a per-session `pub_seq` fetch-added by the completer; TX library pools use the engine framebuffers in MS1 (OI-59) | core.md |
| D-127 | One tick per session (MS2): TX in `tvs_tasklet_handler` (the builder; the transmitter calls nothing), RX in `rvs_pkt_rx_tasklet_handler` | core.md |
| D-128 | Stats blocks are per scheduler: a completion on another scheduler (a shared TX queue) writes that scheduler's block, never a block another tasklet writes; a session allocates its home scheduler's, the application's and the library's blocks at create, and a foreign scheduler's on the control plane when a shared TX queue gains a session of that scheduler, never on a tasklet | engine.md |
| D-129 | Tasklets log already formatted, length-capped lines into a per-scheduler ring that drops when full; a library thread hands them on; no per-site codes; one log thread calls the sinks (D-153); the threshold, a per-site token bucket (10 lines a second) and a reserved ring entry are checked before the line is formatted in place, so a dropped line costs no formatting | engine.md |
| D-130 | Superseded by D-218 (MTL adds the lead). Was: The pick-up lead is max(RL warm-up lead, one bulk build time + S0's p99.99 scheduler iteration, from P0's histogram) plus any conversion stage, about 0.5 ms with RL and 20 µs with TSC, not the ring depth (1.9 ms, kept as information); a capture producer that sets `min_tx_delay_ns` = one frame period + the pick-up lead gets L = 1 | timing.md |
| D-131 | The TX binding decides the launch at `get_next_frame` (video MS1) via `st_core_admit` (D-173), exactly (AUTO next feasible, late ON_TIME + `MTL_TXR_DEFERRED`; TAI nearest, SNAP_COLLISION, BEHIND, TOO_LATE DROPPED; EXACT checked, then `EXACT_USER_PACING`) (continued below the table). Amended by D-218: TAI nearest, moved by the session's launch delay L | timing.md, core.md |
| D-132 | In MS1 AUTO is `CLOCK_TAI` if valid, else SYSTEM_TAI (vDSO); a PHC, one syscall per read, comes in MS6 with the published time base (E9) | timing.md |
| D-133 | ST 2022-7: one RTP identity per session (`sc.ssrc`, `sc.payload_type`; 0 = one random SSRC per session, the essence's default PT); RX relocks only when the stale value lags the newest by more than `rx.skew_budget_ns` worth of ticks or every enabled leg is stale; ceil(skew_budget / TFRAME) + 1 out-of-order slots, else `info.tolerated_skew_ns` and a warning | timing.md, contract.md |
| D-134 | DSCP lands with the bindings (`mtl_flow.dscp` into the TX builders' TOS; video MS1, the rest MS4); RTCP sender reports in MS5; FREERUN with E9 in MS6 (those names leave `MTL_LATER` then); any video frame rate in MS2a (C-FPS), fastmeta MS4a1, ANC MS4a2, cvideo MS4b; a rational outside the legacy table is `-MTL_ENOTSUP` until then | implementation-plan.md |
| D-135 | The implementing session commits per task after its gates pass (signed off through the mtl-commit skill, no AI attribution) on a work branch; it never pushes and never opens PRs; the maintainer reviews and pushes | implementation-plan.md |
| D-136 | Tooling task P0 opens MS1: `run_gtest` gains `pacing_way`, `kernel:lo`, whitelisted args and `binary`; `mtl-developer` may edit the new trees of plan §4.1 and §4.2, the new pytest application, `.github/`, `build.sh`, `meson_options.txt`, `doc/unified-api/`; the frozen trees are BLOCKER paths for `mtl-reviewer` | implementation-plan.md |
| D-137 | Counts (functions per header, frozen names, functions per milestone tag) come only from `check.sh`; a function's milestone is its header tag, a value's its row in the data tables (D-185), a feature's its coverage.md U-row; other documents link them | README.md |
| D-138 | Samples on the new API only: each legacy sample in `app/sample/` is deleted in the commit that adds its replacement (those of cut features in MS1); the new samples together call every exported function, checked in CI (G-114) | implementation-plan.md §4.2 |
| D-139 | Option keys are stable or provisional (`mtl_option_desc.flags`, `P` in the header): stable keys freeze at `MTL_1.0` and may only become discoverable no-ops (`MTL_OPTION_DEPRECATED`); provisional ones (engine, DPDK and legacy knobs, 66 at the start) may change or be withdrawn (`OPTION_WITHDRAWN`); numbers and names are never reused; no tier prefix (OI-65) | contract.md §12.6 |
| D-140 | One index rule: an index that may be absent (field, argument or output) holds `MTL_INDEX(i)` = i + 1, 0 = default, all or none; an index that is always present is 0-based; an option value is literal (OI-70) | contract.md R3 |
| D-141 | Superseded by D-158, D-161, D-168 (one event word with counts the waiter removes replaces `armed`, `wseq` and `h_pend`; no wake enters the in-flight counter). Was (OI-63): a WT call counts itself in its lane of `armed` and sleeps on the futex word `wseq`, never on the wait handle; the handle is level, reset by misses; every wake runs inside the in-flight counter | wait-protocol.md §2–§6 |
| D-142 | Wake cost (OI-66): a flush wakes at most `wake_k` marked objects or queues needing a syscall per iteration and at most 32 without one; the rest stay marked, round robin (`sched.wake_carried`). `wake_k` is 1 under software-paced video TX, else `ST_WAKE_K_FREE` (1 in MS1, set by SR6, ≤ 32). (continued below the table) | wait-protocol.md §4, engine.md §6.1 |
| D-143 | Sender types are granted, never misreported (from MS2a, C-GRANT): N only on the formats of ST 2110-21 §6.3.1; elsewhere W on today's N emission until E5a (MS5), then NL; an explicit NL is N on those formats and refused elsewhere until MS5; MS1 sends every type as legacy does and flags NL, off-format N and interlaced or PsF N non-compliant | contract.md §3.6 |
| D-144 | A wrapper of a legacy instance inherits the legacy `ptp_get_time_fn`: the engine, core and bindings call it on tasklets, and from MS2a the wrapper's time thread, while the bridge exists; the bridge is not in `MTL_1.0` and leaves with `mtl_init` (D-190). It is the one exception to "no application code on the pinned cores"; a unified instance has none | contract.md §1; timing.md §2.2 |
| D-145 | An ANC unit is one slot: plane 0 the packet table (`struct mtl_anc_packet`, 16 B), plane 1 the words, RAW plane 2 the header words; `used` counts entries; `max_packets`, `max_udw_words`, `word_mode` are fields; a private wire area per session for `pool_count` slots, made at create; session allocations TX 9 + L + 1 (attached 8 + L + 1), RX 9 + 3L (8 + 3L), none later | contract.md §5.7 |
| D-146 | Submit claims the lease (APP → XFORM) before its first write, checks the unit in place, offsets before words, and encodes it into the wire area: refused with nothing queued, or sent whole; outside RAW MTL writes every derived RFC 8331 field; dequeue decodes, skipping and counting what the session cannot carry; RAW is verbatim; every RX entry is a valid TX entry | contract.md §5.7 |
| D-147 | ANC RTP packets: table order, ≤ 255 ANC packets and `max_udp_payload` − 20 bytes each, a break at `MTL_ANCF_NEW_RTP` (RX sets it on each received packet's first entry) and at a PsF segment; the tasklet builds headers and copies payloads, ≤ 16 per call; the shared-queue transmitter drains ≤ max(`max_idx`, 32) per call; options 704 and 705 retired | contract.md §5.7 |
| D-148 | ANC launch from the line alone (timing.md §9.2): suffix-min target, prefix-max early bound and the link term, every bound capped by the session reserve Res = Δ + F_min + TD − ε − C_cap; create requires 2·C_cap ≤ TFRAME − 3·TLINE − ε; exact lines in raster order unless `MTL_ANCF_AS_IS`; PsF F bits by segment; the hint's deadline is the capacity's earliest first launch | timing.md §9.2 |
| D-149 | The legacy ANC wire stays: legacy sessions and st40p keep the legacy codec, checks, timing and RX parse; only bugfixes SF-87 and SF-88 change them; st40p is re-based on the core in MS4a2. ANC reaches its engines through one create-time setter per direction (`st40_tx_set_unit_source`, `st40_rx_set_unit_sink`): `st40_frame` is frozen ABI until F+2 (D-178) | migration.md §6.3, engine.md |
| D-150 | Superseded by D-228 (release by lease alone; copy what dequeue flags). Was: Frameworks get RX zero copy by wrapping library slots (`av_buffer_create`, `gst_buffer_new_wrapped_full`) and releasing from the free callback, with session and lease by value; they copy instead when fewer than 2 slots would stay free (continued below the table) | contract.md §9.8, frameworks.md §9 |
| D-151 | Video and cvideo RX library slots: planes packed (FFmpeg `av_image_fill_arrays` align 1), one 64 B-aligned allocation per slot of `unit_bytes` + 64 rounded to 64, a 64 B tail that dequeue zeroes; V210 rows of 128 B per 48 pixels | contract.md §9.1, §5.3 |
| D-152 | `mtl_rx_provide` (MS2b): oldest destination first; delivery in reception order; `mtl_attach.cookie` the identity; none held = missed; every stop, discard, detach and close is one hand-back (tag, sweep, publish `provide_gen`); provide returns its 31-bit generation, so the application's ledger is exact | contract.md §9.12 |
| D-153 | Log sinks: `mtl_log_add_sink` with a level and filter per sink, records with origin, name and severity, one log thread, removal by `mtl_close` on `MTL_OBJ_LOG_SINK`; one process-wide threshold, `mtl_instance_params.log_level` (mismatch rule) or the bridged legacy level, the lowest sink minimum with sinks; no `log.level` option, no environment variable | contract.md §10.3 |
| D-154 | Text helpers are exported: `mtl_option_parse(name, value, &opt)` ("/N" scope as `MTL_INDEX(N)`; returns the object kind; `enum_names` as `name=value`) and `mtl_port_parse` (the `MTL_PORTS` parser open uses, names normalised); `caps.pacing_req` splits the requirement from the class | contract.md §2.2, §12.5, §12.7 |
| D-155 | Detection publishes every format within the maximum (width and height size the pool, `raster.fps` the quota, `{0,0}` = 120, any scan), with a first-unit flag and `mtl_rx_detail.format_seq`; the rate from timing.md §11.9; re-detection on two violating complete units or the rate rules; above the maximum no units, with `format_reason` | contract.md §3.5 |
| D-156 | Superseded by D-225 (the caller's work moves into `latency_min_ns`). Was: Latency: RX min = the link offset, else S + U + F + W; RX max = (pool_count − 1) × U; TX min = max(0, `min_submit_lead_ns`); TX max = the horizon; caller work in `convert_ns`; `MTL_INFO_LATENCY_INFEASIBLE` with remedy `ceil(min / U) + 1` (continued below the table) | contract.md §3.9 |
| D-157 | ANC skip counts: the unit record and the decode info carry one total of the corrupt ANC packets skipped (`mtl_rx_detail.anc_skipped`, `mtl_anc_decode_info.skipped`); the per-cause detail is the stats key `anc.pkts_skipped{cause}`, so no cause enum is frozen in the headers | contract.md §5.7, §11.3; migration.md §2.4 |
| D-158 | Waits: an event word per session and instance on never-freed line A, `intr` on the in-flight line; a change RMWs it after its publish; a call with a timeout loads it, attempts, arms by CAS of a lane count, then sleeps; no producer clears an arming; probes arm nothing; wakes take no in-flight count. Why: one location orders both; producer-cleared bits lose wake-ups | wait-protocol.md §3 |
| D-159 | Queues (MS2a) replace the wait handle, wait groups and the `MTL_LATER` queue block: `mtl_queue_create`, `mtl_queue_arm` (DP), `mtl_queue_wait` (WT, its own export), close and interrupt by the verbs; an eventfd (Windows: a manual-reset event); posts (D-206); MS1 has no descriptor. Why: one descriptor for many objects, O(ready), one write per burst (wait-protocol.md §7) | contract.md §7.2 |
| D-160 | One-shot reports with a per-arm mask: a report disarms; an arm that finds a target ready reports at once; an arm racing a report in flight returns 1 and gives it the new `user`. Why: no level spin, no drain rule, one report per arm (thread pools) | contract.md §7.2 |
| D-161 | A queue is an entry of its own never-freed table (wait-protocol.md §2) with an intrusive list through the objects' slots and an eventfd never closed while the process runs (a forked child closes it); a push and each call check its generation; close sets CLOSED, wakes and waits out its calls. Why: no use-after-close, no capacity, no write to a reused descriptor | wait-protocol.md §7 |
| D-162 | The consumer resets its descriptor only on the empty path, without the lock, and arms by CAS on an empty list and chain; a signal its reset took is written back on every exit but its own `-MTL_EAGAIN`; queue interrupts are the one seq_cst pair; no syscall runs under the queue lock. Why: any thread may make one call and leave; no thread spins behind a syscall | wait-protocol.md §7 |
| D-163 | Row waits (MS2) gate on the slot's `progress.want` (RW1–RW2): one wake per wait whatever `rx.rows_step` is | wait-protocol.md §3 |
| D-164 | Conditional (MS4a1, OI-80): a separate application-side producer word for TX ACQUIRE, only if W0 TX with results misses its budget. Why: it keeps line A off application threads | wait-protocol.md §3 |
| D-165 | Contingency, in place of W3: if S1 shows the flush's writes hurting pacing after D-170's patterns and the slack gate, one notifier thread per instance flushes; only contract.md §7.5's latency changes. Why: the one remedy for sleeping consumers under staggered dense load; its price (a thread, one point of failure, ≤ 250–600 k wakes/s) keeps it out | wait-protocol.md §4, engine.md §6.1 |
| D-166 | Conditional (MS4a1, OI-80): packed claim bits per queue, if aligned armed completions miss their budget; the contract is unchanged. Why: it amortises the line transfer in aligned bursts only | wait-protocol.md §7 |
| D-167 | A target's sleeper count is 7 bits: beyond it the others sleep uncounted with a 1 ms re-check (contract.md §7.1 states the limit). Why: four lanes and the attachment bits fit one word; no busy loop | wait-protocol.md §2 |
| D-168 | Retirement never runs inside a data call: the call that makes an object retirable (the last release or hold drop) stores RETIRED and posts the rest to the control plane (a library worker, or the orphan drain of the next control-plane call, D-186). Why: retirement waits for the data calls, which would include its own caller | core.md §7.2 |
| D-169 | A closing object is never reported. A detach and `o`'s close return only after every `mtl_queue_wait` handing out a report of `o` has returned (a two-phase delivery count), so `user` may be freed then. State changes are reported once (`MTL_READY_STATE`); arming works in every state but CLOSING and RETIRED. Why: `user`'s lifetime is exact across threads | contract.md §7.2 |
| D-170 | A `MTL_QUEUE_POLLED` queue (no descriptor, D-206) never arms and never costs a write; dense consumers poll it, or the sessions (W0), on their own timer; MS4a1 measures the three dense shapes (OI-80). Why: the write rate of staggered dense loads | contract.md §7.2 |
| D-171 | The core's one interface below it is `struct st_core_binding_ops` (13 ops) plus 14 core calls a binding makes; the core never reads `essence`; the shell picks the binding from one table (binding set, essence, direction, unit); `TestBinding`, `bind_null` and every binding implement it; it is checkpoint 1's acceptance text | core.md §2, §2.1 |
| D-172 | The shell validates options and stores the effective values, never an engine field; each option row names its owner (SHELL, CORE, BINDING); bindings alone build engine `ops`; a present key is never overwritten by a derived value (`OPTION_RANGE`); a binding key changed after create goes through the optional `set_option` op, else `-MTL_ENOTSUP` | core.md §2.6 |
| D-173 | One launch decision for every TX essence: the pure core function `st_core_admit`, called by `st_core_tx_pick` at pick-up (continued below the table). Amended by D-234: C2b owns it in MS1, with the hysteresis (D-234) and the launch steps (D-218) | core.md §3.2, timing.md §6.2 |
| D-174 | Two RX shapes: slot-RX for every frame and rows unit of every essence, ring-RX for packet units only; fastmeta frame RX is slot-RX through `st41_rx_set_unit_sink` (E16, MS4a1), as ANC's; `RX_BY_INDEX`, `RX_LATEST`, `RX_NO_FILL` are `-MTL_EINVAL` on packet units | core.md §2.2, contract.md §3.10 |
| D-175 | `mtl_option_desc` carries `essence_mask`, `dir_mask` and `unit_mask`; a key that does not apply to a session (or an instance or port key on one) is `-MTL_EINVAL`, `NOT_APPLICABLE` (234), the reason `st_avail_check` also gives a flag or value outside its essence or direction; an instance default applies only to the sessions it fits | contract.md §12.1, §12.5 |
| D-176 | The slot word's two spare bits next to X are `xs`, the transform stage (0 until Phase 7): a chain of at most four stages, TX convert → encode → encrypt, each a claim and done on one slot; the slot word is internal, not ABI | core.md §3.1 |
| D-177 | The compatibility log lines the acceptance engine parses come from one file of the API shell, `compat_log.c`, as deltas of the core's cumulative counters at the stat dump; the core and the bindings keep no legacy counter; the file leaves at F+2 | core.md §1 |
| D-178 | After F+2 the former session API is the internal engine interface, free to change; until then no legacy struct layout changes; the `st*p_*` wrappers, the bridge and `compat_log.c` leave with the legacy tier | core.md §1.1, abi.md §6.3 |
| D-179 | A port prefix selects one binding set (engine bindings or an engine-less set such as `null`); a session's legs are in one set; a new essence, backend or transport feature enters without a core edit, by the recipes of the core's extension section | core.md §2.5, §2.4 |
| D-180 | `st_core.h` is the register of the core's shared state: each field carries `W: R: ord: reset: test:` and sits on the 64-byte line of its writer class; `check_register.sh` fails a field without it; until C0 the register is core.md §4.8 (the wait words: wait-protocol.md §2). Why: about 35 lock-free words were described in eight sections | core.md §4.8 |
| D-181 | The wait state is observable: `wait.*` per object and `wq.*` per queue, `queue.gauge{state}` (MS2a), by plain loads; USDT probes at E3, K1, K2, T8, T3/T5, F4, P2, A7, A8, A10, J8; `MTL_FAULT_DUMP_STATE` (debug builds). Why: a lost wake-up, a sticky interrupt and a forgotten re-arm look alike from outside | engine.md §1.8 |
| D-182 | Handle-table chunks are per NUMA node: an entry comes from a chunk of its scheduler's node (an instance and a queue: the instance's main lcore's node), allocated with `mmap` + `mbind` outside EAL; the chunk table and the handle limit stay process-wide. Why: tasklets RMW line A on every completion | core.md §4.7 |
| D-183 | The reaper lock is a TTAS spinlock held for the cursor move, the RX slot CAS and the copy of at most `max` results, never across conversion, zero fill or a syscall; an empty poll evaluates the predicate with loads and takes no lock. Why: empty polls dominate W0 loops; a mutex could sleep in a DP call. Amended by D-242: the fill, black for video, not a zero fill | core.md §4.4 |
| D-184 | One dense-audio reference load: per scheduler 512 TX + 512 RX audio sessions at 1 ms units (≈ 1 M units/s); its budgets are implementation-plan.md §8.4's (unarmed and armed completion, flush share, dequeue latency) | implementation-plan.md §8.4 |
| D-185 | One source per table: reasons, option keys, value availability, codes per call, session states and calls, stats keys are `.def` files in the library tree (README §4), made by the task that first needs them; `gen_api_doc.py` writes the enum blocks and marked tables, `check.sh` fails on a difference; unmarked tables are prose; a value works once its row names its task | README.md §1, §4 |
| D-186 | The session machine: a fault in DRAINING or FLUSHING enters ERROR, and a waiting stop completes ERROR → STOPPED with `-MTL_EIO`; *closed by the instance* is a condition and a column of the call table, not a state; an interrupt on a retired handle returns 0; memory freed after the instance's close goes to an orphan list the next control-plane call frees | contract.md §4 |
| D-187 | One home per load-bearing number: the header for a public value, the register for an internal layout (until C0, decisions.md or core.md as `numbers.txt` names), the D-row for a decided bound or a process rule; other documents state the rule in words and link the home; `sketch/numbers.txt` lists the numbers and `check.sh` fails on a restatement | README.md §1 |
| D-188 | Validation stacks: harness bugfixes are allowed in the frozen trees under the approval of a legacy bugfix; unified-only JSON keys need `"api": "unified"`; at F+2 one harness and one name (task F2-1) (amends D-109, D-110) | implementation-plan.md §4.1 |
| D-189 | The legacy-behaviour ledger (migration.md §6.5) lists every legacy-visible behaviour of a pipeline before its re-base; a behaviour the core keeps is an `ST_CORE_LEGACY_*` mode citing its row; the ledger is the deletion list at F+2 | migration.md §6.5 |
| D-190 | The bridge is legacy tier: `mtl_instance_from_legacy` keeps its MS1 tag in its own node, `MTL_UNIFIED_EXPERIMENTAL_<rev>_BRIDGE`, then `MTL_LEGACY_BRIDGE` from F, never in `MTL_1.x`; `mtl_legacy.h` moves to `mtl/legacy/` at F; both leave with `mtl_init`, ending D-144's exception; `check.sh` counts neither as frozen | migration.md §6.2, abi.md §2 |
| D-191 | Sealed version nodes: a node a release carries never changes in its revision; a milestone open at a release continues in a part `_MSn.k`; a revision change merges the parts; the checks compare HEAD with every tag and refuse an open node in a `.REL` tree, so a released libmtl fails a binary only at load; `MTL_LEVEL` stays per milestone; no `-z now` | abi.md §3 |
| D-192 | A wrapper of a legacy instance is its process's shared instance (MS2a): another wrap returns another reference and a shared open joins it; a joining open names a subset of the live ports, which keep their indices; a legacy `mtl_init` after a unified instance fails, so the legacy instance opens first | contract.md §2.3, §2.8 |
| D-193 | `mtl_session_update` is exported from MS3 for CREATED and STOPPED, every part through the binding's re-create of its engine session (handle, name, counters, SSRC kept); ARMED and RUNNING are `-MTL_ENOTSUP` until MS5, which adds the boundary swap (D-93, D-103 stand). Amended by D-236: ARMED and RUNNING: FLOWS from MS3 (D-236); FLOWS in place in every state | contract.md §4.7 |
| D-194 | At F the unified headers move to `mtl/`; `mtl/experimental/<h>` stays as a forwarding stub until F+2, with a `#warning` from F+1; include guards end in `_` | abi.md §2, §6.3 |
| D-195 | A function that walks lengths, counts or offsets read from network data (RTP payloads, RTCP, SDP) is never `static inline`: one exported, fuzzed implementation, fixed by replacing the library; the RFC 8331 decode and encode (AS, MS4, `mtl_packet.h`) and `mtl_rtcp_mib_next` (Phase 7) are exported; fixed-offset accessors stay inline | sketch/README.md |
| D-196 | SDP render and parse move to `mtl_sdp.h`, the header of the companion library `libmtl_sdp` (`ecosystem/sdp/`, its own soname and version node), built on libmtl's public calls only and outside libmtl's frozen set; scheduled with NX-4 (Phase 7 unless the maintainer pulls NX-4 forward). D-94's substance stands | nmos-ipmx.md §12 |
| D-197 | Legacy enum values are converted by eight inline functions of `mtl_legacy.h` (`mtl_<field>_from_legacy`), which leave with that header (D-190); the enum values themselves do not change (D-97 stands) | migration.md §5 |
| D-198 | Two legs on one instance port are accepted (D-140 stands) and reported: `MTL_INFO_LEGS_SHARE_PORT` and one WARNING line; `mtl_flow_on_port()` writes `MTL_INDEX(port)` | contract.md §14.1 |
| D-199 | Superseded by D-227 (`mtl_rx_reserve` removed). Was: The RX copy-fallback reserve of D-150 is one inline helper, `mtl_rx_reserve()` (`mtl_util.h`); the pool size is R + H, documented, no second helper | frameworks.md §9.3 |
| D-200 | Performance budgets are deltas against legacy measured the same night on one host at the S0 loads (st20p at quota, 1080p to 4320p, one and two legs, RL and TSC): visit cost, iteration p99.9 and p99.99 (P0's histogram), lost frames, a monitor's compliance class, capacity (MS2a); medians of 3 runs, an A/A rule, one rerun; the values are §8.4's | implementation-plan.md §8.4 |
| D-201 | ANC capacity defaults: `max_packets` 0 = 32, `max_udw_words` 0 = max(255, 32 × `max_packets`) ≤ 255 × `max_packets`; a unit's entries use at most `max_udw_words` words (a shared word counted per entry); the wire area is 16 × (P + 2) + min(328 × P, 13 × P + ⌈5W/4⌉) bytes per slot, so a default session takes about 15–20 KB, not 0.6–0.9 MB | contract.md §3.3, §5.7, §9.4 |
| D-202 | `internal_bytes` is every hugepage byte a session allocates besides its pool (internal frames, mempools with their caches, the slot table, stats blocks, ANC wire areas): the query computes it, `get_info` sums it; `mem.mempool_bytes{numa}`; `MTL_SESSION_TX_SRC_PLANES` (MS2) lets a converting TX pool skip its app-format planes | contract.md §9.4, deployment.md §5 |
| D-203 | `mtl_rx_dequeue` claims the unit under the reaper lock (the cursor and PUBLISHED → APP) and converts, decodes, copies or zero-fills it outside the lock, DPC whenever it works; the fill runs only there, from a bitmap copied only for incomplete fill-enabled units (continued below the table). Amended by D-242 and D-244: it fills; its cost is in `rx.bytes_filled` | contract.md §5.3, §9.8 |
| D-204 | The default RX video pool is n + 2, n the units in reception at once (1 with one leg, min(the engine's slots, ceil(F / U) + 1) with two): 3 with one leg, 4 with two, so a loss on the leading leg never costs a unit to a full pool | contract.md §3.2, engine.md §2.14 |
| D-205 | Time on the data path: one TSC read per RX burst, converted to TAI once per unit; due times in TSC against the loop's one sample per iteration; open checks the clocksource and reports clock syscalls on tasklets as bit 2 of `*.backend_syscalls_on_tasklet`, warns, never refuses, and builds no TSC extrapolation before E9 | timing.md §11.8, engine.md §2.12, core.md §3.3 |
| D-206 | Posts (MS2a): `mtl_queue_post(q, bits)` (AS) ORs bits within `MTL_QUEUE_POST_MASK` into the queue word by the push's CAS and checks; a wait reports them first, `{MTL_OBJ_NONE, user = the OR}`; they coalesce and are dropped at close. `native` NULL keeps the descriptor in MTL; `MTL_QUEUE_POLLED` is D-170's queue. (continued below the table) | contract.md §7.2, wait-protocol.md §7 |
| D-207 | Superseded by D-208 and D-209 (units complete out of order). Was: The results ring reserves `pool_count` + 1 entries: after a reap of every ready result at most `pool_count` units are leased, in flight or unread, so one acquire never blocks on RESULTS (continued below the table) | contract.md §5.5, core.md §4.4 |
| D-208 | The results ring admits 2 × `pool_count` + 1 entries (a power of two at least that). (continued below the table) | contract.md §5.5; core.md §4.4 |
| D-209 | The hold. A TX completer stores PUBLISHED\|W (W: the slot is done while an older entry is not) exactly when the entry at `seq` − 1 is not done: its slot word carries that entry's generation and is not PUBLISHED, or is HELD, or is PUBLISHED with W (core.md §4.2 step 3). (continued below the table) | contract.md §6.2; core.md §3.1, §4.2, §4.4 |
| D-210 | EOS is not a stop. A framework sink keeps the session RUNNING, reaps until `unreported` ≤ its pins, and lets the base class post EOS; a reap timeout ends the wait. `FLUSH_STOP` is `mtl_session_discard`. Stop belongs to PAUSED → READY and close to READY → NULL; FFmpeg's `write_trailer` reaps and closes, with no stop. (continued below the table) | frameworks.md §4; ex21 |
| D-211 | The ports of a shared instance are a plugin recipe, not a library rule. GStreamer's `gst_mtl_common` registers each element's ports at NULL → READY, and the first READY → PAUSED opens the union; later joins follow contract.md §2.3. FFmpeg lists every port in `MTL_PORTS`. (continued below the table) | frameworks.md §11; contract.md §2.3 |
| D-212 | The pin rule (`MTL_UNDERRUN_REPEAT`, RUNNING only). A frame unit finishes at its pick-up, sent or dropped; a rows unit at its last row or when `tx.rows_late` ends it. After each finish the pins are the whole units among the last `defer_step` finished units, one per parity. (continued below the table) | core.md §3.1; contract.md §6.1; timing.md §6.4 |
| D-213 | Bounded completion: every unit handed to the engine completes without a later unit of its session. On dedicated queues by E1b's idle cleanup. (continued below the table) | core.md §4.4; engine.md §2.3 |
| D-214 | MEM3 is not a reap gate. An application-memory session always produces results, and a unit's result says that its memory is no longer read. An attached slot is acquirable when PUBLISHED, under D-209's hold, like every TX slot. (continued below the table) | contract.md §6.1; `mtl_mem.h` MEM3 |
| D-215 | One "no value" for outputs: an `int64_t` time, duration or index with none reads `MTL_TIME_NONE` (INT64_MIN, FFmpeg's `AV_NOPTS_VALUE`), a count or an enum 0. The 17 validity flags are retired and their bits stay holes, never reused; the remaining flags say only how a value was obtained. (continued below the table) | `mtl.h` R1; contract.md R1, §6.4, §6.5; timing.md §2.1 |
| D-216 | No value on input. Wherever a call that returns a status reads a TAI time or a media index (submit's media field, a launch, a start or update instant, the user time pair), a value below 0 is `-MTL_EINVAL`, `FIELD_REQUIRED`, naming the field. (continued below the table) | `mtl.h` R5; contract.md R5, §5.1; timing.md §4.4 |
| D-217 | Growth after MTL_1.0: an appended output field that can have no value comes with a bit in its struct's flags word that only a library filling it sets, or else a version gate on `mtl_library_version_num()`. Why: R3's zero fill makes an unknown field read 0, and no `struct_size` reports what was filled. | `mtl.h` R3; contract.md R3 |
| D-218 | `min_tx_delay_ns` (d) is the producer's readiness: each unit's submit is called by M + d, M the unit's media time. 0 is playback, launched at M's own index. (continued below the table) | timing.md §5.2, §6.1 |
| D-219 | `suggested_min_tx_delay_ns` = the worst (hand-over − M) of the late units, M their media time, minus C (D-225) on a converting session; `shortfall_ns` = the worst hand-over − deadline. In frames, raising the launch delay L (D-218) by ceil(`shortfall_ns` / TFRAME), TFRAME the frame period, gives the same result. (continued below the table) | timing.md §5.2; contract.md §4.10 |
| D-220 | The lead's stage term: 0 for conversion in the caller (MS1, OI-60); MTL's worker conversion timed once at create; a plugin session's `latency_ns`, declared by `create_session` or else timed with one unit at create, with one WARNING. (continued below the table) | timing.md §6.1; contract.md §15.3 |
| D-221 | Live framework sinks: one media time `TAI(ct) + δ`, ct the buffer's presentation time, which already holds the programme latency L, and δ the programme's phase (OI-68); one L and one δ per programme; `media_time_offset_ns` 0. (continued below the table) | timing.md §10.5; frameworks.md §4 |
| D-222 | Format names: each name string names one (format, kind). The GStreamer and FFmpeg names sit on the value a framework should use: a compliant transport format before an application layout of the same bytes, never a `_NONSTD` value. Values of every kind are below 256. (continued below the table) | contract.md §15.1, §15.2 |
| D-223 | Every data-path function's comment states its concurrency, `mtl_tx_acquire_layout` included (contract.md §5.6 stays the table and gains its row). (continued below the table) | the headers; implementation-plan.md §8.4 |
| D-224 | No per-packet TX timestamps in results. A TX capture stamps the software hand-over time of each burst (on `MTL_PACING_HW_LAUNCH`, the commanded launch time), never a wire time; per-packet wire times are an RX capture's on a monitor. Why: 4320 packets per frame against a 216 B record; E810 latches one TX timestamp per port, owned by the PTP client, and a VF none. | contract.md §15.4 |
| D-225 | Supersedes D-156. RX min = D + C, with D the link offset, else S + U + F + W (contract.md §3.9; U the unit period); RX max = (`pool_count` − 1) × U; TX min = max(0, `min_submit_lead_ns` + C); TX max = the horizon. (continued below the table) | contract.md §3.9 |
| D-226 | Amends D-104 and D-07. `mtl_release(s, lease)` accepts a null `s`, since the lease names its session ({session:16, slot:16, gen:32}, core.md §4.7); a non-null `s` is checked as before. The null path takes the entry's in-flight counter, checks the entry's state, and touches slot memory only for a state that owns it. (continued below the table) | contract.md §5.4, §5.6; core.md §4.7 |
| D-227 | The RX copy decision of a lending source belongs to the library. The claim, under the reaper lock, sets `MTL_UNITF_COPY_ADVISED` when `lent` ≥ the copy budget B = max(0, `pool_count` − `R_copy`), with `R_copy` = ceil(`latency_min_ns` / U) + 1 and U the unit period. (continued below the table) | contract.md §5.3, §9.8; core.md §3.1 |
| D-228 | Frameworks get RX zero copy by wrapping library slots (`av_buffer_create`, `gst_buffer_new_wrapped_full`) and releasing from the free callback with the lease alone (D-226). (continued below the table) | contract.md §9.8; frameworks.md §9 |
| D-229 | `MTL_TIME_SOURCE_AUTO` takes the best valid source at open and, while running, moves up only when the switch is no step: SYSTEM_TAI → CLOCK_TAI when the kernel TAI offset k equals `U_utc` (MS3); → a PHC within E9's slew threshold (MS6). It posts `TIME_STATE`, never `TIME_STEP`. (continued below the table) | timing.md §2.7 |
| D-230 | No time state and no ESTIMATED or HOLDOVER flag changes what a session sends; times, results and `TIME_STATE` report it; `MTL_HEALTH_TIME_UNLOCKED` is readiness only; only a step changes sending. | contract.md §4.10 |
| D-231 | The step rule per unit state and pacing class (timing.md §12). With Δ the step and U the unit period, for units queued at the step: a forward step drops at most ⌈Δ/U⌉ + 1 per session, a backward one loses none (gap ≤ Δ + U). (continued below the table) | timing.md §12 |
| D-232 | `mtl_epoch_index_at` gives the first unit at or after its `tai_ns`, the same on the exact and the floored scale; `tai_ns` below 0 is a bare `-MTL_EINVAL`, with no reason. A media time MTL computes for a TX unit (a result, t0, `next_media_tai_ns`) gives its own unit. (continued below the table) | timing.md §7.2, §10.4; `mtl_sync.h` |
| D-233 | A single start uses the session's index grid; 90 kHz and audio rates join the programme's grid rate G only in arrays. `MTL_AT_INDEX k` starts at k, `MTL_AT_TAI t` at `mtl_epoch_index_at(t)`. | timing.md §7.1 |
| D-234 | NEAREST's hysteresis (D-38) is in `st_core_admit` from MS1 (task C2b); engine change E3 keeps the reporting. `tx.snap_tolerance_ns` ∈ [0, U/2], U the unit period, `OPTION_RANGE` above, so U/2 + the tolerance ≤ U and a sink's TFRAME covers the slot the hysteresis may take. δ stays the programme's phase (OI-68). Amends D-173. | timing.md §4.3 |
| D-235 | Bridge clock: MS1 logs one WARNING when the wrapper's clock reads UTC as TAI. (continued below the table) | timing.md §2.2 (the bridge clock); contract.md §2.8 |
| D-236 | `mtl_session_update` with FLOWS, and LEGS unchanged, applies to an ARMED or RUNNING session from MS3 with `when` NULL or `MTL_NOW`, leg by leg: an unchanged leg is untouched; a key-same leg changes only its template, or its source list and filter; a key-changed leg adds a rule beside the old one. (continued below the table) | contract.md §4.7; core.md §5 |
| D-237 | The launch delay (`min_tx_delay_ns`) changes while running from MS5 under `MTL_UPDATE_MEDIA`, from the first media index at or after `when`. Longer: the skipped slots follow the underrun policy. Shorter: `BEHIND`. MTL never changes it by itself. | contract.md §4.7; timing.md §5.2 |
| D-238 | No running format or TROFFSET change: stop, MEDIA and start, or a second session started at an instant while the first drains (ex18 `hand_over_*`); the first session's REPEAT pins drop when its DRAIN begins (D-212). Two TX sessions may carry the same flows (contract.md §4.3). | contract.md §4.3, §4.7; nmos-ipmx.md §11.5 |
| D-239 | `MTL_UNDERRUN_REPEAT` (= 5; video, and cvideo JPEG XS from MS4b), RUNNING only, `-MTL_ENOTSUP` until T-REP (MS4a1). (continued below the table) | timing.md §6.4 |
| D-240 | The engine for REPEAT: a use record per E1a `sh_info` (meta, address, IOVA, generation, busy); `busy` is atomic: the free callback's release store is its last access to the record, and the builder's acquire load precedes any write to it. The builder refuses only a busy use; the free callback completes its own use. (continued below the table) | engine.md §2.2 |
| D-241 | Default SKIP for video, cvideo and audio (KEEPALIVE for ANC and fastmeta, D-243; D-13 stands); REPEAT is opt-in (MS4a1). (continued below the table) | contract.md §3.2, §3.10, §9.6 |
| D-242 | Fill of lost data in library pools (MS2a): video, the format's black (sampling, depth, `video.range`, the NONSTD formats too; RGB FULL a `memset`); audio, cvideo and fastmeta: zero; ANC units are never filled; the tail zero. (continued below the table) | contract.md §9.8 |
| D-243 | One keep-alive value: `MTL_UNDERRUN_KEEPALIVE` is the essence's keep-alive at its standard's cadence and the default of ANC and fastmeta; `MTL_UNDERRUN_EMPTY_ANC` is removed and value 2 retired; SKIP is refused for ANC and fastmeta frame units (`OPTION_RANGE`); packet units keep SKIP. | timing.md §6.4 |
| D-244 | Stats: `tx.units_repeated` removed; `tx.units_padded` → `tx.indices_filled`, a subset of `tx.indices_empty`; `rx.units_zero_filled` → `rx.units_filled` and `rx.bytes_zero_filled` → `rx.bytes_filled`, for every essence. Amends D-203. | contract.md §11.3 |

Rows too long for the table, continued:

- **D-93**, continued: REAPPLY, DRY_RUN, cancel, mute and the REPLACED and CANCELLED states are
  Phase 7 under `MTL_LATER`
- **D-104**, continued: each exported in its milestone (D-106); every Phase 7 name under `MTL_LATER`
  until its milestone
- **D-118**, continued: `{state:4 | X:1 | C:1 | xs:2 | holds:8 | spare:16 | gen:32}` (`xs`: D-176):
  the CAS of acquire and dequeue writes a new generation, tasklet CASes keep it; release, submit,
  hold changes and tasklet CASes compare the whole word; a completion CASes to C (claimed), writes
  the result, release-stores PUBLISHED, then RMWs the event word
- **D-119**, continued: the binding picks up only the entry at its cursor whose slot word is QUEUED
  with its generation; size a power of two ≥ pool_count + 1 (D-207)
- **D-131**, continued: and drives the engine with `USER_PACING | RTP_TIMESTAMP_EPOCH`,
  `required_tai = N·T`; no `notify_frame_late`
- **D-142**, continued: If S1 fails: D-170's patterns, the slack gate, the notifier (D-165)
- **D-150**, continued: `mtl_rx_provide` serves only destinations chosen per unit
- **D-156**, continued: with two legs F is the configured `rx.skew_budget_ns`, not the measured
  `leg.observed_skew_ns`, so it stays stable
- **D-173**, continued: fed by the binding's `struct st_core_grid` (index rate, first-packet offset
  per parity, pick-up lead, EXACT window, defer step, index bytes); D-131 stands; C2 owns it in MS1
- **D-203**, continued: its cost is in `rx.bytes_zero_filled`, not `convert_ns`; a close may wait
  out a conversion
- **D-206**, continued: Why: a loop fed by threads needs no epoll
- **D-207**, continued: never reaping still meets `MTL_BLOCKED_RESULTS` (G-04). Not taken:
  `-MTL_EAGAIN` at once (spins when another thread reaps), or for the reaper only (needs its
  identity)
- **D-208**, continued: While each unit's first submit follows a reap of every ready result, no
  acquire on any thread waits on RESULTS: with P = `pool_count`, at most P entries stay after the
  reap and at most P more slots cycle before the next one, which D-209's hold makes exact. With S
  submitting threads (`MTL_SESSION_MT_SUBMIT`) the bound is 2P + S, so those use a finite acquire
  timeout. A session that never reaps still meets `MTL_BLOCKED_RESULTS` (G-04). Not taken: 2P with
  "one reap per P acquires" (another thread cannot count them); 3P with "one reap per P submits".
  Supersedes D-207's bound; amends D-119's size clause.
- **D-209**, continued: A W slot is acquirable only once every entry from `reap_seq` to its own is
  done (the reaper's done test, loads only). Every publish, W or not, runs E2 with RESULTS |
  ACQUIRE; a waiter it wakes early re-checks and sleeps again. Why: without the hold a slot cycles
  behind a unit in flight and D-207's reap-then-acquire hangs ex21 after `pool_count` late drops
  (`trace_d207.py`); a W store without E2 loses a wake-up (`w_noe2.py`); a W chosen by code path
  breaks the hold under interlaced REPEAT. Supersedes D-207 with D-208; amends D-03 and D-118.
- **D-210**, continued: Why: STOPPED holds submits, and under REPEAT the newest units report only at
  the next unit of their parity, a discard or a DRAIN.
- **D-211**, continued: Not taken: an instance that defers EAL until the first create. Runtime port
  open stays NG8.
- **D-212**, continued: A `BEHIND` drop and a unit cut short (TRUNCATE, PAD) release their parity's
  pin and take none, so older content is never repeated. For one index E the old pin repeats first;
  a unit for E that comes after it is a `TOO_LATE` drop, and a `TOO_LATE` or `SNAP_COLLISION` drop
  takes the pin. A discard and leaving RUNNING (DRAIN, FLUSH, ERROR, close, recovery) release every
  pin. The pin is the slot word's pin bit, taken and cleared by a CAS on the generation, so a second
  release finds the bit clear and does nothing; the repeat uses in flight are counted in `holds`;
  whoever leaves the pin bit clear and `holds` 0 publishes, with E2 (D-209). Why: each weaker rule
  deadlocks a ring that reaps in order (`pin_ring.py`); one counter released by two contexts frees a
  slot still on the wire. Amends D-118.
- **D-213**, continued: On shared queues by E1b's rewrite of `mt_tsq_done_cleanup`: it takes
  `tx_mutex` by trylock on the idle path and by lock on the queue-fatal recovery path, never
  `tsq_lock`; it frees at most `tx_rs_thresh` descriptors per visit; it is skipped on an AF_XDP
  queue. A failed trylock is retried at the next rate-limited idle visit, which gives liveness.
  AF_XDP and kernel sockets complete at the copy or the send. With the link down, the stalled-queue
  path completes (engine.md §4, MS3); until then a queue that stops completing holds every slot
  behind its oldest unit, and acquire returns `-MTL_EAGAIN` (BUFFERS) at its timeout. Amends DF-13.
  Why: the hold and the pins wait for older units, a TSQ's last frame never completes today (G-03,
  DRAIN), and MS1 offers `MTL_TXQ_SHARED` (U-075).
- **D-214**, continued: Why: acquire returning slot i already says surface i is free, and a reap
  gate would make an acquire on another thread wait for the reaper.
- **D-215**, continued: `rtp` is the unit's RTP, sent or not, 0 when `media_tai_ns` is NONE;
  `time_error_ns` keeps INT64_MAX as an unknown bound. The TX result fields follow from `status`
  (contract.md §6.4; margins and sent times from MS3). RX `media_index`: video from MS2b, the other
  essences from MS6. Reap and dequeue write NONE; tasklets keep internal bits. Why: one
  representation; a missed check never reads as index 0 or a margin of 0 ns; bindings map one
  constant; 16 fewer frozen names. Supersedes D-15; amends D-14.
- **D-216**, continued: This includes negative indices under `MTL_WHEN_ORIGIN`, so priming samples
  are dropped before submit. The exceptions: the helpers `mtl_epoch_index_at`, `mtl_time_convert`,
  `mtl_rx_align` and `mtl_grid_offset` return a bare `-MTL_EINVAL`, with no reason (`mtl_tx_write`
  passes on submit's reason), and the value-returning AS functions return 0 (`mtl_media_ticks`) or `MTL_TIME_NONE`
  (`mtl_media_tai`). `NOT_IMPLEMENTED` is checked before `FIELD_REQUIRED`. `START_IN_PAST` and
  `LAUNCH_IN_PAST` (`-MTL_ERANGE`) stay for real instants at or after 0. A duration is not an
  instant: `min_tx_delay_ns` or `preroll_ns` below 0 is `-MTL_EINVAL`, `INVALID_ARGUMENT`, and a
  signed duration refuses `MTL_TIME_NONE` the same way; option keys keep `OPTION_RANGE`, and NONE
  lies below every key's minimum; AUTO takes 0 or NONE as unset. A refused first submit returns the
  slot to the pool (D-88). Why: NONE + x escapes an equality test, and nothing below 0 is a time.
- **D-218**, continued: With d > 0, MTL adds its own pick-up lead and, on a session that converts in
  submit (`video.app_format` other than the transport format, or `MTL_SESSION_TX_SRC_PLANES`), the
  submit's work C (D-225; create's calibration at create). The launch delay L is the fewest index
  steps (fields when interlaced, always whole frames) whose pick-up deadline at the worse parity is
  at or after M + d (+ C), one L per session, derived at create, in the dry run and when an update
  changes the delay or the essence member; MTL never changes it otherwise. A negative value is
  `-MTL_EINVAL`, `INVALID_ARGUMENT`. One rule on every essence's grid. The lead is max(the RL
  warm-up lead, one bulk build time + S0's p99.99 scheduler iteration from P0's histogram), plus any
  asynchronous stage (D-220), not the ring depth. `submitted_tai_ns` is stamped at the hand-over,
  when submit returns. Why: the lead is MTL's number, readable only after create, so every capture
  or processor open was create → stat → `MTL_UPDATE_MEDIA` → start; a converting capture producer
  was late on every RL frame. Supersedes D-130 (its lead definition moves here); amends D-39, D-131
  and DF-24.
- **D-219**, continued: Both read `MTL_TIME_NONE` without `MTL_STATUS_TIMING_WARNING`.
- **D-220**, continued: From MS2b (converter plugins on the transform state) to MS4b (plugin ABI v2
  with the parameter) the timed unit is the only source; a library worker stage (crypto, Phase 7) is
  timed at create. A device shared by sessions declares its latency under full load; the timed unit
  measures it idle. Fixed at create, so the launch delay L (D-218) is final there. `mtl_plugin.h`
  keeps revision 0.2: the ABI is not frozen.
- **D-221**, continued: A sink's minimum `M_sink` = `latency_min_ns` + margin + TFRAME, TFRAME the
  frame period; one TFRAME always covers NEAREST's hysteresis (D-234). GStreamer lets basesink wait
  (`sync=TRUE`, render-delay `M_sink`, async on, its pipeline latency L); FFmpeg, OBS and
  applications let the pool wait (ex21); GstMtlClock lives in the plugin (MS6). Why: both waits give
  the same media time, and with `sync=FALSE` basesink reports no latency and loses preroll, QoS,
  `ts-offset` and PAUSED.
- **D-222**, continued: `mtl_convert` is exported in MS2 with `MTL_CONVERT_CHECK` only; its
  conversions come in MS4b. Caps lists come from `caps_formats` (ex19), which includes the transport
  format only on request, off for sources. The plugins keep a logged, deprecated alias table of
  today's names for one release cycle, and the MS3 ecosystem CI check asserts that each legacy name
  keeps its (transport, app) pair or that frameworks.md §2.1 and §2.2 list the change. The names
  stay in libmtl; GstMtlClock and the caps lists stay in the plugins; no glue library.
- **D-223**, continued: The nightly publishes the measured `UnifiedPerf.dp_calls` table with each
  release, for reporting only: the budgets of implementation-plan.md §8.4 stay the gate and their
  one home. No lean fast path (core.md §4.1's pointer form only if measured).
- **D-225**, continued: C is the caller's work per unit, held once in `latency_min_ns`: from
  create's calibration (the dry run times one unit of the conversion the configuration implies,
  never 0 for a converting configuration); from MS2a raised only when the 1 s window `convert_ns`
  exceeds C in two consecutive windows, then to that value rounded up to 250 µs; it never falls
  before the next start or format change. C is an atomic on the session's `.app` line, raised by
  CAS-max. Each rise of C is one LATENCY re-post. No formula adds `convert_ns`, which is information
  only. `MTL_INFO_LATENCY_INFEASIBLE` with the remedy ceil(min / U) + 1; F from the configured
  `rx.skew_budget_ns`. Why: every framework added the two fields and forgetting one made each
  converted unit late; a falling window churns LATENCY; a high-water mark without hysteresis
  ratchets on outliers.
- **D-226**, continued: A session's slot generations start at its entry's lease base. At retire (CP,
  after X2) the base becomes base + max over the entry's slots of ((gen − base) mod 2^32) + 2,
  skipping 0, so a stale lease or `u.hold` fails `-MTL_ESTALE` deterministically while no slot made
  2^32 leases, with no data-path write. Framework free callbacks carry the lease alone. Why: two RX
  recipes allocated a struct per frame only to carry `s`; a total lease count wraps `pool_count`
  times sooner and costs one RMW per acquire.
- **D-227**, continued: `lent` counts the session's other leased and held units exactly: RX only, on
  a cache line of its own, +1 by the claim's CAS winner and −1 by the winner of the CAS that stores
  FREE (an application thread or a TX completer), before its last-reference check. B is an atomic on
  `.app`, rewritten at create, start, an option the RX min depends on, a published format (MS3), and
  by the thread whose CAS raised C (CAS-min while running). The flag is set on frame units of
  library and attached pools only, never on rows or packet units, `RX_BY_INDEX` or provide sessions;
  it is advice. `rx.units_copy_advised` counts flagged units claimed while `lent` > 0. At D-204's
  default pool (3, one leg) B = 0 and every unit is flagged: a framework that wants zero copy sets
  `pool_count` = `R_copy` + H, H the units it holds. Not taken: a framework latency key and lag
  term; the library copying into caller planes (OI-84); B = `pool_count` − `R_copy` + 1. Supersedes
  D-199.
- **D-228**, continued: They copy the units dequeue flags (D-227), taking the copy's buffer without
  waiting (`GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT`); with none free they release and drop the unit
  (QoS) and never block `create()` on downstream. `mtl_rx_provide` serves only destinations chosen
  per unit. Supersedes D-150.
- **D-229**, continued: A disagreeing source is reported (`TIME_STATE` value[1], WARNING, state
  unchanged) and not taken. AUTO never moves down; named sources never move. `U_utc`, SYSTEM_TAI's
  TAI − UTC, is the option `time.utc_offset_s` (2212, class C, default 37); after a leap second on a
  running instance no source moves without a step. phc2sys's own first step of CLOCK_REALTIME is not
  removed. Only AUTO's FREERUN fallback and ts-refclk stay Phase 7. The poll runs once a second on
  its own admin-thread timer. If the MS3 move slips, it moves to MS4a1, in the slip order of
  implementation-plan.md §6.2. Supersedes D-85 ("chosen once at open"); amends D-95 and DF-15.
- **D-231**, continued: After it: re-syncing producers lose none after a forward step; a live TAI
  producer loses about ⌈Δ/U⌉ units as `BEHIND` during a backward one. A declared backward step
  extends the horizon (MS6). Only an update pending for an instant fails with `TIME_STEP`; an
  immediate one applies at the next boundary on the stepped clock.
- **D-232**, continued: An RX unit's `media_tai_ns` is its RTP instant and gives the next index for
  a sender whose RTP lies after its grid instant: use `u.media_index`, and for ANC and fastmeta
  before their index exists the `media_index` of the video unit with the same `rtp`. For interlaced
  video the grid period is the frame period. The 1.001 s start rule goes: any programme-grid instant
  works (`mtl_grid_offset(t, 1/G)`, G the programme's grid rate). An array's excluded rates start at
  the first sample at or after the array's start T0 (MS6).
- **D-235**, continued: From MS2a the wrapper's time thread, once a second, recognises a
  `ptp_get_time_fn` that reads CLOCK_TAI (kernel offset set) or CLOCK_REALTIME within 10 µs and sets
  only that clock's flags and state (CLOCK_REALTIME: ESTIMATED, UTC); the value is always the
  function's snapshot, so `mtl_time_now` and the engine never diverge. Amends OI-72.
- **D-236**, continued: TX swaps the templates of every changed leg in the builder's `tick` at
  `WAIT_FRAME`, on the first unit built after the call (with none being built, the next index
  boundary by the clock); the RTP sequence, SSRC and shaper continue. RX takes K, the newest unit in
  assembly, at the call (with none, it switches at once) and moves every changed leg's accept filter
  at the first tick after K and every older unit are published (complete, or at their due time), so
  no old-flow unit is cut. When every enabled leg's flow changes, no unit after K gets a slot, a
  new-flow packet that arrives before the switch is dropped, and the new-flow unit such a drop left
  without its first packets is dropped whole, so the new flow starts at a whole unit; that tick
  resets the slots (timestamps to none, `redundant_error_cnt[]` zeroed, SF-71), and the first unit
  delivered from the new flow carries DISCONTINUITY, and RELOCKED when its `media_index` is not
  after the last delivered. With an enabled leg unchanged, that leg loses nothing, nothing is reset,
  dropped whole or flagged, only the changed leg's `redundant_error_cnt` is zeroed, and its new-flow
  packets join its timeline. Every packet the switch drops counts in
  `rx.pkts_rejected{cause=flow_switch}`. Receipt is acknowledged within D-48's ack timeout; the call
  returns PENDING while a changed TX leg's unit is in mid-build or RX has not switched, and APPLIED
  at the switch (or when every leg is unchanged); in ARMED planned = applied = T0. Until MS5,
  `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`): an instant `when`, a changed `legs_disabled`, a leg on the
  kernel-socket backend or an RX leg on AF_XDP (field "port"); until SP-03 is fixed for core
  sessions, RX with `rx.threads` = 2 (field "rx.threads"). A unicast TX neighbour is resolved in the
  call within `port.arp_timeout_s`, else `-MTL_ETIMEDOUT`, `WAITING_NEIGHBOUR`, nothing changed
  (MS3–MS4). Amends D-193, D-93 and D-48. Why: legacy parity (U-135, U-136) at a unit boundary,
  where legacy switches mid-frame and re-creates every leg's rule.
- **D-239**, continued: Every index without a unit, a late-dropped one's included, carries the
  pinned whole unit of its parity (D-212; no pin: nothing), decided at its pick-up deadline and sent if picked by
  that deadline + `sched_ns` (`repeat_slack_ns`, at most the lead), with the media time M(E) of the
  empty index E and the RTP `rtp(N) + RTP(E) − RTP(N)`, N the pin's index and RTP(i) the RTP of
  index i (timing.md §6.4). A repeat is not a unit: no `seq`, no result, no user meta. A busy or
  late repeat is skipped and counted in `tx.indices_empty`.
- **D-240**, continued: Core sessions only; legacy unchanged. The pins are D-212's. Not taken:
  aliased spare frames (E11), a copy.
- **D-241**, continued: The TX default pool under REPEAT is max(`min_count_direct`, 3) +
  (`defer_step` − 1); the hard minimum is pins + 1 (2, interlaced 3, `POOL_TOO_SMALL`); a REPEAT TX
  over another session's pool needs the owner's n + 1 + `defer_step`, n the owner's units in
  reception at once (D-204; create checks it).
- **D-242**, continued: It runs on the transport bytes before dequeue's conversion; with
  `rx.convert_per_packet` it fills the application format with zero; the black block is floor(4096 /
  L) × L bytes, L = LCM(pgroup, 64), in `internal_bytes`; within 1.2× of `memset`. Lost ranges are
  no longer recognisable in the pixels; `MTL_SESSION_RX_NO_FILL` leaves the slot's previous bytes.
  Amends D-37 and D-203 for video, and D-183.

## 3. Why: prior art

The rules of D-01, D-03, D-13…D-15, D-74, D-80, D-215 and the waits (D-158–D-165) follow other media and
I/O APIs; the standards
facts and the bibliography are in [standards.md](standards.md).

| Rule | Decision | Taken from | Not taken |
|---|---|---|---|
| TX statuses ON_TIME, DROPPED, FLUSHED, FAILED (LATE: Phase 7), with a reason | D-14 | DeckLink {Completed, DisplayedLate, Dropped, Flushed} | JACK's argument-less xrun; AJA's aggregate drop count |
| a results ring that cannot overflow: acquire reserves the entry | D-03 | Vulkan `QUEUE_FULL`, DeckLink `E_OUTOFMEMORY` | io_uring's overflow list (it allocates) |
| a signed `margin_ns` where a deadline exists, `MTL_TIME_NONE` otherwise | D-14, D-215 | Vulkan `presentMargin` | a boolean on-time flag |
| the next index and its deadline | D-14 | OpenXR `predictedDisplayTime`, CoreAudio `inOutputTime` | — |
| one no-value constant `MTL_TIME_NONE` | D-215 | FFmpeg `AV_NOPTS_VALUE`, GStreamer `GST_CLOCK_STIME_NONE` | validity flags beside each field (CoreAudio `mFlags`), zero (Vulkan) |
| cumulative counters | D-80 | WebRTC stats, OpenTelemetry cumulative temporality | SRT and ALSA reset-on-read |
| `struct_size` first in every input | D-74 | AJA `NTV2_HEADER`, Win32 `cbSize` | — |
| inline status per result, every buffer flushed back at close, refcounted memory close | D-03, D-16, D-36 | libfabric's pain points (error queue, silent discard, undefined MR close) | — |
| lifetime ends at an explicit result | D-01 | — | NDI's "valid until the next call" |
| MTL checks lateness against the deadline itself | D-13 | DPDK: past launch times "should be ignored" | trusting the NIC |
| arm by the expected value: load the event word before the attempt, arm by CAS from it, sleep on the futex compare | D-158 | eventcounts (folly `EventCount`), the futex value check | sleeper bits a producer clears (they lose wake-ups) |
| one-shot reports from a list that cannot overflow; sleep only after the queue said nothing | D-159, D-160, D-162 | epoll's ready list and `EPOLLONESHOT`, libfabric `fi_trywait`, io_uring's CQ sized by construction | level reports (they spin), edge reports (they strand) |
| coalesced application posts that need no arm and never overflow | D-206 | libuv `uv_async_send`, GLib `g_main_context_wakeup`, libfabric `fi_signal`, kqueue `EVFILT_USER` with `NOTE_FFOR` (OR-ed flag bits) | io_uring `IORING_OP_MSG_RING`, IOCP `PostQueuedCompletionStatus` (an entry per message: a queue to size, or an allocation) |
| wake syscalls off the pinned cores, into one thread | D-165 (contingency) | io_uring SQPOLL, ksoftirqd, DPDK service cores | a dedicated core by default |

## 4. Defaults for the ST20 port

What MS1 and MS2 implement or test for ST 2110-20 beyond the rows of §2.

| ID | Topic | Default | Where |
|---|---|---|---|
| DF-01 | instance | `struct mtl_instance_params` lands before the rest of the API; `mtl_legacy.h` bridges a legacy `mtl_handle` into an instance, with no reverse call | contract.md |
| DF-02 | progress | automatic only, no manual `run_once` (the null backend and test clock cover tests) | engine.md |
| DF-03 | migration | ported: `session.migrate` set on the instance (off), with a quiesce and acknowledge handshake (MS2) | engine.md |
| DF-04 | TX-hang recovery | a verdict in the slot, never a `sh_info` reset (SF-41, D-123); the builder holds +1 on `sh_info` from frame start to the last attach (MF7, MS1); gap units DROPPED; on a per-instance worker with RX auto-detect re-init in MS6 | engine.md |
| DF-05 | scheduler threads | `MTL_INSTANCE_TASKLET_THREAD` and `_SLEEP` keep their meaning; the sleep path flushes the pending wakes first; scheduler threads `rte_thread_register` | engine.md |
| DF-06 | stop | pauses: the RX join and flow rule stay until close or a flow update | contract.md |
| DF-07 | re-open | EAL survives the last close, so open works again (#1341); a legacy `mtl_uninit` with unified objects open returns `-EBUSY` | contract.md |
| DF-08 | before start | TX acquire and submit are accepted in CREATED, STOPPED and ARMED | contract.md |
| DF-09 | single-leg link loss | the session stays RUNNING, units DROPPED (`LINK_DOWN`), `MTL_EVENT_PORT_LINK` (MS3), resume on link up | contract.md |
| DF-10 | processes | one process per instance; no DPDK secondary processes | deployment.md |
| DF-11 | create | reserves queues, quota and flow-rule capacity; rules and joins at the first start | contract.md |
| DF-12 | back-pressure | `status.blocked_on` explains every stall (BUFFERS, RESULTS, APP_LEASES) | contract.md |
| DF-13 | idle cleanup | the transmitter calls the non-blocking `rte_eth_tx_done_cleanup` on idle sessions with frames in flight, only in frame state `WAIT_FRAME`, rate-limited, dedicated queues only (spike S6). Amended by D-213: on shared queues by trylock of `tx_mutex` | engine.md |
| DF-14 | direct pools | `MTL_SESSION_REQUIRE_DIRECT` with too few slots fails (`POOL_TOO_SMALL`): 2 slots, 3 under 512 packets per unit | contract.md |
| DF-15 | no locked TAI | timed sessions run on `MTL_TIME_SOURCE_SYSTEM_TAI`, labelled estimated, with a warning event (MS3). Amended by D-229: until AUTO moves up without a step (MS3) | timing.md |
| DF-16 | lip-sync offset | `tx.index_offset` in whole units, besides `sc.media_time_offset_ns` | timing.md |
| DF-17 | pacing | per port; a session requests a class (`MTL_OPT_PACING`) and reads the granted one (`info.pacing_class`) | timing.md |
| DF-18 | submission order | FIFO with strictly increasing media time | timing.md |
| DF-19 | sent times | software estimates (`MTL_TXR_ESTIMATED`) first, NIC timestamps (`MTL_TXR_SENT_HW`) later, reported apart from the admission verdict | timing.md |
| DF-20 | late rows | `MTL_ROWS_TRUNCATE` by default, `PAD` opt-in, `STALL` legacy only (MS2) | timing.md |
| DF-21 | ST 2022-7 RX | reports the tolerated path differential (`info.tolerated_skew_ns`); `rx.skew_budget_ns` class A (10 ms) where memory allows; relock and slot count D-133; the default pool 4 with two legs (D-204) | timing.md |
| DF-22 | sender types | granted by format and milestone (D-143); W and NL linear from E5a (MS5), legacy behind `ST20_TX_FLAG_LINEAR_SCHEDULE` (D-24) | contract.md |
| DF-23 | user pacing | legacy nearest-epoch stays in the shim; unified `MTL_SUBMIT_NOT_BEFORE` moves TX by ≈ 604–619 µs at 1080p59.94 | migration.md |
| DF-24 | first late unit | with `min_tx_delay_ns` set, the first late unit raises `MTL_EVENT_TIMING_INFEASIBLE` (MS3). Amended by D-218: with a positive `min_tx_delay_ns` | timing.md |
| DF-25 | windowed stats | maxima over 1 s and 60 s, and log2 histograms | contract.md |
| DF-26 | timing parser | opt-in (`rx.timing_parser`), integer math; without NIC timestamps on processing time as today, in-burst packets in `tp.untrusted_pkts` | timing.md |
| DF-27 | flow headers | DSCP 0 = the profile's (ST 2110: CS0), TTL 0 = 64; VLAN and IPv6 reserved | contract.md |
| DF-28 | atomics | C11 atomics in `lib/`; public headers C99 and C++ clean | engine.md |
| DF-29 | samples | about six canonical samples plus an event loop, each also on the null backend in CI (G-97) | examples.md |

## 5. Open issues for the implementation

The accepted resolution of each issue an implementer must still apply, in code, a header comment
or a named document. A row leaves the table when the code applies it; its ID is not reused.

### 5.1 Instance, lifecycle, time sources

| ID | Rule | Milestone |
|---|---|---|
| OI-2 | On a bridged instance, create works at any time and a start before the legacy `mtl_start()` is `-MTL_EBUSY` (`WRONG_STATE`): no tasklet runs before it (`dev/mt_dev.c:2113-2125`); RxTxApp and KahawaiTest start it first | MS1 |
| OI-3 | The legacy teardown order is fixed too, as a bugfix (D-24): `mt_sch_mrg_uinit` releases lcores before it frees active schedulers (`mt_sch.c:1022-1030`); SP-01 | MS1 |
| OI-7 | `CLOCK_NOT_OWNED` (614) is `PTP_BUILTIN` on a PF with `time.phc_trust` = `MTL_PHC_TRUST_YES`; MTL cannot see a node ptp4l itself ([deployment.md §4.8](deployment.md#48-time-in-a-pod)) | MS1 |
| OI-11 | Generated session names are `<essence>_<tx or rx>_<n>`, stated at `mtl_session_config.name` | MS1 |
| OI-61 | The engine fixes a session's linesize at create (`st_tx_video_session.c:3333-3339`, RX `:3343-3351`), but an attached pool brings its layout at attach. Choose: create the engine session at the first start for `MTL_SESSION_POOL_ATTACHED` (create still validates through query), or add a set-linesize engine call (MF9) | MS2 |
| OI-62 | A MEDIA or POOL update, a changed set of legs or a port change, which re-creates the engine session, restarts the RTP sequence at 0 (D-236): `st20_tx_ops` has no seed. Choose: a seed field in the engine ops, or never re-create on update | MS3 |
| OI-72 | A wrapper, its core and its bindings run on port P's legacy clock (D-144); `mtl_uninit` is `-EBUSY` until it retired (E1d). MS1: `mtl_time_now` reads the default and TSC clocks (UTC, a WARNING: D-235), else `-MTL_ENOTSUP`; C keys `-MTL_EBUSY`, R keys `-MTL_ENOTSUP`. MS2a: the snapshot (timing §2.2) with D-235's flags, the option mapping (contract §2.8) | MS1 (A1, E1d), MS2a (C-BRIDGE) |

### 5.2 Data path, results, memory

| ID | Rule | Milestone |
|---|---|---|
| OI-23 | In pods the node agent that hands over `port.xsk_map` sets and resets the AF_XDP `tx_maxrate` (a rate request in the MtlManager protocol), else software pacing; today the library writes sysfs (`dev/mt_af_xdp.c:867-874`) | MS3 |
| OI-59 | Video binding memory: TX library pools use the engine's own framebuffers in MS1, RX uses `query_ext_frame` with core-allocated slots from MS1 (D-126); from MS2 every TX slot is an ext frame carrying its page table (PA mode builds them for engine frames only, `st_tx_video_session.c:245-266`) | MS1, MS2 |
| OI-60 | MS1 converts in the caller (submit, dequeue) for the formats st20p converts today; converter plugin threads join in MS2 (D-100) | MS1 |
| OI-63 | D-158–D-162, D-167–D-169 in code: the event word, calls with a timeout, interrupts and the walk, close and retire (wait-protocol.md §2–§6, §9); queues in MS2a (wait-protocol.md §7); pinned by the model job and the hook tests (implementation-plan.md §8.2) | MS1 (C0, C1a, C1w, C1b, C2, A2a), MS2a (C1q1, C1q2) |
| OI-66 | D-142: the count-bounded flush from MS1 (C1w); S1 and its rules SR1–SR3 and SR6 in MS2a; the dense shapes OI-80 | MS1, MS2a, MS4a1 |
| OI-67 | Video and cvideo RX library slots per D-151 (MS1; cvideo MS4b); wrapping per D-228 (ex20, MS1; the flag from B2); `mtl_rx_provide` per contract.md §9.12 (MS2b) | MS1, MS2b |
| OI-74 | contract.md §3.9: the latency fields, `convert_ns` (calibration MS1, measurement MS2a), `MTL_INFO_LATENCY_INFEASIBLE` (D-225; D-156 superseded) | MS1, MS2a |
| OI-80 | The dense shapes (D-170): a timer-paced W0 sweep, a timer-paced `MTL_QUEUE_POLLED` queue, and sleeping queues, measured at the dense-audio reference load (D-184) aligned and staggered; D-164 and D-166 are built only if their budget rows of implementation-plan.md §8.4 fail; the notifier (D-165) only by D-142's rules; the estimates to beat: wait-protocol.md §4 | MS4a1 |

### 5.3 RX

| ID | Rule | Milestone |
|---|---|---|
| OI-25 | The fill (video: the format's black; audio, cvideo and fastmeta: zero; ANC units are never filled) is for library pools only; an attached pool is never filled: its unit is `MTL_RX_INCOMPLETE` with the loss counts of `mtl_rx_get_detail`, as legacy reports counts; the `MTL_SESSION_RX_NO_FILL` comment says so | MS2a |
| OI-26 | The RX binding copies the reassembly slot's packet bitmap into the slot table (≈ 540 B at 1080p) through `st_engine_core.h` (D-125): the engine clears it at reuse (`st_rx_video_session.c:1298-1299`); until then no lost ranges are reported | MS2 |
| OI-73 | contract.md §3.4, §3.5 and engine.md E14 per D-155; the joint `raster.fps` rule; the rate detector of timing.md §11.9 | MS3 |
| OI-84 | Dequeue into caller planes (C2's copy, with a bit value of its own, never 0x40) for consumers that copy every unit into foreign CPU memory with a conversion (MXL grains, FFmpeg with `app_format`), keyed on D-227's budget. The MS2a nightly's 2160p `app_format` RX measures the second pass; the opt-in is designed only if that pass matters | MS2a measurement |

### 5.4 Headers

| ID | Rule | Milestone |
|---|---|---|
| OI-40 | Legacy counters without a key (U-402): a key for each one RxTxApp or the gtests read; the debug-only ones listed as "no key" in migration.md §4.14 | MS3 |
| OI-41 | A non-hugepage import's IOVA is its VA where the IOMMU allows, not the range from `0x10000` (`mt_dma.c:21`) | MS2 |
| OI-42 | U-348: byte-layout structs of the RFC 4175 pixel groups in `mtl_format.h`; U-132: `unit.cookie` is the only cookie | MS4 |
| OI-43 | Plugins and framework elements that share an instance leave `lcores` and the port queue counts at 0 and use `session.tx_queue` and `session.migrate` ([contract.md §2.3](contract.md#23-shared-instance)); there are no per-session queue-count keys, so a second element's shared open does not fail with `INSTANCE_MISMATCH` | MS6 |
| OI-44 | Nanosecond ties in the epoch math keep today's rounding (ties down, `st_muldiv_u64_round_closest`), pinned by a legacy-gate unit test | MS3 |
| OI-45 | ANC and fastmeta TX get the video path's hang detection | MS6 |
| OI-64 | Availability: the call-class macro carries the milestone (`MTL_API_DP(1)`); an `error` attribute above `MTL_TARGET_LEVEL`; `MTL_LEVEL` is bumped in each exit commit, never by a release; one node per milestone, sealed at its exit and by each release cut while open (then `_MSn.k`), checked by `unified_exports` and `check_frozen_lists.sh`; in-tree `MTL_LEVEL + 1` (D-106, D-191) | MS1 |
| OI-65 | The option table (`mtl_options.def`) carries each key's `PROVISIONAL` flag, checked against the header's `P` marks by the H1b U test; withdrawal and `OPTION_WITHDRAWN` from the first withdrawal; the tiers reviewed at MS6 (D-139) | MS1, MS6 |
| OI-69 | `enum_names` pairs, `caps.pacing_req`, no `log.level`, `mtl_instance_params.log_level` (MS1: H1b, A1); the `MTL_PORTS` grammar of contract.md §2.2 inside open (MS1: A2c); the log sinks and the two parsers exported (MS2a; D-153, D-154) | MS1, MS2a |
| OI-70 | Pre-freeze polish: `mtl_interrupt(o, mode, uint64_t targets)`; `MTL_RETIRING`, `MTL_MUST_CHECK` on `mtl_mem_close`; a cookie without results is `-MTL_EINVAL` in every build (the maintainer, 2026-10-05); `MTL_INDEX` (D-140); "slot" means a buffer; typed `tsmode`, `max_udp_payload`, `troffset_us`, `timing_model`; final names | MS1, MS2–MS3 |

### 5.5 Formats, grants and ANC

| ID | Rule | Milestone |
|---|---|---|
| OI-71 | An interlaced or PsF raster whose reduced fps exceeds 30 frames per second is `-MTL_EINVAL`, reason `FIELD_RATE` (233), naming the raster's `.fps`; MTL's guard against the field-rate trap, not a conformance rule; ports use `mtl_raster_from_legacy()` | MS1 |
| OI-76 | 1080p at 100, 120000/1001 and 120 is not an ST 2110-21 §6.3.1 format (D-143): a requested N is granted W until MS5, then NL; MS1 sends it as legacy does and flags N there `MTL_INFO_NON_COMPLIANT`; C-GRANT and `Grant.table` pin it | MS1, MS2a, MS5 |
| OI-77 | SD interlaced N (480, 486, 576 lines) is granted N and flagged `MTL_INFO_NON_COMPLIANT` until E5b brings the SD constants from HEIGHT (SF-74), which removes the flag | MS2a, MS6 |
| OI-78 | ANC capacity `anc.max_packets` is 1–65 535 ANC packets per unit (0 = 32): RX records are indexed by sequence and create checks that the link can carry the capacity (D-148); N4 implements the checks | MS4a2 |

### 5.6 Timing and sync

| ID | Rule | Milestone |
|---|---|---|
| OI-68 | No phase relation between sessions and no `tx.phase_of`: the TAI-mode sessions of a programme add one δ = `mtl_grid_offset(anchor, video raster.fps)` (`mtl_sync.h`), computed once per programme and shared by the plugin; interlaced TAI units snap per parity on the frame grid; snap error ≤ 1 ns with times exact to the ns (timing.md §4.3 and §10.5) | MS1 (helper, parity), MS4a (recipes) |

### 5.7 Further issues

| ID | Rule | Milestone |
|---|---|---|
| OI-49 | A second `mtl_instance_open` of the same port set without `MTL_INSTANCE_SHARED` is `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | MS1 |
| OI-50 | Migration reports a move: an event (scheduler index, busy %) and `info.sched_index` as a gauge | MS3 |
| OI-54 | G-83 and G-87 name existing reasons (`HOLD_REQUIRED` 219, `LAYOUT_MISMATCH` 212, `GRID_MISMATCH` 225) | MS2 |
| OI-56 | A muted TX unit is `MTL_TX_DROPPED` (`LEG_DISABLED`), counted only in `tx.units_muted` | Phase 7 |
| OI-75 | The policy text of abi.md §7 is adopted now; the maintainer fills the targets and the LTS end, no-earlier-than, in the MS3 release notes (D-69) | MS3, MS7 |
| OI-81 | The open rows of the legacy-behaviour ledger (migration.md §6.5: LB-02 send order, LB-10 RX order) are decided in task RB0 before the st20p re-base code starts | MS2b |
