# R2 — Verification of revision 2 against reviews C1–C4

| | |
|---|---|
| Date | 2026-09-29 |
| Verifies | revision 2 of `doc/unified-api/`: README, 00–14, OPEN-QUESTIONS, DECISIONS, side-findings |
| Against | [C1](C1-realtime-feasibility.md), [C2](C2-timing-standards.md), [C3](C3-usability-personas.md), [C4](C4-consistency-audit.md); [R2 citation log](R2-citation-repin-log.md) consulted |
| Method | full read of every r2 document and every review; `python3` scripts for identifiers, call arity, Q/G/D/SF/R/GO IDs, phase exits, links, anchors, list indent, table columns, line length; `fractions.Fraction` for all timing arithmetic; 6 `path:line` spot checks with `git show 545a266a:<path>` |
| Mode | read-only; no file other than this one was edited |
| Rule | **Addressed** = the r2 text reflects the fix. **Partly** = part of the fix is missing, or the fix left a contradiction. **Not addressed** = no change. **Declined** = r2 (or the review itself) scopes it out |

## Summary

| Review | Items | Addressed | Partly | Not addressed | Declined |
|---|---|---|---|---|---|
| C1 | 22 (#1–21 + risk table) | 21 | 1 | 0 | 0 |
| C2 | 29 (#1–28 + arithmetic table) | 25 | 4 | 0 | 0 |
| C3 | 44 (P1-1…B-3 + consistency + boilerplate tables) | 38 | 6 | 0 | 0 |
| C4 | 85 (C-01…C-44 + §1 error-code note + 40 §2–§6 items) | 76 | 6 | 2 | 1 |

New inconsistencies found in r2: 55 (§5: 14 in the header sketch, 5 in the examples, 14 in states/codes/classes, 12 in requirement and rule text, 5 in timing text, 5 in traceability). None is a blocker for the design direction; items 5.1-1, 5.2-1, 5.2-2, 5.2-3, 5.3-1 and 5.6-1 should be fixed before the header sketch is called normative.

Arithmetic (§6): every number in 06 §3.3, §4.5, §10.1 and the other stated timing figures is exact, except two wording problems in the grid rule (5.5-1, 5.5-2).

Markdown (§7): 0 lines > 400, 0 broken links or anchors in the r2 documents, 0 list-indent or table-column defects.

## 1. C1 — real-time feasibility

| # | Disposition | Where in r2 | Note |
|---|---|---|---|
| 1 | Addressed | 04 §4.1, §4.4; 02 §2.2; Q-ARCH-1a; D-06; 14 S3 | consumer-side materialisation, L0 hook list, S3 re-scoped |
| 2 | Addressed | 04 §4.4 (idle cleanup, completion latency in `get_info`); 03 §6.2; Q-CMP-7; G-03 | C1's proposed Q-CMP-6 became Q-CMP-7 |
| 3 | Addressed | 04 §5.1 (seq_cst fences both sides), 04 §4.1 cost; G-52 litmus test | |
| 4 | Addressed | 04 §5.2 (deadline-driven, per-scheduler words, timer slack, affinity, W0 for 125 µs audio); Q-THR-2a; S1; 13 §8 | |
| 5 | Addressed | 03 §2.2; Q-THR-5 revised; 04 §9 | |
| 6 | Addressed | 08 §2.4 per-writer blocks; 14 Phase 1 (L2 counters) / Phase 2 (engine); G-40 split; risk table | |
| 7 | Addressed | 04 §2.2 per-backend table; 02 §1 rule 2; G-39 scoped; 06 §2.3 time base for every source; Q-THR-7a | |
| 8 | Addressed | 07 §3.3 (pending state for tasklets, per-class rings) | |
| 9 | Addressed | 13 §8 tail gates, completion latency, waker, DP cost excl. conversion, 2110-21 under stress | |
| 10 | Addressed | 04 §3.2 (double-buffered template, activation index, queue swap, RX add-before-remove); 09 §1.1; SF-36, SF-37 | |
| 11 | Addressed | Q-THR-9 option (b) carries the quiesce handshake; 04 §4.1 CAS claim; SF-39 | audio recovery taking a blocking S on the tasklet (`st_tx_audio_session.c:2742`) and "the worker needs the waker" are not recorded |
| 12 | Addressed | 04 §4.4 rejected-at-pick-up; 07 `REJECTED_AT_PICKUP`; SF-38; 14 §0 | |
| 13 | Addressed | 04 §3 DPC class; 13 §8 | |
| 14 | Partly | 04 §8, 08 §6, G-44 fixed | 01:160 R-OBS-5 still says "nothing at INFO or below on tasklets" (fix 5.4-1) |
| 15 | Addressed | 04 §2.1; G-39 footnote 2 | |
| 16 | Addressed | 05 §6.3, 04 §9, Q-MEM-6 | |
| 17 | Addressed | 08 §1, Q-OBS-3 | |
| 18 | Addressed | 06 §7.5 `enqueued_first`; 07 §1.1 admission verdict; Q-CMP-6; Q-TIME-10 | |
| 19 | Addressed | 02 §4.1 (hand to pipeline), 04 §4.2 (v1 slots, later ring); 03 §3.3 (`tv_mgr_attach`); 04 §4.4 latency | |
| 20 | Addressed | 04 §4.4 CAS reclaim; 03 §3.4 "plugin conversion bounds the abort" | |
| 21 | Addressed | 04 §3.3 `mt_in_busy_loop` in every busy loop | |
| Risk table | Addressed | 14 risks (all 8 rows) | |

## 2. C2 — timing and standards

| # | Disposition | Where in r2 | Note |
|---|---|---|---|
| 1 | Addressed | 06 §5.1 source kinds, `min_tx_delay_ns`, L reported, L ≥ 2 rejected for PLAYBACK, `TIMING_INFEASIBLE`; 06 §4.3 LOCKED_PHASE; Q-TIME-18, Q-TIME-24 | ANC rule correctly deferred to Q-TIME-18 |
| 2 | Addressed | 06 §3.1 lazy `AT_START`, `-EAGAIN` anchor, `-ERANGE`; 03 §5 step 2 cites 06 §3.3 | see risk R-3 for late joiners |
| 3 | Addressed | 06 §3.3 table corrected (verified §6), cap 1 s → `-EINVAL`, horizon from max(now, start) 06 §7.4; Q-TIME-19 | rule wording contradicts the table (5.5-1, 5.5-2) |
| 4 | Addressed | 06 §7.2 bounded SEND_LATE + `WOULD_OVERLAP`; G-24 scoped; 06 §10.2 qualified | |
| 5 | Addressed | 06 §4.3 NEAREST_EPOCH / LOCKED_PHASE, `DUPLICATE_SLOT`/`OFF_GRID` results; Q-TIME-25; D-38 | "implicit re-anchor, if the session allows it" has no knob (5.5-3) |
| 6 | Addressed | 06 §5.2 ST30 `D_a` clamp; Q-TIME-11 revised | |
| 7 | Addressed | 06 §8 carry buffer, n+1 deadline, zero-pad, `samples_padded`; 07 §1.1 | |
| 8 | Addressed | 06 §7.3 EMPTY_ANC / KEEPALIVE / SILENCE; 10 §8 shows ANC submissions; G-61 | |
| 9 | Partly | 06 §5.2 CBR, `mtl_st22_config.codestream_bytes`, `INVALID_PAYLOAD`; G-66 | `st22.padding_bytes` counter not in 08 §2.2 |
| 10 | Addressed | 06 §2.2 `CLOCK_TAI` validation, §2.3 one time base + per-leg compensation, §2.4 step policies and AUTO backward rule; Q-TIME-2 | |
| 11 | Addressed | 06 §11 `rx_rtp_offset`, `mediaclk_mode`, `RX_TIMEBASE_SUSPECT`; 09 §1.2; G-67 | |
| 12 | Addressed | 06 §4.4 forward-only while RUNNING; 07 §5 `-EINVAL` | |
| 13 | Addressed | 13 §7 extended (13 rows), method vs rules stated; 14 Phase 0 contract update | some rows carry "—" instead of a Q-ID, acceptable where r2 matches the contract |
| 14 | Addressed | 06 §5.3 "first-packet wire time ≥ t" | |
| 15 | Addressed | 06 §5.2 invariant T7 + pre-fill cap; E5; G-59 | |
| 16 | Partly | 06 §7.1/§7.5 `scheduled_first` defined, `margin_ns` vs `pickup_slack_ns`; 07 §1.1 | no per-unit `max_packet_lateness_ns` vs TPRj, only the aggregate `tx.tpr_late_pkts` |
| 17 | Addressed | 06 §3.4, §4.2; 10 §1.3 `fps` comment | |
| 18 | Addressed | 06 §4.5 audio formula, grid-only note, 06 §3.2 2^63 / 2262 | |
| 19 | Partly | 06 §4.1 session media mode; D-38 | "groups require INDEX or TAI (AUTO cannot be synchronised)" is stated nowhere (03 §5, 06 §10.3) |
| 20 | Partly | 06 §4.4 and §8 both edited | they still disagree on a non-packet-aligned forward gap (5.5-4); DISCONTINUITY re-phasing when the new index is not a multiple of S is undefined |
| 21 | Addressed | 06 §8 integer S; `mtl_audio_config.samples_per_packet`; SF-35 | |
| 22 | Addressed | 06 §4.2 F bits, §5.2 TSST, `mtl_anc_config.total_lines`, §10.3–10.4 inheritance | |
| 23 | Addressed | 09 §1.2 `tsmode`; 08 §4 declared; 06 §5.4 PRES | |
| 24 | Addressed | 06 §11 tolerated PD, class A default; Q-TIME-14 | |
| 25 | Addressed | 06 §11 flush-deadline default | |
| 26 | Addressed | 06 §2.2 last row | |
| 27 | Addressed | (c) 06 §12, 08 §2.2; (d) 06 §12; (e) 06 §7.4 `BEFORE_START`; (f) 06 §7.5 `observed_first_leg`, 08 leg skew; (a)(b) Q-TIME-20; (g) 06 §11 last bullet | |
| 28 | Addressed | G-19 ranges + footnote | |
| A | Addressed | every "error"/"gap" row of C2 §A fixed; recomputed in §6 | |

## 3. C3 — usability

| # | Disposition | Where in r2 | Note |
|---|---|---|---|
| P1-1 | Addressed | 07 §2.2–2.3 (NONE default, `blocked_on`, 1 s admin warning); Q-CMP-1; D-35; 12 §4 | |
| P1-2 | Addressed | 10 §1.4, §3 `mtl_instance_open_simple`; 02 §3 now `mtl_init` | 02:158 still says the default instance comes "later" (5.4-5) |
| P1-3 | Addressed | 09 §1.1 zero = default per field; 11 §2.1 rule 7; `mtl_flow_parse` | |
| P1-4 | Addressed | 11 §2.1 rule 2; NULL submission/start; 10 §1.1 macros | |
| P1-5 | Addressed | 04 §3 DPC; `app_format` 0 = transport; 10 §3 one plane | |
| P1-6 | Addressed | 10 §1.1 `MTL_MS`/`MTL_SEC`/`MTL_TIMEOUT_INFINITE`; 04 §3 note 4 | |
| P1-7 | Addressed | 05 §5 `rx_fill` default; 05 §8 | |
| P1-8 | Addressed | 10 §2 first-sample surface, §4 minimal RX | the "advanced section" split of the header was not done (cosmetic) |
| P2-1 | Addressed | 04 §5.4 sticky + resume + per-queue cancel; Q-LIFE-6; D-30; G-64; 10 §7 | |
| P2-2 | Addressed | `mtl_tx_acquire_buffer` (03 §4.1, 10 §1.8, §6); D-40; G-60 | |
| P2-3 | Addressed | 11 §5 FFmpeg row + footnote | |
| P2-4 | Addressed | 03 §6.3 (c) default; Q-LIFE-4; D-36; G-65 | |
| P2-5 | Addressed | 04 §5.3 session wait fd with mask, draining rule | draining conflicts with the DP "no syscall" rule (5.3-6) |
| P2-6 | Partly | `mtl_instance_acquire_default` (03 §7, 10 §1.4); private EQ 07 §3.4; Q-LIFE-2 = B | `mtl_port_open` (03:386) is not in 10; no phase in 14 schedules the default instance (5.6-4) |
| P2-7 | Addressed | dry-run `mtl_<media>_session_query`, `mtl_video_format_enum`, `pool.count` in CREATED | 05 §4.1 `mtl_video_layout_query` signature differs from 10 (5.1-1) |
| P2-8 | Addressed | 06 §4.3 `DUPLICATE_SLOT`; 03 §3.4 flush; 06 §2.1 `mtl_time_convert` | |
| P2-9 | Addressed | 06 §3.1 named timelines, §10.3 | late-joiner semantics undefined (risk R-3) |
| P2-10 | Addressed | 04 §5.4 FFmpeg; 04 §4.2 overlap check; 11 §5 export-pool abort; Q-MEM-5; 09 §7 RX_FORMAT; 08 §3 latency range | |
| P3-1 | Addressed | 06 §3.1 `AT_START` default; 10 §8 | |
| P3-2 | Addressed | `mtl_anc_packet` (10 §1.3), submission `anc`/`anc_count`, RX meta area + `anc_count` | 06:235 field order differs from 10 (5.1-5) |
| P3-3 | Addressed | `mtl_tx_write` (10 §1.8), `sample_count` authoritative, carry padding | 06:500 still shows the old `&media` argument (5.1-4) |
| P3-4 | Addressed | 06 §4.1 session mode; 09 §1.2 late default per source kind / mode | |
| P3-5 | Addressed | 06 §4.1 `valid` only for plain values; 07 §5 `-EINVAL` rule | |
| P3-6 | Addressed | 09 §1 `leg_count`; 06 §7.4 preroll; `mtl_tx_cancel`; 06 §10.2 arity | |
| P4-1 | Addressed | 10 §9 loop; `ready_rows == rows` is FINAL | 10 §9 now submits an index in an AUTO session (5.2-3) |
| P4-2 | Addressed | 09 §1 `unit = MTL_UNIT_ROWS`; 02 §2.2 session-layer adapter, Phase 6 | |
| P4-3 | Addressed | `mtl_session_row_deadline` (06 §9, 10 §1.8) | |
| P4-4 | Addressed | 07 §1.3 one mechanism + FINAL | |
| P4-5 | Addressed | 10 §9 note; 04 §5.2 W0; source kinds | |
| P5-1 | Addressed | 03 §4.2 / 05 §5 `BY_INDEX` | |
| P5-2 | Addressed | 05 §3.2 split row; `mtl_mem_get_info` | |
| P5-3 | Addressed | 03 §4.3, `mtl_rx_transfer`; Q-MEM-11 | |
| P5-4 | Partly | 05 §4 view `addr = NULL` + `domain` | no device handle in the view |
| P6-1 | Addressed | 12 §3 row, 12 §4 comment | |
| P6-2 | Partly | `blocked_on` exists (07 §2.3) | 12:73 still maps the Rivermax statuses to "`-EAGAIN` (+ counters)" (5.4-7) |
| P6-3 | Addressed | 12 §3 chunk, commit-time-0 and teardown rows; Q-MODE-6 | |
| P7-1 | Addressed | 03 §3.3, 12 §2 row 1 | |
| P7-2 | Addressed | 07 §1 union, 256 B, `hdr.size`, `mtl_cq_read` signature | typed `mtl_tx_reap` arrays reintroduce a size problem (5.3-5) |
| P7-3 | Addressed | timeout last everywhere; one bind verb; 12 §2 thread row; 07 hdr cookie rule, G-47 | |
| B-1 | Addressed | 09 §1 fixed-size sub-structs; 11 §2.1 rule 4; `mtl_flow_parse` | |
| B-2 | Addressed | 11 §2.1 rule 2 | |
| B-3 | Partly | union CQ; `uint32_t` enum fields; view lifetime 05 §4 | no test for "SWIG releases the GIL around WT calls" in 13 |
| Consistency table | Partly | most rows fixed | residuals: `mtl_is_manager_alive` and `mtl_session_api_init` not in 10; `mtl_completion_config` never defined; `planes`/`plane` naming kept; layout-query signature (5.1-1, 5.1-6, 5.1-8) |
| Boilerplate table | Partly | B5, B11, B13, B14 closed | B6 "published FourCC / AV_PIX_FMT mapping in L4" not planned |

## 4. C4 — consistency audit

### 4.1 C-01 … C-44

| # | Disposition | Where / note |
|---|---|---|
| C-01 | Addressed | 01 R-LIFE-1, 03 §3.1–3.2 (DESTROYING column), 10 §1.2 enum; the enum comment now disagrees with the table (5.3-3) |
| C-02, C-03, C-04, C-05, C-06, C-07, C-08, C-09, C-10, C-11 | Addressed | grep finds no stale spelling; 07 §1.1 EXACT rule; 06 §7.5 margins; 05 §7; 03 §4.2; 05 §1; `update_flow` everywhere; one bind verb |
| C-12, C-13, C-14, C-15, C-16, C-17, C-18, C-19, C-20 | Addressed | signatures follow 10 (timeout last); 06 §10.2; 03 §6.1; 02 §3; no `mtl_mem_region`; 05 §3.1; 09 Phase 6; 03 §2.1 layout and base/lease |
| C-21 | Addressed | the five inputs are `uint64_t`; 11 §2.1 rule 6 — but the new wait masks are `uint32_t` (5.1-9) |
| C-22, C-23 | Addressed | 09 §6.2 `uint32_t`; fixed-size sub-structs, `MTL_TX_REASON_SLOTS` |
| C-24 | Partly | `mtl_slot_hint.struct_size` and `mtl_buffer_view` added; `mtl_rx_timing` is still only a table (06 §11), never a struct, and not listed in 10 |
| C-25 | Addressed | size parameters dropped; EQ uses caller-set `ev[i].struct_size` instead of `entry_size` (acceptable alternative) |
| C-26 | Partly | all listed prototypes added except `mtl_session_api_init` (11:39) |
| C-27 | Partly | requested rows added; 04 §10 still lacks wait fds, `trywait`, CQ/EQ create/destroy/resume, instance and port lookups, stats epochs, `mtl_stat_*`, `mtl_time_convert`, `mtl_last_error_detail` (5.4-10) |
| C-28 | Partly | 07 table rows and G-41 fixed; 01:148 R-CMP-5 still says "a state getter for each event kind" (5.4-2) |
| C-29, C-30, C-31, C-32, C-33, C-34, C-35, C-36, C-37, C-38, C-39, C-40, C-41 | Addressed | 07 §1.2 `missing[]`, 05 `meta_capacity`; 09 §1 `next`/`unit`/timing fields; 06 §2.1/§3.1 flags; outcome = result or counter; 07 §5 rows; 11 §6; 04 no longer requires migration; 03 §3.3; OQ index; `GO-n`; 06 §4.5; 10 §9 |
| C-42 | Partly | 11 §5 names "52 call sites in 8 files" in the consumer cell; the Size column still says "—" |
| C-43 | Not addressed | 07 §2.1 bound "unread results ≤ pool size" ignores `SOURCE_RELEASED` and progress entries (5.3-7) |
| C-44 | Addressed | 13 §7 cites Q-TIME-21 |

### 4.2 Other C4 items

| Item | Disposition | Where / note |
|---|---|---|
| §1 error-code note (wrong-direction verb) | Addressed | 07 §5 `-EINVAL`; G-46 |
| §2.1 requirement gaps | Addressed | 13 §0 map; every MUST/SHOULD mapped (R-LIFE-5 deferred) |
| §2.2 proposed G-45…G-57 | Addressed | 13 §1–§5 (plus G-58…G-67 from other reviews) |
| §2.3 G-34/36/37/39 in no exit | Addressed | 14 Phases 1, 2, 4 — but G-48 and G-51 are now in no exit (5.6-1) |
| §2.4 decisions for B questions and implicit positions | Addressed | D-28…D-41; only Q-TIME-18 (B) has no decision, which is correct because it has no position yet |
| §2.5 link Q-IDs to anchors | Not addressed | optional suggestion; Q-IDs remain plain text |
| §2.6 T-01, T-02 | Addressed | README conventions, 06 header, 13 §7, 14 Phase 0; R2 citation log |
| §3 dropped questions (11) | Addressed | Q-CMP-6, Q-TIME-23 (late policy), Q-TIME-21 (USER_PACING), Q-TIME-22 (CTM/LLTM), Q-LIFE-10, Q-THR-9, Q-THR-10, Q-MEM-12 (IOVA), Q-MODE-8; Q-MODE-4 and Q-ABI-3 extended. r2 renumbered C4's proposals; all IDs resolve |
| §4.1 modes #6, #7, #13, #14, #17, #18 | Addressed | 09 §8 rows, 09 §1.3 options, 05 §7 item 3, 03 §4.3 |
| §4.2 matrix features (8) | Addressed | `mtl_session_options`, `mtl_flow` DIT/K, `rx_incomplete`, pacing classes 06 §6, Q-THR-10 |
| §5.1 labels (rows 7, 8) | Addressed | G-20 and 06 T4 carry the labels |
| §5.1 row 28 | Partly | 06 §8 bullet shortened; the stale SP-09 was replaced, but the ID `SP-09` was reused for a different finding (side-findings:74), and R05 F9 is not marked fixed (research is input) |
| §5.1 rows 29–36, §5.2 sweep | Addressed | re-pinned; R2 citation log; spot checks of 6 citations at `545a266a` (`st20_pipeline_tx.c:268-288`, `st_tx_video_session.c:2130-2134`, `st_tx_audio_session.c:923-930`, `st_tx_ancillary_session.c:942-946`, `mt_dp_socket.c:236`, `mt_admin.c:30`) all show the cited code |
| §6 long lines, table columns, SP-07/DD-13 splits | Addressed | 0 lines > 400; `side-findings.md:48` escaped; SP-07a–d, DD-13a–f |
| §6 research-note indentation and `research/13:135` | Declined | C4 itself scopes these to "only if the notes are published" |

## 5. New inconsistencies in revision 2

Each item: location, the problem, and the exact fix. "10" is the reference.

### 5.1 Header sketch (10) versus the other documents

1. `05-memory-and-buffers.md:163` declares `mtl_video_layout_query(const struct mtl_video_format* f, uint32_t app_format, struct mtl_layout_req* out)`; 10:156 declares `(const struct mtl_video_config* v, struct mtl_buffer_requirements* out)`, and `mtl_video_format` / `mtl_layout_req` exist nowhere. Fix 05:163 to 10's signature.
2. `03-object-model-and-lifecycle.md:386` uses `mtl_port_open`, which 10 §1.4 does not declare. Fix: add `MTL_API_CP int mtl_port_open(mtl_handle mt, const char* name_bdf_or_ip, uint32_t* port);` to 10 §1.4, or drop the phrase from 03:386.
3. `03-object-model-and-lifecycle.md:389` writes `mtl_port_find(mt, name, BDF or IP, &port)` (4 arguments); 10:137 has 3. Fix: `mtl_port_find(mt, name_bdf_or_ip, &port)`.
4. `06-timing-pacing-and-sync.md:500` writes `mtl_tx_write(s, data, bytes, &media, timeout)`; 10:224 takes `const struct mtl_tx_submission* sub`. Fix: `mtl_tx_write(s, data, bytes, &sub, timeout)` with "the submission's `media_index` / `sample_count` / `anc`".
5. `06-timing-pacing-and-sync.md:235` lists `mtl_anc_packet { …, stream, udw_offset, udw_count }`; 10:125 orders `udw_count, udw_offset`. Fix the footnote to 10's order.
6. `09-media-modes-and-backends.md:26`, `:44` use `struct mtl_completion_config`, which is never defined and not in the 10:73-79 list. Fix: in 09 §1 add `struct mtl_completion_config { uint32_t mode; /* MTL_COMPLETE_* */ uint32_t reserved[7]; };` and add it to the 10 list.
7. `07-completions-events-and-errors.md:46`, `:91` use `struct mtl_rx_progress` and `struct mtl_rx_timing`; neither is defined (06 §11 is a table) nor listed in 10:73-79. Fix: add a C definition of `mtl_rx_timing` in 06 §11 (fields of the table, `arrival_first/last[MTL_MAX_LEGS]`, reserved tail) and of `mtl_rx_progress` in 07 §1.3 (`hdr`, `ready_rows`); list both in 10.
8. `11-abi-compatibility-and-migration.md:39` uses `mtl_session_api_init(MTL_API_VERSION)`; neither is in 10 (open since C3/C4). Fix: add `MTL_API_CP int mtl_session_api_init(uint32_t api_version);` and `#define MTL_API_VERSION …` to 10 §1.1, or replace the cell with "API-version field in the versioned init params".
9. `10-api-sketch.md:200-202` take `uint32_t mask` for `mtl_session_get_wait_fd` / `trywait` / `wait`; 11:63 rule 6 and R-ABI-1 require `uint64_t` flag arguments. Fix: `uint64_t mask` and `#define MTL_WAIT_ACQUIRE (1ull << 0)` etc.
10. Constants used elsewhere but absent from 10: `MTL_SESSION_MT_SUBMIT` (04:179), `MTL_SESSION_WAKE_DIRECT`
    (04:258), `MTL_ACTIVATE_NOW` (09:87), `MTL_SUB_RTP` and `MTL_SUBMIT_DISCONTINUITY` (06:207, `:211`),
    `MTL_RX_MISSING_RANGES` (07:103), `MTL_RX_F_USED_REDUNDANCY` (07:114), `MTL_TX_REASON_SLOTS` /
    `MTL_RX_REJECT_SLOTS` (08:76, `:79`), `MTL_TX_TIMING_NON_COMPLIANT` (07:75). Fix: add a "constants" block
    to 10 §1.2.
11. `09-media-modes-and-backends.md:56` shows an initialiser macro `MTL_SESSION_CONFIG(MTL_TX, .name = "cam1")` that 10 §1.1 does not define (and 10 uses `mtl_session_config_init`). Fix: add the macro to 10 §1.1 or replace the sentence with the `*_init()` call.
12. `03-object-model-and-lifecycle.md:377` introduces `MTL_UNINIT_DESTROY_ALL`, but `mtl_uninit(mt)` takes no flags and 10 declares no replacement. Fix: "a versioned uninit (Q-ABI-2) may take `MTL_UNINIT_DESTROY_ALL`", and list it under Q-LIFE-3.
13. `11-abi-compatibility-and-migration.md:36` writes `mtl_stat_get_u64(obj, key, param)`; 10:208 has a fourth out argument. Fix: `mtl_stat_get_u64(obj, key, param, &value)`.
14. `04-threading-and-execution.md:308` `mtl_inline_notify_fn` and `05-memory-and-buffers.md:45` `mtl_rx_provide` are not in 10; both are "later". Fix: add a "reserved / later" list at the end of 10 §1 so the "every function appears here" rule (10:8) holds.

### 5.2 Examples in 10

1. `10-api-sketch.md:295`, `:315`, `:352`, `:382`, `:437`, `:454` pass `struct mtl_buffer_view`, `struct mtl_buffer_requirements`
   and `struct mtl_slot_hint` to the library uninitialised. All three start with `struct_size`, and 11:54 rule
   3 plus the 10:204/`:254` convention ("`struct_size` set by caller") make the library honour it. Fix: add
   `MTL_BUFFER_VIEW()` / `MTL_SLOT_HINT()` / `MTL_BUFFER_REQUIREMENTS()` initialisers to 10 §1.1 and use them,
   or state in 11 rule 3 that these three are written in full and exempt.
2. `10-api-sketch.md:457` forwards with `.media_index = u.timing.media_index`, but
   `06-timing-pacing-and-sync.md:349` says forwarders must copy the input RTP (PASSTHROUGH); the example also
   never attaches the buffer to both sessions (03:296). Fix: create the TX session with `timing.rtp_mode = MTL_RTP_PASSTHROUGH`,
   submit `.rtp_override = u.timing.rtp, .valid = MTL_SUB_RTP`, and show `mtl_session_attach_buffers` on both
   sessions.
3. `10-api-sketch.md:436` creates the progressive session without `media_mode`, so it is AUTO, and `:439` submits `.media_index = …`, which 06:221 makes `-EINVAL`. Fix the comment: "`sc.unit = MTL_UNIT_ROWS, timing.source_kind = MTL_SOURCE_GATEWAY, timing.media_mode = MTL_MEDIA_INDEX, troffset_ns set`".
4. `10-api-sketch.md:404` uses `for (each of video, audio, anc config sc_x)`, which does not compile, against 10:8. Fix: `struct mtl_session_config* scs[] = { &sc_v, &sc_a, &sc_anc }; for (int i = 0; i < 3; i++) { … }`.
5. `10-api-sketch.md:429-431` tells two framework elements to open a named timeline and each start
   `AT_MEDIA_INDEX 0`. After the first start resolves T0, a second start whose M(0) is closer than the lead
   fails with `-ERANGE` (06:140-142). Fix: state that the second element starts at the first index whose
   instant is ≥ now + `min_submit_lead` (from `mtl_timeline_get_anchor`), or define a join mode in
   `mtl_start_params` (`MTL_START_JOIN`) that picks it.

### 5.3 States, statuses, error codes, call classes

1. `07-completions-events-and-errors.md:280` defines `-EAGAIN` as "back-pressure only", but
   `04-threading-and-execution.md:271` makes `*_trywait` return `-EAGAIN` when something *is* available,
   `10-api-sketch.md:166` returns it for an unresolved anchor, and `03-object-model-and-lifecycle.md:149`
   returns it from RX `dequeue` in ARMED. Fix the 07 row: "back-pressure or not-yet-available: no buffer /
   result room / data now (`blocked_on`), RX dequeue before RUNNING, timeline anchor unresolved; `*_trywait`:
   something is already available, do not block".
2. `07-completions-events-and-errors.md:289` limits `-EBUSY` to "resource in use", but 03 §3.2 returns it for
   state refusals (bind/attach/option in ARMED…ERROR, flush and `update_flow` in DRAINING), 03:188 for a group
   member started directly, and 03:252 for `mtl_tx_cancel` after pick-up. Fix the row: add "call not allowed
   in the current state (03 §3.2); a group member started directly; `tx_cancel` of a unit already picked up",
   and add `tx_cancel`, `session_start` to Typical callers. G-57 depends on this table.
3. `03-object-model-and-lifecycle.md:113` says in DESTROYING "every call except release/abort/destroy returns -ESHUTDOWN", but the 03 §3.2 DESTROYING column allows `cancel` and stats/status/state. Fix the comment: "every call except `release`, `abort`, `cancel`, the stats/status/state getters and `destroy` returns -ESHUTDOWN (03 §3.2)".
4. State rules for binds and options disagree: `10-api-sketch.md:191` and
   `09-media-modes-and-backends.md:150-151` say `set_option` "CREATED only",
   `07-completions-events-and-errors.md:169` says `bind_cq` "CREATED only", but
   `03-object-model-and-lifecycle.md:145` allows both in CREATED/STOPPED; `04-threading-and-execution.md:93`
   also lists "runtime option changes" as commands. Fix: "CREATED/STOPPED only" in 10:191, 09:150 and 07:169,
   and delete "and runtime option changes" from 04:93 (or name the runtime-changeable keys).
5. `10-api-sketch.md:223`, `:233` read results into typed `struct mtl_tx_result[]` / `struct mtl_rx_unit*`
   that have no `struct_size` (only a library-written `hdr.size`), so a newer library cannot know the caller's
   stride or size (11 rules 1 and 3). Fix: the caller sets `res[0].hdr.size = sizeof(*res)` (and
   `unit->hdr.size`) as an in/out size, stated in 07 §1 and 11 rule 3; and add the CQ records and
   `mtl_anc_packet` to the rule-1 exemption list at 11:44-45.
6. `04-threading-and-execution.md:74` promises DP calls make "no syscall except the wake of a sleeper", but
   `04-threading-and-execution.md:272-273` makes every successful read/dequeue/acquire drain the eventfd
   counter, which is a `read()` syscall. Fix 04:272-273: "drain the counter only when the fd was signalled
   (one non-blocking `read()`), which the DP contract allows as the call's own wait-object syscall", and add
   that exception to the DP row.
7. `07-completions-events-and-errors.md:136` bounds unread results by the pool size; with `MTL_CQE_TX_SOURCE_RELEASED` (07:123) or RX progress entries a unit has several entries (C-43). Fix: "unread results ≤ pool size (+1 per unit when `SOURCE_RELEASED` is enabled, + progress entries for progressive RX)".
8. `07-completions-events-and-errors.md:199` names the event field `source_kind` (INSTANCE / PORT / …), which collides with `mtl_timing_config.source_kind` (09:101, PLAYBACK / CAPTURE / GATEWAY). Fix: rename the event field `origin_kind` (and `source` → `origin`).
9. `04-threading-and-execution.md:77` limits AS calls to "atomics and at most one `write()`", but `mtl_instance_cancel_all_waiters` and `mtl_session_cancel_waiters` wake every waiter on several fds (04:283-284, 03:391). Fix: "atomics plus one `write()` to the waker's eventfd; the waker fans out the wake-ups" (or allow one `write()` per armed wait object and say so).
10. `04-threading-and-execution.md:386` marks `mtl_time_now` "Signal handler? yes" while 10:68 annotates it `MTL_API_DP`. Fix: annotate it `MTL_API_AS` in 10, or change the cell to "no".
11. `10-api-sketch.md:224` annotates `mtl_tx_write` DPC although it waits with a timeout, and `:232` annotates `mtl_rx_dequeue` WT although it is DPC when converting; G-50 demands exactly one annotation. Fix: state in 10 §1.1 "WT covers DP/DPC when the timeout is 0; DPC calls that take a timeout are annotated WT and document their caller-context work", and annotate `mtl_tx_write` WT.
12. `10-api-sketch.md:200`, `:247`, `:256` annotate `*_get_wait_fd` as DP, but the first call creates an eventfd (a syscall and an allocation). Fix: annotate them `MTL_API_CP`.
13. `07-completions-events-and-errors.md:227` lists a `SESSION_MIGRATED` event while Q-THR-6 recommends no migration for unified sessions. Fix: mark the row "(only if Q-THR-6 (b))".
14. `mtl_group_get_state(g, uint32_t* state)` (10:171) returns group states that no document defines (03:309 shows only "→ CREATED"). Fix: define `enum mtl_group_state { MTL_GROUP_CREATED, MTL_GROUP_ARMED, MTL_GROUP_RUNNING, MTL_GROUP_STOPPED, MTL_GROUP_PARTIAL_FAILURE }` in 03 §5 and 10 §1.2.

### 5.4 Requirements and rule text

1. `01-goals-and-requirements.md:160` R-OBS-5 says "nothing at INFO or below on tasklets" (C1 #14). Fix: "in release builds tasklets never format or print a log line at any level; they write binary records to the log ring (04 §8)".
2. `01-goals-and-requirements.md:148` R-CMP-5 says "a state getter for each event kind" (C-28). Fix: "a state getter for each *state* event kind (notices exempt, 07 §3.2)".
3. `01-goals-and-requirements.md:168` R-THR-3 lists four classes; 04 §3 has five. Fix: "(control / data / data with caller-context work / wait / async-signal-safe)".
4. `01-goals-and-requirements.md:149` R-CMP-6 omits the stale-lease, range and deadlock codes that 07 §5 and D-21 have. Fix: append "stale lease, out-of-range timing, wrong-thread deadlock".
5. `02-architecture.md:158` says the refcounted default instance comes "later (Q-LIFE-2)"; 03:30, 10:132 and D-28 put it in v1. Fix: "`mtl_init` (v1) or `mtl_instance_acquire_default` (refcounted, Q-LIFE-2)".
6. `03-object-model-and-lifecycle.md:77` calls `get_view` a "CP buffer call"; 10:154 annotates `mtl_buffer_get_view` DP. Fix: "Calls that do not move access (attach, destroy, `get_view`) accept either".
7. `12-familiarity-libfabric-and-rivermax.md:73` maps `RMX_NO_FREE_CHUNK` / `RMX_HW_SEND_QUEUE_IS_FULL` / `RMX_BUSY` to "`-EAGAIN` (+ counters saying which resource)" (C3 P6-2). Fix: "`-EAGAIN` + `mtl_session_get_status().blocked_on` (BUFFERS / RESULTS / RING)".
8. `08-observability.md:85` footnote "`units_suppressed` applies in completion mode." is truncated. Fix: "counts results suppressed by completion modes NONE and EXCEPTIONS (07 §2.2)".
9. `13-guarantees-and-tests.md:23` says every MUST and SHOULD has at least one guarantee, but `:48` maps R-LIFE-5 (SHOULD) to "deferred". Fix: "… has at least one guarantee, except R-LIFE-5, deferred with Q-LIFE-2".
10. `04-threading-and-execution.md:356-386` is titled "Thread-safety table (v1 surface)" but has no rows for
    `mtl_session_get_wait_fd`, `mtl_session_trywait`, `mtl_cq/eq_get_wait_fd`, `mtl_cq/eq_create/destroy`,
    `mtl_cq/eq_resume_waiters`, `mtl_session_get_cq/eq`, `mtl_instance_acquire_default/release/get_eq`,
    `mtl_port_count/find`, `mtl_session_stats_new_epoch`, `mtl_stat_get_u64/list`, `mtl_time_convert`,
    `mtl_last_error_detail`, `mtl_version_num`, `mtl_video_layout_query`, `mtl_video_format_enum` (R-THR-3
    MUST). Fix: add the rows.
11. `OPEN-QUESTIONS.md:495-498` Q-CMP-5 option (a) says "poll sets over per-session rings" and links 04 §4.2; shared CQs are poll sets over lease tables in 04 §4.3. Fix both. `OPEN-QUESTIONS.md:276` Q-THR-7 option (b) names `{offset, tsc_ratio, seq}`; 04 §8 and 06 §2.3 use `{tsc_base, tai_base, ratio, seq}`. Fix the option text.
12. `00-summary.md:105` says C4's 44 inconsistencies are "fixed"; C-43 is open and five are partial (§4.1). Fix: "fixed except C-43 (and partials, see reviews/R2-verification.md)", or fix them.

### 5.5 Timing text

1. `06-timing-pacing-and-sync.md:158-162` defines G over video/ANC starts *and* every clock including the
   audio rates, then says "audio members whose rate does not divide G are floor-aligned", which cannot happen
   under that definition. The table applies "include audio while G ≤ 1 s, otherwise drop audio" (24p + 44.1
   kHz = 1/12 s; the 1001/6000 s grid for 1001 rates exists only because of 48 kHz). `OPEN-QUESTIONS.md:873`
   Q-TIME-19 (b) says "exact for video/ANC only", which would give 1/24 s and 1001/30000 s instead. Fix
   06:158-162 and Q-TIME-19 (b): "G is the smallest period over video/ANC frame starts, 90 kHz and every audio
   rate; if it exceeds 1 s, G is recomputed over video/ANC and 90 kHz only, and audio members are
   floor-aligned with the phase reported".
2. `06-timing-pacing-and-sync.md:173` says 1001 families + 44.1 kHz fall back to "the 1001/6000 s video grid". The video/ANC-only grid is 1001/30000 s for 29.97/59.94/119.88 and 1001/6000 s only for 23.976 or when a 48/96 kHz member is present (computed in §6). Fix: "→ audio floor-aligned on the video/ANC grid (1001/30000 s for 59.94p; 1001/6000 s with a 48 kHz member)".
3. `06-timing-pacing-and-sync.md:251` offers "an implicit re-anchor, if the session allows it", but `mtl_timing_config` (09 §1.2) has no such knob. Fix: add `uint32_t off_grid_policy; /* DROP (0) \| REANCHOR */` to 09 §1.2, or delete the clause.
4. `06-timing-pacing-and-sync.md:260-261` (§4.4) allows an audio forward gap that does not start on a packet
   boundary (the carried partial packet is padded), while `:494-495` (§8) allows only DISCONTINUITY or a
   packet-aligned gap. Fix §8: "… unless flagged DISCONTINUITY or forming a forward gap (a gap that does not
   start on a packet boundary pads the carried partial packet, §4.4)"; and add one sentence on packet
   re-phasing after a DISCONTINUITY whose index is not a multiple of S (C2 #20).
5. `03-object-model-and-lifecycle.md:313-314` gives the group anchor as "first common-grid instant ≥ now + max
   lead + preroll", while `06-timing-pacing-and-sync.md:135` adds "− k × period(k's stream)". For a
   heterogeneous group, "k's stream" is undefined (k counts frames for video and samples for audio). Fix 03 §5
   step 2 to cite the 06 formula, and define in 06 §3.1 that a group's `AT_MEDIA_INDEX k` is in the units of
   the group's first video member, other members starting at their first unit with M ≥ M_video(k).

### 5.6 Traceability (G / Q / D)

1. `14-implementation-roadmap.md:67-119` lists no phase exit for **G-48** or **G-51**. Fix: add G-48 and G-51 (U/B parts) to the Phase 1 exit at 14:67-68; keep the SONAME / version-script part of G-51 for the Phase 6 ABI freeze.
2. `14-implementation-roadmap.md:67` puts G-57 ("each status, reason and 07 §5 code") in the Phase 1 exit, but Phase 1 gives only ON_TIME / DROPPED / FLUSHED (14:60). Fix: "G-57 (for the statuses, reasons and codes in Phase 1 scope)", and repeat G-57 in the Phase 3 exit.
3. `14-implementation-roadmap.md:84-85` puts G-61 (ANC keep-alive with `RTP = RTP_v(k)`) in Phase 2, but ANC RTP from media time is Phase 3 (E1, E7). Fix: move G-61 to the Phase 3 exit, or state that Phase 2 keep-alive uses the legacy ANC RTP.
4. `14-implementation-roadmap.md` schedules no phase for these v1 items: `mtl_instance_acquire_default`
   (Q-LIFE-2, **B**), `mtl_instance_open_simple` and `mtl_flow_parse` (used by the first sample, 10 §3),
   `mtl_tx_write`, `mtl_tx_cancel`, `mtl_time_convert`, `mtl_session_update_flow` (D-26), shared CQs
   (Q-CMP-5), the L4 CQ dispatcher (Q-CMP-2, "shipped in v1"), stats epochs (Q-OBS-1). Fix: add each to a
   phase scope (instance, L4 helpers, `update_flow`, epochs → Phase 1–2; `tx_write`, `time_convert`,
   `tx_cancel` → Phase 3; shared CQ, dispatcher → Phase 2).
5. `side-findings.md:74` reuses the ID `SP-09` for a new finding (R05 F6) after C4 asked to delete the stale SP-09 (R05 F9). Fix: renumber the new row `SP-10` so review references stay unambiguous.
6. Checked and consistent: 93 Q headings = 93 index rows, 24 B / 50 P / 19 L, 19 marked "new in r2" (matches 00:142-143); every Q-, G-, D-, SF/SP/DD/SC-, R- and GO- reference in the r2 documents resolves; G-01…G-67 are all defined; every D-nn "Where" section exists; the 13 §0 map covers every MUST and SHOULD.

## 6. Arithmetic spot-check (exact, `fractions.Fraction`)

G was computed as the smallest period for which every video member has an integer number of frames and 90 kHz and each audio rate an integer number of ticks.

| Item | r2 says | Exact | Verdict |
|---|---|---|---|
| 25p / 50p + 48/96 kHz | 1/25, 1/50 s, 1 frame | 1/25, 1/50 s; 3600/1800 ticks; 1920 samples | correct |
| 24p + 48 kHz | 1/24 s, 1 | 1/24 s, 3750 ticks, 2000 samples | correct |
| 30p, 60p + 48 kHz | 1/30, 1/60 s, 1 | same (1600 / 800 samples) | correct |
| 24p + 30p | 1/6 s, 4/5 | 1/6 s, 4 and 5 frames, 15000 ticks | correct |
| 29.97/59.94/23.976/119.88 + 48/96 kHz | 1001/6000 s = 166.83 ms; 5/10/4/20; 15015 ticks; 8008 (16016) | same | correct |
| 1001 families + 44.1 kHz | 1001/300 s = 3.337 s; 735.735 samples/frame at 59.94 | 1001/300 s (300300 ticks, 147147 samples); 147147/200 = 735.735 | correct; fallback wording 5.5-2 |
| 24p + 44.1 kHz | 1/12 s, 2 | 1/12 s, 7500 ticks, 3675 samples | correct under "include audio" (5.5-1) |
| 25p + 59.94p; 50p + 59.94p | 40.04 s; 20.02 s | 1001/25 s (1001 / 2400 frames); 1001/50 s (1001 / 1200) | correct |
| video-only grids (for 5.5-2) | — | 59.94p: 1001/30000 s (2 frames); 29.97p: 1001/30000 s; 23.976p: 1001/6000 s; 119.88p: 1001/30000 s | — |
| 44.1 kHz sub-sample phase | < 22.7 µs | 22.676 µs | correct |
| 06 §4.5 grid formula | `floor(T0·90000) + floor(k·1501.5)` = `floor((T0 + k·TFRAME)·90000)` when T0·90000 ∈ ℤ | identical for n = 0…2, k = 0…19 | correct |
| 06 §10.1 | 15015n, 8008n, 10n; `floor(1501.5k)` | 15015n, 8008n, 10n; 0, 1501, 3003, 4504, 6006, 7507, …, 15015 (k = 10) | correct |
| second field (1080i59.94) | first + `floor(TFRAME·90000/2)` | frames 0–3: 1501, 4504, 7507, 10510, equal to `floor((m·TFRAME + TFRAME/2)·90000)` | correct |
| per-frame audio at 59.94 | 800/801 | 800, 801, 801, 801, 801 repeating | correct |
| AAC 1024; 29.97 frame | 21 × 48 + 16; 1601/1602 | 21 r 16; 8008/5 = 1601.6 | correct |
| carry buffer | ≤ 1152 B | 48 × 8 × 3 = 1152 | correct |
| L = 1 at 1080p59.94 | M + 17.29 ms; RTP offset ≈ −1501 | M + 17.2876 ms; −1501.5 ticks | correct |
| TROFFSET − VRX0·TRS | ≈ 604 µs | 604.31 µs | correct |
| chain completion | ~1.9 ms (512 pkts) | 512 × 3.7074 µs = 1.898 ms | correct |
| `late_tolerance_ns` default | min(TROFFSET, TFRAME − RACTIVE·TFRAME) | min(637.67, 667.33) µs = 637.67 µs | correct |
| last packet, L = 0 | ≈ M + 16.66 ms | 16.654 ms | correct |
| unwrap / wrap | ±6.63 h; 13.26 h, 27.05 h | ±6.628 h; 13.256 h; 27.053 h | correct |
| TAI ns bound | < 2^63 (2262) | 2^63 ns → 2262.3; 2^61 → 2043.1 | correct |

## 7. Markdown

- Lines > 400 characters: 0 in the r2 documents and in `reviews/`.
- Relative links and anchors: 0 broken in README, 00–14, OPEN-QUESTIONS, DECISIONS and side-findings; 93/93 `#q-…` anchors resolve. The only unresolved targets are illustrative text inside reviews (`C4:269` `OPEN-QUESTIONS.md#q-time-7` relative to `reviews/`, `C4:443` `](path)`, `R2-citation-repin-log.md:112` placeholders); not r2 defects.
- List indentation: 0 violations (2-space under `-`, content column under `1.`).
- Table column counts: 0 mismatches; no unescaped `|` inside code spans in any table row.

## 8. Remaining risks

- **R-1 Header sketch not yet self-consistent.** 10 claims to be normative and compilable, but 5.1 and 5.2
  list 19 mismatches, including uninitialised `struct_size` outputs in every example and three struct types
  that are named but never defined. A doc test that "only has to build" will not catch the `struct_size`
  problem; add a runtime check that the examples pass `-EINVAL`-free under the fake engine.
- **R-2 ABI of typed result arrays.** `mtl_tx_reap` and `mtl_rx_dequeue` fill caller structs with no caller-declared size (5.3-5). Growing `mtl_tx_result` inside the 256-byte union would corrupt old callers' arrays.
- **R-3 Named timelines with late joiners.** The framework pattern (06 §10.3, 10 §8) works only if every element starts before T0 − lead; otherwise `-ERANGE` (5.2-5). Members attached after the anchor resolves may also have rates the resolved G did not include, which puts them off-grid.
- **R-4 Group start semantics.** `AT_MEDIA_INDEX k` for mixed-rate members and T0 off the grid for k ≠ 0 are undefined (5.5-5); 06 §10.1's integer identities hold only for k = 0.
- **R-5 DP syscall rule.** Draining eventfds and fan-out wake-ups from AS calls contradict the DP and AS contracts as written (5.3-6, 5.3-9). This matters for G-39/G-52 test design, not for tasklets.
- **R-6 Error-code table completeness.** G-57 is table-driven, but 07 §5 does not describe the `-EAGAIN` and `-EBUSY` uses that 03, 04 and 10 rely on (5.3-1, 5.3-2).
- **R-7 Roadmap gaps.** Several v1 API items, including one B-question outcome (`mtl_instance_acquire_default`), have no phase (5.6-4), and two guarantees have no exit (5.6-1).
- **R-8 Open standards question on the critical path.** Q-TIME-18 (live ANC with L ≥ 1) is B with no recommendation or decision until ST 2110-40 §6 is re-read; CAPTURE sessions with ANC in a group are undefined until then.
- **R-9 Grid rule ambiguity.** 06 §3.3 and Q-TIME-19 describe different rules that give different T0 quanta for common formats (1001/6000 s versus 1001/30000 s at 59.94p + 48 kHz); the maintainer's answer to Q-TIME-19 changes the §10.1 worked numbers (5.5-1).

## Applied

Editor pass after this report (2026-09-29). 10 was treated as the normative reference; other
documents were changed to match it.

| Item | Applied in |
|---|---|
| 5.1-1 layout query | 05 §4.1 now `(const struct mtl_video_config*, struct mtl_buffer_requirements*)` |
| 5.1-2, 5.1-3 ports | `mtl_port_open` added to 10 §1.4; 03 §7 `mtl_port_find(mt, name_bdf_or_ip, &port)` |
| 5.1-4, 5.1-5 | 06 §8 `mtl_tx_write(s, data, bytes, &sub, timeout)`; 06 §4.2 footnote in 10's field order |
| 5.1-6, 5.1-7 structs | `mtl_completion_config` defined in 09 §1, `mtl_rx_timing` in 06 §11, `mtl_rx_progress` in 07 §1.3; all three listed in 10 §1.2 |
| 5.1-8 | `MTL_API_VERSION` and `mtl_session_api_init` added to 10 §1.1; 11 §2 cites it |
| 5.1-9 | wait masks `uint64_t`, `MTL_WAIT_*` as `1ull << n` |
| 5.1-10 constants | constants block in 10 §1.2 (`MTL_SESSION_MT_SUBMIT`, `MTL_SESSION_WAKE_DIRECT`, `MTL_SUB_RTP`, `MTL_SUBMIT_DISCONTINUITY`, `MTL_ACTIVATE_NOW`, `MTL_TX_TIMING_NON_COMPLIANT`, `MTL_RX_F_USED_REDUNDANCY`, `MTL_RX_MISSING_RANGES`, `MTL_TX_REASON_SLOTS`, `MTL_RX_REJECT_SLOTS`) |
| 5.1-11 | `MTL_SESSION_CONFIG(dir, …)` defined in 10 §1.1; 09 §1 points to it |
| 5.1-12 | 03 §6.4 "a versioned uninit (Q-ABI-2) may take `MTL_UNINIT_DESTROY_ALL`"; Q-LIFE-3 option (c) names it |
| 5.1-13 | 11 §2 `mtl_stat_get_u64(obj, key, param, &value)` |
| 5.1-14 | new 10 §1.11 "Reserved and later" (`mtl_inline_notify_fn`, `mtl_rx_provide`, `mtl_sch_run_once`, `mtl_open_ext`, `MTL_UNINIT_DESTROY_ALL`) |
| 5.2-1 | value initialisers `MTL_BUFFER_VIEW_INIT`, `MTL_SLOT_HINT_INIT`, `MTL_BUFFER_REQUIREMENTS_INIT`, `MTL_RX_UNIT_INIT` in 10 §1.1, used in every example; 11 rule 3 |
| 5.2-2 | 10 §10: PASSTHROUGH TX session, `.rtp_override = u.timing.rtp, .valid = MTL_SUB_RTP`, buffers attached to both sessions before start |
| 5.2-3, 5.2-4 | 10 §9 comment sets `media_mode = MTL_MEDIA_INDEX`; 10 §8 loop over `scs[]` |
| 5.2-5, risk R-3 | join rule (first starter resolves T0; joiners `MTL_START_NOW`, first index from `hint.next_media_index`; passed M(k) → `-ERANGE`) in 06 §3.1, §7.6, §10.3 and 10 §8 |
| 5.3-1, 5.3-2 | 07 §5 `-EAGAIN` and `-EBUSY` rows extended |
| 5.3-3 | 03 §3.1 DESTROYING comment |
| 5.3-4 | "CREATED/STOPPED only" for `set_option`, `bind_cq`, `bind_eq` (10, 09 §1.3, 07 §2.4); "runtime option changes" dropped from 04 §3.2 |
| 5.3-5, risk R-2 | `hdr.size` in/out (07 §1, 10 `mtl_tx_reap`); CQ records and `mtl_anc_packet` exempt in 11 rule 1; rule 3 covers `hdr.size` |
| 5.3-6, 5.3-9, risk R-5 | 04 §3 DP row allows non-blocking eventfd `read`/`write` on app-side wait objects; AS = atomics and at most one eventfd `write()` (the waker's), waker fans out; 04 §5.3, §5.4 and 03 §7 match |
| 5.3-7, C-43 | 07 §2.1 capacity counts `SOURCE_RELEASED` and progress entries |
| 5.3-8 | 07 §3.1 `origin_kind` / `origin`; table column "Origin" |
| 5.3-10 | chose "no": 04 §10 `mtl_time_now` not signal-safe (seqlock retry), footnote ⁸; 10 comment |
| 5.3-11 | `mtl_tx_write` annotated WT; rule stated in 10 §1.1 and 04 footnote ⁴ |
| 5.3-12 | `*_get_wait_fd` annotated CP |
| 5.3-13, 5.3-14 | `SESSION_MIGRATED` "(only if Q-THR-6 (b))"; `enum mtl_group_state` in 03 §5 and 10 §1.2 |
| 5.4-1…5.4-4 | 01 R-OBS-5, R-CMP-5, R-THR-3, R-CMP-6 |
| 5.4-5…5.4-9 | 02 §3 instance row; 03 §2.1 "calls that do not move access"; 12 §3 Rivermax statuses → `blocked_on`; 08 footnote ¹; 13 §0 R-LIFE-5 exception |
| 5.4-10, C-27 | 04 §10 rows added for every listed function |
| 5.4-11, 5.4-12 | Q-CMP-5 (lease tables, 04 §4.3); Q-THR-7 `{tsc_base, tai_base, ratio, seq}`; 00 C4 row |
| 5.5-1, 5.5-2, risk R-9 | 06 §3.3 rule and Q-TIME-19 (b) reworded (audio rates included unless they push G above 1 s); 44.1 kHz rows split (1001/30000 s for 29.97/59.94/119.88p; 1001/6000 s for 23.976p or with 48 kHz), recomputed with `fractions` |
| 5.5-3 | `off_grid_policy` (DROP \| REANCHOR) in 09 §1.2 (replaces `reserved32`), 06 §4.3, §5.2 knob list |
| 5.5-4, C2 #20 | 06 §4.4 and §8 agree: non-aligned forward gap needs DISCONTINUITY, carry zero-padded, packets re-phased at d; §4.5 formula notes it; 13 §7 row |
| 5.5-5, risk R-4 | 06 §3.1 group `AT_MEDIA_INDEX k` in the first video member's units; 03 §5 step 2 cites the formula |
| 5.6-1…5.6-4, risk R-7 | 14: G-48, G-51 (U/B) in Phase 1 exit, G-51 SONAME/version script in a Phase 6 exit; G-57 split Phase 1 / Phase 3; G-61 kept in Phase 2 with keep-alive on the legacy ANC RTP; phases for epochs and first-sample L4 helpers (1), default instance + `mtl_port_open`, `update_flow`, shared CQ, dispatcher (2), `tx_write`, `tx_cancel`, `time_convert` (3), format-name mapping (4) |
| 5.6-5 | side-findings SP-09 (R05 F6) renumbered SP-10 |
| C2 #9, #16, #19 | 08 §2.2 ST22 `padding_bytes`; `mtl_tx_timing.max_packet_lateness_ns` (06 §7.5, §12; 07 §1.1; 08); INDEX/TAI-only group members in 03 §5, 06 §10.3, 10 (`-EINVAL`) |
| C3 P5-4, B-3, B6, consistency table | `device_handle` in `mtl_buffer_view` (05 §4, §10); SWIG GIL test note in 13 §5; L4 `mtl_video_format_names` (10 §1.5, 14 Phase 4); `mtl_is_manager_alive` listed in 10 §1.4; `mtl_buffer_desc.planes` → `plane` (05, 10, 11) |
| C-42 | 11 §5 MXL row Size column |

Skipped:

- C4 §2.5 (link every Q-ID to its anchor): too noisy for the benefit; Q-IDs stay plain text.
- C4 §5.1 row 28, "R05 F9 not marked fixed": `research/` is input and is not edited.
- Notes on rows marked Addressed (C1 #11 audio-recovery detail, C3 P1-8 "advanced" header split): outside the partly/not-addressed scope.
- Risk R-1's runtime doc-test suggestion and R-3's "joiner rate not in the resolved G": no ruling; left for the Phase 1 doc test and Q-TIME-19.

Checks after the pass: 0 lines > 400 characters; 93 Q headings = 93 index rows and every Q-, G-, D-, SF/SP/DD/SC- reference resolves; 0 broken relative links or anchors; markdownlint-cli 0.43.0 with `.github/linters/.markdown-lint.yml`: clean.
