# 07 — Catalogue of MTL operating modes, media types, and backends

| Field | Value |
|---|---|
| Topic | Every operating mode, media type, backend and cross-cutting feature the unified session API must cover |
| Revision | r1 |
| Date | 2026-09-29 |
| Baseline | `main` @ `545a266a` (plus uncommitted working tree as found; no files other than this note touched) |
| Evidence labels | **[verified]** = read in code/docs at the cited `path:line`; **[inferred]** = reasoned from code, not executed; **[unknown]** = not established |

## 0. TL;DR

- The public surface today is **5 media families × 2 directions × up to 3 "units of work" (frame / slice / RTP packet) × 2 layers (session, pipeline)**, plus 1 experimental RX family (`st20rc`). That is 20 session create functions and 8 pipeline create functions, each with its own ops struct and its own flag namespace. **[verified]** `include/*.h`.
- Features are **highly asymmetric** across media: e.g. EXACT user pacing only on ST20/ST40, RTP_TIMESTAMP_EPOCH only on ST20, `notify_event` only on ST20/ST22, stats absent on ST22/ST22P, `get_queue_meta` absent on ST30P, RTCP flags on ST30/40/41 are **defined but never read**. A unified API must decide between "uniform contract" and "capability query". **[verified]** see §3.
- Backends impose **silent downgrades** that today are only visible in logs: RL→TSC pacing fallback, chain (zero-copy)→copy TX, DMA→CPU, 2-port→1-port pruning, shared-TX-queue→forced TSC. **[verified]** see §5.
- Easily-forgotten modes: slice-level TX/RX, RTP-level app-built packets (the only route to ST 2022-6), RX user-frame per-pixel-group callback, ST20P per-packet conversion,
  TX ext frame pointing into another session's RX buffer (split-forward), `DATA_PATH_ONLY` app-managed flows, header split, GPU-VRAM RX frames, derive (no-convert) pipelines, ST22P codestream passthrough, ST40 split-by-packet,
  field-as-frame interlace. See §7.

## 1. Public API surfaces (inventory)

| Family | Header | Session TX | Session RX | Pipeline TX | Pipeline RX | Session "types" (unit of work) |
|---|---|---|---|---|---|---|
| ST20 video | `include/st20_api.h` | `st20_tx_create` (`:1842`) | `st20_rx_create` | `st20p_tx_create` (`st_pipeline_api.h:1778`) | `st20p_rx_create` (`:2002`) | FRAME, RTP, SLICE (`st20_api.h:347-360`) |
| ST22 compressed | `include/st20_api.h` | `st22_tx_create` (`:2081`) | `st22_rx_create` (`:2372`) | `st22p_tx_create` (`st_pipeline_api.h:1478`) | `st22p_rx_create` (`:1621`) | FRAME (codestream), RTP (`st20_api.h:365-372`) |
| ST30/31 audio | `include/st30_api.h` | `st30_tx_create` | `st30_rx_create` | `st30p_tx_create` | `st30p_rx_create` (`st30_pipeline_api.h:349`) | FRAME (N packets), RTP |
| ST40 ancillary | `include/st40_api.h` | `st40_tx_create` | `st40_rx_create` | `st40p_tx_create` | `st40p_rx_create` | FRAME, RTP (`st40_api.h:~130`) |
| ST41 fast metadata | `include/st41_api.h` | `st41_tx_create` | `st41_rx_create` | — | — | TX: FRAME, RTP; RX: **RTP only** (`st41_rx_ops` has only `rtp_ring_size`/`notify_rtp_ready`) |
| ST20 redundant-combined (experimental) | `include/experimental/st20_combined_api.h` | — | `st20rc_rx_create` | — | — | FRAME, requires 2 ports |
| Plugins / codecs / converters | `include/st_pipeline_api.h` | `st22_encoder_register` (`:1225`), `st22_decoder_register`, `st20_converter_register` (`:1385`), `st_plugin_register` (`:1440`) | | | | |
| User tasklets | `include/mtl_sch_api.h` | `mtl_sch_create`, `mtl_sch_register_tasklet` | | | | app code on MTL schedulers |
| Standalone convert | `include/st_convert_api.h` | 47 `st20_*`/`st31_*` converters (RFC4175 ↔ planar/v210/y210, AM824 ↔ AES3) | | | | |
| Memory / DMA | `include/mtl_api.h` | `mtl_hp_*`, `mtl_dma_map/unmap`, `mtl_dma_mem_*`, `mtl_udma_*` | | | | |

Consumer usage (grep of create/get/put call sites) **[verified]**: FFmpeg plugin uses only st20p/st22p/st30p; GStreamer only st20p/st30p/st40p; OBS only st20p; Python only st20p/st22p; Rust st20/st20p/st22p/st30p.
Session-level and st41 are used only by `app/sample/` and `tests/tools/RxTxApp/`. Implication **[inferred]**: the pipeline shape is what external integrators depend on;
session-level modes are "power user" and test-harness territory.

Removed but not cleaned **[verified]**: the user-space UDP stack was deleted in `2b182cd87` ("Remove deprecated user-space UDP stack"), yet `MTL_TRANSPORT_UDP` (`mtl_api.h:295-302`) and `MTL_FLAG_UDP_LCORE` (`mtl_api.h:381`) are still public, and `ld_preload/` contains only build stubs (no sources).

## 2. Media types in detail

### 2.1 ST 2110-20 (uncompressed video)

- **Units of work** **[verified]** `st20_api.h:347-360`, `:1180-1276`:
  - FRAME: lib pulls a frame index via `get_next_frame` (tasklet context), reports `notify_frame_done`. RX pushes `notify_frame_ready`, app returns with `st20_rx_put_framebuff`.
  - SLICE: FRAME plus TX `query_frame_lines_ready` (lines ready so far, `st20_tx_slice_meta.lines_ready`), RX `slice_lines` + `notify_slice_ready`. RX SLICE **requires** `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (`st_rx_video_session.c:4317-4321`).
  - RTP: app builds RTP header + payload in an mbuf from `st20_tx_get_mbuf` (`st20_api.h:1932`), lib adds L2-L4 and paces using `rtp_frame_total_pkts`/`rtp_pkt_size` (`:1262`); RX gets raw RTP via `st20_rx_get_mbuf` (`:2279`). RTP-level RX still de-duplicates redundant copies via the per-slot bitmap (`st_rx_video_session.c:1902-1923`).
- **Formats** `enum st20_fmt` (`st20_api.h:320-345`): YUV 4:2:2 / 4:2:0 / 4:4:4 and RGB at 8/10/12/16 bit, plus two **non-RFC4175** "transport as-is" formats `ST20_FMT_YUV_422_PLANAR10LE` (marked "Experimental now, how to support ext frame?") and `ST20_FMT_V210`. Pixel-group helpers `st20_get_pgroup`, `st20_frame_size`, `st20_get_bandwidth_bps`.
- **Packing** BPM / GPM / GPM_SL (`st20_api.h:385-391`); RX ignores `packing` ("RX support all packing type"). Header split only works on BPM (`st20_api.h:222-226`, `stat_pkts_not_bpm` at `st_rx_video_session.c:2378`).
- **Sender pacing profile** `enum st21_pacing` NARROW / WIDE / LINEAR per session (`st20_api.h:311-316`); the pacing **engine** (RL/TSC/TSN/...) is per port, not per session (§5.2).
- **Interlace**: field-as-frame; `height` is full frame, `fps` is fields/s, TX sets `second_field`, lib alternates it and schedules on even/odd field slots (`doc/design.md:483-501`).
- **TX flags** (`st20_api.h:35-111`): USER_P_MAC, USER_R_MAC, EXT_FRAME, USER_PACING, USER_TIMESTAMP, ENABLE_VSYNC, ENABLE_STATIC_PAD_P, ENABLE_RTCP, EXACT_USER_PACING (must be combined with USER_PACING, `st_tx_video_session.c:4040-4045`), RTP_TIMESTAMP_EPOCH, DISABLE_BULK, FORCE_NUMA.
- **TX ops extras**: `linesize` (`:1218`), `start_vrx` (`:1229`), `pad_interval` (`:1234`), `rtp_timestamp_delta_us` (`:1242`), `tx_hang_detect_ms` (`:1247`), `udp_src_port`, `tx_dst_mac`, `rtcp`, `socket_id`; per-frame `user_meta` (≤ `MTL_PKT_MAX_RTP_BYTES`, delivered to RX meta) (`:447`, `:586`).
- **RX flags** (`st20_api.h:176-249`): DATA_PATH_ONLY, ENABLE_VSYNC, ENABLE_RTCP, SIMULATE_PKT_LOSS, FORCE_NUMA, RECEIVE_INCOMPLETE_FRAME, DMA_OFFLOAD, AUTO_DETECT, HDR_SPLIT, DISABLE_MIGRATE, TIMING_PARSER_STAT, TIMING_PARSER_META, USE_MULTI_THREADS.
- **RX ops extras**: `rx_burst_size` (`:1523`), dedicated `ext_frames[]` (`:1550`), dynamic `query_ext_frame` (`:1599`, requires RECEIVE_INCOMPLETE_FRAME, `st_rx_video_session.c:4284-4288`),
  `uframe_size` + `uframe_pg_callback` (per-pixel-group user conversion in tasklet, `:1574`), `notify_detected` (auto-detect reply may set `slice_lines`, `uframe_size`), `mcast_sip_addr` (SSM),
  `ssrc`/`payload_type` filters (0 = off), `gpu_direct_framebuffer_in_vram_device_address` + `gpu_context` (`:1624`).
- **Session queries**: `st20_tx_get_pacing_params` (`:1976`), `st20_tx_get_sch_idx`, `st20_rx_pcapng_dump` (`:2216`), `st20_rx_get_queue_meta` (`:2303`), `st20_rx_dma_enabled`, `st20_rx_timing_parser_critical` (`:2328`), get/reset session stats.

### 2.2 ST 2110-22 (compressed video)

- **Units**: FRAME = one codestream per frame/field, fixed `framebuff_max_size` (`st20_api.h:1339`), real size per frame in `st22_tx_frame_meta.codestream_size`; RTP level as ST20 **[verified]**.
- `pack_type` CODESTREAM only; SLICE packetization "not support now" (`st20_api.h:378-381`) and rejected (`st_tx_video_session.c:4122-4125`).
- Boxes (JPEG-XS headers) on by default; `ST22_TX/RX_FLAG_DISABLE_BOXES`. ST22P forces DISABLE_BOXES for any codec other than JPEG-XS (`st22_pipeline_tx.c:523-525`) **[verified]**.
- **Implementation limits the API does not advertise** **[verified]** `st_tx_video_session.c:3396-3398`, `:3412-3414`: ST22 frame mode **never uses chained (zero-copy) mbufs** and **always downgrades RL pacing to TSC** ("pkts for each frame is vary").
- ST22 is implemented on the ST20 RX/TX session engine (`rv_*`/`tv_*` with an `st22_info`) **[verified]** `st_rx_video_session.c:4934-4935`.
- Missing vs ST20 **[verified]** by header function list: no session stats, no ext frame at session level, no timing parser, no DMA, no auto-detect, no `query_ext_frame` at session level (exists only in `st22p_rx_ops`).

### 2.3 ST 2110-30 / -31 (audio)

- **Formats** PCM8/16/24 and ST31 AM824; sampling 48k/96k/44.1k; ptime 1ms/125us/250us/333us/4ms and ST31 80us/1.09ms/0.14ms/0.09ms (`st30_api.h`, `enum st30_fmt/st30_sampling/st30_ptime`) **[verified]**.
- **Unit of work**: a "frame" is an arbitrary buffer that must be an exact multiple of one packet (`st_tx_audio_session.c:2169-2175`); helper `st30_calculate_framebuff_size` derives a size from a desired frame time. RTP level also available.
- **Per-session pacing engine** `pacing_way` AUTO/RL/TSC (`enum st30_tx_pacing_way`) with RL warm-up knobs `rl_accuracy_ns`, `rl_offset_ns`; RL implies a dedicated queue (`st_tx_audio_session.c:2121-2123`). Video has no per-session engine choice **[verified]**.
- **Flags**: TX USER_P/R_MAC, USER_PACING, USER_TIMESTAMP, BUILD_PACING (pace in builder too), ENABLE_RTCP, DEDICATE_QUEUE, FORCE_NUMA; RX DATA_PATH_ONLY, ENABLE_RTCP, FORCE_NUMA, SIMULATE_PKT_LOSS, RECEIVE_INCOMPLETE_FRAME (missing packets read as silence), TIMING_PARSER_STAT/META (META via `notify_timing_parser_result` every 200 ms, not per frame) (`st30_api.h:34-123`).
- `fifo_size` between builder and pacer (default 10 ms, `ST30_TX_FIFO_DEFAULT_TIME_MS`).
- No `notify_event` (so no VSYNC / recovery events), no ext frame, no EXACT user pacing (by design, `doc/user-pacing-timestamp-contract.md` §Level 1). Deprecated `sample_size`/`sample_num` still in ops.
- Default TX is a **shared queue** per manager unless RL or DEDICATE_QUEUE (`st_tx_audio_session.c:2120-2123`); `st_tx_sessions_queue_cnt` counts all audio sessions as one queue (`st_api.h`) **[verified]**.

### 2.4 ST 2110-40 (ancillary)

- **Unit**: `st40_frame` = up to `ST40_MAX_META` (20) ANC packets (`st40_meta` did/sdid/line/offset/udw) plus UDW buffer (`st40_api.h:308`); pipeline `st40_frame_info` with `udw_buff_addr`, `meta[]`. RTP level also.
- RX FRAME level now exists (transport-owned pool, 64-bit session-merged sequence bitmap for redundancy) and is what ST40P uses (`st40_pipeline_rx.c:175`) **[verified]**; `doc/design.md:604` still says RX is RTP-only (**stale**).
- Interlace: TX `second_field`; RX **auto-detects** from RTP F bits unless `ST40_RX_FLAG_DISABLE_AUTO_DETECT` (`st40_api.h:127`).
- `ST40_TX_FLAG_SPLIT_ANC_BY_PKT`: one ANC per RTP packet, large payloads split across packets, marker on last packet of field (`st40_api.h:80`, `doc/design.md:398-408`).
- `ST40_TX_FLAG_EXACT_USER_PACING` exists (`st40_api.h:74`); dedicated pacing model is location-derived (`doc/user-pacing-timestamp-contract.md` App. D).
- Debug-only `struct st40_tx_test_config test` embedded in public `st40_tx_ops`/`st40p_tx_ops` (mutations active only with `MTL_SIMULATE_PACKET_DROPS`) (`st40_api.h:103-108`).
- Missing: FORCE_NUMA/socket_id on session TX, notify_event, RECEIVE_INCOMPLETE flag (RX always hands up corrupted frames with status; `st_api.h` stat_frames_corrupted doc).

### 2.5 ST 2110-41 (fast metadata)

- TX FRAME (`st41_frame` = one data item) or RTP; RX **RTP only**; `fmd_dit` (22-bit data item type) + `fmd_k_bit`; RX check disabled by `0xffffffff` / `0xff` (`st41_api.h`).
- Minimal flags: TX USER_P/R_MAC, USER_PACING, USER_TIMESTAMP, ENABLE_RTCP (no-op), DEDICATE_QUEUE; RX DATA_PATH_ONLY, ENABLE_RTCP (no-op). No `notify_frame_late`, no pipeline, no `rtp_timestamp_delta_us`. `interlaced` field exists but RX stats say F bits don't exist for ST41 (`st41_api.h` stats comments) **[verified]**.

### 2.6 Other "media" modes

- **ST 2022-6**: not native; documented route is RTP passthrough (`doc/design.md:328`) **[verified]**. Any unified API that drops RTP-level loses this.
- **Raw RTP / app-built packets**: `ST20/22/30/40/41_TYPE_RTP_LEVEL` for every family; ST22 RTP mode requires the app to emit exactly `rtp_frame_total_pkts` per frame (`st20_api.h:1386`).
- **Experimental `st20rc`**: RX that merges two separate ST20 sessions into one frame stream (redundant-combined); used only by a sample and RxTxApp.
- **User UDP**: removed (see §1).

## 3. Feature × media matrix (session layer, with pipeline column where it differs)

Legend: **Y** supported; **N** absent; **noop** flag/field exists but no consumer in `lib/`; **P** partial/conditional. All cells **[verified]** by header + grep of `lib/src/st2110/*.c` and `pipeline/*.c` unless marked.

### 3.1 TX

| Feature | ST20 | ST22 | ST30 | ST40 | ST41 | st20p | st22p | st30p | st40p |
|---|---|---|---|---|---|---|---|---|---|
| Frame unit | Y | Y (codestream) | Y (N pkts) | Y (≤20 ANC) | Y (1 item) | Y | Y | Y | Y |
| Slice unit | Y | N | N | N | N | N | N | N | N |
| RTP unit (app-built) | Y | Y | Y | Y | Y | N | N | N | N |
| 2022-7 (num_port 2) | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| Zero-copy TX (chain mbuf) | P (NIC multi-seg) | N (always copy) | P | P | P | P (derive only) | N | P | P |
| App memory (ext frame) | Y `EXT_FRAME`+`st20_tx_set_ext_frame` | N | N | N | N | Y `put_ext_frame` (+MANUAL_RELEASE) | Y `put_ext_frame` | N | N |
| USER_PACING (TAI only) | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| EXACT_USER_PACING | Y | N | N (by design) | Y | N | Y | N | N | Y |
| USER_TIMESTAMP (TAI or MEDIA_CLK) | Y | Y | Y | Y | Y | Y | Y | **N** | Y |
| RTP_TIMESTAMP_EPOCH | Y | N | N | N | N | Y | N | N | N |
| `rtp_timestamp_delta_us` | Y | N | Y | N | N | Y | N | Y | N |
| VSYNC / `notify_event` | Y | Y | N | N | N | Y | Y | N | N |
| `notify_frame_late` | Y | Y | Y | Y | N | Y | Y | Y | Y |
| DROP_WHEN_LATE | N | N | N | N | N | Y | Y | Y | Y |
| RTCP retransmit | Y | Y | noop | noop | noop | Y | Y | N | noop (forwarded to noop) |
| Per-session pacing engine | N (port) | N (port, forced TSC) | Y `pacing_way` | N | N | N | N | Y | N |
| st21 profile narrow/wide/linear | Y | Y | — | — | — | Y `transport_pacing` | forced NARROW (`st22_pipeline_tx.c:506`) | — | — |
| DEDICATE_QUEUE (else shared) | always dedicated | always dedicated | Y | Y | Y | — | — | Y | Y |
| FORCE_NUMA + `socket_id` | Y | Y | Y | N | N | Y | Y | Y | P ("NOT SUPPORTED" in header, ctx alloc only) |
| DISABLE_BULK | Y | Y | N | N | N | Y | Y | N | N |
| STATIC_PAD_P (E810 RL) | Y | N | N | N | N | Y | N | N | N |
| USER_P/R_MAC | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| per-frame user_meta | Y | N | N | N | N | Y | N | N | N |
| linesize/stride | Y | N | N | N | N | Y | N | N | N |
| Interlaced | Y | Y | — | Y | P | Y | Y | — | Y |
| Queue-hang recovery event | Y (DPDK PMD only) | Y | [unknown] | [unknown] | [unknown] | Y | Y | N | N |
| Session stats get/reset | Y | **N** | Y | Y | Y | Y | **N** | Y | Y |
| `update_destination` | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| pacing params / sch idx | Y / Y | N / Y | N | N | N | Y / Y | N | N | N |
| Test mutation config | N | N | N | Y (debug build) | N | N | N | N | Y |
| Split ANC by packet | — | — | — | Y | — | — | — | — | Y |

Notes: RTCP no-op for ST30/40/41 — grep for `ST30_*/ST40_*/ST41_*_FLAG_ENABLE_RTCP` in `lib/` hits only `st40_pipeline_{tx,rx}.c:317,182` which forward it; the session files never read it.
`doc/rtcp.md` correctly lists only st20/st22/st22p (+st20p). Chain decision: `st_tx_video_session.c:3395-3402`, audio `st_tx_audio_session.c:2146`, anc `st_tx_ancillary_session.c:1717`, fmd `st_tx_fastmetadata_session.c:1461`.

### 3.2 RX

| Feature | ST20 | ST22 | ST30 | ST40 | ST41 | st20p | st22p | st30p | st40p |
|---|---|---|---|---|---|---|---|---|---|
| Frame unit | Y | Y | Y | Y | **N** | Y | Y | Y | Y |
| Slice unit | Y (needs INCOMPLETE flag) | N | N | N | N | N | N | N | N |
| RTP unit | Y | Y | Y | Y | Y | N | N | N | N |
| 2022-7 merge | Y (packet bitmap) | Y | Y (timestamp dedup) | Y (seq bitmap) | Y | Y | Y | Y | Y |
| Deliver incomplete frames | flag | flag | flag (zeros = silence) | always, with status | — | flag | flag | flag | always |
| Ext frames dedicated | Y `ext_frames[]` | N | N | N | N | Y | N | N | N |
| Ext frames dynamic | Y `query_ext_frame` | N | N | N | N | Y `EXT_FRAME`+query | Y `EXT_FRAME`+query | N | N |
| User-frame per-pgroup callback | Y `uframe_pg_callback` | N | N | N | N | Y `PKT_CONVERT` (3 fmts) | N | N | N |
| Auto-detect | Y (w/h/fps/packing/interlace, not fmt) | N | N | Y (interlace only, default on) | N | Y | N | N | Y |
| Timing parser (ST2110-21 / audio) | Y STAT/META | N | Y STAT/META(200 ms cb) | N | N | Y | N | **N** | N |
| DMA copy offload | Y (not with uframe/hdr split) | N | N | N | N | Y | N | N | N |
| Header split | Y (BPM, 1 port, patched DPDK) | N | N | N | N | Y | N | N | N |
| 2 RX threads (>40G) | Y (frame only, not slice, not 2-port) | N | N | N | N | Y | N | N | N |
| GPU VRAM frames | P (raw fields) | N | N | N | N | Y flag | N | N | N |
| VSYNC event | Y | Y | N | N | N | Y | Y | N | N |
| RTCP NACK | Y | Y | noop | noop | noop | Y | Y | N | noop |
| DATA_PATH_ONLY (app flows) | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| SIMULATE_PKT_LOSS | Y | Y | Y | N | N | Y | Y | Y | N |
| FORCE_NUMA | Y | Y | Y | N | N | Y | Y | Y | N ("NOT SUPPORTED") |
| DISABLE_MIGRATE | Y | N | N | N | N | Y | N | N | N |
| SSRC / PT / SSM source filter | Y | Y | Y | Y | Y (+DIT/K) | Y | Y | Y | Y |
| `update_source` | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| `get_queue_meta` | Y | Y | Y | Y | Y | Y | Y | **N** | Y |
| pcapng dump | Y | Y | N | N | N | Y | Y | N | N |
| Session stats | Y | **N** | Y | Y | Y | Y | **N** | Y | Y |
| `rx_burst_size` | Y | N | N | N | N | Y | N | N | N |

Constraint citations **[verified]**: hdr split 1 port `st_rx_video_session.c:4290-4295`; hdr split needs `ST_HAS_DPDK_HDR_SPLIT` from patched DPDK else `-ENOTSUP` (`:3036-3044`),
and the pinned `DPDK_VER=26.07` (`versions.env:1`) has **no** `patches/dpdk/26.07/hdr_split/` directory, so header split is unbuildable on the current pin; DMA skipped with uframe or hdr split `:2569-2571`;
2-thread RX auto above 40 Gbps or by flag, rejected for slice/2-port `:2640-2665`; slice needs INCOMPLETE `:4317-4321`.

## 4. Pipeline-layer features

| Feature | st20p | st22p | st30p | st40p | Evidence |
|---|---|---|---|---|---|
| Transport session type used | FRAME only | FRAME only | FRAME only | FRAME only | `st20_pipeline_tx.c:454`, `st22_pipeline_tx.c:513`, `st30_pipeline_tx.c:293`, `st40_pipeline_rx.c:175` |
| App frame formats | `enum st_frame_fmt` (planar/packed YUV, RGB, RFC4175 BE, CUSTOM8, codestreams) | raw fmt ↔ codestream | PCM (no conversion) | UDW + meta | `st_pipeline_api.h:~50-170` |
| In-lib SIMD conversion | Y — runs **synchronously in the app thread** inside `put_frame`/`get_frame` | N | — | — | `st20_pipeline_tx.c:868-869`, `st20_pipeline_rx.c:841-878` |
| Plugin conversion (`st20_converter_dev`) | Y, preferred; internal fallback | — | — | — | `st20_pipeline_tx.c:611-645` |
| Per-packet conversion in tasklet | Y `ST20P_RX_FLAG_PKT_CONVERT` via `uframe_pg_callback` | — | — | — | `st20_pipeline_rx.c:535-536` |
| Derive (no conversion, zero-copy to transport) | Y when app fmt == transport fmt | Y when app fmt is the codestream fmt (codec bypass) | implicit | implicit | `st20_pipeline_tx.c:1123`, `st20_pipeline_rx.c:1053`, `st22_pipeline_tx.c:994-996`, `st22_pipeline_rx.c:757-759` |
| Codec plugins | — | `st22_encoder_dev`/`decoder_dev`, `codec` JPEGXS/H264(CBR)/H265(CBR), `quality`, `codec_thread_cnt`, `codestream_size` | — | — | `st_pipeline_api.h:670-785`, `:1066-1212` |
| Ext frame semantic | TX: in derive → transport zero-copy; otherwise conversion **source** only | TX ext = encoder source; RX ext = decoder dest | — | — | `st20_pipeline_tx.c:459-460` |
| BLOCK_GET + `wake_block` + `set_block_timeout` | Y | Y | Y | Y | bit 15 in every pipeline flag enum |
| `notify_frame_available` | Y | Y | Y | Y | ops |
| Threading contract | one app thread per session per direction; callbacks synchronous in tasklet | same | same | same | `doc/design.md:355-371` |
| Frame-status / late helpers | `st_frame_is_late` | same | `st30_frame_is_late` | `st40_frame_is_late` | headers |

Pipeline flag bit positions are **not** aligned with session flag bits (e.g. `ENABLE_RTCP` is bit 2 in `ST20_RX_FLAG_*` but bit 4 in `ST20P_RX_FLAG_*`; bit 2 is `EXT_FRAME` in st20p RX), so every pipeline translates flags one by one (`st20_pipeline_rx.c:508-546`) **[verified]**. A unified flag space would remove ~150 lines of translation per pipeline **[inferred]**.

Plugin model **[verified]**: `.so` plugins loaded at `mtl_init` from JSON (`KAHAWAI_CFG_PATH`, `doc/plugin.md`), max `ST_MAX_DL_PLUGINS = 8` (`st_header.h:63`);
devices can also be registered in-process by the app via `st22_encoder_register` etc. Match is by `input_fmt_caps`/`output_fmt_caps` bitmask and `ST_PLUGIN_DEVICE_*` target (CPU/GPU/FPGA/AUTO).
In-tree plugins: `plugins/sample` (JPEG-XS/H264-CBR stand-in and a converter) and `plugins/st22_avcodec` (H.264/H.265 via FFmpeg).

## 5. Transports, backends and their restrictions

### 5.1 Backend matrix

| Backend (port prefix) | `enum mtl_pmd_type` | Status | Pacing available | TX zero-copy | RX steering | CNI (ARP/IGMP/PTP in MTL) | Notes |
|---|---|---|---|---|---|---|---|
| DPDK PMD (PCI BDF) | `MTL_PMD_DPDK_USER=0` | production | RL (ice PF/iavf VF TM), TSC, TSC_NARROW, TSN (E830 PF), PTP, BE | Y if NIC multi-seg | flow director, or shared RSS if NIC has no FDIR (ixgbe, ena) | Y | queue-hang recovery only here (`st_tx_video_session.c:4167`) |
| Native AF_XDP (`native_af_xdp:`) | `=4` | production | RL via sysfs `tx_maxrate` on ice, else TSC | **N** (copies every segment into UMEM, `mt_af_xdp.c:582-584`) | XDP prog + per-queue xsk; MtlManager brokers xsks map/queues/flows (`mt_instance.h:15-24`) | N (kernel) | optional build (`lib/meson.build:28-34`); CAP_NET_RAW; ZC with copy fallback (`mt_af_xdp.c:422-446`) |
| Kernel socket (`kernel:`) | `=17` | experimental | TSC only (no RL type) [inferred from `dev_if_init_pacing`] | **N** (single segment only, `mt_dp_socket.c:36-37`) | none (`MT_DRV_F_RX_NO_FLOW`), socket per flow | N | still needs hugepages (`doc/kernel_socket.md`); RX buffer sizing needs CAP_NET_ADMIN or `rmem_max` |
| DPDK AF_XDP (`dpdk_af_xdp:`) | `=19` | experimental | [unknown] | P (mbuf pool reuse for ZC, `st_tx_video_session.c:3381-3385`) | flows | N | overlaps native AF_XDP |
| DPDK AF_PACKET (`dpdk_af_packet:`) | `=20` | experimental | TSC | [unknown] | none | Y | "testing only — very slow" (KB §7) |

Driver capability table: `lib/src/dev/mt_dev.c:14-107` (`MT_DRV_F_*` flags defined at `mt_main.h:661-679`) **[verified]**. Doc drift: `doc/experimental/af_xdp.md:54` uses prefix `af_xdp:` but code only recognises `dpdk_af_xdp:` (`mt_util.c:940`) **[verified]**.

### 5.2 Cross-cutting transport features and the constraint each imposes on the API

| Feature | Where configured | Constraint / silent behaviour | Evidence |
|---|---|---|---|
| TX pacing engine | per port, `mtl_init_params.pacing` (`enum st21_tx_pacing_way` AUTO/RL/TSC/TSN/PTP/BE/TSC_NARROW) | video sessions inherit port engine; ST22 forced RL→TSC; AUTO falls back RL→TSC with a warn | `st_tx_video_session.c:3409-3415`, `mt_dev.c:1443-1500` |
| Shared TX queue | `MTL_FLAG_SHARED_TX_QUEUE` | **forces TSC pacing for the whole port** | `mt_dev.c:1446-1450` |
| Shared RX queue | `MTL_FLAG_SHARED_RX_QUEUE` | software dispatch after flow rule | `mt_queue.c:66-70` |
| Shared RSS | `rss_mode` or auto when NIC has no FDIR | auto-enabled L3_L4 for `MT_FLOW_NONE` drivers | `mt_dev.c:2277-2280`, `mt_queue.c:61-65` |
| Multicast / IGMPv3 / SSM | per RX session `ip_addr` + `mcast_sip_addr`; `MTL_FLAG_NO_MULTICAST` for SDN | join skipped under DATA_PATH_ONLY | `st_rx_video_session.c:3089-3094` |
| 2022-7 | `num_port = 2` everywhere | down ports silently pruned (2→1) with `MTL_FLAG_ALLOW_DOWN_PORTS` / `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION` | `st_tx_video_session.c:3935` (ST20), `:4051` (ST22), `mtl_api.h:487-529` |
| Header split | `nb_rx_hdr_split_queues` + session flag | patched DPDK only; not on current pin | §3.2 |
| DMA engines | `dma_dev_port[]` in init params | ST20 RX only; CPU fallback silent | `st20_api.h:207-212` |
| TX chain vs copy | `MTL_FLAG_TX_NO_CHAIN`, NIC multi-seg | copy mode silently chosen per session | `st_tx_video_session.c:3399-3405` |
| IOVA mode | `iova_mode` | `mtl_dma_map`/`unmap` refuse non-VA; `mtl_udma_create` refuses PA → ext frames from arbitrary user memory need IOVA VA | `mt_main.c:845-848`, `:906-909`, `:1151-1154` |
| HW RX timestamps | `MTL_FLAG_ENABLE_HW_TIMESTAMP` | PF on E810 only; timing parser otherwise uses SW time | `mtl_api.h:369-373`, `doc/design.md:586-589` |
| PTP | built-in (`MTL_FLAG_PTP_ENABLE`, PI, unicast), or app `ptp_get_time_fn` | VF has no HW timesync (~1 µs) | `doc/design.md:279-296` |
| TSN launch time | pacing TSN | E830 + PF + built-in PTP, checked at init | `doc/design.md:232-238` |
| Scheduler | lcore (default) vs `MTL_FLAG_TASKLET_THREAD`; `lcores` list; quota `data_quota_mbs_per_sch`; `tx/rx_audio_sessions_max_per_sch` | tasklet callbacks must not block | KB §2 |
| Migration | `MTL_FLAG_TX_VIDEO_MIGRATE` / `RX_VIDEO_MIGRATE`; per-session `DISABLE_MIGRATE` | video only | `mtl_api.h:345-356` (TX doc text wrongly says "rx") |
| Multi-process | SR-IOV + MtlManager lcore arbitration; legacy SysV shm lcore map (`mtl_lcore_shm_api.h`) | DPDK secondary process is **not** possible: EAL always gets `--in-memory` (`mt_dev.c:344`); KB §7 "Multi-Process" claim of `--proc-type=secondary` stats is unsupported by code | `doc/design.md:45-51` |
| Windows | `lib/windows/`, NetUIO | no MtlManager, no kernel socket, no AF_XDP (code guarded by `#ifndef WINDOWSENV`) | `mt_instance.c:7`, `mt_dp_socket.c:24`, `lib/meson.build:115` |
| Network header controls | — | **none**: TTL hard-coded 64, TOS/DSCP 0, no VLAN, IPv4 only, MTU ≤ 1500 | `st_tx_video_session.c:944-945`, `mtl_api.h:79-89` |
| Source port | `udp_src_port`, `MTL_FLAG_RANDOM_SRC_PORT`, `MTL_FLAG_MULTI_SRC_PORT` (ST20) | global flags alter per-session behaviour | `st_tx_video_session.c:3372-3377` |

## 6. Session-management features

| Capability | Coverage today | Notes for unified API |
|---|---|---|
| Create before or after `mtl_start` | all | `doc/design.md:66-103` |
| Runtime destination / source change | all session + pipeline TX/RX incl. st41, st30p, st40p | RX update tears down and rebuilds queue, flow and IGMP under the session spinlock (`st_rx_video_session.c:3929-3992`); cannot change NIC port, `num_port`, SSRC, PT |
| Queue metadata for app-managed flows | RX families except st30p | pair with DATA_PATH_ONLY |
| Port description | session ops: flat arrays `port[]`, `dip_addr[]`, `udp_port[]`, `num_port`, `udp_src_port[]`, `tx_dst_mac[]`; pipelines: `struct st_tx_port`/`st_rx_port` (adds `payload_type`, `ssrc`) | two shapes for the same thing (`st_pipeline_api.h:838-879`) |
| SSRC / payload type | TX: SSRC 0 → random; RX: 0 disables check | uniform |
| Per-session NUMA override | FORCE_NUMA + `socket_id` (not ST40/41) | plus port-level `MTL_PORT_FLAG_FORCE_NUMA` |
| Events | `ST_EVENT_VSYNC`, `RECOVERY_ERROR`, `FATAL_ERROR` (`st_api.h`) only via ST20/ST22 `notify_event` | audio/anc/fmd cannot learn about fatal errors |
| Stats | per-family structs with common head `st_tx_user_stats`/`st_rx_user_stats` | ST22/ST22P have none |
| Observability | pcapng dump (video RX), USDT probes, timing parser | |
| Scheduler introspection | `*_get_sch_idx` (video only) | |

## 7. Easily-forgotten modes (checklist for the unified design)

1. **Slice-level low latency** — TX `query_frame_lines_ready` polled from the tasklet, RX `notify_slice_ready` every `slice_lines`; only ST20 session; requires incomplete-frame delivery; incompatible with 2-thread RX.
2. **RTP-level app-built packets** — every family; the only route to ST 2022-6 and custom payloads; app owns RTP header, lib owns L2-L4 + pacing; RX still dedups 2022-7.
3. **RX user-frame per-pixel-group callback** (`uframe_size` + `uframe_pg_callback`) — app converts inside the tasklet as packets land; disables DMA.
4. **ST20P per-packet conversion** (`PKT_CONVERT`) — the pipeline form of #3, limited to three output formats.
5. **Derive pipelines** — st20p/st22p with no conversion/codec; st22p derive is effectively "ST22 codestream frame mode with get/put".
6. **Ext frame in both TX flavours** — ST20 session `set_ext_frame` per index; st20p `put_ext_frame` with optional two-phase `EXT_FRAME_MANUAL_RELEASE` + `st20p_tx_notify_ext_frame_free` (`st_pipeline_api.h:1869`).
7. **Split-forward** — TX ext frame pointing at an offset inside another session's RX frame with a `linesize` (4K → 4×1080p tiles) (`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:127-131`).
8. **RX dedicated vs dynamic ext frames** — fixed `ext_frames[]` array at create vs `query_ext_frame` per frame (needs incomplete-frame flag).
9. **GPU VRAM RX frames** — st20p only; DMA disabled; compile-time dependency on `gpu_direct` (`doc/gpu.md:44-53`).
10. **DATA_PATH_ONLY** — app installs flows and joins multicast; documented "for non MTL_PMD_DPDK_USER". **[inferred] likely broken**: RX video passes a NULL flow (`st_rx_video_session.c:3050-3053`) and `mt_rxq_get` dereferences `flow->flags` unless the port is a kernel socket (`datapath/mt_queue.c:56`, `:76`). Needs a test before the new API commits to this mode.
11. **Exact user pacing** vs user pacing (snap to nearest regular slot) vs normal — ST20/ST40 only; ST30 user pacing re-anchors the audio grid (`doc/user-pacing-timestamp-contract.md` Level 1).
12. **RTP_TIMESTAMP_EPOCH** and `rtp_timestamp_delta_us` — RTP clock policy knobs separate from pacing.
13. **DISABLE_BULK / TSC_NARROW / STATIC_PAD_P / start_vrx / pad_interval** — pacing-tuning knobs exposed per session.
14. **2-thread RX** (auto >40 Gbps) — an implicit extra scheduler consumer.
15. **Header split** — single port, BPM, patched DPDK.
16. **Field-as-frame interlace** with lib-managed field parity; RTP-level apps that never set `ST20_SECOND_FIELD` transmit at half rate (`doc/design.md:494-500`).
17. **ST40 split-by-packet** and interlace auto-detect.
18. **ST30 `BUILD_PACING`**, `fifo_size`, per-session RL warm-up knobs.
19. **ST22P codec threads** (`codec_thread_cnt`) and encoder/decoder `resp_flag` BLOCK_GET — plugins own threads, not tasklets.
20. **Notify-driven vs polled completion** — session layer is purely callback-driven from tasklet context (lib pulls `get_next_frame`); pipelines are get/put with optional `notify_frame_available` and optional blocking.
21. **Non-RFC4175 transport formats** (`ST20_FMT_YUV_422_PLANAR10LE`, `ST20_FMT_V210`, `ST_FRAME_FMT_*CUSTOM8`) — transport bytes are not SMPTE-compliant.
22. **Debug knobs in public ops** — `SIMULATE_PKT_LOSS`, `st40_tx_test_config`, `MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS` + `port_packet_loss[]`.

## 8. Documentation / API drift found while cataloguing

| # | Drift | Evidence |
|---|---|---|
| 1 | `ST40P_TX_FLAG_EXT_FRAME` referenced but does not exist | `doc/design.md:384` vs `st40_pipeline_api.h:81-131` |
| 2 | "ST40 RX: RTP passthrough is the only supported" is stale | `doc/design.md:604` vs `st40_api.h` FRAME_LEVEL RX |
| 3 | `st40p_rx_ops.rtp_ring_size` documented Mandatory, unused by pipeline | `st40_pipeline_api.h:234`; no use in `st40_pipeline_rx.c` |
| 4 | ST30/40/41 `ENABLE_RTCP` flags have no consumer | grep, §3.1 note |
| 5 | `ST40P_*_FLAG_FORCE_NUMA` "NOT SUPPORTED YET", yet TX uses it for ctx memory | `st40_pipeline_api.h:118`, `st40_pipeline_tx.c:675` |
| 6 | `af_xdp:` prefix in experimental doc | `doc/experimental/af_xdp.md:54` vs `mt_util.c:940` |
| 7 | KB claims secondary-process stats access | KB `:768-769` vs `mt_dev.c:344` |
| 8 | `MTL_FLAG_TX_VIDEO_MIGRATE` doc text describes RX | `mtl_api.h:345-350` |
| 9 | UDP transport remnants public after removal | `mtl_api.h:295-302,381`; commit `2b182cd87` |
| 10 | `st_convert_internal.h` is installed as a public header | `include/meson.build` |

## 9. Classification for the unified API (opinion, all **[inferred]**)

| Mode / feature | Class | Rationale |
|---|---|---|
| Frame unit for ST20/22/30/40 (TX+RX), get/put + callback completion | **v1** | what every ecosystem consumer uses |
| ST41 frame TX, RTP RX | **v1** (and add frame RX) | small; parity avoids a special case |
| 2022-7 redundancy (2 ports) | **v1** | universal today |
| App memory import (ext frame) TX/RX incl. two-phase release | **v1** | see PR #1610 review; zero-copy is core value |
| User pacing / exact / user timestamp / epoch RTP / delta | **v1** as one timing policy struct for all media | removes 5 inconsistent flag sets |
| Drop-when-late, notify-late, incomplete-frame delivery with status | **v1** | |
| Runtime update source/destination | **v1** | switching use-cases |
| Pixel-format conversion (in-lib SIMD) and ST22 codec plugins | **v1** as optional stages | pipelines are the popular API |
| Stats + events (fatal/recovery/vsync) for every media | **v1** | today missing on ST22 and ST30/40/41 |
| RTP / packet unit | **v1 in the API shape**, implementation may lag | only route to 2022-6; hard to bolt on later |
| Slice unit | **v1 in the API shape** (partial-frame progress), implementation later | same retrofit argument |
| Backends: DPDK PMD, native AF_XDP, kernel socket | **v1** with capability reporting | |
| Shared TX/RX queue, RSS, multicast/SSM | **v1** (port-level) | |
| Timing parser, auto-detect, DMA offload, pcap dump, 2-thread RX, VSYNC | **later** | valuable but additive |
| RTCP retransmit (non-standard) | **later**, ST20/22 only | noop elsewhere today |
| DATA_PATH_ONLY | **later / verify** | possibly broken (§7 #10) |
| GPU VRAM frames | **later**, via the memory-domain model | |
| Header split | **deprecate candidate** | unbuildable on DPDK 26.07 pin; 1-port only |
| RX `uframe_pg_callback` | **deprecate candidate** (keep PKT_CONVERT-like stage) | app code in tasklet context |
| `st20rc` experimental | **deprecate candidate** | superseded by 2-port sessions |
| DPDK AF_XDP, DPDK AF_PACKET PMDs | **deprecate candidate** | native AF_XDP / kernel socket cover them |
| SysV shm lcore manager | **deprecate** | docs already advise against (`doc/design.md:51`) |
| `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | **remove** | dead after `2b182cd87` |
| Non-RFC4175 transport formats | **later or deprecate** | non-compliant on wire |
| Pacing-tuning knobs (DISABLE_BULK, STATIC_PAD_P, start_vrx, pad_interval) | **advanced/debug struct** | keep, but out of the main ops |
| Test mutation knobs | **separate debug API** | should not be in stable ABI |

## Open questions for the maintainer

1. **Is RTP/packet a first-class unit of the unified session, or a separate raw API?** Every family has RTP level today and ST 2022-6 depends on it, but its ownership model (mbuf get/put, app-built headers) is unlike frames. Options: (a) one session type with `unit = frame | slice | packet`; (b) a separate `mtl_rtp_*` API sharing ports/pacing; (c) drop from v1 and keep legacy headers.
2. **Does v1 commit to slice (partial-frame) progress?** Slice is ST20-only, needs incomplete-frame delivery, and conflicts with 2-thread RX. Retrofitting partial-buffer progress into a frame-only buffer contract is hard. Options: (a) model as "buffer progress callback/line watermark" in v1 even if only ST20 implements it; (b) defer; (c) deprecate.
3. **Where does the pacing engine live?** Video inherits a per-port engine (and ST22 silently forces TSC), audio chooses per session, shared TX queue forces TSC for the whole port. Options: (a) per-port only, session requests a profile; (b) per-session request with negotiated result reported back; (c) keep today's split.
4. **How are silent downgrades surfaced?** RL→TSC, chain→copy, DMA→CPU, hdr-split→off, 2→1 port pruning all happen with only a log line. Options: (a) create-time "negotiated capabilities" out-struct; (b) strict mode flag that fails create instead; (c) stats counters only.
5. **Uniform features or capability query?** Many gaps are accidental (st30p lacks USER_TIMESTAMP, ST22 lacks stats, ST30/40/41 lack events and RTCP is a no-op, st30p lacks `get_queue_meta`). Options: (a) make the contract uniform and fill gaps; (b) keep gaps but expose a per-media capability bitmap; (c) reject unsupported combinations at create with `-ENOTSUP` (today they are often ignored).
6. **Which completion models does v1 expose?** Session layer = callbacks in tasklet context (must not block); pipeline = get/put (single app thread) with optional notify and blocking wait. Options: (a) both, explicitly; (b) get/put + optional event fd/notify only, no user code in tasklets; (c) callbacks only.
7. **Keep in-lib SIMD conversion running in the caller's `put_frame`/`get_frame` thread?** Today conversion cost lands on the app thread for internal converters but on plugin threads for plugin converters. Options: (a) keep; (b) move to a lib-owned worker; (c) make conversion an explicit, separately scheduled stage.
8. **What to do with header split?** It requires DPDK patches that do not exist for the pinned 26.07 and works only single-port BPM. Options: (a) deprecate; (b) keep behind a capability bit; (c) port the patch forward.
9. **Is DATA_PATH_ONLY (app-managed flows) still wanted?** It appears to dereference a NULL flow on non-kernel-socket backends (`mt_queue.c:56`). Options: (a) drop; (b) keep and fix with a test; (c) replace with an explicit "external steering" port mode.
10. **RTCP retransmission scope.** Non-standard, implemented only for ST20/ST22, flags exist elsewhere as no-ops. Options: (a) keep for video only and remove other flags; (b) generalize to all media; (c) move to "later".
11. **Network header controls.** No DSCP/TOS (hard-coded 0), TTL fixed 64, no VLAN, IPv4 only. Broadcast plants usually mark DSCP. Options: (a) add DSCP/TTL (and reserve VLAN/IPv6) in the v1 port/flow description; (b) defer; (c) port-level defaults only.
12. **One port/flow description shape.** Session ops use flat arrays, pipelines use `st_tx_port`/`st_rx_port` (which also carry PT and SSRC); RX update cannot change `num_port` or NIC. Options: (a) a single flow-descriptor struct per leg (P/R) for all media and for update calls; (b) keep both.
13. **Interlace model.** Today every family treats a field as a frame and the lib manages parity. Options: (a) keep field-as-buffer with explicit `field` metadata; (b) introduce frame-of-two-fields buffers for pipelines; (c) both, selectable.
14. **Fate of legacy/experimental surfaces in v1**: `st20rc`, DPDK AF_XDP, DPDK AF_PACKET, SysV shm lcore, UDP remnants, non-RFC4175 transport formats, `st_convert_internal.h` as public. Options per item: keep, freeze in a `legacy/` header, or remove.
15. **Debug/test knobs in the stable ABI** (`SIMULATE_PKT_LOSS`, `st40_tx_test_config`, redundant packet-loss simulation). Options: (a) separate `mtl_debug_*` API; (b) keep in ops under a clearly marked sub-struct; (c) compile-time only.
16. **Multi-process expectations.** The KB claims secondary-process stats access, but EAL runs `--in-memory`, so only SR-IOV + MtlManager multi-instance works. Options: (a) state "one process per MTL instance" as a v1 non-goal; (b) design a manager-mediated stats channel; (c) support DPDK secondary processes (drop `--in-memory`).
