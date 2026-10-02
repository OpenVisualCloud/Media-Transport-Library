# 09 — Media types, operating modes, backends and capabilities

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-OBJ-1, R-CAP-1, R-MEM-5, NG2, NG6 |
| Research | [R07 modes matrix](research/07-modes-matrix.md) (primary), [R02 survey](research/02-current-api-survey.md), [R10 Rivermax](research/10-rivermax.md), [R12 standards](research/12-st2110-timing-standards.md); reviews [C3](reviews/C3-usability-personas.md), [C4 §4](reviews/C4-consistency-audit.md), [C5 §2.4, §2.14, §3.7, §7.4, §7.7–7.9, §8.1–8.2](reviews/C5-adversarial-user-review.md) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

Revision 3 applies the cross-cutting resolutions of [C5-response](reviews/C5-response.md)
Part A and makes these changes:

- the essence names `video`, `cvideo` (ST 2110-22), `audio`, `anc` and `fastmeta`
  (ST 2110-41) are used everywhere (A6);
- the zero-default renames of A7 are applied;
- the knob-placement rule of A8 is applied: essence-specific fields move from
  `timing` / `options` into the media configs (§1.4);
- flow updates are atomic across legs (§7.1);
- legs have an administrative state and are never pruned (§7.2);
- a session can be reconfigured while STOPPED (§7.3);
- plugin and sample parity has a disposition per item (§8);
- one **defaults table** covers every input field (§9).

Items changed in revision 3 are marked **(r3)** and cite `[C5 §x.y]`. The header
(`sketch/include/mtl/experimental/mtl_unified.h`, A1) is normative: if this prose and the
header disagree, the header wins.

## 1. Session creation

One create function (and one dry-run query) per essence, each taking the common session
config plus a typed media config. Runtime verbs are the same for all.

```c
struct mtl_session_config {
  uint32_t struct_size;
  uint32_t direction;                 /* MTL_DIR_TX | MTL_DIR_RX; required, 0 is rejected (§9) */
  const void* next;                   /* extension chain: optional feature blocks (§1.5); must be NULL in 0.1 */
  char name[MTL_NAME_MAX];            /* "" = generated; copied at create, unique per instance (-MTL_EEXIST, 08) */
  uint32_t unit;                      /* MTL_UNIT_FRAME (0) | MTL_UNIT_ROWS (progressive, 06 §9) | MTL_UNIT_PACKET_CHUNK (reserved) */
  uint32_t leg_count;                 /* 0 = derive from populated flows[]; 1, or 2 for ST 2022-7; a populated flows[1] with leg_count 1 is -MTL_EINVAL */
  struct mtl_flow flows[MTL_MAX_LEGS];            /* fixed-size sub-struct (§1.1) */
  struct mtl_pool_config pool;                    /* 05 §5 */
  struct mtl_timing_config timing;                /* §1.2 — cross-essence timing only (r3) */
  struct mtl_completion_config completion;        /* 07 §2: mode only; binding is by mtl_session_bind_cq/eq */
  struct mtl_capability_request caps;             /* §6.2 */
  struct mtl_session_options options;             /* §1.3 — cross-essence options only (r3) */
  uint64_t flags;                     /* orthogonal booleans: MTL_SESSION_MT_SUBMIT, MTL_SESSION_SINGLE_READER, …; unknown bits → -MTL_EINVAL */
  uint64_t user_cookie;
  uint64_t reserved[8];               /* must be zero */
};                                    /* 792 B at 0.1 */
void mtl_session_config_init(struct mtl_session_config* c);   /* A7: sets struct_size, zeroes the rest */

int mtl_video_session_create   (mtl_instance_h, const struct mtl_session_config*, const struct mtl_video_config*,    mtl_session_h*);
int mtl_cvideo_session_create  (mtl_instance_h, const struct mtl_session_config*, const struct mtl_cvideo_config*,   mtl_session_h*);
int mtl_audio_session_create   (mtl_instance_h, const struct mtl_session_config*, const struct mtl_audio_config*,    mtl_session_h*);
int mtl_anc_session_create     (mtl_instance_h, const struct mtl_session_config*, const struct mtl_anc_config*,      mtl_session_h*);
int mtl_fastmeta_session_create(mtl_instance_h, const struct mtl_session_config*, const struct mtl_fastmeta_config*, mtl_session_h*);
/* and mtl_<essence>_session_query(mt, &sc, &mc, flags, &info, &req): same validation, allocates nothing;
   MTL_QUERY_CHECK_CAPACITY also checks free capacity (03 §3.3) */

struct mtl_completion_config {   /* fixed size */
  uint32_t mode;                 /* MTL_COMPLETE_* (07 §2.2); 0 = default for the pool source (§9) */
  uint32_t optional_kinds;       /* MTL_CQE_ENABLE_SOURCE_RELEASED | MTL_CQE_ENABLE_RX_MISSING (07 §1.3) */
  uint32_t reserved[6];
};
```

Embedded sub-structs (`mtl_flow`, `mtl_pool_config`, `mtl_timing_config`,
`mtl_completion_config`, `mtl_capability_request`, `mtl_session_options`) are **fixed
size with reserved tails** and no `struct_size` of their own. They version with the
parent. This keeps the draft's own ABI rule (a growable struct is never embedded mid-struct)
and keeps SWIG output flat `[C3 B-1, C4 C-23]`.

cvideo (ST 2110-22) has its own create (codec, plugin device, `rate_mode`,
`codestream_bytes`, quality, codec threads) rather than a `bool compressed` inside the
video config: PR #1610 accepted `compressed = true` and silently created an ST20 session
`[R01 D5]`. This answers Q-MODE-3.

Every input struct has an exported `mtl_<struct>_init()` that sets `struct_size` and
zeroes the rest (A7), and C users also get the value initialiser `MTL_<STRUCT>_INIT(...)`.
Because of the **zero-default rule**, a zeroed struct *is* the default configuration. No
field has a non-zero default: where zero means something other than the value zero, §9
says what it means `[C5 §2.4, §3.7]`. `struct_size = 0` is rejected `[C3 P1-4]`.

### 1.1 Flows (r3)

One descriptor shape for every essence and for runtime updates. Today sessions use flat
arrays (`dip_addr[2][4]`, `port[2][64]`, `udp_port[2]`, …), pipelines use
`st_tx_port`/`st_rx_port`, and the update struct cannot change MAC, source port or SSRC
`[R02 §3.8, R07 Q12]`.

```c
struct mtl_flow {                /* fixed size */
  uint32_t port;                 /* 0 = leg i uses instance port i; MTL_FLOW_PORT(n) = port n (r3, A7) */
  uint8_t  ip_family;            /* 0 = IPv4; IPv6 reserved */
  uint8_t  payload_type;         /* TX: 0 = the essence default (§9); RX: 0 = no check */
  uint8_t  dscp;                 /* the DSCP value itself; 0 = CS0, today's hard-coded TOS 0 */
  uint8_t  ttl;                  /* 0 = 64 */
  uint8_t  ip[16];               /* TX: destination; RX: group or unicast; required */
  uint8_t  source_filter[16];    /* RX SSM source; all zero = none (must actually filter on the DPDK path, #1239) */
  uint16_t udp_port;             /* required: 0 is -MTL_EINVAL (r3) */
  uint16_t udp_src_port;         /* TX: 0 = udp_port; RX: 0 = any */
  uint32_t ssrc;                 /* TX: 0 = random per session; RX: 0 = no check */
  uint16_t vlan;                 /* reserved, must be zero */
  uint8_t  dst_mac[6];           /* only with MTL_FLOW_USER_MAC, else must be zero */
  uint32_t fmd_dit;              /* ST41 RX: data item type filter, used only with MTL_FLOW_MATCH_FMD_DIT (r3) */
  uint8_t  fmd_k;                /* ST41 RX: K-bit filter, used only with MTL_FLOW_MATCH_FMD_K (r3) */
  uint8_t  reserved0[3];
  uint64_t flow_flags;           /* MTL_FLOW_USER_MAC | MTL_FLOW_MATCH_FMD_DIT | MTL_FLOW_MATCH_FMD_K */
  uint64_t reserved[4];
};
int mtl_flow_parse(const char* spec, struct mtl_flow* f);   /* L4: "239.168.85.20:20000@0000:af:01.0,pt=112" */
```

The A7 renames and their reasons `[C5 §2.4]`:

- **`port`.** A literal `flows[1]` with `port = 0` would silently put both 2022-7 legs on
  one NIC. Zero therefore means "the leg's own port" (leg 0 → port 0, leg 1 → port 1),
  and `MTL_FLOW_PORT(n)` (= n + 1) names port n explicitly. `mtl_flow_parse` fills it
  from `@<port name>`.
- **`fmd_dit` / `fmd_k`.** Revision 2 used "0xffffffff / 0xff = off", so a literal flow
  filtered on DIT 0 and K 0. The filters are now active only with their flag bit, and the
  field values are then used as given, 0 included.
- **`udp_port`.** Today 0 derives an index-dependent port, and TX and RX derive different
  ones: video 10000 + 2·idx on both sides, but audio TX 10100 + 2·idx
  (`st_tx_audio_session.c:2127`) against RX 20000 + 2·idx (`st_rx_audio_session.c:997`),
  and ANC / fast metadata TX 10200 + 2·idx against RX 30000 + 2·idx. Two zeroed ends never
  meet. The unified API requires the port.
- **`ssrc`.** Today's TX default is deterministic, `idx + 0x123450` for video
  (`st_tx_video_session.c:966`), and ANC and fast metadata both use `idx + 0x323450`
  (`st_tx_ancillary_session.c:246`, `st_tx_fastmetadata_session.c:189`). The unified
  default is random (RFC 3550 §8), a behaviour change noted in 11.
- **`dscp` and `ttl`** answer Q-MODE-2. DSCP is a plain value whose zero is CS0 (today's
  `type_of_service = 0`, `st_tx_video_session.c:945`). TTL 0 means 64, as today (`:944`).
  VLAN and IPv6 stay reserved.

### 1.2 Timing config (r3)

Only the cross-essence fields remain; the essence-specific ones are in the media configs
(§1.4) `[C5 §2.14]`. Every field is described in [06](06-timing-pacing-and-sync.md), and
every zero default is in §9.

```c
struct mtl_timing_config {       /* fixed size */
  mtl_timeline_h timeline;       /* null = the epoch timeline (mtl_timeline_epoch(mt)) */
  uint32_t source_kind;          /* PLAYBACK (0) | CAPTURE | GATEWAY */
  uint32_t media_mode;           /* AUTO (0) | INDEX | TAI */
  uint32_t late_policy;          /* 0 = by media mode (§9) */
  uint32_t underrun_policy;      /* 0 = by essence (§9) */
  uint32_t snap_mode;            /* NEAREST (0) | LOCKED_PHASE (opt-in, 06 §4.3) */
  uint32_t tsmode;               /* 0 = derived; SAMP | NEW | PRES */
  uint32_t rtp_mode;             /* DERIVED (0) | PASSTHROUGH */
  uint32_t rx_incomplete;        /* DELIVER (0) | DISCARD */
  uint32_t mediaclk_mode;        /* RX: DIRECT (0) | SENDER */
  uint32_t off_grid_policy;      /* LOCKED_PHASE only: RELOCK (0, after 3 OFF_GRID) | REANCHOR | DROP (06 §4.3) */
  uint32_t rx_rtp_offset;        /* RX: from SDP mediaclk:direct=<offset> */
  uint32_t reserved0;
  int64_t  min_tx_delay_ns;      /* 0 = by source kind (§9) */
  int64_t  media_time_offset_ns; /* r3: TAI mode, shift every unit's media time by the declared latency (06) */
  int64_t  late_tolerance_ns;
  int64_t  snap_tolerance_ns;
  int64_t  horizon_ns;           /* 0 = 1 s */
  int64_t  media_index_offset;   /* lip-sync trim in units */
  int64_t  link_offset_ns;       /* RX */
  int64_t  link_offset_budget_ns;/* r3: declared downstream budget; 0 = none (06) */
  int64_t  rx_flush_offset_ns;   /* RX: 0 = derived */
  int64_t  rx_skew_budget_ns;    /* RX 2022-7: 0 = derived */
  int64_t  rx_signal_timeout_ns; /* RX: 0 = derived */
  uint64_t reserved[12];         /* r3: >= 96 B spare (A8) */
};
```

Removed from `timing`: `troffset_ns`, `sender_type` and `progressive_late` (video and
cvideo), `audio_launch_offset_ns` (audio), and `anc_timing_model` and
`anc_target_delay_ns` (ANC). Their new places are in §1.4.

### 1.3 Per-session options (r3)

Features that exist today per session and must not be lost `[C4 §4.2]`. Only
cross-essence options remain here.

```c
struct mtl_session_options {     /* fixed size */
  uint32_t tx_queue;             /* AUTO (0) | DEDICATED | SHARED; AUTO per essence in §9 */
  uint32_t numa;                 /* 0 = the first port's socket; MTL_NUMA(n) = node n (r3, A7; was int32 -1) */
  uint32_t src_port_mode;        /* FIXED (0) | RANDOM | MULTI (today MTL_FLAG_RANDOM/MULTI_SRC_PORT, instance-wide) */
  uint32_t rx_burst_size;        /* 0 = 128 */
  uint32_t rx_threads;           /* 0 = auto (2 above 40 Gbps today); reported as extra scheduler consumers */
  uint32_t reserved[11];
};
```

`anc_split_by_packet`, `audio_build_pacing` and `audio_fifo_ms` moved to the media configs
(§1.4).

### 1.4 Media configs: the essence-specific knobs (r3)

The header defines the full media config structs (A1) and wins if it disagrees with this
table. The table lists their essence-specific knobs, including the fields moved out of
`timing` and `options`.
The first seven rows are C5 §2.14's seven fields; the next two follow the same A8 rule
`[C5 §2.14, §7.7]`.

| Field | Media config | Meaning; zero | Was |
|---|---|---|---|
| `troffset_ns` | video, cvideo | first-packet offset; 0 = TRODEFAULT (ST 2110-21) | `timing.troffset_ns` |
| `audio_launch_offset_ns` | audio | `D_a` (06 §5.2); 0 = by source kind (§9) | `timing.audio_launch_offset_ns` |
| `audio_build_pacing` | audio | pace in the builder too (boolean) | `options.audio_build_pacing` |
| `audio_fifo_ms` | audio | builder → pacer FIFO; 0 = 10 ms | `options.audio_fifo_ms` |
| `anc_timing_model` | anc | CTM (0) \| LLTM | `timing.anc_timing_model` |
| `anc_target_delay_ns` | anc | target inside the ANC window; 0 = the model's default (06 §5.2) | `timing.anc_target_delay_ns` |
| `anc_split_by_packet` | anc | one ANC packet per RTP packet (boolean, `ST40_TX_FLAG_SPLIT_ANC_BY_PKT`) | `options.anc_split_by_packet` |
| `sender_type` | video, cvideo | N (0) \| NL \| W — ST 2110-21 sender types exist only for video and cvideo | `timing.sender_type` |
| `progressive_late` | video | TRUNCATE (0) \| PAD \| STALL (06 §9) | `timing.progressive_late` |
| `rate_mode` | cvideo | `MTL_CVIDEO_RATE_CBR` (0, default: ST 2110-22 requires constant bytes and packets per frame) \| `MTL_CVIDEO_RATE_VBR_MAX` (opt-in, flagged non-compliant; today's behaviour) (06) | new |
| `codestream_bytes` | cvideo | CBR size, or the VBR_MAX ceiling, per frame or field; required | `mtl_st22_config` |
| `pack_type` | cvideo | CODESTREAM (0); slice packing is not supported (`R07 §2.2`) | today's `st22p pack_type` |
| `rx_unit_samples` | audio | RX unit size; 0 = 10 ms rounded down to whole packets (at least one) | new `[C5 §7.7]` |
| `buffer_capacity_bytes` | audio, fastmeta | library-pool buffer size; 0 = audio 10 ms, fastmeta 64 KiB | new `[C5 §7.7]` |
| `audio_absorb_samples` | audio | CAPTURE contiguity tolerance (06 §8); 0 = by source kind (§9); the audio config flag `MTL_AUDIO_NO_ABSORB` forces none | new (06) |
| `max_udw_bytes` | anc | UDW capacity per unit; 0 = 64 KiB | new `[C5 §7.7]` |
| `max_packets` | anc | ANC packets per unit (TX table, RX meta area); 0 = 255 | new (05 §4.4) |
| `anc_window_anchor` | anc | which frame's ANC window a unit uses: `MTL_ANC_WINDOW_AUTO` (0: the video member's transmit frame in a group, else the unit's own frame + own L) \| `MTL_ANC_WINDOW_MEDIA` (strict, L = 0) (06 §5.5) | new (06) |
| `total_lines` | anc (TX) | SDI raster of the associated video, for TEPO in the ANC window (06 §5.2); unused on RX | new (06) |
| `detect` | anc (RX) | `MTL_DETECT_DEFAULT` (0 = AUTO: interlace detection, today's default) \| `OFF` \| `ON` | `ST40P_RX_FLAG_DISABLE_AUTO_DETECT` `[C5 §7.9]` |
| `rate` | fastmeta | the associated video frame rate; 0/0 = the group video member's rate at start (outside such a group `FIELD_REQUIRED`); with `MTL_FASTMETA_FREE_RUNNING` the stream's own rate, required, off the video grid (06 §4.8, engine change E13 in 06 §14) | `mtl_fmd_config.rate` |
| `fastmeta_target_delay_ns` | fastmeta | `D_fmd` (06 §5.2); 0 = the 06 default | new (06) |
| `interlaced` | video, cvideo, anc, fastmeta | 0 progressive, 1 interlaced, 2 PsF | fastmeta: new (06) |

The granted values are returned in `mtl_session_info`: `unit_bytes`, `unit_samples`,
`buffer_capacity_bytes` and `max_udw_bytes`. An RX source can therefore answer a latency
query and set buffer durations before the first dequeue, and `mtl_tx_acquire` on an audio
session has a known capacity. `mtl_tx_write` splits into units of the granted capacity
`[C5 §7.7]`.

### 1.5 Where a knob lives (r3)

The A8 rule, applied throughout this revision `[C5 §2.14]`:

| Mechanism | Holds | Examples |
|---|---|---|
| `mtl_session_config` field | cross-essence, validated at create | flows, pool, timing, completion, options |
| media config (`mtl_video_config`, …) | essence-specific | §1.4 |
| `next` extension block | optional feature groups | progressive, RTCP, test |
| key-value option | backend-specific or pacing-tuning only; enumerable through `mtl_session_option_list`; `mtl_session_set_option` in CREATED/STOPPED | `MTL_OPT_VIDEO_DISABLE_BULK`, `MTL_OPT_VIDEO_STATIC_PAD_P`, `MTL_OPT_VIDEO_START_VRX`, `MTL_OPT_VIDEO_PAD_INTERVAL`, `MTL_OPT_AUDIO_RL_WARMUP`, `MTL_OPT_HIST_LINEAR_*` (08) |
| flag bit | orthogonal booleans | `MTL_SESSION_MT_SUBMIT`, `MTL_FLOW_USER_MAC`, `MTL_MEM_NUMA_REQUIRED` |

## 2. Units of work per essence

| Essence | TX unit | RX unit | Progressive | Packet (RTP) level |
|---|---|---|---|---|
| video (ST20) | frame or field (+ progressive rows); a field buffer has `rows = height/2` (05 §4.3) | frame or field (+ progressive rows) | v1 shape; implementation **Phase 6** via a session-layer adapter (st20p has no slice mode) | legacy API (NG2) |
| cvideo (ST22) | CBR codestream frame/field, or raw frame through the encoder | same through the decoder | no (ST22 slice packing is not supported today `[R07 §2.2]`) | legacy |
| audio (ST30) | a run of samples (`sample_count`), cut into packets from the absolute sample index; or `mtl_tx_write` | **(r3)** `rx_unit_samples` per unit (0 = 10 ms in whole packets, like today's plugins, `gst_mtl_st30p_rx.c:228-230`, `mtl_st30p_rx.c:126-127`); granted value in `mtl_session_info` `[C5 §7.7]` | — | legacy |
| anc (ST40) | ANC unit for one video frame/field: UDW bytes + packet table; the config carries the associated video format (rate + raster) | same | — | legacy |
| fastmeta (ST41) | one data item group | same (**new**: RX is RTP-only today) | — | legacy |

### 2.1 Packet level (later)

Rivermax users expect "app builds RTP headers, library/NIC paces" `[R10 §2.5]`, and
ST 2022-6 exists in MTL only through RTP-level sessions `[R07 §2.6]`. A packet-level
session would fit the same verbs with a different buffer shape: a buffer is a *chunk* of
N packet slots (header + payload strides), `submit` carries the chunk's first-packet
launch time, results are per chunk. It is out of v1 (NG2); `unit = MTL_UNIT_PACKET_CHUNK`
is reserved so it can be added without an ABI break (Q-MODE-1).

## 3. Feature coverage target

Today's features are asymmetric by accident `[R07 §3]`. Target for the unified API
(Y = v1, L = later, — = not applicable, X = dropped):

| Feature | video | cvideo | audio | anc | fastmeta |
|---|---|---|---|---|---|
| TX/RX unit, 2022-7 | Y | Y | Y | Y | Y (adds frame RX) |
| Library pool / imported pool | Y / Y | Y / Y | Y / Y | Y / Y | Y / Y |
| Direct TX path, including strides > row (05 §4.2) | Y | — (copy) | — | — | — |
| Media modes AUTO / INDEX / TAI, source kinds | Y | Y | Y | Y | Y |
| Launch override NOT_BEFORE / EXACT | Y | Y | L | Y | L |
| Late policy DROP / bounded SEND_LATE / RESLOT | Y | Y | Y | Y | Y |
| Underrun keep-alive (EMPTY_ANC / KEEPALIVE / SILENCE) | — | — | Y (SILENCE option) | Y (default) | Y (default) |
| Underrun REPEAT_LAST | L | L | L | L | — |
| Per-unit results with timing | Y | Y | Y | Y | Y |
| Events (state, recovery, legs, signal, flow state) | Y | Y | Y | Y | Y |
| Stats (one schema) | Y | Y (none today) | Y | Y | Y |
| Timeline + group | Y | Y | Y | Y | Y (in the grid like ANC, 06) |
| Pixel-format conversion | Y (pipeline converters, plugins) | — (codec) | — | — | — |
| Auto-detect (RX) | L | — | — | Y (interlace; `detect = OFF` disables it) | — |
| Timing parser | opt-in | — | opt-in | — | — |
| TX ST 2110-21 self-check | Y | Y (network model only) | — | — | — |
| CBR (default) / VBR_MAX rate mode | — | Y | — | — | — |
| DMA offload (RX) | Y (reported) | — | — | — | — |
| Flow update, leg enable/disable, reconfigure (§7) | Y | Y | Y | Y | Y |
| RTCP retransmission | L (video and cvideo only; the no-op flags elsewhere are removed, Q-MODE-8) | L | X | X | X |
| Header split | X for v1 (NG6) | — | — | — | — |

## 4. Pipeline features in the unified API

| Today (pipeline) | Unified API |
|---|---|
| `transport_fmt` vs `input_fmt/output_fmt` with internal or plugin converter | `mtl_video_config.transport_format` + `app_format` (default: equal, no conversion); the granted converter and `convert_context` are reported (05 §6) |
| derive mode (formats equal, no conversion) | `pool.data_path` DIRECT when possible |
| `transport_linesize` | the buffer layout's stride (05 §4.2); `mtl_video_config` has no linesize field **(r3)** |
| `ST20P_RX_FLAG_PKT_CONVERT` (per-packet conversion on the tasklet via `uframe_pg_callback`) | not in v1: application-visible work on the tasklet (R-THR-4) |
| ST22 encoder/decoder plugins (`st22_encoder_dev`), `codec_thread_cnt` | `mtl_cvideo_config`; the plugin ABI stays as it is in v1 (Q-MODE-4) |
| framebuffer count (2–8) | `pool.count`; `max_count` is reported, 8 until E11 (05 §5.2) |
| BLOCK_GET, `set_block_timeout`, `wake_block` | per-call timeouts, sticky `mtl_session_interrupt` / `mtl_session_uninterrupt` |
| `put_frame_abort` | `mtl_tx_release` |
| `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` two-phase release | subsumed by leases (the terminal outcome is the release) |

## 5. Backends and their constraints

| Backend | Pacing classes | Direct TX | RX steering | Notes for the capability model |
|---|---|---|---|---|
| DPDK PMD (E810/E830 PF or VF) | HW_RATE (RL, ice PF/iavf VF), HW_LAUNCH (TSN, E830 PF + built-in PTP), SW, BEST_EFFORT | yes (multi-seg NICs) | flow director, or shared RSS if no FDIR | the reference backend; queue-hang recovery exists only here `[R07 §5.1]` |
| Native AF_XDP | HW_RATE via sysfs `tx_maxrate` on ice, else SW | no (copies into UMEM) | XDP program + MtlManager | requires MtlManager; `send()` kick on the tasklet (04 §2.2) |
| Kernel socket | SW only | no (single segment) | none (socket per flow) | still needs hugepages; `sendto` on the tasklet; destination fixed at queue creation (a flow update needs a queue swap, prepared before commit, §7.1) |
| DPDK AF_XDP, DPDK AF_PACKET | SW | partial / no | — | deprecate candidates `[R07 §9]` |
| Windows (DPDK only) | as PMD | as PMD | as PMD | no MtlManager, no AF_XDP, no kernel socket; one ABI through the portable wait object and the `MTL_E*` codes (A5), CI compile-only job (14) |
| Null (`null:<n>`, experimental, Phase 1) | none: a unit completes at its scheduled launch instant from the instance clock | — (no NIC) | loopback from a TX session on the same port | no NIC, root, hugepages or VFIO; `MTL_BACKEND_NULL`; the substrate of the U-tier tests, doc examples and bindings (02 §2, 13 §9) |

Constraints the capability model must express, all silent today `[R07 §5.2]`:

- a shared TX queue forces SW pacing for the **whole port**;
- ST22 downgrades RL to SW pacing;
- `mtl_dma_map` requires IOVA-VA mode;
- HW RX timestamps depend on the PMD offering the RX timestamp offload (HEAD enables it on any such port, `mt_dev.c:2422-2440`, with an iavf VF workaround from `99b96c16`, `mt_dev.c:1020-1028`; `include/mtl_api.h` still documents "PF on E810" — to confirm);
- **(r3)** legacy `MTL_FLAG_ALLOW_DOWN_PORTS` silently prunes a 2022-7 session to one leg at
  create. Every essence has a prune function that rewrites `ops->num_port`: video TX
  `tv_ops_prune_down_ports` at `st_tx_video_session.c:4000-4045` (C5 cited `:3935-3975`,
  which is the scheduler sleep code at `545a266a`), audio TX `st_tx_audio_session.c:2599`,
  and video RX `st_rx_video_session.c:4199-4237`. The unified API never prunes (§7.2);
  the legacy behaviour stays legacy-only;
- DATA_PATH_ONLY (app-managed flows) appears to dereference a NULL flow on non-socket
  backends (`datapath/mt_queue.c:56`, **[inferred]**, SP-02) — needs a test before the
  unified API offers anything equivalent (Q-MODE-5).

## 6. Capabilities and negotiation

### 6.1 Query

```c
int mtl_port_get_caps(mtl_instance_h mt, uint32_t port, struct mtl_port_caps* caps);
/* backend type; pacing classes available + accuracy profile each; TX multi-seg (direct TX);
   HW RX/TX timestamp; launch-time offload; DMA engines; header split; max RL queues;
   IOVA mode; NUMA node; max sessions by type; link speed; backend_syscalls_on_tasklet;
   r3: max_regions (05 §3.4), system page size and enabled hugepage sizes (05 §3.3),
   link-state source (LSC interrupt or poll, §7.2) */
```

The dry-run `mtl_<essence>_session_query` (03 §3.3) answers "what would I get for this
configuration" before anything is allocated — the true `fi_getinfo` analogue. Free
capacity (queues, RL queues, lcores, quota) is `mtl_port_get_capacity` (08), and hugepage
memory per NUMA node is `mtl_instance_get_mem_status(mt, numa, &st)` (05 §6.3).

### 6.2 Request and grant

Following libfabric's rule — a non-zero request is required, zero lets the library
choose, and the choice is reported `[R09 §2.1]` — with an explicit tri-state. Enumerated
fields are `uint32_t` (11 §2.1 rule 6):

```c
struct mtl_capability_request {  /* fixed size */
  uint32_t hw_pacing;            /* enum mtl_req: ANY (0) | PREFER | REQUIRE | OFF */
  uint32_t pacing_class;         /* MTL_PACING_ANY (0) | HW_RATE | HW_LAUNCH | SW | BEST_EFFORT */
  uint32_t dma_offload;          /* enum mtl_req */
  uint32_t hw_timestamps;        /* enum mtl_req */
  uint32_t reserved[4];
};
```

The direct data path is requested only through `pool.data_path` (05 §6.1); there is no
second knob for it `[C3 consistency table]`.

- `REQUIRE` that cannot be met → create/query/start fails with `-MTL_ENOTSUP` and a reason,
  readable through `mtl_last_error()`.
- `PREFER` that cannot be met → success, and the granted value differs; visible in
  `mtl_session_get_info` and counted.
- A runtime downgrade (RL failure flipping a port to SW pacing, a leg going down) is an EQ
  event on every affected session (`PACING_CHANGED`, `LEG_STATE`).

This replaces every silent fallback listed in §5 and `[R05 F22, R04 §6 #4]`. Today's seven
pacing ways map to classes: `RL` → HW_RATE, `TSN` → HW_LAUNCH, `TSC`/`TSC_NARROW`/`PTP` →
SW (distinct accuracy profiles), `BE` → BEST_EFFORT (frame start only, no ST 2110-21
claim), `AUTO` → ANY.

## 7. Session management features

| Today | Unified API |
|---|---|
| `st*_update_destination` / `update_source` (all families) | `mtl_session_update_flows`: every leg at once, one activation; a boundary command in ARMED and RUNNING (04 §3.2, §7.1) **(r3)** |
| a leg pruned at create (`ALLOW_DOWN_PORTS`); no leg enable; link checked at start only | legs never pruned; `mtl_session_set_leg_enabled`, a boundary command in ARMED and RUNNING (04 §3.2); admin × oper state; link monitor in Phase 2 (§7.2) **(r3)** |
| format change = destroy + create | `mtl_session_reconfigure` in STOPPED keeps identity (§7.3) **(r3)** |
| auto-detect (ST20 w/h/fps/packing/interlace; ST40 interlace) | RX option; `RX_FORMAT` event with a changed-property bitmask; the unit result says `format_changed`. Acting on it is stop → reconfigure with the detected config → start, off the tasklet (today `rv_init_sw` allocates on it `[R03 H5]`); flows, SSRC filters and stats survive (§7.3) |
| `get_queue_meta` + DATA_PATH_ONLY | later / verify (Q-MODE-5) |
| `pcapng_dump` | CP call that arms a capture; the file is written by a worker, not the tasklet (`[R03 H8]`) |
| SSRC / PT / SSM / DIT/K filters | in `mtl_flow` |
| `get_sch_idx`, `get_pacing_params`, `frame_size` | `mtl_session_get_info()`: `sched_index`, pacing values, `unit_bytes` |

### 7.1 Atomic flow updates (r3)

> **Addendum N (2026-10-01).** Superseded where it differs by [17 §2.2](17-nmos-and-ipmx.md)
> and the `mtl_session_update` comment in `mtl.h` (D-93): the switch happens at the slot
> boundary by the clock, whether a unit is there or not; neighbour resolution starts at
> commit and is not awaited; a port change is made before break while running; R options in
> the update's config apply at its boundary; cancel is `parts` 0; the status keeps
> `update_state`, `update_seq` and `update_applied_tai_ns` (no `first_media_index` or
> `first_rtp`).

`[C5 §8.1]`

An NMOS IS-05 activation changes every leg (group, source filter, destination port, payload
type) at one TAI instant. Revision 2's one-leg `mtl_session_update_flow` needed two
commands with independent activations, and the second could fail after the first had
committed.

```c
struct mtl_activation {           /* 40 B */
  uint32_t struct_size;          /* mtl_activation_init() */
  uint32_t kind;                 /* MTL_ACTIVATE_NOW (0) | MTL_ACTIVATE_AT_TAI | MTL_ACTIVATE_AT_MEDIA_INDEX (TX only) */
  int64_t  tai_ns;               /* AT_TAI */
  int64_t  media_index;          /* AT_MEDIA_INDEX */
  uint64_t reserved[2];
};
int mtl_session_update_flows(mtl_session_h s, const struct mtl_flow* legs, uint32_t n,
                             const struct mtl_activation* act /* NULL = NOW */);   /* CP */
```

- **Scope.** `n` must equal the session's leg count. `legs[i]` is the complete new flow of
  leg *i*; a leg identical to its current flow is unchanged. Every field may change except
  `port`, which changes only through `mtl_session_reconfigure` in STOPPED (§7.3):
  `-MTL_EINVAL`, reason `PORT_CHANGE_NEEDS_RECONFIGURE`. A width, height or rate change in
  the new SDP is also a reconfigure.
- **All-or-nothing.** The call validates the new flows with the create-time rules, then
  prepares everything the commit needs, on the CP thread and a worker, never under a
  tasklet lock:
  - the new flow rules (added next to the old ones) and, on the kernel-socket backend, the
    new queue;
  - IGMP joins for new groups and sources;
  - the ARP requests for new unicast destinations;
  - the per-leg header templates.

  If any step fails, everything prepared is released and the call returns the error with
  nothing changed (G-75). Neighbour resolution is started, not awaited. A destination still
  unresolved at activation leaves that leg in flow state `WAITING_NEIGHBOUR`, with units
  on it `DROPPED/NO_NEIGHBOUR`, exactly as at create (07 `FLOW_STATE`); `MTL_FLOW_USER_MAC`
  bypasses ARP.
- **Commit.** The commit is a boundary command (04 §3.2) carrying the activation. Its
  ack is bounded; a timeout sends the session to ERROR with reason `CMD_TIMEOUT`.
  - TX `NOW`: from the first unit picked up after the command is seen.
  - TX `AT_TAI t`: from the first unit whose media time is ≥ t.
  - TX `AT_MEDIA_INDEX k`: from unit k.
  - One TX session builds every leg's packets from one header set, so **all legs switch on
    the same unit**. No unit is sent to a mix of old and new destinations.
- **RX `AT_TAI t`.** The new rules run alongside the old ones. A packet is accepted for a
  unit whose RTP-derived media time is < t only through the old flows, and ≥ t only
  through the new flows. After the first unit ≥ t completes, or at
  `t + rx_flush_offset + rx_skew_budget` if none does, a worker removes the old rules and
  leaves the old groups. RX `NOW` switches at once; the unit being assembled from the old
  flow completes by its flush deadline and is counted. RX `AT_MEDIA_INDEX` is
  `-MTL_EINVAL`: RX media time comes from RTP, not from a submission index.
- **Reporting.** The call returns once the update is prepared and posted. The
  `FLOW_STATE` event (07) reports the activation per leg, with `first_media_index` (TX)
  or `first_rtp` (RX) on the new flows, and `mtl_session_get_status()` exposes the same
  values.
- **Pending updates.** A second call before a pending update activates replaces it: IS-05
  lets a controller re-stage a scheduled activation. The replaced update's prepared
  resources are released.
- **States.** CREATED and STOPPED apply the update directly, with `NOW` semantics. ARMED
  and RUNNING take any activation kind.
- **IS-05 mapping:**
  - `activate_immediate` → `NOW`;
  - `activate_scheduled_absolute` → `AT_TAI` with the IS-05 TAI time;
  - `activate_scheduled_relative` → `AT_TAI` of now plus the offset, read from the
    instance time base;
  - `rtp_enabled = false` on one leg → `mtl_session_set_leg_enabled` (§7.2).

### 7.2 Redundancy legs (r3)

> **Addendum N (2026-10-01).** Superseded where it differs by [17 §2.2](17-nmos-and-ipmx.md)
> (D-93): every existing leg may be disabled, which mutes the session (it stays RUNNING); a
> leg with its `legs_disabled` bit set and no address is reserved; "disabling the last
> enabled leg is `-MTL_EINVAL`" no longer holds.

`[C5 §8.2]`

- **Reserve every configured leg.** Create reserves the queue, flow rule and scheduler
  quota for every leg, whatever the link state. A leg whose link is down stays
  configured with operational state DOWN. The unified API **never prunes a leg**; the legacy
  `ALLOW_DOWN_PORTS` pruning (§5) stays legacy-only.
- **Administrative state.** `mtl_session_set_leg_enabled(mtl_session_h s, uint32_t leg,
  int enabled)` is a CP call. In CREATED and STOPPED it changes the configuration; in
  ARMED and RUNNING it is a boundary command (04 §3.2), so a leg never switches mid-unit.
  DRAINING and FLUSHING return `-MTL_EBUSY`, ERROR `-MTL_EIO` (03 §3.2).
  - TX: a disabled leg builds and sends nothing, and its reservation stays.
  - RX: a disabled leg's packets are ignored, the flow rule stays installed and the IGMP
    membership is left. Re-enabling joins again.
  - Disabling the last enabled leg is `-MTL_EINVAL` (use stop).
- **State per leg** in `mtl_session_get_status()` and the `LEG_STATE` event:
  - admin: ENABLED / DISABLED;
  - oper: UP / DOWN, from the link monitor (UNSET before its first report);
  - flow state: RESOLVED / WAITING_NEIGHBOUR (TX), JOINING / JOINED / JOIN_FAILED (RX) (07).

  A leg carries traffic only when it is admin ENABLED, oper UP and its flow is resolved or
  joined.
- **Link down while RUNNING (TX).** The transmitter skips a leg whose oper state is DOWN and
  counts the skipped units per leg (`pkts_skipped` in the leg stats, 08). A worker resets
  that leg's queue through the stalled-queue path (03 §6), so descriptors the NIC will
  never complete are reclaimed. The units complete with that leg marked not sent in the per-leg result, and
  the pool never starves on a dead link. When the link returns, the leg resumes at the
  next unit boundary without application action. This answers Q-LIFE-7: a single-leg
  link loss is an event, not ERROR.
- **Link monitor (Phase 2, prerequisite of `LEG_STATE` and `PORT_LINK`).** Today the link is
  checked only at device start: `dev_detect_link` (`dev/mt_dev.c:815-850`) is called from
  `:2008-2020`, next to the comment "no recovery method for this port as of yet TODO"
  (`:2018`). `mt_eth_link_dump` (`mt_util.c:409-422`, C5 cited `:391`) only logs, and no
  LSC handler or link poller exists anywhere in `lib/src`. The monitor:
  - uses the LSC interrupt (`intr_conf.lsc = 1` and `RTE_ETH_EVENT_INTR_LSC`) where the
    PMD supports it;
  - otherwise polls `rte_eth_link_get_nowait` from the admin thread every 100 ms;
  - on the kernel-socket and AF_XDP backends, uses netlink `RTM_NEWLINK`;
  - reports its source in `mtl_port_get_caps`, posts `PORT_LINK` to subscribed EQs and
    `LEG_STATE` to every affected session.

  The instance tolerates a port whose link is down at open, as `ALLOW_DOWN_PORTS` does
  today; in the unified API this is the only behaviour.

### 7.3 Reconfigure in STOPPED (r3)

`[C5 §7.4]`

```c
struct mtl_reconfigure_params {
  uint32_t struct_size;
  uint32_t pool_count;           /* 0 = keep the current count */
  const void* media_config;      /* NULL = keep; else the session's own essence config (mtl_video_config, …) */
  const struct mtl_flow* legs;   /* NULL = keep; else leg_count new flows — the only way to change flow.port */
  uint32_t leg_count;            /* with legs: must equal the session's leg count */
  uint32_t reserved0;
  uint64_t reserved[4];
};
int mtl_session_reconfigure(mtl_session_h s, const struct mtl_reconfigure_params* p);   /* CP; CREATED or STOPPED */
```

- **Keeps** the handle, name, SSRC (unless `legs` sets a new one), cumulative stats,
  timeline binding, group membership, CQ/EQ bindings, completion mode, pool source,
  `user_cookie`, and on RX the installed flow rules and IGMP memberships (kept across stop,
  03).
- **Re-derives** the buffer requirements, the pacing parameters (TRS, VRX, RL rate) and the
  scheduler weight from the new config, as a dry run first. On any failure nothing changes:
  `-MTL_EINVAL` for an invalid config, `-MTL_ENOSPC` if the new weight or rate no longer
  fits the scheduler or the queue.
  - Library pools are re-created at the new size and count; every lease must be FREE
    (`-MTL_EBUSY` otherwise). Phase 2 (14 §3.2).
  - Attached pools are re-validated against the new requirements. If they are
    incompatible the call returns `-MTL_EINVAL`, reason `RECONFIGURE_INCOMPATIBLE`, and the app
    detaches, replaces the buffers, attaches and starts. Phase 4, with attached pools
    (14 §3.4).
- **Timeline and group.** If the unit period changes, it must still fit the group grid
  (`-MTL_EINVAL`, reason `GRID_MISMATCH`). The next start uses `START_NOW` semantics
  (06).
- **Wrong state.** Anywhere other than CREATED or STOPPED the call returns `-MTL_EBUSY`.
- **v1 over the pipelines.** L2 re-creates the underlying pipeline and transport session
  internally while keeping the public identity. It pins the SSRC through `ops.ssrc` and
  carries the stats as L2 offsets. RTP sequence numbers continue when the slot-interface
  hook can seed the engine counter; until then they restart at 0 and
  `mtl_session_get_info()` says so.

This turns a GStreamer CAPS change, a detected RX format change and an MXL grain-count
change into `stop → reconfigure → start` instead of destroy + create. Today's sinks refuse
a second CAPS event (`gst_mtl_st20p_tx.c:452-455`, `gst_mtl_st30p_tx.c:457-461`).

## 8. Mode classification (recommendation)

| Mode / surface | Class | Rationale |
|---|---|---|
| Frame/field units for every essence, TX and RX, 2022-7, library and imported pools, timing model, results, events, stats | **v1** | what every consumer needs `[R08 §1]` |
| Progressive rows (slice) | **v1 shape**, implementation Phase 6 | hard to retrofit into a frame-only contract `[R07 Q2]` |
| Packet-chunk sessions (RTP level) | **reserved**, legacy API meanwhile | different ownership model; ST 2022-6 route |
| Group start, named timelines | **v1** (SHOULD) | the A/V answer |
| Flow updates, leg enable/disable, reconfigure (§7) **(r3)** | **v1**; link monitor Phase 2 | NMOS IS-05 and 24/7 operation `[C5 §8.1, §8.2, §7.4]` |
| REPEAT_LAST underrun | later | needs buffer retention rules |
| Timing parser per unit, auto-detect video | later | additive |
| RTCP retransmission | later, video and cvideo only | non-standard; no-op on other essences today (Q-MODE-8) |
| DATA_PATH_ONLY | verify, then decide | possibly broken |
| Header split, GPU "direct" | later via the memory-domain model; GPU pinned host memory is v1 host import (05 §10) | compiled out / CPU copy today |
| `uframe_pg_callback`, `PKT_CONVERT` | not in unified API | app work on the tasklet |
| Split-forward (sub-rectangle TX from an RX buffer) **(r3)** | **v1, DIRECT**: TX buffers with stride > row over the RX pool's region, one RX hold per TX submission (05 §4.2, §5.4) | used by the fwd samples today; revision 2's "CONVERT/COPY until the builder learns strides" was wrong, the builder already uses `linesize` `[C5 §6.1]` |
| st20p `EXT_FRAME_MANUAL_RELEASE` | subsumed by leases | two-phase release today |
| 2-thread RX (> 40 Gbps) | v1 as `options.rx_threads` (auto), reported as extra scheduler consumers | implicit today |
| ST40 split-by-packet | v1, `mtl_anc_config.anc_split_by_packet` | wire behaviour |
| ST30 `BUILD_PACING`, `fifo_size`, RL warm-up | v1, `mtl_audio_config` fields / key-addressed option | **(r3)** `fifo_size` is honoured at `545a266a` (threshold at `st_tx_audio_session.c:1914-1936`, default `ST30_TX_FIFO_DEFAULT_TIME_MS = 10`, `include/st30_api.h:126`); revision 2's "ignored (#948)" was stale |
| `TSC_NARROW`, `BE`, `PTP` pacing ways | map to SW / BEST_EFFORT classes with accuracy profiles (§6.2) | 7 ways today, 4 classes here |
| DEDICATE_QUEUE, per-session NUMA, src-port modes, ST41 DIT/K, `rx_burst_size` | `mtl_session_options` / `mtl_flow` (§1.1, §1.3) | per-session flags today |
| `st20rc` (experimental redundant-combined RX), DPDK AF_XDP / AF_PACKET PMDs, SysV shm lcore allocator | deprecate candidates (Q-MODE-7) | superseded |
| `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | remove | dead since the UDP stack was deleted (`2b182cd87`) |
| Pacing tuning knobs (`DISABLE_BULK`, `STATIC_PAD_P`, `start_vrx`, `pad_interval`) | key-addressed options (§1.5) | implementation detail in the main config today |
| Test knobs (`SIMULATE_PKT_LOSS`, `st40_tx_test_config`, `port_packet_loss`) | debug API `mtl_debug_inject` in `mtl_debug.h`, built with `-Denable_debug_api=true` (A12) | not in the stable ABI `[R02 §4.1 #8]` |

### 8.1 Plugin flag parity (r3)

`[C5 §7.9]`

Every plugin feature C5 listed has a disposition:

| Today (plugin, `path:line`) | Unified API | Note |
|---|---|---|
| GStreamer `ST40P_RX_FLAG_DISABLE_AUTO_DETECT` (`gst_mtl_st40p_rx.c:515`) | `mtl_anc_config.detect = MTL_DETECT_OFF` | 09 §3 lists ANC auto-detect as Y; now it has its off switch |
| GStreamer ST40 test mutations (`gst_mtl_st40p_tx_test.h`: no-marker, seq-gap, bad-parity, paced; `include/st40_api.h:103-108`) | `mtl_debug_inject(s, MTL_FAULT_TX_MUTATE, &params)` with the same four fields, in debug builds | kept as element properties in debug builds; a new A12 fault kind |
| GStreamer `pts-pacing-offset` in ns (`gst_mtl_st20p_tx.c:210-217`, default 1080) | `timing.media_time_offset_ns` (TAI mode), ns precision | INDEX mode has no ns trim by design; a live sink uses TAI (11 §6.2) |
| GStreamer `st40p` sink stamps `GST_BUFFER_PTS + pts_for_pacing_offset` as TAI (`gst_mtl_st40p_tx.c:718-720`) | TAI media mode with CAPTURE (or PLAYBACK) on the video sink's timeline, with the same TAI media time as the video unit and no pacing offset of its own; ANC RTP = video RTP, window per `anc_window_anchor` | 06 §5.5, §10.8 |
| FFmpeg ST22 `pack_type = ST22_PACK_CODESTREAM` (`mtl_st22p_tx.c:81`) | `mtl_cvideo_config.pack_type`: CODESTREAM (0) only | slice packing is not supported today; listed, not dropped |
| FFmpeg `ptp_get_time_fn` = `CLOCK_TAI` (`mtl_common.c:39-45`) on a host without ptp4l | instance time source `SYSTEM_TAI` accepted and labelled `ESTIMATED`. If the kernel TAI offset is 0, `mtl_instance_get_status()` raises `timing_warning = TIME_ESTIMATED` | behaviour change: today such a host silently runs 37 s off TAI; now it runs and says so |
| per-element instance flags `RX_SEPARATE_VIDEO_LCORE`, `TX_VIDEO_MIGRATE`, `BIND_NUMA`, `ALLOW_DOWN_PORTS` (`include/mtl_api.h:340-350`, `:497`) | the merge table (03 §7.2, A11): an invariant mismatch is `-MTL_EINVAL`, reason `INSTANCE_PARAM_MISMATCH`; an ignored one is reported in `mtl_instance_get_info` | today the first element wins silently; `ALLOW_DOWN_PORTS` is moot (§7.2) |
| FFmpeg `AVFMT_FLAG_NONBLOCK` (11 §6.2) | scoped to the demuxer's `read_packet` (`mtl_rx_dequeue` with timeout 0 → `AVERROR(EAGAIN)`). The muxer's `write_packet` may block and uses `mtl_tx_write(…, timeout)` | the revision-2 sentence is scoped in 11 §6.2 |
| FFmpeg st30p muxer ignores `pkt->pts` (no use in `mtl_st30p_tx.c`) | rescale `pkt->pts` to 1/Fs as `media_index` (INDEX) or to TAI (TAI mode); a gap that the encoder or filter graph introduces is a forward gap or `MTL_SUBMIT_DISCONTINUITY` (06 §4.4, §8) | the demuxers' `pts = frame_counter++` (`mtl_st30p_rx.c:232`, `mtl_st20p_rx.c:301`) become `unit.timing.media_index` or media time |
| FFmpeg `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS` (`mtl_st20p_rx.c:204-220`) | deferred to the device-memory domain (05 §10, Q-MEM-8); GPU pinned host memory import is the v1 alternative | the `gpu_context` there is a stack local (side finding SC-03) |

### 8.2 OBS (r3)

`[C5 §7.8]`

- **Input** (`ecosystem/obs_mtl/linux-mtl/mtl-input.c:101-138`, a thread plus a condvar).
  - `mtl_rx_dequeue(timeout)` on the OBS thread replaces both, and with them the unlock of
    a mutex that is not held (side finding SC-08, `:125`).
  - `obs_source_frame.timestamp` comes from
    `mtl_time_convert(unit.timing.media → MONOTONIC)`; today it is the raw pipeline
    timestamp (`:129`).
- **Output** (`mtl-output.c:209-230`).
  - Today it copies into `st20p_tx_get_frame` and labels OBS's monotonic
    `obs_frame->timestamp` as `ST10_TIMESTAMP_FMT_MEDIA_CLK` (`:224-225`). The label is
    wrong, but it is latent: the session sets neither `USER_PACING` nor `USER_TIMESTAMP`
    (`:154-168`), so st20p discards the timestamp (`st20_pipeline_tx.c:231-234`) and the
    output is effectively AUTO.
  - The recipe for real timing is TAI + CAPTURE + NEAREST, with
    `media_tai_ns = mtl_time_convert(MONOTONIC → TAI, obs_frame->timestamp)` and the
    CAPTURE default `min_tx_delay` (§9). This gives derived RTP on the grid and lets OBS
    audio join the same timeline later.
  - "Just send" stays AUTO.
- **Instance parameters.** OBS passes per-source `lcores` and queue counts (`mtl-output.c:134-141`,
  `mtl-input.c:325-332`). In a merged default instance these go through the merge table
  (03 §7):
  - queue counts are mergeable at the first open (the instance opens each port with
    the hardware maximum), and a later source's counts are admission-checked;
  - `lcores` is **ignored on a second acquire** and reported in `mtl_instance_get_info`,
    so an OBS scene switch never fails over it (03 §7.2);
  - an OBS source that really needs its own lcores opens a private instance
    (`mtl_instance_open`).

### 8.3 Sample parity (r3)

`[C5 §3.4]`

| Sample | Unified API | Where |
|---|---|---|
| Moving-cursor external frames (`app/sample/ext_frame/tx_st20_pipeline_ext_frame_sample.c:147-162`) | one region plus `MTL_POOL_DYNAMIC` (Phase 4), or one attached buffer per arena frame within `max_count` | 05 §2.1, §5.5 |
| RX → TX forward with a library RX pool (`app/sample/fwd/rx_st20p_tx_st20p_fwd.c:96-109`, `:161-165`) | `mtl_session_get_pool_region` + TX buffers over it + submission `hold`; a forward pool may use `MTL_COMPLETE_NONE`, so no reap loop is added | 05 §5.3–§5.4 |
| Split-forward (`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:263-265`) | the same, with sub-rectangle layouts, DIRECT | 05 §5.4 |

## 9. Defaults (r3)

`[C5 §2.4, §3.6, §3.7]`

**The zero-default rule (A7):** no field of any input struct has a non-zero default. This
section lists every field whose zero means something other than the value zero (§9.1) and
every enumerator whose zero is a named mode (§9.2). A header lint in CI checks that each
`/* 0 = … */` annotation in the header has a row here, and the reverse (G-73). "Today" gives the
legacy value where one exists, verified at `545a266a`.

### 9.1 Zero is replaced by a derived or library value

| Field | Zero means | Today |
|---|---|---|
| `mtl_session_config.name` ("") | generated `"<essence>_<tx or rx>_<n>"`, unique per instance | `ops->name` optional; defaults like `"RX_AUDIO_M%dS%d"` (used for TX audio too, `st_tx_audio_session.c:2117`) |
| `mtl_session_config.next` (NULL) | no extension block; must be NULL in 0.1 | — |
| `mtl_session_config.leg_count` | 2 if `flows[1].ip` is set, else 1 | `num_port` required |
| `mtl_flow.port` | leg *i* uses instance port *i* | port name per leg required |
| `mtl_flow.payload_type` (TX) | video 112, cvideo 112, audio 111, anc 113, fastmeta 115 | the same (`lib/src/st2110/st_pkt.h:15-18`; applied at `st_tx_video_session.c:964-965`, `st_tx_audio_session.c:187`, `st_tx_ancillary_session.c:245`, `st_tx_fastmetadata_session.c:188`; cvideo shares the video header) |
| `mtl_flow.payload_type` (RX) | no check | the same (`mt_util.h:100-106`, `st_rx_video_session.c:1600`) |
| `mtl_flow.ttl` | 64 | 64, hard-coded (`st_tx_video_session.c:944`) |
| `mtl_flow.source_filter` (all zero) | RX: no SSM source filter | — |
| `mtl_flow.udp_src_port` | TX: `udp_port`; RX: any source port | TX: the same (`st_tx_video_session.c:3395`) |
| `mtl_flow.ssrc` | TX: random non-zero per session; RX: no check | TX: `idx + 0x123450` video, `+ 0x223450` audio, `+ 0x323450` ANC and fast metadata |
| `mtl_pool_config.count` | LIBRARY: video `max(min_count_direct, 3)`, which is 3 on RX (`min_count_direct` = 0) and for every common TX format (05 §4.1); cvideo, audio, anc, fastmeta 4 (07 §2.2). ATTACHED: the number attached at start. DYNAMIC: the LIBRARY value of the essence (05 §5.5) | `framebuff_cnt` required |
| `mtl_pool_config.rx_fill` | `ZERO_MISSING` for library pools, `NONE` for attached | no zero-fill |
| `mtl_completion_config.mode` | library pools `NONE`; ATTACHED, DYNAMIC and exported (`MTL_SESSION_EXPORT_POOL`) pools `ALL` (forced); forward pools `ALL`, `NONE` allowed (05 §5) | callbacks |
| `mtl_timing_config.timeline` (null) | the epoch timeline | epoch |
| `mtl_timing_config.late_policy` | **the media mode wins**: AUTO → `RESLOT`; INDEX and TAI → `DROP`, for every source kind | AUTO: jump to the current epoch (`stat_epoch_drop`) |
| `mtl_timing_config.underrun_policy` | video, cvideo, audio `SKIP`; anc `EMPTY_ANC`; fastmeta `KEEPALIVE` | nothing sent (`st_tx_ancillary_session.c:942-946`) |
| `mtl_timing_config.tsmode` | derived from source kind and media mode (06 §5.1): `SAMP` for PLAYBACK, CAPTURE and GATEWAY; `PRES` only when set, and with `rtp_mode = PASSTHROUGH` (06 §5.4) | — |
| `mtl_timing_config.min_tx_delay_ns` | PLAYBACK: none; **CAPTURE: one unit period + the session's `pickup_lead_ns` (`mtl_session_info`), never 0** (06 §5.1); GATEWAY: none (L = 0 with `troffset_ns`) | — |
| `mtl_timing_config.late_tolerance_ns` | `min(TROFFSET, TFRAME − RACTIVE·TFRAME)` (SEND_LATE only) | — |
| `mtl_timing_config.snap_tolerance_ns` | by snap mode (TAI media mode only, 06 §4.3): NEAREST TFRAME/8, the hysteresis band being `P/2 + snap_tolerance_ns`; LOCKED_PHASE TFRAME/4 (audio: one packet) | — |
| `mtl_timing_config.horizon_ns` | 1 s, measured from `max(now, S)` with S the resolved start instant (06 §7.4) | the transmitter accepts up to 1 s (`st_video_transmitter.c:184-199`) |
| `mtl_timing_config.media_time_offset_ns` | no shift of the media time (06 §4.6) | `rtp_timestamp_delta_us` 0 |
| `mtl_timing_config.link_offset_budget_ns` | none: no maximum slot delay is derived and no `LINK_OFFSET_BUDGET` warning is raised (06 §5.1) | — |
| `mtl_timing_config.rx_flush_offset_ns` | `rx_skew_budget_ns` (the tolerated path differential) with 2 legs, 1 ms with one leg; the due time is capped at `presentation_tai_ns` when a link offset is set (06 §11.7) | eviction by a newer RTP only |
| `mtl_timing_config.rx_skew_budget_ns` | 10 ms (ST 2022-7 class A) where the pool allows, else the largest it allows, reported | — |
| `mtl_timing_config.rx_signal_timeout_ns` | `max(4 × unit period, 20 ms)` (proposed; object by review) | — |
| `mtl_session_options.tx_queue` (AUTO) | video, cvideo: **DEDICATED** (RL if the port grants HW_RATE, else SW), never silently shared; none left is `-MTL_ENOSPC`, reason `CAPACITY_TX_QUEUES`. audio: DEDICATED if its pacing resolves to RL, else SHARED. anc, fastmeta: SHARED | the same (`mt_queue.c:165`, `st_tx_audio_session.c:2078-2124`, ANC `:1693-1694`, fastmeta `:1438-1439` of their TX files) |
| `mtl_session_options.numa` | the first port's socket | `mt_socket_id(impl, port)` |
| `mtl_session_options.rx_burst_size` | 128 | 128 (`st_rx_video_session.c:3384-3389`) |
| `mtl_session_options.rx_threads` | auto (2 above 40 Gbps) | implicit |
| `mtl_video_config.app_format` | the transport format (no conversion) | — |
| `mtl_video_config.troffset_ns`, `mtl_cvideo_config.troffset_ns` | TRODEFAULT | TRODEFAULT |
| `mtl_cvideo_config.app_format` | the app provides the codestream | — |
| `mtl_cvideo_config.codec_threads`, `quality` | the codec plugin's default | the same |
| `mtl_audio_config.samples_per_packet` | 1 ms class: 48 samples at 44.1 and 48 kHz, 96 at 96 kHz | `ptime` enum required |
| `mtl_audio_config.audio_launch_offset_ns` | PLAYBACK: `clamp(granted profile's max early error + margin, 0, ptime/2)`; CAPTURE: from `min_tx_delay_ns` (06 §5.2) | — |
| `mtl_audio_config.audio_fifo_ms` | 10 ms, at least 2 packets | the same (`include/st30_api.h:126`, `st_tx_audio_session.c:1931-1934`) |
| `mtl_audio_config.rx_unit_samples` | `floor(10 ms × Fs / S) × S`, at least S | plugins pass 10 ms to `st30_calculate_framebuff_size` (`st_fmt.c:1264-1281`) |
| `mtl_audio_config.buffer_capacity_bytes` | 10 ms of samples | `framebuff_size` required |
| `mtl_audio_config.audio_absorb_samples` | by source kind and media mode (06 §8): S (one packet) for CAPTURE and for TAI mode, 0 for PLAYBACK in INDEX mode; the `MTL_AUDIO_NO_ABSORB` flag forces 0 | — |
| `mtl_anc_config.anc_target_delay_ns` | the timing model's default target inside the ANC window (06 §5.2) | spread over the frame |
| `mtl_anc_config.detect` | `AUTO` on RX (interlace detection) | on unless `ST40P_RX_FLAG_DISABLE_AUTO_DETECT` |
| `mtl_anc_config.max_udw_bytes` | 64 KiB (255 packets × 255 words) | `max_udw_buff_size` required (GStreamer passes 5100 B TX, 128 KiB RX) |
| `mtl_anc_config.max_packets` | 255 | `ST40_MAX_META = 20` (`include/st40_api.h:308`) |
| `mtl_anc_config.total_lines` (TX) | the group video member's SDI raster at group start (1125 for 1080 lines, 750 for 720, 2250 for 2160, 625 for 576i, 525 for 480i/486i); 1125 outside a group (06 §5.2) | — |
| `mtl_fastmeta_config.rate` (0/0) | the group video member's frame rate at start, on the video grid; outside such a group `-MTL_EINVAL` (`FIELD_REQUIRED`); required with `MTL_FASTMETA_FREE_RUNNING` (06 §4.8, E13) | the session `fps` |
| `mtl_fastmeta_config.fastmeta_target_delay_ns` | `D_fmd` (06 §5.2): PLAYBACK the audio `D_a` rule (pacing early error + margin, ≤ 1 ms); CAPTURE `min_tx_delay`; a group member with a video member inherits L_v like ANC (06 §5.5) | — |
| `mtl_fastmeta_config.buffer_capacity_bytes` | 64 KiB | — |
| `mtl_mem_desc.numa` | alloc: the first port's socket; import: detected | `-1` in revision 2 |
| `mtl_mem_desc.access` | read and write | — |
| `mtl_mem_desc.device_mask` | lazy mapping at attach (05 §3.5) | port P only |
| `mtl_plane_desc.stride` | `row_bytes` (packed) | `linesize = 0` means no padding (`include/st20_api.h:1215-1217`) |
| `mtl_plane_desc.span` | `stride × (rows − 1) + row_bytes` | — |
| `mtl_buffer_desc.meta_region` (null) | the slot's library-owned meta area (05 §4.4) | — |
| `mtl_tx_submission` (NULL pointer) | every field at its default | — |
| `mtl_tx_submission.hold` (null) | no RX hold | — |
| `mtl_timeline_config.grid` (without `MTL_TIMELINE_GRID_EXPLICIT`) | the common grid derived from the members (06 §3.3) | — |
| `mtl_timeline_config.step_policy` | EPOCH timelines `KEEP`, the others `REANCHOR` (06 §2.4) | — |
| `mtl_reconfigure_params.pool_count` / `media_config` / `legs` | keep the current value | — |
| `mtl_session_destroy(s, flags = 0)` | deferred while leases or holds are out | — |
| `mtl_group_create(…, cfg = NULL)` | a TX group without link offset | — |
| `mtl_eq_config.capacity` | the default ring size per producer class (07 §3.3). CQs have no capacity field: unread results ≤ pool size by construction (07 §2.1) | — |
| key-value options (`mtl_session_set_option`, never set) | the `def` value `mtl_session_option_list` reports; `MTL_OPT_HIST_LINEAR_*` (one key per histogram) unset = the log2 bucket layout (08 §2.6) | — |
| `mtl_instance_params.api_version` | the including header's `MTL_UNIFIED_API_VERSION` | — |
| `mtl_instance_params.time_source` | `AUTO`: built-in PTP if enabled, else the PHC, else `SYSTEM_TAI` (06 §2.2) | `ptp_get_time_fn` or built-in PTP |
| `mtl_instance_params.lcores` ("") | MtlManager allocation, or auto | `lcores` string |
| `mtl_instance_params.sched_max` | auto, at most 18 | `MT_MAX_SCH_NUM = 18` (`lib/src/mt_main.h:49`) |
| `mtl_instance_params.sched_quota_mbs`, `log_level` | the library default | `data_quota_mbs_per_sch`, `log_level` |
| `mtl_instance_params.max_queues` | the hardware maximum per port (default instance) | — |
| `mtl_instance_params.cmd_ack_timeout_ns` | 100 ms; expiry sends the session to ERROR with reason `CMD_TIMEOUT` (04 §3.2) | — |
| `mtl_port_spec.prefix_len` | 24 | `netmask[]` |
| `mtl_port_spec.sip` (all zero) | DHCP on kernel backends | `net_proto[] = MTL_PROTO_DHCP` (`include/mtl_api.h:287`) |
| `mtl_port_spec.gateway` (all zero) | none | — |
| `mtl_port_spec.tx_queues`, `rx_queues` | auto; on the default instance the merge rules of 03 §7.2 apply | `tx_queues_cnt`, `rx_queues_cnt` |
| `mtl_port_spec.numa` | the device's socket | `socket_id` |
| a second `mtl_instance_acquire_default` | each field invariant, mergeable, or ignored per the merge table (03 §7.2); `lcores` is **ignored on a second acquire** and reported in `ignored_fields` | first caller wins silently |

### 9.2 Zero is a named mode

| Field | Zero is | Other values |
|---|---|---|
| `mtl_session_config.unit` | `FRAME` | `ROWS`, `PACKET_CHUNK` (reserved) |
| `mtl_flow.ip_family` | IPv4 | IPv6 reserved |
| `mtl_flow.dscp` | the value 0 (CS0), as today (`st_tx_video_session.c:945`) | any DSCP value |
| `mtl_pool_config.source` | `LIBRARY` | `ATTACHED`, `DYNAMIC` |
| `mtl_pool_config.data_path` | `ALLOW_COPY` | `PREFER_DIRECT`, `REQUIRE_DIRECT` |
| `mtl_pool_config.rx_overflow` | `DROP_NEW` | `RECLAIM_OLDEST_READY` |
| `mtl_pool_config.rx_slot_select` | `ANY_FREE` | `BY_INDEX` |
| `mtl_timing_config.source_kind` | `PLAYBACK` | `CAPTURE`, `GATEWAY` |
| `mtl_timing_config.media_mode` | `AUTO` | `INDEX`, `TAI` |
| `mtl_timing_config.snap_mode` | `NEAREST` **for every source kind**, CAPTURE included (06 §4.3); a collision is `DROPPED/DUPLICATE_SLOT`, a skipped slot takes the underrun policy | `LOCKED_PHASE` (opt-in, genlocked sources) |
| `mtl_timing_config.rtp_mode` | `DERIVED` | `PASSTHROUGH` (not with AUTO, 06) |
| `mtl_timing_config.rx_incomplete` | `DELIVER` | `DISCARD` |
| `mtl_timing_config.mediaclk_mode` | `DIRECT` | `SENDER` |
| `mtl_timing_config.off_grid_policy` | `RELOCK` (after 3 consecutive OFF_GRID units; LOCKED_PHASE only) | `REANCHOR`, `DROP` |
| `mtl_session_options.src_port_mode` | `FIXED` | `RANDOM`, `MULTI` |
| `mtl_capability_request.*` | `ANY` | `PREFER`, `REQUIRE`, `OFF` |
| `mtl_video_config.packing` | `GPM_SL` | `BPM`, `GPM` |
| `mtl_video_config.sender_type`, `mtl_cvideo_config.sender_type` | `N` (today's `ST21_PACING_NARROW = 0`) | `NL`, `W` |
| `mtl_video_config.progressive_late` | `TRUNCATE` | `PAD`, `STALL` |
| `interlaced` (every media config) | progressive | interlaced, PsF |
| `mtl_cvideo_config.rate_mode` | **`CBR`**: constant bytes and packets per frame, as ST 2110-22 requires | `VBR_MAX` (opt-in, flagged non-compliant; today's behaviour) |
| `mtl_cvideo_config.pack_type` | `CODESTREAM` | — |
| `mtl_cvideo_config.plugin_device` | `AUTO` | `CPU`, `GPU`, `FPGA` |
| `mtl_anc_config.anc_timing_model` | `CTM` | `LLTM` |
| `mtl_anc_config.anc_window_anchor` | `AUTO`: the video member's transmit frame in a group, else the unit's own frame + own L (06 §5.5) | `MEDIA` |
| `mtl_port_spec.ip_family` | IPv4 | IPv6 reserved |
| `mtl_mem_desc.domain` | `HOST` | `HOST_HUGEPAGE`; later device domains |
| `mtl_start_params.mode` | `NOW` | `AT_TAI`, `AT_MEDIA_INDEX` |
| `mtl_tx_submission.launch.mode` | `DERIVED` | `NOT_BEFORE`, `EXACT` |
| `mtl_tx_submission.user_meta_type` | `UNTAGGED`: no wire header, as today (05 §4.4) | registered or private types |
| `mtl_timeline_config.anchor_mode` | `AT_START` (created timelines) | `NEXT_GRID`, `AT_TAI`, `EPOCH` |
| `mtl_activation.kind` | `NOW` | `AT_TAI`, `AT_MEDIA_INDEX` |

### 9.3 Required: zero is rejected

> **Addendum N (2026-10-01).** A reserved leg (its `legs_disabled` bit set) may have
> `flows[i].ip` and `udp_port` all zero ([17 §2.2](17-nmos-and-ipmx.md)).

`mtl_session_config.direction`, `flows[0].ip`, `mtl_flow.udp_port`, the video and cvideo
`width`, `height`, `fps` and `transport_format`, `mtl_cvideo_config.codec` and
`codestream_bytes`, the audio `format`, `sample_rate` and `channels`,
`mtl_mem_desc.length` (with `va` for an import), `mtl_buffer_desc.plane_count`,
`mtl_group_config.direction`, and `mtl_instance_params.port_count` with each
`ports[].name`. Each returns `-MTL_EINVAL` with reason
`FIELD_REQUIRED`, and `mtl_last_error()` names the field `[C5 §3.7]`.

A P1 AUTO session with a slow renderer gets `RESLOT`. Its frames go out `ON_TIME`, and
each result carries `slots_skipped_before`. In `MTL_COMPLETE_NONE` the same information
is in the `slots_empty` counter of `mtl_session_get_stats` and in the coalesced
`timing_warning` of `mtl_session_get_status()`, so it is visible without reading results
`[C5 §3.7]`.
