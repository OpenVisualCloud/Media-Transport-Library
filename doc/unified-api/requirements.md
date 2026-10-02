# Requirements: every catalogue and where it is met

| | |
|---|---|
| Status | Reference for the proposed unified API (revision 4, typed configuration D-97, port first D-98). Nothing is implemented; the headers in [sketch/](sketch/) are normative, and where this page and a header disagree, the header wins |
| Date | 2026-10-02 |
| Folded from | the archived design documents 01 (goals and requirements), 13 (guarantees and tests), 16 §12 and 17 §6; Kubernetes study K1 §9–§10; interop studies N1 §3 and I1 §3; simplification study S8 §2.2 and §8; reviews RN §6 and C5 (§6.14 b5, the GPU test) |

This file holds every requirement catalogue of the design in one place: the goals and non-goals, the
core requirements R-* (whose rule text lives in [implementation-plan.md §8.0](implementation-plan.md#80-requirements-and-the-guarantees-that-close-them)),
the guarantees that test them, the Kubernetes requirements K-REQ, the NMOS requirements N-REQ, the
IPMX requirements I-REQ and the RTP passthrough requirements R-PKT. Each requirement is one row: what
the specification or the study asks (with its clause), whether MTL has a part in it at all, how the
design meets it (a function, an option, a field, or "the Node's job"), the phase, and the guarantee
or test that checks it. Read §1 for the columns, then jump to the catalogue you need. Where a row is
already stated in full in another maintained document, it points there instead of repeating it.

## 1. How to read the tables

| Column | Meaning |
|---|---|
| MTL's part | **none**: the application, the NMOS Node or the node (host) does it; MTL is not involved. **API**: MTL must expose a value, a call or a rule, and the work is small. **engine**: the engines must change behaviour (a fix of [engine.md §11](engine.md#11-the-engine-change-list) or new data-path work) |
| How met | the header symbol (`mtl.h` unless named), an option key (`mtl_options.h`), a stat key ([contract.md §11.3](contract.md#113-key-catalogue)), a reason (`mtl_reasons.h`), or "Node's job" / "application" / "node setup" |
| Phase | 0, 0.5, 1–6 as in [implementation-plan.md §2](implementation-plan.md#2-where-the-branch-sits-in-the-phases); M0–M6 are milestones of the ST 2110-20 branch; **2P** the packet-unit phase (decision M14); **7** Phase 7, later (NMOS extras, SDP, IPMX, PEP); "later" is beyond the Phase 7 design; "v1" means the ported API (Phases 1–6) |
| Tested by | a guarantee G-xx (§4), a test of [implementation-plan.md §8](implementation-plan.md#8-test-plan-and-guarantees), or "—" when nothing tests it yet (then the row is not promised; [implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence)) |
| Levels | MUST, SHOULD, MAY as the source states them. For R-*: MUST is a v1 blocker, SHOULD is v1 unless it costs a phase, MAY reserves the shape. For N-REQ: MUST means the Node cannot meet a normative NMOS or SMPTE rule without it. For I-REQ: the TR keyword; "MUST (if X)" applies when the optional feature X is implemented |

Specification short names: IS-04, IS-05, IS-08, IS-09, IS-11, IS-12 are AMWA NMOS interface
specifications; BCP-004-01/-02, BCP-006-01, BCP-008-01/-02 AMWA best current practices; TR-10-n VSF
IPMX technical recommendations; PQCR the IPMX product qualification requirements; ST 2110-n and ST
2059-2 SMPTE standards; RFC n IETF. Research notes are cited as Rnn (research note nn; their facts
are in [research.md](research.md) and [prior-art.md](prior-art.md)), reviews by their IDs (C1–C5,
[history.md](history.md) §6). Sources are named in plain text.

## 2. Goals, non-goals and the quality bar

### 2.1 Goals

[concepts.md §1.3](concepts.md#13-goals) has GO-1…GO-9 in short form; the first table keeps the full
rule and its source, the second where each is met. GO-10 is new (S8): it replaced non-goal NG2.

| ID | Goal (full rule) | Source |
|---|---|---|
| GO-1 | one session model for ST 2110-20, -22, -30, -40, -41, TX and RX; media-specific configuration only at creation; runtime verbs media-polymorphic | R00, R01 §2 |
| GO-2 | a familiar shape: app-driven acquire → fill → submit → result on TX, dequeue → read → release on RX; shared queues for results and events; memory regions; a context cookie per unit; a capability query with requested-versus-granted reporting; recognisable to libfabric and Rivermax users without their baggage | R09 §11, R10 §12 |
| GO-3 | one buffer contract, many provisioning paths: library pool, imported application memory and (later) device memory are the same pool slot with the same lifecycle; the allocation origin never implies a data path | R00 §2 |
| GO-4 | timing correct by construction: media time and launch time are separate; RTP comes from media time only; one exact rational timeline can be shared by several essences on TX and RX; late handling is an explicit policy with a per-unit result, never a silent fallback | R05 §10, R12 §10 |
| GO-5 | nothing happens silently: every accepted unit gets one terminal result with status, reason and timing; every downgrade is reported as requested versus granted; every state change is an event with a matching state getter | R06 §5 |
| GO-6 | real-time safety: no public call runs on a pinned tasklet core; tasklets never block, never allocate, never take a lock an application thread can hold, and never run application code (revision 4: not even by option, decision M5; the later inline notify is the one reserved exception) | R03 |
| GO-7 | a robust lifecycle: explicit per-session states, drain versus flush, interruptible waits, stale-handle safety, no application code and no memory access after a close retires, errors as codes | R13 §6 |
| GO-8 | an evolvable ABI: versioned structs, opaque handles, soname and symbol versioning, an experimental tier | R13 §7 |
| GO-9 | incremental delivery, engines first: the first implementation reuses today's packet builders and RX reassembly; engine fixes land first and reach legacy users; bugfixes are on by default, wire-visible changes opt-in on the legacy API; legacy APIs keep working during the transition | R01 §6, R08 §5, C5 §1.4 |
| GO-10 | every RTP-level use case of today (§8) has a unified equivalent | S8 §8.1 |

| ID | Where met | Phase | Tested by |
|---|---|---|---|
| GO-1 | `mtl_session_config` with one essence member; one verb set ([contract.md §3](contract.md#3-session-configuration)) | video M1–M2; other essences Phase 2 | G-45 |
| GO-2 | `mtl_tx_acquire`, `mtl_tx_submit`, `mtl_tx_reap`, `mtl_rx_dequeue`, `mtl_rx_release`; `mtl_queue.h`; `mtl_mem.h` regions; `unit.cookie`; `mtl_session_query`, `mtl_session_info` | M1–M3a; queues Phase 2; regions Phase 4 | G-46, G-47, G-34 |
| GO-3 | library pools; `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach`; `mtl_mem_import`; `mtl_mem_import_device` (`MTL_LATER`); `MTL_SESSION_REQUIRE_DIRECT` ([contract.md §9](contract.md#9-memory)) | library pools M1; the rest Phase 4; device memory Phases 4–6 | G-13, G-15 |
| GO-4 | `unit.media_index` and `media_tai_ns`, `enum mtl_media_mode`, `enum mtl_source_kind`, `mtl_sync.h` timelines, `tx.late_policy` ([timing.md](timing.md)) | video M3b; Phase 3 | G-19, G-26, G-28, G-76 |
| GO-5 | `struct mtl_tx_result`; `mtl_session_info` (granted values, `pacing_class`, `direct`); `MTL_EVENT_PACING_CHANGED`; events with getters ([contract.md §10](contract.md#10-events-and-queues)) | M1–M3a | G-01, G-13, G-41 |
| GO-6 | rule R6 of `mtl.h`; the slot interface ([engine.md §2](engine.md#2-the-pinned-core-rules)) | measured M1; P with Phase 2 | G-38, G-39, G-79 |
| GO-7 | `enum mtl_state`; `mtl_session_stop` (`MTL_STOP_DRAIN`, `MTL_STOP_FLUSH`); `mtl_session_interrupt`; R4 handles; `mtl_last_error` ([contract.md §4](contract.md#4-lifecycle-and-states)) | M0–M2 | G-29, G-30, G-31, G-49, G-64, G-65 |
| GO-8 | R3 (`struct_size`, `MTL_INIT`); R4 handles; `libmtl_unified.so.0.<rev>` with node `MTL_UNIFIED_EXPERIMENTAL`; `MTL_SIZE_CHECK` | M0 | G-33, G-51, G-73, G-74 |
| GO-9 | the engines track ([engine.md §11](engine.md#11-the-engine-change-list)); the legacy gate ([implementation-plan.md §8.5](implementation-plan.md#85-the-legacy-gate)); decision M7 | every milestone | G-99, G-106 |
| GO-10 | `MTL_UNIT_PACKETS` on every essence, plus the generic `MTL_RTP` essence (`mtl_packet.h`, [contract.md §13](contract.md#13-packet-units)) | 2P | G-PKT-1…8 |

### 2.2 Non-goals of the first version

Several are "reserve the shape now, implement later", so they can come without an ABI break. The
current standing of each is also in [concepts.md §1.4](concepts.md#14-non-goals-of-the-first-version);
this table adds why each was a non-goal and the question it raised.

| ID | Non-goal | Why | Question | Standing now |
|---|---|---|---|---|
| NG1 | replacing the packet builders and RX reassembly (kept). Pacing admission, RTP derivation, the time source and the completion plumbing **are** re-plumbed (E1–E13) | revision 2 said "not replacing the pacing engines", which was not honest given E1–E10 (C5 §1.4) | — | holds; legacy users get each engine change as below |
| NG2 | application-built RTP packets (`*_TYPE_RTP_LEVEL`) inside the unified API | a different ownership model (mbufs); the only route to ST 2022-6; the legacy API keeps it | Q-MODE-1 (answered by the owner 2026-10-01: packet units inside the unified session, every essence) | **dropped**: superseded by D-82, `MTL_UNIT_PACKETS` and `MTL_RTP` (2P); replaced by GO-10 |
| NG3 | SDP parsing inside `lib/` | parser edge cases; NMOS users already have tooling; `mtl_session_get_info` returns every SDP-relevant value | Q-MODE-6 | holds for MTL's own use; `mtl_sdp.h` is a helper built on public calls only (Phase 7, D-94) |
| NG4 | DMA-BUF, CUDA, Level Zero *direct* NIC access | no such path exists today (R04 §5); GPU-pinned *host* memory is a supported import | Q-MEM-8 | `mtl_mem_import_device` is `MTL_LATER` (Phases 4–6) |
| NG5 | multi-process session sharing, DPDK secondary processes | EAL runs `--in-memory` (R07 §5.2); processes synchronise through the epoch timeline | Q-LIFE-8 | one process per instance; `mtl_epoch_index_at` ([timing.md §10.4](timing.md#104-separate-processes)) |
| NG6 | RX header split | compiled out on the pinned DPDK (R04 §5) | Q-MEM-9 | not in the unified API; a decision M12 removal candidate |
| NG7 | library-scheduled RX delivery ("present at T") | report the presentation time first; scheduling later | Q-TIME-12 | `mtl_rx_detail.presentation_tai_ns` with `rx.link_offset_ns`; scheduling later |
| NG8 | runtime `mtl_port_open` | needs a port parameter struct and a list of the subsystems it brings up | — | Phase 6; no header symbol yet |

NG1 and legacy users: per engine change, bugfixes are on by default; wire-visible changes (RTP ±1
tick, default video RTP, ANC RTP, ST 2110-22 CBR, the linear read schedule) are off on the legacy
API behind an opt-in flag and on in the unified API ([engine.md §11](engine.md#11-the-engine-change-list), decision M7).

### 2.3 Quality bar, success criteria and the case for the API

| Item of document 01 | Where it lives now |
|---|---|
| §1 the four facts (no frame outcome, timing bugs #1653, external memory without an object, fragile lifecycle and ABI #1622 #1620 #1341) and the fifth (accidental asymmetry) | [concepts.md §1](concepts.md#1-why-a-new-api) |
| §1.1 the public surface counted honestly | [concepts.md §10](concepts.md#10-where-each-header-fits): today 475 functions (104 pipeline verbs: 34 st20p, 24 st22p, 23 st30p, 26 st40p), 75 `*FLAG*` defines; revision 4: 125 + 14 `MTL_LATER`. Today ≈44 decisions per video TX session (≈29 `st20p_tx_ops` fields + ≈15 flag bits). Revision-3 counts superseded |
| §2 personas P1–P9 | [concepts.md §1.2](concepts.md#12-who-it-is-for). P1–P8 come from code archaeology, not interviews; the external design review (decision M10) validates them |
| §6 quality bar: a phase is done only when every MUST in scope has a contract test at the cheapest tier that can observe it; P versus BE guarantees | [implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence) |
| §7 success criteria in user terms, per phase | [implementation-plan.md §6](implementation-plan.md#6-m7-the-remaining-phases) |

What [concepts.md §1.2](concepts.md#12-who-it-is-for) does not say about the personas is what each
uses today and the study behind it:

| ID | Uses today | Source |
|---|---|---|
| P1 | st20p get and put, blocking | — |
| P2 | pipeline, blocking get, copy | R08 §4 |
| P3 | user timestamp and user pacing, ST 2022-7 | R05 §6, R12 §10.6 |
| P4 | ext frames, slice mode, callbacks | R08 §3.3 |
| P5 | `query_ext_frame`, `put_ext_frame`, `mtl_dma_map` | R08 §1.2 (MXL) |
| P6 | Rivermax `rmx_output_media_*`, `rmx_input_*`; chunks map to packet units, memory regions to regions | R10 §12 |
| P7 | libfabric `fi_mr`, `fi_cq`, `fi_eq`, `fi_getinfo` | R09 §11 |
| P8 | everything | R12 §3.4, C5 §3.3 |
| P9 | `update_destination`, the stats dump | C5 §8.1–§8.2, §8.7, §8.11 |

Document 01 had no constraint catalogue (no C-x IDs); the C-xx IDs of the earlier documents are
findings of review C4, kept in [history.md](history.md) §6.4.

## 3. Core requirements R-*

The 72 requirements of document 01 §5, in revision-4 wording, with their level, guarantees and
milestone, are [implementation-plan.md §8.0](implementation-plan.md#80-requirements-and-the-guarantees-that-close-them);
the rule text of each is in [contract.md](contract.md), [timing.md](timing.md) or
[engine.md](engine.md). They are not repeated here. This section keeps what §8.0 does not: the study
each requirement rests on, and the clauses of the document-01 wording that §8.0 shortened or that
revision 4 changed. The new R-PKT family (§3.3) is not in §8.0 at all.

### 3.1 What each requirement rests on

Sources are research notes (Rnn §x) and review findings. R13's own lifecycle requirements R-L3,
R-L4, R-L5, R-L9 and R-L10 were folded into R-LIFE-3, R-LIFE-4, R-OBJ-5, R-THR-3 and R-CMP-6.

| Family | Sources, per ID |
|---|---|
| R-OBJ | 1: R00 summary, R01 §2 #1. 2: R00 §13, C5 §2.17. 3: R00 §7. 4: R09 §4.2, R11 §10 #6. 5: R13 R-L5, R00 §14 #7, C5 §2.5, §2.8. 6: R09 §5, C5 §7.5 |
| R-MEM | 1: R00 §3. 2: R00 §7, R04 §7, C5 §6.5. 3: R00 §14 #9–10. 4: R04 §4.2. 5: R00 §10, R04 §6 #4, C5 §6.1. 6: R00 §8.4. 7: R04 §7 #10. 8: R08 §4 Q4, C5 §6.2. 9: R00 §8.3, C5 §6.3 |
| R-TIME | 1: R00 §14 #15–16, R11 §10 #3, C5 §2.1. 2: R05 §10 #1, R12 §10.2, C2 #1. 3: R12 §1.3, §10.3, C5 §5.3. 4: R12 Q1, R05 Q1. 5: R05 §2.5, R00 §11, C2 #8. 6: R06 Q7, R05 §9 F1–F2. 7: R12 §10.6, C5 §5.1. 8: R05 §5, R12 §8.2. 9: R11 §10 #8, R08 §3.3. 10: R12 §9, R05 Q14. 11: R00 §12, R07 §7 #1. 12: C5 §5.7. 13: C5 §5.6 |
| R-CMP | 1: R00 §9, C4 C-32. 2: R00 §4.4, R11 §10 #4, C1 #1. 3: R11 §1.2, §10 #1, C5 §2.20. 4: R00 §9 #5, R04 §2.1. 5: R06 §5 Q5. 6: R13 R-L10, C5 §2.13, §2.16, §8.3. 7: R09 §5.2, R08 Q2, C5 §2.10 |
| R-OBS | 1: R06 §5, R11 §10 #12, C5 §8.8. 2: R11 §10 #7, C5 §8.9. 3: R06 Q6–Q7, C5 §8.2. 4: R06 §5, C5 §8.8, §8.12. 5: R06 F9–F10, C5 §8.7 |
| R-THR | 1: the maintainer's review of PR #1610, R03, C5 §4.1. 2: R02 §4.2 #10. 3: R13 R-L9, C5 §2.15. 4: R01 D2, R07 Q7. 5: R09 §11 |
| R-LIFE, R-CAP | LIFE-1: R13 §6.1, C4 C-01. LIFE-2: R00 §14 #25. LIFE-3: R13 R-L3. LIFE-4: R13 R-L4. LIFE-5: R08 B5 (#1341). CAP-1: R09 §2.1, R07 Q4 |
| R-ABI | 1: R13 §7, R02 §4.4, C5 §2.6. 2: R13 §4, C5 §2.18. 3: R13 Q13. 4: C5 §2.9, §8.5. 5: C5 §2.1–§2.4, §2.7 |
| R-OPS | 1: C5 §8.1. 2: C5 §8.2. 3: C5 §8.7. 4: C5 §8.11. 5: C5 §7.4. 6: C5 §8.6. 7: C5 §7.6 |
| R-TEST, R-USE, R-MIG, R-PERF, R-SEC | TEST-1, TEST-2: C5 §3.3. TEST-3: C5 §8.10, §1.8. TEST-4: C5 §8.10. USE-1: C5 §3.1. USE-2: C5 §3.5. MIG-1: C5 §1.4. PERF-1: C5 §1.8. SEC-1: C5 §1.10 |

### 3.2 Cross-check of §8.0 against the document-01 wording

Every R-ID of document 01 is in §8.0 with a guarantee and a milestone, except R-MEM-9 and R-TIME-11
(MAY, no guarantee) and R-PERF-1 and R-TEST-4 (closed by the budgets of §8.4 and the fault steps
of §8.3, not by a G-ID). The rows below are the clauses that differ; every other requirement reads the same in both.

| ID | Document-01 clause | Revision 4 |
|---|---|---|
| R-OBJ-2 | verbs `acquire/submit/release/withdraw`, `dequeue/release`; no verb means opposite ownership transitions in the two directions | the same rule; withdraw is `mtl_tx_withdraw(s, slot)` (DP, `mtl_mem.h`) |
| R-OBJ-3 | per-use information (media time, cookie, overrides) lives in a separate submission, never in the buffer | per-use fields live in `struct mtl_unit` (acquire zeroes them); the slot layout is fixed at attach |
| R-OBJ-5 | handles of instance, session, region, queue, timeline, group, buffer and lease; duplicate handles also fail | there is no group handle (a start array) and no buffer handle (a slot index, R4); a closed handle is never reissued |
| R-OBJ-6 | completion queues and event queues | one queue type, `mtl_queue_h`, with `MTL_BIND_*` parts and `MTL_SUB_*` subscriptions |
| R-MEM-2 | page-aligned `va` and `length`, hugepage-aligned for hugetlbfs, so no neighbouring memory is ever mapped for DMA | the same (`mtl_mem.h` MEM1; reason `UNALIGNED`) |
| R-MEM-3 | destroy returns `-MTL_EBUSY` while referenced | `mtl_mem_close` always consumes the handle and returns 1 while referenced; `MTL_EVENT_REGION_RELEASED` reports the end |
| R-MEM-5 | policy `REQUIRE_DIRECT / PREFER_DIRECT / ALLOW_COPY`; `stride ≥ row_bytes` is direct-capable | one flag, `MTL_SESSION_REQUIRE_DIRECT` (default: prefer direct, report `MTL_TXR_COPIED` and `info.direct`); `caps.tx_copy` forces copy; any stride ≥ row_bytes is direct (`mtl_mem.h` MEM2) |
| R-MEM-8 | one RX lease held by N TX submissions (a hold count) | `unit.hold` on each TX unit ([contract.md §9.6](contract.md#96-holds-and-forwarding)) |
| R-MEM-9 | dynamic per-acquire layouts, `MTL_POOL_DYNAMIC`, moved to Phase 4 | `mtl_tx_acquire_layout`, `mtl_rx_provide` (`MTL_LATER`, Phase 4) |
| R-TIME-1 | inside CQ records every time is TAI ns or invalid | R5: every time is TAI ns on the instance clock, valid by its flag (`MTL_TXR_*`, `MTL_UNITF_*`, `MTL_RXF_*`) |
| R-TIME-5 | no invalid request silently changes the pacing mode | `-MTL_ERANGE` with the reason, never another pacing mode (G-26); `caps.pacing_required` |
| R-TIME-6 | events on lock change and clock step | `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_STEP`; stat `time.state`, `time.offset_ns` |
| R-TIME-7 | an atomic group start | `mtl_session_start(s, n, when, &t0)`, all or none |
| R-TIME-9 | acquire reports the next slot | a separate call, `mtl_tx_next_slot` (`mtl_sync.h`) |
| R-TIME-10 | unwrapped media time, per-leg first and last arrival | `unit.media_index`/`media_tai_ns`; `mtl_rx_detail.arrival_first_tai_ns[]`, `arrival_last_tai_ns[]`, `presentation_tai_ns` |
| R-CMP-2 | results materialised by the reader from the lease table, unread results bounded by the pool size | the results ring holds `pool_count` entries and acquire reserves one, so producing a result never waits ([engine.md §5](engine.md#5-lease-table-and-result-materialisation)) |
| R-CMP-3 | `*_UNSET = 0` is never a terminal status | `enum mtl_tx_status` starts at 1 |
| R-CMP-7 | a race-free try-wait (1 ready, 0 armed, < 0 error) | retired: a data call that returns `-MTL_EAGAIN` arms its target (R2); G-69 retired |
| R-OBS-4 | log2 histograms by default, linear by option; windowed maxima over the last 1 s and 60 s instead of max-since-reset | `MTL_STAT_HIST` (log2 unless `hist.*` sets a width); `{window=1s\|60s}` keys ([contract.md §11.3](contract.md#113-key-catalogue)) |
| R-OBS-5 | tasklets write binary records to the log ring; no fact is only in a log | the same ([engine.md §2.8](engine.md#28-stats-traces-and-logs)) |
| R-THR-1 | tasklets never call application code unless the app registers an explicitly data-plane hook | no hook in v1 (decision M5); `mtl_session_set_inline_notify` is `MTL_LATER` |
| R-THR-3 | classes control, data, data with caller-context work, wait, async-signal-safe | `MTL_API_CP`, `_DP`, `_DPC`, `_WT`, `_AS` |
| R-LIFE-1 | STOPPED ≡ CREATED with history; a transient DESTROYING | `MTL_STATE_CLOSING`, then `MTL_STATE_RETIRED` |
| R-LIFE-5 | deferred with Q-LIFE-2 (no guarantee) | met: G-112 (re-open in the process, and after SIGKILL) |
| R-CAP-1 | granted pacing way, data path, conversion, DMA, timestamps | `caps.pacing`, `caps.dma`, `caps.hw_timestamps` (`MTL_REQ_*`), `info.pacing_class`, `info.direct` |
| R-ABI-1 | flags are plain integer literals | the same (`0x1u` style, unknown bits `-MTL_EINVAL`, `UNKNOWN_BITS`) |
| R-ABI-2 | hidden default visibility; libmtl gets a soname in Phase 0 (Q-ABI-1) | `MTL_API` sets visibility; G-51 |
| R-ABI-4 | the default instance publishes a merge table (invariant, mergeable, ignored) per field | `MTL_INSTANCE_SHARED`: a later open that differs in ports, lcores, time source or options is `-MTL_EEXIST`, `INSTANCE_MISMATCH` |
| R-ABI-5 | every input struct has an exported `*_init()` | one `mtl_struct_init()` behind `MTL_INIT(p)` |
| R-OPS-1 | activation `NOW`, `AT_TAI`, `AT_MEDIA_INDEX` | `struct mtl_when`: `MTL_NOW`, `MTL_AT_TAI`, `MTL_AT_INDEX`; `mtl_session_update` |
| R-OPS-2 | admin state settable while RUNNING; oper state from a link monitor | `sc.legs_disabled` + `MTL_UPDATE_LEGS`; `mtl_leg_status.admin`, `.oper`, `MTL_EVENT_LEG_STATE` |
| R-OPS-4 | capacity of queues, RL queues, scheduler quota, lcores, sessions | `capacity.*` and `port.free_*` keys, `MTL_QUERY_CHECK_CAPACITY` (reasons `CAPACITY_*`) |
| R-OPS-5 | `mtl_session_reconfigure`, keeping timeline, group, CQ and EQ bindings too | `mtl_session_update` with `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` in CREATED or STOPPED |
| R-OPS-6 | new creates fail with a distinct reason, or fall back when the instance allows it | no fallback (D-92): `-MTL_EAGAIN`, `MANAGER_LOST` |
| R-TEST-1 | null backend: experimental, no NIC, root or hugepages; TX → RX loopback on the same port | the same (`null:<n>`) |
| R-TEST-2 | `mtl_time_test_source`, `mtl_time_test_advance` | `mtl_test_clock`, `mtl_test_clock_advance` (`mtl_debug.h`) |
| R-USE-1 | an L4 simple layer gives P1 a ≤ 10-line loop | the simple layer is retired; `mtl_session_open` with a library pool and results off (ex01), one-call sends in `mtl_util.h` |

### 3.3 RTP passthrough: R-PKT-1…5

New with S8 when NG2 was deleted. S8 gave no level; they are the exit rules of phase 2P. The rule
text is [contract.md §13](contract.md#13-packet-units); the engine work is PE1–PE9
([engine.md §11](engine.md#11-the-engine-change-list)).

| ID | Requirement | MTL's part | How met | Phase | Tested by |
|---|---|---|---|---|---|
| R-PKT-1 | packet units on every essence, plus a generic RTP essence (ST 2022-6, custom payloads) | engine | `sc.unit = MTL_UNIT_PACKETS` with `struct mtl_packet_config`; `MTL_RTP` with `struct mtl_rtp_config` | 2P-a (video, ANC, fastmeta, RTP); 2P-b (audio, cvideo) | G-PKT-1, G-PKT-4 |
| R-PKT-2 | the library writes only L2–L4 and the RTP fields the session declares; verbatim by default | engine | `packet.set_fields` (`MTL_PKT_SET_TIMESTAMP`, `_SEQ`, `_SSRC_PT`, `_MARKER`; 0 = verbatim); `MTL_PKT_TX_VALIDATE` counts, never fixes | 2P-a | G-PKT-1, G-PKT-2 |
| R-PKT-3 | one result per chunk | API | the unit is a chunk; `MTL_SUBMIT_UNIT_END` ends a frame or field; `MTL_TXR_PKT_SHORT` | 2P-a | G-PKT-3 |
| R-PKT-4 | no DPDK mbuf is exposed and no application code runs on a tasklet | engine | chunks are pool slots with a packet table (`struct mtl_pkt_tx`, `struct mtl_pkt_rx`); RX copies in the caller by default | 2P-a | G-PKT-8 |
| R-PKT-5 | RX holds a bounded number of NIC buffers | engine | `packet.rx_ring_packets` (512); `MTL_PKT_RX_LEND` for zero copy within that bound; reason `RX_RING_BUDGET` | 2P-c (LEND) | G-PKT-5 |

## 4. Guarantees

A guarantee is a testable promise; a behaviour without a test is not promised. IDs are stable: a
retired guarantee keeps its number. Tiers (B, U, UB, I, A, M), the P/BE marking and test isolation
are [implementation-plan.md §8.1](implementation-plan.md#81-tiers-and-evidence); the fault matrix is
[§8.3](implementation-plan.md#83-fault-injection), the budgets [§8.4](implementation-plan.md#84-performance-budgets),
the oracle [timing.md §16.2](timing.md#162-oracle-and-contract).

### 4.1 Where each guarantee is stated

| Guarantees | Stated in full in |
|---|---|
| G-01…G-09, G-19, G-20, G-22, G-26, G-28…G-33, G-40…G-43, G-46, G-47, G-49…G-53, G-56, G-57, G-64, G-65, G-70…G-74, G-76, G-77, G-80…G-82, G-85, G-88, G-92…G-95, G-99, G-103, G-107…G-112 | [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-in-the-branch) (the branch's guarantees, with tier, milestone and method) |
| G-34, G-54; G-69 (retired: no try-wait call, its rule is now G-52); G-97 (revision-4 meaning: every example compiles, the ST20 ones run on `null:1`) | the paragraphs under [implementation-plan.md §8.2](implementation-plan.md#82-guarantees-in-the-branch) |
| G-17…G-28, G-53, G-59, G-61…G-63, G-66…G-68, G-75, G-76, G-82, G-85, G-86, G-94, G-104…G-106 | [timing.md §16.1](timing.md#161-guarantees) (text and tier); the methods missing there are in §4.3 below |
| G-10…G-16, G-35…G-39, G-44, G-45, G-48, G-55, G-58, G-60, G-78, G-79, G-83, G-84, G-87, G-89…G-91, G-96, G-98, G-100…G-102 | §4.2 below (their rules are in [contract.md](contract.md), tagged with the G-ID, but the test is only here) |
| G-PKT-1…G-PKT-8 | §4.5 below |

The phase of each guarantee follows its requirement ([implementation-plan.md §8.0](implementation-plan.md#80-requirements-and-the-guarantees-that-close-them)).
Requirement → guarantee map: the "Guarantees" column of §8.0. The export-pool guarantee G-78 also
serves R-CMP-1 and R-MEM-8 for framework pools.

### 4.2 Guarantees stated only here

Revision-4 names. Ev.: P promised, BE best effort. The second table gives each test method and
the further clauses ("also") of the same guarantee.

| ID | Guarantee | Tier | Ev. | Phase |
|---|---|---|---|---|
| G-10 | a region outlives every slot, conversion, packet and DMA reference | UB, I | P | 4 |
| G-11 | `mtl_mem_close` on a referenced region consumes the handle and returns 1; `MTL_EVENT_REGION_RELEASED` follows the last reference (revision 3: `-MTL_EBUSY`) | U | P | 4 |
| G-12 | no access outside the declared plane spans or the published rows | UB | P | 4 (rows 6) |
| G-13 | `MTL_SESSION_REQUIRE_DIRECT` never silently copies or converts | UB, I | P | 4 |
| G-14 | the selected data path is queryable and matches what happens | UB | P | granted values M3a; 4 |
| G-15 | library-pool and attached slots obey the same timing and outcome accounting | UB | P | 4 |
| G-16 | an import maps into every device on the session's path (both 2022-7 ports and the DMA engine), or fails | I | BE | 4 |
| G-35 | a session cannot start until its pool is complete and validated | U | P | 4 |
| G-36 | attach, detach and mapping never happen in the packet path. | UB | P | 4 |
| G-37 | once a close retires: no application code is called, no imported memory is touched, the handle reads `MTL_STATE_RETIRED` | UB | P | 4 |
| G-38 | no public function executes on a library busy-loop thread; from one, every call outside the inline-safe DP subset and the AS calls returns `-MTL_EDEADLK` (`BUSY_LOOP_THREAD`). | UB | P | measured M1; P with Phase 2 |
| G-39 | on the DPDK PMD backend the tasklet side of every hand-off makes no syscall, takes no lock an application thread can hold, and never allocates (no `malloc`, `rte_malloc*`, pool or ring create); the W2 wake-up is the one syscall. On the application side a data call makes no syscall except the non-blocking read that drains an armed wait handle (R6) | UB | P | measured M1; P with Phase 2 |
| G-44 | in release builds no log line is formatted or printed on a busy-loop thread at any level; tasklet logs go through the binary log ring | UB | P | 5 |
| G-45 | the same verb sequence passes G-01…G-09 for every essence × direction | U, UB | P | 2 (video only in the branch) |
| G-48 | a slot's planes never change between uses; per-use values live only in `struct mtl_unit` and the result | U | P | 4 |
| G-55 | `queue.queued_media_ns` equals the sum of the queued units' durations; each histogram's `count` equals the number of published results | U | P | 5 |
| G-58 | event coalescing never loses the first or last state of a transition and never blocks a producer; every queue subscribed to a source receives each state event of that source (coalesced), independently of other subscribers | U; I in Phase 5 | P | M3a |
| G-60 | `mtl_tx_acquire_slot` leases exactly the named slot; `MTL_SESSION_RX_BY_INDEX` places unit k in slot k mod `pool_count` | U | P | 4 |
| G-78 | export pool: a framework pool over `mtl_tx_acquire`, `mtl_tx_submit` and `mtl_tx_release` recycles every wrapper exactly once. An unsubmitted buffer's release releases its lease; a submitted one returns only through its result; `MTL_SESSION_EXPORT_POOL` implies results; `set_active(FALSE)` is `mtl_session_stop(MTL_STOP_FLUSH)` and delivers every result | U | P | 4 |
| G-79 | from a library busy-loop thread the inline-safe DP subset (`mtl_tx_acquire`, `mtl_rx_dequeue`, `mtl_tx_reap` with timeout 0, `mtl_tx_submit`, `mtl_tx_release`, `mtl_rx_release`, getters) never blocks: it only trylocks the reaper lock (`-MTL_EAGAIN` when contended), never drains or writes a wait handle and makes no syscall | UB | P | measured M1; P with Phase 2 |
| G-83 | an RX lease held by N TX units (`unit.hold`) returns to free exactly once, when its hold count reaches 0 **and** the application released it, in any order; MTL never writes a slot that is held or being read. | U, UB | P | 4 |
| G-84 | a layout with stride ≥ row_bytes (a sub-rectangle, an interleaved field) over a direct-capable region is direct for ST 2110-20 TX and RX: the stride reaches the engine as its line size and no whole-unit copy happens. | UB | P | 4 |
| G-87 | `mtl_session_update` with `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` in STOPPED keeps the handle, name, flows, SSRC, counters, timeline and queue bindings; library pools are re-created; any validation failure changes nothing | U | P | 2 |
| G-89 | if `mtl_session_query` with `MTL_QUERY_CHECK_CAPACITY` succeeds and nothing else changes the host, the create succeeds; if it fails, the reason names the limiting resource (`CAPACITY_*`); `capacity.*` and `port.free_*` return to their baseline after every close | U, I | P | 2 |
| G-90 | create and start never wait for ARP or IGMP: an unresolved TX leg is `MTL_FLOW_WAITING_NEIGHBOUR` and its units are not sent on that leg (DROPPED, `WAITING_NEIGHBOUR`, when every leg is unresolved); resolution resumes sending without an API call; `MTL_EVENT_FLOW_STATE` and `mtl_leg_status.flow_state` report the state per leg | U, I | P | 2 |
| G-91 | resources are reserved for every configured leg regardless of link, and a configured leg is never pruned; disabling or enabling a leg (`legs_disabled`, `MTL_UPDATE_LEGS`) takes effect at a unit boundary; admin and oper state per leg are in `mtl_session_status.leg[]` and `MTL_EVENT_LEG_STATE`. | U, I | P | 2 |
| G-96 | an import whose `va` or `length` is not page-aligned (hugepage-aligned for hugetlbfs) fails with `-MTL_EINVAL`, `UNALIGNED`, and maps nothing; an aligned import maps exactly `[va, va + length)`. | U, I | P | 4 |
| G-98 | MtlManager loss never kills or stalls the process: no SIGPIPE (manager sends use `MSG_NOSIGNAL`); running sessions continue; a create that needs the manager returns `-MTL_EAGAIN`, `MANAGER_LOST` (the revision-3 shared-memory fallback is removed, D-92); after reconnection the instance re-registers and re-announces its CPUs | U, I | P | 2 (the running half is G-110, M3a) |
| G-100 | MTL never writes a TX slot: after `mtl_tx_submit` its bytes are unchanged and it stays mapped until it is acquired again; a slot pinned with `mtl_tx_pin` is never returned by an any-free `mtl_tx_acquire` | U, UB | P | 4 |
| G-101 | attach fails with `-MTL_EINVAL`, `ACCESS_MISMATCH`, when an RX session gets a region without write access or a TX session one without read access (`MTL_MEM_WRITE`, `MTL_MEM_READ`) | U | P | 4 |
| G-102 | an import beyond the region budget (`caps.max_regions`) fails with `-MTL_ENOSPC`, `REGION_BUDGET`, never with an opaque DPDK error; `instance.regions_used` and `instance.regions_free` are exact | UB | P | 4 |

| ID | Further clauses and how it is tested |
|---|---|
| G-10 | `mtl_mem_close` attempted at every stage of a unit's life |
| G-11 | close with slots attached and units in flight |
| G-12 | guard pages (`mprotect`) around spans; rows units publish progressively with the unpublished rows poisoned |
| G-13 | force every downgrade condition (no multi-segment TX, pool below `min_count_direct`, ST 2110-22, PA mode, conversion): query, create or start fails with a reason; conversion with REQUIRE_DIRECT is `-MTL_ENOTSUP`, `DIRECT_IMPOSSIBLE`, at query and at create alike; a small pool `POOL_TOO_SMALL` |
| G-14 | compare `info.direct` and the `path` of `mtl_tx_result_full` and `mtl_rx_detail` with the per-unit `pkts_dma` and `pkts_copied_partial`. GPU ingest path (C5 §6.14 b5): import `mmap(MAP_ANONYMOUS \| MAP_LOCKED)` memory as a stand-in for GPU-pinned host memory, run RX direct into it and TX direct from it, and check `info.direct`, `path` and the per-unit counters |
| G-15 | identical submissions through both; diff the results |
| G-16 | a 2022-7 session on two ports plus a DMA engine under an IOMMU fault monitor (DMAR or AMD-Vi lines in `dmesg`); P once the Phase 4 I-tier job scans for IOMMU faults |
| G-35 | start with fewer attached slots than `pool_count`: `-MTL_EINVAL`, `POOL_TOO_SMALL` |
| G-36 | also: an attach to a session on a device the region is not yet mapped to maps it before the attach returns (lazy mapping is a CP step); no map or unmap runs on the data path of a RUNNING session. Test: instrumentation: no allocation and no map call while RUNNING |
| G-37 | unmap the region right after `MTL_EVENT_SESSION_RETIRED`; no fault |
| G-38 | also: from a library thread that close would join (dispatch, log sink, codec), the instance close and shutdown return `-MTL_EDEADLK` (`LIBRARY_THREAD`). Test: call each function from the RX packet lcore, the TAP lcore and (with M12) a user busy-loop thread |
| G-39 | syscall counting and a `malloc`/`rte_malloc` hook on the tasklets during a stress run, per `MTL_INSTANCE_TASKLET_THREAD` mode (C1 #7, #15). Excluded: the idle sleep, W2 sessions, and the kernel-socket and AF_XDP backends' own syscalls (reported, not forbidden) |
| G-39, W2 | the W2 exception is one non-blocking eventfd `write()`, only when a waiter is armed: in lcore mode a recommendation for unit periods below 1 ms that needs decision M6 (Q-THR-2); in thread mode the default |
| G-44 | a log hook asserts on busy-loop threads (C1 #14) |
| G-45 | the suites parameterised over `MTL_VIDEO`, `MTL_CVIDEO`, `MTL_AUDIO`, `MTL_ANC`, `MTL_FASTMETA` (and `MTL_RTP` packet units in 2P) |
| G-48 | hash `mtl_session_get_slot` output before and after 10^4 uses |
| G-55 | null-backend runs with the test clock, compared at quiescent points |
| G-58 | a tasklet producer and a reader under stress (C1 #8). The revision-3 clause "USER posts never evict library notices" is superseded: revision 4 has no application-posted event |
| G-60 | named acquires over every slot; an RX index sweep |
| G-78 | also: `mtl_tx_release` on a submitted lease is `-MTL_ESTALE` and changes nothing. Test: the GstBufferPool state machine replayed on `null:1`, including release before the result (C5 §7.2) |
| G-79 | the subset called from a busy loop under contention from an application thread (C5 §4.1) |
| G-83 | also: the N TX units may be in flight together; a TX unit whose planes lie outside the held RX slot fails with `-MTL_EINVAL` (`HOLD_MISMATCH` has no code, OI-54). Test: one RX unit to four TX sessions attached over the same pool (`mtl_session_get_pool_region`); release the RX lease, then complete the TX units in random order: the slot is never reused early and never leaks (C5 §6.2) |
| G-84 | also: only a packet that crosses row padding (non-GPM_SL packing) or a PA-mode page boundary is copied, each counted in `pkts_copied_partial`; slots of one pool with different strides fail with `-MTL_EINVAL`, `STRIDE_MISMATCH`. Test: split-forward quarter frames and 2 × row_bytes field buffers with `MTL_SESSION_REQUIRE_DIRECT`: no copy counted where the engine needs none (C5 §6.1) |
| G-87 | 1080p → 720p → 1080p, counters keep counting (C5 §7.4). Failures: attached slots that no longer fit (`-MTL_EINVAL`; the reason first named for it, `RECONFIGURE_INCOMPATIBLE`, has no code in `mtl_reasons.h`; OI-54 recommends an existing code such as `LAYOUT_MISMATCH`), a unit period that no longer fits the timeline's grid (`GRID_MISMATCH`) |
| G-89 | fill a scheduler to its quota and its session limit on `null:1` (U); RL queues on a VF (I) (C5 §8.11) |
| G-90 | a destination that never answers ARP; create returns within its CP bound (C5 §7.6) |
| G-91 | also: a down leg does not starve the pool (units complete with that leg marked not sent, `leg_reason[]` of the full result) and resumes at the next unit boundary after its link returns, with no application action. Test: `null:1` with `MTL_FAULT_LEG_DOWN` (U); `nicctl.sh vf_link … down\|up` while RUNNING (I) (C5 §8.2) |
| G-96 | also: an import spanning VMAs with different backings fails with `-MTL_EINVAL`, `MIXED_BACKING`. Test: U: every misalignment class; I: the device's IOVA table shows no page outside the range ([deployment.md §1.1](deployment.md#11-imported-memory-and-the-iommu)) (C5 §6.5) |
| G-98 | `mtl_debug_inject(MTL_FAULT_MANAGER_LOST)`; I: `gtest.sh` kills MtlManager, runs creates, restarts it (C5 §8.6) |
| G-100 | hash the slot before submit and after the result on the direct, copy and convert paths (C5 §6.7) |
| G-101 | every direction × access combination (C5 §6.10) |
| G-102 | import until the budget is exhausted, close one region, import again (C5 §6.4) |

### 4.3 Timing guarantees: the methods timing.md does not give

The guarantee text and tier are in [timing.md §16.1](timing.md#161-guarantees); its "methods that
matter" cover G-19, G-22, G-68, G-85, G-86 and G-94. Phases: G-27 Phase 2; G-106 Phase 0.5; the
others Phase 3 (the ST 2110-20 subset in M3b). All are P unless marked.

| ID | Method, and clauses beyond timing.md |
|---|---|
| G-17, G-18 | a schema test over every output struct: each time field has a validity flag and a clock |
| G-20 | today ST20 and ST40 of one frame differ by +54.4…+55.7 ticks at 1080p59.94, depending on the granted VRX0 (computed, R05 F7); the test checks equal RTP once ANC exists (Phase 2) |
| G-21 | random submission sizes (1024, 800/801 and 1601/1602 samples among them) against one large submission: identical packets |
| G-23 | fail one member's validation: nothing starts |
| G-24 | inject lateness into video only: audio RTP and launch are unchanged; a bounded `MTL_LATE_SEND_LATE` never overlaps the next slot (C2 #4) |
| G-25 | RX units with shifted arrival: `media_tai_ns` unchanged |
| G-26 | today invalid exact requests silently fall back (`st_tx_video_session.c:1796-1805`, R05 F3) |
| G-27 | BE. EBU LIST or the timing parser with NIC timestamps, per NIC × pacing class; a Phase 2 exit criterion, not a Phase 1 regression gate, because the new API has no RxTxApp path before Phase 1 ends (C5 §1.8); P per class once measured |
| G-59 | P at UB, BE at M: a wide sender with maximum pre-fill (C2 #14, #15); UB checks the scheduled times, M the wire |
| G-61 | submit no ANC for 10 frames: one empty packet per frame or field; ST41 at least every 500 ms |
| G-62 | `mtl_debug_inject(MTL_FAULT_TIME_STEP)` with `step_ns` = +37 s, or `mtl_test_clock_advance` (today's UTC → PHC switch, C2 #10). Also: the published time base never steps except at a declared `MTL_EVENT_TIME_STEP`; its frame-start error is the S7 budget |
| G-63 | create the sessions slowly (test clock advanced between creates), then start: index 0 of an `MTL_ANCHOR_AT_START` timeline is never late because of creation time |
| G-66 | variable codestream sizes in both rate modes, and an oversize codestream (C2 #9, C5 §5.12): rejected synchronously at submit, reason `CODESTREAM_OVERSIZE`, and the slot goes back to the pool without a result (`mtl_tx_submit`, D-88; OI-15); `MTL_CVIDEO_VBR_MAX` sets `MTL_INFO_NON_COMPLIANT` |
| G-67 | an AES67 sender with a non-zero `rx.rtp_offset` (C2 #11); a direct stream with a large offset posts `MTL_EVENT_RX_TIMEBASE_SUSPECT` |
| G-68 | at 59.94 with k = 3, 7 and 13 the revision-2 formula gives a half-integer `T0·90000`, this one integers (C5 §5.1); interlaced members contribute TFRAME (not TFIELD) to the grid, which makes T0 a first-field instant; the same holds for `MTL_AT_INDEX` on one session |
| G-75 | U, I, Phase 2: move both legs to a new multicast pair at one TAI instant; at I capture both legs: no unit mixes old and new destinations (C5 §8.1). TX `MTL_AT_INDEX k`: unit k is the first on the new flows on every leg; RX `MTL_AT_TAI t`: the new rule takes units whose media time ≥ t, the old rule is removed after |
| G-75, failure half | revision 4 changes it: an unresolved neighbour no longer fails the update (the leg waits in `MTL_FLOW_WAITING_NEIGHBOUR`, RN-2); a capacity shortage is `-MTL_ENOSPC` and changes nothing; a port change is `-MTL_EBUSY`, `PORT_CHANGE_NEEDS_STOP`, until make-before-break (Phase 7) |
| G-104 | a CAPTURE start array with video slot delay L_v = 1 and 2; drop video units: the empty ANC keep-alive still lies in the window (C5 §5.13) |
| G-105 | both set-ups (two processes on the epoch with `mtl_epoch_index_at`, one process with a start array) on the null backend; packet headers diffed (C5 §5.6) |
| G-106 | the oracle against the Phase 0.5 helper `st_timeline_unit().rtp` (U); legacy ST20, ST30 and ST40 sessions fed `legacy_ts_ns` put that RTP on the wire (UB) ([timing.md §14.2](timing.md#142-phase-05-the-legacy-st_timeline_-helper)) |

### 4.4 Clauses of the branch guarantees that §8.2 shortened

| ID | Clause or test detail kept from document 13 |
|---|---|
| G-03 | PR #1610's backlog stalled (R01 R5); chain-mode results today never arrive while the application is idle (C1 #2); the slot interface needs idle descriptor cleanup |
| G-05 | today st20p stores FREE before it calls back (C1 #1), so the race is real |
| G-07 | a lease of another session fails deterministically, on the session index encoded in the lease |
| G-08 | today st20p numbers frames at `get_frame` (`st20_pipeline_tx.c:806-808`) and the builder picks the lowest number (`tx_st20p_newest_available`, `:62-77`, SF-44), so it would send A first; the slot interface assigns `seq` at submit (C5 §4.11). Also the A B D C trace of R01 R6 |
| G-09 | a unit that completes before an older in-flight unit frees its slot at completion without waiting; only its result waits for its predecessors |
| G-30 | today `wake_block` does not return early (R13 §1.4, SF-16; fixed in open PR #1770) |
| G-33 | `struct_size` is input only and never rewritten; 0 or a too-small size is `NONZERO_TAIL` too; a non-zero `reserved` field of an input struct is `-MTL_EINVAL` |
| G-49 | the state × call table runs inside one long-lived instance, because the last `mtl_uninit` of today's default instance happens inside the library and a clean re-init per case is impossible (#1341) |
| G-64 | the GStreamer `unlock`/`unlock_stop` sequence (C3 P2-1, C5 §8.12). Also: `mtl_instance_interrupt(mt, 0)` undoes the instance interrupt but leaves a session or queue interrupted on its own still interrupted; interrupting a CLOSING session is a no-op (close wins) |
| G-41 | state events only: notices such as `MTL_EVENT_EPOCH_TICK` and `MTL_EVENT_OVERFLOW` need no getter. Test: overflow a session's events, then compare every getter with the true state. Open: `MTL_EVENT_RX_TIMEBASE_SUSPECT` reports a state that no `MTL_STATUS_*` flag or `rx.*` key holds, so it is lost after an overflow (OI-51 recommends a status flag) |
| G-70 | the P1 forgotten-start case (C5 §3.2, §8.3); `-MTL_EAGAIN` sets only code and reason in `mtl_last_error`; `detail` stays empty for DP failures |
| G-72 | nothing is written beyond `max × rec_size`; the same rule for event reads with `ev_size` |
| G-73 | the lint checks both the fields whose zero is replaced by another value and the zero-valued enumerators that name a mode; each `MTL_INIT` output equals a zeroed struct except `struct_size` |
| G-74 | every hole is a named `reserved` field; every symbol named in any document is declared; `check.sh` moves to `tests/unit/unified/` in Phase 1. The revision-3 clause on 256-byte CQ entries is superseded (no CQ type) |
| G-77 | the framework teardown order: release the instance before the last lease returns (C5 §7.1, §8.5); close never fails because objects are still live; process exit without a close is supported |
| G-80 | immediate commands (stop, discard, detach, interrupt) are acked within one tasklet iteration even with no unit and no packet; the CP applies a command itself for a detached session; the ack timeout is `instance.cmd_ack_timeout_ns` (100 ms); with `MTL_INSTANCE_TASKLET_SLEEP` one iteration plus the wake-up |
| G-81 | after the bounded idle-cleanup wait a worker resets the queue (dedicated) or quarantines it (shared); mbuf free callbacks then run on that worker; `MTL_EVENT_SESSION_RETIRED` comes only after; a failed reset escalates to port-reset handling. Then no descriptor references imported memory; `mtl_mem_close` returns 0. The I part is P once a hang can be forced on a VF (C5 §4.3) |
| G-82 | the due time is the first-packet arrival on the earliest leg + the unit period + `rx.flush_offset_ns`, capped at `presentation_tai_ns` when a link offset is set; the unit is delivered or discarded per `rx.incomplete`; `mtl_session_stop(MTL_STOP_DRAIN)` on RX delivers the partial unit with its completeness |
| G-76 | start at `MTL_AT_TAI t` with packets flowing before t; stop and start again without an IGMP leave; the latest-only recipe (`pool_count` 2, `MTL_SESSION_RX_LATEST`); video, audio and ANC aligned with `mtl_rx_align` (C5 §5.7, §8.4); every member of an RX start array started at t delivers its first unit with media ≥ t. |
| G-76, rates | the index inverse is checked against the oracle at every rate: 1001 families, fields and floor-aligned 44.1 kHz, across the 2^32 RTP wrap, and for a sender with a phase φ < P (it maps to the slot it falls in); the branch ([implementation-plan.md §8.2](implementation-plan.md#82-guarantees-in-the-branch)) runs only the video rates |
| G-88 | the name is copied (64 B) and unique per instance: `-MTL_EEXIST`, `NAME_EXISTS`, on a clash with a live or CLOSING session; it appears in `mtl_session_info`, the stats, the log prefix and every event (`origin_name`) |
| G-94 | queued units with M < S are FLUSHED with `BEFORE_START` |

### 4.5 Packet-unit guarantees G-PKT-1…8

From S8 §8.3; phase 2P on the null backend (a TX chunk completes at its scheduled instant, a
loopback RX session on the same port receives the packets with synthetic arrival times).

| ID | Guarantee | Tier | How it is tested |
|---|---|---|---|
| G-PKT-1 | verbatim: with `packet.set_fields = 0` every byte from the RTP header on reaches the wire unchanged, on every leg | U, I | `null:1` loopback TX → RX without `MTL_PKT_RX_INCLUDE_L2`, byte compare; I: capture compare |
| G-PKT-2 | stamping writes exactly the declared fields, identical on both legs | U | loopback, field by field |
| G-PKT-3 | one result per chunk; accepted = published + suppressed under withdraw, `MTL_STOP_FLUSH`, leg down and close | U | the identity check under fault injection |
| G-PKT-4 | count rules: more packets than `packets_per_unit` are rejected at submit (`PKT_COUNT`), short units flagged (`MTL_TXR_PKT_SHORT`), the grid kept | U | loopback with scripted counts |
| G-PKT-5 | RX never holds more than `packet.rx_ring_packets` NIC buffers; a full ring on one leg is filled by the other | U, I | `MTL_FAULT_DROP_PKTS` per leg plus a stalled reader |
| G-PKT-6 | dedup by sequence number: one delivery per sequence, `MTL_PKT_RX_NO_DEDUP` delivers both (`MTL_PKTE_REDUNDANT`), `gap` exact | U | loopback with injected loss and reorder |
| G-PKT-7 | `MTL_PKT_PACE_UNIT` passes the ST 2110-21 narrow check for video when the declared count is sent; `MTL_PKT_TIME_FROM_RTP` keeps a fixed RTP-to-launch offset | I | the timing oracle, EBU LIST |
| G-PKT-8 | call classes: a copying dequeue from a busy loop is `-MTL_EDEADLK`; `MTL_PKT_RX_LEND` dequeue and the TX verbs are inline-safe | U | busy-loop harness |

Further S8 tests: a pcap replay test (2P-b, I tier): RxTxApp on the unified path replays a reference
ST 2110-20 pcap and the existing ST 2110-22 pcap (`tests/tools/RxTxApp/script/loop_json/st22p_pcap.json`)
through packet sessions with verbatim headers and `MTL_PKT_TIME_FROM_RTP` into frame RX sessions; the
digest must match and the TX timing must pass the oracle. The out-of-order and truncated-frame cases
of `tests/integration_tests/st20/st20_digest.cpp:567` and `st20_meta.cpp:256` move to the U tier
through packet units, and the ST40 RTP fuzz target is re-pointed at the packet RX path.

## 5. Kubernetes: K-REQ-1…20

From the Kubernetes runtime study (K1 §10); each rests on a runtime fact of K1 (cited as K1 §x) and,
where today's MTL falls short, on a gap of §5.1. The design that meets them is
[deployment.md §4](deployment.md#4-kubernetes); the engine fixes EK1–EK21 are
[engine.md §11](engine.md#11-the-engine-change-list); the open questions Q-K8S-1…11 are decision M16
([decisions.md](decisions.md)). After review RK all twenty are met in the design, several only
through engine fixes and the node settings of [deployment.md §4.14](deployment.md#414-files-and-privileges).
Phase: the instance close and health are in the branch; the rest is the "Kubernetes items" row of
[implementation-plan.md §6](implementation-plan.md#6-m7-the-remaining-phases) (engines track,
Phases 1–2).

| ID | Requirement (K1 section; gap of §5.1) |
|---|---|
| K-REQ-1 | **bounded orderly shutdown**, MUST: one call stops a whole instance within a caller-given deadline: TX drained to a frame boundary, the rest flushed, every session retired, MtlManager released, devices closed; never later than the deadline; the typical cost fits well inside the default 30 s grace period after a `preStop` hook (K1 §1.1; gap G1) |
| K-REQ-2 | **network first**: within the budget (1) TX stops at the next frame boundary, (2) IGMP leaves on every leg, (3) MtlManager resources released, (4) device and memory teardown, which may be cut short because the kernel cleans up; steps 1–2 in about one frame time plus a few ms (K1 §1.4, §7.1) |
| K-REQ-3 | **signal-safe trigger, no handlers**: the library installs no signal handlers and documents the SIGTERM recipe (the handler calls the AS `mtl_instance_interrupt`, the main thread the bounded close), the PID 1 rule (handle SIGTERM explicitly, or run `tini`) and DPDK's temporary SIGBUS handler at open (K1 §1.6) |
| K-REQ-4 | **crash-only correctness**: SIGKILL at any instant leaves nothing that blocks a restart or needs cleanup; pod- and host-scoped leftovers (SysV shm, files in `/tmp` or hugetlbfs, manager state) are avoided or owned by a party that reclaims them on disconnect; each residual is listed with owner and duration (IGMP timeout, node-owned XDP programs) (K1 §1.2, §1.4; gaps G2, G3) |
| K-REQ-5 | **restart in place**: `mtl_instance_open` succeeds in a restarted container of the same pod, with the same VF just reset by vfio, the same IPC namespace and emptyDirs, without depending on PIDs of a previous run; CPU arbitration without a manager uses the affinity mask, not the shm table (K1 §1.4; gap G2) |
| K-REQ-6 | **fail fast, with a reason**: open checks all it can before touching a device (VF and driver, IOMMU mode, hugepages within the **cgroup** limit, memlock or `CAP_IPC_LOCK`, capabilities, CPU set) and fails with one named reason and a one-line text for `terminationMessagePath`, so a CrashLoopBackOff (backoff up to 300 s) is diagnosable (K1 §1.3, §3.1, §6; gap G7) |
| K-REQ-7 | **liveness, readiness and startup signals**: one cheap, lock-free call, any thread and rate. Liveness: every scheduler loop advanced within N ms, control thread not wedged, no fatal device reset. Readiness: links up, time locked (or within its error), started sessions RUNNING. Startup: the phase reached. Liveness never depends on packets or PTP lock (K1 §1.3, §8; gap G5) |
| K-REQ-8 | **CPUs from the affinity mask**: with no `lcores` the instance uses `sched_getaffinity` of the opening thread (DPDK's own default); an explicit CPU outside it fails with a named reason; CPU IDs, not lcore IDs, are reported; MtlManager arbitration is off by default inside an exclusive cpuset (K1 §4) |
| K-REQ-9 | **every thread pinned, in every mode**: each scheduler pinned to exactly one CPU, `TASKLET_THREAD` included; non-scheduler threads on a housekeeping CPU, named or taken from the mask; placement reported, with a warning when SMT siblings of a scheduler CPU are used by others (`full-pcpus-only` absent) (K1 §4; gap G4) |
| K-REQ-10 | **budget against the cgroup, allocate at open**: hugepage capacity checked against the container's hugetlb limit, not host `/sys`; every pool faulted in at create, so an over-limit is `-MTL_ENOMEM` at create, never a SIGBUS later; memlock checked against the VFIO mapping budget (`dma_entry_limit`, `RLIMIT_MEMLOCK` without `CAP_IPC_LOCK`, the user-namespace caveat) (K1 §3.1, §3.2) |
| K-REQ-11 | **ports from Kubernetes**: a port names a device-plugin variable (`env:PCIDEVICE_…`, the Nth address) with a source IP and prefix from the IPAM result; MTL uses the VF's current admin MAC and never sets one; a helper parses the Multus `network-status` file from the downward API (K1 §2.1, §2.2; gap G6) |
| K-REQ-12 | **VF capabilities probed and reported** at open: trust (promiscuous, all-multicast), the MAC filter budget (18 on an untrusted E810 VF), spoof check, a PF-set TX rate, hardware pacing and TM, PHC readability; a session that needs more (the 17th group on an untrusted VF) fails at create with a named reason, not inside the join (K1 §2.4) |
| K-REQ-13 | **no assumptions about a VF's history**: a VF may be reused; MTL assumes nothing but what K-REQ-12 reads; on close it removes what it added (multicast filters, flow rules, TM); PF admin state is read only (K1 §2.3) |
| K-REQ-14 | **an explicit IOMMU policy**: no-IOMMU or PA mode refused unless an instance option opts in by name; the IOVA mode reported in port capabilities; the documentation marks no-IOMMU pods privileged and node-trusting (K1 §3.3) |
| K-REQ-15 | **time sources that fit a pod**: AUTO means the VF-readable PHC when the node disciplines it, then `CLOCK_TAI` with a non-zero kernel offset; the built-in PTP client only when named (D-85); the instance reports the source, who disciplines it, its lock state and estimated error; no source needs `CAP_SYS_TIME` or `/dev/ptp*`; MTL never adjusts a host clock (K1 §5) |
| K-REQ-16 | **a minimal privilege profile per backend**, checked at open. vfio: the plugin's device nodes, `IPC_LOCK`, maybe `SYS_NICE`; AF_XDP: `NET_RAW` and a node agent's socket or map; kernel: nothing. All non-root, read-only root fs (writes only to a runtime directory, never `/tmp`, `/var/run/dpdk` or hugetlbfs), seccomp `RuntimeDefault`, no host IPC, PID or network for vfio (K1 §6, §8) |
| K-REQ-17 | **MtlManager optional and pod-safe**: a pod needs no manager by default; when used (AF_XDP on shared netdevs): identity from `SO_PEERCRED` or a per-pod socket, never PIDs; every grant (CPUs, queues, flows, UDP filters) released when its connection closes; interfaces named by host ifname or PCI address; the client survives a manager restart and re-registers (K1 §6, §7.2; gap G3) |
| K-REQ-18 | **the node owns the AF_XDP program**: in a pod the library never attaches or detaches XDP programs; it takes a pre-created XSK socket or map from a node agent (MtlManager, the AF_XDP device plugin, bpfman), passed over SCM_RIGHTS, inherited or pinned, and says clearly when there is none (K1 §7.2) |
| K-REQ-19 | **multi-container and multi-process pods**: one instance per process; two containers, each with its own VF, hugepage limit and CPUs, never see each other through shared IPC, `/tmp` or lockfiles (follows from K-REQ-5 and -16); a sidecar MTL gateway that terminates last (KEP-753) covers in its bounded close clients that never said goodbye (K1 §1.4, §1.5, §3.1) |
| K-REQ-20 | **multicast that tolerates crashes**: IGMP reports answer queries and stay periodic without one; report interval and IGMP version are options; leaves on every leg at an orderly close; the documentation states the stale-flood window after SIGKILL (the switch's group membership interval, 260 s by default) and asks operators for an IGMP querier and fast-leave on media VLANs (K1 §7.1) |

| ID | MTL's part | How met | Phase | Tested by | Notes |
|---|---|---|---|---|---|
| K-REQ-1 | API, engine | `mtl_instance_close(mt, timeout_ns)`; `mtl_instance_shutdown` (`MTL_SHUTDOWN_DRAIN`, the report); EK1, EK2, EK21 ([deployment.md §4.2](deployment.md#42-shutdown)) | M0, M1; report M3a | G-107 | K1 asked for `-MTL_ETIMEDOUT`; revision 4: 0, 1 or `-MTL_EIO`, and the report names what was skipped. Default: finish the unit on the wire, flush the rest (Q-K8S-10) |
| K-REQ-2 | engine | the step order of [deployment.md §4.2](deployment.md#42-shutdown); EK9 (leaves before the ports close); `groups_left` in the report | M3a | G-108 |  |
| K-REQ-3 | API | rule R8 of `mtl.h`; AS `mtl_instance_interrupt`, `mtl_instance_abort` (a second SIGTERM); `instance.hotplug` installs DPDK's SIGBUS handler only by option (Q-K8S-5); ex11 ([deployment.md §4.4](deployment.md#44-signals-and-fork-r8)) | M0 | G-111 |  |
| K-REQ-4 | engine | R8: everything outside the process is tied to a descriptor the kernel closes or reconciled at the next open, nothing found by PID, close-on-exec, `MADV_DONTFORK`, the fork rule; EK6, EK12, EK15; the crash contract and residuals table ([deployment.md §4.5](deployment.md#45-the-crash-contract)) | engines track; M5 (I) | G-111 (fork), G-112 (SIGKILL, re-open) |  |
| K-REQ-5 | engine | reconcile at open (`instance.reconciled{kind}`); EK6 (SysV table and `kill(pid, 0)` removed), EK16 (VF flows and TM flushed at open); `instance.cpu_arbitration` | engines track; M5 (I) | G-112 |  |
| K-REQ-6 | API | the check order of `mtl_instance_open`; reasons 600–614; `mtl_error_info.detail` ([deployment.md §4.9](deployment.md#49-fail-fast-open)) | M16 package; EK18 with M5 wave 4 | G-57 (each reason by its trigger) | a preflight flag was rejected: an init container has its own CPUs, hugepage limit and memlock, so it would check the wrong container |
| K-REQ-7 | API, engine | `mtl_instance_get_health` (DP), `MTL_HEALTH_LIVENESS` and `MTL_HEALTH_READINESS` masks, `enum mtl_phase`, `instance.stall_ns` (1 s), `MTL_EVENT_HEALTH`, `MTL_EVENT_SCHED_STALLED`; EK11; ex11 ([deployment.md §4.10](deployment.md#410-health-and-probes)) | M3a | G-109 | readiness fails only on what stops the whole instance; one bad stream is `MTL_HEALTH_DEGRADED` |
| K-REQ-8 | engine | `mtl_instance_params.lcores` NULL = the mask; `CPU_NOT_ALLOWED` (606); EK7; `instance.cpu_arbitration` auto ([deployment.md §4.7](deployment.md#47-cpus)) | engines track | — | open: auto picks MtlManager whenever its socket is mounted, also in an exclusive cpuset (RK-28, OI-48) |
| K-REQ-9 | engine | `instance.main_lcore` (default the first CPU of the mask, never given a scheduler); EK7, EK13 (the caller's threads and memory policy are never changed); `instance.cpu_shared` (WARN in v1, Q-K8S-9), `CPU_SHARED` (607); EK17 (a CFS quota below the mask) | engines track | — | K1 suggested the last CPU of the mask; the design takes the first |
| K-REQ-10 | engine | `HUGEPAGES_LIMIT` (603), `MEMLOCK_LIMIT` (604) at open; `HUGEPAGES` (308) at create; keys `mem.hugetlb_limit_bytes`, `mem.hugetlb_usage_bytes`, `mem.memlock_limit_bytes` ([deployment.md §4.11](deployment.md#411-memory-and-the-iommu), [§5](deployment.md#5-hugepage-budgeting)) | engines track | — |  |
| K-REQ-11 | API | `mtl_port_spec.name = "env:VAR#n"`, `PORT_ENV_UNSET` (608); `MTL_PORTS` with `env:…#0=ip/prefix`; `mtl_port_spec.mac` is output only; `mtl_port_specs_from_network_status` (`mtl_util.h`, `MTL_LATER`) | M0 (env ports); the helper later | — | one device-plugin resource per ST 2022-7 network, so each leg's VF comes from its own PF |
| K-REQ-12 | engine | `caps.vf_trusted`, `caps.mcast_filters_max`, `caps.pf_tx_rate_mbps`, `caps.phc_readable`, `caps.iova_mode`, `caps.reset_budget_ns`; `VF_UNTRUSTED` (612) at open; `-MTL_ENOSPC`, `MCAST_FILTERS` (115) at create or update | engines track | — | spoof check has no key yet |
| K-REQ-13 | engine | EK16 (flush and verify at open), EK9; MTL never sets the MAC ([deployment.md §4.5](deployment.md#45-the-crash-contract)) | engines track | — |  |
| K-REQ-14 | engine | `instance.allow_noiommu`, `NO_IOMMU` (600), `caps.iova_mode`; EK8 | M5 wave 4 | — (CI hosts set the option, recorded in the job) | behaviour change for no-IOMMU users; the legacy API only warns (Q-K8S-6) |
| K-REQ-15 | API | `enum mtl_time_source`; keys `time.source`, `time.disciplined_by`, `time.state`, `time.error_ns`; `time.phc_trust`, `mtl_time_set_reference`; EK10; reasons 613, 614 ([deployment.md §4.8](deployment.md#48-time-in-a-pod)) | Phases 1–2 | G-109, G-54 | K1's "then nothing" became `SYSTEM_TAI` (ESTIMATED), FREERUN in Phase 7; node discipline by the agreement test and `locked` (Q-K8S-8) |
| K-REQ-16 | API, node setup | `CAPABILITY_MISSING` (605); `instance.runtime_dir`, `RUNTIME_DIR` (609); EK12; set-ups A–C ([deployment.md §4.14](deployment.md#414-files-and-privileges)), §4.15 | engines track | — | set-up A recommended, B the fallback (Q-K8S-11); today's practice is C (privileged); `IPC_LOCK` puts a pod outside the Baseline PSS |
| K-REQ-17 | engine | EK4, EK5; `MANAGER_REQUIRED` (610); `MTL_EVENT_MANAGER_LOST`, `MTL_HEALTH_MANAGER_LOST` ([deployment.md §4.12](deployment.md#412-mtlmanager-in-kubernetes)) | Phase 2 | G-98, G-110 |  |
| K-REQ-18 | engine | `port.xsk_map`, `XSK_UNAVAILABLE` (611); EK19 ([deployment.md §4.13](deployment.md#413-af_xdp-in-pods)) | engines track | — | open: who sets the AF_XDP queue rate in an unprivileged pod ([deployment.md §4.18](deployment.md#418-open-points)) |
| K-REQ-19 | API | one process per instance (NG5); no shared files (EK6, EK12, `instance.runtime_dir`) ([deployment.md §2](deployment.md#2-processes-and-instances)) | engines track | — | a sidecar gateway's own clients are its concern, not MTL's |
| K-REQ-20 | engine | `port.igmp_version` (`MTL_IGMP_V2`, `_V3`; also IPMX GI-12), `port.igmp_report_ms`; EK9; residual 1 of [deployment.md §4.5](deployment.md#45-the-crash-contract) | engines track | G-108 (leaves) | leaves sent by the manager for a crashed client are later (Q-K8S-7) |

### 5.1 The gaps of today's MTL behind them (K1 §9)

| Gap | Today | Closed by |
|---|---|---|
| G1 | no instance-wide stop that respects a deadline; per-session closes add up | K-REQ-1 |
| G2 | the SysV-shm lcore table and the `/tmp` lock assume a host: they survive container restarts, fail the PID check and need a writable `/tmp` (`mt_sch.c:506-605`, `:685-699`) | K-REQ-4, -5 (EK6) |
| G3 | MtlManager identity comes from the message, the socket is world-writable and needs a hostPath, the manager is blind to network namespaces for AF_XDP, and UDP filter refcounts leak on client death | K-REQ-4, -17 (EK5) |
| G4 | `TASKLET_THREAD` schedulers are unpinned, which is wrong on CPUs with load balancing disabled | K-REQ-9 (EK7) |
| G5 | no health or progress signal for a liveness probe | K-REQ-7 (EK11) |
| G6 | no way to name a port by its Kubernetes resource or take the IP from the IPAM result (`MTL_PORTS` exists) | K-REQ-11 |
| G7 | no up-front check of VF trust, MAC filter budget, memlock and capabilities; failures surface late or as generic errors | K-REQ-6, -12 |

The pod hazards H-K-1…28 found in today's code, with the fix of each, are
[engine.md §12.3](engine.md#123-pod-hazards-h-k) and [deployment.md §4.16](deployment.md#416-todays-code-at-545a266a).

## 6. NMOS: N-REQ-1…60

From the NMOS requirements study (N1 §3): what MTL must give an AMWA NMOS Node so that the Node can
be conformant. The split of work between the Node and MTL, the activation contract and the gap
dispositions G-N1…G-N25 are [nmos-ipmx.md](nmos-ipmx.md) (§2, §4, §14.1); its §15.1 index groups
these rows. Pri: MUST = the Node cannot meet a normative NMOS or SMPTE rule without it; SHOULD = the
Node can work around it at a cost. Phase 7 items are declared already (fields, keys, events, or
`MTL_LATER` functions), so the API does not change when the code comes.

| ID | Requirement (source) | Pri | MTL's part | How met | Phase | Tested by | Notes |
|---|---|---|---|---|---|---|---|
| N-REQ-1 | report width, height, scan and exact rational rate of every video session (IS-04 `flow_video.json`, `flow_core.json`) | MUST | API | `mtl_session_get_config()`: `video.raster` (`rate`, or the rational `fps`) | v1 | — | |
| N-REQ-2 | carry and report colorimetry, transfer characteristic and range (IS-04 Flow `colorspace`, required; ST 2110-20 §7.2, §7.3; BCP-006-01) | MUST | API | `video.colorimetry`, `tcs`, `range` (and in `cvideo`), enums in `mtl_format.h`; zero is the standard's default; colorimetry is always rendered; they may change while running when no conversion uses them | 7 | — | G-N10; RN-14, RN-15 |
| N-REQ-3 | report the sampling and bit depth of the transport format (IS-04 `flow_video_raw.json`; ST 2110-20 §7.4) | MUST | API | `video.format`; `mtl_format_names()`, `mtl_format_pgroup()` | v1 | — | |
| N-REQ-4 | report audio sample rate, bit depth, channel count and packet time (IS-04 `flow_audio*.json`; BCP-004 `packet_time`) | MUST | API | `audio.sample_rate`, `format`, `channels`, `ptime` | v1 | — | |
| N-REQ-5 | report the media type and format of each essence (IS-04 Flow `media_type`) | MUST | API | `essence` and its format | v1 | — | ST 2110-41 has no NMOS media type: publish `urn:x-nmos:format:data` with a vendor value and flag it to AMWA (Q-NMOS-6) |
| N-REQ-6 | report the granted per-leg port, SSRC, PT, UDP source and destination ports, source and destination MACs (IS-05 `/active`; SDP) | MUST | API | `mtl_session_info.leg[]`: `port`, `ssrc`, `udp_src_port`, `udp_dst_port`, `payload_type`, `src_mac`, `dst_mac` (zero while unresolved) | v1 | — | |
| N-REQ-7 | report TP, TROFF and whether it is TRODEFAULT, CMAX, TSMODE, TSDELAY (ST 2110-21 §6.2, §8; ST 2110-10 §8.7) | MUST | API | keys `info.sender_type`, `info.troffset_ns`, `info.cmax`, `info.tsmode`, `info.tsdelay_ns`; `info.troffset_default` | v1; the default flag 7 | — | G-N15 |
| N-REQ-8 | report the maximum UDP datagram size in use (ST 2110-10 §8.6, MAXUDP) | MUST above 1460 B | API | `info.max_udp_bytes` (the input is `session.max_udp_payload`) | 7 | — | G-N15 |
| N-REQ-9 | report the bit rate with RTP/UDP/IP overhead, and the payload bit rate (BCP-006-01 Sender and Flow `bit_rate`; SDP `b=AS`) | MUST for ST 2110-22 | API | `info.wire_kbps{leg}`, `info.payload_kbps`, in kbps rounded up as the NMOS register defines `bit_rate` | 7 | — | G-N15 |
| N-REQ-10 | report the PTP grandmaster identity and domain per port (ST 2110-10 §8.2; IS-04 `clock_ptp.json` `gmid`) | MUST | API | port keys `time.grandmaster_id`, `time.ptp_domain`; `info.ts_refclk.*{leg}` | v1; `ts_refclk` 7 | — | |
| N-REQ-11 | report lock state and traceability (grandmaster `timeTraceable`, `clockAccuracy`) (IS-04 `locked`, `traceable`; ST 2110-10 §8.2) | MUST | API | `time.state`; `time.gm_traceable`, `time.gm_clock_class`, `time.gm_clock_accuracy`, `time.gm_priority1` (gauges: they change at runtime) | v1; `gm_*` 7 | G-54 (lock state) | G-N11 |
| N-REQ-12 | report the time metadata also when ptp4l and phc2sys discipline the PHC (ST 2110-10 §8.2) | MUST | API | `mtl_time_set_reference(mt, port, &ref)` with `struct mtl_time_reference` (gmid, domain, traceable, clock class and accuracy, locked), read by the application from ptp4l through pmc | 1–2 | — | G-N11; the pod time rules need it too |
| N-REQ-13 | signal grandmaster changes (BCP-008 `synchronizationSourceId`; SDP and Sender `version` change) | SHOULD | API | `MTL_EVENT_GRANDMASTER` (27); polling `time.grandmaster_id` works meanwhile | event 1–2; IS-04 use 7 | — | G-N12 |
| N-REQ-14 | report each port's MAC and the port count (IS-04 `interfaces[].port_id` MUST be a MAC) | MUST | API | `mtl_port_spec.mac` (output of `mtl_port_get_spec()`), `instance.port_count` | 7 | — | G-N13 |
| N-REQ-15 | send LLDP and report the LLDP neighbour on DPDK-owned ports (IS-04 `interfaces` "ideally LLDP", `attached_network_device`) | MAY | none | not adopted: `chassis_id = null` is allowed; topology tools read the switch | later | — | G-N14 deferred |
| N-REQ-16 | answer ARP on media ports (IS-04 `node.json`: "ARP at a minimum") | MUST | engine | built-in ARP, as today | v1 | — | |
| N-REQ-17 | map a leg to its interface (IS-04 `interface_bindings`) | MUST | API | `info.leg[i].port`, `mtl_port_get_spec()` | v1 | — | |
| N-REQ-18 | change destinations, sources, ports and PT of every leg at one TAI instant or now, all or nothing (IS-05 activation; RAML `/active` must not change on error) | MUST | API, engine | `mtl_session_update(s, sc, MTL_UPDATE_FLOWS, &when, &planned)`; ex13 | 2 | G-75 | replaces today's `update_destination` and `update_source` |
| N-REQ-19 | report the instant a scheduled activation will take effect and an immediate one did (IS-05 RAML 200 and 202 `activation_time`) | MUST | API | `planned_tai_ns` from the call; `status.update_state`, `update_applied_tai_ns`, `update_seq`; `MTL_EVENT_UPDATE` (26) | 2 | — | G-N1, reduced: a separate getter and first-index and first-RTP fields were rejected |
| N-REQ-20 | let the Node answer an immediate activation only after it applied (IS-05 RAML 200) | MUST | API | the call returns once posted; the Node waits for `MTL_EVENT_UPDATE` APPLIED or `update_applied_tai_ns`; the switch happens at the slot boundary by the clock, unit or not, so an idle or muted sender reaches APPLIED | 2 | — | G-N1; RN-1 |
| N-REQ-21 | re-apply identical parameters on re-activation (IS-05 Behaviour, Re-Activating; it suggests IGMP leave and join) | MUST | API | `MTL_UPDATE_REAPPLY`: RX re-sends its membership reports and re-arms its rules without a leave, TX re-resolves the neighbour and rebuilds its headers | 7 | — | G-N2; RN-12; Q-NI-10 |
| N-REQ-22 | cancel a pending scheduled activation (IS-05 RAML 200, "an activation has been cancelled") | MUST | API | `mtl_session_update(s, NULL, 0, NULL, NULL)`: 0 cancelled, 1 none pending, `-MTL_EBUSY` (`UPDATE_COMMITTING`) when it already applies | 7 | — | G-N8; RN-28 |
| N-REQ-23 | replace a pending scheduled activation (IS-05 re-staging) | MUST | none | the Node's job: IS-05 answers 423 while a scheduled activation is pending; MTL's REPLACED serves non-NMOS callers | — | — | RN-11 |
| N-REQ-24 | enable and disable each leg, also at a scheduled instant and with a flow change (IS-05 `rtp_enabled`) | MUST | API | `sc.legs_disabled` with `MTL_UPDATE_LEGS`, combined with `MTL_UPDATE_FLOWS` under one `when` | 2 | G-91 | |
| N-REQ-25 | stop and resume the whole transmission or reception (`master_enable`), immediately or scheduled (IS-05 `master_enable`) | MUST | API | mute: every existing leg disabled; RUNNING, `MTL_STATUS_MUTED`, TX units counted in `tx.units_muted`, RX groups left; `when` is honoured. `master_enable` itself stays the Node's | 7 (Phase 2: stop and start) | — | G-N3; RN-10; C-N2, C-N7 |
| N-REQ-26 | exist before any connection: a receiver with no address, a sender with no destination (IS-04 resources at boot; IS-05 null address) | MUST | API | a reserved leg (its `legs_disabled` bit set, the flow all zero until `MTL_UPDATE_FLOWS \| MTL_UPDATE_LEGS` addresses it); sessions created at boot, every leg reserved and disabled, started, so muted | 7 | — | G-N4 (no extra field); C-N4 |
| N-REQ-27 | receive unicast and filter on the unicast source (IS-05 receiver `multicast_ip = null`, `source_ip`) | MUST | API | RX `flows[i].ip` is the port's own address or all zero; `source_filter` then checks the sender; a group address means multicast | 1 | — | G-N23 |
| N-REQ-28 | receive SSM with one source filter per leg; send IGMPv3 (IS-05 `source_ip`; RFC 4570) | MUST | engine | `mtl_flow.source_filter` per leg (it must filter on DPDK too, issue #1239); IGMPv3 by default | v1 | — | |
| N-REQ-29 | two legs on one interface (IS-04 `interface_bindings` listing an interface twice; RFC 7104 separate source and destination) | SHOULD | API | an explicit `flows[1].port` | 7 | — | Q-NMOS-3, Q-NI-6; C-N12 |
| N-REQ-30 | accept a one-leg configuration on a two-leg session and back, without a stop (IS-05 ST 2022-7 rules) | MUST | API | the reserved leg of N-REQ-26 plus `MTL_UPDATE_LEGS` | 7 (Phase 2: stop and start) | — | G-N4 |
| N-REQ-31 | change the interface of a running session (IS-05 `source_ip` / `interface_ip` change) | SHOULD | engine | `MTL_UPDATE_FLOWS` with a port change, make before break; `-MTL_ENOSPC` (`CAPACITY_*`) if it cannot reserve, `-MTL_EBUSY` (`PORT_CHANGE_NEEDS_STOP`) on a backend that cannot | 7 | — | G-N5; C-N3 |
| N-REQ-32 | change the received format at an activation (a new SDP, IS-05 receiver `transport_file`) | SHOULD | API | later: an RX `MTL_UPDATE_MEDIA` at an instant. Meanwhile `MTL_UPDATE_MEDIA` in STOPPED, or the A/B swap of two sessions ([nmos-ipmx.md §4](nmos-ipmx.md#4-is-05-the-activation-contract)); colorimetry, TCS and range may change while running | later | — | G-N6; C-N4 |
| N-REQ-33 | validate staged parameters without preparing them (IS-05 PATCH 400 for invalid staging) | SHOULD | API | `MTL_UPDATE_DRY_RUN`: validates and plans (`planned_tai_ns`), reserves and posts nothing, leaves the status untouched; a later update may still fail with `-MTL_ENOSPC` | 7 | — | G-N7; RN-9 |
| N-REQ-34 | bound the double bandwidth of a far-ahead scheduled receiver switch (IS-05 scheduled activation; link capacity) | SHOULD | API | `rx.join_lead_ns`: an `MTL_AT_TAI` update joins at max(call, t − lead) | 7 | — | G-N9; C-N13 |
| N-REQ-35 | treat a scheduled time in the past as now; accept any future time (IS-05: "when internal clock ≥ requested_time") | MUST | API | an `MTL_AT_TAI` or `MTL_AT_INDEX` already past means `MTL_NOW`; no horizon for updates; in ARMED a `when` before T0 means T0 | 2 | — | G-N25 |
| N-REQ-36 | resolve `auto` transport parameters and report the resolved values (IS-05 RAML `/active`) | MUST | API | the Node resolves; MTL reports the granted values (`info.leg[]`, `mtl_port_get_spec()`) | v1 | — | C-N11: the Node sets the source port, never a multi-port mode |
| N-REQ-37 | read the instance TAI clock for versions and relative activations (IS-04 `version`; IS-05 relative) | MUST | API | `mtl_time_now()` | v1 | — | |
| N-REQ-38 | apply SDP `mediaclk:direct=<offset>` and `mediaclk:sender` at an activation (IS-05 transport file; ST 2110-10 §8.3) | SHOULD | API | `rx.rtp_offset` and `rx.mediaclk` are R options: in the update's `sc->options` they apply at its boundary | 7 | — | G-N18; RN-3; C-N10 |
| N-REQ-39 | generate a complete ST 2110 SDP from a session (ST 2110-10 §8.1; IS-05 `/transportfile`; IS-04 `manifest_href`) | SHOULD (MUST for the Node) | API | `mtl_sdp_render()` (`mtl_sdp.h`, `MTL_LATER`) | 7 | — | G-N17; open: RN-17 (`a=privacy`, `a=infoframe`), [nmos-ipmx.md §16](nmos-ipmx.md#16-open-items-for-phase-7) |
| N-REQ-40 | parse an SDP into a session configuration: legs, PT, filters, essence (ST 2110-10 §8.1; IS-05 `transport_file`) | SHOULD | API | `mtl_sdp_parse()`; legs beyond those parsed are zeroed and their `legs_disabled` bits set | 7 | — | G-N17; RN-18, RN-19 |
| N-REQ-41 | report per-port link state and its changes (BCP-008 `linkStatus`) | MUST | API, engine | `port.link_up`, `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE`; the link monitor | 2 | G-91 | |
| N-REQ-42 | RX: report packets arriving, loss per leg and after the ST 2022-7 merge, and the use of redundancy (BCP-008-01 `connectionStatus`, `GetLostPacketCounters`) | MUST | API | `MTL_STATUS_RX_SIGNAL`, `MTL_EVENT_RX_SIGNAL`, `leg.pkts_lost{leg}`, `rx.pkts_lost_est`, `rx.units_used_redundancy`, `rx.units_incomplete_*` | v1 | G-42 | |
| N-REQ-43 | RX: count packets that arrived too late, and units late for presentation (BCP-008-01 `GetLatePacketCounters`) | SHOULD | API | `rx.pkts_stale`, `rx.units_stale`; `leg.pkts_late{leg}`, `rx.units_late_presentation` | v1; per leg 7 | — | G-N16 |
| N-REQ-44 | RX: report stream validity: PT, SSRC, length, format against the expectation (BCP-008-01 `streamStatus`; IS-11 `non_compliant_stream`) | MUST | API | `rx.pkts_rejected{cause}`, `MTL_STATUS_FORMAT_CHANGED`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*`; ST 2110-21 `tp.*` | v1 | — | |
| N-REQ-45 | TX: report recoverable and unrecoverable transmission errors (BCP-008-02 `transmissionStatus`, error counters) | MUST | API | `tx.units_late`, `tx.units_dropped{reason}`, `tx.units_failed`, `leg.pkts_skipped`, `port.tx_errors`, `MTL_STATUS_PACING_DOWNGRADED`, `MTL_EVENT_TIMING_INFEASIBLE`, `MTL_FLOW_WAITING_NEIGHBOUR` | v1 | G-57 | muted units: `tx.units_muted` (RN-32), status OI-56 |
| N-REQ-46 | TX: report missing essence (BCP-008-02 `essenceStatus`; IS-11 `no_essence`) | SHOULD | API | `MTL_EVENT_TX_UNDERRUN`, `tx.slots_empty` | v1 | — | |
| N-REQ-47 | report the PTP state per port for multi-interface sync health (BCP-008 `externalSynchronizationStatus`, PartiallyHealthy) | MUST | API | `time.state`, `time.offset_ns` on each port object; `MTL_EVENT_TIME_STATE` | v1 (Phases 3, 5) | G-54 | C-N9 |
| N-REQ-48 | read counters cheaply and consistently at any time (BCP-008: counters reset on activation, a Node baseline) | MUST | API | `mtl_stat_read()` (DP, one snapshot, no lock a tasklet takes); counters never reset, the Node keeps its baseline | M3a | G-40, G-42 | C-N8 |
| N-REQ-49 | a stable session identity across reconfiguration, with a 36-character UUID as its name (BCP-008: touchpoints 1:1 for the resource's life) | MUST | API | `sc.name` (64 B, unique per instance), kept by `MTL_UPDATE_MEDIA` | M3a | G-88, G-87 | |
| N-REQ-50 | dry-run any candidate configuration with capacity (BCP-004-01/-02 constraint sets; IS-11) | MUST | API | `mtl_session_query(…, MTL_QUERY_CHECK_CAPACITY, …)` | 2 | G-89 | |
| N-REQ-51 | never degrade a published sender type or pacing silently (BCP-004-02: "MUST not produce … incompatible") | MUST | API | `caps.pacing_required` (fail instead of fall back), `MTL_EVENT_PACING_CHANGED` | v1 | G-26, G-34 | C-N14 |
| N-REQ-52 | reconfigure a sender's format keeping its identity (IS-11 active constraints) | MUST | API | `MTL_UPDATE_MEDIA` in CREATED or STOPPED | 2 | G-87 | |
| N-REQ-53 | mute a sender on a constraint violation and refuse activation while it lasts (IS-11: "MUST become inactive") | MUST | none | Node policy over the mute of N-REQ-25 | 7 | — | |
| N-REQ-54 | receive without an SDP, from transport parameters only (IS-05 receivers; IS-11 `unknown`) | SHOULD | API | `video.detect = MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT` | M3a | — | |
| N-REQ-55 | PTP domain and announce timeout from IS-09 at start-up (IS-09 `global.json`) | SHOULD | API | `time.ptp_domain`; `time.ptp_announce_timeout` (2209), built-in client only | v1; timeout 7 | — | G-N18 |
| N-REQ-56 | remap or select audio channels when building TX units (IS-08) | MAY | API | the application copies; `mtl_audio_remap()` (`mtl_convert.h`, `MTL_LATER`); the IS-08 map is applied by the application at a media index | later | — | G-N22 |
| N-REQ-57 | report the ANC DID/SDID seen on a received stream (IS-04 Flow `DID_SDID`, for receivers that forward) | MAY | API | `anc.did_sdid_seen` (up to 16 pairs) | 7 | — | G-N19 |
| N-REQ-58 | IPv6 media addresses (IS-05 schemas allow IPv6) | MAY | none | reserved: `ip_family = 6`; the Node constrains to IPv4 | — | — | |
| N-REQ-59 | ST 2022-5 FEC and RFC 3550 RTCP (IS-05 optional parameter sets) | MAY (IPMX: RTCP MUST) | API | RTCP: IPMX sender reports only, `mtl_rtcp.h` (§7, GI-1…GI-3); FEC: not adopted, the Node omits the FEC parameter set | RTCP 7 | — | G-N20, G-N21 |
| N-REQ-60 | report a port address change, a DHCP renew (SDP `c=` and `source-filter`; BCP-008 connection status) | MAY | API | `MTL_EVENT_PORT_ADDRESS` (28), then `mtl_port_get_spec()`; with `port.dhcp` | 7 | — | G-N24 |

The N1 conflicts C-N1…C-N14 and their resolutions are [nmos-ipmx.md §14.3](nmos-ipmx.md#143-conflicts).
The guarantees named test the transport half of a row; the Phase 7 rows get theirs with the code.
NMOS conformance itself is tested on the Node, with the AMWA nmos-testing suite (IS0502 among others).

## 7. IPMX: I-REQ-1…76

From the IPMX requirements study (I1 §3): what VSF IPMX (TR-10) needs from MTL. Level is the TR
keyword. The profile, the sender reports, the wire details, timing without PTP and PEP are
[nmos-ipmx.md §9–§13](nmos-ipmx.md#9-the-ipmx-profile); the gap dispositions GI-1…GI-17 are
[§14.2](nmos-ipmx.md#142-ipmx-gaps-i1); the open items left by the verification are
[§16](nmos-ipmx.md#16-open-items-for-phase-7). Everything IPMX-specific is Phase 7 (later), after the
ABI freeze; its keys, enums, events and reserved fields are declared now. "Profile" means
`session.profile = MTL_PROFILE_IPMX` (or `instance.profile`). No IPMX requirement has a contract
test yet except where a core guarantee covers it.

| ID | Requirement (source) | Level | MTL's part | How met | Phase | Tested by | Notes |
|---|---|---|---|---|---|---|---|
| I-REQ-1 | work in networks with and without a common reference clock (TR-10-1 §7) | MUST | API, engine | PHC and `PTP_BUILTIN` today; `MTL_TIME_SOURCE_FREERUN` and AUTO re-evaluated while running | v1; 7 | — | GI-5 |
| I-REQ-2 | no PTP: the sender keeps a free-running internal clock (TR-10-1 §7.1) | MUST | engine | `MTL_TIME_SOURCE_FREERUN`: seeded once, never stepped, ESTIMATED; `time.freerun_slew_ppm` keeps it near `CLOCK_TAI` by frequency only. `SYSTEM_TAI` follows NTP steps, so it does not qualify | 7 | — | GI-5; RN-27 |
| I-REQ-3 | PTP present: the sender uses it and its internal clock is synchronised (TR-10-1 §7.2) | MUST | engine | `MTL_TIME_SOURCE_PHC`, `_PTP_BUILTIN`; switching at runtime by AUTO, holdover then `time.fallback` | v1; switch 7 | — | GI-5 |
| I-REQ-4 | BMCA per ST 2059-2 §6.2, `slaveOnly` TRUE by default, any `logMinDelayReqInterval` (TR-10-1 §7.2) | MUST | none | ptp4l with `MTL_TIME_SOURCE_PHC`; the built-in client takes the first Announce (`mt_ptp.c:1032`), so it is labelled non-compliant under the profile until it runs BMCA | — | — | Q-I-7 |
| I-REQ-5 | a PTP leader that BMCA can elect (TR-10-1 §7.2) | SHOULD | none | ptp4l | — | — | |
| I-REQ-6 | video narrow sender with CMAX = max(16, int(Npkts / (21600 · TFRAME))) (TR-10-1 §8.1) | MUST | engine | the profile gives `MTL_SENDER_N` the TR-10-1 CMAX; SDP `TP=2110TPN` | 7 | — | GI-6 replaced: no separate IPMX sender type (RN-23) |
| I-REQ-7 | the IPMX VRX: gapped, drains from VRXFULL/2 at Npkts / ((height / vtotal) · TFRAME), VRXFULL = 2 · CMAX (TR-10-1 §8.1) | MUST | engine | the profile plus `video.vtotal` (505; the ST 2110-21 value for its rasters, else the height) and `video.htotal` (506; the width) | 7 | — | GI-6 |
| I-REQ-8 | audio sender timing per AES67 §7.5; 125 µs packet time for low latency (TR-10-1 §8.2) | MUST | API | `audio.ptime = MTL_PTIME_125US` | v1 | — | I1 named a `samples_per_packet` field; revision 4 has `ptime` |
| I-REQ-9 | senders support async and sync source media (TR-10-1 §8.3) | MUST | API, engine | sync: `MTL_MEDIA_AUTO`, `_INDEX`, `_TAI`; async: `MTL_MEDIA_SENDER` | v1; async 7 | — | GI-4 |
| I-REQ-10 | the media clock is frequency-locked to the baseband source (TR-10-1 §8.4) | MUST | engine | `MTL_MEDIA_SENDER`: RTP follows the source (k advances by max(1, round((M − M_prev) / period))); without it a +50 ppm source on a 59.94 session drops a frame every 5.6 minutes under NEAREST | 7 | — | GI-4; C-I2, C-I3 |
| I-REQ-11 | RTP per ST 2110-10; the first RTP taken from the internal clock (TR-10-1 §8.5, §8.6) | MUST | engine | sync: `floor(M × rate)` on the epoch timeline; async: RTP0 = `floor(M0 × rate)`, re-anchored by `MTL_SUBMIT_DISCONTINUITY` | v1; async 7 | G-19 | |
| I-REQ-12 | baseband video: RTP sampled at VSYNC, and the report's NTP sampled at the same instant (TR-10-1 §8.8.1) | MUST | API | SENDER mode: `unit.media_tai_ns` is the VSYNC instant; the report carries that M | 7 | — | GI-4, GI-1 |
| I-REQ-13 | non-baseband video: RTP as ST 2110-10 (TR-10-1 §8.8.1) | MUST | API | `MTL_MEDIA_AUTO`, `_INDEX`, `_TAI` | v1 | G-19 | |
| I-REQ-14 | a sender report per RFC 3550 §6.4.1 to the media's destination IP at UDP port + 1 (TR-10-1 §8.7) | MUST | engine | `rtcp.sr` (1010), `rtcp.dst_port` (1012, `udp_port` + 1); today's `rtcp.*` NACK keys become `rtx.*` (1000–1004) and share the port, told apart by payload type | 7 | — | GI-1; C-I8 |
| I-REQ-15 | every report carries the IPMX Info Block: tag 0x5831, length, block version, ts-refclk (64 B), mediaclk (12 B), Media Info Blocks (TR-10-1 §8.7) | MUST | engine | library-written (ts-refclk from the time state, mediaclk from the media mode, unless `MTL_RTCP_REFCLK_APP`, `_MEDIACLK_APP`); `mtl_rtcp_set_info()`, `struct mtl_rtcp_info` | 7 | — | GI-2; open RN-29 (no room for the NUL) |
| I-REQ-16 | report NTP = the internal clock as a PTP truncated timestamp (seconds low 32 bits, ns) matching the report's RTP; SSRC = the media SSRC; RC 0 (TR-10-1 §8.7) | MUST; SHOULD for RC | engine | NTP = the unit's media time (R5: TAI ns is the PTP timescale), stamped on the tasklet into a template prepared off it | 7 | — | GI-1 |
| I-REQ-17 | video: one report per frame (per field if interlaced, PsF as progressive) with the frame's RTP, before its first packet and after the previous frame's first (TR-10-1 §8.8.2) | MUST | engine | queued just before the unit's first packet on the same queue; compressed video the same | 7 | — | |
| I-REQ-18 | ANC: one report per new RTP timestamp, same placement (TR-10-1 §8.9.2) | MUST | engine | ANC and fast metadata: one per new RTP timestamp, before its first packet | 7 | — | RN-20 |
| I-REQ-19 | audio: a report for the first packet and every N = int(10 ms / ptime) packets, before that packet (TR-10-1 §8.10.1) | MUST | engine | the audio schedule of `mtl_rtcp.h` | 7 | — | open: SENDER audio, where a report falls mid-unit (RN-20) |
| I-REQ-20 | the report inside a compound packet, first, followed by SDES CNAME (TR-10-9 §8) | MUST | engine | `rtcp.cname` (1011; the session name @ the port IP) | 7 | — | |
| I-REQ-21 | RTCP DSCP = the stream's DSCP (TR-10-9 §16) | MUST | engine | the leg's DSCP and TTL | 7 | — | |
| I-REQ-22 | receivers tolerate other RTCP packets; RR not needed (TR-10-1 §8.7) | MUST | engine | `rtcp.rx` parses sender reports and drops the rest; without it port + 1 is not received | 7 | — | GI-3 |
| I-REQ-23 | with SSM a sender never joins INCLUDE its own source (TR-10-1 §8.7) | MUST | none | TX sessions do not join (joins are RX-only in `mt_mcast.c`, inferred) | v1 | — | |
| I-REQ-24 | inline processors keep the input's timing by default (TR-10-1 §9) | MUST | API | `MTL_SUBMIT_SENDER_TIME`: RTP and the report's NTP from the received unit; launch = submit + `min_tx_delay_ns` on the instance clock; zero copy through `unit.hold` | 7 | — | RN-8 |
| I-REQ-25 | SDP with the `IPMX` keyword, measured values, ts-refclk (`localmac=` without PTP), mediaclk `direct=0` or `sender` (TR-10-1 §10) | MUST | API | `mtl_sdp_render()` under the profile; inputs `mtl_session_info`, `info.ts_refclk.*`, `time.grandmaster_id`, `leg[].src_mac` | 7 | — | open: measured values only as `fmtp_extra` text (RN-17) |
| I-REQ-26 | receivers producing baseband recover async timing as a frequency-locked output (TR-10-1 §11.1) | SHOULD | API | `mtl_rtcp_read()`, `rx.sender_rate_ppb`; locking the output clock is the application's | 7 | — | GI-3 |
| I-REQ-27 | the link offset delay controllable while active (`ext_link_offset_delay`, min and max, `auto`) (TR-10-1 §11.2; TR-10-8 §8) | MUST if supported | API | `rx.link_offset_ns` (203, R) at the next unit or an update's boundary; `MTL_LINK_OFFSET_AUTO` (the measured minimum, sampled at activation and held); gauges `rx.link_offset_min_ns`, `rx.link_offset_max_ns` | 7 | — | GI-13; C-I7; Q-I-6 |
| I-REQ-28 | UDP destination port even and > 1024 (SHOULD > 5000), default 5004 (TR-10-2 §7; TR-10-9 §17) | MUST | none | the application's choice; checked under the profile | 7 (check) | — | GI-16 |
| I-REQ-29 | UDP size within the Standard UDP Size Limit, header extensions included (TR-10-2 §7) | MUST | API | `session.max_udp_payload`; PEP sizes payloads after its extensions | v1; 7 | — | |
| I-REQ-30 | receivers take YCbCr 4:2:2 10-bit and RGB 4:4:4 8-bit; senders at least one (TR-10-2 §8; AIMS profile §6) | MUST | API | `MTL_YUV422_10`, `MTL_RGB_8` | v1 | — | |
| I-REQ-31 | GPM or BPM packing, pgroups, 90 kHz, one RTP per frame or field (TR-10-2 §7, §9) | MUST | API | `video.packing`; the RTP rule | v1 | G-19, G-22 | |
| I-REQ-32 | any resolution and any frame rate (AIMS uncompressed profile §5–6) | MUST | engine | `mtl_raster`: width and height ≤ 32767, rational `fps` with num ≤ 4194303, den ≤ 1023; the engine's fixed table (11 `st_fps` values, `st_api.h:61`) becomes rationals | 7 | — | GI-7 |
| I-REQ-33 | video MIB 0x0001 with colorimetry, TCS, range, PAR, measured pixel clock, htotal, vtotal (TR-10-2 §10) | MUST | engine | built by the library from the config (`colorimetry`, `tcs`, `range`, `video.htotal`, `vtotal`) and `mtl_rtcp_info` (`par_num`, `par_den`, `measured_rate_milli`) | 7 | — | GI-2 |
| I-REQ-34 | non-baseband substitutes: htotal = width, vtotal = height, pixel clock = w · h · rate; measured sample rate = nominal (TR-10-9 §10) | MUST | API | the defaults of `video.htotal`, `vtotal` and `measured_rate_milli = 0` | 7 | — | |
| I-REQ-35 | SDP, NMOS and reports consistent; the block version increments on a change (PQCR §4.1.1; TP-1 §13.3) | MUST | API | `tx.rtcp_info_version`; `MTL_EVENT_RTCP_INFO` (29) is the cue to re-render SDP and update NMOS; a per-unit `MTL_META_RTCP_MIB` never changes the version | 7 | — | RN-22 |
| I-REQ-36 | interlaced: RTP per field, I and S bits (TR-10-2 §9–10) | MUST | API | `MTL_INTERLACED`, `MTL_PSF` | v1 | G-22 | |
| I-REQ-37 | 48 kHz L16 or L24 (receivers both); 44.1 kHz L16, 96 kHz L24 (TR-10-3 §8) | MUST; SHOULD | API | `MTL_PCM16`, `MTL_PCM24`, `audio.sample_rate` | v1 | — | |
| I-REQ-38 | receive ST 2110-30 level A at any channel count (TR-10-3 §10) | MUST | API | `audio.channels`; 1 ms default `ptime` | v1 | — | |
| I-REQ-39 | PCM MIB 0x0002: rate, size, channels, ptime in µs, measured sample rate, channel order (TR-10-3 §11) | MUST | engine | built by the library; `mtl_rtcp_info.channel_order` | 7 | — | |
| I-REQ-40 | AES3 per ST 2110-31 at 48 kHz, MIB 0x0004 (TR-10-12 §7–10) | MUST if AES3 | API, engine | `MTL_AM824`; the MIB built by the library | v1; MIB 7 | — | |
| I-REQ-41 | ANC per ST 2110-40 §5.1, §5.2, §5.5, 90 kHz, with sender reports (TR-10-4 §7–10) | MUST | API | `MTL_ANC`; reports as I-REQ-18 | v1; reports 7 | — | |
| I-REQ-42 | InfoFrames: ST 2110-41, DIT 0x100100, K = 0, port media + 3, the video's RTP (TR-10-10 §5–7) | MUST if InfoFrames | API | `MTL_FASTMETA`, `fastmeta.data_item_type`, `k_bit`, a shared timeline; `tx.precede` copies the video unit's RTP and media time | v1; precede 7 | — | |
| I-REQ-43 | at least one InfoFrame packet per frame or field, before the video's first packet; a Null block when empty (TR-10-10 §9, §12) | MUST if InfoFrames | engine | `tx.precede` (113, C): unit k shares the video's queue and goes just before video unit k | 7 | — | GI-14; open: a SENDER video cannot be a precede target, and the two updates are not atomic (RN-26) |
| I-REQ-44 | CBR compressed video per ST 2110-22 §4, §6, the TR-10-1 narrow model, no VRX (TR-10-11 §7–11) | MUST | API | `MTL_CVIDEO_CBR`; CMAX by the profile | v1; CMAX 7 | G-66 | |
| I-REQ-45 | MIB 0x0003, plus 0x0008 (T, P, Ppih, Plev) for JPEG XS (TR-10-11 §12; TR-10-15-1 §9) | MUST | API | application bytes in `mtl_rtcp_info.mib`, appended unread | 7 | — | the library cannot build them (RN-21) |
| I-REQ-46 | JPEG XS per RFC 9134, High444.12, 4:2:2 10-bit and RGB 4:4:4 8-bit (TR-10-15-1 §7–8) | MUST if JPEG XS | API | `MTL_CODEC_JPEGXS` with a codec plugin; slice packing `MTL_CVIDEO_PACK_SLICE` reserved | v1 | — | |
| I-REQ-47 | VBR compressed video: ST 2110-22 §6 payload, CMAX from MaxRate, `b=AS`, `TP=2110TPW` (TR-10-7 §8–11) | MUST if VBR | API, engine | `MTL_CVIDEO_VBR_MAX` (compliant under the profile), `cvideo.max_bitrate_bps` (515) | 7 | G-66 | GI-8 |
| I-REQ-48 | H.264 and H.265 packetised per RFC 6184 and RFC 7798, no PACI (TR-10-15-2, -3) | MUST if codec | API | packet units meanwhile; library packetisation later | 2P; later | — | GI-8 |
| I-REQ-49 | MIB 0x0005 (and 0x0009, 0x000A) (TR-10-7 §12) | MUST | API | application bytes | 7 | — | |
| I-REQ-50 | FEC Profile A: column FEC on + 2, row on + 4, shaping, partial matrices (TR-10-6 §7) | MAY | none | later | later | — | GI-15 |
| I-REQ-51 | receivers tolerate FEC streams (TR-10-6 §7) | MUST | none | flows filter by UDP port | v1 | — | |
| I-REQ-52 | PEP: AES-128-CTR mandatory; CTR, CMAC-64 and ECDH variants optional (TR-10-13 §13, §20) | MUST if PEP | engine | `crypto.mode`: `MTL_CRYPTO_AES128_CTR` (default), AES256, the CMAC64 and CMAC64_AAD variants; ECDH is only a key-derivation difference | 7, after the cost spike | — | GI-10 |
| I-REQ-53 | RTP header, extensions and payload header in clear; payload in 16-byte slices; IV = iv' ‖ ctr, ctr + 1 per slice (TR-10-13 §20, §20.2) | MUST if PEP | engine | `crypto.clear_bytes`, `crypto.iv`; a partial slice is counted | 7 | — | Q-I-12 asks the authors whether a partial slice consumes a counter |
| I-REQ-54 | the CTR Full extension (key version, 64-bit ctr) on each frame, field, slice or audio-packet start and on every non-A/V packet, Short (24-bit) otherwise; gap < 2^24 (TR-10-13 §20.1–20.2) | MUST if PEP | engine | `crypto.ext_id_full`, `crypto.ext_id_short`; the library writes and parses them; RX needs the header-extension fix (GI-9, SF-68) | 7 | — | |
| I-REQ-55 | MAC modes: CMAC-64, MAC then encrypt into the payload's last 8 bytes (TR-10-13 §15, §20.2) | MAY | engine | the CMAC64 modes, always on the copy path | 7 | — | |
| I-REQ-56 | RTP_KV: the key version changes at frame, field, GOP or audio-packet boundaries; ctr restarts per key (TR-10-13 §20.3) | MAY | API | `MTL_CRYPTO_PEP_RTP_KV`; `mtl_crypto_set_key(s, version, key, bytes, when)`; the counter restarts only for key bytes this session never used | 7 | — | RN-6, RN-7 |
| I-REQ-57 | the same PEP parameters on both ST 2022-7 legs; RTCP not encrypted (TR-10-13 §13, §20) | MUST if PEP | engine | one ciphertext on both legs; reports in clear | 7 | — | |
| I-REQ-58 | PEP MIB 0x0011: privacy_version, f_id, s_id (TR-10-13 §22) | MUST if PEP | engine | built by the library from the `crypto.*` options | 7 | — | open: no option carries privacy_version or the key identity (RN-21) |
| I-REQ-59 | PSK store, KDF (CMAC, HMAC-SHA-512/256), ECDH, the parameters in SDP and IS-05 (TR-10-13 §12–13; BCP-005-03) | MUST if PEP | none | the application's; `mtl_crypto_set_key` takes the derived key; a fresh key generator per boot | — | — | Q-I-10 |
| I-REQ-60 | the HDCP Full or Short IV Counters extension first in every packet (frz, streamCtr, inputCtr); headers clear (TR-10-5 §14–15) | MUST if HDCP | API | packet units, with the vendor's code encrypting; a cipher plugin later | 2P; later | — | GI-11; Q-I-4 |
| I-REQ-61 | receivers watch streamCtr and re-run HDCP on a change (TR-10-5 §14.1) | MUST if HDCP | API | packet units on RX too (frame units deliver ciphertext pixels and no extension values); `MTL_PKTE_HDR_EXT` | 2P; 7 | — | GI-9 |
| I-REQ-62 | HKEP over TCP, `a=hkep`, MIB 0x0010 (TR-10-5 §10–12, §16) | MUST if HDCP | none | the application's; the MIB as application bytes | — | — | |
| I-REQ-63 | DHCP by default on every interface, manual fallback (TR-10-9 §14) | MUST | API | `mtl_port_spec.sip` all zero = DHCP on kernel ports; `port.dhcp` on DPDK ports; the profile defaults to DHCP | v1 | — | |
| I-REQ-64 | DSCP defaults AF42 (video, ANC, compressed), AF41 (audio, AES3), EF (PTP); user-selectable (TR-10-9 §16) | MUST | API | `mtl_flow.dscp` 0 = the profile's default; InfoFrame, FEC and HDCP streams take their associated stream's; `MTL_FLOWF_DSCP_LITERAL` asks for CS0 | 7 | — | open: EF for PTP (RN-30) |
| I-REQ-65 | IPv4 multicast; IGMPv3 SSM with the SDP's source; IGMPv2 too; the version user-selectable (TR-10-9 §17) | MUST | engine | `mtl_flow.source_filter`; `port.igmp_version` (v3, falling back to v2 after a v2 querier; with v2 the source filter is checked in software); today only v3 reports (`mt_mcast.c:215`) | v1; v2 7 | — | GI-12, merged with K-REQ-20 |
| I-REQ-66 | address ranges; default group 239.S.C.D (TR-10-9 §17, §17.1) | MUST | none | the application's | — | — | |
| I-REQ-67 | frame-to-frame interval of first packets and of reports: max − min ≤ 2 ms over 2 s (TR-10-9 §11.2) | MUST | engine | pacing meets it; async units carry the application's jitter; gauge `tx.f2f_pp_ns` | v1; gauge 7 | G-27 | GI-17 |
| I-REQ-68 | receivers pick the sync method from ts-refclk and mediaclk, and support `direct=0` and `sender` (TR-10-9 §11, §11.1) | MUST | API | `rx.mediaclk`: `MTL_MEDIACLK_DIRECT`, `_SENDER`; `_AUTO` compares the Info Block's ts-refclk with the instance's grandmaster | v1; AUTO 7 | G-67 | GI-3 |
| I-REQ-69 | streams that share a reference clock are aligned by their timestamps (TR-10-9 §13) | MUST | API | `mtl_rx_align()` on `media_tai_ns`, SENDER_TIME units of one sender included | Phase 3; 7 | G-76 | |
| I-REQ-70 | watch the PTP state and update the clock source description (TR-10-9 §12) | SHOULD | API | `MTL_EVENT_TIME_STATE`, `time.grandmaster_id`; the Info Block's ts-refclk follows (`localmac=` to `ptp=`) | v1; 7 | G-54 | |
| I-REQ-71 | in-band NMOS and HKEP on the media port: TCP, mDNS and DHCP reach the OS (TR-10-9 §18–19) | MUST if both bands | API | `port.virtio_user` with 224.0.0.251 forwarded, or kernel backends; in a pod the primary network | v1 | — | Q-I-9: verify with an IPMX controller |
| I-REQ-72 | unicast streams; reports to the unicast destination (TR-10-1 §8.7; TR-10-5 §9.1) | MUST | API | `mtl_flow.ip` unicast with ARP or `MTL_FLOWF_USER_MAC`; reports go to the leg's address | v1; reports 7 | — | |
| I-REQ-73 | one network is the common case; ST 2022-7 optional (TR-10-1 §6) | MAY | API | `flows[1]` optional | v1 | — | |
| I-REQ-74 | off-subnet unicast through a gateway (inferred) | SHOULD | API | `mtl_port_spec.gateway` | v1 | — | |
| I-REQ-75 | USB over IP (TR-10-14) | MAY | none | out of scope: TCP, not a media flow | — | — | |
| I-REQ-76 | HDR MIB 0x0006 after the colorimetry MIB, may change per field (TR-10-16 §7) | MAY | API | a unit's `MTL_META_RTCP_MIB` record appends to that unit's report only, with no version change | 7 | — | RN-22 |

The I1 conflicts C-I1…C-I14 and their resolutions are [nmos-ipmx.md §14.3](nmos-ipmx.md#143-conflicts).
Review RN listed the IPMX MUSTs the first Phase 7 design did not meet (I-REQ-18, -24, -25, -27,
-43, -45, -52…-58, -64); the headers since fixed I-REQ-18 (ANC schedule), -24 (`MTL_SUBMIT_SENDER_TIME`),
-27 (R option at the boundary), -45 (application bytes) and -52…-57 (per-sender options, counter reuse,
the RX key rule); -25, -43, -58 and -64 stay open as listed above. I-REQ-4 is met only with ptp4l.

## 8. RTP-level use cases behind GO-10

GO-10 holds when each use case that today needs `*_TYPE_RTP_LEVEL` (S8 §2.2) has a unified home.
Today's RTP level per essence, its defects and the legacy mapping are
[engine.md §12.4](engine.md#124-rtp-level-packet-path-today) and [contract.md §13](contract.md#13-packet-units).

| Use case | Needs from the library | Frame units enough? | Unified home | Step of 2P |
|---|---|---|---|---|
| ST 2022-6 (SDI over IP) and payloads MTL does not packetise (ST 2110-43 timed text, proprietary metadata, RFC 3640 audio) | L2–L4, linear pacing at a declared rate, ST 2022-7, verbatim RTP, ≈1400 B packets | no | `MTL_RTP` with `MTL_RTP_LINEAR`, `rtp.clock_rate`, `rtp.encoding`; packet size from the MTU (PE7: 2022-6 needs 1396 B) | 2P-a; 2P-b exit: a 2022-6 stream accepted by a third party |
| application-side packetisers: a hardware JPEG XS or H.26x encoder emitting RTP, an FPGA stream, a custom RFC 4175 packing | library pacing and ST 2022-7 over app-built packets; optional sequence and timestamp stamping | no | packet units on the essence, `MTL_PKT_PACE_UNIT`, `packet.set_fields` | 2P-a (video), 2P-b (cvideo) |
| gateways and proxies forwarding packets unchanged: unicast ↔ multicast, re-addressing for IS-05, 2022-7 merge to one leg or split to two | RX dedup, TX L2–L4 rewrite, latency of µs, not one frame | partly: frame forwarding reassembles and adds ≥ 1 frame | packet RX (dedup by sequence) into packet TX; `MTL_PKT_RX_LEND` and holds for zero copy | 2P-a; zero copy 2P-c |
| RTP re-stamping: repair a non-compliant source's timestamps, re-time a stream onto local PTP | read the timestamp, stamp a derived one | partly (`MTL_SUBMIT_RTP_TS`) | `MTL_PKT_SET_TIMESTAMP` from the unit's media time; `MTL_PKT_TIME_FROM_RTP` | 2P-b |
| recording and replay of pcaps, captured ST 2110-22 streams into a frame receiver included | verbatim TX with the original or regular timing | no | verbatim packet TX, `MTL_PKT_TIME_FROM_RTP` or `MTL_PKT_PACE_LAUNCH` | 2P-b (the pcap replay test, §4.5) |
| analysers and monitors: sequence gaps, per-leg arrival, path differential, payload conformance | every packet, both legs if asked, NIC arrival times | no | `MTL_PKT_RX_NO_DEDUP`, `struct mtl_pkt_rx` (`leg`, `gap`, `arrival_tai_ns`, `MTL_PKTE_HW_ARRIVAL`) | 2P-a |
| test fault injection: out-of-order, truncated or malformed units against a frame receiver | arbitrary packet order and counts | no (only a debug API) | public packet TX for tests (Q-PKT-8); `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` stays for library-built streams | 2P-a |

Not offered in packet units: RTCP on RTP-level sessions (Q-MODE-8). The spike SP-PKT (per-packet
tasklet cost of extbuf attach against today's RTP level, RX copy cost, chunk size sweep) gates 2P
([engine.md §11.1](engine.md#111-spikes-that-gate-engine-work)).

## 9. Coverage by phase

Where each requirement closes, collected from the tables above (R-* milestones from
[implementation-plan.md §8.0](implementation-plan.md#80-requirements-and-the-guarantees-that-close-them)).
A requirement split over phases appears in each.

| Phase | Requirements it closes |
|---|---|
| M0 | R-OBJ-5, R-THR-3, R-ABI-1…5, R-TEST-1…3, R-CMP-6 (codes), R-LIFE-5 (U); K-REQ-1 (close), K-REQ-3, K-REQ-11 (`env:` ports) |
| M1 | R-OBJ-1 (video TX), R-OBJ-2, R-CMP-1, R-CMP-2, R-CMP-4 (G-06), R-CMP-7, R-THR-1, -2, -4, -5 (measured), R-LIFE-1…3, R-MEM-6 (G-103), R-TIME-4, R-USE-1, R-PERF-1 |
| M2 | R-OBJ-1 (video RX), R-MEM-8 (G-56), R-TIME-10 (G-82) |
| M3a | R-OBJ-4, R-CMP-3, R-CMP-5, R-OBS-1, R-OBS-2 (G-43), R-OBS-3 (G-41), R-OBS-5 (G-88), R-OPS-3, R-OPS-6 (G-110), R-CAP-1 (granted values); K-REQ-1 (report), K-REQ-2, K-REQ-7; N-REQ-48, -49, -54 |
| M3b | R-TIME-2 (G-59), R-TIME-3 (video), R-TIME-5 (G-26, G-28, G-85), R-TIME-7 (G-94), R-TIME-9, R-TIME-12 (video) |
| M5 | R-LIFE-5 (I); K-REQ-4, K-REQ-5 (I), K-REQ-6 (EK18), K-REQ-14 (EK8) |
| Phase 0.5 | the legacy timeline helper (G-106) |
| Phase 1, rest, and Phases 1–2 | R-USE-2; K-REQ-15; N-REQ-12, -13 (the event), -27 |
| Phase 2 | R-OBJ-1 (every essence, G-45), R-OBJ-6, R-CAP-1 (G-34), R-CMP-6 (G-38), R-OPS-1, -2, -4, -5, -6 (G-98), -7, R-TEST-4 (link); K-REQ-17; N-REQ-18…20, -24, -35, -41, -50, -52 |
| 2P | GO-10, R-PKT-1…5; the packet-unit routes of I-REQ-48, -60, -61 |
| Phase 3 | R-TIME-1, -3 (all essences), -5, -6 (G-62), -7, -8, -10, -12, -13; I-REQ-69 |
| Phase 4 | R-OBJ-3, R-MEM-1…5, -7, -8 (holds), R-CMP-4 (G-100), R-LIFE-4 (G-37), R-SEC-1 (G-96); GO-3 |
| Phase 5 | R-OBS-2 (G-55), R-OBS-4, R-OBS-5 (G-44), R-TIME-6 (G-54), R-TEST-4 (VF reset) |
| Phase 6 | R-TIME-11, R-MEM-9 (device memory, Phases 4–6), NG8 |
| engines track | K-REQ-8, -9, -10, -12, -13, -16, -18, -19, -20 |
| 7 (later) | N-REQ-2, -7 (default flag), -8, -9, -10 (`ts_refclk`), -11 (`gm_*`), -14, -21, -22, -25, -26, -29…-31, -33, -34, -38…-40, -43 (per leg), -53, -55 (timeout), -57, -59 (RTCP), -60; I-REQ-1…3, -6, -7, -9…-12, -14…-22, -24…-29, -32…-35, -39, -40 (MIB), -41 (reports), -42…-45, -47, -49, -52…-58, -64, -65 (v2), -67, -68 (AUTO), -70, -72 (reports), -76 |
| later than Phase 7 | N-REQ-32, -56; I-REQ-48 (library packetisation), -50, -60 (cipher plugin) |
| outside MTL | N-REQ-23, -36 (resolution), -53 (policy), -58; I-REQ-4, -5, -28 (choice), -59, -62, -66, -75 |
| not adopted | N-REQ-15 (LLDP), N-REQ-59 (ST 2022-5 FEC) |
