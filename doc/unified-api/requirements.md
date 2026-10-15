# Requirements: every catalogue and where it is met

| | |
|---|---|
| Status | Reference for the unified API. Nothing is implemented; the headers in [sketch/](sketch/) are normative, and where this page and a header disagree, the header wins |

This file is the maintainers' statement of what the API must satisfy: goals, non-goals, personas,
the core requirements R-* and R-PKT with the guarantees that close them, and the Kubernetes
(K-REQ), NMOS (N-REQ) and IPMX (I-REQ) catalogues. The guarantees are
[guarantees.md](guarantees.md). A row says what is asked (with the clause it comes from), MTL's
part, the main symbols that meet it, when, and the guarantee that checks it. The design behind a row is in [contract.md](contract.md),
[timing.md](timing.md), [core.md](core.md), [engine.md](engine.md), [deployment.md](deployment.md) or
[nmos-ipmx.md](nmos-ipmx.md).

## 1. How to read the tables

| Column | Meaning |
|---|---|
| Part | **none**: the application, the NMOS Node or the host does it. **API**: MTL exposes a value, a call or a rule; the work is small. **engine**: the engines change behaviour ([engine.md §6](engine.md#6-the-engine-change-list)) |
| How met | a header symbol (`mtl.h` unless named), an option key (`mtl_options.h`), a stat key ([contract.md §11.3](contract.md#113-key-catalogue)) or a reason (`mtl_reasons.h`); a G-ID in parentheses is the test |
| When | a milestone MS1–MS7 ([implementation-plan.md §1.2](implementation-plan.md#12-the-milestones)); **v1**: in the API frozen at MS7, with its essence (video MS1–MS3, the others MS4); **7**: Phase 7, after MS7, declared now so the API does not change; **later**: beyond Phase 7; **—**: not MTL's |
| Levels | MUST, SHOULD, MAY. R-*: MUST is a v1 blocker, SHOULD is v1 unless it costs a milestone, MAY reserves the shape. N-REQ: MUST means the Node cannot meet a normative NMOS or SMPTE rule without it. I-REQ: the TR keyword; "MUST if X" applies when the optional feature X is built |

A row without a guarantee is not promised ([implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence)).
Short names: IS-04…IS-12 AMWA NMOS specifications; BCP-004, -006, -008 AMWA best current practices;
TR-10-n VSF IPMX technical recommendations; PQCR the IPMX product qualification requirements.

## 2. Goals, non-goals and the quality bar

### 2.1 Goals

[concepts.md §1.1](concepts.md#11-who-it-is-for-and-what-it-is-not) links here.

| ID | Goal | Met by | When |
|---|---|---|---|
| GO-1 | one session model for ST 2110-20, -22, -30, -40, -41, TX and RX; media-specific configuration only at creation; media-polymorphic runtime verbs | `mtl_session_config` with one essence member, one verb set ([contract.md §3](contract.md#3-session-configuration)) (G-45) | video MS1; others MS4 |
| GO-2 | a familiar shape: acquire → fill → submit → result on TX, dequeue → read → release on RX; a queue for many sessions; regions; a cookie per unit; requested versus granted; as in libfabric and Rivermax | acquire, submit, `mtl_reap`, dequeue, `mtl_release`; `mtl_queue_*`; `mtl_mem.h`; `unit.cookie`; `mtl_session_query` (G-46, G-47, G-34) | MS1; regions MS2; queues MS2a; events MS3 |
| GO-3 | one buffer contract: library pool, imported memory and (later) device memory are the same pool slot with the same lifecycle; the allocation origin never implies a data path | library pools, `mtl_session_attach`, `mtl_mem_import`, `MTL_MEM_DEVICE`, `MTL_SESSION_REQUIRE_DIRECT` ([contract.md §9](contract.md#9-memory)) (G-13, G-15) | MS1; attached MS2, MS4; device memory MS6 |
| GO-4 | timing correct by construction: media time and launch time apart; RTP from media time only; one exact rational epoch shared by essences on TX and RX; late handling an explicit policy with a per-unit result | `unit.media_tai_ns`, media modes, `min_tx_delay_ns`, `MTL_WHEN_ORIGIN`, `mtl_sync.h`, `tx.late_policy` ([timing.md](timing.md)) (G-19, G-26, G-28, G-76) | MS1; ST20 subset MS3; MS6 |
| GO-5 | nothing happens silently: one terminal result per accepted unit with status, reason and timing; every downgrade reported as requested versus granted; every state change an event with a state getter | `struct mtl_tx_result`, `mtl_session_info`, `MTL_EVENT_PACING_CHANGED` ([contract.md §10](contract.md#10-events-and-queues)) (G-01, G-13, G-41) | results MS1; events MS3 |
| GO-6 | real-time safety: no public call on a pinned tasklet core; tasklets never block, allocate, take a lock an application thread can hold, or run application code, not even by option (D-04; the `MTL_LATER` inline notify is the one reserved exception) | rule R6; the engine callbacks of the bindings ([engine.md §1](engine.md#1-the-pinned-core-rules)) (G-38, G-39) | measured MS1; P at MS4 |
| GO-7 | a robust lifecycle: per-session states, drain versus flush, interruptible waits, stale-handle safety, no application code and no memory access after a close retires, errors as codes | `enum mtl_state`, `MTL_STOP_DRAIN`, `MTL_STOP_FLUSH`, `mtl_interrupt`, R4, `mtl_last_error` ([contract.md §4](contract.md#4-lifecycle-and-states)) (G-29…G-31, G-49, G-64, G-65) | MS1 |
| GO-8 | an evolvable ABI: versioned structs, opaque handles, soname and symbol versions, an experimental tier | R3, R4, libmtl's version nodes `MTL_UNIFIED_EXPERIMENTAL_<rev>_MSn`, `MTL_SIZE_CHECK` (G-33, G-51, G-73, G-74) | MS1; the libmtl soname and `MTL_LEGACY` node MS3; `MTL_1.0` MS7 |
| GO-9 | engines first: today's packet builders and RX reassembly are reused; engine fixes land first and reach legacy users, bugfixes on by default, wire-visible changes opt-in; legacy APIs keep working | [engine.md §6](engine.md#6-the-engine-change-list); the legacy gate ([implementation-plan.md §8.5](implementation-plan.md#85-the-legacy-gate)); D-24 (G-99, G-106) | every milestone |
| GO-10 | every RTP-level use case of today (§8) has a unified equivalent | `MTL_UNIT_PACKETS` on every essence, the `MTL_RTP` essence ([contract.md §13](contract.md#13-packet-units)) (G-PKT-1…7) | MS5 |

### 2.2 Non-goals of the first version

Several reserve the shape now, so they can come later without an ABI break.
[concepts.md §1.1](concepts.md#11-who-it-is-for-and-what-it-is-not) links here.

| ID | Non-goal | Why | Standing |
|---|---|---|---|
| NG1 | replacing the packet builders and RX reassembly; pacing admission, RTP derivation, the time source and the completion plumbing **are** re-plumbed (E1–E15) | they work; today's defects are in the plumbing | holds; wire-visible engine changes (RTP ±1 tick, default video RTP, ANC RTP, ST 2110-22 CBR, the linear read schedule) are opt-in on the legacy API and on in the unified API (D-24) |
| NG2 | (dropped) application-built RTP inside the unified API | — | replaced by GO-10: packet units and `MTL_RTP` (D-82, MS5) |
| NG3 | SDP parsing inside `lib/` for MTL's own use | parser edge cases; NMOS users have tooling; `mtl_session_get_info` returns every SDP value | holds; SDP render and parse (`mtl_sdp.h`, the companion library libmtl_sdp) are helpers on public calls only (Phase 7, D-94) |
| NG4 | DMA-BUF, CUDA, Level Zero *direct* NIC access | no such path exists; GPU-pinned *host* memory imports | `MTL_MEM_DEVICE` of `mtl_mem_open` is declared, `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`) until built |
| NG5 | multi-process session sharing, DPDK secondary processes | EAL runs `--in-memory`; processes sync through the SMPTE epoch | one process per instance; `mtl_epoch_index_at` ([timing.md §10.4](timing.md#104-separate-processes)) |
| NG6 | RX header split | compiled out on the pinned DPDK | not in the unified API; a cut candidate of the coverage gate (D-87) |
| NG7 | library-scheduled RX delivery ("present at T") | report the presentation time first | `mtl_rx_detail.presentation_tai_ns` with `rx.link_offset_ns`; scheduling later |
| NG8 | runtime `mtl_port_open` | needs a port parameter struct and the list of subsystems it brings up | no header symbol; after MS7, on request |

### 2.3 Personas and the quality bar

Each persona has one thing the API must make excellent, not merely possible; several rules only
make sense per persona (the sticky interrupt for P2, `MTL_SESSION_RX_BY_INDEX` for P5, the capacity
query for P9). The external review with the named consumers before the MS7 freeze validates them.

| ID | Persona | Must be excellent at | Uses today |
|---|---|---|---|
| P1 | simple generator or player (sample apps, test tools) | a short loop with one config, a library pool and results off ([ex01](sketch/examples/ex01_tx_video.c)); one-call sends in `mtl_util.h` | st20p get and put, blocking |
| P2 | framework integrator (FFmpeg, GStreamer, OBS, Python, Rust) | interruptible waits, a queue for many sessions, pool import and export, clean errors, a latency report, a query before create | pipeline, blocking get, copy |
| P3 | broadcast playout (file to ST 2110, A/V/ANC together) | exact RTP for every essence from one epoch, an atomic start, a deterministic late policy | user timestamp and pacing, ST 2022-7 |
| P4 | live capture and contribution (camera, SDI to IP) | latency visibility, `mtl_tx_get_next`, rows units, a drop policy | ext frames, slice mode, callbacks |
| P5 | zero-copy forwarder or GPU pipeline (MXL bridge, split forward) | regions with a defined lifetime, RX release in any order, holds, `MTL_SESSION_REQUIRE_DIRECT` | `query_ext_frame`, `put_ext_frame`, `mtl_dma_map` |
| P6 | Rivermax migrant | an application-driven loop, chunks as packet units, regions, non-blocking back-pressure | `rmx_output_media_*`, `rmx_input_*` |
| P7 | libfabric developer | handles, a result ring per session, a cookie per unit, requested versus granted, `-MTL_EAGAIN` | `fi_mr`, `fi_cq`, `fi_eq`, `fi_getinfo` |
| P8 | validation and compliance (KahawaiTest, pytest, EBU LIST users) | scheduled versus actual times, per-unit status, exact arithmetic, the null backend, fault injection | everything |
| P9 | operator or NMOS integrator | an atomic multi-leg update, leg admin state (`sc.legs_disabled`), stable names, enumeration, a capacity query | `update_destination`, the stats dump |

Quality bar: a milestone is done only when every MUST in its scope has a contract test at the
cheapest tier that can observe it (P and BE: [implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence));
the success criteria are the exit criteria of [implementation-plan.md §5.6](implementation-plan.md#56-exit-criteria)
and [§6](implementation-plan.md#6-ms2ms7). The case for the API is [concepts.md §1](concepts.md#1-why-a-new-api).

## 3. Core requirements R-*

### 3.1 The catalogue

IDs are stable. "Closed by" names the guarantees that close each requirement
([guarantees.md](guarantees.md)) and "When" the milestone that closes them; the rule text lives in [contract.md](contract.md), [timing.md](timing.md), [core.md](core.md) or
[engine.md](engine.md).

| ID | Level | Requirement | Closed by | When |
|---|---|---|---|---|
| R-OBJ-1 | MUST | one opaque session type (`mtl_session_h`) for every essence and direction; essence fields in the one typed config | G-45 | video MS1; G-45 MS4b |
| R-OBJ-2 | MUST | direction verbs: TX `mtl_tx_acquire`, `mtl_tx_submit`, `mtl_tx_release`; RX `mtl_rx_dequeue`, `mtl_rx_release`; no verb names opposite ownership moves in the two directions | G-46 | MS1 |
| R-OBJ-3 | MUST | a buffer is a pool slot whose layout is fixed at attach; per-use values (media time, cookie, overrides) live in `struct mtl_unit`, which acquire zeroes | G-12, G-48 | MS2b (video), MS4a and MS4b (other essences) |
| R-OBJ-4 | MUST | every unit carries a 64-bit cookie returned verbatim in its result | G-47 | MS1 |
| R-OBJ-5 | MUST | typed handles are validated without touching freed memory; the full rule is below the table (R4) | G-07, G-29, G-71 | MS1 |
| R-OBJ-6 | SHOULD | one event loop serves many sessions: a queue shared by many sessions and the instance (`mtl_queue_h`, MS2a) reports through one descriptor what its armed objects have ready; the instance's events are read on the instance (`mtl_read_events`) | G-52, G-95 | per-session waits MS1; queues MS2a; instance-wide events MS3 |
| R-MEM-1 | MUST | the library pool is the default and needs no memory knowledge | G-15 | library pools MS1; G-15 MS2b (video) |
| R-MEM-2 | MUST | application memory is imported once as a region with page-aligned `va` and `length` (hugepage-aligned for hugetlbfs), so no neighbouring memory is mapped; slots reference the region, never raw IOVA | G-10, G-11, G-96, G-101, G-102 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-3 | MUST | a region outlives every reference: `mtl_mem_close` consumes the handle and returns 1 while referenced; `MTL_EVENT_REGION_RELEASED` reports the end | G-10, G-11 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-4 | MUST | an import maps into every device on the session's path (both legs, the DMA engine) or fails | G-16 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-5 | MUST | the data path is independent of the allocation origin: direct where possible and reported (`MTL_TXR_COPIED`, `MTL_INFO_DIRECT`), per session and per unit; `MTL_SESSION_REQUIRE_DIRECT` fails instead of copying; any stride ≥ row bytes is direct-capable | G-13, G-14, G-84 | MS2b (video), MS4a and MS4b (other essences) |
| R-MEM-6 | MUST | fixed pools, attached before start | G-35, G-36, G-103 | G-103 MS1; the rest MS2b |
| R-MEM-7 | SHOULD | imported memory for every essence | G-15 | MS2b (video); every other essence with its binding (MS4a, MS4b) |
| R-MEM-8 | SHOULD | RX leases held across threads and released in any order; one RX lease held by N TX units (`unit.hold`); an explicit exhaustion policy | G-56, G-60, G-83, G-137 | G-56 MS1; holds and G-137 MS2b |
| R-MEM-9 | MAY | mixed-origin pools, device memory (`MTL_MEM_DEVICE`, MS6), per-acquire layouts (`mtl_tx_acquire_layout`, `mtl_rx_provide`, MS2) | G-137 | per-acquire layouts and G-137 MS2b; device memory and mixed-origin pools MS6 |
| R-TIME-1 | MUST | every time field names its clock, unit and epoch: TAI ns on the instance clock (R5), valid by its flag (`MTL_TXR_*`, `MTL_UNITF_*`, `MTL_RXF_*`), never by zero | G-17 | the ST20 fields MS1–MS3; the rest MS6 |
| R-TIME-2 | MUST | media time and launch are separate inputs; launch derives from media time and `min_tx_delay_ns` unless overridden | G-18, G-59 | G-59 MS1 (UB; M tier MS6); G-18 MS6 |
| R-TIME-3 | MUST | RTP = `floor(M × R) mod 2^32`, exact, one rule for every essence and both fields | G-19, G-21, G-22 | video MS3 (MS2a if task X is written in MS1); G-21, G-22 MS6 |
| R-TIME-4 | MUST | the default video media time is the frame epoch | G-20 | MS1 (video), MS4a (the ST40 half) |
| R-TIME-5 | MUST | late and underrun policies with per-unit outcomes; an invalid request is `-MTL_ERANGE` with its reason, never another pacing mode (`caps.pacing_req` = `MTL_REQ_REQUIRE`); ST40 and ST41 keep-alive by default | G-26, G-28, G-61, G-66, G-85, G-86 | G-26 with EXACT (B3, else MS2a), the rest of G-26, G-28, G-85 MS3; G-61 MS4a; G-66 MS4b; G-86 MS6 |
| R-TIME-6 | MUST | the instance exposes its time source, lock state and offset (`time.state`, `time.offset_ns`), with `MTL_EVENT_TIME_STATE` and `MTL_EVENT_TIME_STEP` | G-54, G-62 | events MS3; G-54, G-62 MS6 |
| R-TIME-7 | SHOULD | every session on the SMPTE epoch with an exact rational index period; an all-or-none start array (`mtl_session_start(s, n, when, &t0)`) with T0 on the common grid, and `MTL_WHEN_ORIGIN` to count the media indices of its sessions from T0; created timelines with their own anchors are reserved for later (`MTL_LATER`) | G-23, G-24, G-63, G-68, G-94, G-104 | MS3, MS6 (below) |
| R-TIME-8 | SHOULD | sample-accurate audio submission | G-21 | MS6 |
| R-TIME-9 | SHOULD | `mtl_tx_get_next` gives the next index and the latest submit time that meets it | G-53 | MS3 |
| R-TIME-10 | SHOULD | RX reports unwrapped media time (`unit.media_index`, `media_tai_ns`), per-leg first and last arrival (`mtl_rx_detail`) and an optional link offset (`presentation_tai_ns`) | G-25, G-67, G-82 | G-82 MS2a; the rest MS6 |
| R-TIME-11 | MUST | progressive rows (`MTL_UNIT_ROWS`, slice mode): TX rows leave as the application raises `used`; RX reports rows as they arrive (`mtl_rx_wait_rows`); legacy slice-level users have a home | G-113 | MS2a |
| R-TIME-12 | SHOULD | RX reports `media_index` on the epoch; video, audio and ANC align without application arithmetic (`mtl_rx_align`) | G-76, G-105 | video G-76 MS3; the rest MS6 |
| R-TIME-13 | SHOULD | processes align without IPC: every session on the SMPTE epoch, and `mtl_epoch_index_at` | G-76, G-105 | MS6 (G-105) |
| R-CMP-1 | MUST | every accepted submission has exactly one terminal outcome (a result, or a counter with results off); a failed first submit has none and returns the slot to the pool, except `-MTL_EBADF` and `-MTL_ESTALE`, which change no state (D-88) | G-01, G-02 | MS1 |
| R-CMP-2 | MUST | results are never lost: producing a result never waits for ring space (the ring and acquire's reservation: [contract.md §5.5](contract.md#55-back-pressure); [core.md §4](core.md#4-lease-table-and-result-materialisation)) | G-04, G-05 | MS1 |
| R-CMP-3 | MUST | TX statuses ON_TIME, DROPPED (with reason), FLUSHED, FAILED, and LATE for a bounded late send (Phase 7, `MTL_LATER`); `enum mtl_tx_status` starts at 1, so 0 is never terminal | G-28, G-57 | the MS1 statuses MS1; G-28, G-57 MS3 |
| R-CMP-4 | MUST | "reusable" means nothing (converter, packet, DMA, NIC) can still reach the storage | G-06, G-100 | MS2b |
| R-CMP-5 | MUST | events in a bounded ring per session and per instance, apart from results, coalesced, overflow counted, a getter per state event | G-41, G-58 | MS3 |
| R-CMP-6 | MUST | one error vocabulary: `MTL_E*` with fixed values and one meaning each; `mtl_last_error` | G-30, G-38, G-57, G-70 | G-30, G-70 MS1; G-57 MS3; G-38 MS4b |
| R-CMP-7 | SHOULD | a portable queue descriptor (`mtl_queue_create`, MS2a); probes arm nothing: a data call with timeout 0 changes no wait state (R2) | G-52 | MS2a |
| R-OBS-1 | MUST | one stats schema: cumulative counters that never reset, separate gauges; reads take no lock a tasklet takes | G-40, G-42 | G-42 MS2a (A2b); G-40 and per-scheduler stats blocks MS3 |
| R-OBS-2 | MUST | queue gauges per slot state, from one scan | G-43, G-55 | G-43 MS3; G-55 MS6 |
| R-OBS-3 | MUST | time, link, leg and scheduler status getters, each with a change event | G-41, G-54, G-91 | G-41 MS3; G-91 MS5; G-54 MS6 |
| R-OBS-4 | SHOULD | lateness histograms (`MTL_STAT_HIST`, log2 buckets); maxima over the last 1 s and 60 s (`{window=1s\|60s}` keys) instead of max-since-reset | G-43, G-55 | MS6 |
| R-OBS-5 | SHOULD | logs carry the instance, session and name; tasklets write formatted, length-capped lines into a per-scheduler ring that drops when full, and a library thread hands them to the sink; no fact is only in a log | G-44, G-88, G-134, G-141 | G-88, G-141 MS1; G-134 MS2a; G-44 MS6 |
| R-THR-1 | MUST | no public function runs on a tasklet; no application code on tasklets (D-04); an application thread may busy-poll the DP calls and the WT calls with timeout 0 | G-38 | measured MS1; P in MS4b |
| R-THR-2 | MUST | the tasklet ↔ application hand-off is lock-free; no tasklet waits on anything an application thread can delay | G-39 | measured MS1; P in MS4b |
| R-THR-3 | MUST | every function has one call class (`MTL_API_CP`, `_DP`, `_DPC`, `_WT`, `_AS`), enforced in debug builds | G-50 | MS3 |
| R-THR-4 | MUST | heavy per-unit work never runs on a tasklet, and where it runs is reported; the one exception is `MTL_OPT_RX_CONVERT_PER_PACKET` (library code) | G-14, G-39 | MS1; G-14 MS2b |
| R-THR-5 | SHOULD | waking a sleeping thread costs a tasklet handler no syscall: it sets a bit, and a scheduler loop makes at most one wake per marked object or queue, a bounded number per iteration; a notifier thread can take the writes off the pinned core if they harm pacing (D-165) | G-39, the waker budgets of implementation-plan.md §8.4 | MS1, MS2a (below) |
| R-LIFE-1 | MUST | states CREATED, ARMED, RUNNING, DRAINING, FLUSHING, STOPPED, ERROR, CLOSING, RETIRED; start and stop reversible; discard without stop; close from any state, idempotent (a repeated close polls) | G-35, G-49, G-80 | G-49 MS1; G-80 and discard MS2a; G-35 MS2b |
| R-LIFE-2 | MUST | stop DRAIN and FLUSH leave every accepted unit with a terminal outcome | G-31 | MS1 |
| R-LIFE-3 | MUST | interrupt and stop wake blocked callers at once, with a distinct code | G-30, G-64 | MS1 |
| R-LIFE-4 | MUST | once a close retires: no application code is called, no imported memory touched, the handle reads `MTL_STATE_RETIRED` | G-29, G-37, G-65, G-77, G-81 | G-29, G-65, G-77 (exclusive open) MS1; the shared instance MS2a; G-37 MS2b; G-81 MS3 |
| R-LIFE-5 | SHOULD | instance re-open within one process (#1341) and after a SIGKILL | G-112 | MS1 (U); MS3 (I, wave 4) |
| R-CAP-1 | MUST | capability query per port and backend (`caps.*`); REQUIRE, PREFER or OFF at create (`MTL_REQ_*`); granted values reported (`info.pacing_class`, `MTL_INFO_DIRECT`) | G-13, G-14, G-34 | granted values MS1; G-13, G-14 MS2b; G-34 MS4b |
| R-ABI-1 | MUST | input structs start with `struct_size` (input only); opaque handles; flags are plain integer literals and unknown bits fail (`UNKNOWN_BITS`) | G-33, G-51, G-72 | MS1 |
| R-ABI-2 | MUST | one library: libmtl carries the unified functions in version nodes per milestone, `MTL_UNIFIED_EXPERIMENTAL_<rev>_MSn` and its parts, a node a release carries never changing, all renamed together on every incompatible change before the freeze, `MTL_1.0` at it; `MTL_API` sets the visibility; a soname and the `MTL_LEGACY` node for libmtl | G-33, G-51, G-72 | MS1, MS3 (below) |
| R-ABI-3 | SHOULD | an experimental tier | G-51 | MS1 |
| R-ABI-4 | MUST | versioned `struct mtl_instance_params`; a shared instance opened again with differing ports, lcores, time source or options is `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | G-77 | MS1 (versioned parameters); MS2a (the shared instance) |
| R-ABI-5 | MUST | C99 and C++17 with `-Wpadded -Werror`; `MTL_SIZE_CHECK` per struct; zero defaults; `MTL_INIT(p)`; bindings zero-fill and set `struct_size` | G-73, G-74 | MS1 |
| R-OPS-1 | MUST | flow changes on every leg commit at one activation (`struct mtl_when`, `mtl_session_update`); on failure nothing changes | G-75 | MS5 |
| R-OPS-2 | MUST | resources reserved for every configured leg regardless of link; admin state settable while RUNNING (`sc.legs_disabled`); oper state from a link monitor (`mtl_leg_status`, `MTL_EVENT_LEG_STATE`) | G-91 | MS5 |
| R-OPS-3 | MUST | a stable, unique session name, copied at create; sessions can be listed | G-88 | MS1 (the listing: MS2a, A2b) |
| R-OPS-4 | SHOULD | remaining capacity of queues, RL queues, scheduler quota, lcores and sessions (`capacity.*`, `port.free_*`) and a dry-run create (`MTL_QUERY_CHECK_CAPACITY`) | G-89 | MS5 |
| R-OPS-5 | SHOULD | a CREATED or STOPPED session reconfigured (`MTL_UPDATE_MEDIA`, `MTL_UPDATE_POOL`), keeping handle, name, flows, SSRC and stats | G-87 | MS3 |
| R-OPS-6 | MUST | MtlManager loss never stalls running sessions; creates that need it fail with `-MTL_EBUSY`, `MANAGER_LOST`, never with a fallback (D-63) | G-98, G-110 | G-110 MS3; G-98 MS5 |
| R-OPS-7 | MUST | create and start never block on ARP or IGMP; per-leg flow state with an event and a getter | G-90 | MS5 |
| R-TEST-1 | MUST | the null backend `null:<n>`: no NIC, root or hugepages; TX → RX loopback within the instance, by flow (destination IP and UDP port) | G-92 | MS1 |
| R-TEST-2 | MUST | the test clock (`MTL_FAULT_TEST_CLOCK`, `MTL_FAULT_CLOCK_ADVANCE` of `mtl_debug_inject`) | G-92 | MS1 |
| R-TEST-3 | MUST | fault injection `mtl_debug_inject`, built only with `-Denable_debug_api=true` | G-93, [implementation-plan.md §8.3](implementation-plan.md#83-fault-injection) | MS1; faults per milestone |
| R-TEST-4 | SHOULD | the I tier pulls a link and resets a VF (`nicctl.sh vf_link`, `vf_reset`) | [implementation-plan.md §8.3](implementation-plan.md#83-fault-injection) | MS5 (`vf_link`); MS6 (`vf_reset`) |
| R-USE-1 | SHOULD | P1's loop: `mtl_session_open` with a library pool and results off (ex01); one-call sends in `mtl_util.h` | G-97 | MS1 |
| R-USE-2 | SHOULD | bindings get stride-explicit reads, copy helpers (`mtl_unit_copy_in`, `mtl_unit_copy_out`, the stride-aware `mtl_unit_copy_plane_in`, `_out`) and a reference Python wrapper | G-72, G-97, the GIL test ([implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence)) | MS6 |
| R-FW-1 | MUST | framework buffers, latency and format changes without a re-create, and without loss when a consumer holds buffers | G-130–G-133, G-138–G-140 | G-130–G-132 MS1; G-133 MS2a; G-138, G-139 MS3; G-140 MS6 |
| R-MIG-1 | MUST | every engine fix reaches legacy users: bugfixes on, wire changes opt-in; the legacy gate at every milestone | G-99, G-106 | every milestone ([implementation-plan.md §8.5](implementation-plan.md#85-the-legacy-gate)); G-106 (E2's legacy opt-in) MS3 |
| R-PERF-1 | MUST | numeric budgets before the code, confirmed by spike S0 | [implementation-plan.md §8.4](implementation-plan.md#84-performance-budgets) | MS1 |
| R-SEC-1 | MUST | the security properties of the new surfaces are stated and tested: import alignment, access, debug-API gating, queue-descriptor ownership ([deployment.md §1](deployment.md#1-threat-model-and-surfaces)) | G-93, G-96 | G-93 MS1; G-96 MS2b |

Rows too long for the table:

- **R-OBJ-5**: typed handles are validated without touching freed memory; stale and foreign
  handles fail with `-MTL_EBADF` and a returned lease (a duplicate submit or release) with
  `-MTL_ESTALE`, changing no state; 0 is the null handle, which fails with `-MTL_EBADF` except in
  close, which returns 0; a closed handle is never reissued; a start array replaces a group handle,
  a slot index a buffer handle (R4).
- **R-TIME-7**, When: G-94 MS3; G-23, G-24, G-68, G-104 MS6 on the epoch timeline; G-63 with
  created timelines, not in v1.
- **R-THR-5**, When: MS1 (no syscall in a completing context); S1 in MS2a decides the notifier.
- **R-ABI-2**, When: MS1 (the experimental node); MS3 (soname, `MTL_LEGACY`, `local: *`).

The Kubernetes requirements K-REQ-1…20 are mapped in [deployment.md](deployment.md); the
lifecycle part MS1 and MS3 build is G-107…G-112.

### 3.2 Requirements without a guarantee

Only R-PERF-1 and R-TEST-4 lack one: they are closed by the budgets of
[implementation-plan.md §8.4](implementation-plan.md#84-performance-budgets) and the fault steps of
[§8.3](implementation-plan.md#83-fault-injection), not by a G-ID.

### 3.3 RTP passthrough: R-PKT-1…5

The exit rules of MS5, in the order video, ANC, fastmeta and `MTL_RTP`; then audio and cvideo;
`MTL_PKT_RX_LEND` last. Rule text: [contract.md §13](contract.md#13-packet-units); engine work:
PE1–PE9 ([engine.md §6](engine.md#6-the-engine-change-list)).

| ID | Requirement | Part | How met |
|---|---|---|---|
| R-PKT-1 | packet units on every essence, plus a generic RTP essence (ST 2022-6, custom payloads) | engine | `sc.unit = MTL_UNIT_PACKETS`, `struct mtl_packet_config`; `MTL_RTP`, `struct mtl_rtp_config` (G-PKT-1, G-PKT-4) |
| R-PKT-2 | the library writes only L2–L4 and the RTP fields the session declares; verbatim by default | engine | `packet.set_fields` (`MTL_PKT_SET_TIMESTAMP`, `_SEQ`, `_SSRC_PT`, `_MARKER`; 0 = verbatim); `MTL_PKT_TX_VALIDATE` counts, never fixes (G-PKT-1, G-PKT-2) |
| R-PKT-3 | one result per chunk | API | the unit is a chunk; `MTL_SUBMIT_UNIT_END` ends a frame or field; `MTL_TXR_PKT_SHORT` (G-PKT-3) |
| R-PKT-4 | no DPDK mbuf is exposed and no application code runs on a tasklet | engine | chunks are pool slots with a packet table (`struct mtl_pkt_tx`, `struct mtl_pkt_rx`); RX copies in the caller by default, `MTL_PKT_RX_LEND` lends NIC buffers |
| R-PKT-5 | RX holds a bounded number of NIC buffers | engine | `packet.rx_ring_packets` (512); `MTL_PKT_RX_LEND` for zero copy within it; reason `RX_RING_BUDGET` (G-PKT-5) |

## 4. Guarantees

Every guarantee G-xx and G-PKT-n, with its tier, P or BE, milestone and test, is in
[guarantees.md](guarantees.md).

## 5. Kubernetes: K-REQ-1…20

What MTL must do inside a Kubernetes pod. The design is [deployment.md §4](deployment.md#4-kubernetes)
(sections named per entry); the engine fixes EK1–EK21 are [engine.md §6](engine.md#6-the-engine-change-list)
and run on the Kubernetes track beside the milestones, done by the MS3 exit
([implementation-plan.md §6.10](implementation-plan.md#610-the-kubernetes-track)); today's pod hazards H-K-n are [engine.md §7.3](engine.md#73-pod-hazards-h-k).
Each entry: level; part; when; test.

- **K-REQ-1 bounded orderly shutdown** (MUST; API, engine; MS1, report MS3; G-107). One call stops
  a whole instance within a caller-given deadline: TX drained to a frame boundary, the rest
  flushed, every session retired, MtlManager released, devices closed; the typical cost fits well
  inside the default 30 s grace period after a `preStop` hook. Met by `mtl_instance_close(mt,
  timeout_ns)` (0, 1 or `-MTL_EIO`) and `mtl_instance_shutdown` with its report; by default the
  unit on the wire finishes and the rest is flushed; EK1, EK2, EK21 (§4.2).
- **K-REQ-2 network first** (MUST; engine; MS3; G-108). Within the budget: TX stops at the next
  frame boundary, IGMP leaves on every leg, MtlManager resources released, then device and memory
  teardown, which may be cut short; the first two in about one frame time plus a few ms. Met by the
  step order of §4.2, EK9, `groups_left` in the report.
- **K-REQ-3 signal-safe trigger, no handlers** (MUST; API; MS1, the fork rule MS3; G-111). The
  library installs no signal handlers and documents the SIGTERM recipe (the handler calls the AS
  `mtl_instance_interrupt`, the main thread the bounded close), the PID 1 rule (handle SIGTERM, or
  run `tini`) and DPDK's temporary SIGBUS handler at open. Met by R8, `mtl_instance_abort` (a
  second SIGTERM), `instance.hotplug` (SIGBUS handler by option only), ex16 (§4.4).
- **K-REQ-4 crash-only correctness** (MUST; engine; MS3; G-111, G-112). SIGKILL at any instant
  leaves nothing that blocks a restart or needs cleanup; leftovers (SysV shm, files in `/tmp` or
  hugetlbfs, manager state) are avoided or reclaimed on disconnect; each residual is listed with
  owner and duration. Met by R8 (descriptors the kernel closes, nothing found by PID,
  close-on-exec, `MADV_DONTFORK`), EK6, EK12, EK15 and the residuals table (§4.5).
- **K-REQ-5 restart in place** (MUST; engine; MS3; G-112). `mtl_instance_open` succeeds in a
  restarted container of the same pod, with the same VF just reset by vfio, without depending on
  the PIDs of a previous run; CPU arbitration without a manager uses the affinity mask. Met by the
  reconcile at open (`instance.reconciled{kind}`), EK6, EK16, `instance.cpu_arbitration`.
- **K-REQ-6 fail fast, with a reason** (MUST; API; MS3; G-57). Open checks all it can before
  touching a device (VF and driver, IOMMU mode, hugepages within the **cgroup** limit, memlock or
  `CAP_IPC_LOCK`, capabilities, CPU set) and fails with one named reason and a one-line text for
  `terminationMessagePath`. Met by reasons 600–614, `mtl_error_info.detail`, EK18 (§4.9); no
  preflight flag, because an init container has its own CPUs, hugepage limit and memlock.
- **K-REQ-7 liveness, readiness and startup** (MUST; API, engine; MS3; G-109). One cheap, lock-free
  call from any thread. Liveness: every scheduler loop advanced within N ms, control thread alive,
  no fatal device reset, never dependent on packets or PTP lock. Readiness: links up, time locked,
  started sessions RUNNING. Startup: the phase. Met by `mtl_instance_get_health`, `enum mtl_phase`,
  `instance.stall_ns`, `MTL_EVENT_HEALTH`, EK11 (§4.10); one bad stream is `MTL_HEALTH_DEGRADED`.
- **K-REQ-8 CPUs from the affinity mask** (MUST; engine; MS3). With no `lcores` the instance uses
  `sched_getaffinity` of the opening thread; a CPU outside it fails (`CPU_NOT_ALLOWED`); CPU IDs are
  reported, not lcore IDs; MtlManager arbitration is off by default inside an exclusive cpuset. Met
  by EK7 and `instance.cpu_arbitration` (§4.7): inside an exclusive cpuset auto is none, and a
  mounted MtlManager socket grants AF_XDP queues only.
- **K-REQ-9 every thread pinned** (MUST; engine; MS3). Each scheduler pinned to one CPU,
  `TASKLET_THREAD` included; other threads on a housekeeping CPU; placement reported, with a
  warning when SMT siblings of a scheduler CPU are used by others. Met by `instance.main_lcore`
  (the first CPU of the mask, never a scheduler), EK7, EK13, `instance.cpu_shared`, EK17.
- **K-REQ-10 budget against the cgroup** (MUST; engine; MS3). Hugepages checked against the
  container's hugetlb limit; every pool faulted in at create, so an over-limit is `-MTL_ENOMEM` at
  create, never a later SIGBUS; memlock checked against the VFIO budget. Met by `HUGEPAGES_LIMIT`,
  `MEMLOCK_LIMIT`, `HUGEPAGES` and the `mem.*` keys (§4.11, [§5](deployment.md#5-hugepage-budgeting)).
- **K-REQ-11 ports from Kubernetes** (MUST; API; MS1). A port names a device-plugin variable
  (`env:PCIDEVICE_…`, the Nth address) with the IPAM source IP; MTL uses the VF's admin MAC and
  never sets one. Met by `mtl_port_spec.name = "env:VAR#n"`, `PORT_ENV_UNSET`, `MTL_PORTS`;
  Multus `network-status` is an application recipe; one device-plugin resource per ST 2022-7 network.
- **K-REQ-12 VF capabilities probed** (MUST; engine; MS3). At open: trust, the MAC filter budget
  (18 on an untrusted E810 VF), spoof check, a PF-set TX rate, hardware pacing and TM, PHC
  readability; a session that needs more fails at create with a named reason. Met by `caps.vf_trusted`,
  `caps.mcast_filters_max`, `caps.pf_tx_rate_mbps`, `caps.phc_readable`, `VF_UNTRUSTED`,
  `MCAST_FILTERS`; spoof check has no key yet.
- **K-REQ-13 no assumptions about a VF's history** (MUST; engine; MS3). MTL assumes nothing but
  what K-REQ-12 reads; on close it removes what it added (multicast filters, flow rules, TM); PF
  admin state is read only. Met by EK16, EK9 (§4.5).
- **K-REQ-14 an explicit IOMMU policy** (MUST; engine; MS3; CI hosts set the option). No-IOMMU or PA
  mode is refused unless an instance option opts in; the IOVA mode is reported. Met by
  `instance.allow_noiommu`, `NO_IOMMU`, `caps.iova_mode`, EK8; the legacy API only warns.
- **K-REQ-15 time sources that fit a pod** (MUST; API; MS3; G-109, G-54). AUTO is `CLOCK_TAI`
  when its kernel offset is set, else `SYSTEM_TAI` (ESTIMATED), and from MS6 the VF-readable PHC
  first when the node disciplines it (D-85, D-132); the built-in PTP client only when named; source, discipliner,
  lock and error reported; no source needs `CAP_SYS_TIME`; MTL never adjusts a host clock. Met by
  `time.*` keys, `time.phc_trust`, `mtl_time_set_reference`, EK10 (§4.8).
- **K-REQ-16 a minimal privilege profile per backend** (MUST; API, node setup; MS3). vfio: the
  plugin's device nodes, `IPC_LOCK`, maybe `SYS_NICE`; AF_XDP: `NET_RAW` and a node agent's socket;
  kernel: nothing; all non-root, read-only root fs with writes only to a runtime directory. Met by
  `CAPABILITY_MISSING`, `instance.runtime_dir`, EK12 and set-ups A–C (§4.14; A recommended, B the
  fallback, C privileged).
- **K-REQ-17 MtlManager optional and pod-safe** (MUST; engine; MS3, reconnect MS5; G-98, G-110). No
  manager by default; when used, identity from `SO_PEERCRED`, every grant released when its
  connection closes, interfaces named by ifname or PCI address; the client survives a manager
  restart. Met by EK4, EK5, `MANAGER_REQUIRED`, `MTL_EVENT_MANAGER_LOST` (§4.12).
- **K-REQ-18 the node owns the AF_XDP program** (MUST; engine; MS3). In a pod the library never
  attaches XDP programs; it takes a pre-created XSK socket or map from a node agent. Met by
  `port.xsk_map`, `XSK_UNAVAILABLE`, EK19 (§4.13); open: who sets the AF_XDP queue rate (§4.18).
- **K-REQ-19 multi-container pods** (MUST; API; MS3). One instance per process; containers never
  see each other through shared IPC, `/tmp` or lockfiles; a sidecar gateway's own clients are its
  concern. Met by NG5, EK6, EK12, `instance.runtime_dir` ([deployment.md §2](deployment.md#2-processes-and-instances)).
- **K-REQ-20 multicast that tolerates crashes** (MUST; engine; MS3; G-108). IGMP reports answer
  queries and stay periodic; interval and version are options; leaves on every leg at an orderly
  close; the stale-flood window after SIGKILL (260 s by default) is documented. Met by
  `port.igmp_version` (also I-REQ-65), `port.igmp_report_ms`, EK9; manager-sent leaves are later.

## 6. NMOS: N-REQ-1…60

What MTL must give an AMWA NMOS Node so that the Node can be conformant. The design is
[nmos-ipmx.md](nmos-ipmx.md): §3 names and explains each feature, §10 IS-04 values, §11 IS-05
activation, §12 SDP, §13 IS-08 and IS-11, §14 BCP-008; "How met" names the main symbols only. Phase 7 tests come with the code; NMOS
conformance itself is tested on the Node with the AMWA nmos-testing suite.

| ID | Requirement (source) | Pri | Part | How met | When |
|---|---|---|---|---|---|
| N-REQ-1 | width, height, scan and exact rational rate of every video session (IS-04 `flow_video.json`, `flow_core.json`) | MUST | API | `video.raster` (`fps` rational) of the session's configuration, which the Node keeps | v1 |
| N-REQ-2 | carry and report colorimetry, transfer characteristic and range (IS-04 Flow `colorspace`; ST 2110-20 §7.2, §7.3; BCP-006-01) | MUST | API | `video.colorimetry`, `tcs`, `range`; zero is the standard's default; changeable while running when no conversion uses them | 7 |
| N-REQ-3 | sampling and bit depth of the transport format (IS-04 `flow_video_raw.json`; ST 2110-20 §7.4) | MUST | API | `video.format`; `mtl_format_describe()` | v1 |
| N-REQ-4 | audio sample rate, bit depth, channel count, packet time (IS-04 `flow_audio*.json`; BCP-004 `packet_time`) | MUST | API | `audio.sample_rate`, `format`, `channels`, `ptime` | v1 |
| N-REQ-5 | media type and format of each essence (IS-04 Flow `media_type`) | MUST | API | `essence` and its format; ST 2110-41 has no registered NMOS media type | v1 |
| N-REQ-6 | granted per-leg port, SSRC, PT, UDP ports, MACs (IS-05 `/active`; SDP) | MUST | API | `mtl_session_info.leg[]` (MACs zero while unresolved); SSRC and PT are the session's (`sc.ssrc`, `sc.payload_type`), the same on both legs | v1 |
| N-REQ-7 | TP, TROFF and TRODEFAULT, CMAX, TSMODE, TSDELAY (ST 2110-21 §6.2, §8; ST 2110-10 §8.7) | MUST | API | `info.sender_type`, `info.troffset_ns`, `info.cmax`, `info.tsmode`, `info.tsdelay_ns`; `info.troffset_default` | v1; flag 7 |
| N-REQ-8 | maximum UDP datagram size (ST 2110-10 §8.6, MAXUDP) | MUST above 1460 B | API | `info.max_udp_bytes` | 7 |
| N-REQ-9 | bit rate with RTP/UDP/IP overhead, and payload bit rate (BCP-006-01 `bit_rate`; SDP `b=AS`) | MUST for ST 2110-22 | API | `mtl_session_info.wire_bps` (one leg, headers included); `info.wire_kbps{leg}`, `info.payload_kbps`, rounded up as NMOS defines | v1; kbps keys 7 |
| N-REQ-10 | PTP grandmaster identity and domain per port (ST 2110-10 §8.2; IS-04 `clock_ptp.json` `gmid`) | MUST | API | `time.grandmaster_id`, `time.ptp_domain`; `info.ts_refclk.*{leg}` | v1; `ts_refclk` 7 |
| N-REQ-11 | lock state and traceability (IS-04 `locked`, `traceable`; ST 2110-10 §8.2) | MUST | API | `time.state` (G-54); `time.gm_*` gauges | v1; `gm_*` 7 |
| N-REQ-12 | the time metadata also when ptp4l and phc2sys discipline the PHC (ST 2110-10 §8.2) | MUST | API | `mtl_time_set_reference()` with `struct mtl_time_reference`, read by the application through pmc | MS2 |
| N-REQ-13 | signal grandmaster changes (BCP-008 `synchronizationSourceId`; Sender `version`) | MUST for BCP-008, else SHOULD | API | `MTL_EVENT_GRANDMASTER`; polling `time.grandmaster_id` meanwhile | MS3; IS-04 7 |
| N-REQ-14 | each port's MAC and the port count (IS-04 `interfaces[].port_id`) | MUST | API | `mtl_port_spec.mac` from `mtl_port_get_spec()`, `instance.port_count` | 7 |
| N-REQ-15 | LLDP on DPDK-owned ports (IS-04 `attached_network_device`) | MAY | none | not adopted: `chassis_id = null` is allowed | later |
| N-REQ-16 | answer ARP on media ports (IS-04 `node.json`) | MUST | engine | built-in ARP | v1 |
| N-REQ-17 | map a leg to its interface (IS-04 `interface_bindings`) | MUST | API | `info.leg[i].port`, `mtl_port_get_spec()` | v1 |
| N-REQ-18 | change destinations, sources, ports and PT of every leg at one TAI instant or now, all or nothing (IS-05 activation) | MUST | API, engine | `mtl_session_update(…, MTL_UPDATE_FLOWS, &when, &planned)`, ex18 (G-75) | MS5 |
| N-REQ-19 | report when a scheduled activation will apply and when an immediate one did (IS-05 `activation_time`) | MUST | API | `planned_tai_ns`; `status.update_state`, `update_applied_tai_ns`, `update_seq`; `MTL_EVENT_UPDATE` | MS5 |
| N-REQ-20 | answer an immediate activation only after it applied (IS-05 RAML 200) | MUST | API | wait for `MTL_EVENT_UPDATE` APPLIED; the switch is at the index boundary by the clock, so an idle or muted sender applies | MS5 |
| N-REQ-21 | re-apply identical parameters on re-activation (IS-05 Re-Activating) | MUST | API | `MTL_UPDATE_REAPPLY`: RX re-reports and re-arms without a leave, TX re-resolves and rebuilds headers | 7 |
| N-REQ-22 | cancel a pending scheduled activation (IS-05 RAML 200) | MUST | API | `mtl_session_update(s, NULL, 0, NULL, NULL)`: 0, 1 none pending, `-MTL_EBUSY` (`UPDATE_COMMITTING`) | 7 |
| N-REQ-23 | replace a pending scheduled activation (IS-05 re-staging) | MUST | none | the Node answers 423 while one is pending | — |
| N-REQ-24 | enable and disable each leg, also scheduled and with a flow change (IS-05 `rtp_enabled`) | MUST | API | `sc.legs_disabled` with `MTL_UPDATE_LEGS` under one `when` (G-91) | MS5 |
| N-REQ-25 | stop and resume the whole stream, immediately or scheduled (IS-05 `master_enable`) | MUST | API | mute: every leg disabled, RUNNING, `MTL_STATUS_MUTED`, `tx.units_muted`, RX groups left | 7 (stop and start until then) |
| N-REQ-26 | exist before any connection (IS-04 resources at boot; IS-05 null address) | MUST | API | reserved legs: `legs_disabled` set, the flow zero until an update addresses it; started, so muted | 7 |
| N-REQ-27 | receive unicast and filter on the unicast source (IS-05 `multicast_ip = null`, `source_ip`) | MUST | API | RX `flows[i].ip` the port's own address or zero; `source_filter` checks the sender | MS1 |
| N-REQ-28 | SSM with one source filter per leg; IGMPv3 (IS-05 `source_ip`; RFC 4570) | MUST | engine | `mtl_flow.source_filter`, which must filter on DPDK too (issue #1239) | v1 |
| N-REQ-29 | two legs on one interface (IS-04 `interface_bindings`; RFC 7104) | SHOULD | API | an explicit `flows[1].port` | 7 |
| N-REQ-30 | one leg on a two-leg session and back without a stop (IS-05 ST 2022-7 rules) | MUST | API | the reserved leg of N-REQ-26 plus `MTL_UPDATE_LEGS` | 7 (stop and start until then) |
| N-REQ-31 | change the interface of a running session (IS-05 `interface_ip`) | SHOULD | engine | `MTL_UPDATE_FLOWS` with a port change, make before break; `-MTL_EBUSY` (`PORT_CHANGE_NEEDS_STOP`) where impossible | 7 |
| N-REQ-32 | change the received format at an activation (IS-05 `transport_file`) | SHOULD | API | later an RX `MTL_UPDATE_MEDIA` at an instant; meanwhile in STOPPED, or the A/B swap of two sessions | later |
| N-REQ-33 | validate staged parameters without preparing them (IS-05 PATCH 400) | SHOULD | API | `MTL_UPDATE_DRY_RUN`: validates and plans, reserves and posts nothing | 7 |
| N-REQ-34 | bound the double bandwidth of a far-ahead receiver switch (IS-05 scheduled activation) | SHOULD | API | `rx.join_lead_ns`: join at max(call, t − lead) | 7 |
| N-REQ-35 | a scheduled time in the past means now; any future time is accepted (IS-05) | MUST | API | a past `MTL_AT_TAI` or `MTL_AT_INDEX` is `MTL_NOW`; no horizon for updates; in ARMED a `when` before T0 is T0 | MS5 |
| N-REQ-36 | resolve `auto` transport parameters and report them (IS-05 `/active`) | MUST | API | the Node resolves and sets the source port; MTL reports granted values | v1 |
| N-REQ-37 | the instance TAI clock for versions and relative activations (IS-04 `version`) | MUST | API | `mtl_time_now()` | v1 |
| N-REQ-38 | apply SDP `mediaclk:direct=<offset>` and `mediaclk:sender` at an activation (ST 2110-10 §8.3) | SHOULD | API | `rx.rtp_offset`, `rx.mediaclk` are R options applied at the update's boundary | 7 |
| N-REQ-39 | generate a complete ST 2110 SDP (ST 2110-10 §8.1; IS-05 `/transportfile`) | SHOULD (MUST for the Node) | API | `mtl_sdp_render()` (`mtl_sdp.h`, libmtl_sdp); open items in [nmos-ipmx.md §23](nmos-ipmx.md#23-open-items-for-phase-7) | 7 |
| N-REQ-40 | parse an SDP into a session configuration (IS-05 `transport_file`) | SHOULD | API | `mtl_sdp_parse()`; legs beyond those parsed are zero and disabled | 7 |
| N-REQ-41 | per-port link state and its changes (BCP-008 `linkStatus`) | MUST | API, engine | `port.link_up`, `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE`; the link monitor (G-91) | MS5 |
| N-REQ-42 | RX arrival, loss per leg and after the 2022-7 merge, redundancy use (BCP-008-01) | MUST | API | `MTL_STATUS_RX_SIGNAL`, `leg.pkts_lost{leg}`, `rx.pkts_lost_est`, `rx.units_used_redundancy` (G-42) | v1 |
| N-REQ-43 | RX late packets and units late for presentation (BCP-008-01 `GetLatePacketCounters`) | SHOULD | API | `rx.pkts_stale`, `rx.units_stale`; `leg.pkts_late{leg}`, `rx.units_late_presentation` | v1; per leg 7 |
| N-REQ-44 | RX stream validity against the expectation (BCP-008-01 `streamStatus`; IS-11) | MUST | API | `rx.pkts_rejected{cause}`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*`, `tp.*` | v1 |
| N-REQ-45 | TX recoverable and unrecoverable errors (BCP-008-02 `transmissionStatus`) | MUST | API | `tx.indices_empty`, `tx.units_dropped{reason}`, `tx.units_failed`, `port.tx_errors`, `MTL_EVENT_TIMING_INFEASIBLE` (G-57); muted units' status is OI-56 | v1 |
| N-REQ-46 | TX missing essence (BCP-008-02 `essenceStatus`; IS-11 `no_essence`) | SHOULD | API | `MTL_EVENT_TX_UNDERRUN`, `tx.indices_empty` | v1 |
| N-REQ-47 | the PTP state per port (BCP-008 `externalSynchronizationStatus`) | MUST | API | `time.state`, `time.offset_ns` per port; `MTL_EVENT_TIME_STATE` (G-54) | MS3 (events); MS6 (G-54) |
| N-REQ-48 | cheap, consistent counters at any time (BCP-008: the Node keeps a baseline) | MUST | API | `mtl_stat_read()` (DP, one snapshot); counters never reset (G-40, G-42) | MS2 |
| N-REQ-49 | a stable identity across reconfiguration, a 36-character UUID as name (BCP-008) | MUST | API | `sc.name` (64 B, unique), kept by `MTL_UPDATE_MEDIA` (G-88, G-87) | MS1 |
| N-REQ-50 | dry-run any candidate configuration with capacity (BCP-004-01/-02; IS-11) | MUST | API | `mtl_session_query(…, MTL_QUERY_CHECK_CAPACITY, …)` (G-89) | query MS1; capacity MS5 |
| N-REQ-51 | never degrade a published sender type or pacing silently (BCP-004-02) | MUST | API | `caps.pacing` with `caps.pacing_req` = `MTL_REQ_REQUIRE`, `MTL_EVENT_PACING_CHANGED` (G-26, G-34) | v1 |
| N-REQ-52 | reconfigure a sender's format keeping its identity (IS-11) | MUST | API | `MTL_UPDATE_MEDIA` in CREATED or STOPPED (G-87) | MS3 |
| N-REQ-53 | mute a sender on a constraint violation (IS-11 "MUST become inactive") | MUST | none | Node policy over the mute of N-REQ-25 | 7 |
| N-REQ-54 | receive from transport parameters only, without an SDP (IS-11 `unknown`) | SHOULD | API | `video.detect = MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT` | MS3 |
| N-REQ-55 | PTP domain and announce timeout from IS-09 (`global.json`) | SHOULD | API | `time.ptp_domain`; `time.ptp_announce_timeout`, built-in client only | v1; timeout 7 |
| N-REQ-56 | remap or select audio channels for TX units (IS-08) | MAY | API | `mtl_convert()` with `mtl_convert_desc.channel_map`, applied by the application at a media index | MS4 |
| N-REQ-57 | the ANC DID/SDID seen on a received stream (IS-04 Flow `DID_SDID`) | MAY | API | `anc.did_sdid_seen` (up to 16 pairs) | 7 |
| N-REQ-58 | IPv6 media addresses | MAY | none | reserved `ip_family = 6`; the Node constrains to IPv4 | — |
| N-REQ-59 | ST 2022-5 FEC and RFC 3550 RTCP (IS-05 optional parameter sets) | MAY (IPMX: RTCP MUST) | API | RTCP: IPMX sender reports, TX from the `rtcp.*` options, RX with `mtl_ipmx.h` (§7); FEC not adopted | RTCP TX MS5, RX 7 |
| N-REQ-60 | a port address change, a DHCP renew (SDP `c=`; BCP-008) | MAY | API | `MTL_EVENT_PORT_ADDRESS`, then `mtl_port_get_spec()`; `port.dhcp` | 7 |

## 7. IPMX: I-REQ-1…76

What VSF IPMX (TR-10) needs from MTL. The design is [nmos-ipmx.md](nmos-ipmx.md): §16 the profile
(`session.profile = MTL_PROFILE_IPMX`), §17 RTCP sender reports, §18 wire details, §19 timing
without PTP, §20 PEP and HDCP; open items [§23](nmos-ipmx.md#23-open-items-for-phase-7) (marked
"open" below). Everything IPMX-specific is Phase 7, except TX sender reports (MS5), FREERUN
(MS6), DSCP (with the bindings) and any frame rate (MS4); no IPMX row has a contract test yet
except through a core guarantee.

| ID | Requirement (source) | Level | Part | How met | When |
|---|---|---|---|---|---|
| I-REQ-1 | networks with and without a common reference clock (TR-10-1 §7) | MUST | API, engine | PHC, `PTP_BUILTIN`; `MTL_TIME_SOURCE_FREERUN`, AUTO re-evaluated while running | v1; FREERUN MS6; AUTO 7 |
| I-REQ-2 | no PTP: a free-running internal clock (TR-10-1 §7.1) | MUST | engine | `MTL_TIME_SOURCE_FREERUN`: never stepped, slewed by `time.freerun_slew_ppm`; `SYSTEM_TAI` follows NTP steps, so it does not qualify | MS6; slew 7 |
| I-REQ-3 | PTP present: the internal clock synchronised to it (TR-10-1 §7.2) | MUST | engine | `MTL_TIME_SOURCE_PHC`, `_PTP_BUILTIN`; runtime switch by AUTO, holdover, `time.fallback` | v1; switch 7 |
| I-REQ-4 | BMCA per ST 2059-2 (§5.2 in the 2015 edition), `slaveOnly` by default (TR-10-1 §7.2) | MUST | none | ptp4l; the built-in client takes the first Announce (`mt_ptp.c:1036-1047`), has no announce timeout or domain filter, follows two-step masters only ([standards.md §13.3](standards.md#133-mtls-built-in-ptp-client-against-st-2059-2)): non-compliant under the profile | — |
| I-REQ-5 | a PTP leader BMCA can elect (TR-10-1 §7.2) | SHOULD | none | ptp4l | — |
| I-REQ-6 | video bursts within the IPMX CMAX ceiling max(16, int(Npkts / (21600 · TFRAME))), the ST 2110-21 Type W value (TR-10-1 §8.1) | MUST | engine | `MTL_SENDER_N` keeps its Type N CMAX, which is within the ceiling, and signals `TP=2110TPN`; no separate IPMX sender type | 7 |
| I-REQ-7 | the IPMX VRX (TR-10-1 §8.1) | MUST | engine | the profile plus `video.vtotal`, `video.htotal` | 7 |
| I-REQ-8 | audio timing per AES67 §7.5; 125 µs for low latency (TR-10-1 §8.2) | MUST | API | `audio.ptime = MTL_PTIME_125US` | v1 |
| I-REQ-9 | async and sync source media (TR-10-1 §8.3) | MUST | API, engine | sync: `MTL_MEDIA_AUTO`, `_INDEX`, `_TAI`; async: `MTL_MEDIA_SENDER` | v1; async 7 |
| I-REQ-10 | the media clock frequency-locked to the baseband source (TR-10-1 §8.4) | MUST | engine | `MTL_MEDIA_SENDER`: RTP follows the source; without it a +50 ppm source drops a frame every 5.6 min at 59.94 | 7 |
| I-REQ-11 | RTP per ST 2110-10; the first RTP from the internal clock (TR-10-1 §8.5, §8.6) | MUST | engine | sync `floor(M × rate)`; async RTP0 = `floor(M0 × rate)`, re-anchored by `MTL_SUBMIT_DISCONTINUITY` (G-19) | v1; async 7 |
| I-REQ-12 | baseband video: RTP at VSYNC, the report's NTP at the same instant (TR-10-1 §8.8.1) | MUST | API | SENDER mode: `unit.media_tai_ns` is VSYNC | 7 |
| I-REQ-13 | non-baseband video: RTP as ST 2110-10 (TR-10-1 §8.8.1) | MUST | API | the sync media modes (G-19) | v1 |
| I-REQ-14 | a sender report (RFC 3550 §6.4.1) to the media's destination at UDP port + 1 (TR-10-1 §8.7) | MUST | engine | `rtcp.sr`, `rtcp.dst_port`; NACK keys are `rtx.*`, same port, told apart by PT | MS5 |
| I-REQ-15 | the IPMX Info Block in every report (TR-10-1 §8.7) | MUST | engine | library-written; the application's fields with `MTL_RTCP_REFCLK_APP`, `_MEDIACLK_APP` and `mtl_rtcp_set_info()`; open: NUL in `ts_refclk[64]` | MS5; the application's fields 7 |
| I-REQ-16 | report NTP = the internal clock as a PTP truncated timestamp; SSRC the media's; RC 0 (TR-10-1 §8.7) | MUST; RC SHOULD | engine | NTP = the unit's media time, stamped into a template prepared off the tasklet | MS5 |
| I-REQ-17 | video: one report per frame or field, before its first packet (TR-10-1 §8.8.2) | MUST | engine | queued just before the unit's first packet; compressed video the same | MS5 |
| I-REQ-18 | ANC: one report per new RTP timestamp (TR-10-1 §8.9.2) | MUST | engine | ANC and fastmeta: before the first packet of each new timestamp | MS5 |
| I-REQ-19 | audio: a report at the first packet and every int(10 ms / ptime) packets (TR-10-1 §8.10.1) | MUST | engine | the audio schedule of `mtl_ipmx.h`; open: SENDER audio mid-unit | MS5; SENDER 7 |
| I-REQ-20 | the report first in a compound packet, then SDES CNAME (TR-10-9 §8) | MUST | engine | `rtcp.cname` | MS5 |
| I-REQ-21 | RTCP DSCP = the stream's (TR-10-9 §16) | MUST | engine | the leg's DSCP and TTL | MS5 |
| I-REQ-22 | receivers tolerate other RTCP; RR not needed (TR-10-1 §8.7) | MUST | engine | `rtcp.rx` parses sender reports, drops the rest | 7 |
| I-REQ-23 | with SSM a sender never joins its own source (TR-10-1 §8.7) | MUST | none | TX does not join (joins are RX-only in `mt_mcast.c`) [inferred] | v1 |
| I-REQ-24 | inline processors keep the input's timing (TR-10-1 §9) | MUST | API | `MTL_SUBMIT_SENDER_TIME`; zero copy through `unit.hold` | 7 |
| I-REQ-25 | SDP with `IPMX`, measured values, ts-refclk, mediaclk (TR-10-1 §10) | MUST | API | `mtl_sdp_render()` under the profile; open: measured values only as `fmtp_extra` | 7 |
| I-REQ-26 | baseband receivers recover async timing frequency-locked (TR-10-1 §11.1) | SHOULD | API | `mtl_rtcp_read()`, `rx.sender_rate_ppb`; the output clock is the application's | 7 |
| I-REQ-27 | link offset controllable while active (TR-10-1 §11.2; TR-10-8 §8) | MUST (TR-10-8 §8 sets no condition) | API | `rx.link_offset_ns` (R), `MTL_LINK_OFFSET_AUTO`, `rx.link_offset_min_ns`, `_max_ns` | 7 |
| I-REQ-28 | UDP destination port even, > 1024, default 5004 (TR-10-2 §7; TR-10-9 §17) | MUST | none | the application's; checked under the profile | 7 (check) |
| I-REQ-29 | UDP size within the limit, extensions included (TR-10-2 §7) | MUST | API | `sc.max_udp_payload`; PEP sizes after its extensions | v1; 7 |
| I-REQ-30 | receivers take YCbCr 4:2:2 10-bit and RGB 4:4:4 8-bit (TR-10-2 §8) | MUST | API | `MTL_YUV422_10`, `MTL_RGB_8` | v1 |
| I-REQ-31 | GPM or BPM, pgroups, 90 kHz, one RTP per frame or field (TR-10-2 §7, §9) | MUST | API | `video.packing` (G-19, G-22) | v1 |
| I-REQ-32 | any resolution and frame rate (AIMS profile §5–6) | MUST | engine | `mtl_raster` up to 32767, rational `fps` (num ≤ 4194303, den ≤ 1023); the engine's `st_fps` table (`st_api.h:61`) becomes rationals | MS4 |
| I-REQ-33 | video MIB 0x0001 (TR-10-2 §10) | MUST | engine | built from the config, and from `mtl_rtcp_info` (`par_num`, `par_den`, `measured_rate_milli`) | MS5; `mtl_rtcp_info` 7 |
| I-REQ-34 | non-baseband substitutes for htotal, vtotal and rates (TR-10-9 §10) | MUST | API | the defaults of `video.htotal`, `vtotal`, `measured_rate_milli = 0` | 7 |
| I-REQ-35 | SDP, NMOS and reports consistent; block version bumps (PQCR §4.1.1) | MUST | API | `tx.rtcp_info_version`, `MTL_EVENT_RTCP_INFO`; `MTL_META_RTCP_MIB` never bumps it | 7 |
| I-REQ-36 | interlaced: RTP per field, I and S bits (TR-10-2 §9–10) | MUST | API | `MTL_INTERLACED`, `MTL_PSF` (G-22) | v1 |
| I-REQ-37 | 48 kHz L16 and L24; 44.1 kHz L16, 96 kHz L24 (TR-10-3 §8) | MUST; SHOULD | API | `MTL_PCM16`, `MTL_PCM24`, `audio.sample_rate` | v1 |
| I-REQ-38 | ST 2110-30 level A at any channel count (TR-10-3 §10) | MUST | API | `audio.channels` | v1 |
| I-REQ-39 | PCM MIB 0x0002 (TR-10-3 §11) | MUST | engine | built by the library; `mtl_rtcp_info.channel_order` | MS5; `channel_order` 7 |
| I-REQ-40 | AES3 per ST 2110-31, MIB 0x0004 (TR-10-12 §7–10) | MUST if AES3 | API, engine | `MTL_AM824`; the MIB by the library | v1; MIB MS5 |
| I-REQ-41 | ANC per ST 2110-40 with reports (TR-10-4 §7–10) | MUST | API | `MTL_ANC`; reports as I-REQ-18 | v1; reports MS5 |
| I-REQ-42 | InfoFrames: ST 2110-41, DIT 0x100100, port + 3, the video's RTP (TR-10-10 §5–7) | MUST if InfoFrames | API | `MTL_FASTMETA`, `fastmeta.data_item_type`, `k_bit`; `tx.precede` | v1; precede 7 |
| I-REQ-43 | an InfoFrame packet before each video frame or field; Null block when empty (TR-10-10 §9, §12) | MUST if InfoFrames | engine | `tx.precede`: unit k on the video's queue just before video unit k; open: SENDER video, atomic updates | 7 |
| I-REQ-44 | CBR compressed video per ST 2110-22, the narrow model (TR-10-11 §7–11) | MUST | API | `MTL_CVIDEO_CBR`; CMAX by the profile (G-66) | v1; CMAX 7 |
| I-REQ-45 | MIB 0x0003 and 0x0008 for JPEG XS (TR-10-11 §12; TR-10-15-1 §9) | MUST | API | application bytes in `mtl_rtcp_info.mib`; the library cannot build them | 7 |
| I-REQ-46 | JPEG XS per RFC 9134 (TR-10-15-1 §7–8) | MUST if JPEG XS | API | `MTL_CODEC_JPEGXS` with a codec plugin; `MTL_CVIDEO_PACK_SLICE` reserved | v1 |
| I-REQ-47 | VBR compressed video, CMAX from MaxRate, `TP=2110TPW` (TR-10-7 §8–11) | MUST if VBR | API, engine | `MTL_CVIDEO_VBR_MAX`, `cvideo.max_bitrate_bps` (G-66) | 7 |
| I-REQ-48 | H.264 and H.265 per RFC 6184, RFC 7798 (TR-10-15-2, -3) | MUST if codec | API | packet units meanwhile; library packetisation later | MS5; later |
| I-REQ-49 | MIB 0x0005, 0x0009, 0x000A (TR-10-7 §12) | MUST | API | application bytes | 7 |
| I-REQ-50 | FEC Profile A (TR-10-6 §7) | MAY | none | later | later |
| I-REQ-51 | receivers tolerate FEC streams (TR-10-6 §7) | MUST | none | flows filter by UDP port | v1 |
| I-REQ-52 | PEP: AES-128-CTR; CTR, CMAC-64, ECDH variants (TR-10-13 §13, §20) | MUST if PEP | engine | `crypto.mode` (`MTL_CRYPTO_AES128_CTR` default) | 7, after the cost spike |
| I-REQ-53 | headers in clear; 16-byte slices; IV = iv' ‖ ctr (TR-10-13 §20) | MUST if PEP | engine | `crypto.clear_bytes`, `crypto.iv`; open: whether a partial slice consumes a counter | 7 |
| I-REQ-54 | the CTR Full and Short extensions (TR-10-13 §20.1–20.2) | MUST if PEP | engine | `crypto.ext_id_full`, `_short`; RX needs the header-extension fix (SF-68) | 7 |
| I-REQ-55 | MAC modes, CMAC-64 (TR-10-13 §15) | MAY | engine | the CMAC64 modes, on the copy path | 7 |
| I-REQ-56 | RTP_KV key versions at unit boundaries (TR-10-13 §20.3) | MAY | API | `MTL_CRYPTO_PEP_RTP_KV`, `mtl_crypto_set_key(…, when)`; no counter reuse under one key | 7 |
| I-REQ-57 | the same PEP parameters on both legs; RTCP in clear (TR-10-13 §13, §20) | MUST if PEP | engine | one ciphertext on both legs | 7 |
| I-REQ-58 | PEP MIB 0x0011 (TR-10-13 §22) | MUST if PEP | engine | built from `crypto.*`; open: no option for privacy_version or key identity | 7 |
| I-REQ-59 | PSK store, KDF, ECDH, parameters in SDP and IS-05 (TR-10-13 §12–13) | MUST if PEP | none | the application's; `mtl_crypto_set_key` takes the derived key | — |
| I-REQ-60 | the HDCP IV Counters extension first in every packet (TR-10-5 §14–15) | MUST if HDCP | API | packet units with the vendor's encryption; a cipher plugin later | MS5; later |
| I-REQ-61 | receivers watch streamCtr (TR-10-5 §14.1) | MUST if HDCP | API | packet units on RX, `MTL_PKTE_HDR_EXT` | MS5; 7 |
| I-REQ-62 | HKEP over TCP, `a=hkep`, MIB 0x0010 (TR-10-5 §10–12, §16) | MUST if HDCP | none | the application's | — |
| I-REQ-63 | DHCP by default, manual fallback (TR-10-9 §14) | MUST | API | zero `mtl_port_spec.sip` on kernel ports; `port.dhcp` on DPDK ports | v1 |
| I-REQ-64 | DSCP AF42, AF41, EF; user-selectable (TR-10-9 §16) | MUST | API | `mtl_flow.dscp` into the IP TOS (video MS1, the other essences MS4); 0 = the profile's default and `MTL_FLOWF_DSCP_LITERAL` (7); open: EF for PTP | v1; profile 7 |
| I-REQ-65 | IGMPv3 SSM and IGMPv2, version selectable (TR-10-9 §17) | MUST | engine | `port.igmp_version` (v2 checks the source filter in software; K-REQ-20); today v3 only (`mt_mcast.c:215`) | v1; v2 7 |
| I-REQ-66 | address ranges; default group 239.S.C.D (TR-10-9 §17) | MUST | none | the application's | — |
| I-REQ-67 | frame-to-frame interval ≤ 2 ms over 2 s (TR-10-9 §11.2) | MUST | engine | pacing (G-27); gauge `tx.f2f_pp_ns` | v1; gauge 7 |
| I-REQ-68 | receivers pick the sync method from ts-refclk and mediaclk (TR-10-9 §11) | MUST | API | `rx.mediaclk` DIRECT, SENDER (G-67); `_AUTO` | 7 (the key is under `MTL_LATER`; until then RX follows `mediaclk:direct` only) |
| I-REQ-69 | streams on one reference clock aligned by timestamps (TR-10-9 §13) | MUST | API | `mtl_rx_align()` on `media_tai_ns` (G-76) | MS6; 7 |
| I-REQ-70 | watch PTP and update the clock source description (TR-10-9 §12) | SHOULD | API | `MTL_EVENT_TIME_STATE` (G-54); the Info Block follows | v1; 7 |
| I-REQ-71 | in-band NMOS and HKEP on the media port (TR-10-9 §18–19) | MUST if both bands | API | `port.virtio_user` with 224.0.0.251 forwarded, or kernel backends; open: verify with a controller | v1 |
| I-REQ-72 | unicast streams, reports to the unicast destination (TR-10-1 §8.7) | MUST | API | unicast `mtl_flow.ip` with ARP or `MTL_FLOWF_USER_MAC` | v1; reports MS5 |
| I-REQ-73 | one network common, ST 2022-7 optional (TR-10-1 §6) | MAY | API | `flows[1]` optional | v1 |
| I-REQ-74 | off-subnet unicast through a gateway [inferred] | SHOULD | API | `mtl_port_spec.gateway` | v1 |
| I-REQ-75 | USB over IP (TR-10-14) | MAY | none | out of scope | — |
| I-REQ-76 | HDR MIB 0x0006, per field (TR-10-16 §7) | MAY | API | a unit's `MTL_META_RTCP_MIB` record, no version change | 7 |

Open IPMX MUSTs: I-REQ-25, -43, -58 and -64. I-REQ-4 is met only with ptp4l. The NMOS and IPMX
conflicts and their resolutions are [nmos-ipmx.md §21.3](nmos-ipmx.md#213-conflicts).

## 8. RTP-level use cases behind GO-10

GO-10 holds when each use case that today needs `*_TYPE_RTP_LEVEL` has a unified home, all in MS5.
Today's RTP level and its defects are [engine.md §7.4](engine.md#74-rtp-level-packet-path-today).

| Use case | Needs | Frame units enough? | Unified home |
|---|---|---|---|
| ST 2022-6 and payloads MTL does not packetise (ST 2110-43, proprietary metadata, RFC 3640) | L2–L4, NL or W pacing (ST 2022-8 §6), ST 2022-7, verbatim RTP, a 27 MHz timestamp per packet, 1396–1456 B packets (ST 2022-6 §6.2–§6.5) | no | `MTL_RTP` with `MTL_RTP_LINEAR`, `rtp.clock_rate`, `rtp.encoding`; `sc.max_udp_payload` up to the MTU (PE7); exit: a third party accepts a 2022-6 stream |
| application packetisers: hardware JPEG XS or H.26x encoders, FPGA streams, custom RFC 4175 packing | library pacing and 2022-7 over app-built packets; optional stamping | no | packet units on the essence, `MTL_PKT_PACE_UNIT`, `packet.set_fields` |
| gateways forwarding unchanged: unicast ↔ multicast, IS-05 re-addressing, 2022-7 merge or split | RX dedup, TX L2–L4 rewrite, µs latency | partly: frames add ≥ 1 frame | packet RX into packet TX; `MTL_PKT_RX_LEND` and holds for zero copy |
| RTP re-stamping onto local PTP | read the timestamp, stamp a derived one | partly (`MTL_SUBMIT_RTP_TS`) | `MTL_PKT_SET_TIMESTAMP` from media time; `MTL_PKT_TIME_FROM_RTP` |
| pcap record and replay, captured ST 2110-22 into a frame receiver included | verbatim TX with original or regular timing | no | verbatim packet TX, `MTL_PKT_TIME_FROM_RTP` or `MTL_PKT_PACE_LAUNCH` ([guarantees.md §3](guarantees.md#3-packet-unit-guarantees-g-pkt-17)) |
| analysers: gaps, per-leg arrival, path differential, conformance | every packet, both legs, NIC arrival times | no | `MTL_PKT_RX_NO_DEDUP`, `struct mtl_pkt_rx` (`leg`, `gap`, `arrival_tai_ns`, `MTL_PKTE_HW_ARRIVAL`) |
| fault injection against a frame receiver: out-of-order, truncated, malformed units | arbitrary order and counts | no | public packet TX; `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` for library-built streams |

Not offered in packet units: RTCP on RTP-level sessions. The spike SP-PKT (per-packet tasklet cost
of extbuf attach against today's RTP level, RX copy cost, chunk size sweep) gates the MS5 engine
work ([engine.md §6.1](engine.md#61-spikes-that-gate-engine-work)).
