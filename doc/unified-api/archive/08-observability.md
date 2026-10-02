# 08 — Observability: stats, status, info, names, capacity, tracing, logs

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-OBS-1…5, R-TIME-6, R-CAP-1, R-OPS-2…4 |
| Research | [R06 observability](research/06-observability.md) (primary), [R11 §8 SRT/WebRTC/OpenTelemetry](research/11-media-io-prior-art.md), [R03 §8](research/03-scheduler-threading.md); reviews [C1 #6, #14, #17](reviews/C1-realtime-feasibility.md), [C5 §2.2, §3.4, §8.7–8.12](reviews/C5-adversarial-user-review.md) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

The maintainer's requirement: *users must see what is going on — whether they provide
frames on time — see and react to every exception, and get stats.* The design gives each
fact exactly one home, of a declared kind. Items changed in revision 3 are marked **r3**.

| Kind | Home | Answers | Loss |
|---|---|---|---|
| **Per-unit result** | CQ record ([07 §1](07-completions-events-and-errors.md)) | "what happened to *this* frame / audio buffer?" | never (in modes that keep results) |
| **Event** | EQ ([07 §3](07-completions-events-and-errors.md)) | "what changed?" | coalesced, counted, resync via getter |
| **Status** | getters (§3) | "what is the state *now*?" (link, PTP, session, legs, `blocked_on`) | n/a |
| **Info** | `mtl_session_get_info` (§3.1) | "what was I granted, and what do I put in the SDP?" | n/a |
| **Counter** | stats snapshot | "how many since the session was created?" (r3: cumulative only) | n/a |
| **Gauge** | stats snapshot | "how much is queued / how full / how busy now?" | n/a |
| **Histogram** | stats snapshot | "how are margins, latency, tasklet times distributed?" | n/a |
| **Trace** | USDT (existing) | per-packet / per-decision debugging | n/a |
| **Log** | printer | humans only; never the only carrier of a fact | best effort |

## 1. The questions a live-stream user asks

| Question | Today `[R06 §2]` | New API |
|---|---|---|
| Was my frame sent on time? | `done` fires at mbuf free with no send time; pipeline status is always `COMPLETE` | TX result `status` (admission verdict) + `scheduled_first` / `enqueued_first` / `observed_first` (HW) |
| How early or late was I? | `notify_frame_late(epochs)` in three units, wrong `priv` on one pipeline path (SF-02, fix in open PR #1770) | `margin_ns` per unit; `tx.margin_hist`; slot hint at acquire |
| Why can I not acquire? | NULL for everything | `-MTL_EAGAIN` + `mtl_session_get_status().blocked_on` (`BUFFERS` / `RESULTS` / `RING` / `APP_LEASES`) + `BACKPRESSURE` event |
| How much am I buffering? | dbg-log histogram only | gauges per lease state, in units **and** ns of media queued ahead of the wire |
| Did a slot go empty? | epochs counted, not frames; some counters dead | `tx.slots_empty`, `TX_UNDERRUN` event, `slots_skipped_before` in the next result |
| Was a frame dropped, and why? | only with DROP_WHEN_LATE + USER_PACING + TAI | `DROPPED` + reason, always |
| Is my sender ST 2110-21 compliant? | not measured on TX | TX self-check: windowed `vrx_max_*` / `cinst_max_*` and cumulative `tpr_late_pkts` (06 §12; r3 windows, §2.2) |
| Is PTP locked, what offset, which grandmaster? | no getter; `locked` is sticky | `mtl_time_get_status`, `TIME_STATE` / `TIME_STEP` events |
| Is the link up? Which 2022-7 leg is alive, which is disabled? | link probed only at start | `mtl_port_get_status`; per leg **admin × oper** state in `mtl_session_get_status` (r3); `PORT_LINK`, `LEG_STATE`; link monitor from Phase 2 |
| Did my flow resolve (ARP) and join (IGMP)? | create blocks on ARP | per-leg flow state (`WAITING_NEIGHBOUR`, resolved, joined, join failed) in the status and the `FLOW_STATE` event (r3) |
| RX: late, out of order, duplicated, incomplete? | good for video, weak elsewhere | per-unit `status`, `pkts_*`, missing ranges, arrival per leg; per-leg counters with one definition for all essences |
| Is the RX stream ST 2110-21 compliant? | opt-in timing parser, per frame, trustworthy only with HW timestamps | **r3:** the parser's summary is in v1, opt-in per session (§2.7) |
| End-to-end latency? | app computes it | RX `latency_ns` per unit + histogram; TX `media → observed_first`; `completion_latency_ns`, `min_submit_lead_ns` and the latency range in `get_info` |
| Is my session starved of CPU? | `cpu_busy_score`, tasklet timing: log only | `mtl_sched_get_status` (busy %, loop time, max and p99.99 tasklet time), `SCHED_OVERLOAD` event |
| Did a copy / conversion path get used? | logged at create | granted path in `mtl_session_get_info`; per-unit `path` + `pkts_dma` / `pkts_copied_partial`; `caller_work_ns` for DPC calls |
| Did recovery happen, and did I lose frames? | in-flight frames reported as `COMPLETE` | `SESSION_RECOVERY` event (coalesced; `recoveries_ok` / `recoveries_failed` are the counts of record, r3); affected units `DROPPED/RECOVERY` |
| Which sessions exist in this process? (r3) | no enumeration; `stat_dump_cb_fn` only | `mtl_instance_list_sessions` + stable names (§7) |
| Can this host take 12 more 1080p59 streams? (r3) | no answer before trying | `mtl_port_get_capacity` + `MTL_QUERY_CHECK_CAPACITY` on the dry run (§8) |
| How much hugepage memory is left? (r3) | outside the API | `mtl_instance_get_mem_status` (per NUMA node) and `internal_bytes` per session ([15 §8](15-security-and-deployment.md)) |

## 2. Stats snapshot

### 2.1 Shape (r3)

One schema for every essence: a common header, a direction block, per-leg blocks, queue
gauges, and a **fixed-size** per-essence tail. Today each essence has its own struct,
ST22/ST22P have none, and fields have been renamed in place `[R02 §3.7, R13 §4]`.
Revision 2 located the essence tail by `media_offset` beyond `sizeof` and carried a
`version` field, which contradicted the `struct_size` rules `[C5 §2.20]`; both are gone.
Every block is fixed-size with a reserved tail and grows with the parent
([11 §2.1](11-abi-compatibility-and-migration.md)); reason arrays are sized by frozen
constants.

```c
struct mtl_session_stats {
  uint32_t struct_size;             /* input only: the library writes min(struct_size, native) bytes */
  uint32_t state;                   /* enum mtl_session_state at snapshot */
  uint32_t essence;
  uint32_t legs;
  char name[MTL_NAME_MAX];          /* the session name (§7) */
  struct mtl_time created;          /* every counter counts from here */
  struct mtl_time snapshot;         /* when the snapshot was taken */
  uint64_t unsupported_mask;        /* counters the backend cannot produce (not a silent 0) */
  union mtl_dir_stats dir;          /* tx | rx, fixed size + reserved tail */
  struct mtl_leg_stats leg[MTL_MAX_LEGS];
  struct mtl_queue_gauges queue;
  union mtl_media_stats media;      /* fixed-size per-essence tail: video, cvideo, audio, anc, fastmeta */
  uint64_t reserved[8];
};
int mtl_session_get_stats(mtl_session_h s, struct mtl_session_stats* st);
```

The header in `sketch/` is normative for the layout and the size check; this block
mirrors it.

### 2.2 Proposed fields (all u64; counters are cumulative for the life of the session, gauges marked)

| Block | Fields |
|---|---|
| TX units | `units_submitted`, `units_on_time`, `units_late`, `units_dropped[MTL_TX_REASON_SLOTS]` (by `enum mtl_tx_reason`), `units_flushed`, `units_withdrawn`, `units_before_start`, `units_failed`, `units_suppressed`¹, `slots_empty`, `units_repeated`, `units_padded` (keep-alive, silence, empty ANC) |
| TX work | `acquire_blocked_on_results`, `acquire_blocked_on_buffers`, `acquire_blocked_on_app_leases`, `build_overrun`, `recoveries_ok`², `recoveries_failed`, `bytes`, `pkts`, `pkts_copied_partial`, `caller_work_ns`, `cmd_timeouts` |
| TX self-check (06 §12) | **r3:** `vrx_max`, `cinst_max` (windowed gauges, `struct mtl_window_max`, §2.6); `tpr_late_pkts` (counter; per unit: `timing.max_packet_lateness_ns`, 06 §7.5); `vrx_hist`, `cinst_hist` (histograms) |
| video tail | `pkts_dma`, `pkts_copied_partial` |
| cvideo tail | `padding_bytes` (CBR padding, 06 §5.2), `oversize_rejected` |
| audio tail | `samples_padded`, `samples_dropped`, `samples_inserted`, `audio_drift_samples` (gauge), `rephase_count` (06 §8) |
| anc and fastmeta tails | `anc_packets`, `udw_bytes`; `items`, `keepalives` |
| TX histograms | `margin_hist` (signed `margin_ns`), `launch_error_hist` (`enqueued/observed_first − scheduled_first`), each with `sum`, `count`, `min`, `max` since create; `margin_min` (windowed) |
| RX units | `units_delivered`, `units_complete`, `units_incomplete_delivered`, `units_incomplete_discarded`, `units_used_redundancy`, `units_missed_pool_full`, `units_reclaimed` (latest-wins), `units_flushed`, `units_before_start` (discarded in ARMED), `units_stale`, `units_dropped_notify` (the engine refused delivery, SF-45), `format_changes` |
| RX packets and work | `pkts_received`, `pkts_redundant`, `pkts_stale`, `pkts_dma`, `pkts_cpu`, `pkts_lost_est` + `lost_is_exact` flag, `pkts_rejected[MTL_RX_REJECT_SLOTS]` (by `enum mtl_rx_reject`: `PT`, `SSRC`, `LEN`, `INTERLACE`, …, `MTL_RX_REJECT_DMA_BUSY` (SF-46)), `caller_work_ns`, `cmd_timeouts` |
| RX histograms | `latency_hist` (`arrival_first − media`), `delivery_hist` (`delivered − media`); `latency_max` (windowed) |
| Per leg | `pkts`, `bytes`, `pkts_lost`, `pkts_reordered`, `pkts_duplicate_same_leg`, `pkts_skipped` (TX: not sent on a leg that is down or disabled, r3), `igmp_reports` (periodic renewals, r3), `link_state` (gauge: the oper state; the admin state is in the status, §3), `last_packet_tai_ns` (gauge), TX: `observed_skew_ns` (gauge, 2022-7 senders) |
| Queue gauges (§2.4) | `gauge[]` per lease state (`enum mtl_lease_state`: FREE, APP_WRITABLE, QUEUED, IN_FLIGHT, DONE, RECEIVING, READY, APP_READING, HELD_BY_TX), `held_by_app` (`mtl_buffer_hold` holds), `queued_media_ns`, `unread_results`, `blocked_on`; per state `entries[]` and `exits[]` counters for the G-43 identity |
| RX timing parser summary (opt-in, video/audio, §2.7) | `tp_narrow`, `tp_wide`, `tp_fail`, `tp_untrusted_pkts` (SW-timestamp packets discarded, not exposed today `[R06 Q3]`), windowed `vrx_max_*`, `cinst_max_*`, `fpt_max_*` (first packet time), `latency_max_*` |

¹ `units_suppressed` counts results suppressed by completion modes NONE and EXCEPTIONS
(07 §2.2).

² **r3.** `SESSION_RECOVERY` events coalesce (begin/end are one type with first and last
kept), so three recoveries between two EQ reads show as one event. `recoveries_ok` and
`recoveries_failed` are the **counts of record**; the event only says "something
happened, read the counters" `[C5 §8.12]`.

Guidelines behind the list (WebRTC stats and OpenTelemetry `[R11 §8.3–8.4]`):

- counters only increase, for the life of the session: across stop/start,
  `update_flows`, `reconfigure` and recovery. No library call resets them. Averages are
  never computed by the library — sums and counts are exposed and readers divide;
- a counter the backend cannot produce is flagged in `unsupported_mask`, not left at a
  silent 0 (today iavf `tx_err` is always 0 `[R06 N4]`);
- estimates are named or flagged as estimates (`pkts_lost_est`, `lost_is_exact`)
  `[R06 F12]`;
- one definition per field for every essence (ST30 "dropped" never increments today
  `[R06 F3]`).

### 2.3 Cumulative only; no reset, no epochs (r3)

Reset-on-read breaks as soon as two readers exist (the app, a GStreamer element, an
exporter) `[R11 §11 #9]`. Revision 2 replaced it with a per-session
`mtl_session_stats_new_epoch`. Review C5 showed the epoch is shared library state: when
reader A starts a new epoch, reader B's next delta is against a reset baseline and must
be discarded, the same problem one step removed; and if every reader keeps its own
baseline anyway, the epoch adds nothing `[C5 §8.8]`. Therefore:

- counters are **cumulative for the life of the session**; `created` (in the snapshot and
  in `mtl_session_get_info`) says since when;
- `mtl_session_stats_new_epoch` is **removed**. Q-OBS-1 is answered (a), cumulative only;
- a reader that wants rates or deltas keeps its own previous snapshot and subtracts; the
  `snapshot` time makes the interval exact. Two readers never interfere;
- quantities that only made sense with a reset (`vrx_max`, `cinst_max`, `margin` extrema)
  become **windowed maxima** plus histograms (§2.6);
- the legacy `*_reset_session_stats` functions keep working on the legacy API, and do not
  affect unified counters of the same engine session.

### 2.4 How reads work without touching the tasklet

Review C1 found that counters the draft treated as tasklet-owned are written from several
contexts today — the builder and the app thread (`stat_drop_frame`,
`st20_pipeline_tx.c:145`, `:920`), the stat thread (copy *and reset*, `tv_stat_collect`),
the free-callback context, the admin thread under the blocking session spinlock
(`mt_admin.c:30`) — and that the v1 read path itself takes the spinlock the tasklets
`trylock` (`st20_pipeline_tx.c:1311` → `st_tx_video_session.c:4760`), making the tasklet
skip the session for an iteration `[C1 #6]`. The design:

| Rule | Detail |
|---|---|
| **Per-writer counter blocks** | one block per writer context (tasklet, app/pipeline thread, completion context, admin), each on its own cache line, written with relaxed stores by its single writer; the reader sums them |
| **Gauges by single-pass scan (r3)** | lease-state gauges are **not** sums of per-writer blocks: summing blocks written by four contexts is not a consistent cut, so a slot mid-transition would be counted twice or not at all `[C5 §8.9]`. The reader scans the slot state words once (one relaxed load per slot, pool ≤ a few hundred slots) and counts. `queued_media_ns` comes from the same scan |
| **Transition counters** | each writer counts the transitions it makes into and out of each state (`entries[state]`, `exits[state]`, per-writer blocks). They make G-43 testable as an identity (13) |
| **Seqlock only for grouped values of one writer** | histogram buckets, windowed-max buckets, the PTP offset/path-delay pair |
| **No reset by the library** | the stat dump computes deltas on the reader side instead of dump-and-reset |
| **No session spinlock on any read path** | the admin CPU-busy computation uses tasklet-written busy counters instead of taking the lock |
| **Port stats** | a library thread polls NIC counters periodically (`rte_eth_stats_get` can be a PF mailbox round-trip on a VF `[R06 F8]`); getters read the cached copy |

This is L0 work in `tv_*`/`rv_*` and in every pipeline. L2-owned counters follow it from
Phase 1; engine counters are converted in Phase 2 (14).

### 2.5 Extension stats (r3: typed getters)

Rare or backend-specific values (NIC xstats, RL shaper details, DMA engine counters, PTP
servo internals) are key-addressed so they can grow without ABI breaks, like DeckLink's
`IDeckLinkStatistics::GetInt(statID, param)` `[R11 §1.6]`. Revision 2 had one
`mtl_stat_get_u64(uint64_t object, …)`, which erased every handle type and could not name
an instance or a port `[C5 §2.9]`. Revision 3 has typed getters:

```c
int mtl_session_stat_get(mtl_session_h s, uint32_t key, uint32_t param, uint64_t* value);
int mtl_port_stat_get(mtl_instance_h mt, uint32_t port, uint32_t key, uint32_t param, uint64_t* value);
int mtl_instance_stat_get(mtl_instance_h mt, uint32_t key, uint32_t param, uint64_t* value);
int mtl_stat_list(uint32_t object_kind, struct mtl_stat_desc* descs, uint32_t cap, uint32_t* n); /* name, unit, kind */
```

### 2.6 Histograms and windowed maxima (r3)

- **Histograms** are cumulative since create, with `sum`, `count`, `min`, `max`. The
  default bucket layout is log2 (signed for margins). Log2 buckets give 2× resolution,
  which a compliance lab cannot use for VRX/CINST `[C5 §8.12]`; so every histogram also
  accepts a **linear layout** through one option key per histogram:
  `MTL_OPT_HIST_LINEAR_MARGIN`, `_LAUNCH_ERROR`, `_LATENCY`, `_DELIVERY`, `_VRX` and
  `_CINST`. The value is the bucket width in the histogram's unit (ns for the time
  histograms, packets for VRX and CINST); 0 keeps log2. It is set in CREATED or STOPPED,
  so the layout is fixed while the session runs; every histogram has `MTL_HIST_BUCKETS`
  buckets.
- **Windowed maxima** (`struct mtl_window_max`: `last_1s`, `last_60s`) come from a ring of
  60 one-second buckets written by the single writer of that value; the reader takes the
  current bucket (1 s) or the maximum over the valid buckets (60 s), each bucket read under
  its seqlock. They
  replace revision 2's "max since epoch" (`vrx_max` "gauge per epoch"), which needed a
  per-reader reset the design removes `[C5 §8.8]`.

### 2.7 RX timing-parser summary in v1 (r3)

The RX timing parser stays **opt-in per session** (Q-OBS-3) — it costs per-packet work and
is trustworthy only with HW timestamps. When enabled, its **summary** is part of v1 (not
Phase 5): the `tp_*` counters, windowed maxima and the VRX/CINST histograms of §2.2, with
the linear layout available. Packets with SW timestamps are counted in
`tp_untrusted_pkts` and excluded from the verdicts; the summary is computed with integer
math `[C1, Q-OBS-3]`. Per-packet detail stays in USDT.

## 3. Status getters

| Getter | Returns |
|---|---|
| `mtl_session_get_state(s)` | **r3:** the `enum mtl_session_state` as the non-negative return value (no out pointer); `MTL_STATE_RETIRED` for a retired handle during its tombstone grace period; negative `-MTL_EBADF` otherwise. The cheap DP getter |
| `mtl_session_get_status(s, &st)` | CP copy: `state` and `reason` (`enum mtl_state_reason`) of the last transition; in ERROR `last_error_reason` and `error` (the code data calls return); `rx_signal`; `format_changed` (RX); current `pacing_class` and `path`; `blocked_on`; `timing_warning` (`reason`, `shortfall_ns`, `suggested_min_tx_delay_ns`); inline-hook state; and per leg (next row) |
| … per leg (`leg[MTL_MAX_LEGS]`) | `admin` (ENABLED / DISABLED); `oper` (UP / DOWN, from the link monitor); `flow_state` (`WAITING_NEIGHBOUR` / `RESOLVED` / `JOINING` / `JOINED` / `JOIN_FAILED`); `receiving` (RX); `igmp_reports`; `last_packet_tai_ns` |
| `mtl_session_get_info(s, &info)` | granted configuration and SDP values (§3.1) |
| `mtl_instance_get_status(mt, &st)` | **r3:** `manager` (`NOT_CONFIGURED` / `CONNECTED` / `LOST` / `RECONNECTING`, cached), `refcount` of the default instance, `sessions`, `sessions_closing`, `regions` |
| `mtl_instance_get_info(mt, &info)` | **r3:** the granted `api_version` and `time_source`, port and scheduler counts, the effective instance `flags`, and `ignored_fields` (the parameters a second `acquire_default` ignored, [03 §7](03-object-model-and-lifecycle.md)) |
| `mtl_instance_get_mem_status(mt, numa, &st)` | **r3:** for one NUMA node: `hugepage_size`, `hugepages_total` and `hugepages_free`, the bytes this instance holds (`library_bytes`, of which `internal_bytes`; `imported_bytes`), `largest_free_segment`, and the instance's region budget (`regions_used`, `regions_free`) ([05 §6.3](05-memory-and-buffers.md)) |
| `mtl_instance_list_sessions(mt, handles, cap, &n)` | **r3:** §7 |
| `mtl_port_get_status` | link up/down, speed, NIC counters (cached, with the time they were read), RL queues in use, resets |
| `mtl_port_get_caps` | capabilities, including `max_regions`, the page sizes accepted for import ([05](05-memory-and-buffers.md)) and `link_source`, the link-monitor mode (LSC interrupt or poll) |
| `mtl_port_get_capacity(mt, port, &cap)` | **r3:** §8 |
| `mtl_time_get_status` | 06 §2.4 |
| `mtl_sched_get_status(mt, idx, …)` | busy %, average and p99.99/max loop and tasklet times, sleep ratio, sessions and quota, waker CPU % |
| `mtl_group_get_state`, `mtl_timeline_get_anchor`, `mtl_mem_get_info` | as named |

### 3.1 `struct mtl_session_info` (r3)

Review C5 found `mtl_session_info` used everywhere and defined nowhere, and named it the
struct that matters most: the GStreamer LATENCY query, the SDP generator and the granted
SSRC, UDP source port and MACs all depend on it `[C5 §2.2, §3.4]`. This is its field list;
the header in `sketch/` carries the layout, types and the size check.

| Group | Field | Meaning |
|---|---|---|
| identity | `name[64]` | the session name copied at create (§7) |
| | `direction`, `essence`, `unit` | `MTL_DIR_TX/RX`; video, cvideo, audio, anc, fastmeta; the unit kind (frame, field, rows, packet run) |
| | `created` | TAI time of create; counters count from it |
| | `flags` | `MTL_INFO_RTP_OFF_GRID`, `MTL_INFO_JTNM_DEFAULT_WINDOW_EXCEEDED`, `MTL_INFO_NON_COMPLIANT_2110_22` |
| placement | `sched_index` | scheduler the session runs on (replaces `st20p_tx_get_sch_idx`) |
| | `numa`, `numa_mismatch` | actual node of the lease table and pool; set when it differs from the ports' node |
| | `leg_count`, `leg[].port` | legs and the instance port of each |
| buffers | `unit_bytes` | bytes of one unit in the app layout (replaces `frame_size`) |
| | `pool_count`, `min_count_direct`, `max_count` | granted pool size; the minimum for the DIRECT path (0 = n.a. for non-video); the engine's maximum |
| | `pool_slot_pitch` | library pools: bytes between slots in the pool region |
| | `internal_buffer_count`, `internal_bytes` | library-internal transport frames and their memory (CONVERT paths double memory, C5 §6.6) |
| | `buffer_capacity_bytes`, `unit_samples`, `max_udw_bytes` | audio/anc/fastmeta pool capacity; granted audio samples per unit; ANC UDW limit |
| | `max_user_meta_bytes`, `meta_max_bytes` | user meta limit (≤ 1332 B); RX meta area per unit |
| completion and latency | `completion_mode` | granted (exported pools force ALL) |
| | `completion_latency_ns` | expected delay from the last packet handed to the NIC until the result is visible (≈ `nb_tx_desc` packets on the chain path) |
| | `expected_wake_latency_ns` | expected wake-up latency of a sleeping waiter under the granted `waker_mode` (04 §5.2, from S1) |
| | `min_submit_lead_ns`, `pickup_lead_ns` | the smallest lead before media time at which a submission is still on time: a framework's latency; the engine's gap from the pick-up deadline to `scheduled_first` (06 §7.1) |
| | `latency_min_ns`, `latency_max_ns` | added latency range (GStreamer LATENCY query) |
| | `horizon_ns`, `rx_flush_offset_ns`, `tolerated_skew_ns` | granted horizon; RX deadline offset; tolerated 2022-7 path differential |
| | `media_time_offset_ns` | the granted offset in TAI mode (06) |
| transport | `pacing_class`, `pacing_profile`, `path` | granted pacing class, accuracy profile and data path (DIRECT / COPY / CONVERT) |
| | `source_kind`, `media_mode` | as granted (06) |
| | `tx_queue_kind`, `waker_mode`, `backend_syscalls_on_tasklet` | dedicated RL queue or shared; W0 / W2 / W3; non-zero for kernel-socket and AF_XDP backends |
| | `seq_restarted` | the RTP sequence was re-randomised at the last start |
| per leg (`leg[MTL_MAX_LEGS]`) | `ssrc`, `udp_src_port` | granted SSRC (random unless configured) and UDP source port |
| | `src_mac[6]`, `dst_mac[6]` | own MAC; resolved or user-given destination MAC, zero while `WAITING_NEIGHBOUR` |
| | `payload_type`, `dscp`, `udp_dst_port`, `ttl` | granted values (defaults table in [09](09-media-modes-and-backends.md)) |
| SDP (TX; RX reports what it detected) | `sender_type` | `TP=` 2110TPN / 2110TPNL / 2110TPW |
| | `troffset_ns`, `trs_ps`, `vrx_full`, `pkts_per_unit`, `cmax` | TROFF, TRS (ps), VRX, packets per unit, `CMAX=` |
| | `tsmode`, `tsdelay_ns`, `slot_delay`, `max_slot_delay` | declared `TSMODE=` (SAMP, NEW or PRES — never inferred from the media mode `[C2 #23]`), `TSDELAY`, L in units, and the bound on L derived from `link_offset_budget_ns` |
| | `ts_refclk` | `gm_identity[8]`, `domain`, `kind` (unknown / PTP / local MAC) for `a=ts-refclk`; `a=mediaclk:direct=0` is implied |
| | `anc_tm`, `anc_window_frame_offset` | ANC transmission model for `TM=`; the frame offset L_anc of the ANC window (06 §5.5) |
| | `codestream_bytes`, `box_hdr_bytes`, `cbr_headroom_bytes`, `cvideo_rate_mode` | ST22 codestream size granted in whole packets, box header bytes, CBR headroom, CBR (default) or VBR_MAX |

The names are the header's; the layout and size check are there.

## 4. SDP-relevant values

The timing promise is only interoperable if the application can signal it `[R12 §2]`.
Everything an SDP needs that MTL decides is in the SDP group of §3.1. Generating SDP text
is an L4 helper question (Q-MODE-6).

## 5. Tracing (USDT)

Keep the existing USDT providers and add probes for what the research found missing:
admission decisions (unit, margin, policy fired), late/drop, recovery begin/end, time
state change, link change, back-pressure begin/end, migration, command posted and acked
`[R06 §1.5]`. Probes carry the session's handle ID, unit seq, media index
and RTP so traces correlate with CQ records.

## 6. Logging contract

- The printer receives `(level, instance, session name, module, message)` and a user
  `priv`; today it is process-global with no identity `[R06 F9]`. The session name is the
  log prefix (r3).
- **In release builds, tasklets never format or print a log line at any level.** They
  write fixed binary records (code + arguments) into a lock-free per-scheduler log ring;
  a library thread formats them and calls the printer, rate-limited per (code, session).
  Today tasklets log at every level: `info()` on a late-drop batch
  (`st20_pipeline_tx.c:197`), `warn()` in the free callback (`st_tx_video_session.c:129`),
  `err()` on hang detection (`st_video_transmitter.c:38`) and on a refused RX delivery
  (`st_rx_video_session.c:941`) `[C1 #14]`. A debug-build assert catches direct formatting
  on busy-loop threads (G-44).
- Every fact that is logged has a structured home first (a counter, a gauge, an event or a
  result field).
- **r3.** The normal "no MtlManager" configuration logs nothing above `info`. Today
  `mtl_is_manager_alive()` logs `err` when no manager runs (`mt_instance.c:266`) and
  `mt_instance_init` warns (`:201`) (SF-40). The manager state is a cached field of
  `mtl_instance_get_status`.

## 7. Names and enumeration (r3)

Revision 2 had `const char* name` with no lifetime, copy or uniqueness rule, and an event
`origin` that was a handle ID, which changes on every re-create; Prometheus labels, log
correlation and NMOS receiver-id mapping broke across restarts `[C5 §8.7]`.

- `name` is **copied at create** into a fixed 64-byte field (`MTL_NAME_MAX`, including the
  NUL). The config pointer may be freed after create returns.
- It is returned in `mtl_session_get_info`, in the stats header, and in the log prefix.
  Every event carries the origin's name in `origin_name[64]`
  ([07 §3.1](07-completions-events-and-errors.md)), so a `SESSION_RETIRED` reader, which
  can no longer call `get_info` on the retired handle, still knows which session it was.
- Names are **unique per instance**: a create with a name already used by a live or
  DESTROYING session fails with `-MTL_EEXIST`. An empty name gets a generated one
  (`<essence>-<dir>-<n>`). `mtl_session_reconfigure` keeps the name; destroy + create with
  the same name is how an app keeps its identity across a re-create.
- Session and timeline names are separate namespaces ([06](06-timing-pacing-and-sync.md)).
- **Enumeration** is in v1: `mtl_instance_list_sessions(mt, mtl_session_h* handles,
  uint32_t cap, uint32_t* n)` writes at most `cap` handles and sets `*n` to the total,
  DESTROYING sessions included. It is CP; the handles are ordinary generation-checked
  handles, so a session retired between the list and a later call just returns
  `-MTL_EBADF` (or `MTL_STATE_RETIRED` from `mtl_session_get_state` during its grace
  period).

**The exporter pattern.** A telegraf/Prometheus exporter thread inside the process, which
did not create the sessions, calls `mtl_instance_list_sessions` every period, then
`mtl_session_get_info` (for the name, the labels and the SDP values) and
`mtl_session_get_stats` (cumulative counters), and computes rates from its own previous
snapshot. It needs no `stat_dump_cb_fn` (`mt_stat.c:56-58`), no reset, and does not
disturb any other reader. An out-of-process reader stays later (Q-OBS-2).

## 8. Capacity and admission (r3)

A broadcast controller must answer "can this host take 12 more 1080p59 streams?" before
it activates anything. The hard limits are real and invisible today: 18 schedulers
(`MT_MAX_SCH_NUM`, `mt_main.h:49`), 60 video TX sessions per scheduler
(`ST_SCH_MAX_TX_VIDEO_SESSIONS`, `st_header.h:33`), a per-scheduler data quota in Mb/s
(`mt_sch_add_quota`, `mt_sch.c:1060-1080` → `-ENOMEM`), "no free sch"
(`mt_sch.c:378`, `:1171`), static TX/RX queue counts fixed at `rte_eth_dev_configure`,
and the RL queues of each port `[C5 §8.11]`.

```c
struct mtl_port_capacity {
  uint32_t struct_size;
  uint32_t free_tx_queues, free_rx_queues;
  uint32_t free_rl_queues;              /* dedicated rate-limited TX queues */
  uint32_t free_lcores;                 /* lcores the instance can still take (manager or shm allocator) */
  uint32_t free_sessions_per_sched;     /* free video session slots on the least-loaded scheduler */
  uint32_t quota_free_1080p_x100;       /* remaining scheduler quota in 1080p59.94-equivalents × 100 */
  uint32_t quota_max_one_sched_x100;    /* the largest remaining quota on one scheduler: bounds the biggest single session */
  uint64_t reserved[4];
};
int mtl_port_get_capacity(mtl_instance_h mt, uint32_t port, struct mtl_port_capacity* cap);
```

- The numbers are a snapshot; another creator can take the capacity before the
  controller acts. The placement algorithm is not the controller's problem: the dry run
  applies it.
- **`MTL_QUERY_CHECK_CAPACITY`** on the dry-run `mtl_<essence>_session_query` runs the real
  placement (scheduler choice, quota, queues, RL queue) without reserving anything and
  returns `-MTL_ENOSPC` with the limiting resource in `mtl_last_error().reason`
  (`CAPACITY_TX_QUEUES`, `CAPACITY_RL_QUEUES`, `CAPACITY_SCHED_QUOTA`, `CAPACITY_LCORES`,
  `CAPACITY_SESSIONS`). If the query succeeds and nothing else changes the host, the
  create succeeds (G-89).
- A **reservation object** (hold capacity for N sessions, then create into it) is Phase 6.

## 9. What the operator surface adds up to (r3)

| Operator need | Where |
|---|---|
| atomic multi-leg activation at a TAI instant (NMOS IS-05) | `mtl_session_update_flows` ([09](09-media-modes-and-backends.md), G-75) |
| take one network out for maintenance, bring the second leg up later | `mtl_session_set_leg_enabled`; admin × oper state per leg (G-91) |
| format change without losing identity | `mtl_session_reconfigure` in STOPPED (G-87) |
| stable identity, enumeration, exporter | §7 (G-88) |
| capacity before activation | §8 (G-89) |
| know when the manager is gone | `mtl_instance_get_status().manager`, `MANAGER_LOST` event ([07](07-completions-events-and-errors.md)) |

## R4. Key catalogue (revision 4)

Revision 4 replaces the fixed stats, info and host-status structs with one registry of named
values (`mtl_observe.h`, [REVISION-4 D-80](REVISION-4.md)). This section is the normative list of
names; a key not listed here is not part of the contract. The read rules of §2.4 hold unchanged:
per-writer blocks summed by the reader, one scan of the slot states, seqlocked groups, cached
NIC values, no lock a tasklet takes.

### R4.1 Naming

- Key = `scope.metric`, lower case, `[a-z0-9_.]`, ≤ 47 characters; lookup accepts `scope.metric{label}`, Prometheus syntax. One label string of `k=v[,k=v]`, ≤ 47 characters.
- Scopes: `tx`, `rx`, `leg`, `queue`, `tp`, `video`, `cvideo`, `audio`, `anc`, `fastmeta`, `info` (session CONST); `port`, `caps`, `nic` (driver xstats, names as the driver gives them), `time` (port
  objects); `sched`; `instance`, `mem`, `capacity` (instance object).
- Units appear as a suffix when not a plain count (`_ns`, `_ps`, `_bytes`, `_pct_x100`, `_mbps`) and in `unit`. Exporters append `_total` to COUNTERs; the library never does.
- Label keys: `leg`, `reason` (lower-case `mtl_reason_name` without the prefix), `cause`, `state` (lease state), `window` (`1s`, `60s`), `numa`, `on`, `essence`, `dir`, `name`.
- A published name never changes meaning; removal follows the D-69 deprecation policy; adding names is never an ABI event. Every counter is cumulative for the object's life.

### R4.2 Keys

| Object, scope | Keys (kind COUNTER unless marked G = gauge, C = const, H = histogram) |
|---|---|
| session `tx.` units | `units_submitted`, `units_on_time`, `units_late`, `units_dropped{reason=…}`, `units_flushed`, `units_withdrawn`, `units_before_start`, `units_failed`, `units_suppressed`, `slots_empty`, `units_repeated`, `units_padded` |
| session `tx.` work | `acquire_blocked{on=results\|buffers\|app_leases}`, `build_overrun`, `recoveries_ok`, `recoveries_failed`, `cmd_timeouts`, `bytes`, `pkts`, `pkts_dma`, `pkts_copied_partial`, `tpr_late_pkts`, `caller_work_ns` |
| session `tx.` timing | `vrx_max{window}` G, `cinst_max{window}` G, `margin_min_ns{window}` G, `margin_ns` H, `launch_error_ns` H, `vrx` H, `cinst` H, `pacing_class` G, `path` G, `timing_warning_reason` G, `shortfall_ns` G, `suggested_min_tx_delay_ns` G |
| session `rx.` units | `units_delivered`, `units_complete`, `units_incomplete_delivered`, `units_incomplete_discarded`, `units_used_redundancy`, `units_missed_pool_full`, `units_reclaimed`, `units_flushed`, `units_before_start`, `units_stale`, `units_dropped_notify`, `format_changes`, `discontinuities` (new, P7) |
| session `rx.` packets | `pkts_received`, `pkts_redundant`, `pkts_stale`, `pkts_dma`, `pkts_cpu`, `pkts_lost_est` (flag ESTIMATE when inexact; replaces `lost_is_exact`), `cmd_timeouts`, `caller_work_ns`, `pkts_rejected{cause=…}` (causes: pt, ssrc, len, interlace, seq_old, rtp_out_of_range, no_slot, dma_busy, wrong_port, fmd_filter, before_start, offset, stale) |
| session `rx.` timing, format | `latency_max_ns{window}` G, `latency_ns` H, `delivery_ns` H, `format_changed` G mask; new (F4): `detected.width`, `detected.height`, `detected.fps_num`, `detected.fps_den`, `detected.interlaced` (all G) |
| session `tp.` (opt-in parser) | `narrow`, `wide`, `fail`, `untrusted_pkts`, `vrx_max{window}` G, `cinst_max{window}` G, `fpt_max_ns{window}` G, `latency_max_ns{window}` G; audio (new, F6): `dpvr_max_ns{window}` G, `ipt_max_ns{window}` G, `tsdf_max_ns{window}` G; thresholds (new, F6): `pass.*` C |
| session `leg.` (`leg=N`) | `pkts`, `bytes`, `pkts_lost`, `pkts_reordered`, `pkts_duplicate_same_leg`, `pkts_skipped`, `igmp_reports`, `receiving` G, `last_packet_tai_ns` G, `observed_skew_ns` G (TX 2022-7) |
| session `queue.` | `gauge{state=free\|app_writable\|queued\|in_flight\|done\|receiving\|ready\|app_reading\|held_by_tx}` G, `entries{state}`, `exits{state}`, `unread_results` G, `held_by_app` G, `queued_media_ns` G (one scan, G-43 unchanged) |
| session essence | `cvideo.padding_bytes`, `cvideo.oversize_rejected`, `audio.samples_padded`, `audio.samples_dropped`, `audio.samples_inserted`, `audio.drift_samples` G, `audio.rephase_count`, `anc.packets`, `anc.udw_bytes`, `fastmeta.items`, `fastmeta.keepalives` (the video tail duplicated `tx.`/`rx.` keys and goes) |
| session `info.` C (placement, pacing) | `sched_index`, `pacing_profile`, `sender_type`, `tsmode`, `source_kind`, `media_mode`, `tx_queue_kind`, `waker_mode`, `backend_syscalls_on_tasklet`, `numa`, `numa_mismatch`, `internal_buffer_count`, `internal_bytes`, `box_hdr_bytes`, `cvideo_rate_mode`, `cbr_headroom_bytes`, `anc_tm`, `anc_window_frame_offset`, `total_lines` |
| session `info.` C (timing, conversion) | `max_slot_delay`, `slot_delay`, `vrx_full`, `cmax`, `seq_restarted`, `troffset_ns`, `trs_ps`, `tsdelay_ns`, `pickup_lead_ns`, `media_time_offset_ns`, `completion_latency_ns`, `expected_wake_latency_ns`, `tolerated_skew_ns`, `horizon_ns`, `rx_flush_offset_ns`, `convert_context` |
| session `info.` C (SDP, converter) | `ts_refclk.gm_identity` (unit ID), `ts_refclk.domain`, `ts_refclk.kind`, `converter{name=<plugin>}` |
| port `port.` | `link_up` G, `speed_mbps` G, `rl_queues_in_use` G, `resets`, `rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`, `rx_errors`, `tx_errors`, `rx_missed`, `rx_nombuf` (new, F5), `sampled_tai_ns` G, `free_tx_queues` G, `free_rx_queues` G, `free_rl_queues` G |
| port `caps.` C | `backend`, `pacing_classes`, `numa`, `tx_multi_seg`, `hw_rx_timestamp`, `hw_tx_timestamp`, `launch_time_offload`, `dma_engines`, `header_split`, `max_rl_queues`, `iova_va`, `backend_syscalls_on_tasklet`, `max_regions`, `link_source`, `page_size`, `hugepage_sizes`, `max_sessions{essence}` |
| port `time.` | `source` G, `state` G, `ptp_domain` G, `offset_ns` G, `path_delay_ns` G, `last_sync_age_ns` G, `grandmaster_id` G (unit ID), `utc_offset_s` G, `step_count` |
| port `nic.` | driver xstats as today's keyed design (`nic.rx_missed`, `rl.shaper_drops`, …) |
| sched `sched.` | `lcore` C, `sessions` G, `busy_pct_x100` G, `sleep_ratio_x100` G, `waker_cpu_pct_x100` G, `quota_used_1080p_x100` G, `loop_avg_ns` G, `loop_max_ns` G, `tasklet_p9999_ns` G, `tasklet_max_ns` G |
| instance `instance.` | `api_version` C, `sched_count` C, `time_source` C, `flags` C, `ignored_fields` C, `debug_api` C, `manager` G, `refcount` G, `sessions` G, `sessions{essence,dir}` G (new, F5), `sessions_closing` G, `regions` G, `regions_used` G, `regions_free` G, `lcores` G, `dma_devs` G (new, F5) |
| instance `mem.` (`numa=N`) | `hugepage_size` C, `hugepages_total` G, `hugepages_free` G, `library_bytes` G, `internal_bytes` G, `imported_bytes` G, `largest_free_segment` G |
| instance `capacity.` | `free_lcores` G, `free_sessions_per_sched` G, `quota_free_1080p_x100` G, `quota_max_one_sched_x100` G |

### R4.3 Keys added by the revision-4 reviews

| Object, scope | Keys |
|---|---|
| port `caps.` C | `backend` (enum `mtl_backend` of `mtl_observe.h`) |
| instance `instance.` C | `simd_level` (enum `mtl_simd`) |
| session `rx.detected.` G | `width`, `height`, `fps_num`, `fps_den`, `interlaced`, `format`, `packing` (the getter of `MTL_EVENT_RX_FORMAT`) |
| session `pkt.` (packet units) | `chunks`, `units`, `pkts{leg}`, `bytes{leg}`, `stamp_violations{reason}`, `validate_violations{reason}`, `late_pkts`, `rx_ring_full`, `lend_to_copy`, `dedup_drops`, `seq_gaps` |
| session `rtx.` (video, cvideo; NACK retransmission, named `rtcp.` before addendum N) | `nacks_sent`, `nacks_received`, `retransmitted`, `recovered` |
| session `info.` C | `rx_path` (packet units: copy or lend), `pkts_per_chunk`, `slot_stride` |

### R4.4 Legacy stats fields

| Legacy (`include/st_api.h`, `st20_api.h`, `mtl_api.h`) | Key |
|---|---|
| `st_tx_user_stats.port[i].packets`, `.bytes`, `.frames` | `leg.pkts{leg=i}`, `leg.bytes{leg=i}`, `tx.units_on_time` + `tx.units_late` |
| `st_tx_user_stats.stat_epoch_drop`, `stat_epoch_onward` | `tx.units_dropped{reason=too_late}`, `tx.units_late` |
| `st_tx_user_stats.stat_frames_sent` | `tx.units_on_time` + `tx.units_late` |
| `st_rx_user_stats.port[i].packets`, `.bytes`, `.lost_packets`, `.reordered_packets`, `.duplicates_same_port` | `leg.pkts`, `leg.bytes`, `leg.pkts_lost`, `leg.pkts_reordered`, `leg.pkts_duplicate_same_leg` (label `leg=i`) |
| `st_rx_user_stats.stat_pkts_received`, `stat_pkts_redundant`, `stat_lost_packets` | `rx.pkts_received`, `rx.pkts_redundant`, `rx.pkts_lost_est` |
| `st_rx_user_stats.stat_pkts_wrong_pt_dropped`, `stat_pkts_wrong_ssrc_dropped` | `rx.pkts_rejected{cause=pt}`, `rx.pkts_rejected{cause=ssrc}` |
| `st_rx_user_stats.stat_frames_received`, `stat_frames_dropped`, `stat_frames_corrupted` | `rx.units_delivered`, `rx.units_incomplete_discarded`, `rx.units_incomplete_delivered` |
| `st20_rx_user_stats.stat_bytes_received`, `stat_pkts_no_slot`, `stat_pkts_dma` | `leg.bytes` (summed), `rx.pkts_rejected{cause=no_slot}`, `rx.pkts_dma` |
| `st20_rx_user_stats.stat_pkts_wrong_len_dropped`, `stat_pkts_wrong_interlace_dropped` | `rx.pkts_rejected{cause=len}`, `rx.pkts_rejected{cause=interlace}` |
| `mtl_port_status` (`rx_packets`, `tx_packets`, `rx_bytes`, `tx_bytes`, `rx_err_packets`, `rx_hw_dropped_packets`, `rx_nombuf_packets`, `tx_err_packets`) | `port.rx_pkts`, `port.tx_pkts`, `port.rx_bytes`, `port.tx_bytes`, `port.rx_errors`, `port.rx_missed`, `port.rx_nombuf`, `port.tx_errors` |

Fields not listed map to a key of the same meaning in R4.2, or to an engine counter that stays
internal (debug counters such as `stat_pkts_slice_merged`).

### R4.5 Keys added by the addenda (Kubernetes, NMOS, IPMX)

[16](16-kubernetes-and-crash-safety.md) and [17](17-nmos-and-ipmx.md) add these. `info.ts_refclk.*`
changes from C to G with a `leg` label, because a grandmaster can change at runtime (C-N9).

| Object, scope | Keys |
|---|---|
| sched `sched.` | `loops`, `last_loop_tai_ns` G (the heartbeat of `MTL_HEALTH_SCHED_STALLED`; advanced on wake-ups and timer ticks too) |
| instance `instance.` | `health` G mask (`MTL_HEALTH_*`), `phase` G (`enum mtl_phase`), `port_count` C, `iova_mode` C, `cpu_quota` C (CFS quota in µs per period, 0 = none), `reconciled{kind}` (what open cleaned up) |
| instance `mem.` | `hugetlb_limit_bytes{size}` G and `hugetlb_usage_bytes{size}` G (the container's cgroup, not the host); `memlock_limit_bytes` C |
| port `caps.` C | `vf_trusted`, `mcast_filters_max`, `pf_tx_rate_mbps`, `phc_readable`, `iova_mode`, `reset_budget_ns` (the port-reset budget a shutdown must leave) |
| port `port.` | `mcast_filters_used` G, `removed` G, `mac` C (unit ID) |
| port `time.` | `disciplined_by` G (node, MTL, application, none), `error_ns` G, `gm_traceable` G, `gm_clock_class` G, `gm_clock_accuracy` G, `gm_priority1` G |
| session `session.` | `last_progress_tai_ns` G, `leases_out` G, `oldest_lease_ns` G |
| session `info.` | `wire_kbps{leg}` C, `payload_kbps` C, `max_udp_bytes` C, `troffset_default` C, `ts_refclk.gm_identity{leg}` G, `ts_refclk.domain{leg}` G, `ts_refclk.kind{leg}` G |
| session `tx.` | `units_muted` (retired at their slots with every leg disabled; not in `units_dropped`), `rtcp_sr` (reports sent), `rtcp_info_version` G, `f2f_pp_ns` G (TR-10-9 §11.2), `units_no_key` |
| session `rx.` | `units_late_presentation`, `rtcp_sr`, `rtcp_sr_dropped`, `rtcp_info_version` G, `sender_rate_ppb` G, `link_offset_min_ns` G, `link_offset_max_ns` G, `crypto_auth_fail`, `crypto_unknown_key{version}` |
| session `leg.` | `pkts_late` (arrived after their unit was delivered or flushed) |
| session `anc.` | `did_sdid_seen` G (up to 16 DID/SDID pairs, one slot each) |
