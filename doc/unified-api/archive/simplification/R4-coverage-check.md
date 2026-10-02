# R4 coverage check: revision 4 headers against the S1 inventory and S9 gaps

| Field | Value |
|---|---|
| Purpose | Row-by-row check that revision 4 still reaches every use case of today's MTL (S1 §2), every session-only feature (S9 §3) and every pre-hide gap (S9 §7.1, H-01…H-19); D-87's gate |
| Baseline | headers in `sketch/include/mtl/experimental/` as of 2026-10-01 11:31 UTC (after the close / wait-handle / RTCP / `MTL_LATER` edits; `check.sh` OK, 113 functions + 4 `MTL_LATER`); revision 3 = `archive/r3-sketch/` and the S1 coverage column |
| Rule | The headers are normative. A feature described only in a comment, or only in prose (S5, 09), with no symbol to request it, is PARTIAL at best. Option keys of `mtl_options.h` and calls are symbols |
| Scope | Read-only check; no other file edited |

Legend:

- **COVERED**: a symbol does it. **CHANGED**: reachable, but the model, unit or default differs (stated). **PARTIAL**: part reachable (missing part stated).
- **LATER**: the missing part is declared in an `MTL_LATER` block (what works now is in the note). **NOT COVERED**: no symbol. **CUT-CANDIDATE**: on S1 §6, S9 §7.2 or REVISION-4 M12; needs approval.
- **(K)**: the row depends on a stats key name. Key names live only in S5 §3.4; `mtl_observe.h` says "the catalogue is 08", and 08 has no key catalogue (regression R-4, inconsistency I-1).
- Debug-only rows (`mtl_debug.h`) count as COVERED and say "debug".

## 1. Summary

| Status | S1 rows (r4) | S1 rows (r3, S1 column) | S9 §3 rows | H-01…H-19 |
|---|---|---|---|---|
| COVERED | 186 | 138 (incl. 3 DEBUG) | 43 | 10 |
| CHANGED | 43 | 24 | 2 | 0 |
| PARTIAL | 24 | 34 | 11 | 4 |
| LATER | 5 | (in NOT COVERED) | 3 | 3 |
| NOT COVERED | 8 | 93 | 1 | 0 |
| CUT-CANDIDATE | 24 | (not used) | 14 | 2 |
| Total | 290 (+ U-414, a note) | 290: the above + U-415 ("see U-062"); U-414 is a note | 74 | 19 |

Reading the table:

- Revision 4 closes most of revision 3's NOT COVERED rows: the instance, scheduler and port knobs (U-014…U-086) became option keys, RTP passthrough (U-340…U-346) is first class, the plugin ABI, conversion, log sink, capture, RTCP and per-unit timing now have symbols.
- 8 S1 rows remain NOT COVERED (§3) and 24 are PARTIAL; D-87 ("every row has a home, a later phase, or an approved cut") is not met yet.
- 24 S1 rows are CUT-CANDIDATE; all are on S1 §6, S9 §7.2 or REVISION-4 M12. One of them (U-095) was covered by revision 3 (R-2); U-413 lost its reserved value without an M12 entry (R-6).
- 19 rows depend on stats key names that are not in the normative set (R-4).
- REVISION-4 §4.2 says all of H-01…H-19 are in the headers or a named later phase. Not quite: H-02, H-04, H-07 and H-13 are PARTIAL; H-08 and H-17 are cuts (M12), not phases.

## 2. Regressions: rows revision 3 covered that revision 4 covers worse

Ordered by impact. "Fix" is the minimal header change that restores revision 3's reach.

| # | Rows | Revision 3 | Revision 4 | Fix |
|---|---|---|---|---|
| R-1 | U-218 slice RX; S9 V5; H-02 (RX) | `mtl_rx_wait_progress` + `MTL_CQE_RX_PROGRESS` | only `MTL_UNIT_PARTIAL`: no wait for more rows of a held unit, no row step (`slice_lines`); S4 P7's `mtl_rx_wait_rows` dropped | mtl_sync.h `int mtl_rx_wait_rows(s, lease, uint32_t min_rows, uint32_t* rows, int64_t timeout_ns)` (WT) + `MTL_OPT_RX_ROWS_STEP = 212` ("rx.rows_step") |
| R-2 | U-095 DPDK AF_XDP / AF_PACKET PMDs | `MTL_BACKEND_DPDK_AF_XDP`, `MTL_BACKEND_DPDK_AF_PACKET` | no `dpdk_af_xdp:` / `dpdk_af_packet:` name prefix, no enum | listed in REVISION-4 M12; approve the cut, or add the two prefixes to the `mtl_port_spec.name` comment |
| R-3 | U-096 backend query; S9 M8 | typed `mtl_port_get_caps().backend` + `enum mtl_backend` | key `caps.backend` (S5 only) whose values are `enum mtl_backend`, defined in no r4 header; no pre-open name parse | mtl_observe.h: `enum mtl_backend { MTL_BACKEND_DPDK_PMD = 1, MTL_BACKEND_AF_XDP = 2, MTL_BACKEND_KERNEL_SOCKET = 3, MTL_BACKEND_NULL = 4 };` (+ the DPDK PMD values if R-2 is kept) |
| R-4 | the 19 rows marked (K) in §5 | typed structs (`mtl_port_caps/status`, `mtl_instance_info/status`, `mtl_time_status`, `mtl_sched_status`, `mtl_session_stats`, `info.trs_ps` …) | named keys, listed only in S5 §3.4; `mtl_observe.h` points at 08, which has no catalogue | make S5 §3.4 the normative key table in 08 (or generate `mtl_stat_names.h`, S5 Q9) |
| R-5 | U-185, U-238; S9 V6 | `mtl_tx_acquire_dynamic` + `MTL_POOL_DYNAMIC` on the 0.1 surface (Phase 4) | `mtl_tx_acquire_layout` under `MTL_LATER` | none needed if the owner accepts LATER; record it in M12 or the phase plan |
| R-6 | U-235, U-413; S9 C4 | `mtl_cvideo_config.pack_type` + `MTL_CVIDEO_PACK_SLICE` (`-MTL_ENOTSUP`) | no field: codestream implied, slice not expressible; not on M12 | add to M12, or `MTL_OPT_CVIDEO_PACK = 514 /* "cvideo.pack" MTL_CVIDEO_PACK_CODESTREAM/SLICE (CODESTREAM) C */` |
| R-7 | U-132 | `user_cookie` on the config, the buffer and the submission | per-unit `cookie` only; the app maps `mtl_tx_result.session` / `mtl_event.origin` to its context | optional: `uint64_t user_cookie` in `mtl_session_info` from a config field, echoed in `mtl_event` |
| R-8 | U-210 | per-buffer `user_cookie` | stable `unit.slot` only | none needed (slot index is the identity, D-76); note it in the migration table |
| R-9 | U-068, U-069; S9 X1, H-08 | `mtl_sch_run_once` reserved under `MTL_UNIFIED_LATER` | dropped; user schedulers and tasklets moved to the M12 cut list | approval tracked in M12; nothing else |
| R-10 | U-139; S9 V23, H-17 | `mtl_open_ext` reserved under `MTL_UNIFIED_LATER` (the queue-meta route) | dropped; queue meta on M12 | approval tracked in M12 |

R-4 rows: U-004, U-023, U-033, U-035, U-046, U-061, U-096, U-097, U-099, U-114, U-161, U-212, U-214, U-216, U-254, U-266, U-286, U-400, U-402.

Not regressions, although the status word changed:

- U-176, U-209, U-238 (part): revision 3 had `mtl_session_set_inline_notify` and `mtl_rx_provide` in its later block; revision 4 has both under `MTL_LATER` again (rated LATER here, PARTIAL in S1).
- U-117: revision 3 reserved `MTL_MEM_DMABUF/DEVICE`; revision 4 reserves `mtl_mem_import_device` under `MTL_LATER`.
- U-022 and U-115: same reach as revision 3; rated more strictly here (the library dump period has no option; only leased-unit copies exist).
- U-174: the EXCEPTIONS completion mode is gone (M11), but it had no legacy counterpart; per-frame results remain.
- U-153: `MTL_SUBMIT_RTP_TS` with media mode AUTO is rejected (`MTL_REASON_RTP_TS_AUTO`), as revision 3 rejected `PASSTHROUGH_AUTO`.
  Legacy `USER_TIMESTAMP` without `USER_PACING` therefore needs INDEX mode with `mtl_tx_next_slot`; worth one sentence in the migration table.

## 3. Still NOT COVERED in revision 4, with minimal proposals

| Row | Use case | Header | Proposal (one line) |
|---|---|---|---|
| U-025 | schedulers for shared-RSS dispatch (`rss_sch_nb[]`) | mtl_options.h | `MTL_OPT_RSS_SCHEDS = 2122, /* "port.rss_scheds" schedulers dispatching shared RSS; scope = port C */` |
| U-044 | PTP time from TSC (`MTL_FLAG_PTP_SOURCE_TSC`) | mtl_options.h | `MTL_OPT_PTP_SOURCE_TSC = 2204, /* "time.ptp_source_tsc" bool C */` |
| U-066, S9 M9 | query CPU SIMD level | mtl_observe.h | catalogue key `instance.simd_level` (C, `enum mtl_simd_level { MTL_SIMD_NONE = 1, MTL_SIMD_AVX2 = 2, MTL_SIMD_AVX512 = 3, MTL_SIMD_AVX512_VBMI2 = 4 }`) |
| U-098 | port address in use, incl. a DHCP lease | mtl_observe.h | `MTL_API_CP int mtl_port_get_spec(mtl_instance_h mt, uint32_t port, struct mtl_port_spec* out);` (name, sip, prefix, gateway as granted) |
| U-111 | IOVA of a region or a plane | mtl_mem.h | `MTL_API_DP int mtl_mem_iova(mtl_region_h r, uint64_t offset, uint64_t* iova);` (also closes U-113) |
| U-143 | TX queue hang-detect timeout (`tx_hang_detect_ms`) | mtl_options.h | `MTL_OPT_TX_HANG_DETECT_NS = 305, /* "session.tx_hang_detect_ns" (library default) C */` |
| U-348 | RFC 4175 pixel-group layouts | mtl_format.h | `MTL_API_AS int mtl_format_pgroup(uint32_t format, uint32_t* bytes, uint32_t* pixels);` (also closes the pgroup half of U-196, V28, H-04) |
| U-364 | converter device for raw video | mtl_options.h | `MTL_OPT_VIDEO_CONVERT_DEVICE = 504, /* "video.convert_device" MTL_CODEC_DEVICE_* (AUTO) C */` |

PARTIAL rows and the smallest addition that would close each:

| Row | Missing | Proposal |
|---|---|---|
| U-006 | device abort | mtl.h: `MTL_API_AS int mtl_instance_abort(mtl_instance_h mt);` (interrupt + stop every session's TX without draining), or approve "interrupt is enough" |
| U-022 | library stat-dump period | mtl_options.h: `MTL_OPT_STAT_DUMP_S = 2211, /* "log.stat_dump_s" (10; 0 = off) R */` |
| U-024 | RSS mode L3 | mtl_options.h: add `MTL_RSS_L3`; drop or document `MTL_RSS_L3_L4_DST_PORT` |
| U-052, S9 M6 | per-scheduler sleep on/off | state "0 = never sleep" on `MTL_OPT_SCHED_SLEEP_US` |
| U-096 | backend enum, pre-open parse | R-3 |
| U-113 | IOVA | U-111 |
| U-115 | general fast copy | approve S9 CUT-8, or `MTL_API_DPC int mtl_memcpy(void* dst, const void* src, size_t n);` in mtl_util.h |
| U-196, V28 | pgroup | U-348 |
| U-207 | bytes received per video frame | mtl_observe.h: `uint64_t bytes_received` in `mtl_rx_detail` (replaces `reserved`) |
| U-215, V12, H-07 | cinst min, vrx avg, ipt, rtp delta, failed cause; per leg | mtl_observe.h: extend `mtl_rx_timing_result` and add a `leg` argument to `mtl_rx_get_timing` |
| U-218, V5, H-02 | RX rows wait | R-1 |
| U-246 | pure audio size helper | mtl_format.h: `MTL_API_AS int mtl_audio_bytes(const struct mtl_audio_config* a, uint32_t samples, uint64_t* bytes);` |
| U-248, S9 A3 | RL warm-up in ns | mtl_options.h: `MTL_OPT_AUDIO_RL_ACCURACY_NS = 605`, `MTL_OPT_AUDIO_RL_OFFSET_NS = 606` (ns), retire the packet-valued key |
| U-254, A4 | audio timing result per period | mtl_observe.h: a per-unit `mtl_rx_get_timing` variant for audio (dpvr, ipt, tsdf) or a `MTL_EVENT_RX_TIMING` event every 200 ms |
| U-255 | AM824 subframe layout | mtl_convert.h: `struct mtl_am824_subframe` with documented bit fields |
| U-266 | RTP marker, per-leg discontinuity on frame units | mtl.h: `MTL_UNIT_MARKER_SEEN 0x400000u`; mtl_observe.h `mtl_rx_detail.seq_discont[MTL_MAX_LEGS]` |
| U-268, N3, H-13 | parity check, RFC 8331 encode/decode | mtl_util.h: `mtl_anc_parity_ok(uint16_t word)`, `mtl_anc_rfc8331_encode/decode(...)` |
| U-347, S10 | payload header layouts | mtl_packet.h: `struct mtl_rfc4175_hdr`, `mtl_rfc4175_srd`, `mtl_rfc9134_hdr`, `mtl_rfc8331_hdr`, `mtl_st41_hdr` with accessors |
| U-349 | limit constants | mtl_packet.h: `#define MTL_PKT_MAX_RTP_BYTES` and `MTL_UDP_MAX_BYTES` |
| U-368, S5 | per-plane linesize/rows without a session | mtl_format.h: `MTL_API_AS int mtl_format_plane(uint32_t format, int is_app, uint32_t w, uint32_t h, uint32_t plane, uint32_t* row_bytes, uint32_t* rows);` |
| U-369 | downsample, field merge/split | approve a cut, or `MTL_CONVERT_FIELD_MERGE/SPLIT` flags and a scale factor in `mtl_convert_desc` |
| U-370, S7, H-04 | codec name | mtl_format.h: `MTL_API_AS int mtl_codec_parse(const char* name, uint32_t* codec);` and `mtl_codec_name()` |
| U-385 | select a test plugin device | mtl_options.h: `MTL_CODEC_DEVICE_TEST = 4` (with `mtl_plugin_register`) |
| U-402 | field map of `st20_rx_user_stats` | a migration table legacy field → key in 08 |

## 4. Inconsistencies inside the revision-4 headers

- **I-1.** `mtl_observe.h` says key names are the contract and "the catalogue is 08", but 08-observability.md still documents revision 3's typed structs; the key list exists only in S5 §3.4. Keys the headers rely on: `rx.detected.*` (`MTL_EVENT_RX_FORMAT`), `caps.*`, `port.*`, `info.trs_ps`.
- **I-2.** Enums that key values and events need are defined nowhere: `enum mtl_backend` (`caps.backend`), `enum mtl_time_state` (`MTL_EVENT_TIME_STATE` "time state", `time.state`). `mtl_session_info.pacing_class` and `MTL_EVENT_PACING_CHANGED` do not name their enum (`enum mtl_pacing` lives in mtl_options.h).
- **I-3.** mtl_options.h group "2100-2199 instance: ports and NIC (scope = port)": `MTL_OPT_MONO_POOL` uses scope 0 TX / 1 RX, and `MTL_OPT_IOVA_MODE`, `MTL_OPT_MEMZONE_MAX`, `MTL_OPT_CNI` are named `instance.*` inside the port group.
- **I-4.** `MTL_OPT_AUDIO_RL_WARMUP` is "packets; scope 1 = accuracy", but both legacy knobs (`rl_accuracy_ns`, `rl_offset_ns`) are nanoseconds.
- **I-5.** `enum mtl_rss` = NONE, L3_L4, L3_L4_DST_PORT; legacy `enum mtl_rss_mode` = NONE, L3, L3_L4. L3 is unreachable and L3_L4_DST_PORT has no legacy meaning.
- **I-6.** `MTL_OPT_PTP_UNICAST` is a string "delay-request address"; legacy `MTL_FLAG_PTP_UNICAST_ADDR` is a bool (unicast to the learned master). The value meaning "the learned master" is unstated.
- **I-7.** R6 in mtl.h says "the library never calls the application", yet `mtl_queue_dispatch_start` and `mtl_log_set_sink` call application functions on library threads, the plugin ABI calls plugin code, and the `MTL_LATER` `mtl_session_set_inline_notify` runs on the completing context. Reword as "never on a tasklet, except inline notify".
- **I-8.** R2 says a data call that finds nothing by its timeout returns `-MTL_EAGAIN`; `mtl_session_wait` returns 0 on timeout, and `mtl_tx_reap`, `mtl_session_read_events`, `mtl_queue_reap/ready/read_events` say "Count >= 0" without saying whether an empty result is 0 or `-MTL_EAGAIN`. `mtl_index_at` reuses `-MTL_EAGAIN` for "T0 unresolved".
- **I-9.** Two knobs for one thing: `MTL_SESSION_RX_NO_FILL` (flag) and `MTL_OPT_RX_FILL` = `MTL_FILL_NONE` (option); session `MTL_OPT_TX_QUEUE` = SHARED and port `MTL_OPT_SHARED_TX_QUEUE` (precedence unstated); video `detect` is a typed field but ANC detect is `MTL_OPT_ANC_RX_DETECT`.
- **I-10.** `mtl_log_set_sink` is process-wide, but its level `MTL_OPT_LOG_LEVEL` is an instance option (R): with two instances the effective level is undefined.
- **I-11.** Option names and stats keys share scopes: option `caps.pacing` vs stat `caps.pacing_classes`, option `port.rx_desc` vs stat `port.rx_missed`, `instance.*` in both. A name in a spec string or a doc does not say which registry it belongs to.
- **I-12.** Naming after the close rename: sessions, queues, timelines and instances `close`, regions still `mtl_mem_destroy` (and M1 "cannot be destroyed"), plugins `mtl_plugin_unload`. The mtl_queue.h banner still says "behind one wait object" although `struct mtl_wait_object` became a wait handle.
- **I-13.** `MTL_OPT_CVIDEO_DEVICE` defaults to "(AUTO)" but `enum mtl_codec_device` has no AUTO or TEST value, and `mtl_plugin_device.device` ("MTL_CODEC_DEVICE_* of mtl_options.h") has no value meaning "any".
- **I-14.** `struct mtl_anc_packet.stream` is one byte for RFC 8331's S flag and StreamNum (legacy `st40_meta` has `s` and `stream_num`); the bit layout is unstated.
- **I-15.** `MTL_OPT_PROGRESSIVE_LATE` is named "tx.rows_late" with `enum mtl_rows_late`; the symbol says progressive, the name and enum say rows.
- **I-16.** REVISION-4.md is behind the headers: it counts 112 functions (+2 later); `check.sh` now reports 113 (+4 `MTL_LATER`: `mtl_tx_acquire_layout`, `mtl_rx_provide`, `mtl_mem_import_device`, `mtl_session_set_inline_notify`). Its §4.2 claim about H-01…H-19 is addressed in §1.

## 5. Row-by-row: S1 §2 (290 rows + U-414)

| ID | Use case | r3 (S1) | r4 coverage | r4 symbol / note |
|---|---|---|---|---|
| U-001 | Open an instance on a list of ports | COVERED | COVERED | `mtl_instance_open(spec, params)`; `mtl_port_spec` array or spec string; `MTL_PORTS` env fallback |
| U-002 | Close an instance | COVERED | COVERED | `mtl_instance_close` (refcounted; lives until its last session retires) |
| U-003 | Re-open in the same process | CHANGED | CHANGED | by design (03 §7.1); nothing in the header contradicts it |
| U-004 | Share one instance between components | COVERED | COVERED | `MTL_INSTANCE_SHARED`; merge result in stat key `instance.ignored_fields` (K) |
| U-005 | Device start/stop, auto start/stop | CHANGED | CHANGED | no instance start; `mtl_session_start/stop`, `mtl_session_open` (create + start) |
| U-006 | Abort from a signal handler | PARTIAL | PARTIAL | `mtl_instance_interrupt(mt, 1)` (AS) wakes data waits; no device-abort verb |
| U-007 | Static source IP per port | COVERED | COVERED | `mtl_port_spec.sip` |
| U-008 | Netmask per port | COVERED | COVERED | `mtl_port_spec.prefix_len` |
| U-009 | Gateway per port | COVERED | COVERED | `mtl_port_spec.gateway` |
| U-010 | DHCP on a DPDK port | PARTIAL | COVERED | `MTL_OPT_DHCP` ("port.dhcp"); kernel backends: `sip` all zero |
| U-011 | TX/RX queue counts per port | COVERED | COVERED | `mtl_port_spec.tx_queues/rx_queues`, `MTL_OPT_MAX_QUEUES` |
| U-012 | Force a port's NUMA node | COVERED | COVERED | `mtl_port_spec.numa` |
| U-013 | Bring up with ports link-down | CHANGED | CHANGED | prose rule (09 §7.2); visible as `mtl_leg_status.oper`, `MTL_EVENT_PORT_LINK` |
| U-014 | ICE PF rl_burst_size devarg | NOT COVERED | COVERED | `MTL_OPT_RL_BURST` ("port.rl_burst") |
| U-015 | Restrict MTL to an lcore list | COVERED | COVERED | `mtl_instance_params.lcores` |
| U-016 | Choose the DPDK main lcore | NOT COVERED | COVERED | `MTL_OPT_MAIN_LCORE` |
| U-017 | Log level at init | COVERED | CHANGED | option `MTL_OPT_LOG_LEVEL` (`enum mtl_log_level`, renumbered from 1) instead of a field |
| U-018 | Change/read the log level at runtime | NOT COVERED | COVERED | `mtl_set_option` / `mtl_get_option` with `MTL_OPT_LOG_LEVEL` (R) |
| U-019 | Route MTL logs into the app's logger | NOT COVERED | COVERED | `mtl_log_set_sink(fn, user, prefix)` (mtl_observe.h) |
| U-020 | Custom log line prefix | NOT COVERED | CHANGED | static `prefix` argument of `mtl_log_set_sink`; a dynamic prefix is formatted in the sink |
| U-021 | Log to a FILE stream | NOT COVERED | CHANGED | the sink writes to the app's FILE |
| U-022 | Periodic stats dump with a user callback | CHANGED | PARTIAL | exporter loop over `mtl_stat_list/read` (CHANGED); the library's own dump period (`dump_period_s`) has no option. r3: same surface |
| U-023 | Force/query IOVA mode | PARTIAL | COVERED | `MTL_OPT_IOVA_MODE` (`MTL_IOVA_VA/PA`); query via `mtl_get_option` or key `caps.iova_va` (K) |
| U-024 | RSS mode; query it | NOT COVERED | PARTIAL | `MTL_OPT_RSS_MODE`; `enum mtl_rss` lacks legacy `MTL_RSS_MODE_L3` and adds `L3_L4_DST_PORT` (no legacy value) |
| U-025 | Schedulers for RSS dispatch (rss_sch_nb) | NOT COVERED | NOT COVERED | no option |
| U-026 | NIC descriptor ring sizes | NOT COVERED | COVERED | `MTL_OPT_TX_DESC`, `MTL_OPT_RX_DESC` (scope = port) |
| U-027 | RX mempool data room size | NOT COVERED | COVERED | `MTL_OPT_RX_POOL_DATA_SIZE` |
| U-028 | DPDK memzone limit | NOT COVERED | COVERED | `MTL_OPT_MEMZONE_MAX` |
| U-029 | Maximum UDP payload size | NOT COVERED | CHANGED | per session: `MTL_OPT_MAX_UDP_PAYLOAD`; packet units `packet.slot_bytes` |
| U-030 | ARP cache timeout | NOT COVERED | COVERED | `MTL_OPT_ARP_TIMEOUT_S` |
| U-031 | Name the DMA devices | NOT COVERED | COVERED | `MTL_OPT_DMA_DEVICES` ("instance.dma", str) |
| U-032 | Deprecated session-count sizing | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-033 | Instance fixed/varying info, session counts | COVERED | COVERED | keys `instance.*`, `instance.sessions{essence,dir}` (K); `mtl_instance_list_sessions`; `mtl_session_info.essence` |
| U-034 | Library version string | PARTIAL | COVERED | `mtl_version_string()` (mtl_observe.h), `mtl_version_num()` |
| U-035 | Is MtlManager alive | COVERED | COVERED | key `instance.manager` (K); `MTL_EVENT_MANAGER_LOST` |
| U-036 | Survive MtlManager loss | COVERED | COVERED | `MTL_INSTANCE_MANAGER_OPTIONAL`, `MTL_EVENT_MANAGER_LOST`, `MTL_REASON_MANAGER_LOST` |
| U-040 | Built-in PTP client | COVERED | COVERED | `time_source = MTL_TIME_SOURCE_PTP_BUILTIN` (duplicate flag of r3 removed) |
| U-041 | PTP PI servo and gains | NOT COVERED | COVERED | `MTL_OPT_PTP_PI` (scope 1/2 = kp/ki x 1e-9) |
| U-042 | PTP unicast delay request | NOT COVERED | CHANGED | `MTL_OPT_PTP_UNICAST` is a string address; legacy flag is a bool (unicast to the learned master): the "" / learned-master value is unstated |
| U-043 | Built-in phc2sys | NOT COVERED | COVERED | `MTL_OPT_PHC2SYS` |
| U-044 | PTP time from TSC | NOT COVERED | NOT COVERED | no time source or option |
| U-045 | App-supplied time function | CHANGED | CHANGED | `MTL_TIME_SOURCE_USER` + `mtl_time_user_update` (pushed pairs); `CLOCK_TAI`, `SYSTEM_TAI` |
| U-046 | Notify on each PTP sync | COVERED | COVERED | `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_STEP`, `MTL_SUB_TIME`; keys `time.offset_ns`, `time.utc_offset_s` (K) |
| U-047 | HW RX timestamps | COVERED | COVERED | `MTL_INSTANCE_HW_TIMESTAMP`, `MTL_OPT_HW_TIMESTAMPS`, `MTL_RXF_HW_ARRIVAL`, `MTL_PKT_HW_ARRIVAL` |
| U-048 | Read current PTP/TAI time | COVERED | COVERED | `mtl_time_now`, `mtl_time_cross`, `mtl_time_convert` |
| U-050 | Schedulers as pthreads | COVERED | COVERED | `MTL_INSTANCE_TASKLET_THREAD` |
| U-051 | Schedulers sleep when idle | COVERED | COVERED | `MTL_INSTANCE_TASKLET_SLEEP` |
| U-052 | Toggle sleep per scheduler at runtime | NOT COVERED | PARTIAL | `MTL_OPT_SCHED_SLEEP_US` scope = scheduler + 1 (R); whether 0 disables sleep is unstated |
| U-053 | Maximum scheduler sleep | NOT COVERED | COVERED | `MTL_OPT_SCHED_SLEEP_US` |
| U-054 | Separate lcore for RX video | PARTIAL | COVERED | `MTL_OPT_RX_SEPARATE_VIDEO_LCORE` |
| U-055 | Migrate busy video sessions; opt out | CHANGED | CHANGED | `MTL_OPT_TX_VIDEO_MIGRATE`, `MTL_OPT_RX_VIDEO_MIGRATE`; unified sessions never migrate, so DISABLE_MIGRATE is moot |
| U-056 | Data quota per scheduler | COVERED | COVERED | `MTL_OPT_SCHED_QUOTA_MBS` |
| U-057 | Audio sessions per scheduler cap | NOT COVERED | COVERED | `MTL_OPT_SCHED_AUDIO_MAX` (scope 0 TX, 1 RX) |
| U-058 | Tasklets per scheduler | NOT COVERED | COVERED | `MTL_OPT_TASKLETS_PER_SCHED` |
| U-059 | Dedicated system lcore | NOT COVERED | COVERED | `MTL_OPT_SYS_LCORE` = `MTL_SYS_DEDICATED` |
| U-060 | CNI on a thread vs a tasklet | NOT COVERED | COVERED | `MTL_OPT_CNI` (`MTL_CNI_THREAD/TASKLET`) |
| U-061 | Tasklet time measurement | COVERED | COVERED | `MTL_OPT_TASKLET_TIME_MEASURE`; key `sched.tasklet_p9999_ns` (K) |
| U-062 | Bind threads to NIC NUMA or not | PARTIAL | COVERED | default bound; opt out `MTL_OPT_NO_BIND_NUMA` (fixes r3's inverted flag) |
| U-063 | Do not bind the process to NIC NUMA | NOT COVERED | COVERED | `MTL_OPT_NO_BIND_PROCESS_NUMA` |
| U-064 | Allow cores across NUMA nodes | NOT COVERED | COVERED | `MTL_OPT_ACROSS_NUMA_CORES` |
| U-065 | 512-bit SIMD burst | NOT COVERED | COVERED | `MTL_OPT_SIMD_512` |
| U-066 | Query CPU SIMD level | NOT COVERED | NOT COVERED | no key in the S5 §3.4 catalogue, no call (S9 M9's "instance stat key" does not exist) |
| U-067 | Borrow an MTL lcore | NOT COVERED | CUT-CANDIDATE | S9 CUT-5b; REVISION-4 M12 |
| U-068 | App-created scheduler | NOT COVERED | CUT-CANDIDATE | REVISION-4 M12 (user schedulers); r3's reserved `mtl_sch_run_once` dropped |
| U-069 | App tasklet on an MTL scheduler | NOT COVERED | CUT-CANDIDATE | S9 H-08 / REVISION-4 M12 |
| U-070 | Session's scheduler index | COVERED | COVERED | `mtl_session_info.sched_index` |
| U-071 | Name a thread | NOT COVERED | CUT-CANDIDATE | S9 CUT-8; REVISION-4 M12 |
| U-072 | Legacy shm lcore allocator print/clean | NOT COVERED | CUT-CANDIDATE | S1 §6, S9 X2 (internal admin tool) |
| U-073 | Sleep / busy-delay helpers | NOT COVERED | CUT-CANDIDATE | S9 CUT-8; REVISION-4 M12 |
| U-075 | Shared TX queue | COVERED | COVERED | session `MTL_OPT_TX_QUEUE` = `MTL_TXQ_SHARED`; port `MTL_OPT_SHARED_TX_QUEUE` |
| U-076 | Shared RX queue | NOT COVERED | COVERED | `MTL_OPT_SHARED_RX_QUEUE` |
| U-077 | RX through the CNI queue | NOT COVERED | COVERED | `MTL_OPT_RX_USE_CNI` |
| U-078 | Flow rules on UDP port only | NOT COVERED | COVERED | `MTL_OPT_RX_UDP_PORT_ONLY` |
| U-079 | No system RX queues | NOT COVERED | COVERED | `MTL_OPT_NO_SYSTEM_RX_QUEUES` |
| U-080 | Promiscuous RX | NOT COVERED | COVERED | `MTL_OPT_PROMISCUOUS` |
| U-081 | No IGMP join | NOT COVERED | COVERED | `MTL_OPT_NO_MULTICAST` ("port.no_igmp") |
| U-082 | virtio_user exception path | NOT COVERED | COVERED | `MTL_OPT_VIRTIO_USER` |
| U-083 | AF_XDP copy mode only | NOT COVERED | COVERED | `MTL_OPT_AF_XDP_COPY` |
| U-084 | Mono RX/TX mempool | NOT COVERED | COVERED | `MTL_OPT_MONO_POOL` (scope 0 TX, 1 RX; see inconsistency I-3) |
| U-085 | Force TX copy (no chained mbuf) | NOT COVERED | CHANGED | per session `MTL_OPT_TX_COPY` instead of instance-wide |
| U-086 | Skip TX burst packet check | NOT COVERED | COVERED | `MTL_OPT_TX_NO_BURST_CHECK` |
| U-087 | Random / multiple UDP source ports | COVERED | COVERED | `MTL_OPT_SRC_PORT_MODE` (`MTL_SRC_PORT_RANDOM/MULTI`) |
| U-088 | Pacing engine (AUTO/RL/TSC/TSN/PTP/BE/TSC_NARROW) | PARTIAL | CHANGED | per session `MTL_OPT_PACING` (`MTL_PACING_HW_RATE/HW_LAUNCH/SW/SW_NARROW/PTP/BEST_EFFORT`), `MTL_OPT_PACING_REQUIRED`; no instance default; TSN to HW_LAUNCH mapping unstated |
| U-089 | UDP transport remnants | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-090 | Simulated loss on redundant TX | DEBUG | COVERED | debug: `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` per leg |
| U-092 | DPDK PMD port (PF or VF) | COVERED | COVERED | `mtl_port_spec.name` = PCI BDF |
| U-093 | Native AF_XDP port | COVERED | COVERED | name prefix `native_af_xdp:` |
| U-094 | Kernel socket port | COVERED | COVERED | name prefix `kernel:` |
| U-095 | DPDK AF_XDP / AF_PACKET PMDs | COVERED | CUT-CANDIDATE | r3 `MTL_BACKEND_DPDK_AF_XDP/AF_PACKET`; r4 has no name prefix and no enum; listed in REVISION-4 M12. REGRESSION R-2 |
| U-096 | Backend type by name; DPDK or AF_XDP test | COVERED | PARTIAL | key `caps.backend` (K) after open, but `enum mtl_backend` is defined in no r4 header; no pre-open name parse. REGRESSION R-3 |
| U-097 | Port NUMA node | COVERED | COVERED | key `caps.numa` (K) |
| U-098 | Port IP/netmask/gateway in use (DHCP lease) | NOT COVERED | NOT COVERED | `mtl_port_find` by IP only; no address getter or key |
| U-099 | Port I/O counters | PARTIAL | COVERED | keys `port.rx_pkts`, `tx_bytes`, `rx_errors`, `rx_missed`, `rx_nombuf`, ... (K) |
| U-100 | Reset port counters | CHANGED | CHANGED | cumulative only; deltas by the reader |
| U-101 | Interface IP helper | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-102 | Init-param setters for bindings | CHANGED | CHANGED | POD structs, `MTL_ADDR(T)`, spec string of `mtl_instance_open` |
| U-103 | Windows (DPDK only) | COVERED | COVERED | `mtl_session_get_wait_handle` returns an auto-reset event HANDLE; Linux errno values on every OS |
| U-104 | Multi-process via SR-IOV + MtlManager | COVERED | COVERED | manager model unchanged; `MTL_INSTANCE_MANAGER_OPTIONAL` |
| U-110 | Hugepage alloc/zalloc/free | CHANGED | CHANGED | `mtl_mem_alloc` (region + va), `mtl_mem_destroy`; no malloc-style pointer API |
| U-111 | IOVA of memory or of a frame plane | NOT COVERED | NOT COVERED | `mtl_plane`/`mtl_mem_info` carry no IOVA (only `mtl_plugin_plane.iova` for plugins) |
| U-112 | DMA-map app memory | COVERED | COVERED | `mtl_mem_import`, `MTL_MEM_MAP_ALL`, `mtl_mem_destroy` |
| U-113 | Page-aligned DMA-mapped block | PARTIAL | PARTIAL | `mtl_mem_alloc`; no IOVA getter (U-111) |
| U-114 | Page size and alignment | COVERED | COVERED | `mtl_mem_info.page_size`, key `caps.page_size`, `caps.hugepage_sizes` (K); align helper not |
| U-115 | Library fast memcpy | PARTIAL | PARTIAL | `mtl_unit_copy_in/out` (mtl_util.h) for leased units only; a general `mtl_memcpy` is S9 CUT-8 / REVISION-4 M12 |
| U-116 | User DMA engine | NOT COVERED | CUT-CANDIDATE | S9 CUT-5; REVISION-4 M12 |
| U-117 | RX frames in GPU VRAM | NOT COVERED | LATER | `mtl_mem_import_device` (mtl_mem.h, `MTL_LATER`, Phases 4-6) |
| U-120 | Create / free a session | COVERED | COVERED | `mtl_session_create`, `mtl_session_close(s, timeout)`, `mtl_session_open` |
| U-121 | Session live from create | CHANGED | CHANGED | explicit `mtl_session_start` (`mtl_when` NOW/AT_TAI/AT_INDEX) and `mtl_session_stop` (DRAIN/FLUSH) |
| U-122 | TX destination IP + UDP per leg | COVERED | COVERED | `mtl_flow.ip`, `udp_port` |
| U-123 | RX group/source + UDP per leg | COVERED | COVERED | `mtl_flow.ip`, `udp_port` |
| U-124 | SSM source filter | COVERED | COVERED | `mtl_flow.source_filter` |
| U-125 | Bind a leg to a named port | COVERED | COVERED | `mtl_flow.port` (index + 1), `mtl_port_find` |
| U-126 | ST 2022-7 dual-leg TX/RX | COVERED | COVERED | `flows[1]`, `legs_disabled`, `mtl_leg_status`, `pkts_received[leg]` |
| U-127 | Payload type | COVERED | COVERED | `mtl_flow.payload_type` |
| U-128 | SSRC | COVERED | COVERED | `mtl_flow.ssrc` |
| U-129 | UDP source port | COVERED | COVERED | `mtl_flow.udp_src_port` |
| U-130 | Static destination MAC | COVERED | COVERED | `MTL_FLOW_USER_MAC` + `mtl_flow.dst_mac` |
| U-131 | Session name | COVERED | COVERED | `mtl_session_config.name` |
| U-132 | Callback private pointer | CHANGED | CHANGED | per-unit `cookie` only; session identity is `mtl_tx_result.session` / `mtl_event.origin` (r3 also had a config and buffer `user_cookie`). REGRESSION R-7 (minor) |
| U-133 | Per-session NUMA override | COVERED | COVERED | `MTL_OPT_NUMA` |
| U-134 | Dedicated TX queue for audio/ANC/fastmeta | COVERED | COVERED | `MTL_OPT_TX_QUEUE` = `MTL_TXQ_DEDICATED` |
| U-135 | Change TX destination at runtime | COVERED | COVERED | `mtl_session_update(..., MTL_UPDATE_FLOWS, &when)` |
| U-136 | Change RX source at runtime | COVERED | COVERED | `mtl_session_update(..., MTL_UPDATE_FLOWS, &when)` |
| U-137 | RX burst size | COVERED | COVERED | `MTL_OPT_RX_BURST` |
| U-138 | Number of frame buffers | COVERED | COVERED | `mtl_session_config.pool_count`; `mtl_session_info.max_count` |
| U-139 | App-managed flows; queue meta | NOT COVERED | CUT-CANDIDATE | REVISION-4 M12 (r3's reserved `mtl_open_ext` dropped) |
| U-140 | pcapng capture of RX packets | NOT COVERED | CHANGED | `mtl_session_capture` / `_capture_stop` + `MTL_EVENT_CAPTURE_DONE` (asynchronous; legacy `sync` mode not offered) |
| U-141 | RTCP retransmission ST20/ST22 | NOT COVERED | COVERED | `MTL_OPT_RTCP`, `_BUFFER_PKTS`, `_NACK_INTERVAL_US`, `_SEQ_BITMAP`, `_SEQ_SKIP`; RTCP stats keys not in the catalogue |
| U-142 | RTCP flags on ST30/40/41/st40p | NOT COVERED | CUT-CANDIDATE | S9 CUT-7; REVISION-4 M12 |
| U-143 | TX queue hang-detect timeout | NOT COVERED | NOT COVERED | no option (`MTL_OPT_CMD_ACK_TIMEOUT_NS` is a different timer) |
| U-144 | TX queue recovery / fatal notification | COVERED | COVERED | `MTL_EVENT_RECOVERY`, `MTL_EVENT_SESSION_STATE` to ERROR, `MTL_REASON_TX_QUEUE_FATAL/HANG` |
| U-145 | Create fails with a reason | COVERED | COVERED | negative `MTL_E*`, `mtl_last_error`, `enum mtl_reason` |
| U-150 | Library-paced TX on the epoch | COVERED | COVERED | `media_mode = MTL_MEDIA_AUTO` |
| U-151 | User pacing at a TAI time | COVERED | COVERED | `MTL_MEDIA_TAI` + `MTL_SUBMIT_NOT_BEFORE` + `unit.launch_tai_ns` |
| U-152 | Exact user pacing | COVERED | COVERED | `MTL_SUBMIT_EXACT` + `launch_tai_ns` (flagged non-compliant) |
| U-153 | User-chosen RTP timestamp | CHANGED | CHANGED | `MTL_SUBMIT_RTP_TS` + `unit.rtp`; needs INDEX or TAI (`MTL_REASON_RTP_TS_AUTO`); library-slot + app RTP = INDEX with `mtl_tx_next_slot` (undocumented recipe). Same as r3 |
| U-154 | RTP exactly on the epoch | COVERED | COVERED | the default |
| U-155 | Legacy default ST20 RTP (TX cursor) | CHANGED | CHANGED | not the default; `media_time_offset_ns` approximates it (wire-visible, M13) |
| U-156 | RTP delta / TROFF trim | COVERED | COVERED | `media_time_offset_ns`, `MTL_OPT_INDEX_OFFSET`, `video.troffset_ns` |
| U-157 | Drop frames handed over late | COVERED | COVERED | `MTL_OPT_LATE_POLICY` = `MTL_LATE_DROP` |
| U-158 | Late-frame notification | COVERED | COVERED | `MTL_TX_LATE/DROPPED`, `reason`, `margin_ns`; `mtl_tx_result_full.slots_skipped_before` |
| U-159 | Per-epoch vsync event | PARTIAL | COVERED | `MTL_OPT_EPOCH_TICK` + `MTL_EVENT_EPOCH_TICK` (r3's missing switch now exists) |
| U-160 | ST 2110-21 sender type | COVERED | COVERED | `video.sender_type`, `cvideo.sender_type` (`MTL_SENDER_N/NL/W`) |
| U-161 | Query TR offset, TRS, VRX | COVERED | CHANGED | keys `info.troffset_ns`, `info.trs_ps`, `info.vrx_full` (K); r3 had typed `mtl_session_info` fields; `min_submit_lead_ns` typed |
| U-162 | RL tuning: start VRX, pad, static pad, no bulk | COVERED | COVERED | `MTL_OPT_VIDEO_START_VRX`, `_PAD_INTERVAL`, `_STATIC_PAD_P`, `_DISABLE_BULK`, `MTL_OPT_CVIDEO_DISABLE_BULK` |
| U-163 | Next frame due / is a frame late | COVERED | COVERED | `mtl_tx_next_slot` (`mtl_slot_hint`), `mtl_index_at`, `mtl_epoch_index_at`, `mtl_tx_row_deadline` |
| U-170 | Blocking get with a timeout | COVERED | COVERED | `timeout_ns` on `mtl_tx_acquire`, `mtl_rx_dequeue` |
| U-171 | Wake a blocked getter | COVERED | COVERED | `mtl_session_interrupt(s, 1/0)` (`-MTL_ECANCELED`) |
| U-172 | Non-blocking poll | COVERED | COVERED | timeout 0, `-MTL_EAGAIN` |
| U-173 | Frame-available notification | COVERED | COVERED | `mtl_session_get_wait_handle`, `mtl_session_wait` |
| U-174 | Pipeline TX frame done with status | COVERED | COVERED | `MTL_SESSION_RESULTS` + `mtl_tx_reap` (`mtl_tx_result.status`) |
| U-175 | Session TX done by index | COVERED | COVERED | `mtl_tx_result.slot`, `mtl_tx_reap`, `mtl_queue_reap` |
| U-176 | Zero-hop callback on the tasklet | PARTIAL | LATER | `mtl_session_set_inline_notify` (mtl_queue.h, `MTL_LATER`); now: `MTL_OPT_WAKER` = `MTL_WAKER_DIRECT` |
| U-177 | Session event callback | COVERED | COVERED | `mtl_session_read_events`, `MTL_BIND_EVENTS`, `mtl_queue_read_events` |
| U-178 | Abort a got frame | COVERED | COVERED | `mtl_tx_release`, `mtl_rx_release` |
| U-180 | Pipeline TX loop | COVERED | COVERED | `mtl_tx_acquire` + `mtl_tx_submit`; `mtl_session_open` |
| U-181 | Session TX pull model | CHANGED | CHANGED | push: `mtl_tx_acquire` / `mtl_tx_submit`; slot hint `mtl_tx_next_slot` |
| U-182 | Session framebuffers by index | COVERED | COVERED | `mtl_session_get_slot`, `mtl_tx_acquire_slot`, `info.unit_bytes`, `pool_count` |
| U-183 | Pipeline framebuffer address/size | COVERED | COVERED | as U-182 |
| U-184 | Session TX app-owned frames per index | COVERED | COVERED | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` + `mtl_tx_acquire_slot` |
| U-185 | Pipeline TX app memory per frame (any address) | PARTIAL | LATER | attached slots (<= `max_count`) now; per-acquire layout `mtl_tx_acquire_layout` is `MTL_LATER` (r3 had `mtl_tx_acquire_dynamic` on the surface). REGRESSION R-5 (minor) |
| U-186 | Two-phase release of app memory | COVERED | COVERED | app memory always produces results (M3 of mtl_mem.h); `mtl_tx_hold` |
| U-187 | Interlaced TX | COVERED | COVERED | `MTL_INTERLACED`; parity from the media index |
| U-188 | Line padding / stride | COVERED | COVERED | `mtl_plane.stride`, `mtl_attach.stride[]` |
| U-189 | Split-forward TX tiles from RX | COVERED | COVERED | `mtl_session_get_pool_region` + `mtl_session_attach` + `unit.hold`, `mtl_tx_send_slot` (ex09) |
| U-190 | Per-frame user metadata | COVERED | COVERED | meta area `MTL_META_USER` (<= 1332 B, `tag`, `tag_version`) |
| U-191 | Slice-level TX | PARTIAL | CHANGED | `MTL_UNIT_ROWS`: resubmit the lease with a larger `used`; `mtl_tx_row_deadline`, `MTL_OPT_PROGRESSIVE_LATE` (push, not the pull callback) |
| U-192 | Packing BPM/GPM/GPM_SL | COVERED | COVERED | `video.packing` |
| U-193 | RFC 4175 transport formats | PARTIAL | COVERED | `enum mtl_video_format`: all 16 values (r3 had 11) |
| U-194 | Non-RFC 4175 transport (planar 10LE, V210) | NOT COVERED | COVERED | `MTL_YUV422P10LE_NONSTD`, `MTL_V210_NONSTD` (mtl_format.h), `MTL_INFO_NON_COMPLIANT` |
| U-195 | Resolution and frame rate | COVERED | COVERED | `mtl_raster`, `MTL_FPS_*`, `mtl_fps_rational` |
| U-196 | Frame size, pgroup, bandwidth helpers | PARTIAL | PARTIAL | `mtl_format_frame_bytes`, `mtl_video_bandwidth`, `mtl_session_requirements`; no pgroup helper |
| U-197 | Transport format names | PARTIAL | COVERED | `mtl_format_names`, `mtl_format_parse` |
| U-200 | Pipeline RX loop | COVERED | COVERED | `mtl_rx_dequeue` + `mtl_rx_release` |
| U-201 | Session RX push callback + put | CHANGED | CHANGED | `mtl_rx_dequeue` / `mtl_rx_release` (any thread, any order) |
| U-202 | Deliver incomplete frames | CHANGED | CHANGED | `MTL_OPT_RX_INCOMPLETE` DELIVER is the default (M13); DISCARD opt-in |
| U-203 | Frame integrity status | COVERED | COVERED | `unit.status` (`MTL_RX_COMPLETE/INCOMPLETE`), `MTL_UNIT_USED_REDUNDANCY` |
| U-204 | Packets per leg | COVERED | COVERED | `mtl_rx_get_detail` (`pkts_expected`, `pkts_received[]`, `pkts_recovered`), `mtl_rx_get_missing` |
| U-205 | First/last packet arrival TAI | COVERED | COVERED | `mtl_rx_detail.arrival_first_tai_ns[]`, `arrival_last_tai_ns[]` |
| U-206 | RX RTP timestamp and media time | COVERED | COVERED | `unit.rtp`, `unit.media_tai_ns`, `unit.media_index` |
| U-207 | Received byte counts per frame | PARTIAL | PARTIAL | `unit.used` is rows for video; no byte counts |
| U-208 | RX into fixed app buffers | COVERED | COVERED | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` |
| U-209 | RX into an app buffer chosen per frame | PARTIAL | LATER | `mtl_rx_provide` (`MTL_LATER`); now `MTL_SESSION_RX_BY_INDEX` |
| U-210 | Per-buffer app identity | COVERED | CHANGED | `unit.slot` (stable index); r3's buffer `user_cookie` is gone. REGRESSION R-8 (minor) |
| U-211 | RX copy offload to DMA | PARTIAL | COVERED | `MTL_OPT_DMA`, `MTL_OPT_DMA_DEVICES`, `MTL_PATH_DIRECT_DMA`, `pkts_dma` |
| U-212 | Auto-detect video format | PARTIAL | COVERED | `video.detect = MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT`, keys `rx.detected.*` (K); delivery before acceptance unstated |
| U-213 | Header split | NOT COVERED | CUT-CANDIDATE | S9 CUT-1; REVISION-4 M12 |
| U-214 | Timing parser in stats | COVERED | COVERED | `MTL_OPT_RX_TIMING_PARSER`, keys `tp.*` (K) |
| U-215 | Timing parser result per frame and port | NOT COVERED | PARTIAL | `mtl_rx_get_timing` (`mtl_rx_timing_result`); no leg argument, no cinst min, vrx avg, ipt, rtp delta, failed cause |
| U-216 | Timing parser pass thresholds | NOT COVERED | COVERED | keys `tp.pass.*` (K) |
| U-217 | Two RX threads above 40 Gbps | COVERED | COVERED | `MTL_OPT_RX_THREADS` |
| U-218 | Slice-level RX | PARTIAL | PARTIAL | `MTL_UNIT_ROWS` + `MTL_UNIT_PARTIAL` only: no call to wait for more rows of a held unit, no row step (r3 `mtl_rx_wait_progress`). REGRESSION R-1 |
| U-219 | App pgroup conversion on the tasklet | NOT COVERED | CUT-CANDIDATE | S9 CUT-2; REVISION-4 M12 |
| U-220 | Per-packet conversion in st20p | NOT COVERED | COVERED | `MTL_OPT_RX_CONVERT_PER_PACKET` |
| U-221 | RX user metadata | COVERED | COVERED | meta area `MTL_META_USER` |
| U-222 | First-packet time per frame | PARTIAL | COVERED | `mtl_rx_timing_result.fpt_ns` (parser on); else `arrival_first_tai_ns - media_tai_ns` |
| U-223 | Simulated RX packet loss | DEBUG | COVERED | debug: `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` (pattern; random rate not offered) |
| U-224 | Session RX framebuffer size/count | COVERED | COVERED | `info.unit_bytes`, `pool_count`, `mtl_session_get_slot` |
| U-230 | ST22 pipeline TX with a codec | COVERED | COVERED | `cvideo.codec`, `app_format`, `codestream_bytes`; `MTL_OPT_CVIDEO_QUALITY/THREADS/DEVICE` |
| U-231 | ST22 pipeline RX decoded | COVERED | COVERED | `mtl_cvideo_config` (RX) |
| U-232 | Codestream passthrough | COVERED | COVERED | `cvideo.app_format = 0`, `unit.used` |
| U-233 | CBR vs VBR bytes per frame | CHANGED | CHANGED | `cvideo.rate_mode` CBR default; `MTL_CVIDEO_VBR_MAX` = today (M13) |
| U-234 | JPEG-XS box headers off | NOT COVERED | CUT-CANDIDATE | S9 CUT-6; REVISION-4 M12 |
| U-235 | ST22 packetization mode | COVERED | CHANGED | no `pack_type`: codestream implied (r3 had the field). REGRESSION R-6 (minor) |
| U-236 | Session-level ST22 TX/RX | CHANGED | CHANGED | as U-181 / U-201 |
| U-237 | Interlaced ST22 | COVERED | COVERED | `cvideo.raster.scan` |
| U-238 | ST22 pipeline app memory TX/RX | PARTIAL | LATER | as U-185 / U-209 |
| U-239 | Deliver incomplete ST22 frames | COVERED | COVERED | `MTL_OPT_RX_INCOMPLETE` |
| U-240 | PCM8/16/24, AM824 | COVERED | COVERED | `enum mtl_audio_format` |
| U-241 | 48 / 96 / 44.1 kHz | COVERED | COVERED | `audio.sample_rate` |
| U-242 | Channel count | COVERED | COVERED | `audio.channels` |
| U-243 | Packet time incl. ST 2110-31 values | COVERED | COVERED | `audio.samples_per_packet` |
| U-244 | Pipeline audio get/put | COVERED | COVERED | `mtl_tx_acquire/submit` (`used` bytes), `mtl_rx_dequeue`, `mtl_tx_write` |
| U-245 | Buffer size for a frame duration | COVERED | COVERED | `audio.unit_samples`; `info.buffer_capacity_bytes`, `unit_samples` from `mtl_session_query` |
| U-246 | Audio size/time math | PARTIAL | PARTIAL | `mtl_session_query` grants sizes; no pure helpers |
| U-247 | Per-session pacing engine | COVERED | COVERED | `MTL_OPT_PACING` |
| U-248 | RL warm-up accuracy and offset | PARTIAL | PARTIAL | `MTL_OPT_AUDIO_RL_WARMUP` scope 0/1 reaches both, but in packets; legacy values are ns |
| U-249 | Pace in the builder | COVERED | COVERED | `MTL_OPT_AUDIO_BUILD_PACING` |
| U-250 | Builder-to-pacer FIFO | COVERED | CHANGED | `MTL_OPT_AUDIO_FIFO_MS` (ms, not packets) |
| U-251 | Audio user timestamp | COVERED | COVERED | `media_mode` TAI/INDEX + `MTL_SUBMIT_RTP_TS`; `MTL_OPT_AUDIO_ABSORB_SAMPLES` |
| U-252 | Audio user pacing | COVERED | COVERED | `MTL_MEDIA_TAI` + `MTL_SUBMIT_NOT_BEFORE` |
| U-253 | Lost packets read as silence | COVERED | COVERED | `MTL_OPT_RX_INCOMPLETE`, `MTL_OPT_RX_FILL` = `MTL_FILL_ZERO` (default) |
| U-254 | Audio RX timing parser | PARTIAL | PARTIAL | `MTL_OPT_RX_TIMING_PARSER` + keys `tp.dpvr_max_ns`, `ipt_max_ns`, `tsdf_max_ns` (K); no per-period result (`notify_timing_parser_result`) |
| U-255 | AM824 / AES3 subframe layouts | NOT COVERED | PARTIAL | `mtl_convert_am824_to_aes3` / `aes3_to_am824`; no subframe struct layouts |
| U-256 | Deprecated sample fields | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-257 | Audio RX arrival time | COVERED | COVERED | `mtl_rx_detail.arrival_first_tai_ns[]` |
| U-258 | Session-level audio TX/RX | CHANGED | CHANGED | as U-181 / U-201 |
| U-260 | ANC TX/RX packet table + UDW | COVERED | COVERED | meta area `MTL_META_ANC` + `mtl_anc_packet` + UDW in plane 0; `stream` byte layout (S + StreamNum) unstated |
| U-261 | ANC packets per frame | CHANGED | CHANGED | `MTL_OPT_ANC_MAX_PACKETS` (<= 255; legacy 20) |
| U-262 | UDW capacity | COVERED | COVERED | `anc.max_udw_bytes`, `info.buffer_capacity_bytes` |
| U-263 | One ANC packet per RTP packet | COVERED | COVERED | `MTL_OPT_ANC_SPLIT_BY_PACKET` |
| U-264 | RX interlace auto-detect off switch | COVERED | COVERED | `MTL_OPT_ANC_RX_DETECT` |
| U-265 | Interlaced ANC TX | COVERED | COVERED | `anc.video.scan` + index parity |
| U-266 | RX per-port seq loss, marker | PARTIAL | PARTIAL | `pkts_received[]`, `units_missing_before`, keys `leg.pkts_lost` (K); no `rtp_marker` and per-leg discontinuity on frame units |
| U-267 | ANC TX test mutations | DEBUG | COVERED | debug: `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` + `enum mtl_tx_mutation` |
| U-268 | RFC 8331 helpers | NOT COVERED | PARTIAL | `mtl_anc_udw_get/set`, `mtl_anc_parity`, `mtl_anc_checksum`; no parity check, RFC 8331 encode/decode, sizes |
| U-269 | Session-level ANC TX/RX | CHANGED | CHANGED | as U-181 / U-201 |
| U-270 | ANC user pacing / timestamp / exact | COVERED | COVERED | as U-151 ... U-153 |
| U-271 | ST40P FORCE_NUMA (unsupported) | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-272 | st40p_rx_ops.rtp_ring_size (unused) | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-280 | TX one data item per frame | COVERED | COVERED | `essence = MTL_FASTMETA`, `unit.used` |
| U-281 | TX DIT and K bit | COVERED | COVERED | `fastmeta.data_item_type`, `k_bit` |
| U-282 | RX filter on DIT / K | COVERED | COVERED | `MTL_FASTMETA_RX_MATCH_DIT/K` |
| U-283 | RX fast metadata | CHANGED | CHANGED | frame-level RX is new; raw path is U-345 |
| U-284 | Fastmeta user pacing / timestamp | COVERED | COVERED | `media_mode`, `MTL_SUBMIT_*` |
| U-285 | Fastmeta rate and interlace | COVERED | COVERED | `fastmeta.video` (fps, scan), `MTL_FASTMETA_FREE_RUNNING` |
| U-286 | Fastmeta stats, update, queue meta | COVERED | COVERED | stats keys (K), `mtl_session_update`; queue meta: U-139 |
| U-340 | TX app-built RTP video | NOT COVERED | COVERED | `unit = MTL_UNIT_PACKETS`, `mtl_packet_config` (`packets_per_unit`, `slot_bytes`, `set_fields`, `pacing`), `mtl_pkt_tx`, `MTL_SUBMIT_UNIT_END` |
| U-341 | RX raw RTP video (de-duplicated) | NOT COVERED | COVERED | `mtl_rx_dequeue` chunk, `mtl_pkt_rx`, `MTL_PKT_RX_NO_DEDUP`, `rx_ring_packets` |
| U-342 | ST22 RTP TX/RX | NOT COVERED | COVERED | `MTL_CVIDEO` + `MTL_UNIT_PACKETS`, `packets_per_unit` |
| U-343 | ST30 RTP TX/RX | NOT COVERED | COVERED | `MTL_AUDIO` + `MTL_UNIT_PACKETS` |
| U-344 | ST40 RTP TX/RX | NOT COVERED | COVERED | `MTL_ANC` + `MTL_UNIT_PACKETS` |
| U-345 | ST41 RTP TX/RX | NOT COVERED | COVERED | `MTL_FASTMETA` + `MTL_UNIT_PACKETS` |
| U-346 | ST 2022-6 and custom payloads | NOT COVERED | COVERED | `essence = MTL_RTP`, `mtl_rtp_config` (`clock_rate`, `profile`, `encoding`) |
| U-347 | RTP and payload header layouts | NOT COVERED | PARTIAL | `mtl_rtp_hdr` + accessors only; no RFC 4175 (ext seq, SRD), RFC 9134, RFC 8331 or ST 2110-41 header layouts |
| U-348 | Pixel-group bit layouts | NOT COVERED | NOT COVERED | none |
| U-349 | Packet size limits | NOT COVERED | PARTIAL | `slot_bytes` default (MTU - 28), `MTL_OPT_MAX_UDP_PAYLOAD`, `MTL_REASON_PKT_CONFIG`; no limit constants |
| U-360 | App layout to transport conversion | COVERED | COVERED | `video.app_format` + `video.format`, `mtl_format_convertible`, `info.direct`, `MTL_TXR_COPIED` |
| U-361 | App frame formats | PARTIAL | COVERED | `enum mtl_app_format`: all 23 legacy raw formats + NV12, RGBA; codestreams via `enum mtl_codec` + `rate_mode` |
| U-362 | CUSTOM8 passthrough | NOT COVERED | COVERED | `MTL_APP_YUV420_CUSTOM8`, `MTL_APP_YUV422_CUSTOM8` |
| U-363 | Zero-copy when formats match | COVERED | COVERED | `MTL_SESSION_REQUIRE_DIRECT`, `info.direct`, `MTL_PATH_DIRECT` |
| U-364 | Converter device for st20p | NOT COVERED | NOT COVERED | `MTL_OPT_CVIDEO_DEVICE` covers cvideo only |
| U-365 | Frame struct | COVERED | COVERED | `struct mtl_unit` + `mtl_tx_result` / `mtl_rx_detail` |
| U-366 | Buffer requirements before create | COVERED | COVERED | `mtl_session_query`, `mtl_session_requirements`, `mtl_format_frame_bytes` |
| U-367 | Standalone frame allocation | PARTIAL | CHANGED | `mtl_mem_alloc` + `mtl_session_attach`, or app memory + `mtl_format_frame_bytes` for `mtl_convert` |
| U-368 | Format helpers: planes, linesizes | PARTIAL | PARTIAL | `mtl_format_names` (planes), `mtl_format_frame_bytes` (plane bytes), `mtl_format_parse`; no per-plane linesize/rows without a session |
| U-369 | Software frame operations | NOT COVERED | PARTIAL | `mtl_convert` replaces `st_frame_convert`; downsample, field merge/split absent; `st_draw_logo` S9 CUT-8 |
| U-370 | Codec name to enum | PARTIAL | PARTIAL | `enum mtl_codec`; no parser or name helper |
| U-371 | SWIG-friendly helpers | CHANGED | CHANGED | POD structs, `MTL_ADDR(T)`, `mtl_flow_parse`, `mtl_session_config_parse` |
| U-380 | Register an ST22 encoder/decoder in-process | NOT COVERED | COVERED | `mtl_plugin_register` + `mtl_plugin_device` (`MTL_PLUGIN_ENCODER/DECODER`) |
| U-381 | Encoder/decoder worker loop | NOT COVERED | CHANGED | `mtl_plugin_host.get_work/put_work` with a timeout (ABI v2) |
| U-382 | Register a format converter | NOT COVERED | COVERED | `MTL_PLUGIN_CONVERTER` |
| U-383 | Load codec plugins from .so | NOT COVERED | CHANGED | `mtl_plugin_load`, `MTL_PLUGIN_ENTRY_SYMBOL`; no plugin list at open |
| U-384 | Plugin format capability masks | NOT COVERED | CHANGED | `mtl_plugin_format_pair` arrays |
| U-385 | Test-only plugin devices | NOT COVERED | PARTIAL | `mtl_plugin_register` (in-process) replaces them; `enum mtl_codec_device` has no TEST value to select one |
| U-390 | Frame rate enum / number / name | COVERED | COVERED | `mtl_fps_rational`, `mtl_fps_parse` (mtl_util.h) |
| U-391 | TAI to media clock and back | PARTIAL | COVERED | `mtl_media_ticks`, `mtl_media_tai` (mtl_sync.h) |
| U-392 | Sampling-rate constants | COVERED | COVERED | implicit (Hz numbers) |
| U-393 | Standalone colour conversion library | NOT COVERED | CHANGED | `mtl_convert(&desc)` (one call, optional DMA via `desc.mt`), `mtl_convert_am824_to_aes3`; per-pair functions S9 CUT-4 |
| U-400 | Session stats per port and family | COVERED | COVERED | `mtl_stat_list/find/read/get` keys `tx.*`, `rx.*`, `leg.*` (K) |
| U-401 | Reset session stats | CHANGED | CHANGED | cumulative only (D-20) |
| U-402 | Family-specific diagnostic counters | PARTIAL | PARTIAL | keys `rx.pkts_rejected{cause}` etc. (K); no field-by-field map of the ~35 `st20_rx_user_stats` fields |
| U-410 | Redundant-combined ST20 RX | COVERED | CHANGED | two-leg RX session; `st20rc` API itself S9 CUT-3 / M12 |
| U-411 | Deprecated sip_addr alias | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-412 | Reserved RX pacing/packing fields | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-413 | ST22 slice packetization | COVERED | CHANGED | no value to request it (r3 `MTL_CVIDEO_PACK_SLICE` = -ENOTSUP); never implemented. Part of R-6 |
| U-414 | Legacy callback guard pattern | CHANGED | CHANGED (note, not counted) | no callbacks; handle returned before start |
| U-415 | MTL_FLAG_BIND_NUMA (no effect) | see U-062 | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-416 | Plain mtl_udma_fill | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-417 | Raw GPU session field | NOT COVERED | CUT-CANDIDATE | S1 §6; REVISION-4 M12 |
| U-418 | Pipeline fb address getters (no users) | COVERED | COVERED | `mtl_session_get_slot` |

## 6. Row-by-row: S9 §3 session-only features (74 rows)

| # | Feature | r4 coverage | r4 symbol / note |
|---|---|---|---|
| V1 | Frame TX by index callback | COVERED | `mtl_tx_acquire/submit/reap` (U-181, push) |
| V2 | Frame RX callback + put_framebuff | COVERED | `mtl_rx_dequeue/release`; MXL `MTL_SESSION_RX_BY_INDEX` |
| V3 | Tasklet-context latency of V1/V2 | LATER | `mtl_session_set_inline_notify` (mtl_queue.h `MTL_LATER`); now `MTL_OPT_WAKER` |
| V4 | RTP level TX/RX | COVERED | `MTL_UNIT_PACKETS`, mtl_packet.h (U-340, U-341) |
| V5 | Slice TX / RX | PARTIAL | TX: rows resubmit + `mtl_tx_row_deadline`; RX: no wait-for-rows, no row step (R-1) |
| V6 | TX ext frame | COVERED | `mtl_session_attach` + `mtl_tx_acquire_slot`; per-acquire layout `MTL_LATER` (R-5) |
| V7 | RX static ext_frames[] | COVERED | `MTL_SESSION_POOL_ATTACHED` |
| V8 | RX dynamic query_ext_frame | LATER | `mtl_rx_provide` (`MTL_LATER`) |
| V9 | RX uframe_pg_callback | CUT-CANDIDATE | CUT-2 / M12; `MTL_OPT_RX_CONVERT_PER_PACKET` kept |
| V10 | Video auto-detect | COVERED | `video.detect`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*` (K) |
| V11 | Timing parser STAT | COVERED | `MTL_OPT_RX_TIMING_PARSER`, `tp.*` (K) |
| V12 | Timing parser META per frame | PARTIAL | `mtl_rx_get_timing` (fields and per-leg missing, U-215) |
| V13 | RX DMA offload | COVERED | `MTL_OPT_DMA`, `MTL_OPT_DMA_DEVICES`, `MTL_PATH_DIRECT_DMA` |
| V14 | Header split | CUT-CANDIDATE | CUT-1 / M12 |
| V15 | GPU VRAM frames | LATER | `mtl_mem_import_device` (`MTL_LATER`) |
| V16 | RTCP retransmission | COVERED | `MTL_OPT_RTCP*` (310-314) |
| V17 | Two RX threads | COVERED | `MTL_OPT_RX_THREADS` |
| V18 | User pacing / exact / user timestamp / epoch RTP / delta | COVERED | media modes, `MTL_SUBMIT_NOT_BEFORE/EXACT/RTP_TS`, `media_time_offset_ns` |
| V19 | VSYNC | COVERED | `MTL_OPT_EPOCH_TICK` + `MTL_EVENT_EPOCH_TICK` |
| V20 | notify_frame_late | COVERED | `mtl_tx_result` status/reason/margin |
| V21 | update_destination / update_source | COVERED | `mtl_session_update(MTL_UPDATE_FLOWS)` |
| V22 | get_sch_idx, get_pacing_params | COVERED | `info.sched_index`; keys `info.trs_ps`, `troffset_ns`, `vrx_full` (K) |
| V23 | Queue meta + DATA_PATH_ONLY | CUT-CANDIDATE | M12 |
| V24 | pcapng dump | COVERED | `mtl_session_capture` (async) |
| V25 | Stats get/reset | CHANGED | stats registry; no reset |
| V26 | Static pad, bulk, NUMA, user MAC, migrate, burst, loss | COVERED | `MTL_OPT_VIDEO_*`, `MTL_OPT_NUMA`, `MTL_FLOW_USER_MAC`, `MTL_OPT_RX_BURST`, `MTL_OPT_*_VIDEO_MIGRATE`, `mtl_debug_inject` |
| V27 | Linesize, user_meta | COVERED | `mtl_plane.stride`, `MTL_META_USER` |
| V28 | Format helpers (pgroup, size, bandwidth, names) | PARTIAL | `mtl_format_frame_bytes`, `mtl_video_bandwidth`, `mtl_format_names/parse`; no pgroup |
| V29 | Transport-format set | COVERED | 16 RFC 4175 values + 2 `_NONSTD` |
| C1 | Codestream frame level | COVERED | `cvideo.app_format = 0` |
| C2 | ST22 RTP level | COVERED | `MTL_CVIDEO` + `MTL_UNIT_PACKETS` |
| C3 | DISABLE_BOXES | CUT-CANDIDATE | CUT-6 / M12 |
| C4 | ST22_PACK_SLICE | CHANGED | no `pack_type`, slice not expressible (R-6) |
| C5 | RTCP, pcapng, queue meta, VSYNC, update for ST22 | COVERED | as V16, V24, V19, V21; queue meta CUT-CANDIDATE |
| A1 | Frame-level audio sessions | COVERED | `MTL_AUDIO`, `mtl_tx_write` |
| A2 | Audio RTP level | COVERED | `MTL_UNIT_PACKETS` |
| A3 | Pacing way, RL warm-up, FIFO | PARTIAL | `MTL_OPT_PACING`, `MTL_OPT_AUDIO_FIFO_MS`; RL warm-up in packets, legacy ns (U-248) |
| A4 | Audio timing parser STAT/META | PARTIAL | STAT via `tp.*` (K); no per-period META |
| A5 | Audio size helpers | COVERED | `mtl_session_query` grants `unit_bytes`, `buffer_capacity_bytes` (no pure helper, U-246) |
| A6 | Audio RTCP flags | CUT-CANDIDATE | CUT-7 / M12 |
| A7 | AM824/AES3 conversion | COVERED | `mtl_convert_am824_to_aes3`, `mtl_convert_aes3_to_am824` |
| N1 | Frame-level ANC sessions | COVERED | `MTL_ANC`, `MTL_META_ANC`, `MTL_OPT_ANC_MAX_PACKETS` |
| N2 | ANC RTP level | COVERED | `MTL_UNIT_PACKETS` |
| N3 | RFC 8331 / UDW helpers | PARTIAL | `mtl_anc_udw_get/set`, `mtl_anc_parity`, `mtl_anc_checksum`; no parity check or encode/decode |
| N4 | Test mutation config | COVERED | `MTL_FAULT_TX_MUTATE` (debug) |
| N5 | Split by packet, EXACT, DEDICATE_QUEUE | COVERED | `MTL_OPT_ANC_SPLIT_BY_PACKET`, `MTL_SUBMIT_EXACT`, `MTL_OPT_TX_QUEUE` |
| N6 | ANC RTCP flags | CUT-CANDIDATE | CUT-7 / M12 |
| F1 | Fastmeta TX | COVERED | `MTL_FASTMETA` |
| F2 | Fastmeta RX | COVERED | frame RX + `MTL_UNIT_PACKETS` |
| F3 | DIT/K filters | COVERED | `MTL_FASTMETA_RX_MATCH_DIT/K` |
| F4 | Fastmeta RTCP flags | CUT-CANDIDATE | CUT-7 / M12 |
| S1 | st_fps and fps helpers | COVERED | `mtl_fps_rational`, `mtl_fps_parse` |
| S2 | Media-clock helpers | COVERED | `mtl_media_ticks`, `mtl_media_tai` |
| S3 | struct st_frame, st_ext_frame | COVERED | `struct mtl_unit`, `mtl_attach` |
| S4 | st_frame_fmt | COVERED | `enum mtl_app_format` (25), `enum mtl_codec` |
| S5 | st_frame_* helpers | PARTIAL | no per-plane linesize/rows helper (U-368) |
| S6 | st_draw_logo | CUT-CANDIDATE | CUT-8 / M12 |
| S7 | st_name_to_codec | PARTIAL | no codec parser (U-370) |
| S8 | SWIG setters | COVERED | `mtl_flow_parse`, `mtl_port_spec`, spec strings |
| S9 | Port/user stats structs, st_var_info | COVERED | stats keys (K) |
| S10 | RFC 3550 header struct | PARTIAL | `mtl_rtp_hdr`; payload headers missing (U-347) |
| M1 | Log hooks | COVERED | `mtl_log_set_sink`, `MTL_OPT_LOG_LEVEL` |
| M2 | Lcore borrow | CUT-CANDIDATE | CUT-5b / M12 |
| M3 | User DMA | CUT-CANDIDATE | CUT-5 / M12 |
| M4 | Hugepage and DMA memory | COVERED | `mtl_mem_alloc`, `mtl_mem_import` |
| M5 | PTP read, time fn, sync notify | COVERED | `mtl_time_now`, `MTL_TIME_SOURCE_USER`, `MTL_EVENT_TIME_*` |
| M6 | sch_enable_sleep, sch_set_sleep_us | PARTIAL | `MTL_OPT_SCHED_SLEEP_US` per scheduler (R); on/off semantics unstated |
| M7 | memcpy, sleep, delay, setname, page_align | CUT-CANDIDATE | CUT-8 / M12 |
| M8 | Port/PMD queries, manager alive, port stats, abort | PARTIAL | keys `caps.*`, `port.*`, `instance.manager` (K); `enum mtl_backend` missing (R-3); abort as U-006 |
| M9 | SIMD level query | NOT COVERED | no key or call (U-066) |
| X1 | User schedulers and tasklets | CUT-CANDIDATE | M12; r3's reserved `mtl_sch_run_once` dropped |
| X2 | lcore shm print/clean | CUT-CANDIDATE | internal admin tool |
| X3 | Per-pair converters | COVERED | `mtl_convert` (per-pair public functions CUT-4) |
| X4 | st20rc redundant-combined RX | CUT-CANDIDATE | CUT-3 / M12; function by two-leg RX |

## 7. Row-by-row: S9 §7.1 pre-hide gaps

| ID | Gap | r4 coverage | r4 symbol / note |
|---|---|---|---|
| H-01 | RTP level, all essences; ST 2022-6 | COVERED | `MTL_UNIT_PACKETS`, `MTL_RTP`, mtl_packet.h |
| H-02 | Slice / progressive rows | PARTIAL | TX covered; RX has no wait-for-rows call or row step (R-1) |
| H-03 | Format parity | COVERED | 16 transport + 2 `_NONSTD`; 25 app formats |
| H-04 | Format helpers | PARTIAL | `mtl_format_parse`, `mtl_video_bandwidth`; no `mtl_codec_parse`, no pgroup |
| H-05 | RTCP video/cvideo | COVERED | `MTL_OPT_RTCP*` options (not a `next` block); RTCP stats keys not catalogued |
| H-06 | Video RX auto-detect | COVERED | `video.detect`, `MTL_EVENT_RX_FORMAT`; acceptance rule unstated |
| H-07 | Per-unit ST 2110-21 results + thresholds | PARTIAL | `mtl_rx_get_timing`; fields, per-leg, audio META missing; thresholds only as keys `tp.pass.*` (K) |
| H-08 | User tasklets | CUT-CANDIDATE | REVISION-4 M12 |
| H-09 | RX dynamic destination | LATER | `mtl_rx_provide` |
| H-10 | GPU VRAM | LATER | `mtl_mem_import_device` |
| H-11 | pcapng capture | COVERED | `mtl_session_capture` (no SYNC flag) |
| H-12 | Media-clock helpers | COVERED | `mtl_media_ticks`, `mtl_media_tai` |
| H-13 | Standalone conversion and essence utilities | PARTIAL | `mtl_convert`, AM824/AES3, ANC udw/parity/checksum; no parity check, RFC 8331 encode/decode |
| H-14 | Log sinks | COVERED | `mtl_log_set_sink` (prefix argument, not `instance_params.log_prefix`) |
| H-15 | Inline notify | LATER | `mtl_session_set_inline_notify` (mtl_queue.h `MTL_LATER`) |
| H-16 | Per-packet RX conversion | COVERED | `MTL_OPT_RX_CONVERT_PER_PACKET` (no `convert_context` report) |
| H-17 | Queue meta | CUT-CANDIDATE | REVISION-4 M12 |
| H-18 | Scheduler sleep interval | COVERED | `MTL_OPT_SCHED_SLEEP_US` |
| H-19 | Plugin ABI v2 | COVERED | mtl_plugin.h |

## 8. Re-check after the fixes

Headers re-read at 2026-10-01 11:45 UTC (last edit 11:41; `check.sh` OK, 125 functions + 6 `MTL_LATER`). Key names: `mtl_observe.h` + 08 §R4 ("R4 key catalogue") + S5 §3.4. Sections 1–7 above are left as written; where they disagree with this section, this section is current.

### 8.1 New counts

| Status | S1 rows (§5 → now) | S9 §3 rows (§6 → now) | H-01…H-19 (§7 → now) |
|---|---|---|---|
| COVERED | 186 → **212** | 43 → **54** | 10 → **14** |
| CHANGED | 43 → **44** | 2 → **2** | 0 → **0** |
| PARTIAL | 24 → **5** | 11 → **1** | 4 → **0** |
| LATER | 5 → **5** | 3 → **3** | 3 → **3** |
| NOT COVERED | 8 → **0** | 1 → **0** | 0 → **0** |
| CUT-CANDIDATE | 24 → **24** | 14 → **14** | 2 → **2** |
| Total | 290 | 74 | 19 |

S9 changes: V5, V12, V28, A3, N3, S5, S7, S10, M6 and M9 become COVERED; A4 becomes CHANGED (per-unit audio timing); C4 becomes COVERED (`MTL_OPT_CVIDEO_PACK`). M8 stays PARTIAL (device abort, U-006). H-02, H-04, H-07 and H-13 become COVERED.

### 8.2 The 8 NOT COVERED and 24 PARTIAL rows, and other rows that moved

| ID | Use case | Was | Now | Symbol / note |
|---|---|---|---|---|
| U-025 | Schedulers for RSS dispatch (rss_sch_nb) | NOT COVERED | COVERED | `MTL_OPT_RSS_SCHEDS` ("port.rss_scheds") |
| U-044 | PTP time from TSC | NOT COVERED | COVERED | `MTL_OPT_PTP_SOURCE_TSC` |
| U-066 | Query CPU SIMD level | NOT COVERED | COVERED | key `instance.simd_level` (08 §R4.3) + `enum mtl_simd_level` (mtl_observe.h) |
| U-098 | Port IP/netmask/gateway in use (DHCP lease) | NOT COVERED | COVERED | `mtl_port_get_spec` (address, prefix, gateway in use, DHCP lease included) |
| U-111 | IOVA of memory or of a frame plane | NOT COVERED | PARTIAL | `mtl_mem_iova(region, offset, &iova)`; a plane of a library pool has no known region offset (`mtl_mem_info` has no base address) |
| U-143 | TX queue hang-detect timeout | NOT COVERED | COVERED | `MTL_OPT_TX_HANG_DETECT_NS` |
| U-348 | Pixel-group bit layouts | NOT COVERED | PARTIAL | `mtl_format_pgroup` gives bytes and pixels per group; no bit layouts of the pixel groups |
| U-364 | Converter device for st20p | NOT COVERED | COVERED | `MTL_OPT_VIDEO_CONVERT_DEVICE` |
| U-006 | Abort from a signal handler | PARTIAL | PARTIAL | unchanged: `mtl_instance_interrupt(mt, 1)` only; no device-abort verb |
| U-022 | Periodic stats dump with a user callback | PARTIAL | COVERED | `MTL_OPT_STAT_DUMP_S` ("log.stat_dump_s", R) + exporter loop over `mtl_stat_list/read` |
| U-024 | RSS mode; query it | PARTIAL | COVERED | `MTL_OPT_RSS_MODE` with `MTL_RSS_NONE/L3/L3_L4` (legacy set) |
| U-052 | Toggle sleep per scheduler at runtime | PARTIAL | COVERED | `MTL_OPT_SCHED_SLEEP_US` per scheduler, "0 = never", R |
| U-096 | Backend type by name; DPDK or AF_XDP test | PARTIAL | CHANGED | key `caps.backend` + `enum mtl_backend` (mtl_observe.h) after open; before open the name prefix says it |
| U-113 | Page-aligned DMA-mapped block | PARTIAL | COVERED | `mtl_mem_alloc` + `mtl_mem_iova` |
| U-115 | Library fast memcpy | PARTIAL | PARTIAL | unchanged: `mtl_unit_copy_in/out` only; `mtl_memcpy` is on M12 (CUT-8) |
| U-196 | Frame size, pgroup, bandwidth helpers | PARTIAL | COVERED | `mtl_format_frame_bytes`, `mtl_format_pgroup`, `mtl_video_bandwidth`, `mtl_session_query(..., req)` |
| U-207 | Received byte counts per frame | PARTIAL | COVERED | `mtl_rx_detail.bytes_received` |
| U-215 | Timing parser result per frame and port | PARTIAL | COVERED | `mtl_rx_get_timing(s, lease, leg, ...)`: compliance, `failed_cause`, cinst min/max/avg, vrx min/max/avg, fpt, latency, rtp offset/delta, ipt max |
| U-218 | Slice-level RX | PARTIAL | CHANGED | dequeue at the first rows with `MTL_UNITF_PARTIAL`, then `mtl_rx_wait_rows` (mtl_sync.h); step `MTL_OPT_RX_ROWS_STEP` (pull of a growing unit instead of a slice callback) |
| U-246 | Audio size/time math | PARTIAL | COVERED | `mtl_audio_bytes` (mtl_format.h), `mtl_session_query` |
| U-248 | RL warm-up accuracy and offset | PARTIAL | COVERED | `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` |
| U-254 | Audio RX timing parser | PARTIAL | CHANGED | `MTL_OPT_RX_TIMING_PARSER`; per unit `mtl_rx_get_timing` (`dpvr_max_ns`, `ipt_max_ns`, `tsdf_ns`) and `tp.*` keys, instead of a 200 ms callback |
| U-255 | AM824 / AES3 subframe layouts | PARTIAL | COVERED | `struct mtl_am824_subframe` + `mtl_convert_am824_to_aes3/aes3_to_am824` |
| U-266 | RX per-port seq loss, marker | PARTIAL | COVERED | `mtl_rx_detail.marker_seen`, `seq_discont[leg]`, `pkts_received[leg]`, `leg.pkts_lost` |
| U-268 | RFC 8331 helpers | PARTIAL | COVERED | `mtl_anc_udw_get/set`, `mtl_anc_parity`, `mtl_anc_parity_ok`, `mtl_anc_checksum`, `mtl_anc_rfc8331_encode/decode` |
| U-347 | RTP and payload header layouts | PARTIAL | COVERED | `mtl_rtp_hdr`, `mtl_rfc4175_hdr`, `mtl_rfc4175_srd`, `mtl_rfc9134_hdr`, `mtl_rfc8331_hdr`, `mtl_st41_hdr` (mtl_packet.h) |
| U-349 | Packet size limits | PARTIAL | CHANGED | `MTL_UDP_HDR_BYTES`, `MTL_IPV4_HDR_BYTES` + rule "MTU minus IP and UDP"; no fixed `MTL_PKT_MAX_RTP_BYTES` |
| U-368 | Format helpers: planes, linesizes | PARTIAL | COVERED | `mtl_format_plane`, `mtl_format_names`, `mtl_format_frame_bytes`, `mtl_format_parse` |
| U-369 | Software frame operations | PARTIAL | COVERED | `mtl_convert` with `MTL_CONVERT_FIELD_SPLIT/FIELD_MERGE/HALF_SCALE`; `st_draw_logo` on M12 |
| U-370 | Codec name to enum | PARTIAL | COVERED | `mtl_codec_parse`, `mtl_codec_name` |
| U-385 | Test-only plugin devices | PARTIAL | COVERED | `MTL_CODEC_DEVICE_TEST` + `mtl_plugin_register` |
| U-402 | Family-specific diagnostic counters | PARTIAL | PARTIAL | keys + 08 §R4.4 maps 8 legacy fields; the other ~27 `st20_rx_user_stats` fields are unmapped |
| U-235 | ST22 packetization mode | CHANGED | COVERED | `MTL_OPT_CVIDEO_PACK` (1 codestream; 2 slice "not yet") |
| U-413 | ST22 slice packetization | CHANGED | COVERED | `MTL_OPT_CVIDEO_PACK` = 2 (slice, not yet) |
| U-042 | PTP unicast delay request | CHANGED | COVERED | `MTL_OPT_PTP_UNICAST` bool (delay requests to the master) |
| U-068 | App-created scheduler | CUT-CANDIDATE | CUT-CANDIDATE | M12; manual progress `mtl_sched_run_once` reserved again (mtl_util.h `MTL_LATER`) |
| U-139 | App-managed flows; queue meta | CUT-CANDIDATE | CUT-CANDIDATE | M12; route `mtl_open_ext` reserved again (mtl_util.h `MTL_LATER`) |

### 8.3 Regressions R-1…R-10

| # | Now | Symbol / note |
|---|---|---|
| R-1 | FIXED | `mtl_rx_wait_rows` (mtl_sync.h), `MTL_OPT_RX_ROWS_STEP`, `MTL_UNITF_PARTIAL` |
| R-2 | OPEN (approval) | U-095 still a cut; the `mtl_port_spec.name` comment now names `dpdk_af_xdp:` / `dpdk_af_packet:` as on the M12 removal list |
| R-3 | FIXED | `enum mtl_backend` in mtl_observe.h, key `caps.backend` (08 §R4.3) |
| R-4 | FIXED (residual I-1r) | 08 §R4 is the normative catalogue and `mtl_observe.h` points at "08 §R4"; the `info.*` row still defers to "the 37 names listed in P3" of S5 |
| R-5 | OPEN (approval) | `mtl_tx_acquire_layout` still `MTL_LATER`; not yet recorded in M12 or the phase plan |
| R-6 | FIXED | `MTL_OPT_CVIDEO_PACK` (1 codestream, 2 slice not yet) |
| R-7 | OPEN (minor) | U-132: still only `unit.cookie`; no session-level cookie |
| R-8 | ACCEPTED | U-210: slot index is the identity (D-76); nothing to change |
| R-9 | PARTLY FIXED | `mtl_sched_run_once` reserved again (mtl_util.h `MTL_LATER`); user tasklets remain an M12 cut |
| R-10 | FIXED (reserve) | `mtl_open_ext(mt, name, version, ops, ops_size)` reserved again (mtl_util.h `MTL_LATER`); queue meta itself remains an M12 decision |

### 8.4 Inconsistencies I-1…I-16

| # | Now | Symbol / note |
|---|---|---|
| I-1 | FIXED (residual) | 08 §R4; residual: `info.*` names are not listed in 08 itself (I-1r) |
| I-2 | FIXED | `enum mtl_pacing`, `enum mtl_time_state`, `enum mtl_flow_state` in mtl.h; `enum mtl_backend` in mtl_observe.h; `pacing_class` says "enum mtl_pacing, granted" |
| I-3 | FIXED | `MTL_OPT_IOVA_MODE/MEMZONE_MAX/CNI` moved to 2017–2019; `MTL_OPT_TX_MONO_POOL` / `RX_MONO_POOL`; `MTL_OPT_SCHED_TX/RX_AUDIO_MAX`; `enum mtl_option_scope` + `mtl_option_desc.scope_kind` |
| I-4 | FIXED | `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` |
| I-5 | FIXED | `enum mtl_rss { NONE, L3, L3_L4 }` |
| I-6 | FIXED | `MTL_OPT_PTP_UNICAST` is a bool |
| I-7 | FIXED (residual) | R6 names the dispatch and log threads; residual: the `MTL_LATER` `mtl_session_set_inline_notify` runs "on the completing context", which R6's "never on an MTL tasklet" does not except (I-7r) |
| I-8 | FIXED (residual) | R2: every data call returns a count >= 1 or `-MTL_EAGAIN`, `mtl_session_wait` too; residual: `mtl_index_at` still uses `-MTL_EAGAIN` for "T0 unresolved" (I-8r) |
| I-9 | PARTLY FIXED | `MTL_OPT_RX_FILL` removed (flag `MTL_SESSION_RX_NO_FILL` only); still open: `MTL_OPT_TX_QUEUE`=SHARED vs port `MTL_OPT_SHARED_TX_QUEUE` precedence, and video `detect` field vs `MTL_OPT_ANC_RX_DETECT` (I-9r) |
| I-10 | FIXED | `MTL_OPT_LOG_LEVEL` is per instance ("this instance's lines"); the sink stays process-wide |
| I-11 | FIXED | mtl_options.h: "Option names and stats names are separate registries" |
| I-12 | FIXED (residual) | `mtl_mem_close`, `mtl_queue_close`, wait handle in the mtl_queue.h banner; residual: 08 key `instance.sessions_destroying` still uses the old word (I-12r) |
| I-13 | FIXED | `enum mtl_codec_device` "absent = any device", `MTL_CODEC_DEVICE_TEST` |
| I-14 | FIXED | `mtl_anc_packet.stream`: bit 7 = S, bits 0–6 = StreamNum |
| I-15 | FIXED | `MTL_OPT_ROWS_LATE` |
| I-16 | FIXED (residual) | REVISION-4.md now says 125 (+6); residual: its §4.2 says 15 gaps have a home and 2 are later, but H-15 (`mtl_session_set_inline_notify`) is also `MTL_LATER`: 14 + 3 + 2 cuts (I-16r) |

New, from the fixes: `MTL_OPT_CVIDEO_PACK` takes the bare values 1 and 2 while every other enumerated key has an enum (I-17). Nothing else new was found.

Symbols in §5 renamed since (no row lost coverage through a rename):

- `mtl_mem_destroy` → `mtl_mem_close`
- `mtl_tx_hold` → `mtl_tx_pin`
- `mtl_session_requirements` → `mtl_session_query(..., req, req_size)`
- `MTL_FLOW_USER_MAC` → `MTL_FLOWF_USER_MAC`
- `MTL_UNIT_*` RX flags → `MTL_UNITF_*`
- `MTL_PKT_HW_ARRIVAL` → `MTL_PKTE_HW_ARRIVAL`
- `MTL_OPT_PROGRESSIVE_LATE` → `MTL_OPT_ROWS_LATE`
- `MTL_OPT_MONO_POOL` → `MTL_OPT_TX/RX_MONO_POOL`
- `MTL_OPT_SCHED_AUDIO_MAX` → `MTL_OPT_SCHED_TX/RX_AUDIO_MAX`
- `MTL_OPT_AUDIO_RL_WARMUP` → `_RL_ACCURACY_NS` / `_RL_OFFSET_NS`
- `MTL_OPT_RX_FILL` removed

### 8.5 Still open, with one-line proposals

| Item | Proposal |
|---|---|
| U-006 device abort | mtl.h: `MTL_API_AS int mtl_instance_abort(mtl_instance_h mt);`, or record "interrupt is enough" in M12 |
| U-111 IOVA of a library-pool plane | mtl_mem.h: add `MTL_ADDR(void) va` (region base) to `struct mtl_mem_info`, so a plane's IOVA = `mtl_mem_iova(r, addr - va)` |
| U-115 general fast copy | decide CUT-8 in M12, or mtl_util.h `MTL_API_DPC int mtl_memcpy(void* dst, const void* src, size_t n);` |
| U-348 pixel-group bit layouts | add to M12 as internal (perf tools, tests), or mtl_format.h byte-layout structs for the RFC 4175 pgroups |
| U-402 legacy stats field map | complete 08 §R4.4 for every `st20/22/30/40/41_*_user_stats` field |
| R-2 / U-095 | decide in M12 (the comment already flags it) |
| R-5 / U-185, U-238 | add "per-acquire TX layouts (`mtl_tx_acquire_layout`) are Phase 4" to M12 or the phase plan |
| R-7 / U-132 | optional: `uint64_t user_cookie` in `mtl_session_config`, echoed in `mtl_session_info` and `mtl_event` |
| I-1r | list the `info.*` names in 08 §R4.2 instead of "the 37 names listed in P3" |
| I-7r | R6: "… except the `MTL_LATER` inline notify, which runs on the completing context under the DP rules" |
| I-8r | `mtl_index_at`: return `-MTL_EBUSY` (or a reason) for an unresolved T0, keeping `-MTL_EAGAIN` for data calls |
| I-9r | say which wins for a shared TX queue (session key over port key), and make ANC detect a typed field or video detect an option |
| I-12r | rename the key to `instance.sessions_closing` |
| I-16r | REVISION-4 §4.2: "14 have a home, 3 are in a named later phase (RX destinations, GPU memory, inline notify), 2 are removals" |
| I-17 | mtl_options.h: `enum mtl_cvideo_pack { MTL_CVIDEO_PACK_CODESTREAM = 1, MTL_CVIDEO_PACK_SLICE = 2 };` |

