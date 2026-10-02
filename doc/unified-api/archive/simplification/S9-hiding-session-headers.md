# S9 — Hiding the session-level headers: one public API, every feature kept

| | |
|---|---|
| Status | Proposal for maintainer review (simplification track, S9). Nothing implemented; no other file edited |
| Date | 2026-10-01 |
| Baseline | `main` @ `545a266a`; design revision 3 (`doc/unified-api/`); normative sketch `sketch/include/mtl/experimental/mtl_unified.h` |
| Trigger | Owner requirement: keep RTP passthrough, hide `include/st20_api.h`, expose only the highest-level API, lose no functionality |
| Sibling | S8 designs the RTP-passthrough (app-built packets) mode itself. S9 covers everything else needed to make the lower headers non-public |
| Labels | **[verified]** read at the cited `path:line`; **[inferred]** reasoned from verified facts; **[external]** prior art from upstream docs or headers, verified where a local copy or fetch is cited, otherwise from general knowledge |

## TL;DR

1. **What "hide" means here.** The session-level headers (`st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`, `st_api.h`) are the vocabulary of *every* other installed header, not just a lower layer. Hiding them is a header-tier and symbol-tier change, not a deletion. **[verified]**
   The session layer stays as an internal engine: the pipelines call `st20_tx_create` today (`lib/src/st2110/pipeline/st20_pipeline_tx.c:482`), and S8's passthrough adapter will too.
2. **The target public set** is self-contained, stdint-only headers:
   - `mtl/mtl_unified.h` (it includes only `<stddef.h>` and `<stdint.h>`, `mtl_unified.h:81-82` **[verified]**), with `mtl_simple.h` and `mtl_debug.h`;
   - two new small headers: `mtl/mtl_plugin.h` (codec and converter plugin ABI v2) and `mtl/mtl_convert.h` (colour conversion; Q-MODE-4).

   Nothing in that set includes a legacy header.
3. **Mechanism: three tiers, staged by release.**
   - **Public**: pkg-config `mtl-unified`, which becomes the default `mtl` at F+1.
   - **Legacy**: `include/mtl/legacy/`, which needs `MTL_LEGACY_API`, carries `MTL_LEGACY_DEPRECATED` on every prototype and its own `mtl-legacy.pc`.
   - **Internal**: `lib/include/`, never installed; `MTL_INTERNAL` version node with a DPDK-style `__rte_internal` guard.

   Version nodes split the symbols three ways: `MTL_LEGACY_SESSION`, `MTL_LEGACY_PIPELINE` and `MTL_LEGACY_CORE`. Every stage is cheap, and each is reversible until the last one.
4. **Session-only features: 74 rows catalogued** (§3, every one counted in the §3.10 tally):
   - **34 COVERED** by the sketch;
   - **11 PARTIAL** (mostly format and helper parity);
   - **6 LATER** (named in the design but scheduled after v1, or under `MTL_UNIFIED_LATER`);
   - **5 NOT COVERED**: video RX `query_ext_frame` (H-09), per-frame timing-parser META (H-07), pcapng dump (H-11), ANC/RFC 8331 helpers (H-13), log sinks (H-14);
   - **12 rows behind 10 proposed CUTs**: header split, user per-pgroup RX callbacks, `st20rc`, public per-pair convert functions, public user DMA, public lcore borrow, `ST22_*_FLAG_DISABLE_BOXES`, the RTCP no-op flags on audio/ANC/fastmeta, trivial wrappers, queue meta;
   - **5 S8 rows**: RTP level for all five essences, owned by S8;
   - **1 internal-only** row (the lcore-shm admin tool).

   All the PARTIAL, LATER and NOT COVERED rows, together with the S8 work, become a **pre-hide gate**: H-01 to H-19 (§7).
5. **In-tree blockers are few** (§3.9):
   - RxTxApp's ANC and fast-metadata apps, plus the five legacy JSON kinds behind the acceptance suite. The `st41` acceptance cases run on the session API.
   - `app/v4l2_to_ip`, the MXL POC, Rust `video.rs`, Python SWIG, `plugins/`, and the FFmpeg/GStreamer helper calls.

   KahawaiTest, unit and fuzz tests are **not** blockers: they move to the internal tier and keep testing the session engine.
6. **Timeline** (§8). Phase 0 does the symbol nodes and the gate machinery, all inert. Phase 1 moves the files, with forwarding stubs. Phases 2–6 close the gaps. Then the release stages: at the ABI-freeze release **F**, deprecation warnings; at **F+1**, opt-in only; at **F+2** at the earliest (14 §7), headers not installed, `MTL_LEGACY_SESSION` becomes internal, and libmtl's soname is bumped.
7. **Decisions this changes** (§9): D-25 / NG2 / Q-MODE-1 (RTP level enters the unified API, via S8); Q-ABI-4 (session-level APIs are hidden after the freeze, not just frozen); 09 §3 "later" items pulled before the hide; and a concrete answer for D-23's freeze-time DSO question, which is to keep `libmtl_unified.so.1` separate from libmtl.

---

## 1. The requirement and what it changes

The owner's requirement has two halves. **(a)** RTP passthrough stays supported, but in a coherent API. **(b)** `include/st20_api.h` and its siblings stop being public, with no loss of functionality. Revision 3 assumed the opposite on both points:

| Today's design position | Where | Conflict |
|---|---|---|
| RTP level stays in the legacy API for v1; `MTL_UNIT_PACKET_CHUNK` is only reserved | D-25, NG2 (`01-goals-and-requirements.md:131`), Q-MODE-1 (a), `mtl_unified.h:410` | (a) and (b) together mean RTP level must reach the unified API before the legacy header can go. S8 owns that design **[verified]** |
| Session-level callback APIs, slice and RTP modes "stay on their current implementation and are frozen" | 11 §5, Q-ABI-4 | Frozen-but-public contradicts (b). Proposed: deprecated at the freeze, then hidden (§8) **[verified]** |
| Legacy APIs keep working unchanged in v1 | D-24 | Compatible. Nothing is hidden before the ABI-freeze release F, and removal follows the two-release rule of 14 §7 / D-69 **[verified]** |
| Several session-only features are "later" or "X" (RTCP, video auto-detect, timing parser per unit, header split, `uframe_pg_callback`) | 09 §3, §8 | Hiding the headers turns every "later" into "before the hide, or an approved CUT" (§7) **[inferred]** |

Hiding the headers does **not** mean hiding the session *engine*. Its public symbols are already its internal API:
- the pipelines create sessions through it (`st20_pipeline_rx.c:599`, `st22_pipeline_tx.c:527`, `st30_pipeline_tx.c:300`, `st40_pipeline_rx.c:186`) and set ext frames through it (`st20_pipeline_tx.c:963`) **[verified]**;
- the L1 adapters for packet-chunk (S8) and rows units, which do not go through a pipeline (02 §2.2: "the only L1 adapter that does not go through a pipeline"), will call it too **[verified]**.

## 2. Today's public header set

### 2.1 What is installed, and how it is found

- All 13 top-level headers are installed into `<prefix>/include/mtl/` (`include/meson.build:4-8`), and `experimental/st20_combined_api.h` into `mtl/experimental/` (`:11-16`) **[verified]**.
- The generated `mtl_build_config.h` (version and compiler string, `meson.build:39-53`) is installed too, because `mtl_api.h:27` includes it **[verified]**.
- pkg-config emits `-I${includedir} -I${includedir}/mtl` (`meson.build:77-84`, `subdirs : ['.', meson.project_name()]`), so `<mtl/st20_api.h>` and `<st20_api.h>` both resolve. Apps use the `<mtl/…>` form (`app/sample/sample_util.h:11`, `tests/tools/RxTxApp/src/app_base.h:12`); the MXL POC uses the bare form (`ecosystem/MTL_with_MXL/poc/src/include/poc_mtl_rx.h:8-9`) **[verified]**.
- **No tiering at all.**
  - The `st_convert_internal.h` banner says "for internal test usage only" (`include/st_convert_internal.h:5-9`), yet the header is installed and SWIG-wrapped (`python/swig/pymtl.i:21`, `:30`).
  - The experimental header has no opt-in gate (`st20_combined_api.h:6-8`).

  **[verified]**

### 2.2 Include graph

```text
 mtl_build_config.h ◀── mtl_api.h ◀──┬── st_api.h ◀──┬── st20_api.h ◀──┬── st_pipeline_api.h ◀──┬── st30_pipeline_api.h ──▶ st30_api.h
 (generated)                         │               │   (st20+st22)   │                         └── st40_pipeline_api.h ──▶ st40_api.h
                                     │               ├── st30_api.h    ├── experimental/st20_combined_api.h
                                     │               ├── st40_api.h    └── st_convert_internal.h ◀── st_convert_api.h
                                     │               └── st41_api.h                (also includes st30_api.h)
                                     ├── mtl_sch_api.h
                                     └── mtl_lcore_shm_api.h
```

Cited lines **[verified]**:

| Header | Includes | Line |
|---|---|---|
| `st_api.h` | `mtl_api.h` | :12 |
| `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h` | `st_api.h` | :12 |
| `st_pipeline_api.h` | `st20_api.h` | :14 |
| `st30_pipeline_api.h` | `st30_api.h`, `st_pipeline_api.h` | :12-13 |
| `st40_pipeline_api.h` | `st40_api.h`, `st_pipeline_api.h` | :8-9 |
| `st_convert_api.h` | `st_convert_internal.h` | :13 |
| `st_convert_internal.h` | `st20_api.h`, `st30_api.h` | :13-14 |
| `st20_combined_api.h` | `../st20_api.h` | :12 |
| `mtl_sch_api.h`, `mtl_lcore_shm_api.h` | `mtl_api.h` | :12 |

### 2.3 Inventory

Function counts come from `-aux-info` (R02 §2.1) **[verified]**.

| Header | Fn | Exposes (key items) |
|---|---|---|
| `mtl_api.h` | 70 | instance (`mtl_init`/`start`/`stop`/`abort`), `mtl_init_params`, logging hooks (`mtl_set_log_printer` :1086, `mtl_openlog_stream` :1100), lcore borrow (`mtl_get_lcore` :1142, `mtl_bind_to_lcore` :1157), PTP read, hugepage and DMA memory (`mtl_hp_*`, `mtl_dma_map` :1310), user DMA (`mtl_udma_*` :1400-1505), SIMD level, `mtl_memcpy` :1185, `enum st21_tx_pacing_way` :318 |
| `st_api.h` | 12 | `st_fps` :60, `st_frame_status` :78, RFC 3550 header :99, `st_tx_dest_info` :148, `st_rx_source_info` :159, `st_queue_meta` :186, `st10_vsync_meta` :196, `st_event` :208, port and user stats :251-397, media-clock helpers :545-621 |
| `st20_api.h` | 51 | ST20 **and ST22** sessions; flags :35-289; `st20_type` :352 (FRAME/RTP/SLICE), `st20_fmt` :321, pgroup structs :817-1096, `st20_ext_frame` :1105, RTCP ops :1119/:1441, ops :1133/:1286/:1473/:1632, timing-parser meta :475/:515, format helpers `st20_get_pgroup` :2020, `st20_frame_size` :2034 |
| `st30_api.h` | 23 | ST30 session, `st30_fmt`/`sampling`/`ptime` :132-153, AM824/AES3 :193/:236, size helpers :756-832 |
| `st40_api.h` | 27 | ST40 session, test hooks :90-103, RFC 8331 structs :142-214, `st40_meta` :284, UDW/parity/RFC 8331 helpers :816-925 |
| `st41_api.h` | 16 | ST41 session (RX RTP-only, `st41_rx_ops` :232) |
| `st_pipeline_api.h` | 106 | st20p/st22p, `st_frame_fmt` :98, `st_ext_frame` :248, `struct st_frame` :262, plugin ABI (`st_plugin_meta` :77, `st22_encoder_dev` :700, `st22_decoder_dev` :756, `st20_converter_dev` :806, register :1225/:1385/:1440), `st_tx_port`/`st_rx_port` :838/:857, `st_frame_*` helpers :2260-2456, `st_draw_logo` :2365 |
| `st30_pipeline_api.h`, `st40_pipeline_api.h` | 24, 27 | st30p, st40p |
| `mtl_sch_api.h` | 6 | user schedulers and tasklets (`mtl_sch_register_tasklet` :128) |
| `mtl_lcore_shm_api.h` | 2 | SysV-shm lcore table print/clean (:44, :59) |
| `st_convert_api.h` + `st_convert_internal.h` | 47 + 58 | ≈105 colour converters; ≈40 public `static inline` wrappers call `*_simd` from the internal header (`st_convert_api.h:41-45`); `*_simd_dma` take `mtl_udma_handle` |
| `experimental/st20_combined_api.h` | 6 | `st20rc` redundant-combined RX |

### 2.4 Type leakage: why the lower headers cannot simply stop being installed today

| Higher header | Lower-layer types it uses | Evidence |
|---|---|---|
| `st_pipeline_api.h` | `st_api.h`: `st_fps`, `st10_timestamp_fmt`, `st_frame_status`, `st_event`, `st10_vsync_meta`, `st_queue_meta`, `st_pcap_dump_meta`, `st_tx_dest_info`, `st_rx_source_info`. `st20_api.h`: `st21_pacing`, `st20_packing`, `st20_fmt`, `st22_pack_type`, RTCP ops, rx frame metas, detect meta/reply, `st20_rx_tp_meta` (in `st_frame` :321), stats | :288-2176 **[verified]** |
| `st30_pipeline_api.h` | `st30_fmt`/`sampling`/`ptime`/`tx_pacing_way`, `st30_*_user_stats`; `st_tx_port`/`st_rx_port` from `st_pipeline_api.h` (:111, :274), which drags in all of `st20_api.h` | **[verified]** |
| `st40_pipeline_api.h` | `st40_meta` (:24), `st40_tx_test_config` (:155), `st_fps`, `st_queue_meta`; `st_tx_port`/`st_rx_port` (:140, :224) | **[verified]** |
| plugin ABI (in `st_pipeline_api.h`) | `mtl_handle` (`st_plugin_create_fn`, :89), `st_fps`, `st_frame_fmt`, `struct st_frame`, `st_plugin_device`, `st22_quality_mode`; 64-bit caps masks over `st_frame_fmt` | `:85-95`, `:670-830` **[verified]** |
| `st_convert_api.h` | rfc4175 pgroup structs, `st31_am824`/`st31_aes3`, `mtl_simd_level`, and through the internal header `mtl_udma_handle` | `:13`, `:44`, `:1064`; internal `:48` **[verified]** |
| `ecosystem/` pipeline users | GStreamer: `st20_rx_frame_meta` (`gst_mtl_st20p_rx.c:140`), `st30_calculate_framebuff_size` (`gst_mtl_st30p_rx.c:229`), st40 UDW/parity (`gst_mtl_st40p_tx.c:706-707`). FFmpeg: `st30_calculate_framebuff_size` (`mtl_st30p_rx.c:126`), session flag `ST20_RX_FLAG_DMA_OFFLOAD` on st20p ops (`mtl_st20p_rx.c:176`) | **[verified]** |

**Consequence.** A pipeline-only consumer needs `st20_api.h` today, and so does a plugin. "Hide `st20_api.h` but keep the pipelines public" is impossible without first splitting the vocabulary types out, which is pure churn on an API slated for deprecation.

**Recommendation:** do not split the legacy headers. Move the whole legacy set as one tier. The unified set already has its own vocabulary: `mtl_video_format`, `mtl_app_format`, `mtl_rational`, `mtl_buffer_view` (`mtl_unified.h:1197-1300`, `:1077-1095`) **[inferred]**.

### 2.5 ABI hygiene today

| Item | State | Evidence |
|---|---|---|
| Visibility | everything exported. There is no `-fvisibility`, no version script, and no export macro anywhere | `lib/meson.build:85-87` (`link_args` = `-z now -z relro`), `:150-157`; 757 dynamic symbols, about 228 internal `mt_*` (`readelf` on `build/lib/libmtl.so`) **[verified]** |
| soname | plain `libmtl.so`, with no `version:` or `soversion:` | `lib/meson.build:150-157` **[verified]** |
| Deprecation | `__mtl_deprecated_msg()` is applied only to **fields**, never to functions. It is a no-op under `__MTL_PYTHON_BUILD__`, and suppressed in-library by `__MTL_LIB_BUILD__` (`meson.build:30`) | `include/mtl_api.h:141-147`, `:745-759` **[verified]** |
| Windows | the `.def` is generated from every export (`-Wl,--output-def,libmtl.def`) | `lib/meson.build:122-123` **[verified]** |
| `ALLOW_EXPERIMENTAL_API` | the DPDK macro, set for lib only. No MTL header checks it | `lib/meson.build:8` **[verified]** |

### 2.6 How `lib/src` uses the public headers

- `lib/src/st2110/st_header.h:9-15` is the hub. It includes `st20/30/40/41_api.h`, `st_pipeline_api.h`, `st_convert.h` and `st_fmt.h`.
- `mt_main.h:24-25` includes `mtl_sch_api.h`, and `mt_sch.c:15` includes `mtl_lcore_shm_api.h`.
- The include path is `include_directories('.', 'include')` (`meson.build:58`).

The library compiles against the same files it installs **[verified]**. Moving the files therefore needs exactly one include-path change in the library build, and no source edits in `lib/src` **[inferred]**.

## 3. Features reachable only through the session-level headers

Legend for "Unified":
- **COVERED**: a v1 mechanism exists in the sketch.
- **PARTIAL**: a mechanism exists, but there is a parity gap.
- **LATER**: named in the design but scheduled after v1, or declared under `MTL_UNIFIED_LATER` (`mtl_unified.h:3008-3022`).
- **NOT COVERED**: absent from the sketch.
- **S8**: owned by the passthrough design.
- **CUT?**: proposed removal that needs approval.

"H-nn" points to the gap list in §7. Consumer cites come from a full grep of `app/`, `tests/`, `ecosystem/`, `plugins/`, `rust/` and `python/` at `545a266a` **[verified]**. No consumer outside lib uses `manager/`, and `ld_preload/` has no sources (`ld_preload/meson.build` only, sources removed in `2b182cd87`) **[verified]**.

### 3.1 ST 2110-20 video session (`st20_api.h`)

| # | Feature | Legacy surface | In-tree consumers | Unified | Mechanism / gap |
|---|---|---|---|---|---|
| V1 | Frame-level TX by index callback | `get_next_frame`/`notify_frame_done` `:1186-1195` | samples `legacy/tx_video_sample.c:220`; `app/v4l2_to_ip/v4l2_to_ip.c:1963`; RxTxApp `legacy/tx_video_app.c:858`; integration `st20/*` (13 creates); Rust `rust/src/imtl/video.rs:543` | COVERED | `mtl_tx_acquire`/`submit`/`reap` (`mtl_unified.h:2585`, `:2605-2615`); no tasklet code (GO-6) |
| V2 | Frame-level RX by pointer callback + `st20_rx_put_framebuff` | `:1544`, `:2262` | samples, RxTxApp legacy, MXL `poc/src/sender/mxl_bridge.c:499`, Rust `video.rs:858` | COVERED | `mtl_rx_dequeue`/`release`; MXL via `MTL_RX_SLOT_BY_INDEX` (`:1151`, 10 §8) |
| V3 | Tasklet-context latency of V1/V2 (bobi, MXL, fwd chose these callbacks for latency, R08 §5.2) | callback model | MXL POC, fwd samples, external bobi | LATER | `mtl_session_set_inline_notify` (`:3010-3012`, Q-THR-1) → **H-15** |
| V4 | **RTP level** TX/RX (`ST20_TYPE_RTP_LEVEL`, `get_mbuf`/`put_mbuf`, `notify_rtp_ready`, `rtp_ring_size`) | `:356`, `:1932-1948`, `:2279-2290`, `:1258`, `:1619` | samples `low_level/{tx,rx}_rtp_video_sample.c:161/107`; RxTxApp `legacy/tx_video_app.c:287`; integration (61 `RTP_LEVEL` sites) | S8 | `MTL_UNIT_PACKET_CHUNK` (`:410`) → **H-01** |
| V5 | **Slice level** TX (`query_frame_lines_ready`) / RX (`notify_slice_ready`, `slice_lines`) | `:362`, `:1254`, `:1610`, `:1603` | samples `low_level/*slice*`; RxTxApp `legacy/tx_video_app.c:784`, `rx_video_app.c:548`; integration `st20/{digest,uframe,detect}` | LATER | `MTL_UNIT_ROWS` (`:409`), `mtl_tx_publish` (`:2608`), `mtl_rx_wait_progress` (`:2636`); Phase 6 → **H-02** |
| V6 | TX ext frame `st20_tx_set_ext_frame` / `ST20_TX_FLAG_EXT_FRAME` | `:1882`, `:45` | samples ×4, `v4l2_to_ip.c:1457`, integration `st20_common.cpp:148` | COVERED | attached buffers or `mtl_tx_acquire_buffer`; per-acquire layouts via `MTL_POOL_DYNAMIC` (`:1120`, Phase 4) |
| V7 | RX static `ext_frames[]` | `:1550` | integration | COVERED | attached pool (`mtl_session_attach_buffers`) |
| V8 | RX dynamic `query_ext_frame` (app supplies each destination) | `:1599`, needs `RECEIVE_INCOMPLETE_FRAME` | integration `st20/st20_ext_frame.cpp:203`; MXL `poc/src/sender/mtl_rx.c:199`; GStreamer via st20p (`gst_mtl_st20p_rx.c:140`) | NOT COVERED | `mtl_rx_provide` exists only in the LATER block (`:3014`) → **H-09** |
| V9 | RX per-pgroup user callback `uframe_pg_callback` + `uframe_size` | `:1586`, `:1574` | RxTxApp `legacy/rx_video_app.c:593`; integration `st20_uframe.cpp:151`, `st20_detect.cpp:167` | CUT? | app code on the tasklet (R-THR-4; 09 §8) → **CUT-2**. Library-side per-packet conversion (`ST20P_RX_FLAG_PKT_CONVERT`, `st_pipeline_api.h:590`; only `st20p_test.cpp`) stays as an option → **H-16** |
| V10 | Auto-detect (`ST20_RX_FLAG_AUTO_DETECT`, `notify_detected` + `st20_detect_reply`) | `:219`, `:1592` | RxTxApp `legacy/rx_video_app.c:534,550`; integration `st20_detect.cpp:171`; sample `rx_st20p_auto_detect_sample.c:195` | LATER | `MTL_EVENT_RX_FORMAT` (`:2691`) and `format_changed` exist; no video `detect` field (`enum mtl_detect` is ANC-only, `:1288`) → **H-06** |
| V11 | Timing parser STAT (`ST20_RX_FLAG_TIMING_PARSER_STAT`) | `:238` | RxTxApp `legacy/rx_video_app.c:565`; acceptance `tests/single/rx_timing` | COVERED | `MTL_OPT_RX_TIMING_PARSER` (`:1995`) + stats keys |
| V12 | Timing parser META per frame (`st20_rx_tp_meta`, `st20_rx_timing_parser_critical`) | `:243`, `:475`, `:2328` | sample `rx_st20p_timing_parser_sample.c:85`; Python `rx_timing_parser.py:135`; unit `st20_harness.h:147` | NOT COVERED | `struct mtl_rx_unit` (`:2474-2498`) has no ST 2110-21 fields → **H-07** |
| V13 | DMA offload RX (`ST20_RX_FLAG_DMA_OFFLOAD`, `st20_rx_dma_enabled`) | `:212`, `:2314` | RxTxApp, integration ×5, FFmpeg (misused flag, `mtl_st20p_rx.c:176`) | COVERED | `caps.dma_offload` REQUIRE/PREFER (`:1564`), `MTL_PATH_KIND_DIRECT_DMA` (`:1135`), `pkts_dma` |
| V14 | Header split (`ST20_RX_FLAG_HDR_SPLIT`) | `:226` | RxTxApp `legacy/rx_video_app.c:560`; integration `st20_digest.cpp:155`; sample `ext_frame/rx_st20p_hdr_split_gpu_direct.c:311` | CUT? | NG6, Q-MEM-9 (a); unbuildable on the DPDK 26.07 pin (R07 §3.2) → **CUT-1** |
| V15 | GPU VRAM frames (`gpu_direct_framebuffer_in_vram_device_address`; st20p `USE_GPU_DIRECT_FRAMEBUFFERS`) | `:1624`; `st_pipeline_api.h:642` | FFmpeg `mtl_st20p_rx.c:204-220`; `app/sample/gpu_direct/` | LATER | device memory domain (`MTL_MEM_DMABUF` "later", `:993`; NG4) → **H-10** |
| V16 | RTCP retransmission TX/RX (`ENABLE_RTCP`, `st_tx_rtcp_ops`/`st_rx_rtcp_ops`) | `:87`, `:186`, `:1119`, `:1441` | RxTxApp `legacy/{tx,rx}_video_app.c:801/562`; integration `st20_digest.cpp:86,157`; `doc/rtcp.md` | LATER | 09 §3 "L", video/cvideo only → **H-05** |
| V17 | Two RX threads above 40G (`USE_MULTI_THREADS`) | `:249` | RxTxApp | COVERED | `options.rx_threads` (`:1585`) |
| V18 | USER_PACING / EXACT / USER_TIMESTAMP / RTP_TIMESTAMP_EPOCH / `rtp_timestamp_delta_us` | `:56-100` | integration `st20_user_pacing.cpp:75`; `tx_timestamp_sync_test.cpp:227`; sample `fwd/rx_st20_tx_st20_split_fwd.c:271` | COVERED | timing model: media mode INDEX/TAI, `mtl_launch` EXACT, `rtp_mode` PASSTHROUGH + `rtp_override` (`:2555`), `media_time_offset_ns` |
| V19 | VSYNC (`ENABLE_VSYNC`, `ST_EVENT_VSYNC`) | `:75`, `:181`; `st_api.h:208` | RxTxApp `legacy/tx_video_app.c:9,792`; integration `tests.cpp:955` | COVERED | `MTL_EVENT_EPOCH_TICK` (`:2704`) |
| V20 | `notify_frame_late` | `:1204` | integration | COVERED | per-unit `TX_RESULT` with reason and margins (D-14) |
| V21 | `update_destination` / `update_source` | `:1866`, `:2187` | integration `st20/st20_update_source.cpp:146` | COVERED | `mtl_session_update_flows` (`:2264`, D-26) |
| V22 | `get_sch_idx`, `get_pacing_params` | `:1959`, `:1976`, `:2198` | RxTxApp `legacy/tx_video_app.c:139` (only consumer) | COVERED | `mtl_session_info.sched_index` (`:1661`), `vrx_full` (`:1693`), `trs_ps` (`:1702`) |
| V23 | Queue meta + `DATA_PATH_ONLY` | `:2303`, `:176` | integration only (`st20_digest.cpp:212` and two more; `st22_test.cpp:1412`) | CUT? | Q-MODE-5 (verify first); alternative `mtl_open_ext("queue_meta")` (`:3018`) → **CUT-?** in §7.2, or H-17 |
| V24 | pcapng dump (`st20_rx_pcapng_dump`) | `:2216` | RxTxApp `legacy/rx_video_app.c:700` (plus st20p/st22p/st20rc variants); integration `st20/st20_dump.cpp:113` | NOT COVERED | → **H-11** |
| V25 | Stats get/reset | `:1993`, `:2344` | RxTxApp, integration | COVERED | `mtl_session_get_stats` (cumulative, D-20; no reset by design) |
| V26 | Static pad, disable bulk, force NUMA, user MACs, migrate off, burst size, simulate loss | flags `:35-249` | RxTxApp, integration | COVERED | key options `MTL_OPT_VIDEO_*` (`:1996-1999`), `options.numa`, `MTL_FLOW_USER_MAC`, `rx_burst_size`, `mtl_debug_inject` |
| V27 | Linesize, `user_meta` | ops fields | integration `st20_linesize_digest.cpp` | COVERED | plane `stride` (D-54), `submission.user_meta` (`:2562`) |
| V28 | Format helpers `st20_get_pgroup`, `st20_frame_size`, `st20_get_bandwidth_bps`, `st20_fmt_name`/`st20_name_to_fmt` | `:2020-2517` | samples ×6, RxTxApp ×3, integration ×15, fuzz | PARTIAL | `mtl_video_layout_query` (`:1407`), `mtl_video_format_names` (`:1411`); no name→format parse, no bandwidth query → **H-12** |
| V29 | Transport-format set (18 `st20_fmt` values) | `:321-351` | all video users | PARTIAL | `mtl_video_format` has 11 (`:1200-1212`). Missing: YUV420 12/16-bit, RGB 16-bit, YUV444 8/16-bit, and the non-RFC 4175 `V210`/`YUV_422_PLANAR10LE` (Q-MODE-7) → **H-12** |

### 3.2 ST 2110-22 session (also in `st20_api.h`)

| # | Feature | Legacy | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|---|
| C1 | Codestream frame level (`ST22_TYPE_FRAME_LEVEL`, `st22_tx_get_fb_addr`) | `:373`, `:2161` | samples `legacy/{tx,rx}_st22_video_sample.c:196/179`; RxTxApp `legacy/{tx,rx}_st22_app.c`; MXL `poc_8k/…/main.c:355,813`; integration `st22_test.cpp` | COVERED | `mtl_cvideo_session_create` with `app_format = 0` ("app gives codestream", `:1322`), CBR default (D-32) |
| C2 | ST22 RTP level | `ST22_TYPE_RTP_LEVEL`, `:2120-2136`, `:2443-2454` | integration `st22_test.cpp` (×12) | S8 | → **H-01** |
| C3 | `DISABLE_BOXES` (TX/RX) | `:127`, `:266` | **none** (grep, `git grep DISABLE_BOXES` in all consumer trees returns nothing) | CUT? | **CUT-6**; `info.box_hdr_bytes` stays reported |
| C4 | Slice packing (`ST22_PACK_SLICE`, "not support now") | `:386-387` | none | COVERED | `MTL_CVIDEO_PACK_SLICE` reserved, `-MTL_ENOTSUP` (`:1252`) |
| C5 | RTCP, pcapng, queue meta, VSYNC, update dst/src | as V16, V24, V23, V19, V21 | integration `st22_test.cpp:1327,1152,1412,740` | as V16/V24/V23/V19/V21 | H-05, H-11 |

### 3.3 ST 2110-30 audio session (`st30_api.h`)

| # | Feature | Legacy | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|---|
| A1 | Frame-level TX/RX sessions | `:676`, `:847` | RxTxApp `legacy/{tx,rx}_audio_app.c:484/416`; integration `st30_test.cpp:372/563` | COVERED | audio essence (`mtl_audio_config`, `:1335`), `mtl_tx_write` |
| A2 | RTP level | `:729-745`, `:902-913` | RxTxApp `legacy/tx_audio_app.c:141`; integration `st30_test.cpp:72` | S8 | → **H-01** |
| A3 | Per-session pacing way (`st30_tx_pacing_way`), RL warm-up, FIFO | `:178`, ops | RxTxApp | COVERED | `caps.pacing_class`, `MTL_OPT_AUDIO_RL_WARMUP` (`:2000`), `audio_fifo_ms` |
| A4 | RX timing parser STAT/META (200 ms callback) | `:118` and following | RxTxApp `legacy/rx_audio_app.c:380,383` | PARTIAL | STAT through the option and stats; META as H-07 → **H-07** |
| A5 | Size helpers (`st30_get_packet_size`, `st30_calculate_framebuff_size`, sample size/num/rate) | `:756-832` | FFmpeg `mtl_st30p_{rx,tx}.c:126/129`; GStreamer `gst_mtl_st30p_rx.c:229`; Rust `audio.rs:215` | COVERED | `buffer_capacity_bytes` default (10 ms), `mtl_audio_session_query` → `info.unit_bytes` |
| A6 | RTCP flags | ops | RxTxApp `legacy/rx_audio_app.c:379` | CUT? | no-op today (R07 §3.1); Q-MODE-8 (a) → **CUT-7** |
| A7 | AM824/AES3 conversion (`st31_am824_to_aes3`) | `st_convert_api.h:1064` | — | PARTIAL | `MTL_AUDIO_AM824` carries the wire format (`:1335` area); conversion goes to `mtl_convert.h` (§7.1) → **H-13** |

### 3.4 ST 2110-40 ancillary session (`st40_api.h`)

| # | Feature | Legacy | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|---|
| N1 | Frame-level sessions (≤ 20 meta) | `:612`, `:725` | RxTxApp `tx_ancillary_app.c:480`, `rx_ancillary_app.c:159` (session-based in the **main** app, not legacy/); integration `st40_test.cpp:409/586` | COVERED | anc essence; up to 255 packets (`max_packets`) |
| N2 | RTP level | `:695-711`, `:766-777` | RxTxApp `tx_ancillary_app.c:147`, `rx_ancillary_app.c:67`; integration | S8 | → **H-01** |
| N3 | RFC 8331 and UDW helpers (`st40_get_udw`/`set_udw`, parity, checksum, encode/decode) | `:816-925` | GStreamer `gst_mtl_st40p_tx.c:655,706-707`; RxTxApp `rx_ancillary_app.c:17,30`; unit and fuzz | NOT COVERED as public helpers | → **H-13** (pure helpers in `mtl_util` scope) |
| N4 | Test mutation config | `:90-108` | GStreamer test element | COVERED | `mtl_debug_inject(…, MTL_FAULT_TX_MUTATE)` (`mtl_debug.h`) |
| N5 | Split by packet, EXACT pacing, DEDICATE_QUEUE | flags | RxTxApp `tx_ancillary_app.c:475-478` | COVERED | `anc_split_by_packet`, `mtl_launch` EXACT, `options.tx_queue` |
| N6 | RTCP flags | flags | RxTxApp `rx_ancillary_app.c:155` | CUT? | no-op (R07 §3.1) → **CUT-7** |

### 3.5 ST 2110-41 fast-metadata session (`st41_api.h`)

| # | Feature | Legacy | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|---|
| F1 | TX frame sessions | `:377` | RxTxApp `tx_fastmetadata_app.c:475` (only consumer); acceptance `tests/single/st41` | COVERED | fastmeta essence |
| F2 | RX (RTP-only today) | `st41_rx_ops` `:232`, `:501-512` | RxTxApp `rx_fastmetadata_app.c:166,277` | COVERED (frame RX is new) + S8 (raw) | → **H-01** for the raw path |
| F3 | DIT/K filters | ops | RxTxApp | COVERED | `MTL_FLOW_MATCH_FMD_DIT/K` (`:1421-1422`) |
| F4 | RTCP flags | ops | RxTxApp `rx_fastmetadata_app.c:258` | CUT? | **CUT-7** |

### 3.6 Shared vocabulary and helpers (`st_api.h`, `st_pipeline_api.h`)

| # | Item | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|
| S1 | `st_fps`, `st_frame_rate*`, `st_name_to_fps` | FFmpeg `mtl_common.c:36`; plugins | COVERED | `mtl_rational`, `mtl_fps_rational`, `mtl_fps_parse` (`:645-647`) |
| S2 | Media-clock helpers `st10_tai_to_media_clk`, `st10_media_clk_to_tai` | RxTxApp latency code (R08 §1.2) | PARTIAL | CQ records carry `rtp` and TAI; no public ticks↔TAI helper (`mtl_time_convert` has no media clock, `:552-558`) → **H-12** |
| S3 | `struct st_frame`, `st_ext_frame` | every pipeline consumer | COVERED | `mtl_buffer_view` / `mtl_buffer_desc` (`:1065-1095`) |
| S4 | `st_frame_fmt` (≈28 app formats incl. RFC 4175 BE, CUSTOM8, 12/16-bit planar, codestreams) | GStreamer `gst_mtl_st20p_tx.c:370,666`; OBS `mtl-output.c:217`; samples | PARTIAL | `mtl_app_format` has 12 (`:1215-1228`); codestreams via `mtl_codec` → **H-12** |
| S5 | `st_frame_*` helpers (planes, plane size, `st_frame_size`, to/from transport, name) | samples, GStreamer, plugins (`st_frame_fmt_name` ×10, `st_frame_plane_size` ×6) | PARTIAL | `mtl_video_layout_query`, `mtl_video_format_enum`, `mtl_video_format_names` → **H-12** |
| S6 | `st_draw_logo` | samples ×5, integration `st22p_test.cpp:435` | CUT? | sample utility → move into `app/sample` common code → **CUT-8** |
| S7 | `st_name_to_codec` | FFmpeg `mtl_st22p_rx.c:92` | PARTIAL | `enum mtl_codec` exists, but has no parser → **H-12** |
| S8 | `st_txp_*`/`st_rxp_*` and `mtl_para_*` setters (SWIG helpers) | Python | COVERED | `mtl_flow_parse`, `struct mtl_port_spec`; no 2-D arrays (11 §4) |
| S9 | Port and user stats structs, `st_var_info`, queue-count helpers | RxTxApp | COVERED | `mtl_session_get_stats`, `mtl_instance_get_status`, auto queues |
| S10 | RFC 3550 header struct (`st_rfc3550_rtp_hdr`) | RTP-level users | S8 | the packet-chunk header layout → **H-01** |

### 3.7 Core utilities in `mtl_api.h` (legacy core, same tier)

| # | Item | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|
| M1 | Log hooks (`mtl_set_log_printer`, `_prefix_formatter`, `mtl_openlog_stream`) | RxTxApp `args.c`, `rxtx_app.c` | NOT COVERED | only `log_level` exists (`:778`) → **H-14** |
| M2 | Lcore borrow (`mtl_get_lcore`, `mtl_bind_to_lcore`, `mtl_put_lcore`) | `app/perf/*` ×14, RxTxApp, integration `st_test.cpp` | CUT? | perf tools move to the internal tier. Public: **CUT-5b**, or an L4 helper if an external user needs it (Q-MIG-1) |
| M3 | User DMA `mtl_udma_*` | `app/sample/dma/dma_sample.c:24`, `app/perf/*`, integration `dma_test.cpp`, `cvt_test.cpp` | CUT? | **CUT-5**; the engines keep using DMA internally (`dma_offload`) |
| M4 | Hugepage and DMA memory (`mtl_hp_*`, `mtl_dma_map`, `mtl_dma_mem_*`) | samples, fwd | COVERED | `mtl_mem_alloc`, `mtl_mem_import` (`:1045-1047`), regions (D-16) |
| M5 | `mtl_ptp_read_time`, `ptp_get_time_fn`, `ptp_sync_notify` | RxTxApp, FFmpeg | COVERED | `mtl_time_now` (`:609`), `MTL_TIME_SOURCE_USER` + `mtl_time_user_update` (`:582`, `:620`), time status and events |
| M6 | `mtl_sch_enable_sleep`, `mtl_sch_set_sleep_us` | RxTxApp `rxtx_app.c:176`; integration `st20p_test.cpp:929` | PARTIAL | `MTL_INSTANCE_TASKLET_SLEEP` (`:740`); no sleep-interval knob → **H-18** |
| M7 | `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align` | widely (`mtl_memcpy` in ≈60 files) | CUT? | libc equivalents, `mtl_lease_copy_in/out`, `port_caps.page_size` → **CUT-8** |
| M8 | Port and PMD queries, `mtl_is_manager_alive`, `mtl_get_port_stats`, `mtl_abort` | samples, RxTxApp | COVERED | `mtl_port_get_caps`/`status`, `mtl_instance_get_status.manager`, `mtl_instance_interrupt_all` |
| M9 | SIMD level query | perf tools | COVERED | instance stat key (`mtl_instance_stat_get`) |

### 3.8 Scheduler, lcore shm, convert, experimental

| # | Item | Consumers | Unified | Mechanism / gap |
|---|---|---|---|---|
| X1 | User schedulers and tasklets (`mtl_sch_create`, `mtl_sch_register_tasklet`) | integration `sch_test.cpp:16,147` only | LATER | the sketch already defines the inline-safe DP subset for "a user tasklet" (`mtl_unified.h:69-73`), but has no way to register one; `mtl_sch_run_once` is LATER (`:3016`) → **H-08** |
| X2 | `mtl_lcore_shm_print`/`clean` | `app/tools/lcore_shmem_mgr.c:60,63` only | internal | an admin tool, not an app API; build it in-tree against the internal tier (Q-MODE-7 lists the shm allocator as a deprecation candidate) |
| X3 | ≈105 per-pair converters (`_simd`, `_simd_dma`, `_2way_cpuva` variants) | `app/tools/convert_app.c` (≈20), `app/perf/*`, RxTxApp `fmt.c:27`, `cvt_test.cpp` (≈70), `plugins/sample/convert_plugin_sample.c:24`, Python (`st_convert_internal.h:1423`) | PARTIAL | pipelines convert internally (`:1139-1143`); standalone: `mtl/mtl_convert.h` → **H-13**, **CUT-4** |
| X4 | `st20rc` redundant-combined RX | `app/sample/experimental/rx_st20_redundant_combined_sample.c:191`; RxTxApp `experimental/rx_st20r_app.c:341` | CUT? | 2022-7 legs cover it (D-59); Q-MODE-7 → **CUT-3** |

### 3.9 Who actually blocks hiding

| Consumer | Session-header dependency | Path |
|---|---|---|
| FFmpeg plugin | pipeline-only, plus `st30_calculate_framebuff_size`, `st_frame_rate_to_st_fps`, `st_name_to_codec`, and the misused `ST20_RX_FLAG_DMA_OFFLOAD` (`mtl_st20p_rx.c:176`) | Phase 6 rewrite (14 §3.6); fix the flag in Phase 0 (a one-line side fix) |
| GStreamer plugin | pipeline-only, plus `st20_rx_frame_meta`, st40 UDW/parity helpers, `st_frame_fmt_*` | Phase 6 rewrite; needs H-09 (RX provide) and H-13 |
| OBS | `st_pipeline_api.h` only (`linux-mtl/linux-mtl.h:10`) | trivial port (11 §6.1) |
| `plugins/` (st22 sample, avcodec, converter) | implements `st22_encoder_dev`/`decoder_dev`/`st20_converter_dev` from `st_pipeline_api.h` (`plugins/sample/st22_plugin_sample.c:231,251`; `plugins/st22_avcodec/st22_avcodec_plugin.c:519,535`; `plugins/sample/convert_plugin_sample.c:144`) | plugin ABI v2 (§4); the V1 loader stays during the legacy window |
| MXL POC | `st20_rx` + dynamic `query_ext_frame`, `st22` sessions | `MTL_RX_SLOT_BY_INDEX` (10 §8); cvideo |
| Rust `imtl-rs` | bindgen over `wrapper.h:1-6`; session `st20_tx/rx_create` frame mode (`video.rs:543,815`) | rebind on `mtl_unified.h` (11 §4); the safe layer needs a rewrite anyway (R08 §1.2) |
| Python `pymtl` | SWIG over six headers, including the internal convert header (`pymtl.i:16-32`); examples pipeline-only | the Phase 1 `pymtl.unified` wrapper (11 §4); `_2way_cpuva` replaced by `mtl_convert` |
| `app/sample` | `sample_util.h:11-15` pulls every legacy header; session samples in `legacy/`, `low_level/`, `ext_frame/`, `fwd/`, `experimental/`, `dma/` | Q-MIG-2 canonical set on unified; old samples build in-tree against the legacy dep until removal |
| `app/v4l2_to_ip` | `st20_api.h` (`v4l2_to_ip.c:31`), session TX with ext frames | unified video TX with attached buffers |
| `app/tools`, `app/perf` | lcore shm, convert, `*_simd_dma`, lcore borrow, udma | internal dep (they are in-tree tools) |
| RxTxApp | session apps for ANC and fastmeta in the main tree; legacy video/audio/st22; experimental st20rc; JSON kinds `video`, `audio`, `ancillary`, `fastmetadata`, `st22` (`tests/acceptance/mtl_engine/rxtxapp_config.py:20-41`) | ANC and fastmeta onto unified (Phase 2); `video`/`st22` RTP and slice JSON onto packet-chunk/rows (Phase 6); keep the JSON schema (11 §6.1) |
| KahawaiTest, unit, fuzz | 752 session call sites; unit and fuzz compile lib `.c` directly (`tests/unit/session/st20_harness.c:22`, `tests/fuzz/st20/st20_rx_frame_fuzz.c:26`) | **not a blocker**: in-tree internal dep (§6.6); they keep testing the session engine that the unified adapters use |
| `manager/`, `ld_preload/` | none (`manager/` includes no MTL public header; `ld_preload/` has no sources) | — |

### 3.10 Tally

The 74 rows of §3.1–§3.8: V1–V29, C1–C5, A1–A7, N1–N6, F1–F4, S1–S10, M1–M9, X1–X4. Each row is counted once, under the status in its "Unified" column. C5 bundles several V-row features with different statuses, so it counts as PARTIAL. F2 counts as COVERED; its raw path is part of the S8 work item.

| Status | Rows | Count |
|---|---|---|
| COVERED | V1, V2, V6, V7, V11, V13, V17–V22, V25–V27; C1, C4; A1, A3, A5; N1, N4, N5; F1–F3; S1, S3, S8, S9; M4, M5, M8, M9 | 34 |
| PARTIAL | V28, V29; C5; A4, A7; S2, S4, S5, S7; M6; X3 | 11 |
| LATER | V3, V5, V10, V15, V16; X1 | 6 |
| NOT COVERED | V8, V12, V24; N3; M1 | 5 |
| CUT? | V9, V14, V23; C3; A6, N6, F4; S6; M2, M3, M7; X4 (10 distinct cuts, §7.2) | 12 |
| S8 | V4, C2, A2, N2, S10 (one work item, H-01) | 5 |
| internal only | X2 (in-tree admin tool, built against the internal tier) | 1 |

## 4. Plugin interfaces

### 4.1 Today

- **Loading.** `.so` plugins are loaded at `mtl_init` from the `plugins` array of the JSON config (`lib/src/mt_config.c:45-56`, `KAHAWAI_CFG_PATH`). The library `dlopen`s the plugin and resolves `st_plugin_get_meta`, `st_plugin_create(mtl_handle)` and `st_plugin_free` (`include/st_pipeline_api.h:85-95`).
- **What a plugin does.** The plugin calls `st22_encoder_register` (or the decoder/converter variant) with a device struct. Each struct holds 64-bit caps masks over `enum st_frame_fmt`, plus `create_session`, `notify_frame_available` and `free_session` (`:700-830`).
- **The work loop.** A plugin thread pulls work with `st22_encoder_get_frame` and `put_frame`, blocking when the plugin sets `ST22_*_RESP_FLAG_BLOCK_GET` (`plugins/sample/st22_plugin_sample.c:39-116`).
- **Versioning.** `st_plugin_meta{version, magic}` is the only versioned interface, at V1 only. The magic macro shifts by 16 where 8 was meant (`st_pipeline_api.h:71`).

All of this is **[verified]**.

**Problems if `st_pipeline_api.h` stops being public:**
1. Third-party codecs cannot build: the device structs, `struct st_frame` and `mtl_handle` all live in legacy headers.
2. The caps masks cap the format space at 64 values.
3. `notify_frame_available` is called from the tasklet, which conflicts with GO-6.
4. Plugins link libmtl symbols (`st22_encoder_get_frame` etc.), so the plugin ABI is the whole libmtl ABI.

These are **[inferred]** from the verified structure.

### 4.2 Proposal: `mtl/mtl_plugin.h`, ABI v2 (stable, minimal, self-contained)

Principles:

| Principle | Prior art |
|---|---|
| The host passes a function table to the plugin, so a plugin `.so` never links libmtl | OpenSSL 3 providers receive a core dispatch table and "cannot be accessed using the Low Level APIs" **[external, fetched docs.openssl.org/3.0/man7/migration_guide]** |
| One versioned entry point with version negotiation | Vulkan loader–ICD negotiation **[external]** |
| `struct_size` on every struct, the C3/C5/C6 rules of `mtl_unified.h` | 11 §2.1 |
| Formats as an explicit pair list (`mtl_format_pair`-like), not a bitmask | — |
| No tasklet callbacks: plugin threads wait in `host->get_work(…, timeout)` | GO-6 |

```c
/* include/mtl/mtl_plugin.h - sketch; must pass sketch/check.sh rules when written */
#include <stddef.h>
#include <stdint.h>
#define MTL_PLUGIN_ABI_VERSION 2u
enum mtl_plugin_kind { MTL_PLUGIN_ENCODER = 1, MTL_PLUGIN_DECODER = 2, MTL_PLUGIN_CONVERTER = 3 };
struct mtl_plugin_plane { void* addr; uint64_t iova; uint64_t bytes; uint32_t stride; uint32_t reserved; };
struct mtl_plugin_frame {            /* lent by the host between get_work and put_work */
  uint32_t struct_size, format;      /* enum mtl_app_format / mtl_video_format, or MTL_PLUGIN_FMT_CODESTREAM(codec) */
  uint32_t width, height, plane_count, second_field;
  uint64_t data_bytes;               /* codestream: valid bytes (in) / produced bytes (out) */
  int64_t media_tai_ns;              /* carried through unchanged */
  struct mtl_plugin_plane plane[4];
  uint64_t reserved[4];
};
struct mtl_plugin_session_req {
  uint32_t struct_size, kind, in_format, out_format, width, height, fps_num, fps_den;
  uint32_t interlaced, frame_count, codec_threads, quality, numa, reserved0;
  uint64_t codestream_bytes;         /* encoder: CBR target per unit (D-32) */
  uint64_t reserved[4];
};
struct mtl_plugin_host {             /* valid from create_session until free_session returns */
  uint32_t struct_size, abi_version;
  int (*get_work)(void* host_session, struct mtl_plugin_frame** in, struct mtl_plugin_frame** out, int64_t timeout_ns);
  int (*put_work)(void* host_session, struct mtl_plugin_frame* in, struct mtl_plugin_frame* out, int result);
  void (*log)(void* host_session, uint32_t level, const char* msg);
  uint64_t reserved[8];
};
struct mtl_plugin_format_pair { uint32_t in_format, out_format; };
struct mtl_plugin_device {
  uint32_t struct_size, kind;
  const char* name;
  uint32_t target_device, pair_count; /* enum mtl_plugin_device (mtl_unified.h) */
  const struct mtl_plugin_format_pair* pairs;
  void* dev_priv;
  int (*create_session)(void* dev_priv, const struct mtl_plugin_session_req* req,
                        const struct mtl_plugin_host* host, void* host_session, void** session_priv);
  int (*free_session)(void* dev_priv, void* session_priv);
  uint64_t reserved[4];
};
/* The only symbol a .so plugin exports; returns <0 if it cannot serve host_abi. */
#define MTL_PLUGIN_ENTRY_SYMBOL "mtl_plugin_entry_v2"
typedef int (*mtl_plugin_entry_fn)(uint32_t host_abi, uint32_t* plugin_abi,
                                   const struct mtl_plugin_device* const** devs, uint32_t* count);
```

In-process registration, used by tests and by apps that embed a codec (today `st22_encoder_register` at `tests/integration_tests/st22p_test.cpp:356,372`), goes into `mtl_unified.h`, because it needs `mtl_instance_h`:
- `mtl_plugin_register(mt, const struct mtl_plugin_device*, mtl_plugin_h*)`;
- `mtl_plugin_unregister`.

`.so` loading moves from the JSON config to an instance parameter or an explicit `mtl_plugin_load(mt, path)`. Q-MODE-4 already proposes "freeze the plugin ABI for v1; redesign later". S9 refines that: freeze **V1** as a legacy interface, and ship **V2** before the hide.

**Compatibility.**
- The loader accepts V1 (via `ST_PLUGIN_GET_META_API`) and V2 (via `MTL_PLUGIN_ENTRY_SYMBOL`) until F+2.
- A V1 plugin needs the legacy headers to *build*, but not to *run*.
- At F+2 the V1 loader is removed together with `MTL_LEGACY_PIPELINE`.
- The in-tree plugins port to V2 in Phase 2: about 1.3 kLOC (R08 §1.1), mostly mechanical, because the work loop keeps its shape.

**[inferred]**

## 5. Prior art: one public high-level surface, internal lower layers

| Project | Mechanism | What MTL takes |
|---|---|---|
| DPDK | `__rte_experimental` = `deprecated(…)` unless `ALLOW_EXPERIMENTAL_API`; `__rte_internal` = `error("Symbol is not public ABI")` unless `ALLOW_INTERNAL_API`; driver headers installed only with `-Denable_driver_sdk=true` | the error guard for `MTL_INTERNAL`; headers behind a build option for the legacy tier |
| GStreamer | unstable -bad libraries emit `#warning "… is unstable API and may change in future."` unless `GST_USE_UNSTABLE_API` | the warn-first opt-in macro for stage 1 |
| FFmpeg | only listed headers are installed; the codec struct `FFCodec` is in an uninstalled header; `FF_API_*` ties removal to the next major; `attribute_deprecated` | removal tied to a soname major. Counter-example: no external codec ABI, so out-of-tree codecs patch the tree (as MTL's FFmpeg plugin does) |
| OpenSSL 3.0 | low-level algorithm APIs deprecated for EVP; `OSSL_DEPRECATEDIN_3_0` on prototypes; `OPENSSL_API_COMPAT`/`OPENSSL_NO_DEPRECATED` hide them; engines replaced by providers | closest analogue: high-level API and redesigned plugin interface at once; a compat macro that hides deprecated declarations |
| libfabric | the app sees `rdma/fabric.h` + `fi_*.h`; provider internals (`ofi_*.h`) are not installed; extensions via `fi_open_ops(fid, name, …)` and `fi_ext*.h` | named extension tables for niche knobs (`mtl_open_ext`, in the LATER block) |
| Vulkan | one core header plus extensions; provisional ones only with `VK_ENABLE_BETA_EXTENSIONS`; `pNext` chains | the `next` chain (D-33) for groups such as RTCP; a beta gate has the shape of `MTL_LEGACY_API` |
| Linux | `include/uapi/` is the only exported header set (`make headers_install`) | physical separation: what is not in the public directory cannot be included |

Evidence:
- **DPDK** **[external, verified locally]**: `/usr/local/include/rte_compat.h:9-53`; DPDK 26.07 source `meson_options.txt:25-26` ("Install headers to build drivers") and `lib/meson.build:128,210`.
- **GStreamer** **[external, verified locally]**: an installed -bad header, `gst/basecamerabinsrc/gstbasecamerasrc.h:26-29`.
- **FFmpeg** **[external, verified locally]**: in the local tree `ecosystem/ffmpeg_plugin/FFmpeg-release-7.0/` (untracked), `libavcodec/Makefile:4` (`HEADERS = …`), `libavcodec/codec_internal.h:127` (`FFCodec`), `libavcodec/version_major.h:31-43` (e.g. `FF_API_INIT_PACKET (LIBAVCODEC_VERSION_MAJOR < 62)`), `libavutil/attributes.h:100`.
- **OpenSSL** **[external, verified]**: `/usr/include/openssl/macros.h:158-187`, `/usr/include/openssl/aes.h:49`; the 3.0 migration guide (fetched): "All such low level APIs have been deprecated"; provider algorithms "cannot be accessed using the Low Level APIs".
- **libfabric**: `fi_open_ops` **[verified]** in `research/09-libfabric.md:81`; the header split is **[external]**.
- **Vulkan, Linux** **[external]**; `pNext` in R11 §5.1.

**Lessons.**
1. Separate *physically*: an uninstalled header is the only hiding that cannot be defeated with `-D`.
2. Stage with a *visible* opt-in first (GStreamer, DPDK), then a compile error (DPDK internal), then removal tied to a major (FFmpeg).
3. Tie symbols to version nodes, so a binary records which tier it uses and the loader fails cleanly (DPDK).
4. Redesign the plugin ABI together with the high-level API, so plugins never depend on the low level (OpenSSL providers).

**[inferred]**

## 6. Proposed hiding mechanism

### 6.1 Three tiers

| Tier | Headers | Installed | Symbols (version node) | Who may use it |
|---|---|---|---|---|
| **Public** | `mtl/mtl_unified.h`, `mtl/mtl_simple.h`, `mtl/mtl_debug.h`, `mtl/mtl_plugin.h`, `mtl/mtl_convert.h` (in `mtl/experimental/` until the freeze, D-34) | always; pkg-config `mtl-unified` (then `mtl`) | `MTL_UNIFIED_EXPERIMENTAL`, then `MTL_1.0` (D-23) | everyone |
| **Legacy** | today's 14 headers + `mtl_build_config.h`, moved unchanged to `mtl/legacy/` | until F+2 (`-Dlegacy_headers`); pkg-config `mtl-legacy` | `MTL_LEGACY_CORE` (`mtl_api.h`, `mtl_sch_*`, `mtl_lcore_shm_*`); `MTL_LEGACY_PIPELINE` (`st2xp_*`, `st_frame_*`, plugin V1); `MTL_LEGACY_SESSION` (`st20_`…`st41_`, `st20rc_`, `st10_*`, per-pair convert) | migrating apps; in-tree legacy code |
| **Internal** | `lib/include/mtl_internal/*.h`: the session declarations once their legacy header is withdrawn, plus the slot interface (02 §2.2) | never | `MTL_INTERNAL`, guarded by `__mtl_internal` (DPDK `__rte_internal` pattern) | `libmtl_unified`, in-tree tests and tools |

Why three nodes for legacy and not one:
- **Different exits.** Pipelines are what every ecosystem consumer uses (R08 §0.1); Phase 6 re-bases them on L2 (Q-ABI-4). They may need a later removal date than the session layer.
- **Audit.** `readelf -V app` shows which nodes a binary needs, so the private-user survey (Q-MIG-1) can be answered with binaries, not questionnaires.

**[inferred]**

### 6.2 Physical layout and the zero-churn move

```text
include/
  mtl/experimental/mtl_unified.h, mtl_simple.h, mtl_debug.h, mtl_plugin.h, mtl_convert.h   (public)
  mtl/legacy/mtl_api.h, st_api.h, st20_api.h, …, st_convert_internal.h, mtl_legacy_gate.h  (legacy, files unchanged)
  mtl/legacy/experimental/st20_combined_api.h
  mtl/st20_api.h, …            forwarding stubs: #include "legacy/st20_api.h"  (F-1 .. F only)
lib/include/mtl_internal/      session declarations after F+2; slot interface (never installed)
```

- During the transition, `mtl.pc` emits `-I${includedir} -I${includedir}/mtl -I${includedir}/mtl/legacy`. The bare form `<st20_api.h>` resolves through the legacy directory, and the `<mtl/st20_api.h>` form through the forwarding stub. **No consumer source changes at the move** **[inferred]** from §2.1.
- `mtl-legacy.pc` emits the legacy `-I` and `-DMTL_LEGACY_API=1` and `Requires: mtl`, so a consumer opts in once in its build file.
- **Do not split the legacy headers** to make the pipeline tier stand alone (§2.4); they move and retire as a set. The only edit inside them is the gate include and the deprecation macro on prototypes.

### 6.3 The gate

```c
/* include/mtl/legacy/mtl_legacy_gate.h, first include of every legacy header */
#include "mtl_build_config.h" /* defines MTL_LEGACY_STAGE for this release: 0, 1 or 2 */
#if !defined(__MTL_LIB_BUILD__) && !defined(MTL_LEGACY_API)
#if MTL_LEGACY_STAGE >= 2
#error "Legacy MTL API: use <mtl/mtl_unified.h>, or build with pkg-config mtl-legacy (MTL_LEGACY_API)"
#elif MTL_LEGACY_STAGE == 1
#warning "Legacy MTL API is deprecated: see doc/unified-api migration table; define MTL_LEGACY_API to silence"
#endif
#endif
#if MTL_LEGACY_STAGE >= 1 && !defined(__MTL_LIB_BUILD__) && !defined(__MTL_PYTHON_BUILD__) && \
    !defined(MTL_LEGACY_NO_DEPRECATION_WARNINGS)
#define MTL_LEGACY_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define MTL_LEGACY_DEPRECATED(msg)
#endif
```

- **Every legacy prototype** gets `MTL_LEGACY_API_FN` = `MTL_LEGACY_DEPRECATED("…") MTL_API`. `MTL_API` is the visibility export macro of 11 §2.6, which Phase 0 adds anyway. Applying it to about 370 prototypes is a scripted, reviewable diff.
- **MSVC** has no `#warning`; use `#pragma message`. **[external]**
- **Python and Rust.** `__MTL_PYTHON_BUILD__` already disables deprecation attributes (`include/mtl_api.h:141-147`), and bindgen ignores them. Both bindings move to the unified header in Phase 1–2 (11 §4).
- **Internal guard.** `__mtl_internal` uses `__attribute__((error("MTL internal API")))` unless `MTL_ALLOW_INTERNAL_API`, exactly as `rte_compat.h:35-53`. The in-tree internal dependency defines it.

### 6.4 Symbols, visibility and soname

- **Phase 0** (Q-ABI-1, 14 §1.3) already adds `-fvisibility=hidden`, a version script and a soname. S9 asks that the script be written **with the four nodes from day one**, plus `local: *`. All current public symbols get `MTL_LEGACY_*` nodes, and `mt_*`/`tv_*`/`rv_*` become local, except the few `libmtl_unified` needs under `MTL_INTERNAL`.
- Binaries built before Phase 0 have unversioned references, which bind to the default definition. Consumers linked after Phase 0 record the node they use. **[external: GNU symbol versioning]**
- **At F+2:** `MTL_LEGACY_SESSION` symbols move to `MTL_INTERNAL`, and their headers move to `lib/include/mtl_internal/`. Removing a node that consumers reference is an incompatible change, so **libmtl's soname is bumped** at F+2 (FFmpeg's FF_API rule). A binary that needs the removed node fails at load with "version `MTL_LEGACY_SESSION` not found", not at a lazy call. **[inferred]**
- **Concrete answer for D-23's freeze-time question.** Keep the unified API in its own `libmtl_unified.so.1` at the freeze rather than merging it into libmtl. libmtl becomes the engine DSO (legacy nodes plus `MTL_INTERNAL`), so the legacy removal bumps *libmtl's* soname without touching the ABI unified applications link against. **[inferred]**
  - After F+2, libmtl exports only `MTL_INTERNAL` and could be renamed `libmtl_engine` or folded into `libmtl_unified`.
  - Cost: one more DSO for applications using both APIs in one process, which they already have pre-freeze. Feeds M8.
- **Windows.** The `.def` is generated from exports (`lib/meson.build:122-123`), so hidden visibility shrinks it automatically. Legacy symbols stay exported until F+2.

### 6.5 Build options and packaging

| Item | Proposal |
|---|---|
| meson option | `legacy_headers` (bool): default `true` up to F+1, `false` from F+2; `legacy_stage` (0/1/2) baked into `mtl_build_config.h` per release |
| pkg-config | `mtl-unified.pc` (public; becomes `mtl.pc` at F+1); `mtl-legacy.pc` (Requires the engine library, adds the legacy `-I` and `-DMTL_LEGACY_API=1`) |
| Distro packages (advice) | `libmtl-dev` = public headers; `libmtl-legacy-dev` = legacy headers until F+2 (DPDK driver-SDK precedent) |
| CI | (1) `sketch/check.sh` (later the unified doc test) compiles the public headers with **only** the public include directory, so leakage cannot creep in. (2) A legacy-coverage check: every `MTL_LEGACY_*` symbol is mapped in the migration table (10 §12) or is an approved CUT (§7.2); F cannot be tagged otherwise. (3) A stage-2 build of every in-tree consumer *without* `MTL_LEGACY_API` |

### 6.6 How the library and in-tree code keep using the session layer

- **`lib/src`.** It includes the files by name through `include_directories('.', 'include')` (`meson.build:58`). The library build adds `include/mtl/legacy` (later `lib/include/mtl_internal`) and keeps the global `__MTL_LIB_BUILD__` (`meson.build:30`). No `lib/src` source changes. Pipelines keep calling `st20_tx_create` (`st20_pipeline_tx.c:482`), intra-DSO after F+2. **[inferred]**
- **`libmtl_unified`.** It calls the slot interface and, for S8's packet-chunk and the rows adapter, the session entry points, all through `MTL_INTERNAL`.
- **In-tree consumers.** KahawaiTest, unit, fuzz, RxTxApp legacy mode, `app/perf`, `app/tools` and the legacy samples depend on `mtl_legacy_dep` (later `mtl_internal_dep`): a meson `declare_dependency` with the include directory and `-DMTL_LEGACY_API` / `-DMTL_ALLOW_INTERNAL_API`. They never need installed legacy headers.
  - Unit and fuzz already include lib `.c` files and add `../../lib/src` (`tests/unit/meson.build:20-21`, `tests/fuzz/meson.build:43-44`) **[verified]**.
- **KahawaiTest stays valuable** as the L0 regression net for the engine the unified adapters run on. It is reclassified as an internal test, not deleted (11 §6.1 already keeps it for legacy).

### 6.7 Rejected alternatives

| Alternative | Why not |
|---|---|
| `#ifdef MTL_INTERNAL` guards inside installed headers, without moving them | anyone can `-D` it; the declarations stay installed, so the ABI promise does not shrink (Lesson 1) |
| Splitting `st20_api.h` into vocabulary + session headers, so the pipelines stay public without it | churn on a surface slated for deprecation; the unified header already has its own vocabulary (§2.4) |
| A separate compat DSO `libmtl_legacy.so` exporting the legacy symbols | the engine calls the same functions internally (`st20_pipeline_tx.c:482`), so they would need renaming throughout `lib/src` to avoid symbol clashes; version nodes give the same audit and removal properties without the rename |
| Shimming session callbacks, slice and RTP modes over the unified API | needs a thread hop and changes latency (R08 §5.2, 11 §5); S8 and the rows mode provide the *capability* natively instead |

## 7. What must be added before the hide

### 7.1 Gaps (pre-hide gate)

Every row must be done, or turned into an approved CUT, before stage 1 (release F). Phases refer to 14. Sizes: S < 0.5 EM, M 0.5–2 EM, L > 2 EM (C5 yardstick, 02 §2.3); these are guesses to be costed by spikes. **[inferred]**

| ID | Gap | Proposed unified mechanism | Where it lives (D-46 rule) | Phase | Size |
|---|---|---|---|---|---|
| H-01 | RTP level, all essences, TX and RX (V4, C2, A2, N2, F2, S10); ST 2022-6 route | packet-chunk unit (`MTL_UNIT_PACKET_CHUNK`, `:410`): **S8** | S8 | Phase 6 → pulled before F | L |
| H-02 | Slice / progressive rows (V5) | `MTL_UNIT_ROWS`, `mtl_tx_publish`, `mtl_rx_wait_progress` (already in the 0.1 shape); session-layer adapter (02 §2.2) | core | Phase 6 (planned) | M–L |
| H-03 | Format parity (V29, S4) | complete `mtl_video_format` (+7, incl. the two non-RFC 4175 layouts or a CUT under Q-MODE-7) and `mtl_app_format` (+≈16: 12/16-bit planar, RFC 4175 BE app layouts, CUSTOM8, ARGB) | `mtl_unified.h` §10 | Phase 1 (enum values only) | S |
| H-04 | Format helpers (V28, S5, S7) | `mtl_video_format_parse(name, &fmt)`, `mtl_codec_parse`, `mtl_video_bandwidth(cfg, &bps)` (pure, AS); the `fourcc`/gst/av names already exist (`:1411`) | `mtl_unified.h` L4 | Phase 1 | S |
| H-05 | RTCP retransmission, video and cvideo (V16, C5) | `next` extension block `struct mtl_rtcp_config {struct_size, kind = RTCP, buffer_pkts, nack_interval_us, seq_skip_window, …}` (Vulkan-style typed chain; D-33 lifts "must be NULL" for this kind); stats keys `MTL_STAT_RTCP_*` | `next` block | Phase 2 | M |
| H-06 | Video RX auto-detect (V10) | `mtl_video_config.detect` (`enum mtl_detect` extended to video). Create with width/height 0 and `detect = ON`; the session raises `MTL_EVENT_RX_FORMAT` with a state getter for the detected config, and delivers nothing until `mtl_session_reconfigure` (D-58) or `MTL_DETECT_ACCEPT` auto-applies it | media config + EQ | Phase 2 | M |
| H-07 | Per-unit ST 2110-21 timing results (V12, A4) and pass thresholds | optional CQ record `MTL_CQE_ENABLE_RX_TIMING` → `struct mtl_rx_tp_result` (cinst max/avg, vrx min/max, fpt, latency, rtp offset, compliance class) following the unit record; thresholds in `mtl_session_info` | completion config + CQ kind | Phase 2 | M |
| H-08 | User tasklets on MTL schedulers (X1) | `mtl_sched_attach_tasklet(mt, sched, const struct mtl_tasklet_desc*, &h)` / `detach`: a desc with `poll(void* user)` returning >0/0, run under the C10 inline-safe DP subset, plus `mtl_sch_run_once` (Q-THR-3); or CUT if the maintainer prefers "no app code on tasklets" absolutely (D-04) | `mtl_unified.h` advanced section | Phase 2 | M |
| H-09 | RX dynamic destination per unit (V8) | `mtl_rx_provide(s, buffer)` (now LATER, `:3014`), the RX twin of `MTL_POOL_DYNAMIC` | core | Phase 4 (with D-56) | M |
| H-10 | GPU VRAM destinations (V15) | device-memory domain (`MTL_MEM_DMABUF`, `device_handle`, 05 §10); or CUT the VRAM-address mode if Q-MEM-8 keeps NG4 | memory | Phase 4–6 | M–L |
| H-11 | pcapng capture (V24, C5) | `mtl_session_capture(s, const struct mtl_capture_params* {struct_size, path, max_pkts, legs_mask, flags SYNC})`; runs on a worker, `MTL_EVENT_CAPTURE_DONE`; available in release builds (an operator feature, not debug) | `mtl_unified.h` | Phase 2 | S |
| H-12 | Media-clock helpers (S2) | `mtl_media_ticks_from_tai(tai_ns, rate, &ticks)` and `mtl_tai_from_media_ticks(anchor_tai, ticks, rate, &tai)` (exact, D-11); or `MTL_CLOCK_MEDIA` in `mtl_time_convert` | L4 | Phase 1 | S |
| H-13 | Standalone conversion and essence utilities (X3, A7, N3) | `mtl/mtl_convert.h`: `mtl_convert(const struct mtl_convert_desc*)`, `mtl_convert_supported()`, AM824↔AES3; ANC helpers `mtl_anc_udw_get/set`, `_parity_add/check`, `_checksum`, `_rfc8331_encode/decode` (pure, AS) in L4; `libmtl_convert` DSO optional (Q-MODE-4) | new public header | Phase 2 | M |
| H-14 | Log sinks (M1) | `mtl_log_set_sink(fn(level, const char* msg, void* user), user)` (CP, process-wide, called off tasklets through the existing log ring) and `mtl_instance_params.log_prefix` | `mtl_unified.h` | Phase 1 | S |
| H-15 | Latency parity for former tasklet-callback users (V3) | `mtl_session_set_inline_notify` (Q-THR-1), moved out of the LATER block, with the DP contract of C10 | core | Phase 3 | S–M |
| H-16 | Library-internal per-packet RX conversion (V9's `PKT_CONVERT`) | `MTL_OPT_RX_CONVERT_PER_PACKET` key option, reported as `convert_context = TASKLET`; library code only, so R-THR-4 is not violated | option key | Phase 2 | S |
| H-17 | Queue meta / app-managed flows (V23), only if CUT-? is rejected | `mtl_open_ext(mt, "queue_meta", 1, &ops)` (`:3018`, Q-ABI-3 (c)) after Q-MODE-5's verification test | extension table | Phase 5 | S |
| H-18 | Scheduler sleep interval (M6) | `mtl_instance_params.sched_sleep_us` (0 = default) | instance params | Phase 1 | S |
| H-19 | Plugin ABI v2 (§4) | `mtl/mtl_plugin.h`, `mtl_plugin_register`/`load`; V1 loader kept until F+2 | new public header | Phase 2 | M |

### 7.2 Cuts needing approval

| ID | Cut | Evidence | Replacement |
|---|---|---|---|
| CUT-1 | Header split (`ST20_RX_FLAG_HDR_SPLIT`, `ST20P_RX_FLAG_HDR_SPLIT`) | NG6, Q-MEM-9 (a); needs a patched DPDK, and there is no `patches/dpdk/26.07/hdr_split/` (R07 §3.2); consumers are a test flag, one integration case and one sample (§3.1 V14) | none; GPU and zero-copy RX via regions and H-10 |
| CUT-2 | App per-pgroup RX callback (`uframe_pg_callback`, `uframe_size`) | app code on the tasklet (R-THR-4, 09 §8); consumers: one RxTxApp option and two integration files | H-16 for the library-side benefit; user formats via a converter plugin (H-19) |
| CUT-3 | `st20rc` redundant-combined RX + `experimental/st20_combined_api.h` | Q-MODE-7 deprecation candidate; consumers: one sample and one RxTxApp experimental app; 2022-7 legs (D-59) cover the function | two-leg video RX |
| CUT-4 | Public per-pair converters (≈105, incl. `_simd`, `_simd_dma`, `_2way_cpuva`) | the internal header says "internal test usage only" (`st_convert_internal.h:5-9`); the public set is mostly `static inline` wrappers (`st_convert_api.h:41-45`) | `mtl_convert()` (H-13); per-pair functions stay `MTL_INTERNAL` for `cvt_test.cpp` and `app/perf` |
| CUT-5 | Public user DMA (`mtl_udma_*`) and the `*_simd_dma` converters | one sample (`app/sample/dma/dma_sample.c:24`), perf tools, two integration files | engines keep DMA internally (`dma_offload`); perf and tests via `MTL_INTERNAL` |
| CUT-5b | Public lcore borrow (`mtl_get_lcore`/`bind_to_lcore`/`put_lcore`) | consumers are perf tools, RxTxApp and one integration case (§3.7 M2) | internal for in-tree tools; revisit if Q-MIG-1 finds external users |
| CUT-6 | `ST22_TX_FLAG_DISABLE_BOXES` / `ST22_RX_FLAG_DISABLE_BOXES` | no consumer in any tree (grep) | none |
| CUT-7 | RTCP flags on ST30/ST40/ST41 | no-ops in `lib/` (R07 §3.1); Q-MODE-8 (a) | none (H-05 covers video and cvideo) |
| CUT-8 | `st_draw_logo`, `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align` | thin wrappers or sample utilities | sample-local code, libc, `mtl_lease_copy_in/out`, `mtl_port_caps.page_size` |
| CUT-? | Queue meta + `DATA_PATH_ONLY` (V23) | integration-only consumers; Q-MODE-5 suspects a NULL-flow dereference on non-socket backends | H-17 if rejected |

## 8. Migration plan

### 8.1 Stages mapped onto 14's phases

| When | Header and symbol actions | Consumer actions | Gate to pass |
|---|---|---|---|
| **Phase 0** | version script (`MTL_LEGACY_*`, `MTL_INTERNAL`, `local: *`); `-fvisibility=hidden` + `MTL_API`; soname; inert gate and `MTL_LEGACY_DEPRECATED`; convert `static inline` wrappers exported, so `st_convert_internal.h` leaves the installed set | FFmpeg flag fix (`mtl_st20p_rx.c:176` → `ST20P_RX_FLAG_DMA_OFFLOAD`) | legacy KahawaiTest + acceptance pass (D-24); no `mt_*` in `nm -D` |
| **Phase 0.5, engines track** | none | none | — |
| **Phase 1** | unified headers installed (`mtl/experimental/`); legacy files moved to `mtl/legacy/` (forwarding stubs, extra `-I` in `mtl.pc`); `mtl-legacy.pc`; in-tree `mtl_legacy_dep`; H-03, H-04, H-12, H-14, H-18 | new samples (Q-MIG-2) and the Python wrapper on unified only | installed-header consumers build with **no source change** (CI builds ecosystem plugins against the install) |
| **Phase 2** | `mtl_plugin.h` (H-19) and V2 loader; `mtl_convert.h` (H-13); H-05, H-06, H-07, H-08, H-11, H-16 | `plugins/` → V2; RxTxApp ANC and fastmeta apps → unified; Rust rebind | re-base go/no-go (14 §3.2) also decides `MTL_LEGACY_PIPELINE`'s exit date |
| **Phases 3–5** | H-15 (Phase 3); H-09, H-10 (Phase 4); H-17 if kept (Phase 5) | MXL POC and `v4l2_to_ip` → unified | — |
| **Phase 6** | H-01 (S8), H-02; legacy pipeline functions re-based on L2 | FFmpeg, GStreamer, OBS rewrites (14 §3.6); RxTxApp `video`/`st22`/`audio` JSON kinds onto unified essences; acceptance suite runs on unified RxTxApp | legacy-coverage check green (§6.5); every CUT approved or reversed |
| **Release F** (ABI freeze) | `MTL_LEGACY_STAGE 1`: deprecation attributes on, `#warning` without `MTL_LEGACY_API`; unified to `MTL_1.0` in `libmtl_unified.so.1` (§6.4) | out-of-tree users get warnings and the migration table | release notes list every legacy symbol → replacement |
| **F+1** | `MTL_LEGACY_STAGE 2`: `#error` without `MTL_LEGACY_API`; forwarding stubs removed; `mtl.pc` = public only; legacy only via `mtl-legacy.pc` | — | — |
| **≥ F+2** (14 §7) | `-Dlegacy_headers=false`; session headers → `lib/include/mtl_internal/`; `MTL_LEGACY_SESSION` → `MTL_INTERNAL`; V1 plugin loader removed; libmtl soname bump; pipeline headers on the same date or a later one (Phase 2 go/no-go) | in-tree legacy users already on `mtl_internal_dep` | maintainer sign-off (14 §7); no unmapped symbol |

### 8.2 ABI and soname implications

| Item | Implication |
|---|---|
| Phase 0 soname | every consumer relinks once (already planned, D-23). Adding version nodes is otherwise transparent to binaries built before the change **[external: GNU symbol versioning]** |
| Phase 1 file move | no ABI change; an API change only for consumers that hard-code `-I…/include/mtl` paths without pkg-config (the forwarding stubs cover `<mtl/x.h>`) **[inferred]** |
| Release F | no ABI change; the deprecation is compile-time only |
| F+2 removal | ABI break for legacy binaries → libmtl soname bump (§6.4); unified binaries unaffected if `libmtl_unified.so.1` stays a separate DSO; with a merged DSO, a soname bump would hit unified users too, which argues for keeping it separate (M8) |
| Struct layouts | none of the legacy structs need to change for any stage; the gate edits add only attributes |

### 8.3 What stays public in the transition (D-24)

Until F everything is public and unchanged; only attributes and an include path are added. From F to F+2 everything still works, first with a warning, then with an explicit opt-in.

Engine fixes reach legacy users throughout (D-24), and wire-visible changes stay behind legacy opt-in flags. The legacy session layer is bug-fix-only from F (14 §7) and is never deleted, only moved to the internal tier.

## 9. Proposed log entries (for the maintainer; not applied)

| Proposed ID | Text | Changes |
|---|---|---|
| D-71 | Session-level headers become non-public in three tiers (public / legacy / internal), with the legacy version nodes `MTL_LEGACY_SESSION`, `_PIPELINE`, `_CORE` and `MTL_INTERNAL` defined in Phase 0; deprecation at F, opt-in at F+1, not installed at ≥ F+2 | Q-ABI-4 ("frozen" → "deprecated, then internal"); refines D-23, D-69 |
| D-72 | Pre-hide gate: every legacy symbol is mapped in the migration table or an approved CUT; H-01 to H-19 scheduled as in §8.1 | 09 §3/§8 "later" rows (RTCP, video detect, timing parser per unit, user tasklets, RX provide, inline notify) become pre-F |
| D-73 | RTP level enters the unified API before F (S8) | supersedes D-25; NG2 and Q-MODE-1 (a) |
| D-74 | Codec and converter plugin ABI V2 (`mtl_plugin.h`, host dispatch table, no libmtl link, no tasklet callback); V1 loaded until F+2 | refines Q-MODE-4 |
| D-75 | Standalone conversion through `mtl_convert.h`; per-pair converters internal | answers Q-MODE-4's second half and the `st_convert_internal.h` row of Q-MODE-7 |
| Q-HIDE-1 | Approve CUT-1 … CUT-8 and CUT-? individually | §7.2 |
| Q-HIDE-2 | Keep `libmtl_unified.so.1` separate at the freeze, so that legacy removal bumps only libmtl's soname? | M8 / D-23 |
| Q-HIDE-3 | Same removal date for `MTL_LEGACY_PIPELINE` as for `MTL_LEGACY_SESSION`, or decided at the Phase 2 go/no-go? | Q-ABI-4 |
| Q-HIDE-4 | User tasklets (H-08): keep as an advanced, opt-in surface, or cut (D-04 purity)? | Q-THR-1, Q-THR-3 |

## 10. Open questions and risks

1. **Scope growth.** H-01, H-02, H-05 to H-11 and H-15 move "later" work before F. If the maintainer will not grow v1, F slips, or the hide moves to a later release than the freeze; the stages of §8.1 shift as a block. Spike S3 should cost H-01 and H-02 together with S8. **[inferred]**
2. **Private users** (Q-MIG-1): about 10 private repositories may depend on session-only features that no in-tree consumer shows (slice mode at bobi, `DATA_PATH_ONLY`, user DMA). The CUT list must be re-checked against the M10 survey, preferably using the `readelf -V` audit that the Phase 0 nodes make possible.
3. **`mtl_api.h` is legacy too.** The unified header bridges to it by a forward declaration only (`mtl_unified.h:845-848`), so no include is needed. But `mtl_instance_from_legacy` takes `struct mtl_main_impl*`. It must stay exported (`MTL_1.0`) for the whole legacy window, and is then deprecated with the legacy tier.
4. **Acceptance suite.** It hard-codes `.local_install` and RxTxApp's legacy JSON kinds (`tests/acceptance/mtl_engine/rxtxapp_config.py:20-41`). Moving RxTxApp to unified while keeping the JSON schema is the critical path for the Phase 6 gate.
5. **Stage-2 surprises.** A `#error` gate fails builds that include a legacy header only for a type, for example `st20_rx_frame_meta` in GStreamer (`gst_mtl_st20p_rx.c:140`). The stage-2 CI job of §6.5 must run on the ecosystem trees before F+1.
