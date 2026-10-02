# S3 — Configuration surface: simplification study

| | |
|---|---|
| Status | Proposal for maintainer review (simplification pass over revision 3). Nothing here is applied to the header or the design documents |
| Date | 2026-10-01 |
| Scope | header sections 8 (instance, ports), 9 (pool config only), 10 (media configs), 11 (session config), 12 (option keys only), 13 (timeline config), 14 (start, discard, reconfigure, activation) of `sketch/include/mtl/experimental/mtl_unified.h` |
| Inputs | 06 §4–§8, §10.8, §11; 09 §1, §6, §9; 05 §5; 11 §2; C5 §2.14, §2.20, §3.7; C5-response A7, A8; DECISIONS D-22, D-33, D-46; OPEN-QUESTIONS Q-ABI-3, Q-MODE-3, Q-MODE-6 |

Verdicts: **RECOMMEND** (adopt), **OPTION** (sound, the owner picks), **REJECT** (tried, does not
stick). "BREAKS: x" marks a proposal that changes a recorded design assumption x and needs
approval. "CUT" would mark a lost use case; this study found none (§5).

## 0. Summary

| # | Proposal | Saving | Verdict |
|---|---|---|---|
| P1 | Typed core + presence-based key-value options for every knob that no 06 §10.8 recipe needs | session typed leaves 75 → 40 (one leg: 61 → 28); 21 input enums leave the core header; six `*_DEFAULT`/`AUTO` sentinels and `MTL_AUDIO_NO_ABSORB` disappear | RECOMMEND (BREAKS: A8/D-46 placement rule) |
| P2 | One options API on `struct mtl_object` (instance, port, session) with a static, session-free key list | 3 session-only calls → 3 generic calls; discovery before create; instance knobs get the same mechanism | RECOMMEND |
| P3 | Flatten `mtl_session_config`: no sub-structs, no `leg_count`, flags for binary RX pool choices and optional CQE kinds | 792 B → 384 B; 6 nested structs → 0; `sc.timing.media_mode` → `sc.media_mode` | RECOMMEND |
| P4 | A "profile" field, or media mode inferred from the submission | — | REJECT (the two-field derivation already is the profile) |
| P5 | `struct mtl_raster` value type shared by all media configs; ANC and fastmeta inherit it from the group video | ANC/fastmeta configs in a group become all-zero; `total_lines` derived; one parse helper fills it | RECOMMEND |
| P6 | Merge video + cvideo, or anc + fastmeta | — | REJECT; two small merges inside configs RECOMMEND |
| P7 | Kind tag in every media config; optionally one generic create and query | fixes the untagged `reconfigure.media_config`; optional −8 prototypes | tag RECOMMEND; generic create OPTION (BREAKS: Q-MODE-3 shape) |
| P8 | String helpers: `mtl_video_config_parse`, `mtl_option_parse`, SDP parse | P1 app: 4 config statements; plugins forward "mtl-options" generically | RECOMMEND (L4); SDP OPTION (L4, Phase 2) |
| P9 | Instance params: port array by pointer, 10 flags → 5, numeric tuning → instance options | 1496 B → 240 B; `PTP_BUILTIN` flag/`time_source` overlap removed | RECOMMEND |
| P10 | One `struct mtl_when` value type for start and activation | −1 struct, −1 init, −2 enums (+1) | RECOMMEND; discard as scalar args OPTION |
| P11 | Defaults: derive `transport_format` from `app_format`, close the ANC `fps` gap, drop `MTL_TIMELINE_GRID_EXPLICIT` | required fields for a video TX: 7 values | RECOMMEND |

A minimal video TX with P1–P11 (P8 parse helpers) reads, in full:

```c
struct mtl_session_config sc = MTL_SESSION_CONFIG_INIT(.direction = MTL_DIR_TX);
struct mtl_video_config vc = MTL_VIDEO_CONFIG_INIT();
if (mtl_flow_parse("239.168.85.20:20000,pt=112", &sc.flows[0]) < 0 ||
    mtl_video_config_parse("1920x1080p59.94 yuv422_10bit", &vc) < 0) return -1;
ret = mtl_video_session_create(mt, &sc, &vc, &s);
```

## 1. Diagnosis

### 1.1 Counts at revision 3 (header as compiled)

Non-reserved input fields ("typed leaves"), excluding `struct_size`; sizes from section 21.

| Struct | Leaves | Size | Note |
|---|---|---|---|
| `mtl_session_config` (direct) | 7 | 792 B | + 6 embedded sub-structs |
| `mtl_flow` × 2 | 14 each (+ reserved `vlan`) | 104 B each | `fmd_dit`, `fmd_k` are ST 2110-41-only |
| `mtl_pool_config` | 6 | 48 B | 3 of them RX-only |
| `mtl_timing_config` | 23 | 240 B | 10 enum fields, 11 `int64_t` values; 7 RX-only |
| `mtl_completion_config` | 2 | 32 B | |
| `mtl_capability_request` | 4 | 32 B | `hw_pacing` and `pacing_class` overlap, combinations unspecified |
| `mtl_session_options` | 5 | 64 B | |
| **session total** | **75** (61 with one leg) | | C5 counted ≈ 90 at r2 |
| `mtl_video_config` | 10 | 128 B | |
| `mtl_cvideo_config` | 14 | 144 B | `pack_type` has one legal value |
| `mtl_audio_config` | 11 | 120 B | two capacity fields in two units (samples, bytes) |
| `mtl_anc_config` | 10 | 128 B | same size as video: `reconfigure` cannot tell them apart |
| `mtl_fastmeta_config` | 7 | 112 B | |
| `mtl_instance_params` | 12 + 8 × 9 port fields | 1496 B | 1184 B of it is `ports[8]` |
| `mtl_timeline_config`, `mtl_start_params`, `mtl_discard_params`, `mtl_reconfigure_params`, `mtl_activation` | 5, 4, 2, 4, 3 | 80, 64, 56, 64, 40 B | three "when" encodings |

Input enums in this area: 39 (sections 10: 13, 11: 14, pool: 5, timeline: 2, start/activation:
2, instance: 3), about 150 enumerators. Six of them exist only to name "zero = derived":
`MTL_LATE_DEFAULT`, `MTL_UNDERRUN_DEFAULT`, `MTL_TSMODE_DEFAULT`, `MTL_RX_FILL_DEFAULT`,
`MTL_DETECT_DEFAULT`, `MTL_TXQ_AUTO`. The configuration part of the header (sections 8, the
pool part of 9, 10, 11, the option keys of 12, 13, 14) is about 1040 of 3029 lines.

### 1.2 What a video TX app faces

| | Legacy `st20p_tx_ops` | Unified r3 | After this study |
|---|---|---|---|
| typed fields in the structs it fills | 25 + `st_tx_port` 7 | 85 (71 with one leg) | 50 (38 with one leg) |
| flag bits | 14 `ST20P_TX_FLAG_*` | 5 session + 3 flow, 13 option keys | 7 session + 1 flow; options opt-in |
| values a minimal app sets | ≈ 10 (ports, fps, fmt, `framebuff_cnt`) | 7 | 7 (or 2 parse calls) |
| fields the 06 §10.8 recipes set | — | 2–5, under `sc.timing.` | the same, directly on `sc` |

C5's point stands at r3 (11 §2.2 says so): the name surface fell, the decisions per session
roughly doubled, and usability rests on defaults. The 71 typed fields are what a reader must
scan to learn which 7 matter.

### 1.3 Findings (defects independent of any proposal)

| ID | Finding | Fix (proposal) |
|---|---|---|
| F1 | `mtl_reconfigure_params.media_config` is `const void*` with no kind; `mtl_video_config` and `mtl_anc_config` are both 128 B, so a wrong pointer is undetectable | kind tag (P7) |
| F2 | 06 §7.4 says "raise `horizon_ns` through reconfigure in STOPPED", but `mtl_reconfigure_params` has no timing field | `horizon_ns` as an option settable in STOPPED (P1) |
| F3 | `caps.hw_pacing` (ANY/PREFER/REQUIRE/OFF) and `caps.pacing_class` overlap; `hw_pacing = OFF` with `pacing_class = HW_RATE` has no defined result | one class option + one strength (P1) |
| F4 | `MTL_INSTANCE_PTP_BUILTIN` and `time_source = MTL_TIME_SOURCE_PTP_BUILTIN` say the same thing twice; the source without the flag is undefined | drop the flag (P9) |
| F5 | `mtl_timeline_config.grid` is used only with `MTL_TIMELINE_GRID_EXPLICIT`, though `grid` 0/0 already means "derived" | drop the flag (P11) |
| F6 | `mtl_anc_config.fps` zero is neither in 09 §9.1 nor in the required list §9.3 (fastmeta's `rate` has a rule) | same rule as fastmeta (P5, P11) |
| F7 | `mtl_flow.fmd_dit`/`fmd_k` (+2 flow flags) and `mtl_session_config.unit` (ROWS is video-only) are essence-specific fields in common structs, against the A8 rule | move them (P3, P6) |
| F8 | `mtl_session_option_list()` needs a session handle, so GStreamer `class_init`, FFmpeg `AVClass` and bindings cannot enumerate keys before create; `mtl_stat_list()` is session-free | static list (P2) |
| F9 | `MTL_OPT_HIST_LINEAR` is a "deprecated alias" in a header that has not shipped | delete it (P2) |
| F10 | `mtl_activation`, `mtl_start_params` and the submission each encode "NOW / at TAI / at index" differently | `mtl_when` (P10) |

### 1.4 Root causes

1. **The zero-default rule taxes every tuning knob.** A typed field cannot say "unset", so each
   knob needs a zero that means "derived" (09 §9, linted in CI), and a knob whose literal 0 is
   meaningful needs a side flag (`MTL_AUDIO_NO_ABSORB`) or a +1 code (`MTL_NUMA(n)`). About 70
   of the 87 rows of 09 §9.1–9.2 belong to this area.
2. **A8 puts tuning in typed structs.** Only backend knobs are key-value, so every rare semantic
   knob (snap tolerance, off-grid policy, RX flush offset, …) is a field every user reads past.
3. **Nesting for versioning.** Sub-structs with reserved tails cost ≈ 270 B and a second level
   of names.

## 2. Proposals

### P1 — Typed core, presence-based options for the rest (H1)

**Change.** Keep as typed fields only what (a) most apps set, or (b) a 06 §10.8 recipe or a
required-field rule needs. Every other knob becomes a key in one enumerable, typed key-value
space. Options are passed at create in an array in the config (so the dry-run query and
create-time resources see them), and may be changed in CREATED/STOPPED with the P2 setter.

```c
struct mtl_option { /* value type; reserved: 0, a future scope index (leg, port) */
  uint32_t key;     /* MTL_OPT_* (mtl_options.h) */
  uint32_t reserved;
  int64_t value;    /* enum value, boolean 0/1, count, or ns */
};
/* mtl_session_config and mtl_instance_params gain, copied at create: */
const struct mtl_option* options; uint32_t option_count;
```

The criterion is testable: **every 06 §10.8 recipe, the MXL bridge, the forwarder and the
A/V/ANC playout are expressible with typed fields only.** Options express deviations from
standard behaviour (SEND_LATE, LOCKED_PHASE, a longer horizon, PASSTHROUGH) and tuning.

**Before/after** (deep-buffer playout that also wants bounded late sends):

```c
/* r3 */
sc.timing.media_mode = MTL_MEDIA_INDEX;
sc.timing.horizon_ns = MTL_SEC(5);
sc.timing.late_policy = MTL_LATE_SEND_LATE;
/* P1 */
static const struct mtl_option opts[] = {{MTL_OPT_HORIZON_NS, 0, MTL_SEC(5)},
                                         {MTL_OPT_LATE_POLICY, 0, MTL_LATE_SEND_LATE}};
sc.media_mode = MTL_MEDIA_INDEX;
sc.options = opts;
sc.option_count = 2;
```

**What is lost and how it is mitigated.**

| Loss | Size of the loss | Mitigation |
|---|---|---|
| compile-time field names | none: keys are enum constants, a misspelt key does not compile | — |
| compile-time value types | small: C6 already made every enumerated field `uint32_t`, so `late_policy = MTL_SNAP_LOCKED_PHASE` compiles today too | validation at create/set: `-MTL_EINVAL`, reason `INVALID_ARGUMENT`, detail names the key and the range from its descriptor |
| debugger readability | a key number instead of a field name | the descriptor `name` (`mtl_option_list`); `mtl_get_option` returns the effective value |
| a struct as documentation | the rare knobs are no longer next to the common ones | that is the point; the options header lists them grouped, one line each |
| a typo'd key from a newer header on an older library | — | strict, like `NONZERO_TAIL`: `-MTL_EINVAL`, new reason `OPTION_UNKNOWN`; `mtl_option_find()` is the field-level feature test, simpler than the `offsetof` pattern of 11 §2.3 |

**What is gained.**

- **Presence instead of zero sentinels.** An absent key is the documented default; a present
  key is literal. `audio.absorb_samples = 0` *is* "strict" (no `MTL_AUDIO_NO_ABSORB`),
  `session.numa = 1` *is* node 1 (no `MTL_NUMA()` in sessions), and the six `*_DEFAULT`/`AUTO`
  enumerators disappear. The defaults table shrinks to the typed fields (§3.4); option defaults
  live in the descriptor (`def`), queryable at runtime.
- **Extensibility without struct growth.** A new knob is a new key; no reserved tail is spent,
  no `struct_size` bump, and backends (AF_XDP, a future vendor PMD) get private key ranges
  — the `mtl_open_ext` role of Q-ABI-3 (c) for scalar knobs.
- **Frameworks map 1:1.** GStreamer properties and FFmpeg `AVOption`s are key-value; with
  string names a plugin forwards `mtl-options="timing.horizon_ns=5s"` without code per knob (P8).
- **Leaner core header.** Keys and their value enums move to a generated `mtl_options.h`
  (§3.3); the core header keeps only the enums of typed fields.

**Semantics.** Unchanged: every moved knob keeps its 06/09 meaning, default and derivation;
only its location changes. Options are read on the CP (create, query, `mtl_set_option` in
CREATED/STOPPED) and copied into the session; no tasklet ever looks a key up.

**Savings.** Session typed leaves 75 → 40; video TX 85 → 50 (§1.2). 21 input enums
(≈ 60 enumerators) leave the core header. 09 §9 loses about 40 of its 87 rows.

**Assumptions touched.** BREAKS: A8 / D-46 ("key-value = backend-specific only") becomes
"key-value = everything outside the recipes". D-22's zero-default rule stays for typed fields
and gets an equivalent (presence) for options. **Risk:** medium — two mechanisms instead of
one per knob class, but each knob lives in exactly one place. **Verdict: RECOMMEND.**

### P2 — One options API for every object (supports P1)

**Change.** Replace the three session-only calls with generic ones over the existing
`struct mtl_object` (section 3), and make the key list static like `mtl_stat_list`:

```c
MTL_API_CP int mtl_set_option(struct mtl_object o, uint32_t key, int64_t value); /* CREATED/STOPPED */
MTL_API_CP int mtl_get_option(struct mtl_object o, uint32_t key, int64_t* value); /* effective value */
MTL_API_AS int mtl_option_list(uint32_t object_kind, struct mtl_option_desc* d, uint32_t cap,
                               uint32_t* n); /* no instance, no session */
MTL_API_AS int mtl_option_find(const char* name, struct mtl_option_desc* d); /* -MTL_ENOENT */
#define MTL_OPT_DEFAULT INT64_MIN /* mtl_set_option: back to the default */
```

`mtl_option_desc.reserved` becomes `flags`: direction mask, essence mask, value kind (enum,
bool, count, ns), class `CREATE` (array only) or `STOPPED` (also settable), and the
default-instance merge class (invariant, mergeable, ignored) for instance keys.
`mtl_get_option` returns the *effective* value (derived default, or as changed by
`discard_queued(REBASE)` for `media_index_offset`), so `mtl_session_info` fields that only mirror
an option (`tsmode`, `horizon_ns`, `rx_flush_offset_ns`, `media_time_offset_ns`) could go — a
note for the owner of section 12, not counted here.

**Savings.** Discovery works before create (F8); instance and port knobs need no new mechanism
(P9); F9 is deleted. **Risk:** low. **Verdict: RECOMMEND.**

### P3 — Flatten the session config (H3)

**Change.** Inline the five fields that stay typed from `timing`, the three from `pool` and the
one from `completion`; drop the six sub-structs and their reserved tails; derive the leg count;
turn the binary choices into flag bits (the A8 rule for booleans):

| r3 | P3 |
|---|---|
| `sc.timing.timeline`, `.media_mode`, `.source_kind`, `.min_tx_delay_ns`, `.media_time_offset_ns` | `sc.timeline`, `sc.media_mode`, `sc.source_kind`, `sc.min_tx_delay_ns`, `sc.media_time_offset_ns` |
| `sc.pool.source`, `.count`, `.data_path` | `sc.pool_source`, `sc.pool_count`, `sc.data_path` |
| `sc.pool.rx_overflow = MTL_RX_RECLAIM_OLDEST_READY` | flag `MTL_SESSION_RX_RECLAIM_OLDEST` |
| `sc.pool.rx_slot_select = MTL_RX_SLOT_BY_INDEX` | flag `MTL_SESSION_RX_SLOT_BY_INDEX` |
| `sc.completion.mode`, `.optional_kinds` | `sc.complete_mode`; flags `MTL_SESSION_CQE_SOURCE_RELEASED`, `MTL_SESSION_CQE_RX_MISSING` |
| `sc.leg_count` (0 = derive; a populated `flows[1]` with 1 is an error) | removed: two legs iff `flows[1]` is not all zero (then its `ip` and `udp_port` are required) |
| `sc.unit` | `mtl_video_config.unit` (ROWS is video-only; PACKET_CHUNK stays reserved there) |
| `caps`, `options`, the other 18 timing fields, `rx_fill` | options (P1) |
| `MTL_SESSION_WAKE_DIRECT`, `MTL_SESSION_WAKE_WAKER` | option `session.waker` |

MXL bridge, before and after:

```c
/* r3 */                                          /* P3 */
sc.pool.source = MTL_POOL_ATTACHED;               sc.pool_source = MTL_POOL_ATTACHED;
sc.pool.count = GRAINS;                           sc.pool_count = GRAINS;
sc.pool.rx_slot_select = MTL_RX_SLOT_BY_INDEX;    sc.flags = MTL_SESSION_RX_SLOT_BY_INDEX |
sc.pool.rx_overflow = MTL_RX_RECLAIM_OLDEST_READY;           MTL_SESSION_RX_RECLAIM_OLDEST;
sc.timing.timeline = mtl_timeline_epoch(mt);      /* timeline: null is the epoch already */
```

**Alternatives tried.**

- **B: keep sub-structs, move rare blocks behind `next`** (`struct mtl_timing_ext` with its own
  `struct_size` and kind). Keeps compile-time typing, but every rare knob costs the app a
  chained block, the library a chain walker per create, and bindings a hand-written chain
  builder; scalar knobs gain nothing over P1. **REJECT** for scalars; `next` stays for
  structured groups (RTCP, HDR metadata, progressive details), as D-33 planned.
- **C: all-options** (config = direction + flows + options). Mirrors GStreamer, but turns the
  recipes into key arrays and loses designated-initialiser readability for the 15 values
  most apps set. **REJECT.**
- **D: flows by pointer + count** (`const struct mtl_flow* flows; uint32_t flow_count`), as
  `update_flows` and `reconfigure` already take them. Removes `MTL_MAX_LEGS` from the config ABI
  and saves 160 B, but `mtl_flow_parse(…, &sc.flows[0])` becomes two statements. **OPTION.**

**Savings.** 792 B → 384 B; nesting depth 2 → 1; flow 104 B → 80 B (reserved tail 32 → 16 B,
`fmd_*` moved). **Assumptions touched:** none beyond P1 (sub-structs were a means, not a
decision). **Risk:** low. **Verdict: RECOMMEND** (A); D OPTION.

### P4 — Profiles and media-mode inference (H2)

**Profile field.** Tried: one `profile` (LIVE, PLAYOUT, SINK, CAPTURE, PROCESSOR, GATEWAY)
setting media mode, source kind, delays, late/underrun policy, tsmode and completion. Finding:
late policy, tsmode, snap, absorb, `D_a`, `D_fmd` and completion are *already* derived from
`media_mode` × `source_kind` × pool source (06 §5.1, §7.2, §8; 09 §9). The two fields are the
profile. A third field would add an enum, a mapping table, and override conflicts
(`profile = PLAYOUT` with `media_mode = TAI`). Saving: one field. **REJECT.** Instead, publish
the derivation as a 3 × 3 table in 06 ("you set two values; this is what you get"), with the
recipes as rows.

**Media mode inferred from the submission.** **REJECT**, because the mode is needed before the
first submission:

- `mtl_group_add` rejects AUTO members, and `PASSTHROUGH + AUTO` fails at create — both CP checks
  before any unit exists;
- the late-policy, absorb and snap defaults and the start lead (`min_submit_lead_ns`) are fixed
  at create/start and reported by the dry-run query;
- `media_index = 0` and `media_tai_ns = 0` are legal values, so "which field is set" needs
  valid bits in the submission, and a forgotten bit silently becomes AUTO — the exact hazard
  06 §4.1 removed `[C3 P3-4]`.

Kept: `media_mode` explicit, zero = AUTO.

### P5 — A shared raster, inherited by ANC and fastmeta (H5)

**Change.** One value type carries what is repeated in four configs (`width`, `height`,
`interlaced`, `fps`/`rate`):

```c
struct mtl_raster { /* value type: a video raster, or the associated video of ANC/fastmeta */
  uint32_t width, height, scan, reserved; /* scan: enum mtl_scan */
  struct mtl_rational fps;                /* the FRAME rate, also when interlaced; MTL_FPS_* */
};
MTL_API_CP int mtl_raster_parse(const char* s, struct mtl_raster* out); /* "1080p59.94", "1920x1080i50" */
```

- `mtl_video_config.raster`, `mtl_cvideo_config.raster`: required, as today.
- `mtl_anc_config.video` and `mtl_fastmeta_config.video`: **all zero = the group video member's
  raster at group start**; outside such a group `fps` is required (`FIELD_REQUIRED`), closing F6.
  `width` is ignored for ANC; `height` gives the SDI raster (1125, 750, 2250, 625, 525 per the
  06 §5.2 table), so `total_lines` becomes an override option. With `MTL_FASTMETA_FREE_RUNNING`
  the fastmeta `fps` is its own unit rate, required, as today.
- The group's existing check (ANC/fastmeta rate and raster equal the video member's, 06 §10.3)
  becomes "a non-zero raster must equal it".

**Savings:** in a group, ANC and fastmeta set nothing (ex07's ANC config becomes
`MTL_ANC_CONFIG_INIT()`); `total_lines` leaves the typed config; one parse helper and one
"associated video" rule for all four. **Risk:** low — the same deferred resolution fastmeta `rate` already uses; a dry-run query
outside a group reports the dependent `info` fields as unresolved. **Verdict: RECOMMEND.**

### P6 — Essence merges (H5)

- **video + cvideo with a `codec` field.** Saves 1 struct, 1 init, 2 prototypes. Costs: ST20-only
  (`transport_format`, `packing`, `unit`) and ST22-only (`codestream_bytes`, `rate_mode`) fields in
  one struct with cross-validation; the essence still differs in SDP media type, stats tail,
  pacing model (no VRX) and info fields. The PR #1610 failure mode returns in a weaker form
  (forgetting `codec` creates ST20 unless ST22-only fields are rejected). With the shared raster
  and the codec knobs moved to options, cvideo has 6 typed fields beyond the raster; the merge buys little.
  **REJECT** (Q-MODE-3 stays answered).
- **anc + fastmeta as one "metadata" config.** Overlap is raster, target delay and a capacity;
  payload model (packet table vs data-item bytes), wire format and filters differ. **REJECT.**
- **Inside configs (RECOMMEND):**
  - audio `rx_unit_samples` + `buffer_capacity_bytes` → one `unit_samples` (RX: unit size; TX:
    library-pool capacity per submission; 0 = 10 ms in whole packets). A session has one
    direction, and bytes = samples × channels × sample size, so nothing is lost.
  - fastmeta `data_item_type`/`k_bit` serve both directions: TX sends them; RX filters on them
    when `MTL_FASTMETA_RX_MATCH_DIT`/`_MATCH_K` is set. `mtl_flow.fmd_dit`, `fmd_k` and two flow
    flags go (F7). Runtime change of the filter stays possible through reconfigure in STOPPED;
    today's `st41_rx_update_source` cannot change it either (`st_rx_source_info`, `st_api.h:159`).
  - cvideo `pack_type` is removed: CODESTREAM is its only legal value (`ST22_PACK_SLICE` is
    "not support now", `include/st20_api.h:386`); a key is reserved for SLICE.

### P7 — Kind-tagged media configs; optional generic create

**Tag (RECOMMEND).** Every media config starts `uint32_t struct_size; uint32_t kind;`, where
`kind` is the existing `MTL_STRUCT_*_CONFIG` value set by `mtl_<x>_config_init()` and the
`MTL_<X>_CONFIG_INIT()` macro. It fixes F1 (`reconfigure.media_config` is checked:
`-MTL_EINVAL`, new reason `CONFIG_KIND`) and lets P8's SDP helper fill "whatever config this
m-line needs".

**Generic create (OPTION).** With the tag, the five creates and five queries can be two:

```c
MTL_API_CP int mtl_session_create(mtl_instance_h mt, const struct mtl_session_config* sc,
                                  const void* media_config, mtl_session_h* out);
MTL_API_CP int mtl_session_query(mtl_instance_h mt, const struct mtl_session_config* sc,
                                 const void* media_config, uint64_t flags,
                                 struct mtl_session_info* MTL_NULLABLE info,
                                 struct mtl_buffer_requirements* MTL_NULLABLE req);
```

Saves 8 prototypes (≈ 45 header lines) and one dispatch table in bindings; a sixth essence adds
no function. Costs the compile-time check that an audio config is not passed to a video create
(caught at runtime by the tag). BREAKS: Q-MODE-3's "own create per essence" shape (the essence
stays its own config). **Verdict: OPTION** — RECOMMEND if header size outranks compile-time
typing for the owner.

### P8 — String helpers (H4)

All L4 (pure functions over the typed structs, no new semantics):

| Helper | Fills | Verdict |
|---|---|---|
| `mtl_video_config_parse("1920x1080p59.94 yuv422p10le[ gpm][ nl]", &vc)` | raster, `app_format` (or `transport_format` for a transport name), packing, sender type; works for cvideo with `"… jpegxs 12.5MB"`-style tails left to Phase 2 | RECOMMEND; `mtl_simple_*_open` then takes one format string (7 → 4 arguments) |
| `mtl_option_parse("timing.horizon_ns=5s,timing.late_policy=send_late", opts, cap, &n)` | an option array from names, enum value names and ns/ms/s units | RECOMMEND; GStreamer/FFmpeg expose one `mtl-options` property |
| `mtl_sdp_essence(sdp, m, &e)`, `mtl_sdp_parse(sdp, m, &sc, mc, opts, cap, &n)` | 2022-7 legs (`a=group:DUP`), PT, `source-filter`, raster, format, `PM`, `TP`, `TROFF`, audio rate/channels/ptime, `mediaclk:direct=` → `rx.rtp_offset`, `mediaclk:sender` → `rx.mediaclk` | OPTION: Q-MODE-6 plans it for Phase 2; P1 makes its output complete, P7 its signature generic; own `mtl_sdp.h` |
| a whole-session spec string ("tx 239.1.1.1:20000 1080p59.94 …") | — | REJECT: a mini-language that duplicates three parsers |

The core stays string-free; `mtl_flow_parse` and `mtl_fps_parse` already set the precedent.

### P9 — Instance parameters (H6)

**Change.**

- `ports[MTL_INSTANCE_MAX_PORTS]` (1184 B) → `const struct mtl_port_spec* ports` + `port_count`;
  `MTL_INSTANCE_MAX_PORTS` leaves the struct ABI. A port-list *string* stays the job of
  `mtl_instance_open_simple`; one typed path and one string path, not two typed ones.
- `mtl_port_spec.port_flags` (reserved, must be 0) and half the reserved tail go (148 → 128 B).
- Flags 10 → 5. Kept: `MANAGER_OPTIONAL`, `TASKLET_THREAD`, `TASKLET_SLEEP`, `HW_TIMESTAMP`,
  `BIND_NUMA`. `PTP_BUILTIN` is removed: `time_source = MTL_TIME_SOURCE_PTP_BUILTIN` enables the
  built-in client, and AUTO becomes "PHC, else SYSTEM_TAI" (F4). The three legacy scheduler
  flags (`RX_SEPARATE_VIDEO_LCORE`, `TX_VIDEO_MIGRATE`, `RX_VIDEO_MIGRATE`, which affect only
  legacy sessions on a bridged instance) and `TASKLET_TIME_MEASURE` become instance options.
- `sched_max`, `sched_quota_mbs`, `ptp_domain`, `max_queues`, `cmd_ack_timeout_ns` → instance
  options, each with its merge class in the descriptor (P2). `ignored_fields` keeps one bit per
  remaining typed field; an ignored option is visible as a differing `mtl_get_option` value.

```c
/* r3: p.port_count = 1; snprintf(p.ports[0].name, …); p.ports[0].sip[…] = …;
       p.flags = MTL_INSTANCE_PTP_BUILTIN; p.time_source = MTL_TIME_SOURCE_PTP_BUILTIN; p.sched_max = 4; */
struct mtl_port_spec port = {.name = "0000:af:01.0", .sip = {192, 168, 1, 10}};
struct mtl_option o[] = {{MTL_OPT_INST_SCHED_MAX, 0, 4}};
struct mtl_instance_params p = MTL_INSTANCE_PARAMS_INIT(.port_count = 1, .ports = &port,
    .time_source = MTL_TIME_SOURCE_PTP_BUILTIN, .options = o, .option_count = 1);
```

**Savings.** 1496 B → 240 B; typed leaves 12 → 9 (+ options); 5 flags fewer.
**Assumptions touched:** AUTO's meaning changes (no built-in PTP without asking) — a nod
needed, no use case lost. **Risk:** low. **Verdict: RECOMMEND.**

### P10 — One "when" (H7)

**Change.**

```c
enum mtl_when_kind { MTL_WHEN_NOW = 0, MTL_WHEN_TAI = 1, MTL_WHEN_INDEX = 2 };
struct mtl_when { uint32_t kind; uint32_t reserved; int64_t value; }; /* value type: TAI ns or index */
/* static inline in C and C++: mtl_when_now(), mtl_when_tai(t), mtl_when_index(k) */
struct mtl_start_params {
  uint32_t struct_size;
  uint32_t reserved0;
  struct mtl_when at;
  int64_t preroll_ns; /* extra lead added to the T0 resolution (06 §3.1) */
  uint64_t reserved[4];
};
MTL_API_CP int mtl_session_update_flows(mtl_session_h s, const struct mtl_flow* legs, uint32_t n,
                                        const struct mtl_when* MTL_NULLABLE at); /* NULL = NOW */
```

`enum mtl_start_mode` and `enum mtl_activation_kind` become one enum; `struct mtl_activation`,
its init, `MTL_ACTIVATION_INIT` and `MTL_STRUCT_ACTIVATION` go. The RX restriction (INDEX
activation is TX-only) stays a validation rule. `mtl_discard_params` (`flags`, `first_index`)
is not a "when" (it maps an index to the next feasible slot); making it
`mtl_session_discard_queued(s, uint64_t flags, int64_t first_index)` removes one more struct and
init but freezes the argument list — **OPTION**. `mtl_reconfigure_params` is kept, with the
tagged `media_config` (P7); timing changes in STOPPED go through `mtl_set_option` (F2).

**Savings.** −1 struct, −1 init, −1 macro, −1 struct kind, −1 enum; one encoding for three verbs.
**Risk:** low (a value type cannot grow; `reserved` is the hedge). **Verdict: RECOMMEND.**

### P11 — Defaults that make the zero config right (H8)

| Field | r3 zero | Proposal | Why |
|---|---|---|---|
| `mtl_video_config.transport_format` | required | 0 = the natural transport of `app_format` (v210, Y210, YUV422P10LE → YUV422_10BIT; UYVY, YUV422P → YUV422_8BIT; I420, NV12 → YUV420_8BIT; RGBA, BGRA, RGB8 → RGB_8BIT; YUV444P10LE → YUV444_10BIT; GBRP10LE → RGB_10BIT); both 0 → `FIELD_REQUIRED` | a deterministic table, not a guess; `mtl_simple` already does it |
| `mtl_anc_config` / `mtl_fastmeta_config` raster | ANC: undocumented (F6) | P5 | one rule for both |
| `mtl_timeline_config.flags` | `GRID_EXPLICIT` needed | `grid` non-zero = explicit (F5) | zero already meant "derived" |
| audio `format`, `sample_rate`, `channels`; video raster | required | stay required | a wire format that silently defaults is a receive-garbage bug; 48 kHz/L24/2 ch is common, not universal |
| `rx.incomplete` (option) | DELIVER | keep DELIVER | today's st20p default is to drop (`ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`, `st_pipeline_api.h:610`); with zero-fill and per-unit status DELIVER is right for monitors, and P1 makes DISCARD one key — noted in §6 |

**Required (zero rejected, `FIELD_REQUIRED`):** `direction`; `flows[0].ip` and `.udp_port`, and
both for a non-zero `flows[1]`; video/cvideo `raster.width`, `height`, `fps`; video `transport_format` or
`app_format`; cvideo `codec`, `codestream_bytes`; audio `format`, `sample_rate`, `channels`;
ANC/fastmeta `video.fps` outside a group with a video member (fastmeta: always with
`FREE_RUNNING`); instance `port_count` and each `ports[i].name`; `mtl_group_config.direction`.

## 3. Proposed lean shape

### 3.1 Session config

`struct mtl_flow` keeps its r3 fields and comments minus `fmd_dit`, `fmd_k` and their flags,
with a 16 B reserved tail: 80 B.

```c
/* mtl_session_config.flags; 0x2u and 0x10u (WAKE_*) retired: option session.waker */
#define MTL_SESSION_MT_SUBMIT 0x1u
#define MTL_SESSION_SINGLE_READER 0x4u
#define MTL_SESSION_EXPORT_POOL 0x8u          /* forces ALL, implies MT_SUBMIT */
#define MTL_SESSION_RX_SLOT_BY_INDEX 0x20u    /* was pool.rx_slot_select */
#define MTL_SESSION_RX_RECLAIM_OLDEST 0x40u   /* was pool.rx_overflow */
#define MTL_SESSION_CQE_SOURCE_RELEASED 0x80u /* was completion.optional_kinds */
#define MTL_SESSION_CQE_RX_MISSING 0x100u

struct mtl_session_config { /* 384 B */
  uint32_t struct_size;
  uint32_t direction; /* required */
  char name[MTL_NAME_MAX];
  struct mtl_flow flows[MTL_MAX_LEGS]; /* two legs iff flows[1] is not all zero */
  mtl_timeline_h timeline;             /* null = epoch */
  uint32_t media_mode;                 /* AUTO (0) | INDEX | TAI */
  uint32_t source_kind;                /* PLAYBACK (0) | CAPTURE | GATEWAY */
  int64_t min_tx_delay_ns;             /* 0 = by source kind */
  int64_t media_time_offset_ns;        /* declared latency (TAI) / ns trim (AUTO, INDEX) */
  uint32_t pool_source;                /* LIBRARY (0) | ATTACHED | DYNAMIC */
  uint32_t pool_count;                 /* 0 = by essence */
  uint32_t data_path;                  /* ALLOW_COPY (0) | PREFER_DIRECT | REQUIRE_DIRECT */
  uint32_t complete_mode;              /* 0 = by pool source */
  uint64_t flags;                      /* MTL_SESSION_* */
  uint64_t user_cookie;
  const struct mtl_option* options;
  uint32_t option_count;
  uint32_t reserved0;
  const void* next; /* structured extension blocks; NULL in 0.1 */
  uint64_t reserved[8];
};
```

### 3.2 Media configs

Each starts with `uint32_t struct_size; uint32_t kind;` (P7) and ends with `uint64_t
reserved[6]`; both are elided below.

```c
struct mtl_video_config {
  struct mtl_raster raster;  /* required */
  uint32_t transport_format; /* 0 = from app_format (P11) */
  uint32_t app_format;       /* 0 = the transport format */
  uint32_t packing;          /* GPM_SL (0) | BPM | GPM */
  uint32_t sender_type;      /* N (0) | NL | W */
  uint32_t unit;             /* FRAME (0) | ROWS; PACKET_CHUNK reserved */
  uint32_t reserved0;
  int64_t troffset_ns;       /* 0 = TRODEFAULT; GATEWAY recipe */
};
struct mtl_cvideo_config {
  struct mtl_raster raster;
  uint32_t codec, codestream_bytes; /* required; CBR size or VBR_MAX ceiling, box header included */
  uint32_t rate_mode;               /* CBR (0) | VBR_MAX */
  uint32_t app_format;              /* 0 = the app gives the codestream */
  uint32_t sender_type, reserved0;
  int64_t troffset_ns;
};
struct mtl_audio_config {
  uint32_t format, sample_rate, channels; /* required */
  uint32_t samples_per_packet;            /* 0 = 1 ms class */
  uint32_t unit_samples;                  /* RX unit / TX capacity; 0 = 10 ms in whole packets */
  uint32_t reserved0;
};
struct mtl_anc_config {
  struct mtl_raster video; /* all zero = the group video's; else fps required */
  uint32_t max_udw_bytes;  /* 0 = 64 KiB */
  uint32_t reserved0;
};
#define MTL_FASTMETA_FREE_RUNNING 0x1u
#define MTL_FASTMETA_RX_MATCH_DIT 0x2u /* was MTL_FLOW_MATCH_FMD_DIT */
#define MTL_FASTMETA_RX_MATCH_K 0x4u   /* was MTL_FLOW_MATCH_FMD_K */
struct mtl_fastmeta_config {
  struct mtl_raster video;       /* as ANC; FREE_RUNNING: fps = own rate, required */
  uint32_t data_item_type, k_bit; /* TX: on the wire; RX: filters with RX_MATCH_DIT / _K */
  uint32_t fastmeta_flags;
  uint32_t buffer_capacity_bytes; /* 0 = 64 KiB */
};
```

Typed leaves: video 10, cvideo 10, audio 5, ANC 5, fastmeta 8 (a raster counts 4).

### 3.3 Option keys (everything moved out)

Generated header `mtl_options.h` (keys, value enums, one-line docs) from one table that also
generates the `mtl_option_list` descriptors and the options half of the 09 defaults table.
Keys are grouped by hundreds like `enum mtl_state_reason`; 0x8000–0xFFFF are backend-private.
Class: **S** = in the create array and settable in CREATED/STOPPED; **C** = create array only
(sizes resources). Dir: T, R, TR. Value enums keep their r3 names and numbers, minus the
`*_DEFAULT` sentinels (absence is the default).

| Key | Name | Class | Dir / essence | Values; default when absent |
|---|---|---|---|---|
| 100 | `timing.late_policy` | S | T | DROP, SEND_LATE, RESLOT; AUTO → RESLOT, INDEX/TAI → DROP (RESLOT rejected there) |
| 101 | `timing.late_tolerance_ns` | S | T | ns; `min(TROFFSET, TFRAME − RACTIVE·TFRAME)`, SEND_LATE only |
| 102 | `timing.underrun_policy` | S | T | SKIP, EMPTY_ANC, KEEPALIVE, SILENCE, REPEAT_LAST (later); by essence |
| 103 | `timing.snap_mode` | S | T, TAI mode | NEAREST, LOCKED_PHASE; NEAREST |
| 104 | `timing.snap_tolerance_ns` | S | T | ns; NEAREST TFRAME/8, LOCKED_PHASE TFRAME/4 (audio: one packet) |
| 105 | `timing.off_grid_policy` | S | T, LOCKED_PHASE | RELOCK, REANCHOR, DROP; RELOCK after 3 |
| 106 | `timing.tsmode` | S | T | SAMP, NEW, PRES; derived (06 §5.1) |
| 107 | `timing.rtp_mode` | S | T | DERIVED, PASSTHROUGH (not with AUTO); DERIVED |
| 108 | `timing.horizon_ns` | S | T | ns; 1 s from `max(now, S)` (F2) |
| 109 | `timing.media_index_offset` | S | TR | units; 0; rebased by `discard_queued(REBASE)`, inherited via the timeline |
| 110 | `timing.link_offset_budget_ns` | S | T | ns; none |
| 111 | `video.progressive_late` | S | T video, ROWS | TRUNCATE, PAD, STALL; TRUNCATE |
| 200 | `rx.incomplete` | S | R | DELIVER, DISCARD; DELIVER |
| 201 | `rx.mediaclk` | S | R | DIRECT, SENDER; DIRECT |
| 202 | `rx.rtp_offset` | S | R | ticks (SDP `mediaclk:direct=`); 0 |
| 203 | `rx.link_offset_ns` | S | R | ns; 0 (a group's value overrides) |
| 204 | `rx.flush_offset_ns` | S | R | ns; skew budget with 2 legs, 1 ms with one |
| 205 | `rx.skew_budget_ns` | S | R | ns; 10 ms where the pool allows |
| 206 | `rx.signal_timeout_ns` | S | R | ns; `max(4 × unit period, 20 ms)` |
| 207 | `rx.burst_size` | C | R | packets; 128 |
| 208 | `rx.threads` | C | R | count; auto (2 above 40 Gbps) |
| 209 | `rx.timing_parser` | S | R | bool; 0 (was `MTL_OPT_RX_TIMING_PARSER`) |
| 300 | `pool.rx_fill` | S | R | ZERO_MISSING, NONE; ZERO_MISSING for library pools, NONE for attached |
| 301 | `session.tx_queue` | C | T | DEDICATED, SHARED; by essence (09 §9.1) |
| 302 | `session.numa` | C | TR | node number, literal; the first port's socket |
| 303 | `session.src_port_mode` | S | T | FIXED, RANDOM, MULTI; FIXED |
| 304 | `session.waker` | C | TR | W2_DIRECT, W3_THREAD; by unit period and thread mode (D-68) — replaces `MTL_SESSION_WAKE_*` |
| 400 | `caps.pacing` | C | T | ANY, HW (new: any hardware class), HW_RATE, HW_LAUNCH, SW, BEST_EFFORT; ANY — `hw_pacing = OFF` ≡ SW (F3) |
| 401 | `caps.pacing_req` | C | T | PREFER, REQUIRE; PREFER |
| 402 | `caps.dma` | C | R | PREFER, REQUIRE, OFF; library choice |
| 403 | `caps.hw_timestamps` | C | TR | PREFER, REQUIRE, OFF; library choice |
| 500–503 | `video.disable_bulk` (C), `video.static_pad_p`, `video.start_vrx`, `video.pad_interval` (S) | C/S | T video | bool, bool, packets, packets; r3 behaviour when absent (the r3 `MTL_OPT_VIDEO_*`) |
| 600–602 | `cvideo.plugin_device`, `cvideo.codec_threads`, `cvideo.quality` | C | TR cvideo (quality: T) | AUTO/CPU/GPU/FPGA, count, plugin scale; AUTO and the plugin defaults |
| 603 | `cvideo.pack_type` | — | reserved | for SLICE, when it exists |
| 700 | `audio.absorb_samples` | S | T audio | samples, literal (0 = strict); S for CAPTURE and TAI, 0 for PLAYBACK/GATEWAY INDEX — replaces `MTL_AUDIO_NO_ABSORB` |
| 701 | `audio.launch_offset_ns` | S | T audio | ns (`D_a`); by source kind |
| 702 | `audio.build_pacing` | C | T audio | bool; 0 |
| 703 | `audio.fifo_ms` | C | T audio | ms; 10, at least 2 packets |
| 704 | `audio.rl_warmup` | S | T audio | as r3 `MTL_OPT_AUDIO_RL_WARMUP` |
| 800 | `anc.timing_model` | S | T anc | CTM, LLTM; CTM |
| 801 | `anc.target_delay_ns` | S | T anc | ns; the model's default |
| 802 | `anc.window_anchor` | S | T anc | AUTO, MEDIA; AUTO (06 §5.5) |
| 803 | `anc.total_lines` | S | T anc | lines; from the raster height (P5) |
| 804 | `anc.split_by_packet` | S | T anc | bool; 0 |
| 805 | `anc.max_packets` | C | TR anc | count ≤ 255; 255 |
| 806 | `anc.rx_detect` | S | R anc | OFF, ON; AUTO (interlace detection) |
| 900 | `fastmeta.target_delay_ns` | S | T fastmeta | ns (`D_fmd`); 06 §5.2 |
| 1000–1005 | `hist.linear.margin`, `.launch_error`, `.latency`, `.delivery`, `.vrx`, `.cinst` | S | TR | bucket width; log2 when absent. `MTL_OPT_HIST_LINEAR` deleted (F9) |
| 2000–2003 | `instance.sched_max`, `.sched_quota_mbs`, `.max_queues`, `.cmd_ack_timeout_ns` | I | instance | auto ≤ 18; 12 × 1080p59.94 4:2:2 10-bit; HW maximum; 100 ms — merge: ignored |
| 2004 | `instance.ptp_domain` | I | instance | 0 — invariant if set |
| 2005–2008 | `instance.rx_separate_video_lcore`, `.tx_video_migrate`, `.rx_video_migrate`, `.tasklet_time_measure` | I | instance (the first three act on legacy sessions) | bool; 0 — merge: ignored |

Class **I** = instance array at open only (merge class as listed, 03 §7.2). 57 session keys
(45 moved knobs, 12 existing tuning/observability keys renumbered) and 9 instance keys.

### 3.4 What the typed defaults table keeps

Zero-replaced rows that remain (09 §9.1): `name`, `flows[].port`, `payload_type`, `ttl`,
`udp_src_port`, `ssrc`, `source_filter`, `timeline`, `min_tx_delay_ns`, `pool_count`,
`complete_mode`, `transport_format` (new), `app_format` ×2, `troffset_ns` ×2,
`samples_per_packet`, `unit_samples`, ANC/fastmeta `video` raster, `max_udw_bytes`,
`buffer_capacity_bytes`, the instance and port rows, `grid`, `step_policy`. About 30 rows
instead of 57; the named-mode table (§9.2) keeps its enum rows for typed fields only.

## 4. Field-by-field disposition

Keep = typed, as r3 (possibly renamed/flattened); Option = §3.3 key; Derive = no field, value
derived; Move = typed elsewhere; Remove = no field, no use case lost (§5).

| Field (r3) | Disposition | Note |
|---|---|---|
| `mtl_session_config.direction`, `.name`, `.flags`, `.user_cookie`, `.next` | Keep | flags lose WAKE_*, gain 4 bits |
| `mtl_session_config.unit` | Move → `mtl_video_config.unit` | F7 |
| `mtl_session_config.leg_count` | Derive | `flows[1]` not all zero |
| `mtl_flow.port`, `ip_family`, `payload_type`, `dscp`, `ttl`, `ip`, `source_filter`, `udp_port`, `udp_src_port`, `ssrc`, `dst_mac`, `vlan` (reserved), `flow_flags` | Keep | per leg, IS-05 relevant |
| `mtl_flow.fmd_dit`, `fmd_k`, `MTL_FLOW_MATCH_FMD_DIT`, `_K` | Move → fastmeta `data_item_type`, `k_bit`, `RX_MATCH_*` | P6 |
| `pool.source`, `pool.count`, `pool.data_path` | Keep (flattened) | `pool_source`, `pool_count`, `data_path` |
| `pool.rx_overflow`, `pool.rx_slot_select` | Move → flags | binary choices |
| `pool.rx_fill` | Option 300 | three-state with source-dependent default |
| `timing.timeline`, `.media_mode`, `.source_kind`, `.min_tx_delay_ns`, `.media_time_offset_ns` | Keep (flattened) | the recipe fields |
| `timing.late_policy`, `.late_tolerance_ns`, `.underrun_policy`, `.snap_mode`, `.snap_tolerance_ns`, `.off_grid_policy`, `.tsmode`, `.rtp_mode`, `.horizon_ns`, `.media_index_offset`, `.link_offset_budget_ns` | Option 100–110 | |
| `timing.rx_incomplete`, `.mediaclk_mode`, `.rx_rtp_offset`, `.link_offset_ns`, `.rx_flush_offset_ns`, `.rx_skew_budget_ns`, `.rx_signal_timeout_ns` | Option 200–206 | |
| `completion.mode`; `completion.optional_kinds` | Keep (`complete_mode`); Move → flags | |
| `caps.hw_pacing` + `pacing_class`; `dma_offload`; `hw_timestamps` | Option 400 + 401; 402; 403 | F3; OFF ≡ SW |
| `options.tx_queue`, `.numa`, `.src_port_mode`, `.rx_burst_size`, `.rx_threads` | Option 301, 302, 303, 207, 208 | `numa` without the +1 code |
| `MTL_SESSION_WAKE_DIRECT`, `MTL_SESSION_WAKE_WAKER` | Option 304 | |
| `mtl_video_config.width`, `height`, `interlaced`, `fps`; `transport_format`; `app_format`, `packing`, `sender_type`, `troffset_ns`; `progressive_late` | Keep → `raster` (P5); Keep, 0 = from `app_format` (P11); Keep; Option 111 | |
| `mtl_cvideo_config.width`, `height`, `interlaced`, `fps`; `codec`, `codestream_bytes`, `rate_mode`, `app_format`, `sender_type`, `troffset_ns` | Keep → `raster`; Keep | |
| `mtl_cvideo_config.plugin_device`, `codec_threads`, `quality` | Option 600–602 | |
| `mtl_cvideo_config.pack_type` | Remove | one legal value; key 603 reserved |
| `mtl_audio_config.format`, `sample_rate`, `channels`, `samples_per_packet` | Keep | |
| `mtl_audio_config.rx_unit_samples` + `buffer_capacity_bytes` | Keep as one `unit_samples` | P6 |
| `mtl_audio_config.audio_absorb_samples` + `MTL_AUDIO_NO_ABSORB` | Option 700 | presence replaces the flag |
| `mtl_audio_config.audio_build_pacing`, `audio_fifo_ms`, `audio_launch_offset_ns` | Option 702, 703, 701 | |
| `mtl_anc_config.interlaced`, `fps`; `total_lines`; `max_udw_bytes` | Keep → `video` raster, inherited (P5, F6); Derive from the raster, Option 803 overrides; Keep | |
| `mtl_anc_config.detect`, `anc_split_by_packet`, `anc_timing_model`, `anc_window_anchor`, `max_packets`, `anc_target_delay_ns` | Option 806, 804, 800, 802, 805, 801 | |
| `mtl_fastmeta_config.interlaced`, `rate` | Keep → `video` raster, inherited | |
| `mtl_fastmeta_config.data_item_type`, `k_bit`, `fastmeta_flags`, `buffer_capacity_bytes` | Keep | RX filter role added |
| `mtl_fastmeta_config.fastmeta_target_delay_ns` | Option 900 | |
| all media configs | add `kind` | P7 |
| `mtl_instance_params.api_version`, `port_count`, `time_source`, `lcores`, `log_level`; `ports[8]`; `flags` | Keep; Keep as pointer + count (P9); Keep, 5 bits | |
| `MTL_INSTANCE_PTP_BUILTIN` | Remove | `time_source` (F4) |
| `MTL_INSTANCE_RX_SEPARATE_VIDEO_LCORE`, `TX_VIDEO_MIGRATE`, `RX_VIDEO_MIGRATE`, `TASKLET_TIME_MEASURE` | Option 2005–2008 | |
| `mtl_instance_params.sched_max`, `sched_quota_mbs`, `ptp_domain`, `max_queues`, `cmd_ack_timeout_ns` | Option 2000–2004 | |
| `mtl_port_spec.name`, `ip_family`, `prefix_len`, `sip`, `gateway`, `tx_queues`, `rx_queues`, `numa` | Keep | |
| `mtl_port_spec.port_flags` | Remove | reserved, must be 0 |
| `mtl_option_key`, `MTL_OPT_*` (13) | Move → `mtl_options.h`, renumbered | `MTL_OPT_HIST_LINEAR` removed |
| `mtl_session_set_option`, `_get_option`, `_option_list` | Replace → `mtl_set_option`, `mtl_get_option`, `mtl_option_list`, `mtl_option_find` | P2 |
| `mtl_timeline_config.anchor_mode`, `tai_ns`, `grid`, `step_policy` | Keep | |
| `mtl_timeline_config.flags` / `MTL_TIMELINE_GRID_EXPLICIT` | Remove → reserved | F5 |
| `mtl_group_config.direction`, `link_offset_ns` | Keep | |
| `mtl_start_params.mode`, `tai_ns`, `media_index`; `preroll_ns` | Keep → `struct mtl_when at` (P10); Keep | |
| `struct mtl_activation` (`kind`, `tai_ns`, `media_index`) | Replace → `const struct mtl_when*` | P10 |
| `mtl_discard_params.flags`, `first_index` | Keep (OPTION: scalar arguments) | P10 |
| `mtl_reconfigure_params.pool_count`, `media_config`, `legs`, `leg_count` | Keep; `media_config` tag-checked | F1 |
| 5 × `mtl_<essence>_session_create/query` | Keep (OPTION: 2 generic calls) | P7 |

## 5. Cuts and approvals

**Cuts (use cases lost): none.** Each removal was checked against today's MTL:

| Removed | Why nothing is lost |
|---|---|
| `pack_type` | `ST22_PACK_SLICE` is "not support now" (`include/st20_api.h:386`); FFmpeg passes CODESTREAM only |
| `leg_count` | a non-zero `flows[1]` is the second leg; its `ip` and `udp_port` are then required, so no state is ambiguous |
| `MTL_INSTANCE_PTP_BUILTIN` | `time_source = PTP_BUILTIN` expresses it; built-in PTP with another time source has no consumer |
| `MTL_TIMELINE_GRID_EXPLICIT` | `grid` non-zero expresses it, including the cross-process AT_TAI recipe (06 §10.7) |
| `MTL_AUDIO_NO_ABSORB`, `MTL_NUMA()` for sessions, `*_DEFAULT` sentinels | option presence expresses every value they encoded |
| flow `fmd_dit`/`fmd_k`; `MTL_OPT_HIST_LINEAR` | the same filter is in the fastmeta config, and today's update path cannot change it either; an unshipped alias |

**Design assumptions changed (approval needed):**

| BREAKS | By | Ask |
|---|---|---|
| A8 / D-46: key-value is "backend-specific only" | P1 | accept "typed = recipes + required; key-value = everything else" |
| D-22: one zero-default mechanism for every knob | P1 | accept presence semantics for options alongside zero-default typed fields |
| Q-MODE-3 shape: one create per essence | P7 (only if the generic create is chosen) | pick typed creates or two generic calls |
| `time_source = AUTO` may pick built-in PTP "if enabled" | P9 | AUTO = PHC, else SYSTEM_TAI; built-in PTP only when named |
| 09 §9 lint checks header comments | P1 | the lint checks typed fields; option defaults are generated from one table |

## 6. Open questions

1. **Live options.** Should some S-class keys (`horizon_ns`, `media_index_offset`) be settable in
   RUNNING as boundary commands (04 §3.2)? This study assumes CREATED/STOPPED only.
2. **Value width.** All 57 + 9 keys fit `int64_t`; a knob needing a rational or a string would go
   to a `next` block. Acceptable, or reserve a pointer form in `struct mtl_option` now?
3. **Reset.** `MTL_OPT_DEFAULT = INT64_MIN` is the sentinel style C5 §2.20 disliked (no key's
   range reaches it); the alternative is a `mtl_reset_option()` call.
4. **Generator.** One table → `mtl_options.h`, descriptors, docs and the options half of 09 §9 in
   Phase 0 CI? Does `mtl_unified.h` include `mtl_options.h` (one include) or not (leaner core)?
5. **`troffset_ns`, `sender_type`** stay typed for the GATEWAY recipe and ST 2110-21 sender
   selection; options instead, if GATEWAY is rare?
6. **AUTO + CAPTURE** is not defined by 06 (CAPTURE's delay is for media-timed units): reject at
   create, or define?
7. **`rx.incomplete` = DELIVER** differs from today's st20p (drop unless
   `ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`). Keep?
8. **`ignored_fields`** for instance options: a list call, or a differing `mtl_get_option` value?
9. **`mtl_session_info` mirrors** of options: drop them once `mtl_get_option` returns effective
   values (owner of section 12)?
10. **Flows by pointer (P3-D):** 160 B and `MTL_MAX_LEGS` out of the ABI, against a two-statement
    `mtl_flow_parse`?
