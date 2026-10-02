# S5 — Observability and errors: simplification

| | |
|---|---|
| Status | Simplification proposal for maintainer review; nothing here is applied to the sketch |
| Date | 2026-10-01 |
| Scope | Header sections 2, 4 (`blocked_on`, reasons), 5, 6 (time status), 7, 8 (instance, mem, port, scheduler observability), 12, 15 (`mtl_tx_reason`, `mtl_rx_reject` only), 19, and the matching parts of 20–21 |
| Inputs | the sketch header (3029 lines); [07 §3, §5](../07-completions-events-and-errors.md), [08](../08-observability.md), [11 §2](../11-abi-compatibility-and-migration.md), [13](../13-guarantees-and-tests.md), [15](../15-security-and-deployment.md); R06, R13; C5 and its response; [DECISIONS](../DECISIONS.md) |
| Owner's brief | defaults, simpler helpers, more abstraction, better logic; modular and extensible; every current MTL use case covered (any cut is flagged); the include file as lean as possible |

## 0. Verdicts at a glance

| # | Proposal | Hypothesis | Verdict | Functions | Struct/union types | Core lines |
|---|---|---|---|---|---|---|
| P1 | One stats registry: named values, bulk read by index, enumerable schema | H1 | **RECOMMEND** | −2 | −13 | ≈ −215 |
| P2 | Instance, mem, port caps/status/capacity, scheduler and time status become registry objects | H6 | **RECOMMEND** | −15 | −8 | ≈ −150 |
| P3 | Session info: a 256 B typed core; diagnostics and SDP values as `info.*` keys | H2 | **RECOMMEND** | 0 | −1 | ≈ −35 |
| P4 | Lean session status (state, reasons, error, `blocked_on`, flags, legs) | H2 | **RECOMMEND** | 0 | 0 | ≈ −15 |
| P5 | One `enum mtl_reason`: absorbs `mtl_tx_reason`, `mtl_rx_reject` becomes labels; optional own header | H3 | **RECOMMEND** (own header: OPTION) | 0 | 0 | ≈ −25 (−100 with own header) |
| P6 | One event record: uniform payload, `origin` is a `struct mtl_object`, no producer class | H4 | **RECOMMEND** | 0 | −12 | ≈ −60 |
| P7 | Fewer event routes and types: no `mtl_eq_post`, instance EQ or `bind_eq`; LATER-only types out | H4 | **RECOMMEND** (EPOCH_TICK: OPTION, cut) | −3 | 0 | ≈ −20 |
| P8 | ABI: no struct kinds or `known_size`; one `mtl_struct_init`; outputs take their size | H5 | **RECOMMEND** | −28 | 0 | ≈ −120 |
| P9 | errno-style last error (drop `mtl_call_seq`) | own | OPTION | −1 | 0 | ≈ −3 |
| P10 | A typed 8-counter summary struct next to the registry | H1 | OPTION (default: no) | +1 | +1 | +15 |
| P11 | Reason packed into the return code | own | REJECT | | | |
| P12 | Reasons as strings only, no constants | H3 | REJECT | | | |
| P13 | State, `blocked_on` and errors as keys too | H6 | REJECT | | | |
| P14 | Merge `LEG_STATE` and `FLOW_STATE` | H4 | OPTION | 0 | 0 | −2 |
| | **Total of the RECOMMEND set** | | | **−48** (217 → ≈ 169) | **−34** | **≈ −640 core** (+≈ 90 in `mtl_observe.h`) |

H7 (tiering) is answered by §3: the lean core keeps errors, reasons, state, a small status, the core info, time and events; `mtl_observe.h` carries the registry. Every capability of today's MTL stays
covered (§3.5); the only cut of a current capability is optional (EPOCH_TICK, §4).

## 1. Diagnosis

### 1.1 Size of this area in the sketch

| Header section | Lines | Exported functions | Struct/union types | Enums (constants) |
|---|---|---|---|---|
| 2 error codes | 23 | 0 | 0 | 16 codes |
| 4 `blocked_on`, `mtl_state_reason` | 86 | 0 | 0 | 2 (6 + 68) |
| 5 last error | 37 | 4 + 1 init | 1 | — |
| 6 time status and time functions | ≈ 50 of 118 | 6 + 1 init | 1 | 3 |
| 7 struct kinds | 55 | 2 | 0 | 1 (44) |
| 8 instance/mem/port/scheduler observability | ≈ 185 of 270 | 11 + 8 inits | 7 | 4 |
| 12 info, status, stats, keyed stats, options | 404 | 7 + 3 inits | 20 + 2 unions | 8 |
| 14 session getters | 4 | 4 | — | — |
| 15 `mtl_tx_reason`, `mtl_rx_reject` | ≈ 45 | 0 | 0 | 2 (23 + 13) |
| 19 events | 182 | 10 + 1 init | 13 + 1 union | 4 (27 + 4 + 9 + 5) |
| 20–21 value macros and size checks of these types | ≈ 75 | — | — | — |
| **Total** | **≈ 1,140 (38 %)** | **57 (26 % of 217), plus all 37 `*_init`** | **45 (≈ half of all types)** | **≈ 295 constants** |

Bytes an app copies: `mtl_session_stats` 2,096 B per read (the TX block of 1,264 B is in the union even for an RX session), `mtl_session_info` 512 B with 64 fields, `mtl_session_status` 200 B,
`mtl_event` 184 B. About a fifth of the stats snapshot is reserved tail or unused union space (`reserved[16]` + `reserved0` in TX, 368 B of RX slack in the union, 11–14 reserved slots per essence
tail, `reserved[4]` per leg, `reserved[8]` overall).

### 1.2 Four mechanisms for one kind of fact

A number about a session can live in a fixed struct field (`get_stats`), a keyed value (`mtl_session_stat_get`, "backend-specific or rare values"), the info struct (`get_info`), or the status struct
(`get_status`); a number about the host in one of eight typed output structs (instance info, instance status, mem, port caps, port status, port capacity, scheduler, time). Each struct has its own
`*_init`, `MTL_STRUCT_*` kind, size check, reserved tail and "since" growth story. The keyed path already exists (`mtl_stat_desc` with names such as `"nic.rx_missed"`), so the design pays for both a
schema-in-the-header and a schema-at-runtime. Exporters (Prometheus, OpenTelemetry, telegraf) consume names and labels; nobody consumes C offsets except the app that compiled them.

### 1.3 Duplicated facts (the same value in two places)

| Value | Places |
|---|---|
| `pkts_dma`, `pkts_copied_partial` | `mtl_rx_stats.pkts_dma` and `mtl_video_stats.pkts_dma`; `mtl_tx_stats.pkts_copied_partial` and `mtl_video_stats.pkts_copied_partial` |
| per-leg `igmp_reports`, `last_packet_tai_ns`, `time_valid`, oper state | `mtl_leg_status` and `mtl_leg_stats` (`link_state` = `oper`) |
| `blocked_on` | `mtl_session_status` and `mtl_queue_gauges` |
| `state`, `essence`, `legs`, `name`, `created` | the stats header, and info/status |
| `producer_class` | `mtl_event` and `mtl_event_overflow` |
| object kind | `enum mtl_origin_kind` (9), `enum mtl_object_kind` (4), `enum mtl_stat_object` (3) |
| grandmaster and domain | `mtl_time_status.grandmaster_id/ptp_domain` and `mtl_session_info.ts_refclk` |
| library version, port count | `mtl_instance_info.lib_version` = `mtl_version_num()`; `.port_count` = `mtl_port_count()` |
| link speed | `mtl_port_caps.link_speed_mbps` and `mtl_port_status.speed_mbps` |
| region budget | `regions_used/free` in the per-NUMA `mtl_mem_status`, although the budget is instance-wide |
| pacing class and path | granted in info, current in status; the `PACING_CHANGED` row of 07 §3.2 names `get_info` as its getter |
| reason vocabulary | `MTL_TX_REASON_{LINK_DOWN, LEG_DISABLED, TX_QUEUE_FATAL, DEVICE_GONE, NO_NEIGHBOUR, STOP_TIMEOUT}` duplicate `MTL_REASON_{LINK_DOWN, LEG_DISABLED, TX_QUEUE_FATAL, DEVICE_GONE, WAITING_NEIGHBOUR, DRAIN_TIMEOUT}` |
| struct identity | the 44-value `enum mtl_struct_kind` restates the 37 `*_init` functions and 106 size checks |

### 1.4 Defects and gaps found on the way

| # | Finding | Consequence |
|---|---|---|
| F1 | The 37 `*_init(struct x*)` are **exported** and take no size: in a 0.3 library, `mtl_session_config_init(p)` zeroes and stamps the 0.3 `sizeof` into a 0.1 app's smaller object | out-of-bounds write when an older app runs on a newer library (or, frozen at the 0.1 size, new fields silently ignored); bindings must use these (11 §2.1 rule 9) |
| F2 | `unsupported_mask` is 64 bits for ≈ 250 values in a TX snapshot | unsupported counters beyond bit 63 read as a silent 0, which 08 §2.2 forbids |
| F3 | `converter`: "name via `mtl_session_stat_get`" | a `uint64_t` getter cannot return a plugin name |
| F4 | `RX_FORMAT` and `RX_TIMEBASE_SUSPECT` name `get_status` as their state getter, but status has only the `format_changed` mask and no timebase field; detected width/height/fps exist only in the event payload | after `OVERFLOW` the app cannot resync the detected format; today's `notify_detected` (`st20_detect_meta`) has no getter equivalent |
| F5 | The unified port status lacks `rx_bytes`, `tx_bytes`, `rx_nombuf` of today's `struct mtl_port_status` (`include/mtl_api.h`); instance status lacks per-essence session counts (`st_var_info`), lcore and DMA counts (`mtl_var_info`) | coverage regressions against today's API, unless a backend happens to export them as `nic.*` |
| F6 | The `tp_*` summary is video-shaped (VRX, CINST, FPT); ST30's parser reports DPVR/IPT/TSDF (`notify_timing_parser_result`), and `st20_rx_timing_parser_critical` thresholds have no home | audio timing-parser results and pass thresholds are not covered |
| F7 | 0.1 carries LATER-only items: `MTL_EVENT_INLINE_HOOK_DISABLED` and `status.inline_hook` (the inline hook is in `MTL_UNIFIED_LATER`), `MTL_EVENT_SESSION_MIGRATED` (Q-THR-6 proposed default (a): no migration) | dead constants in the frozen surface |
| F8 | `MTL_OPT_HIST_LINEAR` is a "deprecated alias" in a header that was never released | remove |
| F9 | Per-reason arrays are capped by frozen 32-slot counts; `mtl_tx_reason` already uses 23 | the 10th new TX reason forces an ABI break of the stats block |
| F10 | `margin_min` is stored in `struct mtl_window_max` | naming |

### 1.5 What the examples actually read

The 13 examples use `mtl_last_error`, `mtl_error_name`, `mtl_reason_name`, the CQ status and reason, one `mtl_eq_read` for `SESSION_RETIRED`, and `info.pool_count`. None reads stats, port, scheduler,
memory or time status. That is the shape of the split in §3: the core is what a media loop branches on; everything else is diagnostics.

## 2. Proposals

### P1 — One stats registry (H1) — RECOMMEND

**Change.** Every observable number is a named value of an object (`struct mtl_object`, which already exists for EQ subscriptions and fault injection, so C5 §2.9's objection to an untyped `uint64_t
object` does not return). An object has a schema, enumerable with `mtl_stat_list`; each descriptor gives a stable name, an optional label, kind (COUNTER, GAUGE, CONST, HIST), unit and the slot of its
first value. `mtl_stat_read` copies a range of slots into an `int64_t` array with one snapshot time; `mtl_stat_find` resolves `"name{label}"` to a slot once; `mtl_stat_get` is the one-line
convenience. The fixed stats structs, the two unions, the five essence tails, the 64-bit unsupported mask and the frozen reason slot counts are deleted. A value the backend cannot produce is **absent
from the schema** (find returns `-MTL_ENOTSUP`), never a silent 0 (fixes F2).

Before:

```c
struct mtl_session_stats st;
mtl_session_stats_init(&st);
mtl_session_get_stats(s, &st); /* 2,096 B */
uint64_t late = st.dir.tx.units_late, too_late = st.dir.tx.units_dropped[MTL_TX_REASON_TOO_LATE];
uint64_t miss = 0;
mtl_session_stat_get(s, KEY_NIC_RX_MISSED, 0, &miss); /* second mechanism for "rare" values */
```

After:

```c
int64_t late, too_late;
mtl_stat_get(mtl_object_session(s), "tx.units_late", &late);
mtl_stat_get(mtl_object_session(s), "tx.units_dropped{reason=too_late}", &too_late);

/* exporter: schema once per object, then one DP bulk read per period */
struct mtl_stat_desc d[512];
uint32_t n;
mtl_stat_list(o, d, sizeof(d[0]), 512, &n);
int64_t v[1024], snap_ns;
int got = mtl_stat_read(o, 0, 1024, v, &snap_ns); /* d[i].slot indexes v */
```

**Read rules (unchanged semantics, 08 §2.4).** Per-writer counter blocks summed by the reader; queue gauges from one scan of the slot state words; seqlocked groups (histogram buckets, windowed-max
buckets, the PTP offset/path-delay pair) are contiguous slots, and a read that covers a group is consistent for that group; port, NIC and memory values are polled off the tasklet and read from the
cache. `mtl_stat_read` is DP and takes no lock a tasklet takes (G-40 holds as written); `list`, `find` and `get` are CP (string work). Counters stay cumulative for the life of the object (D-20, G-42).
A histogram is `4 + MTL_HIST_BUCKETS` consecutive slots laid out exactly like today's `struct mtl_histogram` (count, sum, min, max, buckets), which `mtl_observe.h` keeps as an overlay; the descriptor
carries the bucket layout (`bucket_width` 0 = log2), so the reader no longer has to remember which option it set.

**What is lost, and why it is acceptable.**

| Lost | Mitigation |
|---|---|
| compile-time field names (a typo becomes a runtime error) | resolve keys once at setup; `mtl_stat_find` fails loudly; a CI doc-test compares the 08 key catalogue with `mtl_stat_list` on the null backend |
| ABI-stable offsets | stable **names** (the same governance as function names, D-69 deprecation); slots are stable for the object's lifetime and identical for objects of one class on one library (port `nic.*` excepted) |
| typed fields (`uint64_t` vs signed, struct histogram) | one value type, `int64_t` (63 bits of bytes at 100 Gb/s last 23 years); unit and kind in the descriptor; the histogram overlay |
| one call copies "everything" | `mtl_stat_read(o, 0, n, …)` is that call; partial reads are cheaper than the 2 KB copy |
| per-value cost on the data path | same work as filling the struct (sum of writer blocks per slot); no string work on DP |

Gains: growth costs nothing (no reserved tails, no slot caps — F9), per-essence and per-backend values are just more names, labels map directly to Prometheus and OpenTelemetry attributes, bindings get
a dict for free, and a flat `int64_t` array plus schema is the natural shape for the later out-of-process reader (Q-OBS-2).

**Savings.** Functions: `mtl_session_get_stats`, `mtl_session_stats_init`, `mtl_session_stat_get`, `mtl_port_stat_get`, `mtl_instance_stat_get`, old `mtl_stat_list` (−6) for
`mtl_stat_list/find/read/get` (+4). Types: `mtl_tx_stats`, `mtl_rx_stats`, `mtl_dir_stats`, `mtl_leg_stats`, `mtl_queue_gauges`, five essence tails, `mtl_media_stats`, `mtl_session_stats`,
`mtl_window_max`, old `mtl_stat_desc` (−14) for the new `mtl_stat_desc` (+1). Enums: `mtl_stat_object` (→ `mtl_object_kind`) and `mtl_lease_state` (its values become `state=` labels). Defines:
`MTL_TX_REASON_SLOTS`, `MTL_RX_REJECT_SLOTS`. ≈ 215 core lines out, ≈ 60 lines into `mtl_observe.h`.

**Assumptions touched.** D-20 keeps its semantics; its "one stats schema" becomes a runtime schema. 08 §2.1–2.2 field tables become the key catalogue (§3.4). BREAKS: D-15 "validity by flags, never by
zero" for keyed TAI gauges (`leg.last_packet_tai_ns`): they use `INT64_MIN` = never, since a value array has no flag word — needs approval (alternative in §5 Q2).

**Risk.** Low–medium: name governance is new process; the read path is the one 08 §2.4 already requires.

### P2 — Host-side status into the registry (H6) — RECOMMEND

**Change.** The eight host-side output structs become objects of the same registry: `mtl_object_instance(mt)` (instance info and status, memory with a `numa=N` label, capacity that is instance-wide),
`mtl_object_port(mt, p)` (caps as CONST, link and counters as GAUGE/COUNTER, free queues, time status of that port), and a new `mtl_object_sched(mt, i)` (scheduler load). Capacity is split by owner:
queues per port, lcores and quota per instance (the old struct mixed them).

Before:

```c
struct mtl_port_status ps;   mtl_port_status_init(&ps);   mtl_port_get_status(mt, 0, &ps);
struct mtl_sched_status ss;  mtl_sched_status_init(&ss);  mtl_sched_get_status(mt, 3, &ss);
struct mtl_mem_status ms;    mtl_mem_status_init(&ms);    mtl_instance_get_mem_status(mt, 0, &ms);
struct mtl_time_status ts;   mtl_time_status_init(&ts);   mtl_time_get_status(mt, 0, &ts);
```

After:

```c
int64_t up, busy, free_hp, tstate;
mtl_stat_get(mtl_object_port(mt, 0), "port.link_up", &up);
mtl_stat_get(mtl_object_sched(mt, 3), "sched.busy_pct_x100", &busy);
mtl_stat_get(mtl_object_instance(mt), "mem.hugepages_free{numa=0}", &free_hp);
mtl_stat_get(mtl_object_port(mt, 0), "time.state", &tstate); /* enum mtl_time_state */
```

The common "is time usable" question needs no status read at all: `mtl_time_now()` already returns `MTL_TIME_VALID | ESTIMATED | HOLDOVER | HW | SW` and the accuracy. `time.offset_ns` and
`time.path_delay_ns` are one seqlocked group, so a read of both is consistent.

**What must stay typed.** `mtl_time_now/convert/cross_timestamp/user_update/diff_ns` (typed `struct mtl_time`, DP, used on every unit), `mtl_port_count`, `mtl_port_find` and
`mtl_instance_list_sessions` (addressing, not observing), and the enums the values are interpreted with when they are also config or event vocabulary (`mtl_time_state`, `mtl_time_source`,
`mtl_manager_state`, `mtl_pacing_class` and its masks). `mtl_backend`, `mtl_link_source` and `mtl_waker_mode` are read only through keys and move to `mtl_observe.h`.

**Savings.** −16 functions (8 getters, 8 output inits), +1 (`mtl_object_sched` twin); −8 types; ≈ 150 core lines. Closes F5: `port.rx_bytes`, `port.tx_bytes`, `port.rx_nombuf`,
`instance.sessions{essence=…,dir=…}`, `instance.lcores`, `instance.dma_devs` are ordinary keys.

**Assumptions touched.** D-19 (capability query per port) stays, as CONST keys; D-60 capacity stays, as keys plus `MTL_QUERY_CHECK_CAPACITY`. `mtl_object_kind` gains `SCHED`, `REGION`, `TIMELINE`,
`EQ` (P6 needs them).

**Risk.** Low. Port caps are the only values an app might branch on before choosing a config; the dry-run query is the primary answer to "can I have X", and `caps.pacing_classes` is one `mtl_stat_get`
(§5 Q3 keeps a typed caps struct as the fallback).

### P3 — Session info: typed core plus `info.*` keys (H2) — RECOMMEND

**Change.** `mtl_session_info` keeps only what a running app or framework needs to operate, and what the dry-run `mtl_<essence>_session_query` must answer before an object exists (pool sizing, path,
pacing class, latency). It shrinks from 512 B / 64 fields to 256 B / 27 fields plus the per-leg wire identity. Everything else (SDP timing values, transport internals, placement, wake model) becomes
CONST keys in the `info.*` namespace of the live session.

```c
struct mtl_session_info {     /* output (P8: the getter takes its size); 256 B, was 512 */
  uint32_t direction, essence, unit, flags;          /* flags: MTL_INFO_* compliance warnings */
  char name[MTL_NAME_MAX];
  struct mtl_time created;                           /* counters count from here */
  uint32_t pool_count, max_count, min_count_direct, completion_mode;
  uint32_t path, pacing_class, leg_count, pkts_per_unit; /* granted */
  uint32_t buffer_capacity_bytes, unit_samples, max_udw_bytes, meta_max_bytes;
  uint32_t max_user_meta_bytes, codestream_bytes, reserved0[2];
  uint64_t unit_bytes, pool_slot_pitch;
  int64_t latency_min_ns, latency_max_ns, min_submit_lead_ns; /* GStreamer LATENCY, framework lead */
  struct mtl_leg_info leg[MTL_MAX_LEGS];             /* port, SSRC, UDP ports, PT, DSCP, TTL, MACs */
};
```

Moved to keys (§3.4 `info.*`): `sched_index`, `pacing_profile`, `sender_type`, `tsmode`, `source_kind`, `media_mode`, `tx_queue_kind`, `waker_mode`, `backend_syscalls_on_tasklet`, `numa`,
`numa_mismatch`, `internal_buffer_count`, `internal_bytes`, `box_hdr_bytes`, `cvideo_rate_mode`, `cbr_headroom_bytes`, `anc_tm`, `anc_window_frame_offset`, `total_lines`, `max_slot_delay`,
`slot_delay`, `vrx_full`, `cmax`, `seq_restarted`, `troffset_ns`, `trs_ps`, `tsdelay_ns`, `pickup_lead_ns`, `media_time_offset_ns`, `completion_latency_ns`, `expected_wake_latency_ns`,
`tolerated_skew_ns`, `horizon_ns`, `rx_flush_offset_ns`, `convert_context`, `converter`, `ts_refclk.*`. `struct mtl_ts_refclk` goes (the grandmaster is also `time.grandmaster_id` of the port). The
converter plugin name is the label of `info.converter` (fixes F3; §5 Q6).

**Savings.** −1 type, −37 typed fields, 256 B per copy; ≈ 35 lines.

**Assumptions touched.** BREAKS: D-60 "`mtl_session_get_info` carries every granted and SDP value" — the SDP values move to keys, which exist only on a live session, so the dry run no longer reports
SDP timing values or diagnostics. Needs approval; NMOS publishes SDP after create, so no known flow needs them earlier. An L4 `mtl_session_sdp()` helper (Q-MODE-6) would be the typed consumer.

**Risk.** Low–medium: SDP generators read ≈ 15 keys instead of fields.

### P4 — Lean session status (H2) — RECOMMEND

```c
#define MTL_STATUS_RX_SIGNAL 0x1u          /* RX: packets within rx_signal_timeout */
#define MTL_STATUS_FORMAT_CHANGED 0x2u     /* RX: detected != config; rx.format_changed, rx.detected.* */
#define MTL_STATUS_TIMEBASE_SUSPECT 0x4u   /* RX: |arrival - media| > 1 s on DIRECT (06 §11) */
#define MTL_STATUS_PACING_DOWNGRADED 0x8u  /* current class != granted; tx.pacing_class, tx.path */
struct mtl_leg_status { uint32_t admin, oper, flow_state, reserved; };  /* 16 B */
struct mtl_session_status {    /* output; 88 B, was 200 */
  uint32_t state, reason, error_reason;  /* error_reason: why it entered ERROR, else NONE */
  int32_t error;                         /* in ERROR: the code data calls return */
  uint32_t blocked_on, flags, leg_count, reserved0;
  struct mtl_timing_warning timing_warning; /* kept typed: the live sink's min_tx_delay fix (C5 §7.5) */
  struct mtl_leg_status leg[MTL_MAX_LEGS];
};
```

Moved: `format_changed` mask, current `pacing_class` and `path`, per-leg `receiving`, `igmp_reports`, `last_packet_tai_ns`, `time_valid` → keys (they were duplicated in leg stats anyway).
`inline_hook` → LATER (F7). The flags give `RX_TIMEBASE_SUSPECT` the getter 07 §3.2 promises (F4). `PACING_CHANGED`'s getter becomes the status flag plus `tx.pacing_class`, which fixes the info/status
mismatch of §1.3. Savings ≈ 15 lines, 112 B per copy. Risk low.

### P5 — One reason vocabulary (H3) — RECOMMEND; own header OPTION

**Change.**

1. Rename `enum mtl_state_reason` to `enum mtl_reason`: it already carries argument, capacity and timing reasons, not only state reasons.
2. Absorb `mtl_tx_reason` as group 500–599 (per-unit TX outcomes). The six duplicates reuse the existing values (`LINK_DOWN` 102, `WAITING_NEIGHBOUR` 107 for `NO_NEIGHBOUR`, `LEG_DISABLED` 109,
   `TX_QUEUE_FATAL` 100, `DEVICE_GONE` 104, `DRAIN_TIMEOUT` 4 for `STOP_TIMEOUT`); 16 TX-only values become 500–515. `mtl_tx_result.reason` is already `uint32_t`; `leg_reason` goes from `uint8_t[2]` +
   `uint16_t reserved0` to `uint16_t[2]`, the same 4 bytes, so `mtl_tx_result` keeps 208 B.
3. Delete `enum mtl_rx_reject`: it appears only as the index of `pkts_rejected[]`; under P1 the causes are label values (`rx.pkts_rejected{cause=ssrc}`), which grow without slot caps.
4. OPTION: move the enum into `mtl/experimental/mtl_reason.h` (included by the core header), generated from one table that also produces `mtl_reason_name()` and the G-57 trigger list, so docs, strings
   and tests cannot drift.

Before / after:

```c
if (r.hdr.status == MTL_TX_DROPPED && r.reason == MTL_TX_REASON_LINK_DOWN) ...  /* two vocabularies */
if (r.hdr.status == MTL_TX_DROPPED && r.reason == MTL_REASON_LINK_DOWN) ...     /* one */
```

**Savings.** −2 enums, 104 → 84 constants, −2 defines, one name function covers everything; ≈ 25 core lines (≈ 100 with the own header). **Assumptions touched.** 07 §1.1.1 and §5.4 merge into one
table; values stay frozen once published. **Risk.** Low. P12 (strings only) is rejected: controllers branch on `CAPACITY_*`, framework sinks on `TIMING_SHORTFALL`, creators on `MANAGER_LOST`.

### P6 — One event record (H4) — RECOMMEND

**Change.** The eleven payload structs and their union become four generic fields whose meaning is a per-type table; `origin_kind` + `origin` become a `struct mtl_object`; `producer_class` goes (with
`USER` gone, P7, tasklet-class sources never overflow and the remedy is "re-read the getters" whichever ring overflowed).

```c
struct mtl_event {             /* EQ record; size is an OUTPUT (C4); 144 B, was 184 */
  uint16_t size, severity;     /* enum mtl_severity */
  uint32_t type;               /* enum mtl_event_type */
  uint32_t reason;             /* enum mtl_reason, NONE if n/a */
  uint32_t coalesced;          /* occurrences this record stands for, >= 1 */
  uint64_t seq;                /* per EQ; a gap means overflow */
  int64_t time_tai_ns;         /* valid if flags & MTL_EVENT_TIME_VALID */
  struct mtl_object origin;    /* session, port, sched, group, region, timeline, eq, instance */
  char origin_name[MTL_NAME_MAX];
  uint32_t flags;              /* MTL_EVENT_TIME_VALID */
  uint32_t index;              /* leg, else 0 */
  uint32_t old_value, new_value;
  int64_t value[2];
};
```

| Type | `old → new` | `value[0]`, `value[1]` |
|---|---|---|
| `SESSION_STATE` | state | — |
| `SESSION_RECOVERY`, `TX_UNDERRUN`, `RX_SIGNAL`, `RX_TIMEBASE_SUSPECT`, `SCHED_OVERLOAD` | 0 → 1 begin (present), 1 → 0 end | units dropped / slots empty / — / offset ns / busy % × 100 |
| `BACKPRESSURE` | `blocked_on` (NONE → X is begin, X → NONE is end) | — |
| `LEG_STATE` (`index` = leg) | oper | admin |
| `FLOW_STATE` (`index` = leg) | flow state | `first_index` after `update_flows` |
| `RX_FORMAT` | changed-property mask in `new` | values: `rx.detected.*` keys (F4) |
| `PACING_CHANGED` | pacing class | — |
| `TIMING_INFEASIBLE` | — | shortfall ns, suggested `min_tx_delay_ns` |
| `TIME_STATE` (origin port) | time state | offset ns |
| `TIME_STEP` | — | step ns, policy |
| `PORT_LINK` (origin port) | up | speed Mb/s |
| `MANAGER_LOST` | manager state | — |
| `GROUP_MEMBER_FAILED` (origin group) | — | member session ID |
| `OVERFLOW` (origin EQ) | — | lost |

**Savings.** −12 types, −2 enums (`mtl_origin_kind`, `mtl_producer_class`), 40 B per record, ≈ 60 lines; the event table in 07 §3.2 gains two columns and loses the payload column. **Assumptions
touched.** D-61 "OVERFLOW with `producer_class`" (C5 §7.5 remedy, motivated by `USER` sharing a ring — gone with P7). **Risk.** Low: typed access `ev.u.flow.state` becomes `ev.new_value`; one table to
learn instead of eleven structs.

### P7 — Fewer event routes and types (H4) — RECOMMEND; EPOCH_TICK OPTION

| Item | Change | Why it is safe |
|---|---|---|
| `mtl_eq_post`, `MTL_EVENT_USER`, `MTL_EQ_SUB_USER`, `MTL_ORIGIN_APP`, the USER ring | remove | not a capability MTL has today; `mtl_eq_interrupt` wakes a blocked reader, and an app that wants its own messages in the same loop adds its own eventfd to its epoll set next to `mtl_eq_get_wait_object` |
| `mtl_instance_get_eq` | remove | the shared instance EQ is the "one reader" hazard C5 §7.5 described; `mtl_eq_create(mt, {.mask = PORT \| TIME \| INSTANCE \| SESSION})` gives each consumer its own copy |
| `mtl_session_bind_eq` | remove | `mtl_eq_subscribe(eq, mtl_object_session(s), MTL_EQ_SUB_SESSION)` routes a copy; the private EQ is pending state that costs nothing unread (07 §3.3) |
| `SESSION_MIGRATED`, `INLINE_HOOK_DISABLED` | move to `MTL_UNIFIED_LATER` (numbers reserved) | no producer in 0.1 (F7) |
| `RX_DISCONTINUITY` | remove; keep the lossless per-unit `MTL_RXT_DISCONTINUITY` flag and add counter `rx.discontinuities` | the CQ flag is already the getter of record, and an RX session always has unit records |
| `EPOCH_TICK`, `MTL_EQ_SUB_EPOCH_TICK` | OPTION: replace by `mtl_timeline_index_at` + a sleep to the next boundary | lossy by design; **CUT - needs approval** (today's `ST_EVENT_VSYNC`) |

Result: 26 types → 22 (21 with the option), 6 subscription bits → 5 (4 with the option), 13 EQ functions → 9 (`eq_config_init` goes with P8). **Assumptions touched.** BREAKS: D-61 (instance EQ default subscriptions,
`USER` posts in their own ring) — needs approval; 07 §3.4 table shrinks to two rows (private EQ, app-created EQ). **Risk.** Low.

### P8 — ABI mechanics: kinds, known sizes, initialisers (H5) — RECOMMEND

**Change.**

1. Delete `enum mtl_struct_kind` (44 values) and `mtl_struct_known_size`. Field-level detection for **inputs** is `mtl_version_num() >= MTL_VERSION_NUM(0, N, 0)` against the field's `since 0.N` marker
   (11 §2.3 already lists it as the alternative), and the strict rule still turns a mistake into `-MTL_EINVAL`/`NONZERO_TAIL`. For **outputs** the rule is "zero means not reported": every output enum
   starts at `*_UNSET = 0`.
2. Replace the 37 exported `*_init()` (F1) by one exported `mtl_struct_init(void* p, size_t size)` (zero `size` bytes, stamp `struct_size = size`) and one macro `MTL_INIT(p)` = `mtl_struct_init((p),
   sizeof(*(p)))`, valid in C and C++. This is correct across versions because the size comes from the caller's compilation. The zero-default rule (D-22) is what makes a per-struct init unnecessary:
   there is nothing to set but the size.
3. Replace the 25 `MTL_<STRUCT>_INIT(...)` value macros by the generic one the header already has internally: `MTL_VALUE(mtl_tx_submission, .media_index = k)` (C only).
4. Output structs lose `struct_size` and reserved tails; their getters take the size like array reads already do (C4): `mtl_session_get_status(s, &st, sizeof(st))`. The library writes `min(size,
   known)` and zero-fills the rest, so an uninitialised output struct is fine and growth needs no reserved space. One rule: **inputs carry `struct_size`, outputs are told their size** — the same rule
   CQ and EQ reads use.

Before / after:

```c
struct mtl_session_config sc;  mtl_session_config_init(&sc);          /* one of 37 */
if (mtl_struct_known_size(MTL_STRUCT_SESSION_CONFIG) >= need) sc.new_field = v;
struct mtl_session_status st;  mtl_session_status_init(&st);  mtl_session_get_status(s, &st);

struct mtl_session_config sc;  MTL_INIT(&sc);                          /* every input struct */
if (mtl_version_num() >= MTL_VERSION_NUM(0, 3, 0)) sc.new_field = v;
struct mtl_session_status st;  mtl_session_get_status(s, &st, sizeof(st));
```

**Savings.** −37 + 1 = −36 init functions (8 counted in P2, 1 in P1, so −27 here), −1 `known_size`; −44 constants; −24 value macros; output-struct reserved tails; ≈ 120 lines; `check.sh` drops the
`*_init` coverage lint and the G-xx "`known_size` agrees for every kind" test. **Assumptions touched.** D-22 / A7 ("an exported `*_init()` for every input struct"), A3 ("`struct_size` on single
structs"), C5 §2.11 remedy (field-level detection: still field-level, by version) — need approval. Coordinate the output-getter signature change with the other simplification notes, since
`buffer_requirements`, `buffer_view`, `slot_hint`, `timeline_info`, `mem_info` and the query outputs are outside S5. **Risk.** Low; it removes F1. The hypothesis "one generic `mtl_init_struct(kind,
ptr)`" is rejected in favour of a size argument: a kind cannot tell the library the caller's `sizeof`.

### P9 — errno-style last error — OPTION

Today: every call increments a per-thread `call_seq`; the app reads `mtl_call_seq()` before a call and compares after. Alternative: `mtl_last_error` describes the most recent failing call on this
thread and is valid until the next non-AS MTL call (errno's contract; AS calls such as `mtl_error_name` never touch it). Saves `mtl_call_seq`, 8 B and a TLS increment on every DP call. Loses detection
when a wrapper makes other MTL calls between the failure and the read. Touches C5 §2.16's remedy. Codes and their meanings are unchanged, so timeout (`ETIMEDOUT`), app stop (`ESHUTDOWN`), failure
(`EIO`) and device gone (`ENODEV`) stay distinguishable.

### P10 — Typed summary counters — OPTION (default: do not add)

A 96 B `struct mtl_session_counters { units, units_ok, units_late, units_dropped, pkts, bytes, pkts_lost, snapshot }` with direction-neutral meanings would ease migration from `st20_tx_user_stats`.
Against it: a second mechanism again, and "late" or "dropped" means different things per direction. `mtl_stat_get` with three names is as short.

### P11, P12, P13, P14

- **P11 — REJECT.** Packing the reason into the return value (`-(reason << 8 | code)`, HRESULT-style) breaks "values equal Linux errno" (D-21) and every `ret == -MTL_EAGAIN` test.
- **P12 — REJECT.** Strings-only reasons: see P5.
- **P13 — REJECT.** Keys for session state, `blocked_on`, `error`: these are what the data loop branches on after `-MTL_EAGAIN` or `-MTL_EIO`; they stay typed (`mtl_session_get_state` is DP and
  returns the state).
- **P14 — OPTION.** `LEG_STATE` and `FLOW_STATE` are both per-leg state with one getter (`status.leg[i]`); one `LEG_STATE` with a `what` bit in `flags` saves a type and a test row, at some loss of
  readability.

## 3. The result

### 3.1 Lean core error and status surface (`mtl_unified.h`)

```c
/* §2 codes: unchanged, 16 MTL_E* values equal to Linux errno */
#include "mtl_reason.h"            /* enum mtl_reason (P5); or inline in this header */
enum mtl_blocked_on { MTL_BLOCKED_NONE, BUFFERS, RESULTS, RING, APP_LEASES, APP_HOLDS }; /* unchanged */

struct mtl_error_info { int32_t code; uint32_t reason; uint64_t call_seq; char detail[MTL_ERROR_DETAIL_MAX]; };
MTL_API_DP int mtl_last_error(struct mtl_error_info* out, size_t size);
MTL_API_DP uint64_t mtl_call_seq(void);                 /* P9 option removes it */
MTL_API_AS const char* mtl_error_name(int code);
MTL_API_AS const char* mtl_reason_name(uint32_t reason);

/* ABI (P8) */
MTL_API_AS uint32_t mtl_version_num(void);
MTL_API_AS void mtl_struct_init(void* p, size_t size);
#define MTL_INIT(p) mtl_struct_init((p), sizeof(*(p)))

/* Objects (existing; kinds extended) */
enum mtl_object_kind { MTL_OBJECT_UNSET, INSTANCE, PORT, SESSION, GROUP, SCHED, REGION, TIMELINE, EQ };
MTL_API_AS struct mtl_object mtl_object_sched(mtl_instance_h mt, uint32_t sched); /* + inline */

/* Session state, status, info (P3, P4) */
MTL_API_DP int mtl_session_get_state(mtl_session_h s);
MTL_API_CP int mtl_session_get_status(mtl_session_h s, struct mtl_session_status* st, size_t size);
MTL_API_CP int mtl_session_get_info(mtl_session_h s, struct mtl_session_info* info, size_t size);
MTL_API_CP int mtl_instance_list_sessions(mtl_instance_h mt, mtl_session_h* h, uint32_t cap, uint32_t* n);
MTL_API_CP int mtl_port_count(mtl_instance_h mt, uint32_t* n);
MTL_API_CP int mtl_port_find(mtl_instance_h mt, const char* name_bdf_or_ip, uint32_t* port);

/* Time (06; status values are keys, P2) */
enum mtl_time_state { UNSET, FREERUN, ACQUIRING, LOCKED, HOLDOVER, LOST };      /* event vocabulary */
MTL_API_DP int mtl_time_now(mtl_instance_h mt, struct mtl_time* now);         /* flags answer "usable?" */
MTL_API_DP int mtl_time_convert(mtl_instance_h mt, const struct mtl_time* in, uint32_t to_clock, struct mtl_time* out);
MTL_API_DP int mtl_time_cross_timestamp(mtl_instance_h mt, int64_t* tai, int64_t* mono, int64_t* real);
MTL_API_CP int mtl_time_user_update(mtl_instance_h mt, int64_t tai_ns, int64_t monotonic_ns);
MTL_API_AS int mtl_time_diff_ns(int64_t a, int64_t b, uint32_t time_valid, uint32_t required, int64_t* out);

/* Events (P6, P7): 22 types, 5 subscription bits (EPOCH_TICK's not shown), one record, 9 functions */
enum mtl_event_type { ... };   enum mtl_severity { INFO, WARNING, ERROR, FATAL };
#define MTL_EQ_SUB_PORT 0x1u
#define MTL_EQ_SUB_TIME 0x2u
#define MTL_EQ_SUB_INSTANCE 0x4u
#define MTL_EQ_SUB_SESSION 0x8u
struct mtl_event { ... };      struct mtl_eq_config { uint32_t struct_size, capacity; uint64_t mask; uint64_t reserved[4]; };
mtl_eq_create, mtl_eq_destroy, mtl_eq_subscribe, mtl_eq_read, mtl_eq_get_wait_object, mtl_eq_trywait,
mtl_eq_interrupt, mtl_eq_uninterrupt, mtl_session_get_eq
```

Count for this area: 33 exported functions (was 57), 8 struct types (error info, session info, leg info, leg status, timing warning, session status, event, EQ config; was 45), and the enums an app
branches on.

### 3.2 Optional header outline (`mtl/experimental/mtl_observe.h`)

```c
/* mtl_observe.h - stats, diagnostics and capability values (since 0.1). Optional: a media
   loop never needs it. Every value is int64_t; names are the stable contract (08 key catalogue). */
#include "mtl_unified.h"

enum mtl_stat_kind { MTL_STAT_COUNTER = 1, MTL_STAT_GAUGE = 2, MTL_STAT_CONST = 3, MTL_STAT_HIST = 4 };
enum mtl_stat_unit { MTL_UNIT_COUNT, NS, PS, BYTES, PKTS, SAMPLES, PCT_X100, MBPS, TAI_NS /* INT64_MIN = never */,
                     ENUM, MASK, BOOL, ID };
#define MTL_STAT_ESTIMATE 0x1u /* e.g. rx.pkts_lost_est on a backend without exact loss */
#define MTL_STAT_CACHED 0x2u   /* polled off the tasklet; <scope>.sampled_tai_ns says when */
struct mtl_stat_desc {         /* 96 B, output of mtl_stat_list */
  char name[48];               /* "tx.units_dropped" */
  char label[24];              /* "reason=too_late", "leg=1", "" */
  uint32_t slot;               /* first value in mtl_stat_read */
  uint16_t width, kind, unit, flags; /* width 1, or 4 + MTL_HIST_BUCKETS */
  uint32_t reserved;
  int64_t bucket_width;        /* HIST: 0 = log2, else linear width in `unit` */
};
MTL_API_CP int mtl_stat_list(struct mtl_object o, struct mtl_stat_desc* d, size_t desc_size, uint32_t cap, uint32_t* n);
MTL_API_CP int mtl_stat_find(struct mtl_object o, const char* key, uint32_t* slot); /* "name" or "name{label}" */
MTL_API_DP int mtl_stat_read(struct mtl_object o, uint32_t first, uint32_t count, int64_t* values,
                             int64_t* MTL_NULLABLE snapshot_tai_ns);  /* count written, >= 0 */
MTL_API_CP int mtl_stat_get(struct mtl_object o, const char* key, int64_t* value);

struct mtl_histogram { uint64_t count; int64_t sum, min, max; uint64_t bucket[MTL_HIST_BUCKETS]; }; /* overlay */
static inline const struct mtl_histogram* mtl_stat_hist(const int64_t* v, const struct mtl_stat_desc* d);

enum mtl_backend { ... };  enum mtl_link_source { ... };  enum mtl_waker_mode { ... }; /* moved from core */
/* histogram layout option keys MTL_OPT_HIST_LINEAR_{MARGIN, LAUNCH_ERROR, LATENCY, DELIVERY, VRX, CINST}
   move here from core (they configure observability only); MTL_OPT_HIST_LINEAR alias deleted (F8) */
```

≈ 90 lines with comments. No key list lives in a header: the schema is runtime-enumerable and the catalogue is a table in 08, checked against `mtl_stat_list` in CI.

### 3.3 Key naming scheme

- Key = `scope.metric`, lower case, `[a-z0-9_.]`, ≤ 47 characters; lookup accepts `scope.metric{label}`, Prometheus syntax. One label string of `k=v[,k=v]`, ≤ 23 characters.
- Scopes: `tx`, `rx`, `leg`, `queue`, `tp`, `video`, `cvideo`, `audio`, `anc`, `fastmeta`, `info` (session CONST); `port`, `caps`, `nic` (driver xstats, names as the driver gives them), `time` (port
  objects); `sched`; `instance`, `mem`, `capacity` (instance object).
- Units appear as a suffix when not a plain count (`_ns`, `_ps`, `_bytes`, `_pct_x100`, `_mbps`) and in `unit`. Exporters append `_total` to COUNTERs; the library never does.
- Label keys: `leg`, `reason` (lower-case `mtl_reason_name` without the prefix), `cause`, `state` (lease state), `window` (`1s`, `60s`), `numa`, `on`, `essence`, `dir`, `name`.
- A published name never changes meaning; removal follows the D-69 deprecation policy; adding names is never an ABI event. Every counter is cumulative for the object's life.

### 3.4 Keys replacing the struct fields

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
| session `info.` C | the 37 names listed in P3 (`trs_ps`, `troffset_ns`, `sched_index`, …), plus `ts_refclk.gm_identity` (unit ID), `ts_refclk.domain`, `ts_refclk.kind`, `converter{name=<plugin>}` |
| port `port.` | `link_up` G, `speed_mbps` G, `rl_queues_in_use` G, `resets`, `rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`, `rx_errors`, `tx_errors`, `rx_missed`, `rx_nombuf` (new, F5), `sampled_tai_ns` G, `free_tx_queues` G, `free_rx_queues` G, `free_rl_queues` G |
| port `caps.` C | `backend`, `pacing_classes`, `numa`, `tx_multi_seg`, `hw_rx_timestamp`, `hw_tx_timestamp`, `launch_time_offload`, `dma_engines`, `header_split`, `max_rl_queues`, `iova_va`, `backend_syscalls_on_tasklet`, `max_regions`, `link_source`, `page_size`, `hugepage_sizes`, `max_sessions{essence}` |
| port `time.` | `source` G, `state` G, `ptp_domain` G, `offset_ns` G, `path_delay_ns` G, `last_sync_age_ns` G, `grandmaster_id` G (unit ID), `utc_offset_s` G, `step_count` |
| port `nic.` | driver xstats as today's keyed design (`nic.rx_missed`, `rl.shaper_drops`, …) |
| sched `sched.` | `lcore` C, `sessions` G, `busy_pct_x100` G, `sleep_ratio_x100` G, `waker_cpu_pct_x100` G, `quota_used_1080p_x100` G, `loop_avg_ns` G, `loop_max_ns` G, `tasklet_p9999_ns` G, `tasklet_max_ns` G |
| instance `instance.` | `api_version` C, `sched_count` C, `time_source` C, `flags` C, `ignored_fields` C, `debug_api` C, `manager` G, `refcount` G, `sessions` G, `sessions{essence,dir}` G (new, F5), `sessions_destroying` G, `regions` G, `regions_used` G, `regions_free` G, `lcores` G, `dma_devs` G (new, F5) |
| instance `mem.` (`numa=N`) | `hugepage_size` C, `hugepages_total` G, `hugepages_free` G, `library_bytes` G, `internal_bytes` G, `imported_bytes` G, `largest_free_segment` G |
| instance `capacity.` | `free_lcores` G, `free_sessions_per_sched` G, `quota_free_1080p_x100` G, `quota_max_one_sched_x100` G |

### 3.5 Coverage against today's MTL and C5 §8

| Today (`include/`) or C5 need | Unified after S5 |
|---|---|
| `st20/22/30/40/41(p)_{tx,rx}_get_session_stats` | session `tx.`/`rx.`/`leg.`/essence keys (ST22 gains stats it lacks today) |
| `*_reset_session_stats`, `mtl_reset_port_stats` | not in the unified API since D-20 (readers subtract); legacy keeps them — unchanged by S5 |
| `mtl_get_port_stats` (8 counters) | `port.*`, all eight present (F5 closed) |
| `mtl_get_fix_info`, `mtl_get_var_info`, `st_get_var_info`, `mtl_get_numa_id` | `instance.*`, `caps.numa`, `instance.sessions{essence,dir}` |
| `mtl_ptp_read_time(_raw)`, `ptp_sync_notify` | `mtl_time_now` / `mtl_time_convert`; `TIME_STATE`, `TIME_STEP`; `time.*` |
| `mtl_is_manager_alive` | `instance.manager`; `MANAGER_LOST` |
| `stat_dump_cb_fn` | exporter: `mtl_instance_list_sessions` + `mtl_stat_list` once + `mtl_stat_read` per period |
| `st20p_*_get_sch_idx`, `st20p_tx_get_pacing_params` | `info.sched_index`; `info.troffset_ns`, `info.trs_ps`, `info.vrx_full` |
| `notify_event(VSYNC)` | `EPOCH_TICK` (kept unless the P7 option is approved) |
| `notify_event(RECOVERY_ERROR, FATAL_ERROR)` | `SESSION_RECOVERY`, `SESSION_STATE` → ERROR, reasons |
| `notify_frame_late` | per-unit result, `tx.units_late`, `tx.margin_ns` H |
| `notify_detected` | `RX_FORMAT` + `rx.detected.*` (getter added, F4) |
| timing parser meta / `_STAT` / ST30 `notify_timing_parser_result` / `timing_parser_critical` | per-unit later (09); summary `tp.*` incl. audio metrics and `tp.pass.*` (F6 closed) |
| C5 §8.7 enumeration, stable names, exporter | session names (unchanged) + `mtl_stat_list` names and labels: exporters need no per-struct mapping code |
| C5 §8.11 capacity | `port.free_*`, `capacity.*`, `MTL_QUERY_CHECK_CAPACITY` (unchanged) |
| C5 §8.12 linear histograms, TP summary | options kept; the layout is visible in `mtl_stat_desc.bucket_width` |
| reads never lock or delay the tasklet (G-40) | `mtl_stat_read` is the 08 §2.4 read path; `list/find/get` touch no tasklet state |

## 4. Cuts needing approval

| Item | Kind | Today's capability lost? | Alternative left |
|---|---|---|---|
| `EPOCH_TICK` (P7 option) | **CUT - needs approval** | yes, `ST_EVENT_VSYNC` | `mtl_timeline_index_at` + sleep to the next boundary on the app thread; or keep the event (then no cut) |
| `mtl_eq_post` / `USER`, instance EQ, `mtl_session_bind_eq` (P7) | design reduction, reverses part of D-61 | no (not in MTL today) | own eventfd in the app's poll set; `mtl_eq_create` with a mask; `mtl_eq_subscribe` |
| `RX_DISCONTINUITY` event (P7) | design reduction | no | per-unit flag (lossless) + `rx.discontinuities` |
| `producer_class` in `OVERFLOW` (P6) | design reduction of a C5 §7.5 remedy | no | `lost` count; resync by getters as before |
| SDP and diagnostic values from the dry-run query (P3) | **BREAKS: D-60** | no (no dry run today) | `info.*` on the created (not started) session |
| typed stats offsets (P1) | **BREAKS: D-20 / 08 §2.1 shape** (semantics kept) | no | stable names, runtime schema |
| validity flag for keyed TAI gauges (P1) | **BREAKS: D-15** | no | `INT64_MIN` sentinel, or a companion `*_valid` key (§5 Q2) |
| `mtl_struct_known_size`, exported per-struct `*_init`, `struct_size` on outputs (P8) | **BREAKS: D-22 / A3 / A7**, C5 §2.11 remedy | no | version check + `since` markers; `mtl_struct_init`; size argument |
| `mtl_call_seq` (P9 option) | C5 §2.16 remedy | no | errno contract |

No other capability of today's MTL is removed; F4, F5 and F6 add coverage the sketch was missing.

## 5. Open questions

1. **Value type.** `int64_t` for every value (proposed) or `uint64_t` with a SIGNED flag? `int64_t` keeps gauges such as `audio.drift_samples` and `margin_min_ns` natural; counters lose one bit.
2. **Validity of keyed times (D-15).** `INT64_MIN` = never (proposed), or a companion `leg.last_packet_valid` key, or a per-read validity bitmap argument to `mtl_stat_read`?
3. **Port caps.** Keys only (proposed), or keep a typed `mtl_port_caps` because apps choose pacing and memory settings from it before any session exists?
4. **Dry-run diagnostics.** Is losing `info.*` from `mtl_<essence>_session_query` acceptable (P3), or should the query return a short-lived query object whose keys can be read?
5. **Status class.** Once lean (P4), can `mtl_session_get_status` become DP, so a GStreamer streaming thread reads it after `-MTL_EAGAIN` without a CP call?
6. **Converter name.** As the label of `info.converter` (proposed), or a `char converter_name[32]` in the core info?
7. **`EPOCH_TICK`.** Keep (no cut) or replace (cut, §4)?
8. **Typed summary (P10).** Wanted for migration from `st_tx_user_stats`, or names only?
9. **Compile-time names.** Should a generated `mtl_stat_names.h` with `#define MTL_K_TX_UNITS_LATE "tx.units_late"` ship for typo safety, or is the CI catalogue check enough (proposed)?
10. **Reasons header (P5.4).** Separate generated `mtl_reason.h`, or keep the enum inline?
11. **Last error (P9).** Keep `call_seq` or adopt the errno contract?
12. **Multi-label keys.** Is one label string `k=v[,k=v]` of 23 characters enough (`essence=fastmeta,dir=tx` is 22), or should descriptors carry two label fields?

## 6. Net effect

| Measure | Before | After (RECOMMEND set) |
|---|---|---|
| exported functions, whole header | 217 | ≈ 169 (this area 57 → 33; the rest is the 37 → 1 initialiser change) |
| struct/union types in this area | 45 | 10 (8 core + descriptor + histogram overlay) |
| enum constants in this area | ≈ 295 | ≈ 195 (≈ 15 more move to `mtl_observe.h`) |
| core header lines in this area | ≈ 1,140 | ≈ 500, plus ≈ 90 in `mtl_observe.h` |
| bytes per "everything about my session" read | 2,096 + 512 + 200 | as many `int64_t` as the app asks for; 256 + 88 typed |
| mechanisms for a number | 4 (struct field, key, info field, status field) | 2 (typed core for what the loop branches on; one registry for everything else) |
