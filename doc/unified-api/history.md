# History of the design

| | |
|---|---|
| Status | Maintained record. Nothing here is normative; the headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) and the maintained documents win |
| Date | 2026-10-02 |
| Folded from | the archive (git `15a27bb6`): README, README-revision-4, 00-summary, REVISION-4, LIST-OF-CHANGES, LEARN, DECISIONS (superseded), the revision-3 sketch, studies S2…S6 (with `mtl_alt.h`), S8 §8.1, N1 §4, the R4 header review; reviews C1–C5, R2, RA, RK, RN, RKNA, RV; the samples page; research notes 00, 01 |

This file says how the design got to its current shape: the revisions and what each changed
(§1), the revision-3 names and what replaced them (§2), the alternatives that were rejected and
why (§3), what the revision-3 header looked like (§4), what the simplification studies proposed
and what was taken (§5), and every review with one row per finding ID and its outcome (§6). An
outcome is **fixed** (with the maintained document and section, or the header, where the fix
lives now), **rejected** (with the reason), **superseded** (a later decision replaced the thing
the finding was about) or **open** (with the OI-n of [decisions.md](decisions.md) §5, or the
question it waits on). Names in the outcome column are revision-4 header names; a revision-3 name
appears only as "was X". Read §1 for the story, §2 when an old PR or review uses an unknown name,
and §6 when you want to know whether a review finding is still a problem.

## 1. Timeline

- **before 2026-09-29: PR #1610 and its review.** PR #1610 (`14a1f80c`, 16 commits on `74c9af0e`,
  283 commits behind `main`) proposed one polymorphic session for every essence. The maintainer's
  review of it asked three questions: did my frame go out on time; can A/V/ANC from one file get
  consistent RTP without app maths; can nothing I call block the pinned cores. Research note 01
  confirmed every review claim and found three worse (the transport overwrites TX pacing timestamps
  and user metadata, so they never reach the wire). Decision: supersede, do not rebase (§3.1).
  The design directory is meant to replace PR #1610's `doc/new_API/` once the decisions are
  made: its `List-of-changes.md` (now [concepts.md](concepts.md) §3 and
  [migration.md](migration.md) §14), `CURRENT_STATE.md`, `GRACEFUL_SHUTDOWN.md` (salvaged as a
  target spec) and `samples/diagrams.md` (now [diagrams.md](diagrams.md)). Baselines used
  throughout: `main` @ `545a266a`, PR #1610 @ `14a1f80c`, open PR #1770 @ `74b9991d` (38 commits,
  +3208/−371)
- **2026-09-29: Revision 1.** First draft over `main` @ `545a266a`: documents 00–14, 13 research
  notes, OPEN-QUESTIONS and DECISIONS. Reviewed the same day by four critics: C1 real-time
  feasibility, C2 timing and standards, C3 usability by persona, C4 consistency
- **2026-09-29: Revision 2.** Applied C1–C4: consumer-side result materialisation, explicit L0
  hooks, idle descriptor cleanup, seq_cst fences, deadline-driven waker, per-writer stats (C1);
  source kinds and `min_tx_delay_ns`, lazy timeline anchors, bounded SEND_LATE, corrected grids,
  ANC/fastmeta keep-alive, clock-step policies, RX RTP offsets (C2); results off by default for
  library pools with `blocked_on`, sticky interrupt, named-buffer acquire, `RX_BY_INDEX`, deferred
  destroy, discard, dry-run query (C3); 44 inconsistencies, guarantee map, new questions, mode
  dispositions, citations re-pinned to `545a266a` (C4). R2 verified it (§6.6) and its applied list
  was done in the same pass
- **2026-09-30: Revision 3.** Answered review C5 (adversarial user and feasibility review; the
  review text is not in the repository, its 207 findings and answers are in §6.5). The header became
  a real file set that compiles (`mtl_unified.h`, `mtl_simple.h`, `mtl_debug.h`, §4); RX got a
  timing model and engine hooks; the plan got an engines-first track and Phase 0.5 (the A/V answer
  on the legacy API); a null backend, test clock and fault injection in Phase 1; the operator
  surface (atomic flow activation, leg admin state, names, enumeration, capacity); a security and
  deployment document; ten maintainer decisions M1–M10 and a proposed default for every other
  question. Effort ≈ 55–85 EM
- **2026-10-01: Revision 4 (simplification pass).** The maintainer found the diagrams and samples
  too complicated and asked for the leanest include file, more defaults, simpler helpers, every
  current use case kept, RTP passthrough in the high-level API and `include/st20_api.h` hidden. Nine
  studies S1–S9 (§5), merged into one header set, then checked by the R4 coverage check and the R4
  header review (§6.7). Result: `mtl.h` + 13 optional headers, one typed config and one unit struct
  for every essence, options for the rare knobs, `MTL_INIT`, slots by index, one queue type, one
  stats registry, packet units, a three-tier hiding plan for the legacy headers. Decisions
  D-71…D-88, maintainer decisions M11–M15
- **2026-10-01: Addendum K (Kubernetes).** Requirement: "design API in a way that the library can
  safely work in kubernetes pod". Studies K1 (runtime), K2 (code audit, hazards H-K-n), K3 (shutdown
  prior art, P-1…P-12). Bounded network-first `mtl_instance_close(mt, timeout)`,
  `mtl_instance_shutdown`, rule R8 (signals, fork, crash-only), lock-free `mtl_instance_get_health`,
  CPUs and time from the pod, fail-fast open (reasons 600–614), no-IOMMU refused by default. Engine
  items EK1–EK21. D-89…D-92, M16. Now in [deployment.md](deployment.md) §4
- **2026-10-01: Addendum N (NMOS and IPMX).** Requirement: "we also want to work with NMOS and
  IPMX". Studies N1 (N-REQ-n) and I1 (I-REQ-n). The IS-05 activation contract on
  `mtl_session_update` (planned and applied instants), `mtl_sdp.h`, `mtl_rtcp.h`, `mtl_crypto.h`,
  timing without PTP (`MTL_TIME_SOURCE_FREERUN`, `MTL_MEDIA_SENDER`). D-93…D-96, M17. Now in
  [nmos-ipmx.md](nmos-ipmx.md)
- **2026-10-01: Reviews RK, RN, RA, response, RV.** 118 findings on the addenda (RK 43, RN 34, RA
  41), four Critical (RK-1, RK-5, RK-7, RN-1) and one Blocker (RA-1, two headers could not be
  included together). The response fixed almost all (§6.8–§6.10); RV verified the fixes and found 9
  regressions and 13 incomplete fixes (§6.11). check.sh gained the all-headers probe. Count went 126
  (+6 later) → 139 → 135 (+8 later), 17 headers, `mtl.h` 33
- **2026-10-01: Typed configuration, D-97.** Maintainer: "session parameters passed by string is a
  mistake ... please use enums or defines". Removed `mtl_session_config_parse`, the spec grammar,
  `spec_options[16]`, the instance spec string and the parsers `mtl_flow_parse`, `mtl_raster_parse`,
  `mtl_fps_parse`. Added `mtl_flow_ipv4()`, `raster.rate` (`MTL_FPS_*`), `audio.ptime`
  (`MTL_PTIME_*`); enums in the legacy order (legacy + 1 where 0 means "not set", same values where
  0 is a real default); a field map of every legacy ops struct ([migration.md](migration.md) §4).
  `mtl_session_config` 1320 B → 936 B. `MTL_PORTS` stays
- **2026-10-01: Port first, D-98.** Maintainer: "first we need to port current functionalities"; no
  effort on crypto and SDP unless NMOS or IPMX requires it. Phases 1–6 port today's functions plus
  the Kubernetes lifecycle; the NMOS contract extras (REAPPLY, DRY_RUN, cancel, mute, reserved legs,
  port change while running), `mtl_sdp.h`, IPMX (RTCP, FREERUN, SENDER, the profile) and PEP are
  Phase 7, later, declared under `MTL_LATER`. Count: `mtl.h` 32 functions, 125 in 17 headers, 14
  under `MTL_LATER`
- **2026-10-02: Distillation.** The archive was folded into the maintained set
  ([README.md](README.md)); disagreements found on the way became OI-1…OI-47, and the loss audit
  before the archive was deleted added OI-48…OI-58 ([decisions.md](decisions.md) §5). The archive
  is removed from the branch; commit `15a27bb6` keeps it

### 1.1 Numbers across the revisions

| | Revision 3 | Revision 4 at the R4 review | After the R4 fixes | After the addenda and RKNA | Now (D-97, D-98) |
|---|---|---|---|---|---|
| Headers | 3 | 14 | 14 | 17 | 17 |
| Functions in the first include | 203 (`mtl_unified.h`, 3029 lines) | — | 33 | 33 | 32 |
| Functions in all headers | 217 (37 `*_init()`, 23 inline twins) | 113 (+4 later) | 126 (+6 later) | 135 (+8 later) | 125 (+14 later) |
| Structs / enums | 93 / 79 | — | 24 / 25 in the core | — | see the headers |
| Handle types | 9 + the simple handle | — | 5 core, 2 in extensions | — | 5 core (instance, session, lease, region, timeline), queue and plugin in extensions |
| Example lines (minimal TX / zero-copy TX / A/V/ANC / forwarder) | 63 / 139 / 120 / 88 | — | 34 / 60 / 75 / 59 (spec strings) | — | 42 / 67 / 86 / 59 (typed fields) |
| Typed knobs per video TX session | ≈ 85 | — | ≈ 40 + options | — | ≈ 40 + 161 option keys |
| Hidden rules a reader of the examples must know (S7 count) | 115 | — | listed in each example's first comment | — | the same |

S3's counts at revision 3: a video TX filled 85 typed fields (71 with one leg) against legacy
`st20p_tx_ops`' 25 + `st_tx_port`'s 7 and 14 `ST20P_TX_FLAG_*`; a minimal application set ≈ 10
legacy values and 7 unified ones. `mtl_session_config` had 75 typed leaves (61 with one leg,
792 B, six sub-structs), `mtl_instance_params` 1496 B (1184 B of it `ports[8]`); 39 input enums
with ≈ 150 enumerators, six of them only to name "zero = derived" (`MTL_LATE_DEFAULT`,
`MTL_UNDERRUN_DEFAULT`, `MTL_TSMODE_DEFAULT`, `MTL_RX_FILL_DEFAULT`, `MTL_DETECT_DEFAULT`,
`MTL_TXQ_AUTO`); ≈ 70 of the 87 defaults rows sat in configuration. The root causes it named: the
zero-default rule taxes every knob (side flags, +1 codes), typed tuning fields every user reads
past, nesting for versioning (≈ 270 B of reserved tails).

## 2. Revision-3 names and what replaced them

Every row of the revision-3 → revision-4 map, with the current header name (checked against the
headers on 2026-10-02). Where revision 4 first had another name, it is given as "r4 first". The
compact form of this table is [migration.md](migration.md) Appendix A.

| Was (revision 3) | Now |
|---|---|
| `mtl_unified.h`, `mtl_simple.h` | `mtl.h` + 16 optional headers; r4 first: `mtl_session_config_parse` for the simple layer, removed by D-97 (typed fields, `mtl_session_open`) |
| `mtl_<essence>_session_create`, `mtl_<essence>_session_query` | `mtl_session_create`, `mtl_session_query` with `sc.essence` |
| `struct mtl_video_config` etc. as a separate argument | members `sc.video`, `sc.cvideo`, `sc.audio`, `sc.anc`, `sc.fastmeta`, `sc.rtp` of one `struct mtl_session_config` |
| `sc.timing.*`, `sc.pool.*`, `sc.completion`, `sc.caps`, `sc.options` | `sc.media_mode`, `sc.source_kind`, `sc.timeline`, `sc.min_tx_delay_ns`, `sc.media_time_offset_ns`, `sc.pool_count`, `sc.flags`; the rest are options (`mtl_options.h`) |
| `MTL_DIR_TX`, `MTL_ESSENCE_VIDEO` | `MTL_TX`, `MTL_VIDEO` |
| `*_init()`, `MTL_*_INIT(...)`, `mtl_struct_known_size()`, `api_version` field | `MTL_INIT(&s)`; `MTL_API_VERSION`, `mtl_version_num()` |
| `mtl_instance_open_simple`, `mtl_instance_acquire_default`, `mtl_instance_release` | `mtl_instance_open(&params, &mt)` with `MTL_INSTANCE_SHARED`, `mtl_instance_close(mt, timeout_ns)`; r4 first: `mtl_instance_open(spec, params, &mt)`, spec removed by D-97 |
| `mtl_instance_interrupt_all` / `uninterrupt_all` | `mtl_instance_interrupt(mt, 1 / 0)` |
| `mtl_tx_acquire(s, &lease, &view, &hint, t)` | `mtl_tx_acquire(s, &unit, timeout_ns)`; the hint is `mtl_tx_next_slot` (`mtl_sync.h`) |
| `mtl_tx_submit(s, lease, &submission)` | `mtl_tx_submit(s, &unit)` |
| `mtl_tx_publish` | submit the same lease again with a larger `used` (rows units) |
| `mtl_rx_dequeue(s, &lease, &view, &rx_unit, size, t)` | `mtl_rx_dequeue(s, &unit, timeout_ns)`; detail: `mtl_rx_get_detail` (`mtl_observe.h`) |
| `mtl_buffer_h`, `mtl_buffer_create`, `mtl_session_attach_buffers` | slot index `unit.slot`; `mtl_session_attach(s, &attach)` (`mtl_mem.h`) |
| `mtl_tx_acquire_buffer` | `mtl_tx_acquire_slot` |
| `mtl_buffer_hold` | `mtl_tx_pin(s, slot, on)`; r4 first: `mtl_tx_hold` (renamed by RV-42) |
| `mtl_rx_transfer` | `mtl_tx_send_slot` with `how.hold` = the RX lease |
| `MTL_POOL_DYNAMIC`, `mtl_tx_acquire_dynamic` | no header; `mtl_tx_acquire_layout` under `MTL_LATER` (Phase 4, D-56, OI-20) |
| `MTL_COMPLETE_NONE / EXCEPTIONS / ALL` | `MTL_SESSION_RESULTS` for library pools; always on for application memory (EXCEPTIONS cut, D-77) |
| `mtl_group_*` | `mtl_session_start` / `mtl_session_stop` with an array (D-78) |
| `mtl_timeline_open`, `mtl_timeline_destroy`, `mtl_timeline_get_anchor`, `mtl_timeline_epoch` | `mtl_timeline_create` (with a name), `mtl_timeline_close`, `mtl_timeline_get_info`; a null timeline is the epoch |
| `mtl_rational_index_at` | `mtl_epoch_index_at` (and `mtl_index_at` on a session) |
| `mtl_start_params`, `mtl_activation`, `mtl_launch` | `struct mtl_when`; per-unit launch: `MTL_SUBMIT_NOT_BEFORE` / `MTL_SUBMIT_EXACT` with `unit.launch_tai_ns` |
| `mtl_session_reconfigure`, `mtl_session_update_flows`, `mtl_session_set_leg_enabled` | `mtl_session_update(s, &sc, parts, &when, &planned_tai_ns)`; r4 first had no `planned_tai_ns` (added by addendum N) |
| `mtl_session_discard_queued` | `mtl_session_discard` |
| `mtl_session_stop` + `mtl_session_destroy`, `MTL_DESTROY_FORCE` | `mtl_session_close(s, timeout_ns)`; `mtl_session_stop` stays for stop and restart; no force flag (RV-53) |
| `mtl_session_get_wait_object` + `struct mtl_wait_object` | `mtl_session_get_wait_handle(s, mask, &native)` |
| `mtl_session_trywait` + `mtl_session_wait` | `mtl_session_wait` (`-MTL_EAGAIN` = nothing ready, armed); every data call returning `-MTL_EAGAIN` also arms |
| `mtl_session_uninterrupt` | `mtl_session_interrupt(s, 0)` |
| `mtl_mem_destroy` | `mtl_mem_close` (retires at the last reference) |
| `mtl_*_session_query` + `mtl_session_get_buffer_requirements`; r4 first `mtl_session_requirements` | `mtl_session_query(mt, &sc, flags, &info, size, &req, size)` (RV-23) |
| `MTL_RT_*`, `MTL_TT_*` validity masks | `MTL_UNITF_*` unit flags, `MTL_TXR_*` result flags |
| `MTL_FLOW_USER_MAC`; `MTL_FLOW_MATCH_FMD_DIT/K` | `MTL_FLOWF_USER_MAC`; `MTL_FASTMETA_RX_MATCH_DIT/K` |
| `mtl_rx_wait_progress` | `mtl_rx_wait_rows` (`mtl_sync.h`) |
| `mtl_cq_*`, `mtl_eq_*`, `mtl_session_bind_cq/eq`, `mtl_eq_post` | `mtl_queue_*`, `mtl_session_read_events` (`mtl_queue.h`); no user posts |
| `mtl_session_get_stats`, `mtl_*_stat_get`, the port, scheduler, memory and time status getters | `mtl_stat_list`, `mtl_stat_find`, `mtl_stat_read`, `mtl_stat_get` (`mtl_observe.h`) |
| `enum mtl_state_reason`, `enum mtl_tx_reason`, `enum mtl_rx_reject` | `enum mtl_reason` (`mtl_reasons.h`); RX rejects are stats labels |
| `MTL_RTP_PASSTHROUGH`, `rtp_override`, `MTL_SUB_RTP` | `MTL_SUBMIT_RTP_TS`, `unit.rtp` |
| `MTL_UNIT_PACKET_CHUNK` (reserved) | `MTL_UNIT_PACKETS` |
| `mtl_session_set_option` and 13 `MTL_OPT_*` keys | `mtl_set_option` and 161 keys in `mtl_options.h` |
| `mtl_time_test_source`, `mtl_time_test_advance` | `mtl_test_clock`, `mtl_test_clock_advance` (`mtl_debug.h`) |
| `mtl_time_cross_timestamp`; `mtl_lease_copy_in/out` | `mtl_time_cross`; `mtl_unit_copy_in/out` |
| `mtl_call_seq()` | removed; `mtl_last_error()` follows the errno contract (D-84) |
| `mtl_tx_release`, `mtl_tx_withdraw`; `mtl_tx_cancel` (revision 2) | unchanged: `mtl_tx_release` (`mtl.h`) returns an unsubmitted lease, `mtl_tx_withdraw(s, slot)` (`mtl_mem.h`) a queued unit; a failed submit returns the slot itself (D-88) |
| `mtl_instance_from_legacy`, `mtl_instance_to_legacy` | `mtl_legacy.h` |
| early revision 4: spec strings, `mtl_flow_parse`, `mtl_raster_parse`, `mtl_fps_parse`, `spec_options[16]` | removed (D-97): typed fields, `mtl_flow_ipv4()`, `raster.rate` |
| addenda, removed by the reviews (1): `MTL_INSTANCE_PREFLIGHT`, `MTL_INSTANCE_MANAGER_OPTIONAL`, `MTL_SENDER_IPMX`, `MTL_UPDATE_CANCEL`, `mtl_ext_hdr`, `enum mtl_ext_kind`, `struct mtl_ext_crypto`, `MTL_EVENT_TIME_SOURCE`, `mtl_rtcp_mib_build`, `struct mtl_rtcp_media_desc` | gone; replacements in §6.8–§6.10 |
| addenda, removed by the reviews (2): `mtl_crypto_clear_keys`, `struct mtl_rtcp_sr`, `enum mtl_rtcp_sr`, `housekeeping_cpus`, `update_planned_tai_ns`, `enum mtl_shutdown_outcome`, `rtcp.sr_interval_ms`, `MTL_SDP_ONE_MLINE`, `bytes_kept_until_exit` | gone; replacements in §6.8–§6.10 |
| N1 study: `mtl_session_get_update()`, `struct mtl_update_status` (`first_media_index`, `first_rtp[2]`) | `mtl_session_status.update_state`, `update_seq`, `update_applied_tai_ns` and the `planned_tai_ns` argument of `mtl_session_update` |
| N1 study: `uint32_t legs_reserved` | a set `legs_disabled` bit on an all-zero flow |
| N1 study: `MTL_EVENT_UPDATE = 23`, `MTL_EVENT_PORT_ADDRESS = 25`, `MTL_OPT_PTP_ANNOUNCE_TIMEOUT = 2207` | 26, 28 and 2209 |
| N1 study: `MTL_TCS_DEFAULT = 0`, `MTL_RANGE_DEFAULT = 0` with SDR = 1 and NARROW = 1 | `MTL_TCS_SDR = 0`, `MTL_RANGE_NARROW = 0` |
| N1 study: G-N14 options `port.lldp`, `port.lldp_chassis_id`, `mtl_port_get_neighbor()`; RX `REAPPLY` as "IGMP leave and join" | deferred (Q-NMOS-8); RX REAPPLY re-sends membership reports without a leave (RN-12) |
| S4 `struct mtl_pool_config` flags `MTL_POOL_ATTACHED`, `_REQUIRE_DIRECT`, `_RX_BY_INDEX`, `_RX_LATEST`, `_RX_NO_FILL` | session flags `MTL_SESSION_POOL_ATTACHED`, `MTL_SESSION_REQUIRE_DIRECT`, `MTL_SESSION_RX_BY_INDEX`, `MTL_SESSION_RX_LATEST`, `MTL_SESSION_RX_NO_FILL` (S3 P3) |
| S4 `mtl_rx_get_result`, `struct mtl_rx_result(_full)` | `mtl_rx_get_detail`; no RX result record (`mtl_queue_ready` + dequeue) |
| S4 `MTL_SUBMIT_RTP`, `MTL_SUBMIT_LAUNCH_NOT_BEFORE`, `_LAUNCH_EXACT`; RX `MTL_UNIT_*_VALID`, `MTL_UNIT_PARTIAL` | `MTL_SUBMIT_RTP_TS`, `MTL_SUBMIT_NOT_BEFORE`, `MTL_SUBMIT_EXACT`; `MTL_UNITF_*` |
| S4 `mtl_tx_hold(s, slot, on)`; `mtl_unified_rows.h`, `mtl_unified_dynamic.h`, `mtl_unified_mem.h` | `mtl_tx_pin`; `mtl_rx_wait_rows` in `mtl_sync.h`, `mtl_tx_acquire_layout` under `MTL_LATER`, `mtl_mem.h` |
| S4 TX result with a 48-B `struct mtl_cq_entry_hdr` | `struct mtl_tx_result` without a header: `status`, `reason`, `slot`, `flags`, `cookie`, `seq` first |
| S5 `mtl_object_session(s)`, `mtl_object_port`, `mtl_object_sched`; `mtl_reason.h`; `MTL_EVENT_TIME_VALID`; stat units `MTL_UNIT_*` | `MTL_OBJ_OF_SESSION(s)`, `MTL_OBJ_OF_PORT`, `MTL_OBJ_SCHED`; `mtl_reasons.h`; `MTL_EVF_TIME_VALID`; `MTL_SU_*` |
| S8 `MTL_RTP_TS_OVERRIDE`, `MTL_SUB_RTP_TS`, `MTL_REASON_RTP_TS_OVERRIDE_AUTO` (207) | `MTL_SUBMIT_RTP_TS`, reason `RTP_TS_AUTO` |
| S8 `mtl_packet_config.layout`, `header_slot_bytes`, `rx_min_packets`, `rx_max_wait_ns`, `mtl_rtp_session_create`, `mtl_tx_submission.pkt_count`, `MTL_TX_TIMING_UNIT_START/END` | `MTL_PKT_SPLIT`; options `packet.*` ([contract.md](contract.md) §12.6); `sc.essence = MTL_RTP`; `u.used`; dropped |

## 3. Rejected alternatives

### 3.1 The whole-API alternatives

- **The incremental path on the legacy API (revision 3, document 02 §2.3).** No new API; extend the
  pipelines: `*_get_frame2(h, &f, timeout)` returning `int` (0.5 EM), per-frame result fields in
  `st_frame` (0.5 on top of E3/E4), per-session start/stop (1–1.5), a timeline in ops with exact
  math (0.5–1), `ST30P_TX_FLAG_USER_TIMESTAMP` (0.1), ST40P USER_TIMESTAMP without USER_PACING
  (0.1–0.2), `st41p` copied from st40p (0.5–1), the armed bit instead of the tasklet mutex and
  condvar (SF-15, 0.5–1), exactly-once results through the slot interface (2–3): ≈ 6–9 EM, 4–6 of it
  shared with the unified plan. *Outcome:* Not chosen as the end state, taken as the first step: it
  cannot reach GO-1 (one verb set and one metadata struct for every essence; four exchange models
  and twelve metadata structs are the legacy ABI), `struct_size` on the ops structs (would need
  `*_create2`, a second API in all but name), removal of the tasklet `notify_*` callbacks (only
  optional, not removable), or one lease and result contract for imported memory (five "done" points
  today). So: engines first, Phase 0.5 on the legacy API, then the unified API (D-24, D-66). Option
  (c) of M1 ([decisions.md](decisions.md) §2.2) keeps it open; the engines track and Phase 0.5 stand
  on their own if the maintainer stops there
- **Rebase PR #1610.** Keep PR #1610's `include/mtl_session_api.h` (808 lines, 29 functions) and ≈
  3.3 kLOC in `lib/src/new_api/`, rebased onto `main`. *Outcome:* Rejected (D-27, M9): only ST20
  frame mode was implemented; it wrapped the low-level `st20_tx_create`/`st20_rx_create` layer and
  re-implemented pipeline behaviour (conversion, frame state machine, drop-when-late, blocking get)
  by reaching into transport internals; TX user pacing, user timestamp and `user_meta` never reached
  the wire (the transport overwrote `tv_meta`); RX "user-owned" copied or converted a full frame
  inside the RX tasklet; RX VSYNC and FORCE_NUMA were never enabled; `compressed = true` silently
  created an ST20 session; `event_poll` refused queued completions after stop; a double `buffer_put`
  corrupted slot state; the zero-copy samples DMA'd unmapped page-cache files. Textual conflicts
  were small (7 build and test files), but `main` had since hardened what the PR duplicated (FIFO by
  sequence, handle guards, blocking waits, drop-when-late `DROPPED`, user-pacing timestamps), and
  the public contract itself was being redesigned. Keep it open as a reference until the unified
  headers land, then close with credit; salvage the vocabulary (one opaque session, media-specific
  create, `event_poll`, `get_event_fd`), `mt_session_event.c`, the completion-contract wording,
  `GRACEFUL_SHUTDOWN.md` as a target spec, the RxTxApp migration shape, the four samples (memory
  handling rewritten) and the parity-suite idea; drop the `.github` changes. The comparison table is
  [migration.md](migration.md) §14
- **The maintainer's review of PR #1610: no library-owned vs user-owned mode.** PR #1610 had
  `MTL_BUFFER_LIBRARY_OWNED` / `MTL_BUFFER_USER_OWNED`. *Outcome:* Adopted as D-01: "ownership"
  mixed five questions (who allocated, who owns the lifetime, who may access now, how it was
  registered for DMA, direct or copied). One slot and lease state machine and one submit/dequeue
  contract for every provisioning path; the review's other asks (media time on the submission, not
  on storage; media time, RTP, launch and observed times apart; one rational timeline for A/V;
  progressive lines within one timed frame; completion as the authority for reuse; refcounted
  regions with layouts and a data-path policy; guarantees mapped to tests; adapt the existing
  pipelines) are D-09, D-38, D-78, D-03, D-16, D-17, D-06
- **Revision 3's single header (`mtl_unified.h`).** One 3029-line header with 203 functions, 93
  structs and 79 enums that every program included (§4). *Outcome:* Replaced by D-71: a lean `mtl.h`
  (C library includes only) and optional headers with one job each. S7 counted 115 hidden rules in
  the examples and ≈ 82 % API ceremony; the maintainer asked for the leanest include file
- **S6, a minimal alternative designed from scratch.** `S6-alt/mtl_alt.h`: 381 lines, 30 functions,
  7 structs, 6 enums, compiled as C99 and C++17. Configuration as `key=value` strings
  (`mtl_stream(inst, "video tx addr=... width=1920 ...", &s)`), staged keys committed by
  `mtl_apply(s, at, when)`; one object handle `mtl_h` for instance, stream, timeline, queue and
  region, its kind checked at runtime like a file descriptor; one 320-byte frozen `struct mtl_unit`
  used for acquire, submit, results and attach layouts; per-unit `time_mode` (0 = AUTO); the
  timeline is the group (`mtl_start(tl)`); a null instance = the process-wide default; generic
  `mtl_close`, `mtl_wait` (ready mask), `mtl_wait_handle`, `mtl_interrupt(h, on)`, `mtl_events`;
  packet chunks as plane 0 slots + plane 1 `uint16_t` lengths, `MTL_U_SCATTER` for a pointer table.
  Examples 32 / 61 / 59 lines (minimal TX / A/V/ANC / zero-copy TX). Its diagnosis: revision 3
  was large for three reasons, none of them the data path: every choice was a typed field (93
  structs, 37 `*_init()`, `struct_size` everywhere); every object kind had its own handle type and
  its own close, wait, trywait, interrupt, uninterrupt, get-wait-object and read; one unit of
  media appeared as five structs (buffer view, slot hint, submission, RX unit record, TX result).
  Revision 3 had 217 exported functions (203 + 10 in `mtl_simple.h` + 4 in `mtl_debug.h`), 93
  structs and 4 unions, 79 enums, 195 `#define` constants, 9 handle types plus `mtl_simple_h`; the
  S6 core 30 functions, 7 structs, 6 enums, 60 constants, 2 handle types, no `*_init()`; with its
  12 optional headers (`mtl_keys.h`, `mtl_reasons.h`, `mtl_stats.h`, `mtl_time.h`, `mtl_anc.h`,
  `mtl_unit_ext.h`, `mtl_advanced.h`, `mtl_legacy.h`, `mtl_debug.h`, `mtl_sdp.h`,
  `mtl_formats.h`, `mtl.hpp`; 28 functions) ≈ 1100 lines, 58 functions, ≈ 22 structs, ≈ 9 enums.
  *Outcome:* Rejected as a whole;
  its cuts needed approval and broke rules earlier reviews set: C1 no static typing between object
  kinds (wrong kind compiles); C2 no compile-time check of configuration names or value types; C3 no
  `struct_size` on inputs (R-ABI-1); C4 one atomic start set per timeline; C5 no hold after submit
  with results off; C6 no `mtl_call_seq`; C7 untyped event payloads (`arg[3]`) and frozen record
  sizes. It also did not remove decisions (≈ 90 per video TX stream, only moved into a key
  catalogue) and per-unit time mode reopened C2 #19. Its own §7 list was mined instead: taken were
  one unit struct (D-75), slots by index (D-76), packet chunks as a plane convention (D-82, with
  lengths in the meta area), keys for the long tail only (D-73), the error record naming the field
  (`mtl_last_error`), staged-then-apply as one `mtl_session_update` (D-72, typed, not staged keys),
  generic verbs only as `struct mtl_object` for events, stats and options; the spec-string
  constructor was taken (D-72) and later removed (D-97); per-unit time mode and the
  timeline-as-group were not taken (D-38 keeps the session media mode; §3.2). Taken only in part:
  the one unit covers acquire, dequeue and submit; TX results stay their own record (`struct
  mtl_tx_result`, the full record by size) and attach layouts are `struct mtl_attach`, not arrays
  of units; S6's `mtl_wait` returning the ready mask became `mtl_session_wait` / `mtl_queue_wait`
  returning the ready subset, with `-MTL_EAGAIN` arming (D-88). Not taken: generic `close`,
  `wait`, `interrupt`, `events` across object kinds; the `ext` chain on frozen data structs (inputs
  carry `struct_size`, records take their size per call, D-74); `MTL_GET_SLOT` instead of a
  dynamic pool (`mtl_tx_acquire_layout`, `MTL_LATER`); staged keys and `mtl_apply` (typed
  `mtl_session_update`, D-72); `mtl_keys.h` string macros (option IDs `MTL_OPT_*` with
  `mtl_option_list`); `mtl_stats.h` typed snapshots (S5 P10); a header-only `mtl.hpp` with typed
  C++ wrappers and a move-only `Lease` (the C handles are typed already; `examples_cpp.cpp` shows
  an RAII lease guard). `mtl_reasons.h`, `mtl_legacy.h`, `mtl_debug.h` and `mtl_sdp.h` exist
  under those names. Other `mtl_alt.h` rules a reader meets, none in revision 4: `-MTL_ETIMEDOUT`
  for an expired data wait (now `-MTL_EAGAIN`, D-88); a failed `mtl_put` left the lease with the
  caller (now the slot returns to the pool, D-88); `get` read only `buf` and `ext`, so an
  uninitialised unit was legal (now `MTL_INIT` once); states ARMED, DRAINING, FLUSHING and CLOSING
  (now `enum mtl_state`, D-08); `MTL_STOP_DISCARD` staying RUNNING plus `MTL_U_REBASE` (now
  `mtl_session_discard`); `results=none|errors|all` (now on or off, D-77); a 128-B `struct
  mtl_event` with `arg[3]` and `struct mtl_error` with `key[40]` (now typed `old_value`,
  `new_value`, `value[2]`; `mtl_last_error` names the field)
- **Spec strings (early revision 4).** `mtl_session_open(mt, "video tx addr=239.1.1.1:20000
  1080p59.94 format=yuv422_10", &s)`, `mtl_session_config_parse`, `mtl_instance_open(spec, &params,
  &mt)`, `spec_options[16]`. *Outcome:* Removed by D-97 (the maintainer's direction): less readable
  and a bigger change for legacy users than typed fields with enums in the legacy order. `MTL_PORTS`
  stays for tests and pods. Option names stay, for GStreamer properties, FFmpeg AVOptions and
  bindings

### 3.2 Smaller design alternatives rejected on the way

| Alternative | Where | Why rejected |
|---|---|---|
| A startable timeline as the group (S2 H2a) | S2 P5 | implicit membership would make a start on a named timeline start other framework elements' sessions, and on the epoch timeline the whole instance; an explicit join list is the group again; replaced by start arrays (D-78) |
| A group object (`mtl_group_*`, revision 3, D-12) | S2 P5 | 7 functions, a config, a state enum and `GROUP_MEMBER_FAILED` for what an array start does; superseded by D-78 |
| Separate CQ and EQ objects, an instance EQ, `mtl_eq_post` user events, per-port subscription masks (D-61) | S2 P7, S5 P7 | one queue type (`mtl_queue.h`) gathers results, RX readiness and events; user posts are not an MTL feature today; superseded by D-79 |
| A single record union for results and events in one queue | S2 P7 | rejected inside P7: results stay lossless and ordered, events coalesce, so they keep separate reads |
| An implicit process-default instance (null `mt` = default) | S2 P11 | hidden global state (C5 §2.12) and ports still need configuring; `MTL_INSTANCE_SHARED` is explicit |
| Merge the lease into the buffer | S2 P11 | C5 §2.5 Blocker: every access-moving misuse would compile; the lease stays its own type (D-76) |
| Drop `trywait`, keep only wait objects | S2 P11 | no race-free "arm then block" without it; revision 4 instead arms on every `-MTL_EAGAIN` (D-88) |
| Instance-only interrupt | S2 P11 | GStreamer `unlock` is per element (Q-LIFE-6); both `mtl_session_interrupt` and `mtl_instance_interrupt` exist |
| Drop deferred destroy | S2 P11 | GstBufferPool and AVBufferPool semantics (Q-LIFE-4); `mtl_session_close` returns 1 while leases are out |
| Generic functions taking a raw `uint64_t` | S2 P11 | any integer compiles; typed handles kept |
| A blocking destroy by default | S2 P9 | frameworks must be able to finish `stop` while downstream holds buffers |
| One `mtl_close(obj, flags)` for every object (S2 P2b) | S2 | not taken: typed `mtl_session_close`, `mtl_instance_close`, `mtl_timeline_close`, `mtl_queue_close`, `mtl_mem_close` keep the type checks; the verb was unified to "close" instead (RV-47) |
| A "profile" field | S3 P4 | the session's media mode and source kind already are the profile (D-38, D-39) |
| Media mode inferred from the submission | S3 P4 | the mode is needed before the first unit: a start array rejects AUTO TX sessions (`START_SET_MIXED`) before any unit exists; late, absorb and snap defaults and `min_submit_lead_ns` are fixed at create and reported by the query; `media_index = 0` and `media_tai_ns = 0` are legal, so inference needs valid bits and a forgotten bit silently becomes AUTO ([timing.md](timing.md) §4.1) |
| Video + cvideo as one config with a `codec` field | S3 P6 | the essences differ in SDP media type, stats, pacing model (no VRX for cvideo) and info; a forgotten `codec` would create ST 2110-20, PR #1610's failure mode in a weaker form |
| ANC + fastmeta as one "metadata" config | S3 P6 | only raster, target delay and a capacity overlap; payload model (packet table vs data-item bytes), wire format and filters differ; only two small merges inside configs were taken |
| Keep the config sub-structs and move rare blocks behind `next` chains | S3 P3 (B) | every rare knob costs the application a chained block, the library a chain walker per create and bindings a chain builder; scalars gain nothing over options; `next` stays reserved for structured groups |
| An all-options config (direction + flows + options) | S3 P3 (C) | turns the timing recipes into key arrays and loses designated-initialiser readability for the ≈ 15 values most applications set |
| Flows by pointer + count instead of `flows[MTL_MAX_LEGS]` | S3 P3 (D), §6 Q10 | would save 160 B and take `MTL_MAX_LEGS` out of the ABI; not taken: the array stays in `struct mtl_session_config` |
| A whole-session spec string ("tx 239.1.1.1:20000 1080p59.94 …") | S3 P8 | a mini-language duplicating three parsers; all string configuration was later removed (D-97) |
| Reason packed into the return code | S5 P11 | one code with one meaning (D-21); the reason is in `mtl_last_error` and the status |
| Reasons as strings only | S5 P12 | applications must branch on a stable constant; `mtl_reason_name()` gives the string |
| State, `blocked_on` and errors as stats keys | S5 P13 | a running loop branches on them; they stay typed in `struct mtl_session_status` |
| A typed 8-counter summary struct next to the registry | S5 P10 | not taken (default no); names only |
| The revision-3 default-instance merge table (invariant / mergeable / ignored fields, `ignored_fields` bits) | revision-3 document 03 §7.2 | replaced by `-MTL_EEXIST` (`INSTANCE_MISMATCH`) on a differing shared open (D-44); see OI-1 and OI-43 |
| `MTL_INSTANCE_MANAGER_OPTIONAL`, `MTL_INSTANCE_PREFLIGHT`, `MTL_SENDER_IPMX` | addenda | removed by RK-28, RK-32, RN-23 (§6.8, §6.9) |
| REANCHOR as the CAPTURE snapping default | C5 §5.8 | NEAREST snapping for every source kind keeps RTP on the grid; REANCHOR would jump the RTP offset at every re-anchor (D-38) |
| Auto-select W2 (tasklet eventfd write) below 1 ms | C5 §4.10 | kept as the recommendation but left to the maintainer (M6): it is a syscall on a pinned core |
| Variable-size ST 2110-22 as a mode | C5 §5.12 | ST 2110-22 requires constant bytes and packets per frame; CBR stays the default, `MTL_CVIDEO_VBR_MAX` is opt-in and flagged non-compliant (D-32) |
| Completion mode EXCEPTIONS | S4 P6 | saved bandwidth, not safety; results on or off (D-77) |
| A per-unit cookie auto-promoting results | C5 §3.6 | would re-create C3's silent stall; a cookie without results fails (`COOKIE_WITHOUT_RESULTS`) |
| `MTL_INSTANCE_SHARED` as the default | S2 P8 | a shared default merges two applications of one process that open different NICs, and the second open fails; the default stays private, sharing is a flag |
| Discard as a stop mode | S2 P9c | discard keeps the session RUNNING (GStreamer flush and seek); a "stop" that does not stop is a trap; `mtl_session_discard` stays separate |
| A dry-run flag on create | S2 P4 | create has no `info` or `req` outputs; `mtl_session_query` keeps its own signature |
| Single-session `mtl_session_start(s, when)` plus `_start_all` / `_stop_all` | S2 P5 | +2 functions for the same semantics; the array form with n = 1 was taken (D-78) |
| An explicit `video` session handle in the ANC and fastmeta configs | S2 §5 Q4 | the first video of the start array, else the timeline's video owner, gives the association; on the epoch timeline the raster is required instead ([timing.md](timing.md) §7.1) |
| Instance events read from the instance (`read_events(instance)`, a single-reader stream) | S2 P7a, §5 Q5 | port, time and instance events reach the application only through a queue subscription (`MTL_SUB_PORT`, `MTL_SUB_TIME`, `MTL_SUB_INSTANCE`, [contract.md](contract.md) §10.3) |
| A lease ID with bit 63 set and a 15-bit session index, so a lease passed as a generic object fails with `-MTL_EBADF` | S2 P2, §5 Q9 | not needed: the verbs stay typed, so a lease is never passed as an object; leases keep `session:16 \| slot:16 \| generation:32` |
| A `kind` header field in every versioned struct, checked as `WRONG_KIND` | S2 P3, §5 Q7 | `struct_size` only (D-74); the member-per-essence config (D-72) removed the need for a runtime essence check |
| A library-owned unit pointer, `mtl_tx_acquire(s, struct mtl_unit** u, t)`, like `st20p_tx_get_frame()` returning `struct st_frame*` | S4 P1 | a write after release lands in the next acquirer's unit, and submit still snapshots the per-use fields (only ≈ 130 B saved); kept as spike S3's comparison: if the 208-B copy shows, a per-session flag could return such a pointer |
| Lease only, plus getters (`mtl_lease_addr(lease, plane)`, `mtl_lease_stride(...)`) | S4 P1 | fine for TX planes, but RX needs status, used, media time, RTP and flags: one getter per field |
| A separate in-args struct, `submit(s, lease, &args)`, next to an out-unit | S4 P1 | two structs again and TX/RX asymmetric; V4L2's `struct v4l2_buffer` (QBUF/DQBUF, `bytesused`, timestamp and flags both ways) shows one in/out unit works |
| A slim unit without `plane[]`, planes cached by slot | S4 P1 | saves 96 B per call but puts the per-slot cache back on the simplest path |
| One `media` field interpreted by the session's media mode | S4 P2 | an index submitted to a TAI session would read as a time near 1970 and be dropped late instead of failing; with `media_index` and `media_tai_ns` apart, a value in the field the mode does not use stays detectable |
| No copy at submit: the builder bounds-checks each `struct mtl_anc_packet` at pick-up (`udw_offset + udw_count ≤ used`, `count ≤ 255`; ≤ 255 compares per ANC unit on the tasklet) | S4 P2, Q4 | the meta area is validated and copied at submit on the application thread (≤ 4 KiB memcpy, today's cost; D-86, RV-03), so later writes never reach the wire and the tasklet does no extra checks |
| A detail follow-on record per unit (a `MORE` flag, like `TX_SOURCE_RELEASED`) | S4 P3 | doubles the records for every detail reader; the record size selects the depth instead |
| A per-session "detail" flag | S4 P3 | redundant with the record size passed to reap |
| The missing ranges as a CQ record kind (`RX_MISSING`, inline `missing[4]`) | S4 P3 | replaced by `mtl_rx_get_missing()` while the lease is held |
| Attach from an array of plane pointers, layout derived from the session | S4 P4 | framework pointers are 64-B aligned, not page aligned; covering them needs outward page rounding, which maps neighbouring memory into the NIC (C5 §6.5, D-16) |
| Infer the pool source from "slots attached before start" | S4 P4 | library pools are allocated and their defaults resolved at create; inference would move allocation to start (or allocate twice) and leave create's capacity check incomplete; the source stays one flag, `MTL_SESSION_POOL_ATTACHED` |
| Direction-generic `get`/`put` for TX and RX (as `st20p` does today) | S4 P5 | merges submit (an ownership transfer with an outcome, DPC) with RX release (a return, DP); only one `mtl_lease_release(lease)` for both releases was an option, and D-34 kept `mtl_tx_release` / `mtl_rx_release` |
| A `producer_class` in `MTL_EVENT_OVERFLOW` (D-61, the C5 §7.5 remedy) | S5 P6 | it mattered only because application `USER` posts shared a ring with library events; with user posts gone (P7) the remedy after any overflow is the same: re-read the getters |
| A separate `RX_DISCONTINUITY` event | S5 P7 | the per-unit `MTL_UNITF_DISCONTINUITY` flag is lossless and already the getter of record (every RX session has unit records); `rx.discontinuities` counts them; OI-34 keeps the event out (option (b)) |
| One `mtl_ipmx.h` for `mtl_rtcp.h` and `mtl_crypto.h` | RA §3.2 | RTCP has an RFC 3550 mode that is not IPMX-only; crypto has its own security review, key-handling rules and phase (Q-NI-3); a combined header would pull key handling into every RTCP user and RTCP into every non-IPMX crypto user (HDCP over packet units) |
| Merge `mtl_sdp.h` into `mtl_util.h` (both "built only on public calls, allocates nothing") | RA §3.2, §6 | proposed by RA, not taken: `mtl_sdp.h` stays its own header under `MTL_LATER` (Phase 7), so the 17-header count holds; `mtl_util.h` lost its parsers with D-97 |
| Fold `mtl_instance_shutdown` into close: a thread-local `mtl_last_shutdown_report()` and DRAIN as `instance.shutdown_drain` (RK), or the summary in `mtl_last_error().detail` and `instance.close_drain` (RA), or `mtl_instance_close_all()` | RK §7, RA §3.2 | the flags and the report need a call ([deployment.md](deployment.md) §4.2); `mtl_last_error` is valid only after a failure (R1, D-84), so returns 0 and 1 would need a new rule; `MTL_SHUTDOWN_ALL_REFERENCES` has no option form and stays with per-reference handles (RK-22). Slimmed instead: `enum mtl_shutdown_outcome` removed, `references_left` (RA-15) |
| Drop `MTL_CPU_SHARED_SLEEP` (it duplicates `MTL_INSTANCE_TASKLET_SLEEP`) | RK §7 | not taken: `enum mtl_cpu_shared` keeps WARN, REFUSE, SLEEP |
| Drop `MTL_HEALTH_STARTING` (= phase below READY) and `MTL_PHASE_SHUTDOWN` (= `MTL_HEALTH_SHUTTING_DOWN`) | RK §7 | RK itself kept them: probes read bits, not the phase |

## 4. What revision 3 looked like

Revision 3's sketch (2026-09-30) was `mtl_unified.h` (3036 lines with the banner: the whole API), `mtl_simple.h`
(the L4 "simple" layer: `mtl_simple_open` from a port-spec string, frame, send, close, plus
`mtl_simple_session()` and `mtl_simple_interrupt()`) and `mtl_debug.h` (test time source, fault
injection, null-backend note), 12 examples in C99 (`ex01_tx_minimal` 63 lines … `ex12_simple` 46)
and one in C++17, and a `check.sh` that compiled them plain, `-DMTL_UNIFIED_NO_INLINE` and
`-DMTL_UNIFIED_LATER`, with gcc and clang, and checked that every struct had a size check and every
`struct_size` struct an exported `*_init()`. It supported 64-bit targets only (`#error`).

### 4.1 Its conventions C1–C11, and what revision 4 made of them

| Rule | Revision 3 | Revision 4 (`mtl.h` R1–R8) |
|---|---|---|
| C1 Returns | 0 or a negative `MTL_E*`; arrays a count ≥ 0; `*_trywait` 1 ready / 0 armed / < 0, never `-MTL_EAGAIN`; `get_state` returns the state | R1: 0 or a count, negative `MTL_E*`; trywait gone, `-MTL_EAGAIN` arms (D-88) |
| C2 Timeouts | last argument, `int64_t` ns, 0 = do not wait, `MTL_TIMEOUT_INFINITE` (-1); nothing ready: arrays 0, singles `-MTL_EAGAIN`; expiry `-MTL_ETIMEDOUT` | R2: one "nothing now" code, `-MTL_EAGAIN`, for timeout 0 and expiry alike (S7 F-06); `-MTL_ETIMEDOUT` only for a DRAIN stop that missed its deadline (OI-10) |
| C3 `struct_size` | set by the exported `mtl_<struct>_init()` or a C-only value macro; library reads min(size, known); 0 or below the 0.1 size, or non-zero bytes past the known size, `-MTL_EINVAL` (`NONZERO_TAIL`); reserved must be 0 | R3: `MTL_INIT(&s)` / `mtl_struct_init(p, size)` for every input; outputs carry no `struct_size` and take a size argument (D-74) |
| C4 Array reads | caller's record size as an argument; `hdr.size` an output in CQ records | kept for `mtl_tx_reap`, `mtl_queue_reap`, `mtl_queue_read_events` (D-42) |
| C5 Zero default | a zero-filled input is the default; non-zero meanings in document 09's defaults table, checked in CI; `MTL_NUMA(n)` | kept for typed fields; options use presence (absent = default, D-73); the 09 defaults table went into the header comments and contract.md |
| C6 Enumerated fields | `uint32_t` fields, enums only name constants; flags `uint64_t`, unknown bits `-MTL_EINVAL` (`UNKNOWN_BITS`) | kept; new enums follow the legacy order (D-97) |
| C7 Handles | 64-bit values of distinct struct types, 0 = null; `type:8 \| reserved:8 \| index:16 \| generation:32`; leases `session:16 \| slot:16 \| generation:32`, random seeds; grow-only chunked tables | R4: kept (D-07), with handle slots process-wide and never freed (RK-5) |
| C8 Times | `struct mtl_time {value, clock, flags, accuracy}` in arguments; `int64_t` TAI ns + `time_valid` in CQ records | R5: every time is `int64_t` TAI ns, validity by flags (D-15, `MTL_UNITF_*`, `MTL_TXR_*`) |
| C9 Versions | `since 0.N` on section banners, inherited; `MTL_UNIFIED_API_VERSION` vs `mtl_version_num()` | `MTL_API_VERSION`, `mtl_version_num()`; the experimental DSO (D-23) |
| C10 Call classes | `MTL_API_CP`, `_DP`, `_DPC`, `_WT`, `_AS`, enforced in debug builds; from busy loops only the inline-safe subset (acquire, dequeue, reap with timeout 0, submit, releases, DP getters), trylock only; else `-MTL_EDEADLK` | R6: kept (D-05, D-47); R6 also names the two library threads that call application code (RV-16) |
| C11 `MTL_NULLABLE` | marks pointers that may be NULL; others `-MTL_EINVAL` | kept |

### 4.2 Its instance and its defaults

- **Instance.** `struct mtl_instance_params` carried a fixed `ports[]` array of `struct
  mtl_port_spec` (name: a PCI BDF, `kernel:<if>`, `native_af_xdp:<if>` or `null:<n>`; `ip_family`,
  `prefix_len` 0 = 24, `sip`, `gateway`, `tx_queues`, `rx_queues`, `numa`), `lcores` as a string,
  ten `MTL_INSTANCE_*` flags (`MANAGER_OPTIONAL`, `TASKLET_THREAD`, `TASKLET_SLEEP`, `PTP_BUILTIN`,
  `HW_TIMESTAMP`, `BIND_NUMA`, `RX_SEPARATE_VIDEO_LCORE`, `TX_VIDEO_MIGRATE`, `RX_VIDEO_MIGRATE`,
  `TASKLET_TIME_MEASURE`), `sched_max`, `sched_quota_mbs`, `ptp_domain`, its own log-level enum
  (`enum mtl_unified_log_level`, 0 = INFO), `max_queues`, `cmd_ack_timeout_ns` (0 = 100 ms). S3 P9
  cut it from 1496 B to 240 B (ports by pointer, flags 10 → 5, numeric tuning to instance options);
  D-97 then made it typed only.
- **The merge table** (revision-3 document 03 §7.2) told a second `mtl_instance_acquire_default` per field
  whether it was invariant (`-MTL_EINVAL`, `INSTANCE_PARAM_MISMATCH`), mergeable, or ignored (bit in
  `mtl_instance_info.ignored_fields`, one `warn`); a zero field matched anything and set flag bits
  were requests. Today FFmpeg shares an instance by a `memcmp` of `mtl_init_params`
  (`ecosystem/ffmpeg_plugin/mtl_common.c:166-181`) and GStreamer ignores later parameters
  (`ecosystem/gstreamer_plugin/gst_mtl_common.c:657-686`). Queues were "mergeable at first open
  only" because they are configured once (`dev/mt_dev.c:993`); `lcores` was ignored on a second
  acquire because EAL's lcore set is fixed at init (OBS passes per-source `lcores`). Revision 4
  replaced the table with `-MTL_EEXIST` (`INSTANCE_MISMATCH`) for differing ports, lcores, time
  source or options (D-44, RV-24); what that costs OBS and FFmpeg is OI-43.
- **The defaults table** (revision-3 document 09 §9) listed every field whose zero meant something other than
  the number zero, checked against the header in CI. Its content now lives in the header field
  comments and [contract.md](contract.md) §3; option defaults are in `mtl_options.h` and
  [contract.md](contract.md) §12.

## 5. The simplification studies (revision 4)

S1 (coverage inventory), S7 (samples friction), S8 (RTP passthrough) and S9 (hiding the session
headers) are in [coverage.md](coverage.md) and [contract.md](contract.md) §13. S8 proposed its
decisions as D-71 (packet units), D-72 (the `rtp_ts_mode` rename and the no-"passthrough" naming
rule), D-73 (generic RTP essence) and D-74 (verbatim by default); those numbers went to other
revision-4 decisions, and all four became D-82 (naming rule: [contract.md](contract.md) §13.1);
its renamed symbols are in §2. The outcome of the other studies, proposal by proposal (verdicts
RECOMMEND / OPTION / REJECT were the study's; "Taken" is what the headers have now). S2 counted
137 of revision 3's 203 core functions (1025 of 3029 lines) in its area; 86 of them were per-type
duplicates with no behaviour of their own (15 wait and interrupt over session, CQ, EQ and
instance; 7 destroy, release and close over 6 types; 18 null and equality over 9 types; 36
`*_init`; 10 create and query, one per essence). Its target: ≈ 53 functions, ≈ 560 lines, 8
handle types and 16 concepts instead of 29 in its area; header-wide 203 → ≈ 102 before tiering.

| Study | Proposal | Taken? |
|---|---|---|
| S2 object model | P1 three handle macros (`MTL_NULL`, `MTL_IS_NULL`, `MTL_SAME`) instead of 18 `is_null`/`eq` twins | yes (`mtl.h`) |
| S2 | P2 generic object verbs on `mtl_obj_h` (wait, interrupt, reap, read events) | partly: `struct mtl_object` / `mtl_obj()` for events, stats, options and fault injection; the verbs stay typed per object |
| S2 | P2b one `mtl_close` | no (§3.2) |
| S2 | P3 one initialiser `mtl_struct_init` + `MTL_INIT`, with a `kind` field | yes without `kind` (S5 P8, D-74) |
| S2 | P4 one create and one query; P4c the essence config as a member of the session config | yes, P4c chosen (D-72) |
| S2 | P5 no group object, start/stop arrays | yes (D-78) |
| S2 | P6 one `mtl_timeline_create` with a name, refcounted close; null = epoch | yes (`mtl_sync.h`) |
| S2 | P7 no CQ/EQ in core; one `mtl_queue_h` in `mtl_queue.h` | yes (D-79) |
| S2 | P8 one `mtl_instance_open` with `MTL_INSTANCE_SHARED`; legacy bridge in `mtl_legacy.h` | yes (spec argument later removed, D-97) |
| S2 | P9 one `struct mtl_when`, one `mtl_session_update`, discard without a params struct, deferred close returns 1; state merge (IDLE, STOPPING) as an option | yes; the state merge not taken (D-08 keeps nine states) |
| S2 | P10 header tiering: lean `mtl.h` + extensions | yes (D-71) |
| S2 | open questions 1–12 | 1 generic verbs on an erased handle: no, verbs stay typed per object (`struct mtl_object` only for events, stats, options, faults); 2 one `mtl_close`: no, typed closes (RV-47); 3 start arrays: the array form, n = 1 for one session (D-78); 4 ANC/fastmeta association: first video of the start, else the timeline's video owner; 5 instance events: through a queue subscription only; 6 P4 or P4c: P4c, one config with a member per essence (D-72); 7 `kind` in every struct: no (D-74); 8 state merge: no, nine states (D-08); 9 lease encoding: `session:16 \| slot:16 \| generation:32`; 10 USER posts: dropped (D-79); 11 `mtl_api.h`: installed in the legacy tier until release F+2, `mtl_legacy.h` the bridge until then ([migration.md](migration.md) §6.2, §8); 12 `mtl_timeline_epoch`: gone, the null timeline is the epoch |
| S3 configuration | P1 typed core (≈ 75 → 40 typed leaves per session) + presence-based options | yes (D-73) |
| S3 | P2 one options API on `struct mtl_object`, with a static key list | yes (`mtl_set_option(struct mtl_object, …)`, descriptors) |
| S3 | P3 flatten `mtl_session_config` (792 B → 384 B), no `leg_count`, flags for RX pool choices | yes (a leg exists when its `udp_port` ≠ 0; `MTL_SESSION_RX_*` flags) |
| S3 | P4 profile field | no (§3.2) |
| S3 | P5 shared `struct mtl_raster`, inherited by ANC and fastmeta | yes (`mtl.h`; RV-09 made it required on the epoch timeline) |
| S3 | P6 essence merges | no (§3.2) |
| S3 | P7 kind tag in media configs; optional generic create | superseded by the member-per-essence config (D-72) |
| S3 | P8 string helpers (`mtl_video_config_parse`, `mtl_option_parse`, SDP parse) | taken in early revision 4, removed by D-97; SDP parse is `mtl_sdp.h` (Phase 7) |
| S3 | P9 instance params cut (1496 B → 240 B, flags 10 → 5, `MTL_INSTANCE_PTP_BUILTIN` removed, AUTO never starts built-in PTP) | yes (D-85) |
| S3 | P10 one `struct mtl_when` | yes |
| S3 | P11 defaults that make the zero config right (`format` from `app_format`, ANC `fps`, no `MTL_TIMELINE_GRID_EXPLICIT`) | yes (`format` 0 = from `app_format`) |
| S3 | findings F1–F3 on revision 3 (not S7's F-01…F-26), all closed | F1 untagged `reconfigure.media_config` (video and ANC configs both 128 B): gone with the member-per-essence config (D-72); F2 no way to raise `horizon_ns` in STOPPED: `MTL_OPT_HORIZON_NS` (mark S); F3 `caps.hw_pacing` and `caps.pacing_class` overlapped: `MTL_OPT_PACING` + `MTL_OPT_PACING_REQUIRED` |
| S3 | findings F4, F5 | F4 a PTP flag and the time source said one thing twice: `mtl_instance_params.time_source` only (D-85); F5 `MTL_TIMELINE_GRID_EXPLICIT` redundant: `grid` {0, 0} = derived |
| S3 | findings F6–F10, all closed | F6 ANC `fps` 0 undefined: `anc.video` required on the epoch timeline; F7 fastmeta and rows fields in common structs: `data_item_type`/`k_bit` with `MTL_FASTMETA_RX_MATCH_*`, `sc.unit`; F8 the option list needed a session: `mtl_option_list` needs none; F9 an unshipped alias `MTL_OPT_HIST_LINEAR`: deleted; F10 three "when" encodings: `struct mtl_when` |
| S3 | open questions 1–10 | 1 live options: the R mark (`tx.index_offset`, `log.level`, …), applied at a unit boundary; 2 value width: `struct mtl_option` has `str` for string keys; 3 reset: `mtl_reset_option()`, not an `INT64_MIN` sentinel (C5 §2.20); 4 generator: the header lists every key, `mtl.h` does not include `mtl_options.h`; 5 `troffset_ns` and `sender_type` stay typed (GATEWAY recipe; an explicit 0 TROFFSET is `MTL_OPT_TROFFSET_NS`); 6 AUTO + CAPTURE is defined: AUTO's late policy RESLOT for every source kind ([timing.md](timing.md) §6.2); 7 `rx.incomplete` DELIVER: decision M13; 8 `ignored_fields`: gone with the merge table (`-MTL_EEXIST`, D-44); 9 info mirrors of options: the effective value is `mtl_get_option`, `info.*` keys report granted values; 10 flows by pointer: not taken. A backend-private key range (0x8000–0xFFFF) was proposed; the header reserves none |
| S4 data path | P1 one caller-owned `struct mtl_unit` (208 B) | yes (D-75) |
| S4 | P2 the submission folds into the unit; one `used`; ANC table and user meta in the slot's meta area | yes (D-86, snapshot at submit after RV-03) |
| S4 | P3 a 96-byte core result, the full record by size; RX detail and missing ranges on demand | yes (D-77, `mtl_rx_get_detail`, `mtl_rx_get_missing`) |
| S4 | P4 one `mtl_session_attach`, slots instead of `mtl_buffer_h` | yes (D-76) |
| S4 | P5 43 → 28 core + 2 extension + 6 helper data-path functions | yes, in shape |
| S4 | P6 one flag `MTL_SESSION_RESULTS`, app memory always produces results; EXCEPTIONS cut | yes (D-77) |
| S4 | P7 progressive and dynamic pools in extensions; re-submit is publish; `BY_INDEX` a flag; packet chunks | yes (`mtl_rx_wait_rows` in `mtl_sync.h`, `mtl_tx_acquire_layout` under `MTL_LATER`) |
| S4 | P8 pool config as flags; `PREFER_DIRECT` cut; zero fill for app-memory RX pools | flags yes; zero fill only for library pools (OI-25) |
| S4 | P9 submit consumes the lease on failure except `-MTL_EAGAIN` | yes (D-88; rows units RV-01) |
| S4 | P10 region calls in `mtl_mem.h` | yes |
| S4 | open: unit copy versus pointer (spike S3); one release verb; ANC TOCTOU | copy form kept pending spike S3; `mtl_tx_release` and `mtl_rx_release` stay (D-34); snapshot at submit (D-86) |
| S4 | the diagnosis | revision 3's data path and memory were 568 of 3029 header lines: 43 functions, 22 structs + 1 union, 16 enums (5 pool tri-states with 13 constants), a 152-B 20-field `struct mtl_tx_submission`, 5 CQ record kinds in a 256-B union (`rx_unit` 240 B), 21 validity bits, 4 completion modes |
| S4 | the proposal's size | 28 core functions + 8 in extensions and helpers, 16 + 1 structs, 9 enums, 2 + 2 record kinds, ≈ 400 + 70 lines. EXCEPTIONS would have saved 60 × 96 B/s for 60 Hz video and 96 kB/s for 1 kHz audio |
| S5 observability | P1 one stats registry (`mtl_stat_list`, `mtl_stat_read`) | yes (D-80) |
| S5 | P2 host status (instance, memory, port, scheduler, time) into the registry | yes |
| S5 | P3 session info: 256 B typed core + `info.*` keys | yes |
| S5 | P4 lean session status | yes (104 B with the IS-05 update state) |
| S5 | P5 one `enum mtl_reason`, own header | yes (D-81, `mtl_reasons.h`) |
| S5 | P6 one event record, origin a `struct mtl_object` | yes (`mtl_queue.h`) |
| S5 | P7 fewer event routes; `EPOCH_TICK` cut as an option | routes cut (D-79); `MTL_EVENT_EPOCH_TICK` kept, opt-in (`session.epoch_tick`, Q-CMP-4) |
| S5 | P8 no struct kinds or `known_size`; one initialiser; outputs take their size | yes (D-74) |
| S5 | P9 errno-style last error, no `mtl_call_seq` | yes (D-84) |
| S5 | P10 typed summary; P11–P13 | no (§3.2) |
| S5 | P14 merge `LEG_STATE` and `FLOW_STATE` events | no: both events exist (`mtl_queue.h`) |
| S5 | the diagnosis | revision 3's error, status, stats, info, time, host-status and event sections were ≈ 1140 lines (38 %), 57 functions plus the 37 `*_init`, 45 struct types, ≈ 295 constants; four mechanisms for one number (struct field, keyed value, info, status); a 2096-B stats copy (the 1264-B TX block in the union for RX too), 512-B info with 64 fields, 200-B status, 184-B event. Defects F1 exported inits (an out-of-bounds write across versions, [migration.md](migration.md) §7.3), F2 a 64-bit unsupported mask for ≈ 250 values, F3 a converter name through a `uint64_t` getter, F4 no getter for the detected format, F5 port `rx_bytes`/`tx_bytes`/`rx_nombuf` and per-essence session counts missing, F6 no audio timing-parser results, F7 later-only events in 0.1, F8 a deprecated alias in an unreleased header, F9 32-slot per-reason arrays, F10 naming: all fixed by the registry (D-80), `rx.detected.*`, `port.*`, `instance.sessions{essence,dir}`, `tp.dpvr_max_ns`, `converter{name}` |
| S5 | open questions (1) | value type: `int64_t` (taken); validity of keyed times: `INT64_MIN` = never, `MTL_SU_TAI_NS` (taken); port caps as keys only (taken, `caps.*`); dry-run diagnostics: keys only on a created session; status class DP: no, `mtl_session_get_status` is CP; converter name as a label (taken) |
| S5 | open questions (2) | EPOCH_TICK: kept, opt-in; typed summary: no; generated key-name header: no, a CI check; own reasons header: taken, `mtl_reasons.h`; errno contract: taken (D-84); one label string: taken, 47 characters |

## 6. Reviews and the outcome of every finding

| Review | Date | Revision | Lens | Findings |
|---|---|---|---|---|
| C1 | 2026-09-29 | 1 | real-time and implementation feasibility on today's engines | 21 (#1–#21) + a risk table |
| C2 | 2026-09-29 | 1 | timing, pacing and standards (numbers recomputed with exact rationals) | 28 (#1–#28) + an arithmetic table |
| C3 | 2026-09-29 | 1 | usability, role-playing personas P1–P7 and binding authors | 41 (P1-1…P7-3, B-1…B-3) + two tables |
| C4 | 2026-09-29 | 1 | consistency, traceability, research and mode coverage, citations | C-01…C-44, T-01, T-02, §2–§6 items |
| R2 | 2026-09-29 | 2 | verification of revision 2 against C1–C4; citation re-pin log | risks R-1…R-9, items 5.1-1…5.6-6, 13 re-pinned citations |
| C5 | 2026-09-30 | 2 | adversarial users (sample author, plugin maintainer, operator, validation), the header as compiled, plan size and sequencing | 207 entries (§x.y); answered in the C5 response |
| R4 header review | 2026-10-01 | 4 | adversarial review of the revision-4 headers | RV-01…RV-51 open, RV-52…RV-59 resolved by the S7 update |
| R4 coverage check | 2026-10-01 | 4 | every S1 row against the headers | in [coverage.md](coverage.md) |
| RK | 2026-10-01 | 4 + K | Kubernetes and crash safety | RK-1…RK-43 |
| RN | 2026-10-01 | 4 + N | NMOS and IPMX suitability | RN-1…RN-34 |
| RA | 2026-10-01 | 4 + K + N | API coherence and leanness | RA-1…RA-41 |
| RKNA | 2026-10-01 | — | the designer's response to RK, RN, RA | dispositions, §6.8–§6.10 |
| RV | 2026-10-01 | — | verification of RKNA against headers and documents | verdict per RK/RN/RA ID, R-1…R-12 |

Note on IDs: "RV-nn" are the R4 header review's findings; "RV" is also the name of the
verification of RKNA, whose own findings are "R-n" (§6.11), unrelated to R2's risks "R-1…R-9"
(§6.6) and to the requirement IDs `R-AREA-n` of [requirements.md](requirements.md).

Where the reviews RK, RA and RN disagreed, the response (RKNA) chose ("the §6 choices"):

| Topic | Proposals | Chosen, and why |
|---|---|---|
| fold `mtl_instance_shutdown` into close | RK: fold, with a thread-local report; RA: keep | keep: the flags (all references, drain) and the report need a call; close stays the one-argument path (§3.2) |
| the unsafe close result | RK-21: `-MTL_EIO` + `QUEUE_QUARANTINED`; RA E13: `-MTL_ETIMEDOUT` | `-MTL_EIO`: `-MTL_ETIMEDOUT` keeps its benign "drain ran out" meaning |
| memory under leases still out | RK-12: pick one; RA E5: freed after the last lease | freed after the last lease, by the next CP call or at exit (a DP release cannot free); keeping it until exit would leak on every GStreamer NULL → PLAYING cycle |
| cancel results | RA E3: 0 / 1 / `-MTL_EBUSY`; RN-28: 0 when none pending | RA: 1 for "none pending" lets the Node tell it from a real cancel |
| `MTL_SENDER_IPMX` | RA E2: renumber with `MTL_SENDER_DEFAULT = 0`; RN-23: drop it | drop it: the IPMX profile gives N the TR-10-1 CMAX and VRX, so `MTL_SENDER_N = 0` means N under both profiles |
| crypto parameters | RA E9: options; RN-5: changeable at an activation | options that are R keys: both satisfied, the extension chain goes |
| per-activation options | RA E7: R keys in the config apply at the boundary; RN-3: a new part `MTL_UPDATE_OPTIONS` | RA: no new part |

### 6.1 C1 — real-time feasibility

| ID | Finding | Outcome |
|---|---|---|
| C1 #1 (blocker) | exactly-once ordered results cannot be built over unmodified pipelines: no session tasklet, st20p frees the slot before the callback, converting sessions get no callback, copy paths signal at build | fixed: results built by the reader from a lease table (D-03); the slot interface with held slot, done hook, reclaim, rejected-at-pick-up (D-06, [engine.md](engine.md) §3, §5) |
| C1 #2 | TX results arrive ≈ `nb_tx_desc` (512) packets late (≈ 1.9 ms at 1080p59.94) and never while idle; the busy flush cannot run on a tasklet | fixed: idle `rte_eth_tx_done_cleanup`, rate-limited, dedicated queues only (Q-CMP-7, [engine.md](engine.md) §9); completion latency reported (`mtl_buffer_requirements.completion_latency_ns` and the `info.completion_latency_ns` key, OI-19 closed); spike S6 |
| C1 #3 | the armed-waiter protocol loses wake-ups (release store then seq_cst load has no StoreLoad order) | fixed: seq_cst fence on both sides, one per unit ([engine.md](engine.md) §5.2, §7.1); litmus test G-52 |
| C1 #4 | the W3 waker costs more and wakes later (timer slack 50 µs, 20k–50k wakes/s, inherited affinity, unusable at 125 µs audio, one contended bitmap) | fixed: deadline-driven waker, per-scheduler wake words, `PR_SET_TIMERSLACK`, explicit affinity, optional `SCHED_FIFO` (OI-9), W0 for short packet times ([engine.md](engine.md) §7.2); spike S1 |
| C1 #5 | Q-THR-5's state gate + QSBR is unsafe for arbitrary app threads and saves nothing in v1; the real cost is false sharing | fixed: an in-flight counter on its own cache line + generation-tagged handles over type-stable tables (D-07, Q-THR-5, [engine.md](engine.md) §5.7) |
| C1 #6 | stats are not single-writer today; the v1 read path takes the session spinlock, so tasklets skip the session | fixed: per-writer counter blocks, reader-side deltas, no spinlock on reads ([engine.md](engine.md) §2.8); H9 in [engine.md](engine.md) §2.1 |
| C1 #7 | "the tasklet never makes a syscall" is false for kernel socket TX, AF_XDP kick and built-in PTP PHC reads | fixed: per-backend syscall table, G-39 scoped ([engine.md](engine.md) §2.3); the published time base for every source (Q-THR-7a, [engine.md](engine.md) §8) |
| C1 #8 | EQ coalescing in an SPSC ring races; the EQ has more producers than stated | fixed: per-source pending state for tasklets, one ring per non-pinned producer class ([engine.md](engine.md) §7.5) |
| C1 #9 | the performance gates miss what detects pacing damage | fixed: p99.99/max iteration time, completion latency, waker CPU and wake latency, DP cost without conversion, ST 2110-21 under stress ([implementation-plan.md](implementation-plan.md) §8.4) |
| C1 #10 | `update_destination` is not a pointer swap; socket GSO and RTCP keep the old destination; ack ≠ wire switched | fixed: double-buffered header templates applied at a boundary, a queue swap for the socket backend, RX add-before-remove ([engine.md](engine.md) §4.2, §6); SF-36, SF-37 |
| C1 #11 | recovery off the tasklet needs a quiesce handshake; two paths can signal DONE (#1147 class); audio recovery spins with a blocking lock (`st_tx_audio_session.c:2742`); the worker needs the waker | fixed: worker recovery with the flag, lock and CAS-claim handshake ([engine.md](engine.md) §10; Q-THR-9; SF-39); the audio detail is recorded as H4 in [engine.md](engine.md) §2.1 |
| C1 #12 | lost-completion paths: the builder claims a frame and returns without building it | fixed: rejected-at-pick-up callback (SF-38, R2 in [engine.md](engine.md) §11) |
| C1 #13 | DP promises O(1) but submit and dequeue convert in the caller | fixed: the DPC call class (D-05, [contract.md](contract.md) R6) |
| C1 #14 | documents disagree on which log levels a tasklet may emit | fixed: tasklets never format in release builds; binary log ring ([engine.md](engine.md) §2.8; R-OBS-5) |
| C1 #15 | "never allocate" needs a definition (mempool get/put is per packet) | fixed: no `malloc`, `rte_malloc`, pool or ring create; mempool get/put allowed ([engine.md](engine.md) §2.2) |
| C1 #16 | RX zero fill must not run on the RX tasklet (5 MB memset) | fixed: in the caller's dequeue or a worker ([contract.md](contract.md) §9.8; D-37) |
| C1 #17 | "always-on" timing-parser counters cost double divisions per packet | fixed in part: opt-in, integer math (`rx.timing_parser`, Q-OBS-3); open: NIC-timestamped packets only versus today's SW time on VFs (OI-29) |
| C1 #18 | the TX time taken at `tx_burst` is enqueue time in RL mode | fixed: `enqueued_tai_ns`; ON_TIME is an admission verdict; observed times only from NIC timestamps ([timing.md](timing.md) §6.6, Q-CMP-6, Q-TIME-10) |
| C1 #19 | internal contradictions: submission ring vs `put_frame`; start/destroy use the tasklet handshake though video sessions attach under the spinlock (`tv_mgr_attach`); "microseconds" completion latency | fixed: no ring in v1, the slot interface; command handshake ([engine.md](engine.md) §3, §6) |
| C1 #20 | stop(ABORT) → FLUSHED needs a pipeline reclaim | fixed: CAS reclaim of queued slots (`st20p_tx_reclaim_queued`, [implementation-plan.md](implementation-plan.md) §5.2); plugin conversion bounds the flush |
| C1 #21 | `mt_in_sch_thread` misses the RX packet lcore and TAP lcore | fixed: every library busy loop sets the flag ([engine.md](engine.md) §2.4) |
| C1 risk table | 8 risks (pipeline changes, completion latency, lost wake-up, waker cost, trylock skips, syscalls, EQ producers, destination update) | fixed: in [implementation-plan.md](implementation-plan.md) §9 |

### 6.2 C2 — timing and standards

| ID | Finding | Outcome |
|---|---|---|
| C2 #1 (blocker) | live capture drops every frame with the defaults; L ≥ 1 fails JT-NM windows; integer L cannot express camera phase; the SDI→IP gateway model is missing | fixed: `min_tx_delay_ns`, the wire-time slot rule, L reported; source kinds; `MTL_EVENT_TIMING_INFEASIBLE` ([timing.md](timing.md) §5.2; D-39); the live ANC window is decision M4 (D-67) |
| C2 #2 | the A/V example resolves the timeline anchor before the sessions exist, so T0 is in the past | fixed: `MTL_ANCHOR_AT_START`, T0 resolved at the first start ([timing.md](timing.md) §3.1, §3.3) |
| C2 #3 | the common-grid table is wrong for 24p, omits 44.1 kHz; mixed families give 20–40 s grids | fixed: corrected table, audio dropped above the 1 s cap and floor-aligned, horizon from max(now, start) ([timing.md](timing.md) §3.4; Q-TIME-19) |
| C2 #4 | unbounded SEND_LATE cascades, so the stream slides | fixed: bounded `MTL_LATE_SEND_LATE`, `WOULD_OVERLAP` ([timing.md](timing.md) §6.2; D-13) |
| C2 #5 | nearest snapping flips duplicates and gaps for jittery capture timestamps | fixed: NEAREST with hysteresis (TFRAME/8), `MTL_SNAP_LOCKED_PHASE` opt-in, duplicates as results ([timing.md](timing.md) §4.3; D-38, Q-TIME-25) |
| C2 #6 | the audio launch offset default (one ptime) breaks the JT-NM 1 ms limit | fixed: `D_a` clamp of the pacing profile's early error, capture uses `min_tx_delay` ([timing.md](timing.md) §5.3; Q-TIME-11) |
| C2 #7 | partial audio packets across submissions: timing, ownership, result and stop undefined | fixed: carry buffer (≤ 1152 B), straddling packet belongs to n+1, zero-pad on stop ([timing.md](timing.md) §8; D-41) |
| C2 #8 | SKIP underrun violates the ST 2110-40/-41 keep-alive; the example never submits ANC | fixed: `MTL_UNDERRUN_EMPTY_ANC`, keep-alive for fastmeta, silence option ([timing.md](timing.md) §6.3) |
| C2 #9 | ST 2110-22 packet count per frame is variable today | fixed: `MTL_CVIDEO_CBR` default with padding, oversize fails at submit ([timing.md](timing.md) §5.3; D-32) |
| C2 #10 | clock steps: TAI-anchored timelines drop forever; AUTO sends RTP backwards; `CLOCK_TAI` may be UTC; PHC reads are syscalls; per-port clocks | fixed: per-timeline step policy, AUTO never emits a smaller index, `CLOCK_TAI` validated, one published time base ([timing.md](timing.md) §2, §12; Q-TIME-2); the step default for created timelines is OI-31 |
| C2 #11 | RX media time is wrong for AES67 offsets ≠ 0 and `mediaclk:sender` | fixed: `rx.rtp_offset`, `rx.mediaclk`, `MTL_EVENT_RX_TIMEBASE_SUSPECT` ([timing.md](timing.md) §11.1–§11.2) |
| C2 #12 | DISCONTINUITY lets RTP jump backwards, which MTL's own RX drops | fixed: only forward while RUNNING ([timing.md](timing.md) §4.4) |
| C2 #13 | the oracle contract table is incomplete (13 differences listed) | fixed: method adopted, selection rules replaced ([timing.md](timing.md) §16.2) |
| C2 #14 | NOT_BEFORE contradicts itself with VRX pre-fill | fixed: "first slot whose first-packet wire time ≥ t" ([timing.md](timing.md) §5.4) |
| C2 #15 | no stated invariant that no packet leaves before its media time | fixed: T7 and the pre-fill cap ([timing.md](timing.md) §5.6; G-59) |
| C2 #16 | `scheduled_first` and "deadline" used in two senses; lateness is per packet | fixed: `scheduled_first = TVD − VRX0·TRS`, `margin_ns` vs `pickup_slack_ns`, `max_packet_lateness_ns` ([timing.md](timing.md) §5.1, §6.6) |
| C2 #17 | interlaced indexing: field rate, parity off the epoch, L in frames | fixed: index counts fields, interlaced members contribute TFRAME to the grid, L whole frames ([timing.md](timing.md) §3.6, §3.4) |
| C2 #18 | formula errors: audio RTP double count; off-grid T0; "TAI ns < 2^61" expires in 2043 | fixed: < 2^63 (2262) ([timing.md](timing.md) §3.5, §4.2) |
| C2 #19 | `media_mode` is both a session and a per-submission field | fixed: session property only; start arrays need INDEX or TAI members for sync (D-38, [timing.md](timing.md) §4.1) |
| C2 #20 | §4.1 and §8 disagree on audio gaps | fixed: a non-aligned forward gap needs DISCONTINUITY, carry padded, packets re-phased ([timing.md](timing.md) §4.4, §8) |
| C2 #21 | audio packet time must be S/Fs; legacy 80 µs is +4.17 % fast (SF-35) | open: OI-32 (reject, pace 4 samples at 4/Fs, or keep today) |
| C2 #22 | ANC details missing for interlaced and TEPO (F bits, TSST, raster, offset inheritance) | fixed ([timing.md](timing.md) §9.1; `anc.video` raster, RV-09) |
| C2 #23 | TSMODE must be declared, not inferred | fixed: `tx.tsmode` with TSDELAY ([timing.md](timing.md) §5.2) |
| C2 #24 | the 2022-7 class D default is too narrow; tolerance vs latency | fixed: tolerated PD reported, `rx.skew_budget_ns` class A (10 ms) ([timing.md](timing.md) §11.7; Q-TIME-14) |
| C2 #25 | RX flush deadline default unspecified | fixed: the due time follows the sender model ([timing.md](timing.md) §11.7) |
| C2 #26 | leap seconds and estimated clocks | fixed: offset from PTP `currentUtcOffset`, TIME_STEP at leap events ([timing.md](timing.md) §2.2) |
| C2 #27 | missing broadcast features: (a) timecode, (b) 1001 audio cadence, (c) TX ST 2110-21 self-check, (d) restart semantics, (e) preroll, (f) per-leg observed times, (g) RX link offset | fixed: (c) `tx.vrx_max`, `tx.cinst_max`; (d) SSRC kept ([timing.md](timing.md) §4.4); (e) `BEFORE_START`; (f) `observed_first_tai_ns[leg]`; (g) [timing.md](timing.md) §11.3; (a), (b) later (Q-TIME-20) |
| C2 #28 | G-19 misses the 2^32 wrap, 44.1 kHz with 1001 rates, ST41 rates, negative indices, large TAI | fixed ([timing.md](timing.md) §16.1 G-19) |
| C2 §A | arithmetic table (24p grid, 44.1 kHz grid, mixed families, 2^61, audio RTP notation, legacy 80 µs) | fixed, recomputed by R2 §6 |

### 6.3 C3 — usability by persona

Personas: P1 first sample, P2 FFmpeg and GStreamer plugin maintainer, P3 playout with A/V/ANC
and 2022-7, P4 low-latency live (line mode), P5 zero-copy GPU and MXL bridge, P6 Rivermax
migrant, P7 libfabric developer; B the Python and Rust binding authors ([concepts.md](concepts.md) §1.2).

| ID | Finding | Outcome |
|---|---|---|
| P1-1 (blocker) | the recommended completion default (ALL) stalls the minimal loop silently after `pool.count` frames | fixed: results off for library pools, always on for app memory (D-77); `status.blocked_on` (`MTL_BLOCKED_*`, [contract.md](contract.md) §5.5) |
| P1-2 | the "10-line loop" starts after the hardest part, the instance parameters | fixed: `mtl_instance_open(NULL, &mt)` reads `MTL_PORTS` (D-88, [contract.md](contract.md) §2.1); the instance call is in ex01 |
| P1-3 | a compound-literal flow erases every default (`ttl = 0` never leaves the host) | fixed: zero means the default in every field (`ttl` 0 = 64, DSCP 0 = the profile's, `ssrc` 0 = random), `mtl_flow_ipv4()` ([contract.md](contract.md) §3.2) |
| P1-4 | `struct_size` ceremony; a zero `struct_size` is silently accepted | fixed: `MTL_INIT(&s)` (D-74); `NULL` for "now" in `struct mtl_when` arguments |
| P1-5 | submit and dequeue are DP but convert pixels; the example renders one plane of three | fixed: DPC class; `app_format` 0 = no conversion ([contract.md](contract.md) R6, §3.3) |
| P1-6 | timeout units overflow `int`; negative timeout undefined | fixed: `MTL_MS()`, `MTL_SEC()`, `MTL_FOREVER` (R2); open: a timeout below −1 (OI-57) |
| P1-7 | the RX default shows the previous frame on loss | fixed: zero fill by default for library pools (D-37) |
| P1-8 | no minimal RX example, no "P1 surface" | fixed: ex02 and `mtl.h` alone ([examples.md](examples.md), [concepts.md](concepts.md) §4) |
| P2-1 (blocker) | cancel cannot implement GStreamer `unlock`/`unlock_stop`: edge-triggered races, sticky never clears, instance scope wrong | fixed: sticky per-session and per-instance interrupt, `mtl_session_interrupt(s, 0)` clears (D-30, [contract.md](contract.md) §7.4) |
| P2-2 (blocker) | an attached buffer cannot be submitted by name | fixed: `mtl_tx_acquire_slot` ([contract.md](contract.md) §9.5) |
| P2-3 | FFmpeg TX zero copy is impossible in v1 | fixed: stated; FFmpeg TX copies until `mtl_tx_acquire_layout` (Phase 4, `MTL_LATER`; [migration.md](migration.md) §11.4) |
| P2-4 | destroy returns `-EBUSY` while downstream holds RX buffers | fixed: deferred close, returns 1, `MTL_EVENT_SESSION_RETIRED` (D-36, [contract.md](contract.md) §4.9) |
| P2-5 | no fd signals "acquire would succeed"; eventfd draining unspecified | fixed: one wait handle per session with a target mask; a data call returning `-MTL_EAGAIN` arms; data calls drain ([contract.md](contract.md) §7.2–§7.3) |
| P2-6 | the instance singleton (boilerplate B5) is not absorbed; the instance EQ steals events | fixed: `MTL_INSTANCE_SHARED` (D-44), per-session events (D-79); differing shared opens fail, which OBS and FFmpeg trip (OI-43) |
| P2-7 | caps negotiation before create is partial | fixed: dry-run `mtl_session_query` with info and buffer requirements (D-19, RV-23) |
| P2-8 | framework timestamps hit `-EINVAL`; no flush; no clock conversion | fixed: `DUPLICATE_SLOT` results (D-38), `mtl_session_discard` (with rebase), `mtl_time_convert` (`mtl_sync.h`) |
| P2-9 | A/V in separate elements or muxers cannot share a timeline | fixed: named, refcounted timelines and the join rule ([timing.md](timing.md) §3.1, §7.2) |
| P2-10 | smaller: FFmpeg interrupt claim, thread-id assertion breaks a GstBufferPool export, export-pool drops, RX pool size, RX_FORMAT renegotiation, latency query | fixed: polled `AVIOInterruptCB`; overlap check (D-50); `mtl_tx_release` on drop; `MTL_UPDATE_POOL`; `MTL_EVENT_RX_FORMAT`; latency range in info ([migration.md](migration.md) §11–§12) |
| P3-1 (blocker) | the headline A/V example anchors its timeline in the past | fixed: `MTL_ANCHOR_AT_START` default (same fix as C2 #2) |
| P3-2 | ANC has no way to submit or receive its packet table | fixed: the table in the slot's meta area, TX and RX (D-86, [contract.md](contract.md) §9.9) |
| P3-3 | audio capacity and re-framing (B11) half absorbed; `valid_bytes` vs `sample_count`; tail at drain | fixed: `mtl_tx_write` for copy essences; `used` is the one size; carry zero-padded ([timing.md](timing.md) §8) |
| P3-4 | media mode in two places; the late-policy default depends on the per-unit one | fixed: session property only (D-38, D-13) |
| P3-5 | forgetting a `valid` bit silently sends AUTO timestamps | fixed: no `valid` bits on fields with their own mode; validity flags only on plain values (`MTL_UNITF_*`) |
| P3-6 | smaller: `leg_count` vs `flows[]`, preroll depth, playlist edits, `mtl_group_start` arity | fixed: a leg exists when its `udp_port` ≠ 0 (RV-25); 8-frame cap lifted (E11, D-57); `mtl_tx_withdraw`; group gone (D-78) |
| P4-1 | the progressive example never publishes FINAL (1080 is not a multiple of 64) | fixed: rows units end when `used` reaches the height (ex08) |
| P4-2 | progressive mode has no create-time switch | fixed: `sc.unit = MTL_UNIT_ROWS` |
| P4-3 | per-row deadlines are not exposed | fixed: `mtl_tx_row_deadline` (`mtl_sync.h`) |
| P4-4 | RX progressive has two wait mechanisms and no final state | fixed: `mtl_rx_wait_rows`; the unit's status at the end ([timing.md](timing.md) §6.7) |
| P4-5 | smaller: INDEX in the example, waker latency for line mode, a live-capture preset | fixed: source kinds (CAPTURE, GATEWAY) and W0 busy polling ([engine.md](engine.md) §7.2) |
| P5-1 (blocker) | RX cannot place unit k into grain k (MXL) | fixed: `MTL_SESSION_RX_BY_INDEX`, never writing a leased slot (RV-14) |
| P5-2 | shared memory treated like page-cache files | fixed: shmem, memfd, hugetlbfs direct; regular files copy only (D-16) |
| P5-3 | forwarding one buffer between two sessions undefined | fixed: `unit.hold` and `mtl_tx_send_slot` over the RX pool (D-55, [contract.md](contract.md) §9.6) |
| P5-4 | buffer views cannot express device memory | fixed in part: GPU-pinned host memory direct (D-16); device memory imports under `MTL_LATER` (`mtl_mem.h`, `device_handle`; Phase 4) |
| P6-1 | reading completions becomes mandatory; the Rivermax loop stalls | fixed: results optional for library pools (D-77); [migration.md](migration.md) §13 |
| P6-2 | `-EAGAIN` hides which resource ran out | fixed: `blocked_on` |
| P6-3 | familiarity claims need qualifiers (chunk vs publish, commit time 0, teardown, SDP) | fixed: [migration.md](migration.md) §13; SDP helper is `mtl_sdp.h` (Phase 7) |
| P7-1 | no `fi_getinfo`-style dry run | fixed: `mtl_session_query` |
| P7-2 | CQ entry format and size negotiation contradict | fixed then superseded: revision 3 used a 256-B union; revision 4 reads typed records with the caller's size (D-42, D-77) |
| P7-3 | timeout position, two ways to bind a CQ, `FI_THREAD_COMPLETION` wording, two cookies | fixed: timeout last (D-34); queues bound once (`mtl_queue.h`); one cookie per unit (`unit.cookie`) |
| B-1 | nested growable structs break the ABI rule and SWIG | fixed: fixed-size sub-structs with reserved tails, `struct_size` only at the top (R3) |
| B-2 | zero-initialised structs from bindings hit the silent `struct_size = 0` path | fixed: bindings call `mtl_struct_init` ([migration.md](migration.md) §10) |
| B-3 | untyped CQ reads, enum-typed fields, Rust and Python fit (GIL release) | fixed: typed reads, `uint32_t` enum fields (R3), [migration.md](migration.md) §10 |
| consistency table | the examples of document 10 used ≈ 12 undefined structs and mismatched signatures | fixed and superseded: the headers compile (D-45) |
| boilerplate table | B1–B14 against R08 §2 | fixed: B5 (shared instance), B11 (`mtl_tx_write`), B13 (deferred close), B14 (results off); B6 FourCC map is `mtl_format_names` (`mtl_format.h`) |

### 6.4 C4 — consistency, traceability and coverage

Every C4 item was applied to revision 2 (R2 §4, plus its applied pass). Most rows name
revision-1 shapes that revision 4 replaced; "superseded" says so and names what holds now.

| ID | Finding | Outcome |
|---|---|---|
| C-01 | session states differ across 01, 03 and 08 | fixed: `enum mtl_state`, nine states ([contract.md](contract.md) §4.1; D-08) |
| C-02 | late-policy name `MTL_LATE_SEND` vs `SEND_LATE` | fixed: `MTL_LATE_SEND_LATE` ([timing.md](timing.md) §6.2) |
| C-03 | skipped-slot field name | fixed: `slots_skipped_before` ([timing.md](timing.md) §6.6) |
| C-04 | EXACT non-compliance reported as LATE | fixed: ON_TIME with a non-compliant flag ([contract.md](contract.md) §6.3) |
| C-05 | ON_TIME defined against two reference points | fixed: admission verdict (Q-CMP-6) |
| C-06 | two different margins | fixed: `margin_ns` and `pickup_slack_ns` ([timing.md](timing.md) §6.6) |
| C-07 | source-release field names | superseded: early source release has no symbol (D-56, OI-20) |
| C-08 | RX overflow field spelled two ways | superseded: `MTL_SESSION_RX_LATEST` flag |
| C-09 | lease-state list drops DONE and RECEIVING | fixed ([engine.md](engine.md) §3.2) |
| C-10 | the update verb has two names | superseded: `mtl_session_update` (D-72) |
| C-11 | CQ binding call spelled two ways | superseded: queues (D-79) |
| C-12 | data-path signatures disagree | superseded: one unit struct, timeout last (D-75) |
| C-13 | `mtl_group_start` arity | superseded: start arrays (D-78) |
| C-14 | `mtl_session_destroy` arity | superseded: `mtl_session_close` (D-88) |
| C-15 | instance creation call differs | superseded: `mtl_instance_open` (D-44) |
| C-16 | region naming | fixed: `mtl_region_h` |
| C-17 | memory-domain names | fixed then deferred: device memory under `MTL_LATER` (Phase 4) |
| C-18 | progressive phase 5 vs 6 | fixed: rows units in the plan ([implementation-plan.md](implementation-plan.md) §6) |
| C-19 | handle bit layout exceeds 64 bits | fixed: `type:8 \| reserved:8 \| index:16 \| generation:32` (D-07) |
| C-20 | one type for buffers and leases | fixed: `mtl_lease_h` typed, slots by index (D-76) |
| C-21 | input flags are 32-bit in five places | fixed: `uint64_t` flags (R3) |
| C-22 | enum-typed fields in a public struct | fixed: `uint32_t` fields (R3) |
| C-23 | growable structs embedded mid-struct | fixed: fixed-size sub-structs (R3) |
| C-24 | out-structs without `struct_size`; `mtl_rx_timing` undefined | fixed: outputs take a size argument (R3, D-74); `mtl_rx_get_timing` (`mtl_observe.h`) |
| C-25 | size carried in two places, missing in one | fixed: R3 (inputs `struct_size`, outputs a size argument, arrays a record size) |
| C-26 | functions used in the design missing from the header sketch | fixed: the header set is normative (D-45) |
| C-27 | thread-safety table incomplete (R-THR-3) | fixed: every function carries its call class (R6, G-50) |
| C-28 | event table gaps; notices without getters | fixed: [contract.md](contract.md) §10.2, state events have getters |
| C-29 | RX missing-range map and meta area undefined | fixed: `mtl_rx_get_missing`, the meta area ([contract.md](contract.md) §9.9); data source open (OI-26) |
| C-30 | `mtl_session_config` lacks `next`, `unit` and timing fields | superseded: D-72 config; `next` reserved (D-33) |
| C-31 | zero used as a sentinel against R-TIME-1 | fixed: validity flags (R5) |
| C-32 | "exactly one result" vs completion modes | fixed: one outcome per accepted unit, a result or a counter when results are off ([contract.md](contract.md) §6.1–§6.2) |
| C-33 | `-ESTALE` vs `-EBADF` for a lease of another session | fixed: R4 (foreign `-MTL_EBADF`, returned `-MTL_ESTALE`) |
| C-34 | `-ESHUTDOWN` scope | fixed: [contract.md](contract.md) §8.1 |
| C-35 | Q-MIG-3 recommendation differs | fixed: D-27 |
| C-36 | migration required vs dropped | fixed then changed: migration is ported (D-98, Q-THR-6) |
| C-37 | who reserves NIC resources | fixed: create reserves, start attaches (Q-LIFE-9) |
| C-38 | the Q-CORE-1 index lists four choices, the body five | fixed: five ([decisions.md](decisions.md) §2.2) |
| C-39 | goal IDs collide with guarantee IDs | fixed: goals `GO-1…GO-9` ([concepts.md](concepts.md) §1.3) |
| C-40 | audio RTP formula double-counts | fixed ([timing.md](timing.md) §4.2) |
| C-41 | the progressive example never finishes | fixed (ex08) |
| C-42 | MXL POC size missing in the consumer table | fixed: the MXL POC uses MTL at 52 call sites in 8 files (research note 08 §5.1) |
| C-43 | CQ capacity ignores `SOURCE_RELEASED` entries | fixed in the R2 pass, then superseded: the results ring holds `pool_count` entries, reserved at acquire (RV-34); no early-release entry (D-56) |
| C-44 | wrong question reference for user-pacing snaps | fixed: Q-TIME-21 |
| C4 §1 note | no code for a wrong-direction verb | fixed: `-MTL_EINVAL` (G-46) |
| T-01 | `doc/user-pacing-timestamp-contract.md` is not in `545a266a` | fixed by statement: cited as "a separate draft, not part of this baseline" ([timing.md](timing.md) §16.2); the file is still untracked |
| T-02 | citations match the working tree, not `545a266a` | fixed: re-pinned (§6.6) |
| C4 §2.1 | 12 requirements without a guarantee (6 MUST) | fixed: requirement → guarantee map ([implementation-plan.md](implementation-plan.md) §8.0) |
| C4 §2.2 | proposed G-45…G-57 | fixed: G-45…G-57 in [implementation-plan.md](implementation-plan.md) §8 |
| C4 §2.3 | G-34, G-36, G-37, G-39 in no phase exit | fixed ([implementation-plan.md](implementation-plan.md) §8.2) |
| C4 §2.4 | seven B questions without a decision | fixed: D-28…D-34 (D-28, D-29 later superseded, §6.12) |
| C4 §2.5 | link every Q-ID to its anchor | rejected: too noisy; [questions.md](questions.md) now holds every Q-ID |
| C4 §3 | 11 research questions dropped | fixed: Q-CMP-6, Q-TIME-21…Q-TIME-23, Q-LIFE-10, Q-THR-9, Q-THR-10, Q-MEM-12, Q-MODE-8, Q-MODE-4 and Q-ABI-3 extended ([questions.md](questions.md)) |
| C4 §4 | modes silently missing: split-forward, 2-thread RX, ST40 split-by-packet, ST30 `BUILD_PACING`/`fifo_size`/RL warm-up, TSC_NARROW, `EXT_FRAME_MANUAL_RELEASE`, DEDICATE_QUEUE, NUMA, source ports, ST41 DIT/K, `rx_burst_size`, BE/PTP pacing | fixed: each has a field, flag or option ([migration.md](migration.md) §4, §4.16; `rx.threads` with OI-27) |
| C4 §5 | 36 claims spot-checked: 1 stale (SP-09), 8 working-tree lines; 45 of 72 citations into edited files differ | fixed: re-pinned (§6.6); SP-09 deleted and its reused ID renumbered SP-10 |
| C4 §6 | 5 lines over 400 characters, 2 tables with an extra column, under-indented research notes | fixed for the design documents; research notes declined (input, not published) |

### 6.5 C5 — adversarial user and feasibility review, and its response

C5 found the model right but the header uncompilable, RX designed by omission, the plan
≈ 50–75 EM with its payoff last, no NIC-less test substrate and no operator surface. The review
text is not in the repository. The response gave each of its 207 entries a verdict: 175 accepted,
31 accepted with a different remedy, one deferred (§3.4 moving cursor, Phase 4), one partly
rebutted (§6.14 b8). Its Part A cross-cutting rules and the places it disagreed with C5 come
first; then one row per C5 section number (several response bullets per number are merged). "r3"
names the revision-3 fix; the outcome names what holds now.

| Part A rule (revision 3) | Now |
|---|---|
| A1 the header is a real file and compiles | D-45; `sketch/check.sh` |
| A2 CQ records (256-B union, kinds ≤ 240 B, `int64_t` TAI ns + `time_valid`) | superseded: 96-B core result, full record by size (D-77), `MTL_TXR_*` flags |
| A3 array reads take the stride; `struct_size` input only | D-42; R3 |
| A4 handles: `mtl_lease_h` apart from buffers, 0 = null, typed | D-07; buffers became slots (D-76) |
| A5 error vocabulary and returns (`-MTL_ESHUTDOWN` only for app stop) | D-21, D-88 |
| A6 naming (`cvideo`, `fastmeta`, `mtl_tx_release`, `mtl_tx_withdraw`, `MTL_STOP_FLUSH`) | D-34 |
| A7 exported `*_init()` per struct, C-only value macros, zero-default rule | superseded by `MTL_INIT` (D-74) |
| A8 where a knob lives (cross-essence fields, media configs, `next` blocks, key-value options, flags) | superseded by D-73 |
| A9 call classes enforced in debug builds | D-05 |
| A10 waiters and multi-threaded use (one waiter set per target, MT_SUBMIT, SINGLE_READER) | D-50 |
| A11 the instance boundary first (versioned params, soname) | D-44, D-23, Q-ABI-2 |
| A12 the test substrate first (null backend, test clock, fault injection) | D-64 |
| A13 an L4 simple layer `mtl_simple.h` | superseded: D-65 → D-72 (`mtl_session_open`) → D-97 (typed) |
| A14 plan premises (Phase 0.5, engines first, ≈ 55–85 EM) | D-66, M10 |

| C5 § | C5 said | Revision 3 answer (evidence) |
|---|---|---|
| 5.3 | exact second-field RTP alternates +750/+751, so G-19 and G-22 cannot both pass | rebutted for broadcast rasters: with T0 on the frame grid the offset is constant (+1501 at 1080i59.94, +1800 at 1080i50, +1500 at 1080i60); +750/+751 only for a 119.88-field raster. G-22 rewritten as a rule ([timing.md](timing.md) §3.6) |
| 5.7 | RX `media_index = floor((media − T0)/period)` | off by one whenever the sender truncated a tick (half the frames at 59.94p): 2525 mismatches of 5050 against 0 for the exact inverse of `RTP = floor(M·R)` ([timing.md](timing.md) §11.2) |
| 5.8 | REANCHOR as the CAPTURE default | rejected: NEAREST for every source kind (§3.2) |
| 5.12 | CBR with a deferred drop is hostile; keep variable size as a mode | ST 2110-22 requires constant bytes and packets per frame: CBR default with a per-unit ceiling, padding and a synchronous error (D-32) |
| 5.15 | legacy ST40 next to unified ST20 disagrees by ≈ 55–58 ticks | the other way round: legacy ANC stamps its epoch (`st_tx_ancillary_session.c:420-427`); the outlier is legacy ST20's TX-cursor RTP, +54.4…+55.7 ticks (604–619 µs by VRX0) ([migration.md](migration.md) §6.4) |
| 4.10 | auto-select W2 below 1 ms | kept as the recommendation, left to M6 (§3.2) |
| 6.2 | a hold through `mtl_rx_hold`, or a transfer to an array of TX targets | the hold travels in the TX unit (`unit.hold`); a slot belongs to one pool (D-55) |
| 3.6 | auto-promote a cookie with results off, or warn | fail fast: `COOKIE_WITHOUT_RESULTS` (D-77) |
| 1.1 | 50–75 EM | accepted as order of magnitude; revision 3 adds scope: ≈ 55–85 EM, first user result in Phase 0.5 |
| 1.8, 6.14, 8.2 | several `path:line` citations | several did not match `545a266a` (the ALLOW_DOWN_PORTS prune is `st_tx_video_session.c:4000-4045`, not `:3935-3975`; `mt_eth_link_dump` is `mt_util.c:409-422`); the findings stand, the citations were corrected |

| ID | Finding | Outcome |
|---|---|---|
| C5 §1.1 | the plan is ≈ 50–75 EM | fixed: per-phase effort and FTE scenarios ([implementation-plan.md](implementation-plan.md) §10); velocity checked (610 commits in 2026, 133 `lib/`+`include/` commits in six months, one author 74 of them) |
| C5 §1.2 | the "475 functions" premise is dishonest without its breakdown | fixed: ≈ 154 colour conversion, 104 pipeline verbs, 75 `*FLAG*` defines; the case rests on the evidence table ([concepts.md](concepts.md) §1) |
| C5 §1.3 | the incremental alternative is not costed | fixed: §3.1 (≈ 6–9 EM, 4–6 shared); M1 option (c) |
| C5 §1.4 | NG1 ("legacy untouched") is dishonest | fixed: GO-9 engines first; bugfixes on for legacy, wire changes opt-in (D-24, M7); legacy gate every phase (G-99) |
| C5 §1.5 | the A/V answer comes too late | fixed: Phase 0.5 `st_timeline_*` (≈ 500 lines, 0 mismatches in 800 000 oracle cases), ST30P/ST40P flag fixes, SF-15 salvage ([timing.md](timing.md) §14.2; D-66) |
| C5 §1.6 | three passes over the same code | fixed: the L0 hooks are the slot interface, built once; Phase 6 re-base committed, go/no-go at the Phase 2 exit ([engine.md](engine.md) §3) |
| C5 §1.7 | 93 questions for the maintainer | fixed: ten maintainer decisions then (now M1–M17), proposed defaults elsewhere ([decisions.md](decisions.md)) |
| C5 §1.8 | testing realism | fixed: P/BE on every guarantee, U tier on the null backend, numeric budgets confirmed by S0, fault injection funded ([implementation-plan.md](implementation-plan.md) §8) |
| C5 §1.9 | no success criteria or user validation | fixed: success criteria per phase; external design review (FFmpeg and GStreamer owners, MXL team, an external engine team) as the Phase 0 exit (M10); persona P9 operator |
| C5 §1.10 | deliverables, deprecation and status missing | fixed: guide outline ([implementation-plan.md](implementation-plan.md) §6.1), deprecation ≥ two `vYY.MM` releases ([deployment.md](deployment.md) §7), the security document, side-finding status against PR #1770 |
| C5 §2.1 | CQ record shapes do not fit | fixed in r3 (256-B entries, int64 times); superseded by D-77 records |
| C5 §2.2 | the header does not compile and lacks 34 constants and 15 structs | fixed: D-45, `check.sh` |
| C5 §2.3 | init functions missing or inconsistent | fixed in r3 (37 `*_init()`); superseded by `MTL_INIT` |
| C5 §2.4 | zero-default rule broken (`numa`, `flow.port` 0 put both legs on one NIC, sentinels) | fixed: `flow.port` 0 = the leg's own instance port, else index + 1; zero defaults per field (R3) |
| C5 §2.5 | buffer and lease share one handle: misuse compiles | fixed: `mtl_lease_h` typed; slots by index (D-76) |
| C5 §2.6 | array reads rewrite the stride | fixed: caller's record size (D-42) |
| C5 §2.7 | implicit padding | fixed: named `reserved` fields, `-Wpadded -Werror`, a size check per struct |
| C5 §2.8 | no null handle or equality | fixed: `MTL_NULL`, `MTL_IS_NULL`, `MTL_SAME`; null timeline = the epoch |
| C5 §2.9 | `mtl_handle` in prototypes; no instance type; lease encoding caps pools | fixed: `mtl_instance_h`, `mtl_legacy.h` bridge, lease `session:16 \| slot:16 \| generation:32` |
| C5 §2.10 | trywait semantics and wait objects not portable | fixed in r3 (1 / 0 / < 0); superseded by arming on `-MTL_EAGAIN` and `mtl_session_get_wait_handle` (D-43, D-88) |
| C5 §2.11 | struct-size policy and version markers | fixed: the strict rule ([migration.md](migration.md) §7.3) |
| C5 §2.12 | `mtl_session_api_init` and global API version | fixed: version in the instance parameters, `MTL_API_VERSION` |
| C5 §2.13 | error constants and wait objects per OS | fixed: `MTL_E*` with Linux errno values, portable wait handle (D-21, D-43) |
| C5 §2.14 | where a knob lives; seven fields in the wrong struct | fixed in r3 (A8); superseded by D-73. C5's reading, still true: the name surface drops (five essences share one verb set), but the decisions per session roughly double as flag bits become typed fields, so usability rests on defaults; hence G-73 and the defaults checked in CI |
| C5 §2.15 | call classes are only comments | fixed: enforced in debug builds and by an interposer test (D-05, G-50) |
| C5 §2.16 | last-error contract | fixed: `mtl_last_error` with reason and detail (D-84 dropped `call_seq`) |
| C5 §2.17 | naming vocabulary inconsistent | fixed: D-34 |
| C5 §2.18 | no ABI node or soname plan | fixed: `libmtl_unified.so.0.<rev>`, node `MTL_UNIFIED_EXPERIMENTAL` (D-23) |
| C5 §2.19 | nullable outputs, cache by index | fixed: `MTL_NULLABLE`; unit fields |
| C5 §2.20 | stats struct versioning; legs arrays; UNSET values; read returns; pointer structs; `mtl_time` helpers; named rates; sentinels; bindgen | fixed: stats registry (D-80), `MTL_MAX_LEGS` 2, every status enum `*_UNSET = 0`, `MTL_FPS_*` (D-97), `MTL_ADDR`, `MTL_API` empty under SWIG and bindgen |
| C5 §3.1 | the first program needs a simple layer | fixed in r3 (`mtl_simple.h`); superseded by `mtl_session_open` (D-72, D-97) |
| C5 §3.2 | examples ignore returns and leak leases; dequeue before start looks like shutdown | fixed: examples check every return; `MTL_BLOCKED_APP_LEASES`; dequeue before RUNNING is `-MTL_EAGAIN` |
| C5 §3.3 | no null backend, test time or fault injection | fixed: `null:<n>`, `mtl_test_clock`, `mtl_debug_inject` (D-64) |
| C5 §3.4 | migration gaps (moving cursor, library-pool forward, `rtp_timestamp_delta_us`, VSYNC, `put_frame_abort`, `linesize`, `fifo_size`, field-rate `fps`) | fixed: field map ([migration.md](migration.md) §4); `media_time_offset_ns`; `MTL_EVENT_EPOCH_TICK`; deferred: moving cursor (Phase 4, `mtl_tx_acquire_layout`, OI-20) |
| C5 §3.5 | bindings need untyped reads and copy helpers | fixed: `mtl_unit_copy_in/out`, `MTL_ADDR`; a reference Python wrapper in Phase 1 |
| C5 §3.6 | waiters per target; a cookie with results off; `pool.count = 0` | fixed: counted waiters per target (W2 deviation D2), `COOKIE_WITHOUT_RESULTS`, pool defaults (video TX `max(min_count_direct, 3)`, RX 3, others 4) |
| C5 §3.7 | defaults unstated (PT, DSCP, late policy, queue, delays, snap) | fixed: header field comments; PT defaults verified (`st_pkt.h:15-18`, fastmeta 115), TTL/TOS (`st_tx_video_session.c:944-945`), SSRC (`:966`) ([contract.md](contract.md) §3) |
| C5 §4.1 | the busy-loop subset is unnamed | fixed: inline-safe subset, else `-MTL_EDEADLK` (D-47) |
| C5 §4.2 | commands can hang (four cases) | fixed: immediate vs boundary, 100 ms ack timeout → ERROR `CMD_TIMEOUT` (D-48, [engine.md](engine.md) §6) |
| C5 §4.3 | destroy on a stalled queue (no `rte_eth_dev_tx_queue_stop` in `lib/`) | fixed: bounded cleanup, worker queue restart or quarantine, port reset (D-49, [engine.md](engine.md) §9); spike S8 |
| C5 §4.4 | app threads touching tasklet mempools | fixed: frame pools keep the atomic release; no MP/MC mempool shared ([engine.md](engine.md) §2.6) |
| C5 §4.5 | the time base for tasklets | fixed: per-socket seqlocked record, refresh ≤ 100 ms, slewed ([engine.md](engine.md) §8); spike S7 |
| C5 §4.6 | shared-queue reads are O(members) | fixed: per-scheduler ready-summary words ([engine.md](engine.md) §5.6) |
| C5 §4.7 | false sharing in the lease entry | fixed: tasklet half and app half on separate lines ([engine.md](engine.md) §5.1) |
| C5 §4.8 | thread mode and W2 | fixed: W2 in thread mode, `rte_thread_register` (Q-THR-10); thread-mode schedulers are pinned since addendum K (RK-27) |
| C5 §4.9 | RX units complete only when full or evicted | fixed: RX due-time hook ([engine.md](engine.md) §7.4, [timing.md](timing.md) §11.7) |
| C5 §4.10 | auto W2 | modified: decision M6 (D-68) |
| C5 §4.11 | head-of-line order, completing contexts, `sh_info` in recovery, submission order, RX/ST22/ST30 hook gaps, handle tables, RMW count, unverified numbers | fixed: ordered results ([engine.md](engine.md) §5.4), recovery never touches `sh_info` (SF-41), grow-only tables (65 536 per type; at most 29 808 sessions), RMW elision only with SINGLE_READER, numbers labelled by spike |
| C5 §5 intro | past RTP is accepted after 20 redundant errors | fixed: per-unit relock ([timing.md](timing.md) §11.5) |
| C5 §5.1 | T0 for `AT_MEDIA_INDEX k` undefined | fixed: T0 formula on the grid for every k ([timing.md](timing.md) §3.3) |
| C5 §5.2 | preroll and the horizon | fixed: `preroll_ns`, horizon from max(now, start), `BEYOND_HORIZON`, `BEFORE_START` (D-52) |
| C5 §5.3 | second-field RTP | rebutted in part (above); one rule `floor(M(field)·90000)` |
| C5 §5.4 | framework sinks: lead and offset | fixed: `min_submit_lead_ns`, `media_time_offset_ns` (D-53; 1.279–1.294 ms worked at 1080p59.94) |
| C5 §5.5 | the processor recipe | fixed: ex10, TAI + a delay budget; the source kind is OI-33 |
| C5 §5.6 | cross-process timelines | fixed: epoch + `mtl_epoch_index_at` ([timing.md](timing.md) §10.4) |
| C5 §5.7 | RX timing designed by omission | fixed: [timing.md](timing.md) §11 (D-51) |
| C5 §5.8 | capture snapping | modified: NEAREST + LOCKED_PHASE (D-38) |
| C5 §5.9 | seeks | fixed: `mtl_session_discard` with rebase ([contract.md](contract.md) §4.6) |
| C5 §5.10 | groups: late join, offsets, restart | fixed then superseded: start arrays and the join rule (D-78, [timing.md](timing.md) §7) |
| C5 §5.11 | the L ≥ 2 rejection for PLAYBACK was unjustified | fixed: dropped; `link_offset_budget` warning ([timing.md](timing.md) §5.2) |
| C5 §5.12 | ST 2110-22 rate | modified: CBR default (above) |
| C5 §5.13 | fastmeta rate and RTP | fixed: R = 90000, rate from the video ([timing.md](timing.md) §9.3) |
| C5 §5.14 | audio gaps | fixed: silence fills, overlap trimmed, only DISCONTINUITY re-phases (D-41; G-86 restated in [timing.md](timing.md) §16.1, OI-35 closed) |
| C5 §5.15 | live ANC window; late policy by media mode; hint formula; negative indices; PsF; AUTO backward step; clocks; ST40 limits; mixed-API hazard | fixed: D-67 (decision M4), D-13, [timing.md](timing.md) §3.6, §6.5, §9.1, §12; hazard direction rebutted (above) |
| C5 §6.1 | stride-aware DIRECT is possible today (`st20_tx_ops.linesize`) | fixed: D-54 |
| C5 §6.2 | split-forward | modified: hold in the TX unit, one pool per slot (D-55) |
| C5 §6.3 | what v1 does for each producer | fixed: per-producer table (16 producers); now [migration.md](migration.md) §12.7 |
| C5 §6.4 | region budget (`MT_MAP_MAX_ITEMS = 256`, 128 memseg lists) | fixed: `REGION_BUDGET`, one region per arena ([contract.md](contract.md) §9.2) |
| C5 §6.5 | alignment of imports | fixed: page-aligned, never rounded outward (D-16) |
| C5 §6.6 | internal buffers of converting sessions invisible | fixed: reported; CONVERT + `MTL_SESSION_REQUIRE_DIRECT` fails (r3 `CONVERT_NOT_DIRECT`, now `DIRECT_IMPOSSIBLE`, [contract.md](contract.md) §9.7) |
| C5 §6.7 | TX views after submit | modified: MTL never writes a TX buffer; `mtl_tx_pin` keeps a slot out of acquire |
| C5 §6.8 | RX pool sizing for holds and skew | fixed: sizing rule; RECLAIM with BY_INDEX only on the target slot |
| C5 §6.9 | interlaced units | fixed: unit = field, woven frames as two field slots (D-54) |
| C5 §6.10 | access flags | fixed: checked at attach (`ACCESS_MISMATCH`); mappings are read-write in DPDK (OI-21) |
| C5 §6.11 | lazy mapping into devices | fixed: lazy or eager per named port (D-16) |
| C5 §6.12 | teardown order of imported memory | fixed: [contract.md](contract.md) §9.10 |
| C5 §6.13 | GStreamer two-phase pool recipe; early source release | fixed: [migration.md](migration.md) §12.4–§12.5; early release has no symbol yet (D-56, OI-20) |
| C5 §6.14 | b1 `min_count_direct` (`1 + ceil(nb_tx_desc / packets_per_unit)`); b2 the 8-frame cap (`st_tx_video_session.c:4073`); b3 meta area ≤ 1332 B; b4 path counters; b5 GPU-pinned host memory; b6 regular files copy only; b7 NUMA mismatch; b8 citation nits | fixed: [contract.md](contract.md) §9.4, D-57 (E11), D-16; b8 partly rebutted (`mt_main.c:864-865` was correct) |
| C5 §7.1 | instance lifetime and retirement events | fixed: the instance outlives closing sessions (D-44), `MTL_EVENT_SESSION_RETIRED` |
| C5 §7.2 | exported pools | fixed: `MTL_SESSION_EXPORT_POOL`; whether it implies MT_SUBMIT is OI-17 |
| C5 §7.3 | concurrency contract | fixed: D-50 |
| C5 §7.4 | renegotiation needs destroy and create | fixed: `mtl_session_update` with `MTL_UPDATE_MEDIA` or `MTL_UPDATE_POOL` (D-58) |
| C5 §7.5 | event fan-out to several readers | fixed in r3 (subscription masks, deviation D6); superseded by per-session events and queues (D-79) |
| C5 §7.6 | create waits for ARP (`st_tx_video_session.c:926`, 500 ms sleeps in `mt_arp.c:171-199`) | fixed: D-62 (`WAITING_NEIGHBOUR`) |
| C5 §7.7 | audio, fastmeta and ANC capacities | fixed: `unit_samples`, capacities in info ([contract.md](contract.md) §3.3; fastmeta item limit 2044 B) |
| C5 §7.8 | OBS per-source lcores and queues | fixed in r3 by the merge table; superseded by `-MTL_EEXIST`; open: OI-43 |
| C5 §7.9 | plugin details: ANC auto-detect, ST40 test knobs, `pts-pacing-offset`, ST22 `pack_type`, `CLOCK_TAI` without ptp4l, instance flags, `AVFMT_FLAG_NONBLOCK`, st30p muxer pts | fixed: [migration.md](migration.md) §4, §11; `MTL_TIME_SOURCE_SYSTEM_TAI` labelled estimated |
| C5 §7.10 | too many codes | modified: both pairs kept with one handling each; R2 later made `-MTL_EAGAIN` the one "nothing now" code (D-88) |
| C5 §8.1 | atomic flow change | fixed: `mtl_session_update` with `MTL_UPDATE_FLOWS` at a `struct mtl_when` (D-26, D-72) |
| C5 §8.2 | legs pruned when the link is down; no link monitor (link checked only at start, `dev/mt_dev.c:815-850`) | fixed: D-59, link monitor in Phase 2 |
| C5 §8.3 | ERROR semantics and reasons | fixed: [contract.md](contract.md) §4.8, `mtl_reasons.h` |
| C5 §8.4 | RX arm and start | fixed: join at first start, ARMED discards before t (D-51; deviation D10) |
| C5 §8.5 | instance parameters and runtime port open | fixed in r3 (merge table, deviation D8); superseded (D-44) |
| C5 §8.6 | MtlManager loss | fixed: D-63, D-92 |
| C5 §8.7 | identity, enumeration, exporter | fixed: names, `mtl_instance_list_sessions` (D-60; OI-11 for the generated name form) |
| C5 §8.8 | stats epochs are worse than cumulative counters | fixed: cumulative only (Q-OBS-1) |
| C5 §8.9 | the gauge identity G-43 was a tautology | fixed: entries/exits counters ([engine.md](engine.md) §2.8) |
| C5 §8.10 | the fault matrix is not injectable | fixed: `mtl_debug_inject` points, `nicctl.sh vf_link` / `vf_reset` ([implementation-plan.md](implementation-plan.md) §8.3) |
| C5 §8.11 | capacity and admission | fixed: `MTL_QUERY_CHECK_CAPACITY` (D-60) |
| C5 §8.12 | instance interrupt, linear histograms, test isolation, recovery coalescing | fixed: D-30, linear histogram keys (D-70), one long-lived null instance per U run |
| C5 §9 | cross-cutting: `-ESHUTDOWN` overloaded, one handle for base and lease, sketch does not compile, instance boundary, single-process timelines, strides, ST22, RX neglected, EQs, no NIC-less backend, recipes at INDEX, load-bearing defaults | fixed: as the rows above |
| C5 §10 | what the design gets right | kept |
| C5 §11.1–§11.10 | the ten changes (header first, RX chapter, re-sequence, grid arithmetic, recipes, stride-aware direct, error split, operator surface, test substrate, fewer decisions) | fixed: as the rows above; G-68…G-99 added to the guarantees |

### 6.6 R2 — verification of revision 2, and the citation re-pin

R2 checked every C1–C4 item: C1 21 of 22 addressed (C1 #14 partly), C2 25 of 29 (#9, #16, #19, #20
partly), C3 38 of 44, C4 76 of 85 (C-43 and the anchor links not addressed, the research-note
indentation declined). It found 55 new inconsistencies in revision 2 (items 5.1-1…5.6-6) and
nine risks. An editor pass applied everything except the items listed as skipped; the arithmetic
of every timing number was recomputed exactly and found correct, apart from two grid-rule
wordings (5.5-1, 5.5-2).

| ID | Finding | Outcome |
|---|---|---|
| 5.1-1…5.1-14 | header sketch vs documents: layout query, `mtl_port_open`/`find` arity, `mtl_tx_write` argument, ANC field order, three undefined structs, `mtl_session_api_init`, 32-bit masks, constants, init macro, `MTL_UNINIT_DESTROY_ALL`, `mtl_stat_get_u64`, reserved functions | fixed in revision 2; superseded: the header set is normative and compiled (D-45); the later list is `MTL_LATER` |
| 5.2-1…5.2-5 | examples: uninitialised `struct_size` outputs, forwarder RTP passthrough, AUTO session submitting an index, a non-compiling loop, late joiners on a named timeline | fixed; now compiled examples; the join rule is [timing.md](timing.md) §7.2 |
| 5.3-1…5.3-14 | codes, states and classes: `-EAGAIN`/`-EBUSY` rows, DESTROYING calls, bind/option states, typed reads without a size, eventfd drain vs "no syscall", CQ capacity, a field name clash, AS fan-out, `mtl_time_now`, `mtl_tx_write` WT, wait-fd CP, migration event, group states | fixed; now [contract.md](contract.md) R2, R6, §8 and OI-14 (the drain `read()` on a data call) |
| 5.4-1…5.4-12 | requirement and rule text (R-OBS-5, R-CMP-5, R-THR-3, R-CMP-6, default instance, `get_view` class, Rivermax statuses, truncated footnote, R-LIFE-5 exception, thread-safety rows, Q-CMP-5 and Q-THR-7 option text, the 00 claim "fixed") | fixed; requirements now in [requirements.md](requirements.md) and [implementation-plan.md](implementation-plan.md) §8.0 |
| 5.5-1…5.5-5 | timing text: grid rule vs table, 44.1 kHz fallback grid (1001/30000 s at 59.94p), implicit re-anchor knob, audio gap rule, group `AT_MEDIA_INDEX k` units | fixed ([timing.md](timing.md) §3.3–§3.4, §4.3, §8) |
| 5.6-1…5.6-6 | traceability: G-48, G-51 in no exit, G-57 split, G-61 phase, items with no phase, SP-09 reused | fixed ([implementation-plan.md](implementation-plan.md) §8.2; SP-10) |
| R-1 | the header sketch was not self-consistent | fixed: compiled headers; the runtime doc test on a fake engine became the null-backend example runs (G-97) |
| R-2 | typed result arrays without a caller-declared size | fixed: record size argument (D-42) |
| R-3 | named timelines with late joiners | fixed: the join rule; a joiner whose rate is not in the resolved grid is `GRID_MISMATCH` ([timing.md](timing.md) §7.2) |
| R-4 | group start for mixed-rate members | fixed: `AT_INDEX k` in each session's index period ([contract.md](contract.md) §4.4, RV-07) |
| R-5 | the DP syscall rule vs eventfd drains and AS fan-out | fixed: R2 allows the one drain read (OI-14 confirms the header); AS writes one eventfd, the waker fans out |
| R-6 | the error-code table did not describe every `-EAGAIN` and `-EBUSY` use | fixed: [contract.md](contract.md) §8.1, §8.4 (codes per call: OI-12) |
| R-7 | roadmap gaps | fixed ([implementation-plan.md](implementation-plan.md) §2.1) |
| R-8 | Q-TIME-18 (live ANC with L ≥ 1) on the critical path | open: decision M4 (D-67) |
| R-9 | the grid rule gives different T0 quanta | fixed: one rule ([timing.md](timing.md) §3.4; Q-TIME-19) |
| skipped | link Q-IDs to anchors; mark R05 F9 fixed in the research note; C1 #11 audio-recovery note; the "advanced" header split; R-1's runtime doc test; R-3's joiner rate | the anchors: rejected (noise); the research note: input, not edited (R05 F9 was fixed by #1713, `442847c0`, before the baseline); audio recovery: H4 in [engine.md](engine.md) §2.1; header split: done by D-71 |
| re-pin log | 13 citations moved from working-tree lines to `545a266a` (for example SF-07 `mt_main.c:864-865`, SF-11 `st_tx_video_session.c:2468-2474`, SP-01 `:3803`, `:3907-3914`, DD-05 `doc/design.md:390`) and 18 over-long lines | fixed; every citation in the maintained set is pinned to `545a266a` |
| re-pin log §3 | three content claims: DD-13d names a nonexistent `st30p_rx_get_queue_meta`; M6 claims IOVA is unchecked (only `buf_len` is, SF-17); "HW RX timestamps need a PF" (HEAD enables them on any port with the offload, iavf workaround `99b96c16`) | fixed: DD-13d, MF6/SF-17 and the timestamp rule in [engine.md](engine.md) §2.7, §11, §12 |

### 6.7 R4 header review (RV-01…RV-59)

The adversarial review of the revision-4 headers after the S7 samples audit: 1 Blocker, 15
Major, 24 Minor, 11 Nit open, plus 8 the S7 update had resolved (RV-52…RV-59). check.sh then ran
168 compiles; 113 functions + 4 later. Every Blocker and Major was fixed in the headers; the
outcome column says where.

| ID | Finding | Outcome |
|---|---|---|
| RV-01 (Blocker) | a failed re-submit of a rows unit freed a slot still being sent | fixed: once accepted, a failing submit ends the unit where its rows stopped, one result follows (`mtl_tx_submit`, `mtl.h`) |
| RV-02 | the meta-area header and the packet-table helpers disagree | fixed: frame units header first; packet units no header, the table at `meta` (`mtl.h`) |
| RV-03 | D-86's snapshot at submit missing (TOCTOU on ANC tables and lengths) | fixed: validated and copied at submit (`mtl.h`) |
| RV-04 | close covers app leases only; a closed handle cannot be waited on | fixed: close always consumes, 1 covers device references, a closed handle answers `MTL_WAIT_RETIRED`, results discarded and queues unbound (`mtl_session_close`, `mtl_session_wait`) |
| RV-05 | the new R2 not applied to reap, wait, `acquire_slot`, `send_slot`; ex04 leaves its loop | fixed: counts ≥ 1, `-MTL_EAGAIN` for "nothing" and "slot not free yet" (R2, `mtl_mem.h`, `mtl_util.h`) |
| RV-06 | "arms its wait target" has no lifetime, cost or owner | fixed: the arming protocol in R2 (wake request bit, one wait handle per session, data calls drain; busy loops never arm) |
| RV-07 | the start-array sentence is wrong for audio and mixed rates | fixed: index k at T0 + k × that session's index period; one timeline, one direction (`mtl_session_start`) |
| RV-08 | the audio media-index unit is ambiguous | fixed: index periods per essence, audio one sample (`mtl_sync.h`) |
| RV-09 | ANC and fastmeta rasters come from a start that may never happen | fixed: `anc.video` required on the epoch timeline (`FIELD_REQUIRED`) |
| RV-10 | `mtl_session_update` with a whole config: fields outside `parts` | fixed: they must equal the active configuration; `MTL_UPDATE_OPTIONS` removed |
| RV-11 | options parsed from a spec string have nowhere to live | fixed by `spec_options[16]`, then superseded: no spec strings (D-97) |
| RV-12 | option scope has four meanings | fixed: `enum mtl_option_scope` with `MTL_SCOPE_*` in the descriptor (`mtl_options.h`) |
| RV-13 | a pool attached over another session's pool can send memory being written | fixed: the hold must be a lease of the owner's same slot; the owner cannot change its pool while attached (`mtl_mem.h`) |
| RV-14 | `RX_BY_INDEX` can target a leased slot | fixed: dropped and counted, never written over; `RX_LATEST` ignored with it (`mtl.h`) |
| RV-15 | plugin formats share one number space | fixed: `MTL_PLUGIN_APP()`, `MTL_PLUGIN_CODESTREAM()` (`mtl_plugin.h`) |
| RV-16 | two callbacks contradict R6 | fixed: R6 names the dispatch and log-sink threads, what `fn` may call, `-MTL_EDEADLK` otherwise ([contract.md](contract.md) §10.4) |
| RV-17 | `*out` on failure defined for one call; ex01 closes an uninitialised instance | fixed: R4 null handle on failure, closes null-safe |
| RV-18 | output-size rule exceptions not listed; unknown bytes not zeroed | fixed: R3 (in/out structs use their `struct_size`; zeroed); `mtl_format_names` takes a size |
| RV-19 | `struct mtl_unit` in/out rules and templates | fixed: acquire and dequeue write up to `struct_size`, unchanged on failure; `how` contributes only per-use fields (`mtl.h`, `mtl_util.h`) |
| RV-20 | launch-time validity and flag combinations | fixed: NOT_BEFORE and EXACT exclude each other; UNIT_END only on packet units (`mtl.h`) |
| RV-21 | `used` changes meaning with plane shape | fixed: per unit kind (`unit.used`) |
| RV-22 | knobs that exist twice (`RX_NO_FILL`/`rx.fill`, `HW_TIMESTAMP`/`caps.hw_timestamps`, `UPDATE_OPTIONS`/`mtl_set_option`, `REQUIRE_DIRECT`/`caps.tx_copy`, `slot_bytes`/`max_udp_payload`) | fixed: `rx.fill` and `MTL_UPDATE_OPTIONS` gone; `caps.tx_copy` with REQUIRE_DIRECT is `-MTL_EINVAL`; `slot_bytes` 0 = `session.max_udp_payload`; `caps.hw_timestamps` needs the instance flag |
| RV-23 | two dry runs | fixed: requirements folded into `mtl_session_query` |
| RV-24 | shared-instance "merge" undefined; `MTL_PORTS` in set-uid processes | fixed: `-MTL_EEXIST` (`INSTANCE_MISMATCH`), ports required, `MTL_PORTS` ignored set-uid (`mtl_instance_open`) |
| RV-25 | `mtl_flow` addressing (IPv4 position, IPv6 value, defaults, leg existence) | fixed: IPv4 bytes 0–3, `ip_family` 6 = IPv6, prefix 64, a leg exists when `udp_port` ≠ 0 |
| RV-26 | zero as "default" blocks TROFFSET 0 and a zero added delay | fixed: an explicit 0 TROFFSET through `tx.troffset_ns`; `min_tx_delay_ns` 0 is the default for PLAYBACK and GATEWAY, CAPTURE is never 0 by design ([timing.md](timing.md) §5.2) |
| RV-27 | the spec-string grammar is ambiguous | superseded: no spec strings (D-97); `mtl_flow_ipv4()` |
| RV-28 | withdraw: result order and AUTO slots | fixed: published in submission order, later units keep their slots (`mtl_tx_withdraw`) |
| RV-29 | attach flags collide with access flags; `pool_count` vs attached counts | fixed: `MTL_ATTACH_*` from `0x100u`, count and detach rules (`mtl_mem.h`) |
| RV-30 | pointer lifetimes stated for two of eight pointers | fixed: R3 deep-copies every input pointer (plugin device excepted) |
| RV-31 | stats labels truncate; slots can move | fixed: `label[48]`, schema generation, `-MTL_ESTALE` on a stale bulk read (`mtl_observe.h`) |
| RV-32 | core fields point into optional headers | fixed: `enum mtl_flow_state` in `mtl.h` |
| RV-33 | packet RX dequeue: copy or lend, and its class | fixed: `MTL_PKT_RX_LEND`, copy by default (`mtl_packet.h`) |
| RV-34 | the results-ring invariant is unstated | fixed: `pool_count` entries, reserved at acquire (`mtl_tx_submit`) |
| RV-35 | no reason for units flushed by a drain timeout | fixed: `STOP_TIMEOUT` |
| RV-36 | `MTL_SAME` does not do what its comment says | fixed: comment ("warns in C, an error with `-Werror`; does not compile in C++"), check.sh probe |
| RV-37 | option descriptors miss defaults, enum names, "unset" | fixed: `enum_names`, `mtl_get_option_str` (`mtl_options.h`) |
| RV-38 | `mtl_last_error` cost on the polling path | fixed: `-MTL_EAGAIN` sets only code and reason (R2) |
| RV-39 | interlaced parity on RX without a valid index | fixed: `MTL_UNITF_SECOND_FIELD` |
| RV-40 | ex03 reaps once and may not re-arm | fixed: ex03 drains until `-MTL_EAGAIN` |
| RV-41 | R6's busy-loop rule has no subject | fixed: R6 names the library busy loops |
| RV-42 | "hold" names two things | fixed: `mtl_tx_pin`, `MTL_BLOCKED_APP_PINS` |
| RV-43 | one prefix, several value spaces | fixed: `MTL_UNITF_*`, `MTL_FLOWF_*`, `MTL_PKTE_*` |
| RV-44 | const-correctness of `mtl_pkt_tx_table` | fixed: takes `struct mtl_unit*` |
| RV-45 | static inline helpers invisible to bindgen | fixed: "bindings reimplement them" (`mtl_packet.h`) |
| RV-46 | pure parsers labelled CP | fixed (AS, RA-41), then superseded: the parsers were removed (D-97) |
| RV-47 | close, destroy and unload mean different things | fixed: `mtl_mem_close` retires at the last reference; one verb "close" |
| RV-48 | stale text after the S7 update | fixed |
| RV-49 | `compliance` magic numbers; `MTL_MS(1.5)` truncates; `mtl_mem_alloc`'s `void** va` not `MTL_ADDR` | fixed in part: `enum mtl_compliance`; open: the integer macros and `va` (OI-37) |
| RV-50 | concurrency flags default in opposite directions (`MT_SUBMIT`, `SINGLE_READER`) | open: OI-37 (recommendation: keep, documented) |
| RV-51 | check.sh lint gaps | fixed: layering lint, `-pedantic` for C++, `-D__bindgen` variant, the `MTL_SAME` probe |
| RV-52 (was Blocker) | `mtl_session_destroy` returned 0 with device references left | resolved by the S7 update: close's 0 means every reference is gone |
| RV-53 (was Major) | `MTL_DESTROY_FORCE` freed memory in use or meant nothing | resolved: no force flag |
| RV-54 (was Major) | ex03 armed a fixed mask and spun at 100 % | resolved: arming only on `-MTL_EAGAIN` |
| RV-55 (was Minor) | `mtl_session_get_wait_object` wrote an output without a size | resolved: `mtl_session_get_wait_handle(s, mask, intptr_t*)` |
| RV-56 (was Minor) | stop promised every result with no timeout outcome | resolved: `-MTL_ETIMEDOUT` then FLUSH, restartable |
| RV-57 (was Minor) | ex11 had to clear the interrupt before stop | resolved: interrupts cancel data waits only |
| RV-58 (was Minor) | examples released a lease after a failed submit | resolved: submit consumes the lease on failure except `-MTL_EAGAIN` |
| RV-59 (was Nit) | sketch/README described revision 3 | resolved |

### 6.8 RK — Kubernetes and crash safety

RK's verdict: the direction is right, four things would bite in a real pod (the example pod,
the shutdown order, implicit closing, R8's promises). Severity C Critical, H High, M Medium, L
Low. Every finding was fixed in the response; RV's verdict is noted where it was not VERIFIED,
and the outcome is the state of the headers and [deployment.md](deployment.md) on 2026-10-02.

| ID | Finding | Outcome |
|---|---|---|
| RK-1 (C) | non-root + `add: [IPC_LOCK]` leaves CapEff empty; `/dev/vfio/N` is root-owned: the pod spec cannot pin DMA memory | fixed: set-ups A–C (non-root on a prepared node, root in the container, privileged), `runAsUser`/`runAsGroup`, open names the missing piece ([deployment.md](deployment.md) §4.14–§4.15) |
| RK-2 (H) | the startup probe used readiness, so a grandmaster outage at start crash-loops the pod | fixed: startup uses the liveness rule ([deployment.md](deployment.md) §4.10) |
| RK-3 (H) | one SR-IOV resource for both 2022-7 legs gives no leg-to-PF guarantee | fixed: one resource per network (`PCIDEVICE_INTEL_COM_E810_RED`, `_BLUE`) ([deployment.md](deployment.md) §4.9, §4.15) |
| RK-4 (H) | MtlManager grants returned before schedulers and XSK sockets stop (K2 §5.1 double booking) | fixed: grants last ([deployment.md](deployment.md) §4.2 step 5; `mtl_instance_close`) |
| RK-5 (C) | implicitly closed objects must stay callable, but handle tables were per instance and freed | fixed: process-wide, never-freed handle slots, state CLOSED_BY_INSTANCE (R4, EK20, [engine.md](engine.md) §5.7) |
| RK-6 (H) | no step waits for threads inside DP calls | fixed: step 0 waits on the in-flight counters; SINGLE_READER sessions need the app to join first ([deployment.md](deployment.md) §4.2) |
| RK-7 (C) | `mtl_instance_abort` during shutdown races frees and eventfd closes (UAF, or 8 bytes into a recycled fd) | fixed: AS state in the never-freed slot with an AS in-flight counter ([deployment.md](deployment.md) §4.4) |
| RK-8 (H) | DPDK opens VFIO and memfd descriptors without CLOEXEC and swaps the SIGBUS handler | fixed: R8 names DPDK's SIGBUS handlers (heap growth and `instance.hotplug`, the RV regression R-4 closed); `FD_CLOEXEC` after EAL init and at create (EK15) |
| RK-9 (H) | CLOEXEC does nothing on fork; a non-exec child keeps VFIO fds, the manager socket and OFD locks | fixed: `pthread_atfork` child handler closes tracked descriptors; reason `FORKED` ([deployment.md](deployment.md) §4.4) |
| RK-10 (H) | `PORT_REMOVED` as liveness restarts a pod sending on its surviving leg | fixed: `SESSION_LOST` only with no leg left, else `DEGRADED` (RV regression in a table row, since corrected, [deployment.md](deployment.md) §4.5) |
| RK-11 (H) | `WORKER_STALLED` trips during a synchronous VF reset, every pod on the node at once | fixed: a bounded device step counts as progress |
| RK-12 (H) | memory under leases after close described four ways | fixed: freed after the last lease by the next CP call or at exit; `bytes_kept` ([deployment.md](deployment.md) §4.3) |
| RK-13 (H) | `threads_unjoined` with ports stopped fits no outcome | fixed: 1 covers held memory and threads stuck in application code |
| RK-14 (H) | closing the instance from a dispatch or log callback self-joins | fixed: `-MTL_EDEADLK`, reason `LIBRARY_THREAD`, mt not consumed (the RV regression R-3 closed) |
| RK-15 (H) | results flushed at step 1 are dropped when the dispatch thread is joined | fixed: step 3 delivers pending results, `results_discarded` (EK21) |
| RK-16 (H) | re-open in the same process after close undefined | fixed: re-open rules in `mtl_instance_open` (quarantined ports `-MTL_EBUSY`, EAL arguments fixed at first open) |
| RK-17 (M) | a reset started just before the deadline overruns | fixed: a reset starts only if `caps.reset_budget_ns` remains |
| RK-18 (M) | the budget counted from the call, not from SIGTERM | fixed: the handler records the time (`clock_gettime` is AS); ex11. Open nit (RV): when `shutdown_all` runs without a prior signal (a normal exit), `g_term` is zero, `spent` is the monotonic uptime and the budget collapses to the 100 ms floor; ex11 should take `g_term` at the call when no signal came |
| RK-19 (M) | DRAIN can use the whole deadline | fixed: drain stops at the deadline minus the later steps |
| RK-20 (M) | a rows unit with `tx.rows_late` STALL waits for rows that never come | fixed: shutdown and FLUSH treat STALL as TRUNCATE |
| RK-21 (M) | `-MTL_ETIMEDOUT` meant both "drain incomplete, safe" and "not quiesced, exit" | fixed: `-MTL_EIO` with `QUEUE_QUARANTINED`; the error comments were updated after RV (R-3) |
| RK-22 (M) | shared handles are values: a double close drops another component's reference | fixed: one reference handle per shared open; `-MTL_ESHUTDOWN` after `MTL_SHUTDOWN_ALL_REFERENCES` ([contract.md](contract.md) §2.3) |
| RK-23 (M) | the probe thread reads health while the main thread consumes mt | fixed: health returns `-MTL_ESHUTDOWN` after close; ex11 maps it (liveness 200, readiness 503) |
| RK-24 (M) | stopped schedulers look stalled; 100 ms equals the CFS period | fixed: `SCHED_STALLED` masked during close, `instance.stall_ns` 1 s, events at 1/8, 1/4, 1/2 (the RV regression R-2 in `mtl_queue.h` closed) |
| RK-25 (M) | "a CFS quota is present" is true for every Guaranteed pod with the gate off | fixed: flagged only when quota ÷ period < the CPU count (EK17) |
| RK-26 (M) | housekeeping CPU vs EAL main lcore; three overlapping knobs | fixed: `instance.housekeeping_cpus` removed, `instance.main_lcore` is the housekeeping CPU, `instance.sys_lcore` "never main_lcore" (RV INCOMPLETE, closed by that comment) |
| RK-27 (M) | 04 said thread-mode schedulers are unpinned to justify W2; 16 pins them | fixed ([engine.md](engine.md) §2.5) |
| RK-28 (M) | `MTL_INSTANCE_MANAGER_OPTIONAL` implies a required manager; auto arbitration picks the manager whenever its socket exists | fixed in part: the flag removed (D-92). **Open**: `instance.cpu_arbitration` auto still picks MtlManager when its socket is present ([deployment.md](deployment.md) §4.7), so a pod mounting it only for AF_XDP gets manager CPU arbitration inside an exclusive cpuset, which K-REQ-8 forbids (RV: REGRESSION). Open: auto = none in an exclusive cpuset with the manager granting only AF_XDP queues, or keep the header and document `MTL_CPUARB_NONE` for pods (OI-48) |
| RK-29 (M) | `port.xsk_map` without the manager needs an engine change | fixed: EK19 ([engine.md](engine.md) §11); who sets the queue rate is OI-23 |
| RK-30 (M) | PHC agreeing with `CLOCK_TAI` cannot tell lock from holdover | fixed: detect is a start condition; `mtl_time_set_reference` reports grandmaster loss ([deployment.md](deployment.md) §4.8) |
| RK-31 (M) | `network-status` may be absent or stale at start | fixed: the helper under `MTL_LATER` with `-MTL_EAGAIN`; `env:VAR#n=ip/prefix` is the v1 form |
| RK-32 (M) | `MTL_INSTANCE_PREFLIGHT` checks an init container's limits, not the app's | fixed: removed ([deployment.md](deployment.md) §4.9; the RV leftover R-5 closed) |
| RK-33 (M) | handlers installed after open; as PID 1 a SIGTERM during open is discarded | fixed: block the signals before open (ex11) |
| RK-34 (M) | rows missing from the crash table (timelines, plugins, queues); `tx_maxrate` residual | fixed ([deployment.md](deployment.md) §4.5) |
| RK-35 (M) | a RollingUpdate runs two senders on one group | fixed: `strategy: Recreate` ([deployment.md](deployment.md) §4.15) |
| RK-36 (L) | AF_XDP `NET_RAW` for non-root; `SYS_NICE` gates NUMA calls under RuntimeDefault | fixed ([deployment.md](deployment.md) §4.14) |
| RK-37 (L) | health `detail[96]` cannot be atomics only | fixed: a consistent snapshot, detail "" if it changed |
| RK-38 (L) | key 2028 is not a pod key | fixed: `instance.profile` is 2040 |
| RK-39 (L) | `mac` is an output field in an input struct | fixed: ignored on input |
| RK-40 (L) | close with timeout 0 and `MTL_FOREVER` undefined | fixed: 0 = abort semantics now; FOREVER bounded per step |
| RK-41 (L) | one-argument close in the tour; legacy close vs implicit session close | fixed: `mtl_legacy.h` close semantics |
| RK-42 (L) | `port.igmp_report_ms` 1000 ms changes today's 10 s | fixed: 10 s until a query is seen |
| RK-43 (L) | memlock wording in user namespaces; `dma_entry_limit` | fixed ([deployment.md](deployment.md) §4.11) |
| K-REQ coverage | RK rated K-REQ-1–4, 7–12, 15, 17–19 partial and K-REQ-16 not met | fixed by the rows above; dispositions per K-REQ in [requirements.md](requirements.md) |
| E-5, E-6, E-15, E-16, E-19 (RK's names) | the Kubernetes engine items as first numbered in the Kubernetes design document 16 (E-5 MtlManager, E-6 SysV and `kill(pid, 0)` removed, E-15 DPDK descriptors, E-16 reconcile at open, E-19 XSK map without the manager) | renumbered EK5, EK6, EK15, EK16, EK19 ([engine.md](engine.md) §11) |
| F-6 (K3) | K3's manager-less libxdp load is unreachable at HEAD: native AF_XDP needs MtlManager (`dev/mt_af_xdp.c:730-733`) | fixed: EK19; K2 is right where K2 and K3 disagree |

### 6.9 RN — NMOS and IPMX

RN's verdict: the shape is right, but an update completed only when media flowed, the header
contradicted 09 on neighbour resolution, what an activation carries besides flows was not
atomic, and the example taught the wrong pattern. Most of what RN covers is Phase 7 (D-98).

| ID | Finding | Outcome |
|---|---|---|
| RN-1 (C) | TX switches on the first unit picked up; an idle or muted sender never reaches APPLIED | fixed: the switch at the slot boundary by the clock, unit or not; RX by media time or local arrival ([nmos-ipmx.md](nmos-ipmx.md) §4.1) |
| RN-2 (H) | "neighbours resolved before commit" fails an activation to 192.0.2.1 (the nmos-testing default) | fixed: resolved after commit, not awaited |
| RN-3 (H) | the update ignores options (`rx.mediaclk`, `rx.rtp_offset`, `rx.link_offset_ns`, `rtcp.*`) | fixed: R keys in the update's config apply at its boundary (§6 choices) |
| RN-4 (H) | ex13 issues one PATCH as two updates; the second replaces the first | fixed: one update with `FLOWS \| LEGS \| REAPPLY`; `applied_at` by `update_seq` (ex13) |
| RN-5 (H) | PEP parameters read at create only | fixed: `crypto.*` options, R keys (Phase 7) |
| RN-6 (H) | the counter restarts at 0 on every `set_key` (IV and counter reuse) | fixed: restarts only for unused key bytes (`mtl_crypto_set_key`) |
| RN-7 (H) | RX ignores `when`; unknown key versions are silent | fixed: RX keys honour `when`; `MTL_EVENT_KEY_NEEDED`, `rx.crypto_unknown_key` |
| RN-8 (H) | the inline-processor recipe computes launch on the upstream clock | fixed: `MTL_SUBMIT_SENDER_TIME` |
| RN-9 (M) | status keeps only the last call; no seq returned; DRY_RUN and CANCEL effects | fixed in part: DRY_RUN leaves the status untouched; cancel 0/1/`-MTL_EBUSY`. Open (declined): the call returns no seq, the Node serialises updates ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-10 (M) | `subscription.active` derived from MUTED conflates master_enable with rtp_enabled | fixed: the Node keeps `master_enable`; MUTED is a transport state |
| RN-11 (M) | IS-05 returns 423 while a scheduled activation is pending; REPLACED is not an IS-05 path | fixed ([nmos-ipmx.md](nmos-ipmx.md) §4.2) |
| RN-12 (M) | RX REAPPLY did leave + join on both legs at once | fixed: re-sends the report without a leave |
| RN-13 (M) | 09 and 07 not marked superseded | fixed then superseded: those documents are history; [nmos-ipmx.md](nmos-ipmx.md) is maintained |
| RN-14 (M) | "colorimetry 0: the SDP omits it" violates ST 2110-20 §7.2 | fixed: UNSPECIFIED is rendered (`mtl_format.h`) |
| RN-15 (M) | colorimetry, TCS and range change only when STOPPED | fixed: under MEDIA at the boundary when no conversion uses them; open: PAR stays `fmtp_extra` text ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-16 (M) | `MTL_SDP_ONE_MLINE` renders `ssrc-group:DUP` | fixed: removed; always two m-lines under `a=group:DUP` |
| RN-17 (M) | render cannot write privacy, measured values, InfoFrame, IPMX TP | fixed in part; open: the privacy triple, `a=infoframe`, measured values as text ([nmos-ipmx.md](nmos-ipmx.md) §16, Phase 7) |
| RN-18 (M) | parse outputs no IPMX keyword, ts-refclk, RTCP port, privacy | fixed in part (IPMX flag, ts-refclk, RTCP port, crypto options); open: the privacy triple has no destination ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-19 (M) | a one-leg SDP leaves a stale `flows[1]` enabled | fixed: parse reserves the legs it did not find |
| RN-20 (M) | only video and audio sender-report schedules; ANC missing; audio N rule | fixed in part (per-essence schedule, ANC included); open: the NTP of a mid-unit audio report for SENDER audio ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-21 (M) | one MIB builder cannot build 0x0008, 0x0003, 0x0011, 0x0006 | fixed in part (the library builds 0x0001, 0x0002, 0x0004, 0x0011; the rest application bytes); open: no option carries privacy_version ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-22 (M) | a per-unit MIB replaces all MIBs and bumps the version every frame | fixed: per-unit blocks append, never bump |
| RN-23 (M) | `MTL_SENDER_IPMX` has no TP value; `vtotal` 0 undefined off BT rasters | fixed: dropped (§6 choices); `video.vtotal` default as proposed |
| RN-24 (M) | SENDER mode leaves k undefined; DISCONTINUITY; in-flight units across a step | fixed in part (the k rule, re-anchor); open: in-flight units across an AUTO time step ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-25 (M) | RX AT_TAI by RTP-derived media time means nothing without a shared clock | fixed: local arrival time for SENDER or unlocked clocks |
| RN-26 (M) | `tx.precede` orders enqueue only; InfoFrame RTP; two non-atomic updates | fixed in part (`tx.precede` C, shared queue, the video's RTP and M); open: two updates with one `when`, and no SENDER video target ([nmos-ipmx.md](nmos-ipmx.md) §16, R-1) |
| RN-27 (M) | FREERUN drifts from the controller clocks | fixed: `time.freerun_slew_ppm` |
| RN-28 (L) | `-MTL_EAGAIN` from CP calls (cancel, render) | fixed: render `-MTL_EBUSY`; cancel 0/1/`-MTL_EBUSY` |
| RN-29 (L) | `ts_refclk[96]`, `mediaclk[32]` vs 64- and 12-byte wire fields | fixed (`ts_refclk[64]`, `mediaclk[16]`); open nit: no room for the NUL in 64 ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-30 (L) | the IPMX profile table is incomplete (DSCP of InfoFrame, FEC, HDCP, PTP EF; DHCP; source filter) | fixed in part (`MTL_FLOWF_DSCP_LITERAL`, checks); open: EF for PTP, DHCP DNS and domain ([nmos-ipmx.md](nmos-ipmx.md) §16) |
| RN-31 (L) | `MTL_WAIT_RTCP` in an optional header; FREERUN name; key 2028; function count | fixed; the state name FREERUN kept with a reason |
| RN-32 (L) | muted units counted as dropped; reports and render while muted | fixed: `tx.units_muted`, no reports while muted, render writes the legs as configured; open: the muted unit's result status (OI-56) |
| RN-33 (L) | `planned_tai_ns` in CREATED/STOPPED; pending AT_TAI across a time step; RTCP keys | fixed: planned = applied = now; pending updates fail with `TIME_STEP` |
| RN-34 (L) | lean trims: `clear_keys`, `mib_build`, RFC 3550 mode, `sr_interval_ms` | fixed; `mtl_rtcp_mib_next` stays in `mtl_rtcp.h` (declined, beside the format it parses) |

### 6.10 RA — API coherence and leanness of the addenda

RA ran when check.sh reported 198 compile runs, 139 functions (`mtl.h` 33) and 6 under
`MTL_LATER`: one Blocker, 13 Major, 21 Minor, 6 Nit. Its biggest simplification (PEP parameters
as options, deleting the extension chain) was taken. RA gave each Blocker and Major an exact header edit
E1–E29, named after its finding (E1 RA-1, E2 RA-2, E3 RA-3/RA-16/RA-31, E4 RA-4, E5 RA-5/RA-35,
E6 RA-6, E7 RA-7, E8 RA-8, E9 RA-9, E10 RA-10/RA-24, E12 RA-12, E13 RA-13, E14 RA-14, E15 RA-15,
E23 RA-23, E28 RA-28, E29 RA-29); the outcome column says what of each edit is in the headers.
These E-numbers are unrelated to the engine changes E1–E13 of [engine.md](engine.md) §11.

| ID | Finding | Outcome |
|---|---|---|
| RA-1 (Blocker) | `enum mtl_rtcp_sr` and `struct mtl_rtcp_sr` share a tag; two headers cannot be included together | fixed: `rtcp.sr` is a bool, `struct mtl_rtcp_report`, `MTL_RTCP_RPT_*`; check.sh compiles every header together in both orders and as C++ |
| RA-2 | under IPMX, 0 means the profile's default, so `MTL_SENDER_N` and DSCP CS0 cannot be requested | fixed: `dscp` 0 = the profile's, `MTL_FLOWF_DSCP_LITERAL`; `MTL_SENDER_N` = 0 means N under both profiles |
| RA-3 | CANCEL returns `-MTL_EAGAIN` for "none pending" and "too late" | fixed: `parts` 0 cancels: 0 cancelled, 1 none pending, `-MTL_EBUSY` (`UPDATE_COMMITTING`) |
| RA-4 | shared instance: does each open get its own handle? | fixed: one reference handle per open |
| RA-5 | memory under leases after instance close described four ways | fixed: freed after the last lease (as RK-12); R4 extended |
| RA-6 | reserved legs let LEGS add a leg while RUNNING; mute pacing undefined | fixed: leg rules in `mtl.h` (legs fixed at create, a reserved leg is all zero with its bit set, bits ≥ `MTL_MAX_LEGS` rejected; muted units retire at their slot) |
| RA-7 | `rx.rtp_offset`/`rx.mediaclk` cannot change at the update's boundary | fixed: R keys in the config apply at the boundary |
| RA-8 | `mtl_sdp_parse` returns unterminated strings | fixed: fixed arrays, copied and terminated (`mtl_sdp_meta`, Phase 7) |
| RA-9 | the crypto extension block cannot be read back | fixed: PEP parameters are `crypto.*` options; `mtl_ext_hdr`, `mtl_ext_kind` removed, `next` reserved again (D-33) |
| RA-10 | readiness never becomes true without PTP; one bad stream takes the Node out | fixed: FREERUN and HOLDOVER are ready; `DEGRADED` is information only ([deployment.md](deployment.md) §4.10) |
| RA-11 | ex13 has three bugs (flow parse wipes ssrc/dscp, two updates, `applied_at` never ends) | fixed: ex13 rewritten |
| RA-12 | FLUSH's "partly sent is finished" and abort's cut unit have no status | fixed: FLUSH sends the started unit to its end with its normal status; abort FLUSHED (`ABORTED`) with `MTL_TXR_PKT_SHORT` |
| RA-13 | close outcomes incomplete (stuck threads, library-thread callers, re-open, other objects) | fixed: the close comment ([contract.md](contract.md) §2.4) |
| RA-14 | colorimetry 0 renders a non-compliant SDP | fixed (as RN-14); FULLPROTECT with BT2100 `-MTL_EINVAL` |
| RA-15 | `MTL_SHUTDOWN_*` flags and outcomes share a prefix and overlap | fixed: `enum mtl_shutdown_outcome` removed, `references_left` |
| RA-16 (Nit) | parts, modifiers and states share `MTL_UPDATE_` | fixed: `MTL_UPDATE_CANCEL` removed; the RV leftover R-7 (plan rows) closed |
| RA-17 | `-MTL_EAGAIN` outside R2 (render, `mtl_index_at`) | fixed: `-MTL_EBUSY` |
| RA-18 | `mtl_port_get_spec` has no size; `mac` on input | fixed: size argument; `mac` ignored on input |
| RA-19 | option key ranges out of their groups | fixed: RTCP, retransmission, encryption and profile keys in 1000–1039; `instance.profile` 2040 |
| RA-20 | `MTL_WAIT_RTCP` outside the core wait list | fixed: in `mtl.h` |
| RA-21 | a per-frame MIB bumps the version every frame | fixed (as RN-22) |
| RA-22 | "one record per kind" forbids two tagged USER records | fixed: one per (kind, tag) |
| RA-23 | SENDER mode: k, indices, AT_INDEX, `mtl_index_at`, `tx.precede` undefined | fixed: the SENDER rules in `mtl.h` (RV INCOMPLETE on `mtl_index_at` and result indices, closed since: `-MTL_EINVAL`, `media_index` = k) |
| RA-24 | FREERUN is a state, a source and a fallback; `MTL_EVENT_TIME_SOURCE` means "grandmaster changed" | fixed: `MTL_EVENT_GRANDMASTER`; the time-state rule in `mtl.h` |
| RA-25 | ex11: report uninitialised, negative budget, racy signal count, flags 0 on a shared instance | fixed (ex11) |
| RA-26 | `mtl_port_specs_from_network_status` does file I/O and JSON | fixed: under `MTL_LATER` |
| RA-27 | `mtl_audio_remap` is a new capability with the wrong prefix | fixed: under `MTL_LATER` |
| RA-28 | `mtl_rtcp_mib_build` rebuilds what the session holds | fixed: removed; the library builds the essence MIB |
| RA-29 | `vtotal`, `htotal`, `max_bitrate_bps` are tuning with derived defaults | fixed: options `video.vtotal`, `video.htotal`, `cvideo.max_bitrate_bps` |
| RA-30 | status 112 B vs D-80's 80 B; `update_planned_tai_ns` repeats the call's output | fixed: removed, status 104 B |
| RA-31 | `planned_tai_ns` in CREATED/STOPPED/ARMED; DRY_RUN; what `update_seq` counts | fixed (RV INCOMPLETE on ARMED, closed since: a `when` before T0 means T0) |
| RA-32 | `env:` ports read the environment in a set-uid process | fixed: `-MTL_EINVAL` there |
| RA-33 | no translation unit includes every header | fixed: the all-headers probe |
| RA-34 | instance close flushes, session close drains: closing only the instance loses queued units | fixed: stated in the close comment |
| RA-35 | "their close then returns 0" contradicts R4 | fixed: R4 extended |
| RA-36 | health as DP needs type-stable memory and a consistent copy | fixed: snapshot statement |
| RA-37 (Nit) | a comment line without its asterisk | fixed |
| RA-38 (Nit) | ex01 counts frames on `-MTL_EAGAIN`; label | fixed |
| RA-39 (Nit) | crypto mode values lack the prefix; `uint8_t` fields | fixed: `MTL_CRYPTO_AES*`; the `uint8_t` fields explained |
| RA-40 (Nit) | "budget 18" vs "the 17th group fails" | fixed: "the first group past the budget" |
| RA-41 (Nit) | pure parsers have mixed classes | fixed (AS), then superseded: the parsers were removed (D-97) |

### 6.11 RV — verification of the RKNA response

RV checked every RK, RN and RA ID against the headers: 96 VERIFIED, 13 INCOMPLETE, 0 MISSING, 9
REGRESSION; check.sh 201 compile runs, 135 functions + 8 under `MTL_LATER` (CP 58, DP 19, DPC 7,
WT 14, AS 37), 17 headers. The per-ID verdicts are folded into §6.8–§6.10. Its own findings:

| ID | Finding | Outcome |
|---|---|---|
| R-1 | a SENDER session cannot be a `tx.precede` target, so an async HDMI source cannot carry IPMX InfoFrames | open: [nmos-ipmx.md](nmos-ipmx.md) §16 (Phase 7) |
| R-2 | `MTL_EVENT_SCHED_STALLED` at "1x, 2x, 4x" against "1/8, 1/4, 1/2" | fixed (`mtl_queue.h`) |
| R-3 | `MTL_ETIMEDOUT` "(stop, close)", `MTL_EIO` "session failed", `MTL_EDEADLK` "busy-loop thread" no longer matched the close contract | fixed: the three comments and R2/R6 reworded; reason `LIBRARY_THREAD` |
| R-4 | "the one exception" to "no signal handler" vs `instance.hotplug` | fixed: R8 names both DPDK handlers |
| R-5 | `PREFLIGHT` still named after its removal | fixed |
| R-6 | `MTL_INSTANCE_MANAGER_OPTIONAL` still used as live API in older documents | fixed: those documents are history; the maintained set has no live use |
| R-7 | `MTL_SENDER_IPMX` and `CANCEL` still listed as roadmap deliverables | fixed ([implementation-plan.md](implementation-plan.md) §6) |
| R-8 | "health `PORT_REMOVED`" is not a health bit | fixed ([deployment.md](deployment.md) §4.5) |
| R-9 | a muted session renders no m-line | fixed: render writes the legs as configured (`mtl_sdp.h`) |
| R-10 | the header table of the examples page was stale | fixed: [examples.md](examples.md) is generated from the headers |
| R-11 | "118 issues, three of them critical" | fixed: four Critical (RK-1, RK-5, RK-7, RN-1) and one Blocker (RA-1) (§1) |
| R-12 | EK19–EK21 not tracked in the plan | fixed ([implementation-plan.md](implementation-plan.md) §6, [engine.md](engine.md) §11) |

### 6.12 Superseded decisions

[decisions.md](decisions.md) §3.3 lists which decision replaced which; the replaced text was:

| ID | Original decision | Superseded by, and why |
|---|---|---|
| D-12 | timelines with lazy anchors (`AT_START` default), names, the epoch timeline as a real handle, a capped common grid (interlaced members contribute TFRAME), T0 = `ceil((now + lead + preroll − k·P)/G)·G` for every start index; TX and RX groups | D-78: no group object, start arrays; the timeline rules themselves stand ([timing.md](timing.md) §3) |
| D-20 | one stats schema for every essence: cumulative counters (no reset, no epochs), per-writer blocks, gauges by scan, log2 or linear histograms, windowed maxima; reads never take a tasklet lock | D-80: the same semantics through a registry of named values instead of fixed structs |
| D-22 | `struct_size` on every input, never rewritten; an exported `*_init()` per input struct and C-only value macros; the zero-default rule with the 09 table; fixed-size sub-structs; plain flag literals, unknown bits rejected; no implicit padding | D-74: `MTL_INIT(&s)` (the per-struct form had an out-of-bounds write across versions); outputs take a size |
| D-25 | RTP level stays in the legacy API for v1; the `unit` field reserves a packet-chunk kind | D-82: packet units on every essence, a generic RTP essence (the maintainer required RTP passthrough in the high-level API) |
| D-28 | v1 reuses today's `mtl_handle`; a refcounted default instance is added | D-44: typed `mtl_instance_h` over versioned parameters (C5 §2.9) |
| D-29 | one submitting context per session and one reader per queue | D-50: MP-safe acquire and releases, `MTL_SESSION_MT_SUBMIT`, readers under the reaper lock unless SINGLE_READER |
| D-35 | completion mode NONE by default for library pools, ALL forced for attached, dynamic and exported pools; `blocked_on`; a cookie in NONE `-MTL_EINVAL` | D-77: results on or off; EXCEPTIONS cut |
| D-40 | buffers attachable to several sessions with one exclusive lease; `mtl_tx_acquire_buffer` | D-55: a slot belongs to one pool; holds instead of cross-session leases (C5 §6.2) |
| D-46 | where a knob lives: cross-essence fields in the session config, essence fields in media configs, optional groups in `next` blocks, backend tuning as key-value options, booleans as flags | D-73: typed fields for what most applications set, options for the rest |
| D-61 | EQ subscription masks with a copy per subscriber, shared EQs in Phase 1, `USER` posts in their own ring, the origin's name in every event, `SESSION_RETIRED` to every session subscriber | D-79: per-session events and one queue type; user posts and per-port subscription removed |
| D-65 | an L4 simple layer (`mtl_simple.h`: open, frame, send, close) for the 10-line loop | D-72 (`mtl_session_open`), then D-97 (typed fields only) |
| partly | D-72's spec strings and `mtl_session_config_parse` (by D-97); D-63's `MTL_INSTANCE_MANAGER_OPTIONAL` (by D-92); D-43's trywait codes 1 / 0 / < 0 (by D-88); D-85's FREERUN fallback in Phases 1–6 (by D-98, OI-5); Q-MODE-6's "SDP values only" (by D-94) | — |

### 6.13 The samples page

The archived samples page (revision 4) gave each example a persona, the point it shows, its
headers and its picture. That table survives, extended with a phase column and a text link, at
the top of [diagrams.md](diagrams.md) (checked 2026-10-02: every example ex01…ex13, the C++ twin,
`ex_common.h` and the st20p side-by-side); its notation table (blue = application code, green =
MTL, grey = network, at most about eight boxes per picture) is the same page's legend. Nothing of
the samples page is lost.
