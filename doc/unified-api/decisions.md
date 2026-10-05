# Decisions

| | |
|---|---|
| Status | Maintained |
| Date | 2026-10-02 |

The design rationale of the unified API for maintainers: one line per design decision (D-xx),
the prior art behind the main rules, the defaults the ST 2110-20 milestones implement, and the
implementation issues still to apply (OI-xx). The headers in
[sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) are normative; where a row
and a header disagree, the header wins.

## 1. Keeping this file

- A D-xx states one decision in its current form; **Where** names the document that specifies it.
- IDs are stable, never renumbered or reused; the next free numbers are D-139 and OI-76.
- A changed decision changes the header in `sketch/` and the document in **Where** in the same
  commit.

## 2. Decisions

| ID | Decision | Where |
|---|---|---|
| D-01 | One slot and lease state machine and one submit/dequeue contract for every provisioning path (library pool, attached, exported) | contract.md |
| D-02 | App-driven push on TX (`mtl_tx_acquire` → fill → `mtl_tx_submit`); the library never pulls frames through application callbacks | contract.md |
| D-03 | Results are returned in submission order (`seq`) from the order ring, written in place by the completer (D-119); acquire reserves the result entry (D-120), so neither a result nor slot reuse ever waits; state changes are session events | engine.md |
| D-04 | No application code on tasklets: the tasklet side of a hand-off is a wait-free store plus a fence, wake-ups go through an armed-waiter bit and `mt_wake()`; an inline notify (`MTL_LATER`) only if busy polling (W0) cannot close the latency gap | engine.md |
| D-05 | Every public function has a call class (`MTL_API_CP`, `_DP`, `_DPC`, `_WT`, `_AS`), enforced in debug builds from MS3 | contract.md |
| D-06 | One internal core under every API owns the contracts, with one binding per essence and direction over today's engines (D-99) | engine.md |
| D-07 | Typed 64-bit handles over grow-only chunked tables, 0 = null; a lease is session index:16, slot:16, random generation:32; stale lease `-MTL_ESTALE`, foreign or closed `-MTL_EBADF`; an in-flight counter on its own cache line guards destroy | engine.md |
| D-08 | Nine states (CREATED, ARMED, RUNNING, DRAINING, FLUSHING, STOPPED, ERROR, CLOSING, RETIRED); start now, at a TAI or an index with `preroll_ns`; stop DRAIN or FLUSH; `mtl_session_discard`; results readable until close | contract.md |
| D-09 | Media time and launch time are separate; RTP = `floor(M × R) mod 2^32` from media time, one rule for every essence except `MTL_MEDIA_SENDER` (Phase 7, D-95) | timing.md |
| D-10 | The default video media time is the frame epoch `N × TFRAME`, not the TX cursor | timing.md |
| D-11 | Media-time math is exact rational arithmetic (128-bit intermediates) from the epoch, or from the start's T0 under `MTL_WHEN_ORIGIN`, never accumulated | timing.md |
| D-13 | Explicit late policy (`tx.late_policy`: DROP or RESLOT; AUTO → RESLOT, INDEX/TAI → DROP) and underrun policy per essence; an infeasible exact request fails, never falls back; bounded `MTL_LATE_SEND_LATE` (`tx.late_tolerance_ns`, `WOULD_OVERLAP`) is Phase 7 under `MTL_LATER` | timing.md |
| D-14 | TX results carry deadline, sent time and a signed `margin_ns`; `MTL_TX_ON_TIME` is an admission verdict; `mtl_tx_next_slot` names the next slot and its deadline | timing.md |
| D-15 | Every time is `int64_t` TAI ns with a validity flag (`MTL_UNITF_*_VALID`, `MTL_TXR_*`, `MTL_TIMEF_*`), never a sentinel | timing.md |
| D-16 | Regions (`mtl_mem.h`) are refcounted, page-aligned, never rounded outward, mapped at first attach or into named ports; `mtl_session_attach` takes regions, no IOVA call; shmem, memfd, hugetlbfs and pinned host memory direct, files copy only; a non-hugepage import's IOVA is its VA (OI-41) | contract.md |
| D-17 | The data path does not depend on where memory came from: direct when possible, else a copy reported per session (`MTL_INFO_DIRECT`) and unit (`MTL_TXR_COPIED`); `MTL_SESSION_REQUIRE_DIRECT` fails at create | contract.md |
| D-18 | The terminal TX result comes after storage access and the transport outcome are both finished (the engine's done callback after the last buffer reference, MF1) | engine.md |
| D-19 | Capabilities per port, dry-run `mtl_session_query`, REQUIRE/PREFER/OFF requests with granted values; runtime downgrades are events (`MTL_EVENT_PACING_CHANGED`) | contract.md |
| D-21 | One error vocabulary, `MTL_E*` = Linux errno values, one meaning each (`-MTL_ESHUTDOWN` only after the app's own stop or close, `-MTL_EIO` ERROR, `-MTL_ENODEV` device gone, `-MTL_EAGAIN` nothing now); errno values because every caller already maps them (`strerror`, frameworks' errno mappings) and they cross language bindings unchanged | contract.md |
| D-23 | One DSO: libmtl carries the unified API in its own version node with the API shell compiled in, so there is no cross-DSO core ABI and pkg-config stays `mtl` (migration.md §7.2) | migration.md, engine.md |
| D-24 | Engines first: legacy APIs get every engine fix; wire-visible changes (rounding, default RTP, ANC RTP, ST 2110-22 CBR, linear read schedule) only behind legacy opt-in flags, proven by the pcap-diff gate G-99 | migration.md, engine.md |
| D-26 | One flow descriptor (`struct mtl_flow`) for every essence and leg, without RTP identity (`ssrc` and `payload_type` are per session, D-133); flow changes apply on every leg at one `struct mtl_when`, prepared before commit, never holding a tasklet lock across ARP or IGMP | contract.md |
| D-27 | PR #1610 stays a reference until the unified header lands, then is closed with credit; its `mt_session_event.c` seeds `mt_wake` and the SF-15 fix | migration.md |
| D-30 | Async-signal-safe, sticky interruption (`mtl_interrupt(o, mode)` and its wrappers): data waits return `-MTL_ECANCELED` until cleared; bits 8–31 of `mode` name the wait targets (`MTL_WAIT_*` << 8, 0 = every target), so an `unlock` cancels acquire without a reaper spinning; stop and close still work | contract.md |
| D-31 | Explicit time sources (`enum mtl_time_source`); tasklets read one published time base; on a clock step media times are kept (one rule, no per-session policy) | timing.md |
| D-32 | ST 2110-22 is its own essence (`MTL_CVIDEO`), CBR by default, `MTL_CVIDEO_VBR_MAX` opt-in and non-compliant; an oversize codestream fails at submit | timing.md |
| D-33 | Input structs grow through `struct_size` and named `reserved` fields; unknown non-zero bytes are `-MTL_EINVAL` | contract.md |
| D-34 | Names: essences `MTL_VIDEO` … `MTL_RTP`, verbs `mtl_tx_*`/`mtl_rx_*` and object verbs (D-104), timeout last; headers `include/mtl/experimental/`, shell `lib/src/unified/`, core `lib/src/st2110/core/`, both in libmtl (D-23) | contract.md |
| D-36 | Close is idempotent and deferred while leases are out: `mtl_close` returns 1 while retiring, 0 once retired, and a repeated close polls, never `-MTL_EBADF`; on a closing handle only status, the release of earlier leases and interrupt (a no-op) are valid. Not `-MTL_ETIMEDOUT`: the close has happened, only the memory waits for leases still held | contract.md |
| D-37 | RX pools: a full pool drops new units, or reclaims the oldest unread (`MTL_SESSION_RX_LATEST`); `MTL_SESSION_RX_BY_INDEX`; library pools zero-filled in dequeue (`MTL_SESSION_RX_NO_FILL` opts out), attached pools not (OI-25) | contract.md |
| D-38 | The media mode (AUTO, INDEX, TAI) is a session property; TAI snaps NEAREST with hysteresis, with no locked-phase snap, off-grid policy or phase-drift gauge in v1; duplicates are results (`DUPLICATE_SLOT`, `DUPLICATE_INDEX`), not errors; the binding takes the slot decision (D-131) | timing.md |
| D-39 | No source kinds: `min_tx_delay_ns` is the one knob (a camera sets one frame period + the pick-up lead, D-130); the slot delay L is reported, not configured | timing.md |
| D-41 | Audio: a carry buffer, whole samples per packet, ±one packet absorbed in TAI mode, gaps keep the packet grid, only a discontinuity re-phases; `mtl_tx_write` for copy essences | timing.md |
| D-42 | Array reads (`mtl_reap`, `mtl_read_events`) take the caller's record size and return a count ≥ 1 or `-MTL_EAGAIN`; the typed `static inline` wrappers (`mtl_tx_reap`, `mtl_tx_reap_full`, `mtl_session_read_events`) pass `sizeof(*r)` | contract.md |
| D-43 | Wait handles are portable (eventfd, Windows auto-reset event), one per session or instance (`mtl_get_wait_handle`) | contract.md |
| D-44 | `mtl_instance_h` over versioned `struct mtl_instance_params`; `MTL_INSTANCE_SHARED` is one refcounted instance per process, a differing later open `-MTL_EEXIST` (`INSTANCE_MISMATCH`), no merge; runtime port open later | contract.md |
| D-45 | The headers are normative, compile as C99 and C++17 with `-Wpadded -Werror`, size-check every public struct, and `check.sh` runs in CI | contract.md |
| D-48 | Commands are immediate (stop, flush, discard, interrupt) or at a boundary (flow, grid, leg); the control plane applies them for a detached session; one unacknowledged for a fixed 100 ms is ERROR (`CMD_TIMEOUT`); form D-103 | engine.md |
| D-49 | Close or ERROR on a stalled queue: bounded idle cleanup, then a worker restarts a dedicated queue or quarantines a shared one, up to a port reset; retirement after the device references are gone | engine.md |
| D-50 | Acquire and release are MP-safe; one submitter unless `MTL_SESSION_MT_SUBMIT`; readers take the reaper lock; destroy and stop always drain the in-flight counter; no single-reader or export-pool flag: an exported pool binds one framework wrapper to each slot with `mtl_tx_acquire_slot`, so it needs neither results nor `MTL_SESSION_MT_SUBMIT` (migration.md §12.4) | contract.md |
| D-51 | RX runs on the epoch timeline and reports an exact `media_index`; an RX start array arms all filters with one link offset; units complete at a due time (MS2); the join is made at the first start, kept across stop | timing.md |
| D-52 | `preroll_ns` in `struct mtl_when`; the horizon (`tx.horizon_ns`, 1 s) counts from the resolved start; a start stranding queued units beyond it fails (`BEYOND_HORIZON`) | timing.md |
| D-53 | `min_submit_lead_ns` = media time − pick-up deadline; `media_time_offset_ns` has one meaning, the producer's latency in TAI mode (it moves the slot); the legacy `rtp_timestamp_delta_us` (an RTP trim with the launch fixed, AUTO and INDEX) is `tx.rtp_trim_ns`; live sinks use TAI + NEAREST, INDEX is for whole-timeline owners | timing.md |
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
| D-67 | Live ANC and fastmeta follow the first video of their start array (its raster and slot delay), keep the video RTP and go in the ST 2110-40 window of the frame their video unit is sent in; no media-frame anchoring option | timing.md |
| D-68 | Waking is W2-deferred: a completing context never makes a syscall; `mt_wake()` sets the session's bit in a per-scheduler pending bitmap (summary word on its own cache line), and the scheduler loop (`sch_tasklet_func`, after the handler loop) writes each flagged session's eventfd once per iteration; an application thread that completes writes directly | engine.md |
| D-69 | Security posture of the new surfaces (imports, wait handles, the wake path, handle randomness, debug API gating); deprecation no earlier than two `vYY.MM` releases after notice | deployment.md |
| D-70 | Small defaults: ANC `total_lines` 0 = the video's raster, else 1125; fastmeta rate 0 outside a video start is `FIELD_REQUIRED`; histograms are log2, with no layout keys | contract.md |
| D-71 | A lean `mtl.h` (C library includes only) and optional headers with one job each; the function counts per header and per milestone come from `check.sh` (D-137) | contract.md |
| D-72 | One `struct mtl_session_config` (one member per essence, plus `ssrc` and `payload_type`); `mtl_session_create`, `_query`, `_open`; `mtl_session_update(s, &sc, parts, &when, &planned)` reads only the members `parts` names, so no read-back call. Members, not a union: one `MTL_INIT` serves every essence and a wrong member is caught (`OTHER_ESSENCE`) | contract.md |
| D-73 | Typed fields for what most applications set; every other knob is a listable option key (`mtl_options.h`) with presence semantics; no key for what another key, a request or a fixed value covers (`video.*` serves cvideo, `session.migrate` on the instance, `MTL_REQ_REQUIRE` on `MTL_OPT_PACING`, the 100 ms ack timeout); a frozen key may become an accepted no-op, never an error | contract.md |
| D-74 | Inputs carry `struct_size` and are initialised by `MTL_INIT(&s)`; outputs take a size argument | contract.md |
| D-75 | One `struct mtl_unit` (208 B) lent by acquire and dequeue, read by submit; 64-bit flags (TX bits 0–31, RX 32–63) | contract.md |
| D-76 | Buffers are pool slots named by index (`unit.slot`); `mtl_session_attach` lays out N slots; leases stay typed (`mtl_lease_h`) | contract.md |
| D-77 | A 96-byte core TX result, the full record by size; results always for application memory, opt-in (`MTL_SESSION_RESULTS`) for library pools | contract.md |
| D-78 | No group object: start and stop take arrays, because a group would be one more object with its own lifecycle for what is one instant; a start array is one direction on the epoch timeline, all or none; `MTL_WHEN_ORIGIN` in `struct mtl_when.flags` makes media index 0 of every started session the start's T0; created timelines and anchors are `MTL_LATER` | timing.md |
| D-79 | Events per session and per instance (`MTL_OBJ_INSTANCE`: port, time, health, manager) through `mtl_read_events`, `mtl_wait` and `mtl_get_wait_handle`; no shared queues in v1 (`mtl_queue_*` and dispatch threads are `MTL_LATER`), because per-object handles serve the consumers in tree; the scale at which they stop serving is measured by spike S1 (OI-66); no user posts | contract.md |
| D-80 | A stats registry of named, typed values (`mtl_stat_*`) with bulk DP reads; counters cumulative, never reset; lean status and info structs (D-128 for the per-scheduler blocks) | contract.md |
| D-81 | One reason vocabulary (`mtl_reasons.h`) for errors, states, events and TX results | contract.md |
| D-82 | Packet units (`MTL_UNIT_PACKETS`) on every essence and the generic `MTL_RTP` essence (MS5): a chunk of slots is one unit with one result; no mbufs, no app code on tasklets; defaults D-115 | contract.md |
| D-83 | Legacy headers leave the public set in three tiers (session, pipeline, core), stages A → B → F → F+1 → ≥ F+2; their symbols sit in the `MTL_LEGACY` node, are deprecated at MS7 (F) and leave `MTL_LEGACY` from F+2; plugin ABI v2 and the conversion calls of `mtl_format.h` before F; pipeline-header removal date set after MS4 | migration.md |
| D-84 | `mtl_last_error()` follows errno: only a failing call writes it | contract.md |
| D-85 | `MTL_TIME_SOURCE_AUTO` = a disciplined NIC PHC (from MS6, D-132), else `CLOCK_TAI`, else SYSTEM_TAI (estimated), chosen once at open; the built-in PTP client only when named | timing.md |
| D-86 | ANC packet tables and user meta live in the slot's meta area, validated and copied at submit | contract.md |
| D-87 | Coverage gate: every legacy capability has a unified home and a milestone, or is on the cut list ([coverage.md](coverage.md) §4.5, CUT-1…CUT-13) | coverage.md |
| D-88 | Submit consumes the lease on failure (a failed first submit returns the slot to the pool), except `-MTL_EBADF` and `-MTL_ESTALE`, which change no state; `-MTL_EAGAIN` for every "nothing now", arming only the targets of an existing wait handle (D-122); interrupts cancel data waits only | contract.md |
| D-89 | The last `mtl_instance_close` shuts down network first, MtlManager grants last: 0, 1 or `-MTL_EIO` (`QUEUE_QUARANTINED`), and a repeated close polls as in D-36; `mtl_instance_shutdown` adds a report; handle slots never freed (R4) | deployment.md |
| D-90 | R8: no signal handler or `atexit` in the library (DPDK's SIGBUS handlers excepted), close-on-exec, a fork closes MTL's descriptors in the child, nothing found by PID | deployment.md |
| D-91 | Lock-free health (`mtl_instance_get_health`); liveness independent of links, PTP and legs; open waits for no link, neighbour or lock and fails with one environment reason (600–614) | deployment.md |
| D-92 | CPUs from the affinity mask (MtlManager, `MTL_CPUARB_LOCKS` or none; no SysV table); threads pinned; no-IOMMU refused without `instance.allow_noiommu`; files only in `instance.runtime_dir` | deployment.md |
| D-93 | IS-05 activation at the slot boundary by the clock (`planned_tai_ns`, `status.update_state`, `MTL_EVENT_UPDATE`; both legs may share a port through an explicit `flows[1].port`), the atomic destination and source change in MS5; REAPPLY, DRY_RUN, cancel, mute and the REPLACED and CANCELLED states are Phase 7 under `MTL_LATER` | nmos-ipmx.md |
| D-94 | SDP render and parse (`mtl_ipmx.h`) on public calls only (Phase 7, `MTL_LATER`) | nmos-ipmx.md |
| D-95 | IPMX (Phase 7, under `MTL_LATER`): `session.profile` changes defaults and labels only (N keeps its Type N CMAX, within the IPMX ceiling, and adds the IPMX VRX check); `rtx.*`; `MTL_MEDIA_SENDER`; AUTO re-evaluation; IGMPv2; RTCP sender reports with the Info Block and FREERUN come earlier (D-134) | nmos-ipmx.md |
| D-96 | PEP encryption (`crypto.*`, `mtl_crypto_set_key`), no IV reuse under one key, HDCP keys never in MTL; built after its cost spike (Phase 7) | nmos-ipmx.md |
| D-97 | Typed configuration only, no spec strings in any struct; the inline `mtl_flow_parse(&f, "ip:port[@source]")` fills a `struct mtl_flow`; enums in legacy order (+1 where 0 means "not set"); `mtl_flow_ipv4()`, `raster.fps`, `audio.ptime`; `MTL_PORTS` stays | contract.md |
| D-98 | Port first: MS1–MS6 port today's functionality plus the Kubernetes lifecycle; NMOS extras, SDP, IPMX and PEP are Phase 7 under `MTL_LATER` | implementation-plan.md |
| D-99 | One core in libmtl with one binding per essence and direction (plus packet and null): a session is a slot table shaped like a lease table (D-118, seven slot states; a completion claims, writes the result, then publishes); the binding implements the engines' existing callbacks, no new entry point for frames and rows; legacy `st*p_*` become wrappers on the core | engine.md |
| D-100 | Conversion and codecs are a transform state on the slot (claim, done) shared by the caller, converter plugin threads and ST 2110-22 codec threads | engine.md |
| D-101 | Slice mode is required: `MTL_UNIT_ROWS` on video in MS2 through `query_frame_lines_ready` and `notify_slice_ready`; the engine adds a `query_frame_lines_ready` return code that ends a frame early (TRUNCATE) and `tx.troffset_ns`; interlaced rows are kept | implementation-plan.md, engine.md |
| D-102 | MS1 ships W2-deferred behind one internal `mt_wake(session, target)`; W3, a waker thread draining the same bitmap, is a contingency built only if spike S1 shows the deferred writes harm pacing (checked in MS2) | engine.md |
| D-103 | Commands are one `ctl` and one `ack` word per session, read by the session's tick (D-127: ack, RX due time, idle cleanup, heartbeat); flush and discard are CASes on the slot table; MS2 | engine.md |
| D-104 | Object verbs (`mtl_close`, `_interrupt`, `_wait`, `_get_wait_handle`, `_reap`, `_read_events`, `_release`) with typed inline wrappers; conversion in `mtl_format.h`; RTCP, SDP and PEP in `mtl_ipmx.h`; each function exported in its milestone (D-106; counts from `check.sh`); every Phase 7 name, values and inlines too, under `MTL_LATER` until the milestone that implements it | the headers |
| D-105 | Struct rules: `raster.fps` rational only; TR offset only `MTL_OPT_TROFFSET_NS`; 64-bit unit flags; growth room checked by `check.sh` (essence members 128 B, `mtl_when.flags` + `reserved[3]`, `mtl_port_spec` +32 B, `mtl_flow` +16 B); `used` 0 = a whole video frame, rows count rows, bytes and packets literal; acquire resets the TX meta area (`MTL_META_NONE`) | the headers |
| D-106 | Export only what is implemented, because an exported function is frozen: a function is exported in the milestone of its tag; the installed header declares the whole design and a later function fails to link (OI-64); the node is renamed freely before MS7; a known value not built yet is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`), an unknown one `-MTL_EINVAL` | contract.md |
| D-107 | AI agents write the code; the maintainer's review sets the pace: a task ≤ 1.5 k lines (mechanical moves exempt), `mtl-reviewer` first, ≤ 3 commits awaiting review and ≤ 2 developer tasks in flight | implementation-plan.md |
| D-108 | MS1 changes no existing export (the version script holds only the unified node); the soname, `MTL_LEGACY` and hidden internals wait for MS3's `nm` audit of leaked-symbol users, and the legacy symbols are deprecated at MS7 (F) and leave `MTL_LEGACY` from F+2 (D-83; migration.md §7.2) | implementation-plan.md |
| D-109 | Two validation stacks while both APIs exist: `KahawaiTest` (with NoCtx) and RxTxApp are frozen; their copies, rewritten on the new API with the same case names, are `UnifiedKahawaiTest` and `UnifiedRxTxApp`; the scripts run either or both; the legacy ones go with the legacy API (F+2) | implementation-plan.md §4.1 |
| D-110 | Acceptance runs RxTxApp and `UnifiedRxTxApp` as two pytest applications (`--app`) on the same configs; `UnifiedRxTxApp` keeps RxTxApp's CLI, JSON and output, and the bindings print the library lines the framework parses byte-identical (e.g. `TX_st20p(n), frame get try …`, `application_base.py:424-426`), so the plugin adapters survive the rewrite | implementation-plan.md §4.1 |
| D-111 | The null backend (`null:<n>`) is a binding; nothing changes in `lib/src/dev/`; null loopback delivers a TX unit to every RX session whose flow (destination IP and port) matches | engine.md |
| D-112 | Only the cut list of D-87 goes; per-packet conversion, VSYNC (`MTL_EVENT_EPOCH_TICK`), two RX threads, the ST 2110-30 knobs, pcap capture, converter plugins and the conversion calls of `mtl_format.h` stay | coverage.md |
| D-113 | An external review with at least three named consumers (FFmpeg and GStreamer plugin owners, the MXL team, the external engine team) and the private-user questionnaire gates the header freeze (MS7) | implementation-plan.md |
| D-114 | Unified wire defaults differing from legacy: frame-epoch RTP (D-10), incomplete RX frames delivered (`rx.incomplete` = `MTL_RX_DELIVER`), ST 2110-22 CBR (D-32), verbatim packets (D-115); legacy keeps today's (D-24) | contract.md |
| D-115 | Packet defaults: verbatim (only `packet.set_fields` written); RX chunks formed at dequeue, no reordering, legs deduplicated by RTP timestamp and sequence number; `MTL_RTP` is RTP only; RX copies unless `MTL_PKT_RX_LEND`; ST 2022-6 headers are the app's | contract.md |
| D-116 | Crash safety: a slot leased at shutdown is freed at the next control-plane call after its release; device removal is ERROR; SIGBUS hotplug only with `instance.hotplug`; `instance.cpu_shared` WARN; shutdown finishes the unit on the wire | deployment.md |
| D-117 | Pods: the IGMP window after SIGKILL (≈ 260 s) is documented; a node-disciplined PHC is trusted through `time.phc_trust`; set-up A (non-root, node prepared) is recommended, B the fallback | deployment.md |
| D-118 | One 64-bit atomic word per slot `{state:4 \| X:1 \| C:1 \| spare:2 \| holds:8 \| spare:16 \| gen:32}`: acquire and dequeue write the new generation in the CAS that takes the slot, tasklet CASes keep it; release, submit, hold changes and every tasklet CAS compare the whole word; a completion CASes to C (claimed), writes the result, then release-stores PUBLISHED | engine.md |
| D-119 | The order ring holds 64-byte submission descriptors `{seq:48 \| slot:16, gen, media time, launch, flags, used, cookie, …}` written by submit in one line; the completer writes the result in place, the reaper returns entries in `seq` order; the binding picks up only the entry at its cursor whose slot word is QUEUED with the entry's generation; size a power of two ≥ pool_count | engine.md |
| D-120 | The result reservation is one counter `resv`, CAS-incremented while `resv − reap_seq < N`; releasing an APP lease decrements it | engine.md |
| D-121 | Stop against submit: after its QUEUED store, submit re-loads the session state (seq_cst) and, if the session no longer accepts, applies the flush CAS itself, so exactly one of stop and submit wins; stop always drains the in-flight counter | engine.md |
| D-122 | One eventfd per session, because it keeps a many-session process within its descriptor limit and gives epoll one entry per session; a WT wait `poll()`s it and the instance interrupt eventfd; armed counters only suppress writes; a DP call arms only the targets in a wait handle's mask; several threads on one session: OI-63 | engine.md |
| D-123 | TX recovery (MS1, E1) records a verdict (DROPPED, `RECOVERY`) in the slot and drops only software-held references; the last PMD reference (the free callback) publishes; two `sh_info` per engine frame alternate per use, a use generation in `fcb_opaque`, so a stale free never completes the next use; attached memory completes after the queue stop/start (S8, MS3) | engine.md |
| D-124 | The core sees an instance-context interface (socket-aware alloc, TSC or test clock, wake flush hook, scheduler ID), never `struct mtl_main_impl*`; a null-only instance skips `mtl_init`, starts EAL itself (`--no-huge --no-pci --in-memory`) or uses a running one, and loops on a library thread; under the test clock completions run inside `MTL_FAULT_CLOCK_ADVANCE` (G-92) | engine.md |
| D-125 | `st_engine_core.h` lists every engine entry point the bindings use besides the callbacks: an RX frame put callable from the tasklet, the packet bitmap (MS2), "last packet handed" (MS2), the recovery verdict (MS1), per-leg arrival time, an RTP sequence seed (OI-62), an engine frame count ≥ pool_count + slot_max for RX_LATEST and BY_INDEX (E11) | engine.md |
| D-126 | RX uses `query_ext_frame` with core-allocated slots from MS1, one RX path; delivery order is a per-session `pub_seq` fetch-added by the completer; TX library pools use the engine framebuffers in MS1 (OI-59) | engine.md |
| D-127 | One tick per session (MS2): TX in `tvs_tasklet_handler` (the builder; the transmitter calls nothing), RX in `rvs_pkt_rx_tasklet_handler` | engine.md |
| D-128 | Stats blocks are per scheduler: a completion on another scheduler (a shared TX queue) writes that scheduler's block, never a block another tasklet writes | engine.md |
| D-129 | Tasklets log already formatted, length-capped lines into a per-scheduler ring that drops when full; a library thread hands them to the sink; no per-site codes | engine.md |
| D-130 | The pick-up lead is max(RL warm-up lead, one bulk build time + S0's p99.99 scheduler iteration) plus any conversion stage, about 0.5 ms with RL and 20 µs with TSC, not the ring depth (1.9 ms, kept as information); a capture producer that sets `min_tx_delay_ns` = one frame period + the pick-up lead gets L = 1 | timing.md |
| D-131 | The video TX binding takes the slot decision at `get_next_frame` (MS1) with exact math (AUTO next feasible, late ON_TIME with `MTL_TXR_RESLOTTED`; TAI nearest, DUPLICATE_SLOT, BEHIND, TOO_LATE DROPPED; EXACT checked, then `EXACT_USER_PACING`) and drives the engine with `USER_PACING \| RTP_TIMESTAMP_EPOCH` and `required_tai = N·T`; `notify_frame_late` never fires | timing.md, engine.md |
| D-132 | In MS1 AUTO is `CLOCK_TAI` if valid, else SYSTEM_TAI (vDSO); a PHC, one syscall per read, comes in MS6 with the published time base (E9) | timing.md |
| D-133 | ST 2022-7: one RTP identity per session (`sc.ssrc`, `sc.payload_type`; 0 = one random SSRC per session, the essence's default PT); RX relocks only when the stale value lags the newest by more than `rx.skew_budget_ns` worth of ticks or every enabled leg is stale; ceil(skew_budget / TFRAME) + 1 out-of-order slots, else `info.tolerated_skew_ns` and a warning | timing.md, contract.md |
| D-134 | DSCP lands with the bindings (`mtl_flow.dscp` into the TX builders' TOS; video MS1, the rest MS4); RTCP sender reports (TX, option-driven, library-built Info Block) in MS5; FREERUN with E9 in MS6 (those names leave `MTL_LATER` in that milestone); any frame rate (an engine table change) in MS4, a rational outside the table `-MTL_ENOTSUP` until then | implementation-plan.md |
| D-135 | The implementing session commits per task after its gates pass (signed off through the mtl-commit skill, no AI attribution) on a work branch; it never pushes and never opens PRs; the maintainer reviews and pushes | implementation-plan.md |
| D-136 | Tooling task P0 opens MS1: `run_gtest` gains `pacing_way`, `kernel:lo`, whitelisted args and `binary`; `mtl-developer` may edit the new trees of plan §4.1 and §4.2, the new pytest application, `.github/`, `build.sh`, `meson_options.txt`, `doc/unified-api/`; the frozen trees are BLOCKER paths for `mtl-reviewer` | implementation-plan.md |
| D-137 | Counts (functions per header, frozen names, functions per milestone tag) come only from `check.sh`; a feature's milestone lives in its coverage.md U-row and other documents link it | README.md |
| D-138 | Samples on the new API only: each legacy sample in `app/sample/` is deleted in the commit that adds its replacement (those of cut features in MS1); the new samples together call every exported function, checked in CI (G-114) | implementation-plan.md §4.2 |

## 3. Why: prior art

The rules of D-01, D-03, D-13…D-15, D-74 and D-80 follow other media and I/O APIs; the standards
facts and the bibliography are in [standards.md](standards.md).

| Rule | Decision | Taken from | Not taken |
|---|---|---|---|
| TX statuses ON_TIME, DROPPED, FLUSHED, FAILED (LATE: Phase 7), with a reason | D-14 | DeckLink {Completed, DisplayedLate, Dropped, Flushed} | JACK's argument-less xrun; AJA's aggregate drop count |
| a results ring that cannot overflow: acquire reserves the entry | D-03 | Vulkan `QUEUE_FULL`, DeckLink `E_OUTOFMEMORY` | io_uring's overflow list (it allocates) |
| a signed `margin_ns` in every TX result | D-14 | Vulkan `presentMargin` | a boolean on-time flag |
| the next slot and its deadline | D-14 | OpenXR `predictedDisplayTime`, CoreAudio `inOutputTime` | — |
| validity flags on every time; one coherent clock sample (`mtl_time_now`) | D-15 | CoreAudio `mFlags`, ALSA `actual_type` | sentinels (NDI `INT64_MAX`, Vulkan 0) |
| cumulative counters | D-80 | WebRTC stats, OpenTelemetry cumulative temporality | SRT and ALSA reset-on-read |
| `struct_size` first in every input | D-74 | AJA `NTV2_HEADER`, Win32 `cbSize` | — |
| inline status per result, every buffer flushed back at close, refcounted memory close | D-03, D-16, D-36 | libfabric's pain points (error queue, silent discard, undefined MR close) | — |
| lifetime ends at an explicit result | D-01 | — | NDI's "valid until the next call" |
| MTL checks lateness against the deadline itself | D-13 | DPDK: past launch times "should be ignored" | trusting the NIC |

## 4. Defaults for the ST20 port

What MS1 and MS2 implement or test for ST 2110-20 beyond the rows of §2.

| Topic | Default | Where |
|---|---|---|
| instance | `struct mtl_instance_params` lands before the rest of the API; `mtl_legacy.h` bridges a legacy `mtl_handle` into an instance, with no reverse call | contract.md |
| progress | automatic only, no manual `run_once` (the null backend and test clock cover tests) | engine.md |
| migration | ported: `session.migrate` set on the instance (off), with a quiesce and acknowledge handshake (MS2) | engine.md |
| TX-hang recovery | a verdict in the slot, never a `sh_info` reset (SF-41, D-123); the builder holds +1 on `sh_info` from frame start to the last attach (MF7, MS1); gap units DROPPED; on a per-instance worker with RX auto-detect re-init in MS6 | engine.md |
| scheduler threads | `MTL_INSTANCE_TASKLET_THREAD` and `_SLEEP` keep their meaning; the sleep path flushes the pending wakes first; scheduler threads `rte_thread_register` | engine.md |
| stop | pauses: the RX join and flow rule stay until close or a flow update | contract.md |
| re-open | EAL survives the last close, so open works again (#1341); a legacy `mtl_uninit` with unified objects open returns `-EBUSY` | contract.md |
| before start | TX acquire and submit are accepted in CREATED, STOPPED and ARMED | contract.md |
| single-leg link loss | the session stays RUNNING, units DROPPED (`LINK_DOWN`), `MTL_EVENT_PORT_LINK` (MS3), resume on link up | contract.md |
| processes | one process per instance; no DPDK secondary processes | deployment.md |
| create | reserves queues, quota and flow-rule capacity; rules and joins at the first start | contract.md |
| back-pressure | `status.blocked_on` explains every stall (BUFFERS, RESULTS, APP_LEASES) | contract.md |
| idle cleanup | the transmitter calls the non-blocking `rte_eth_tx_done_cleanup` on idle sessions with frames in flight, only in frame state `WAIT_FRAME`, rate-limited, dedicated queues only (spike S6) | engine.md |
| direct pools | `MTL_SESSION_REQUIRE_DIRECT` with too few slots fails (`POOL_TOO_SMALL`): 2 slots, 3 under 512 packets per unit | contract.md |
| no locked TAI | timed sessions run on `MTL_TIME_SOURCE_SYSTEM_TAI`, labelled estimated, with a warning event (MS3) | timing.md |
| lip-sync offset | `tx.index_offset` in whole units, besides `sc.media_time_offset_ns` | timing.md |
| pacing | per port; a session requests a class (`MTL_OPT_PACING`) and reads the granted one (`info.pacing_class`) | timing.md |
| submission order | FIFO with strictly increasing media time | timing.md |
| sent times | software estimates (`MTL_TXR_ESTIMATED`) first, NIC timestamps (`MTL_TXR_SENT_HW`) later, reported apart from the admission verdict | timing.md |
| late rows | `MTL_ROWS_TRUNCATE` by default, `PAD` opt-in, `STALL` legacy only (MS2) | timing.md |
| ST 2022-7 RX | reports the tolerated path differential (`info.tolerated_skew_ns`); `rx.skew_budget_ns` class A (10 ms) where memory allows; relock and slot count D-133 | timing.md |
| sender type W | the linear read schedule of ST 2110-21:2022; legacy behind a flag (D-24); `MTL_SENDER_NL` explicit | timing.md |
| user pacing | legacy nearest-epoch stays in the shim; unified `MTL_SUBMIT_NOT_BEFORE` moves TX by ≈ 604–619 µs at 1080p59.94 | migration.md |
| first late unit | with `min_tx_delay_ns` set, the first late unit raises `MTL_EVENT_TIMING_INFEASIBLE` (MS3) | timing.md |
| windowed stats | maxima over 1 s and 60 s, and log2 histograms | contract.md |
| timing parser | opt-in (`rx.timing_parser`), integer math; without NIC timestamps on processing time as today, in-burst packets in `tp.untrusted_pkts` | timing.md |
| flow headers | DSCP 0 = the profile's (ST 2110: CS0), TTL 0 = 64; VLAN and IPv6 reserved | contract.md |
| atomics | C11 atomics in `lib/`; public headers C99 and C++ clean | engine.md |
| samples | about six canonical samples plus an event loop, each also on the null backend in CI (G-97) | examples.md |

## 5. Open issues for the implementation

The accepted resolution of each issue an implementer must still apply, in code, a header comment
or a named document. A resolved issue leaves the table; its ID is not reused.

### 5.1 Instance, lifecycle, time sources

| ID | Rule | Milestone |
|---|---|---|
| OI-2 | On a bridged instance, create works at any time and a start before the legacy `mtl_start()` is `-MTL_EBUSY` (`WRONG_STATE`): no tasklet runs before it (`dev/mt_dev.c:2113-2125`); RxTxApp and KahawaiTest start it first | MS1 |
| OI-3 | The legacy teardown order is fixed too, as a bugfix (D-24): `mt_sch_mrg_uinit` releases lcores before it frees active schedulers (`mt_sch.c:1022-1030`); SP-01 | MS1 |
| OI-7 | `CLOCK_NOT_OWNED` (614) is `PTP_BUILTIN` on a PF with `time.phc_trust` = `MTL_PHC_TRUST_YES`; MTL cannot see a node ptp4l itself ([deployment.md §4.8](deployment.md#48-time-in-a-pod)) | MS1 |
| OI-11 | Generated session names are `<essence>_<tx or rx>_<n>`, stated at `mtl_session_config.name` | MS1 |

### 5.2 Data path, results, memory

| ID | Rule | Milestone |
|---|---|---|
| OI-23 | In pods the node agent that hands over `port.xsk_map` sets and resets the AF_XDP `tx_maxrate` (a rate request in the MtlManager protocol), else software pacing; today the library writes sysfs (`dev/mt_af_xdp.c:867-874`) | MS3 |

### 5.3 RX

| ID | Rule | Milestone |
|---|---|---|
| OI-25 | Zero fill is for library pools only; an attached pool is never filled: its unit is `MTL_RX_INCOMPLETE` with the loss counts of `mtl_rx_get_detail`, as legacy reports counts; the `MTL_SESSION_RX_NO_FILL` comment says so | MS2 |
| OI-26 | The RX binding copies the reassembly slot's packet bitmap into the slot table (≈ 540 B at 1080p) through `st_engine_core.h` (D-125): the engine clears it at reuse (`st_rx_video_session.c:1298-1299`); until then no lost ranges are reported | MS2 |

### 5.4 Headers

| ID | Rule | Milestone |
|---|---|---|
| OI-40 | Legacy counters without a key (U-402): a key for each one RxTxApp or the gtests read; the debug-only ones listed as "no key" in migration.md §4.14 | MS3 |
| OI-41 | A non-hugepage import's IOVA is its VA where the IOMMU allows, not the range from `0x10000` (`mt_dma.c:21`) | MS2 |
| OI-42 | U-348: byte-layout structs of the RFC 4175 pixel groups in `mtl_format.h`; U-132: `unit.cookie` is the only cookie | MS4 |
| OI-43 | Plugins and framework elements that share an instance leave `lcores` and the port queue counts at 0 and use `session.tx_queue` and `session.migrate` ([contract.md §2.3](contract.md#23-shared-instance)); there are no per-session queue-count keys, so a second element's shared open does not fail with `INSTANCE_MISMATCH` | MS6 |
| OI-44 | Nanosecond ties in the epoch math keep today's rounding (ties down, `st_muldiv_u64_round_closest`), pinned by a legacy-gate unit test | MS3 |
| OI-45 | ANC and fastmeta TX get the video path's hang detection | MS6 |

### 5.5 Further issues

| ID | Rule | Milestone |
|---|---|---|
| OI-49 | A second `mtl_instance_open` of the same port set without `MTL_INSTANCE_SHARED` is `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | MS1 |
| OI-50 | Migration reports a move: an event (scheduler index, busy %) and `info.sched_index` as a gauge | MS3 |
| OI-54 | G-83 and G-87 name existing reasons (`HOLD_REQUIRED` 219, `LAYOUT_MISMATCH` 212, `GRID_MISMATCH` 225) | MS2 |
| OI-56 | A muted TX unit is `MTL_TX_DROPPED` (`LEG_DISABLED`), counted only in `tx.units_muted` | Phase 7 |
| OI-59 | Video binding memory: TX library pools use the engine's own framebuffers in MS1, RX uses `query_ext_frame` with core-allocated slots from MS1 (D-126); from MS2 every TX slot is an ext frame carrying its page table (PA mode builds them for engine frames only, `st_tx_video_session.c:245-266`) | MS1, MS2 |
| OI-60 | MS1 converts in the caller (submit, dequeue) for the formats st20p converts today; converter plugin threads join in MS2 (D-100) | MS1 |
| OI-61 | The engine fixes a session's linesize at create (`st_tx_video_session.c:3333-3339`, RX `:3343-3351`), but an attached pool brings its layout at attach. Choose: create the engine session at the first start for `MTL_SESSION_POOL_ATTACHED` (create still validates through query), or add a set-linesize engine call (MF9) | MS2 |
| OI-62 | A flow or media update that re-creates the engine session restarts the RTP sequence at 0: `st20_tx_ops` has no seed. Choose: a seed field in the engine ops, or never re-create on update | MS5 |

### 5.6 Raised by the user review, awaiting the maintainer

Each item has the recommended rule; the maintainer confirms or changes it by the point named
("decide by"), and the item then moves, as a row, to the sections above.

- **OI-63** (MS1 (C1b); decide by C0 (checkpoint 1)). Two threads on one session can lose a wake-up:
  a timeout-0 call of thread B drains the shared eventfd after a completion's write and before
  thread A, armed for another target, reaches `poll()`. Recommended: WT calls block on a futex over
  the session's publication word (`WaitOnAddress` on Windows), not on the eventfd; the eventfd
  serves wait-handle users only, and one thread drains a handle (a contract rule); the flush wakes
  both kinds; G-52 gains a two-thread litmus test
- **OI-64** (MS1; decide by H1a (checkpoint 1)). A function of a later milestone fails only at link
  time, and a SWIG binding that wraps every declaration fails at import. Recommended: `MTL_SINCE(n)`
  before each declaration, expanding to `__attribute__((unavailable))` with the milestone in its
  message when the installed library's level (a generated `MTL_LEVEL`) is below n; the Python
  binding skips them
- **OI-65** (MS7; decide by MS6). Every option key freezes at `MTL_1.0`, including engine internals
  (`instance.tasklet_time_measure`, `instance.memzone_max`, `port.virtio_user`,
  `port.tx_no_burst_check`). Recommended: a stability tier in `mtl_option_desc` (stable or
  provisional); only stable keys freeze; provisional keys may change at any release
- **OI-66** (MS2a, MS6; decide by S1 (MS2a)). No shared completion queue in v1: a 64-stream gateway
  has 64 eventfds, and the flush writes up to one per armed session per scheduler iteration.
  Recommended: spike S1 measures 64 and more armed sessions per scheduler; if pacing suffers, W3 is
  built and `mtl_queue_*` is pulled into MS6
- **OI-67** (MS2b; decide by MS2a). Framework buffers. `mtl_rx_provide` lacks rules (consumption
  order, mapping a unit back to its buffer, no buffer provided, rows units, stop); FFmpeg zero copy
  needs contiguous planes and zeroed tail padding. Recommended: FIFO consumption; a `cookie` in
  `struct mtl_attach` returned in the unit; with none provided the library pool serves; a flag that
  requests contiguous planes and 64 B of zeroed tail
- **OI-68** (MS4a; decide by MS3). Live A/V phase: TAI-mode video snaps to the frame grid (up to
  half a frame), audio does not, so separate sink elements drift apart by the start phase.
  Recommended: a TAI-mode session can take the phase of another (`tx.phase_of` = a session), so
  audio applies the video's snap offset
- **OI-69** (MS2a; decide by MS1 exit). Framework helpers: the log sink has no origin object and is
  process-wide; option enum names assume dense values; no `key=value` parser for properties and
  AVDictionary; ports are set only through `MTL_PORTS`. Recommended: the sink callback gets `const
  struct mtl_object* origin` and several sinks may be set; names for sparse values; inline
  `mtl_option_parse()` and `mtl_port_parse()`
- **OI-70** (MS1–MS7; decide by checkpoint 1 for the MS1 functions, MS6 for the rest). Polish before
  the freeze. Recommended: one index convention or inline helpers for the index + 1 fields
  (`flow.port`, numa, option scope, `mtl_time_set_reference` port); `mtl_interrupt` takes a
  `uint64_t` target mask argument; `MTL_RETIRING` names close's 1 and the close wrappers warn on
  unused results; final names for `mtl_version_string`, `enum mtl_simd`, `enum mtl_log`; rename
  `mtl_tx_next_slot` (slot means a buffer) before its export; anything that reaches the SDP
  (`tx.troffset_ns`, `tx.tsmode`) becomes a typed field; a cookie without results is ignored, not
  `-MTL_EINVAL`
- **OI-71** (MS1; decide by B1). Interlaced rasters: legacy `fps` is the field rate, so a
  field-by-field port of 1080i59.94 asks for 119.88 fields per second. Recommended: an interlaced
  raster above 60 frames per second is `-MTL_EINVAL` with a reason that names the field rate
- **OI-72** (MS1; decide by A1). The bridge: what `mtl_time_now` reports on a wrapper and what
  instance options do there. Recommended: the wrapper reports the legacy time base (built-in PTP
  locked or not, `ptp_get_time_fn` as USER) with its flags; instance options on a wrapper are
  `-MTL_EBUSY` (`OPTION_STATE`) except the R keys
- **OI-73** (MS3; decide by MS2b). Format detection: how the pool is sized before the format is
  known, what units hold before detection, what an application does on `FORMAT_CHANGED`.
  Recommended: the pool is sized for `video.detect_max` (a raster, default 2160p); no unit is
  published before detection; `FORMAT_CHANGED` stops publishing until the application updates or
  re-creates
- **OI-74** (MS1; decide by A2a). `latency_min_ns` and `latency_max_ns` in `mtl_session_info` have
  no definition. Recommended: never negative; RX min = one unit period + `rx.flush_offset_ns` + the
  conversion estimate, max = `pool_count` × period; TX min = max(0, `min_submit_lead_ns`), max = the
  1 s horizon
- **OI-75** (MS7; decide by MS3). Dates: target releases for MS7 and the hiding stages F, F+1 and
  F+2, and the support policy after F+2 (fixes to the frozen legacy API, backports)
