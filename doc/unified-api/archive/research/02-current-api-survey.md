# 02 — Survey of the current public API and its flaws

| Field | Value |
|---|---|
| Topic | Inventory, cross-family pattern analysis and flaw list of the MTL public API (`include/`) |
| Revision | r1 |
| Date | 2026-09-29 |
| Baseline | `main` @ `545a266a` (VERSION `26.01.0.DEV`) |
| Scope | All 14 installed headers (`include/meson.build:4-6`, `:11`) plus spot checks in `lib/src` to confirm behaviour |
| Labels | **[verified]** = read at the cited `path:line`; **[inferred]** = reasoned from verified facts; **[unknown]** = not established |

## TL;DR

- The public surface is **475 function declarations** (gcc `-aux-info` over all installed headers) and **~207 flag bits** across 24 flag namespaces. About 105 of the functions are colour-conversion helpers; the remaining ~370 are 18 session "families" that re-express the same ~12 concepts with different names, struct shapes, bit positions and semantics. **[verified]**
- There are **four incompatible frame-exchange models** (index callbacks for TX sessions, pointer callbacks + put for RX sessions, get/put for pipelines, mbuf get/put for RTP level) and **12 distinct per-frame metadata structs** (5 TX-session, 4 RX-session, 3 pipeline; §3.5) that re-declare the same timestamp/status fields in different orders. None is uniformly available: e.g.
  ST22P has no stats getter at all, ST30P has no user-timestamp flag, ST41 has no frame-level RX. **[verified]**
- **No ABI discipline**: no `soversion`, no symbol version script, no visibility control (`lib/meson.build:151-159`); every ops/meta/stats struct is caller-allocated with no `size`/`version` field; stats structs have already been silently re-laid-out (`include/st20_api.h:1800-1827`). **[verified]**
- **A live-stream app cannot reliably learn what happened to a frame.** TX "done" means "the NIC released the buffer", not "sent on time"; the pipeline reports `COMPLETE` unconditionally (`lib/src/st2110/pipeline/st20_pipeline_tx.c:285`); `notify_frame_late`'s argument means *epochs* for video/ANC, *packet times* for audio and is *always 0* on the pipeline drop path; RX
  back-pressure drops are visible only as a counter. **[verified]**
- Callbacks run on the data-plane tasklet with a contract stated only as "non-block", while the library itself takes a `pthread_mutex` on the tasklet when `*_FLAG_BLOCK_GET` is set (`st20_pipeline_tx.c:29-45`). **[verified]**
- Worth keeping: the pipeline get/put simplicity, the ext-frame concept (esp. RX `query_ext_frame` and TX two-phase release), user pacing vs. user timestamp being separate knobs, rich RX per-frame integrity metadata (`pkts_recv[]`, per-port seq loss, first/last packet TAI), the timing parser, online src/dst update, and the plugin/tasklet extension points.

## 1. Method

1. Read every installed header end-to-end (`mtl_api.h`, `st_api.h`, `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h`, `st_pipeline_api.h`, `st30_pipeline_api.h`, `st40_pipeline_api.h`, `st_convert_api.h` + the installed `st_convert_internal.h`, `mtl_sch_api.h`, `mtl_lcore_shm_api.h`, `experimental/st20_combined_api.h`).
2. Counted functions with `gcc -fsyntax-only -aux-info` over a TU including all headers; counted flags by regular expression over `#define`/enum members.
3. Cross-checked each suspicious header contract against the implementation (cited below) — the header is the contract a redesign must replace, the implementation is the behaviour it must preserve or fix.
4. Read `.github/copilot-docs/mtl-knowledge-base.md` §1, §6 and `doc/design.md` §2, §6.

## 2. Inventory

### 2.1 Functions, callbacks and flags per family

"Fn" = public function declarations (incl. `static inline`). "CB" = function pointers in the family's ops struct(s). "Flags" = distinct flag macros/enumerators. There is **no per-session start/stop** in any family — a session is live from `*_create` until `*_free` (only `mtl_start/stop` and `mtl_sch_start/stop` exist). **[verified]**

| Family | Header | Fn | Create / free | Frame exchange | CB (tx / rx) | Stats | Online update | Flags (tx / rx) |
|---|---|---|---|---|---|---|---|---|
| core `mtl_*` | `mtl_api.h` | 70 | `mtl_init`/`uninit`, `mtl_start`/`stop`/`abort` | — (mem: `mtl_hp_*`, `mtl_dma_*`, `mtl_udma_*`) | 3 in init params (`ptp_get_time_fn`, `ptp_sync_notify`, `stat_dump_cb_fn`) + 2 log hooks | `mtl_get_port_stats`, `mtl_get_fix/var_info` | log level, sch sleep | 39 + 2 port |
| common `st_*`/`st10_*` | `st_api.h` | 12 | — | — | — | `st_get_var_info` (counts only) | — | — |
| st20 session | `st20_api.h` | 13 / 15 (+6 helpers) | `st20_{tx,rx}_create`/`free` | TX `get_next_frame(idx)`+`notify_frame_done(idx)`; RX `notify_frame_ready(ptr)`+`put_framebuff(ptr)`; RTP `get/put_mbuf`; slice callbacks | 6 / 7 | `st20_{tx,rx}_get/reset_session_stats` | update dst / src, `st20_tx_set_ext_frame` | 12 / 13 |
| st22 session | `st20_api.h` | 7 / 10 | `st22_tx_create`/`free`, `st22_rx_create`/`free` | same as st20 (index / pointer / mbuf) | 5 / 3 | **none** | `st22_tx_update_destination`, `st22_rx_update_source` | 9 / 7 |
| st30 session | `st30_api.h` | 8 / 9 (+6 helpers) | `st30_{tx,rx}_create`/`free` | index TX / pointer RX / mbuf | 4 / 3 | yes | update dst / src | 8 / 7 |
| st40 session | `st40_api.h` | 8 / 9 (+10 RFC 8331 helpers) | `st40_{tx,rx}_create`/`free` | index TX / pointer RX / mbuf | 4 / 2 | yes | update dst / src | 8 / 3 |
| st41 session | `st41_api.h` | 8 / 8 | `st41_{tx,rx}_create`/`free` | index TX; **RX is RTP-only** (`st41_rx_ops` has only `notify_rtp_ready`, `include/st41_api.h:271-277`) | 3 / 1 | yes | update dst / src | 6 / 2 |
| st20p | `st_pipeline_api.h` | 16 / 16 | `st20p_{tx,rx}_create`/`free` | `st20p_{tx,rx}_get_frame`/`put_frame`/`put_frame_abort`, `st20p_tx_put_ext_frame`, `st20p_tx_notify_ext_frame_free` | 4 / 4 | yes (reuses `st20_*_user_stats`) | update dst / src | 15 / 17 |
| st22p | `st_pipeline_api.h` | 11 / 12 | `st22p_{tx,rx}_create`/`free` | get/put/abort, `st22p_tx_put_ext_frame` | 4 / 3 | **none** | update dst / src | 12 / 9 |
| st30p | `st30_pipeline_api.h` | 12 / 11 | `st30p_{tx,rx}_create`/`free` | get/put/abort (`struct st30_frame`) | 3 / 1 | yes | update dst / src | 7 / 5 |
| st40p | `st40_pipeline_api.h` | 13 / 13 | `st40p_{tx,rx}_create`/`free` | get/put/abort (`struct st40_frame_info`) | 3 / 1 | yes | update dst / src | 11 / 5 |
| st22 plugin / st20 converter | `st_pipeline_api.h` | 19 | `st22_{encoder,decoder}_register`, `st20_converter_register`, `st_plugin_register` | `st22_encoder_get/put_frame`, `st22_decoder_get/put_frame`, `st20_converter_get/put_frame` | 3 per device struct | — | — | 1 + 1 `*_RESP_FLAG_BLOCK_GET` |
| frame helpers | `st_pipeline_api.h` | 32 | `st_frame_create`/`free`/`create_by_malloc` | — | — | — | — | 3 `ST_FRAME_FLAG_*` |
| convert | `st_convert_api.h` + `st_convert_internal.h` | 47 + 58 | — | — | — | — | — | — |
| sch | `mtl_sch_api.h` | 6 | `mtl_sch_create`/`free`/`start`/`stop`, `register`/`unregister_tasklet` | — | 3 (`start`, `stop`, `handler`) | — | — | — |
| lcore shm | `mtl_lcore_shm_api.h` | 2 | — | — | — | `mtl_lcore_shm_print` | `mtl_lcore_shm_clean` | — |
| experimental st20rc | `experimental/st20_combined_api.h` | 6 | `st20rc_rx_create`/`free` | `notify_frame_ready` + `st20rc_rx_put_frame` | 0 / 2 | **none** | **none** | 0 / 5 |
| **Total** | 14 headers | **475** | | | | | | **~207** |

Per-header function counts from `-aux-info`: `mtl_api.h` 70, `st_api.h` 12, `st20_api.h` 51, `st30_api.h` 23, `st40_api.h` 27, `st41_api.h` 16, `st_pipeline_api.h` 106, `st30_pipeline_api.h` 24, `st40_pipeline_api.h` 27, `st_convert_api.h` 47, `st_convert_internal.h` 58, `mtl_sch_api.h` 6, `mtl_lcore_shm_api.h` 2, `experimental/st20_combined_api.h` 6. **[verified]**

### 2.2 Getter-symmetry matrix (where the same concept exists or not)

| Getter | st20 | st22 | st30 | st40 | st41 | st20p | st22p | st30p | st40p |
|---|---|---|---|---|---|---|---|---|---|
| `*_get_session_stats` tx / rx | Y / Y | – / – | Y / Y | Y / Y | Y / Y | Y / Y | – / – | Y / Y | Y / Y |
| `*_get_sch_idx` tx / rx | Y / Y | Y / Y | – / – | – / – | – / – | Y / Y | – / – | – / – | – / – |
| `*_get_queue_meta` (rx) | Y | Y | Y | Y | Y | Y | Y | – | Y |
| `*_pcapng_dump` (rx) | Y | Y | – | – | – | Y | Y | – | – |
| framebuffer address tx | `get_framebuffer` | `get_fb_addr` | `get_framebuffer` | `get_framebuffer` | `get_framebuffer` | `get_fb_addr` | `get_fb_addr` | `get_fb_addr` | `get_fb_addr` + `get_udw_buff_addr` |
| framebuffer address rx | – | `get_fb_addr` | – | – | n/a | `get_fb_addr` | `get_fb_addr` | – | `get_udw_buff_addr` |
| ext-frame tx / rx | `set_ext_frame` / `ops.ext_frames`+`query_ext_frame` | – / – | – / – | – / – | – / – | `put_ext_frame` / `ops.ext_frames`+`query_ext_frame` | `put_ext_frame` / `query_ext_frame` | – / – | – / – |
| `notify_event` tx / rx | Y / Y | Y / Y | – / – | – / – | – / – | Y / Y | Y / Y | – / – | – / – |
| `notify_frame_late` (tx) | Y | Y | Y | Y (undocumented, `include/st40_api.h:403`) | – | Y | Y | Y | Y |

**[verified]** from the headers listed in §2.1.

## 3. Pattern analysis — one concept, many shapes

### 3.1 Frame acquisition

| Model | Where | App → lib | Lib → app | Identifies frame by | Thread |
|---|---|---|---|---|---|
| A. Pull-by-callback (TX session) | st20/22/30/40/41 TX | `get_next_frame(priv, &idx, meta)` — lib asks, app answers | `notify_frame_done(priv, idx, meta)` | `uint16_t` index into lib-owned array | tasklet |
| B. Push-by-callback + put (RX session) | st20/22/30/40 RX, st20rc | `st*_rx_put_framebuff(h, void* frame)` | `notify_frame_ready(priv, void* frame, meta)`; return `<0` = lib reclaims | `void*` address | tasklet |
| C. Get/put (pipeline) | st20p/22p/30p/40p, plugins | `*_get_frame` → fill → `*_put_frame` / `*_put_frame_abort` | `notify_frame_available(priv)` (no args) + optional `notify_frame_done(priv, frame)` on TX | pointer to lib-owned `st_frame` / `st30_frame` / `st40_frame_info` | app thread (get/put) + tasklet (notify) |
| D. mbuf get/put (RTP level) | all sessions, `*_TYPE_RTP_LEVEL` | TX `get_mbuf(h,&usrptr)` / `put_mbuf(h,mbuf,len)` | RX `notify_rtp_ready(priv)` then app polls `get_mbuf` | opaque DPDK `rte_mbuf*` as `void*` | app thread + tasklet |

Consequences:

- TX identifies frames by index, RX by pointer (`include/st20_api.h:1186`, `:1544`, `:2262`). **[verified]**
- TX session `get_next_frame` meta is **in/out**: the lib pre-fills `epoch`/`timestamp` with its schedule and the app may overwrite `tfmt`/`timestamp` (`lib/src/st2110/st_tx_video_session.c:816-819`). Nothing in the header says which fields are inputs. **[verified]**
- Model C's `notify_frame_available` carries no handle and no frame, so an app with several sessions per `priv` must re-poll all of them. **[verified]** `include/st_pipeline_api.h:922`.

### 3.2 Blocking and timeouts

- Every pipeline has a `*_FLAG_BLOCK_GET` at **bit 15** (consistent), a `*_wake_block()` and a `*_set_block_timeout(h, ns)`; default timeout 1 s (`st20_pipeline_tx.c:1134`). Plugins have the same trio via `*_RESP_FLAG_BLOCK_GET` set *by the plugin* in `resp_flag` (`include/st_pipeline_api.h:653-667`). **[verified]**
- Blocking is a create-time flag, not a per-call argument; timeout is session state, not a per-call argument. A `NULL` return covers "no frame", "timeout", "not ready", and "destroying" alike (`st20_pipeline_tx.c:766-800`). **[verified]**
- Session-level APIs have no blocking concept at all; the app must build its own wait around `notify_*` callbacks. **[verified]**

### 3.3 External memory

| Variant | Struct | Planes | Binding time | Release signal |
|---|---|---|---|---|
| st20 TX session | `st20_ext_frame` (`include/st20_api.h:1105-1114`) | 1 | `st20_tx_set_ext_frame(h, idx, ext)` per index, any time | `notify_frame_done(idx)` |
| st20 RX session | `st20_ext_frame` | 1 | static array `ops.ext_frames` **or** dynamic `query_ext_frame` | `st20_rx_put_framebuff` |
| st20p/st22p TX | `st_ext_frame` (`include/st_pipeline_api.h:248-259`) | 4 | `put_ext_frame(h, frame, ext)` per frame | `notify_frame_done`; st20p only: optional two-phase `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` + `st20p_tx_notify_ext_frame_free` |
| st20p/st22p RX | `st_ext_frame` | 4 | st20p: static `ops.ext_frames` or `query_ext_frame`; st22p: `query_ext_frame` only | `put_frame` |
| st30/st40/st41 (session or pipeline) | — | — | not supported | — |

Two ext-frame structs with different field names (`buf_addr`/`buf_iova`/`buf_len` vs `addr[]`/`iova[]`/`linesize[]`/`size`), a static-array mode whose length is implied by `framebuff_cnt` **[inferred]**, and IOVA that the app must obtain itself via `mtl_dma_map` (`doc/design.md:528`). **[verified]**

### 3.4 User pacing and user timestamp — the flag matrix

Bit positions of semantically equal flags (`–` = not offered):

| Flag concept | st20 | st22 | st30 | st40 | st41 | st20p | st22p | st30p | st40p |
|---|---|---|---|---|---|---|---|---|---|
| USER_PACING | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 |
| USER_TIMESTAMP | 4 | 4 | 4 | 4 | 4 | 4 | 4 | **–** | 4 |
| EXACT_USER_PACING | 8 | – | – | 7 | – | 8 | – | – | 9 |
| RTP_TIMESTAMP_EPOCH | 9 | – | – | – | – | 9 | – | – | – |
| DROP_WHEN_LATE | n/a | n/a | n/a | n/a | n/a | 12 | 12 | 16 | 7 |
| EXT_FRAME (tx) | 2 | – | – | – | – | 2 | 8 | – | – |
| FORCE_NUMA (tx) | 11 | 8 | 8 | – | – | 11 | 9 | 8 | 8 (rejected) |
| ENABLE_RTCP (tx) | 7 | 6 | 6 | 5 | 5 | 7 | 6 | – | 5 |
| BLOCK_GET | n/a | n/a | n/a | n/a | n/a | 15 | 15 | 15 | 15 |

**[verified]** from the flag definitions in each header. Semantics also drift:

- USER_PACING honours only `ST10_TIMESTAMP_FMT_TAI`; MEDIA_CLK silently falls back to epoch pacing (`include/st20_api.h:51-55`). **[verified]**
- USER_TIMESTAMP: video/ANC accept both formats and unwrap MEDIA_CLK around the scheduled instant (`include/st20_api.h:58-70`); ST30 applies it to the *first packet* only, and later packets differ depending on whether USER_PACING is also set (`include/st30_api.h:59-64`); ST41 and ST40P say "MEDIA_CLK is used" (`include/st41_api.h:48-50`, `include/st40_pipeline_api.h:96-99`),
  contradicting ST40 (`include/st40_api.h:50-56`). **[verified]**
- `rtp_timestamp_delta_us` exists for st20, st20p, st30, st30p but not st22/st22p/st40/st40p/st41. **[verified]**

### 3.5 Frame metadata structs

| Role | Video | Compressed | Audio | Ancillary | Fast-meta |
|---|---|---|---|---|---|
| TX session meta | `st20_tx_frame_meta` | `st22_tx_frame_meta` | `st30_tx_frame_meta` | `st40_tx_frame_meta` (+ buffer `st40_frame`) | `st41_tx_frame_meta` (+ buffer `st41_frame`) |
| RX session meta | `st20_rx_frame_meta` (+ `st20_rx_slice_meta`, `st20_rx_uframe_pg_meta`) | `st22_rx_frame_meta` | `st30_rx_frame_meta` | `st40_rx_frame_meta` | — |
| Pipeline frame (both dirs) | `st_frame` | `st_frame` | `st30_frame` | `st40_frame_info` | — |

All share `tfmt`/`timestamp`/`rtp_timestamp`, most share `epoch`, `pkts_total`, `pkts_recv[]`, `status`; each re-declares them in a different order and each adds its own fields. Receive-time fields are named `timestamp_first_pkt` (st20/st30/st40 RX), `receive_timestamp` (pipelines, st20 tp meta), absent in `st22_rx_frame_meta`. **[verified]**

### 3.6 Status reporting

- One enum `st_frame_status` {COMPLETE, RECONSTRUCTED, CORRUPTED, DROPPED} serves TX done and RX receive (`include/st_api.h:78-92`). TX session `notify_frame_done` carries **no status**; TX pipelines set COMPLETE or DROPPED. **[verified]**
- Events: `st_event` = {VSYNC, RECOVERY_ERROR, FATAL_ERROR} (`include/st_api.h:208-220`), delivered only by video sessions; RECOVERY/FATAL are only raised by TX video (`lib/src/st2110/st_tx_video_session.c:4169-4258`). Audio does recover queues (`lib/src/st2110/st_tx_audio_session.c:2718-2749`) but `st30_tx_ops` has no `notify_event`. **[verified]**
- No event for link down, PTP unlocked/relock, ARP failure, IGMP, source change, or format change (except ST20 auto-detect). **[verified]** by absence in `st_event`.

### 3.7 Stats

- Pattern: `struct stNN_{tx,rx}_user_stats { struct st_{tx,rx}_user_stats common; <family fields> }`, getter + reset, "Briefly acquires the per-session spinlock". **[verified]**
- The common struct mixes transport and pipeline counters and documents which layer fills which (`include/st_api.h:344-461`) — a good step, but it claims ST22P fills pipeline counters while no ST22/ST22P getter exists. **[verified]**
- Every getter doc lists a nonexistent `@param port` (`include/st20_api.h:1985-1986`, `include/st30_api.h:611-612`, `include/st40_pipeline_api.h:255-256`, …). **[verified]**

### 3.8 Port config and redundancy

- Sessions inline `dip_addr[2][4]`, `port[2][64]`, `num_port`, `udp_port[2]`, `udp_src_port[2]`, `tx_dst_mac[2][6]`, `payload_type`, `ssrc`; pipelines factor these into `st_tx_port` / `st_rx_port` (`include/st_pipeline_api.h:838-879`) but keep `tx_dst_mac` outside. **[verified]**
- Ports are referenced **by name string** that must match `mtl_init_params.port[]` (`include/st20_api.h:1136`), not by index/handle. **[verified]**
- Redundancy = `num_port = 2`, fixed at `MTL_SESSION_PORT_MAX = 2` (`include/mtl_api.h:193-198`). The session-port setters take the wrong enum (`st_rxp_para_ip_set(..., enum mtl_port port, ...)`, `include/st_pipeline_api.h:2404`, `:2414`). **[verified]**
- `st_tx_dest_info` can change IP and UDP port but not MAC, source port or SSRC (`include/st_api.h:148-153`). **[verified]**

### 3.9 Error returns

- Create returns `NULL` with no `errno`/out-code (no `errno` assignment in `lib/src/st2110/st_tx_video_session.c`, `st_rx_video_session.c`, `pipeline/*.c`). The only diagnosis is the log. **[verified]**
- Size getters return `size_t` (0 on error), count getters return `int`, `st20_rx_put_mbuf` returns `void`; callbacks return `int` but TX `notify_frame_done`'s return is ignored (`lib/src/st2110/st_tx_video_session.c:100-107`). **[verified]**

## 4. Flaws

Grouped; each item is self-contained with evidence.

### 4.1 ABI fragility

1. **No ABI versioning at all.** `shared_library()` has no `soversion`/`version`, no `-Wl,--version-script`, no `-fvisibility=hidden` (`lib/meson.build:151-159`; grep for `visibility|version-script` in `meson.build`, `lib/` is empty). Every non-static internal symbol is exported. **[verified]**
2. **Caller-allocated structs without `size`/`version`.** `mtl_init_params`, all `*_ops`, all `*_meta`, all `*_user_stats`, `st_frame` are passed by pointer with the size baked into the app at compile time. The in-tree precedent is a silent field removal/rename in `st20_rx_user_stats` (`include/st20_api.h:1800-1827`) — an old binary reading `incomplete_frames_cnt` now reads a
   different offset. **[verified]**
3. **Array sizes from compile-time constants in public structs**: `MTL_PORT_MAX` (8) sizes eight arrays in `mtl_init_params` (`include/mtl_api.h:574-733`); `MTL_SESSION_PORT_MAX` (2) everywhere; `ST40_MAX_META` (20) inside `st40_frame` (`include/st40_api.h:308-318`); `ST_MAX_PLANES` (4) in `st_frame`. Any change is an ABI break. **[verified]**
4. **Enums as struct members** (`enum st_fps`, `st20_fmt`, `st_frame_fmt`, …) — width is compiler-defined and new values need recompiling. `mtl_pmd_type` even documents "not change the enum value any more" (`include/mtl_api.h:229-231`). **[verified]** / impact **[inferred]**.
5. **Lib-private pointers in public structs**: `st_frame.priv` "do not touch" (`include/st_pipeline_api.h:315-316`), same in `st30_frame`, `st40_frame_info`, plugin frame metas. **[verified]**
6. **"Internal" header installed publicly**: `st_convert_internal.h` (58 functions, e.g. `*_simd_dma` taking `mtl_udma_handle`) is in `mtl_header_files` (`include/meson.build:4`) and included by `st_convert_api.h:13`. **[verified]**
7. **Build-config header is part of the API**: `mtl_api.h:27` includes the generated `mtl_build_config.h` (version + compiler string, `meson.build:43-52`). There is no runtime header/library compatibility check. **[verified]**
8. **Debug-only fields in production structs**: `mtl_init_params.port_packet_loss[]` (`include/mtl_api.h:577`), `st40_tx_ops.test` / `st40p_tx_ops.test` "silently ignored" in release (`include/st40_api.h:375-378`), `*_SIMULATE_PKT_LOSS` flags, `st_rx_rtcp_ops.sim_loss_rate`. **[verified]**

### 4.2 Datapath-context callbacks with unclear contract

9. **Callbacks run on the tasklet; the contract is one sentence.** Every callback doc says "only non-block method can be used … as it run from lcore tasklet routine" (e.g. `include/st20_api.h:1183-1184`). Not stated: max duration, whether MTL APIs may be re-entered (the st20p code explicitly allows calling `st20p_tx_notify_ext_frame_free` from inside `notify_frame_done`,
   `st20_pipeline_tx.c:265-266`), which thread (with `MTL_FLAG_TASKLET_THREAD` it is a non-EAL pthread), and that one slow callback stalls every session on that scheduler. **[verified]** / impact **[inferred]**.
10. **The library itself breaks the two-world rule when BLOCK_GET is set**: the tasklet calls `tx_st20p_notify_frame_available` → `tx_st20p_block_wake` → `pthread_mutex_lock` (`st20_pipeline_tx.c:29-45`, called from frame-done at `:293`). The same pattern exists in st30p (`lib/src/st2110/pipeline/st30_pipeline_tx.c:22-36`) and, by construction, the other pipelines
    **[inferred]**. **[verified]** for st20p and st30p.
11. **User time source runs in the datapath**: `mtl_init_params.ptp_get_time_fn` (`include/mtl_api.h:662-667`) becomes the interface time source used by pacing (`lib/src/mt_main.h:1458`, `:1950-1951`) with no stated latency or thread-safety requirement. **[verified]** (hot-path use **[inferred]** from `mt_get_ptp_time` call sites in the TX sessions).
12. **`mtl_ptp_read_time` can block and is racy**: it may `pthread_join` the TSC calibration thread (`lib/src/mt_main.c:1114`, `lib/src/mt_main.h:1839-1845`) and updates a two-field cache without synchronisation (`lib/src/mt_main.c:1116-1127`). It is used by the inline helpers `st30_frame_is_late`/`st40_frame_is_late` (`include/st30_pipeline_api.h:374`,
    `include/st40_pipeline_api.h:403`), while `st_frame_is_late` uses `_raw` (`include/st_pipeline_api.h:2472`). **[verified]** (race impact **[inferred]**).

### 4.3 Ownership ambiguity

13. **TX "frame done" is fired for frames that were never sent.** ST22 invalid codestream size → `tv_notify_frame_done` immediately (`lib/src/st2110/st_tx_video_session.c:2449-2455`), indistinguishable from a sent frame. **[verified]**
14. **RX dynamic ext-frame forces incomplete-frame delivery**: `query_ext_frame` is rejected unless `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` is set (`lib/src/st2110/st_rx_video_session.c:4284-4288`; header hint at `include/st_pipeline_api.h:582`). Two orthogonal features coupled because the lib cannot recycle an app-owned buffer. **[verified]**
15. **Two-phase ext-frame release exists only for st20p TX**; st22p TX has `put_ext_frame` without it, and `st20p_tx_notify_ext_frame_free` is a "silent no-op" when the flag is unset or a converter is used (`include/st_pipeline_api.h:1857-1859`). **[verified]**
16. **Threading contract of get/put lives only in `doc/design.md` §6.3.1** ("one application thread per session and per direction", `doc/design.md:357-371`); the header says nothing (`include/st_pipeline_api.h:1791-1816`). **[verified]**
17. **`user_meta` pointer lifetime** in `st20_tx_frame_meta`/`st_frame` (`include/st20_api.h:443-449`) is not specified (copied at put? read at build?). **[unknown]**

### 4.4 Flags: silently ignored, conflicting, mis-wired

18. **Mis-wired flag**: `st30p_tx_create` tests `ST30P_RX_FLAG_FORCE_NUMA` (bit 2) on TX ops (`lib/src/st2110/pipeline/st30_pipeline_tx.c:671`), so `ST30P_TX_FLAG_FORCE_NUMA` (bit 8) forces NUMA for the transport (`:278-280`) but not for the pipeline ctx, and unused TX bit 2 would trigger it. **[verified]**
19. **Unknown bits are never rejected** — no ops-check masks flags (grep for flag-mask validation in `lib/src/st2110` finds only the EXACT-needs-USER_PACING check at `st_tx_video_session.c:4043`, `st_tx_ancillary_session.c:2172`). Combined with 24 non-aligned namespaces (§3.4) an app passing `ST20_TX_FLAG_*` into `st20p_tx_ops.flags` gets silent misbehaviour. **[verified]** /
    impact **[inferred]**.
20. **Dead / contradictory core flags**: `MTL_FLAG_BIND_NUMA` "default behavior" is never read in `lib/src` while `MTL_FLAG_NOT_BIND_NUMA` is (`lib/src/mt_main.c:622`); `MTL_FLAG_CNI_THREAD` silently wins over `MTL_FLAG_CNI_TASKLET` (`lib/src/mt_cni.c:567-572`); `MTL_FLAG_TX_VIDEO_MIGRATE` doc describes RX (`include/mtl_api.h:345-350`). **[verified]**
21. **`*_DROP_WHEN_LATE` silently requires `*_USER_PACING` and a TAI timestamp** (`st20_pipeline_tx.c:124-130`); the header says only "drop frames when the mtl reports late frames … next frame from pipeline is omitted" (`include/st_pipeline_api.h:461-466`), which is not what the code does (it drops frames whose TAI is more than one frame period in the past,
    `st20_pipeline_tx.c:136-139`). **[verified]**
22. **"Reserved"/"not in use" ops fields**: `st20_rx_ops.pacing`/`.packing` (`include/st20_api.h:1509-1512`), `ST22_PACK_SLICE` "not support now" (`include/st20_api.h:386-387`, 0 uses in `lib/src`), deprecated `sample_size`/`sample_num` (`include/st30_api.h:474-479`), `tx/rx_sessions_cnt_max` (`include/mtl_api.h:745-760`). **[verified]**
23. **`ST40P_*_FLAG_FORCE_NUMA` are public but "NOT SUPPORTED YET"** and fail create (`include/st40_pipeline_api.h:117-121`, `lib/src/st2110/pipeline/st40_pipeline_tx.c:675-678`). **[verified]**

### 4.5 Time units and epochs

24. **`st_frame.timestamp` changes meaning with direction**: TX = TAI ns (`st_tx_video_session.c:1981`), RX = 32-bit media-clock ticks in a `uint64_t` with `tfmt = MEDIA_CLK` (`lib/src/st2110/st_rx_video_session.c:867`), duplicated by `rtp_timestamp`. `st_frame_is_late()` therefore always returns false on RX frames (`include/st_pipeline_api.h:2470-2474`). **[verified]**
25. **`epoch` is never defined** (units: frame periods since TAI 0 for video; packet times for audio, `st_tx_audio_session.c:264-268`) and is "epoch info for the done frame" in pipelines. **[verified]** / audio semantics **[verified]**.
26. **Unit typos in the contract**: `rl_offset_ns` documented as "offset time(us)" (`include/st30_api.h:465-466`, `include/st30_pipeline_api.h:176-177`); `st30_get_packet_time` returns `double` ns with "<0 error" (`include/st30_api.h:748-756`); `st10_vsync_meta.frame_time` is `double` ns (`include/st_api.h:196-203`). **[verified]**
27. **Clock domain for TX is implicit**: "current ptp time" means PTP from the primary port, or the user fn, or system time, depending on flags (`include/mtl_api.h:662-667`); sessions on the R port still pace on P (`st_tx_audio_session.c:266`). **[verified]**

### 4.6 Control and data plane mixed

28. **Online updates and stats reads touch data-plane state from app threads** under a spinlock (`@note Thread-safe. Briefly acquires the per-session spinlock`) — fine for stats, but `update_destination`/`update_source` semantics mid-frame (which frame gets the new address?) are unspecified. **[unknown]**
29. **Core header carries media concepts**: `enum st21_tx_pacing_way`, `pacing`, `tx_audio_sessions_max_per_sch`, `pkt_udp_suggest_max_size`, `nb_rx_hdr_split_queues` live in `mtl_init_params` (`include/mtl_api.h:318-335`, `:681-705`). Pacing is chosen once per instance, not per session. **[verified]**

### 4.7 Magic sizes and limits

30. Framebuffer count `[2, 8]` for video (`ST20_FB_MAX_COUNT`, `include/st20_api.h:24`) is a hard cap that bounds pipeline depth/latency tuning; ANC meta capped at 20 per frame and excess is **truncated with a `warn()` from the datapath** (`lib/src/st2110/st_rx_ancillary_session.c:205-207`); `MTL_PKT_MAX_RTP_BYTES = 1460-8-100` (`include/mtl_api.h:89`); `failed_cause[64]`;
    `MTL_PCAP_FILE_MAX_LEN 32`; `ST30_TX_FIFO_DEFAULT_TIME_MS 10`. **[verified]**

### 4.8 Inconsistency and plain bugs in the contract

31. `ST_PLUGIN_MAGIC` shifts `c` by 16 instead of 8 (`include/st_pipeline_api.h:71`). **[verified]**
32. `st_frame_fmt_is_codestream` treats the `_END` sentinel as a codestream (`<=`, `include/st_pipeline_api.h:2309`). **[verified]**
33. Copy-paste docs: `ST40_RX_FLAG_DATA_PATH_ONLY`/`ST41_RX_FLAG_DATA_PATH_ONLY` say "flags of struct st30_rx_ops" (`include/st40_api.h:111`, `include/st41_api.h:64`); `ST30P_RX_FLAG_DATA_PATH_ONLY` points to `st30_rx_get_queue_meta` although `st30p_rx_get_queue_meta` does not exist (`include/st30_pipeline_api.h:243`); `mtl_sch_unregister_tasklet` "can be unregistered at
    runtime before mtl_sch_start" (`include/mtl_sch_api.h:131-133`); `st20_combined_api.h:22` "Handle to rx st2110-22 pipeline". **[verified]**
34. Divergent late/done interplay: st20p/st22p fire **both** `notify_frame_done(DROPPED)` and `notify_frame_late` (`include/st_pipeline_api.h:928-931`); st30p/st40p headers say done is called "only when notify_frame_late is NOT called" (`include/st30_pipeline_api.h:143-145`, `include/st40_pipeline_api.h:166-168`) while the code calls both (`st30_pipeline_tx.c:138-142`,
    `st40_pipeline_tx.c:141-145`). **[verified]**

## 5. Can a live-stream application know what happened?

| Question | What the current API offers | Gap |
|---|---|---|
| TX: was frame N sent on time? | Nothing per frame. `notify_frame_done` meta `timestamp` = TAI the RTP timestamp was derived from (`st_tx_video_session.c:1982-1987`), not the wire time. Aggregate `stat_epoch_drop`/`stat_epoch_mismatch`. | No actual first/last-packet TX time, no lateness per frame, no status on session done. **[verified]** |
| TX: was frame N late / skipped? | `notify_frame_late(priv, n)`, no frame ID; `n` = epochs for video/ANC (`st_tx_video_session.c:682-683`), packet times for audio (`st_tx_audio_session.c:317-318`), always 0 on pipeline drops (`st20_pipeline_tx.c:165`) plus transport pass-through (`:462`). None for ST41. | Inconsistent units, no frame correlation. **[verified]** |
| TX: was frame N dropped? | Pipelines with DROP_WHEN_LATE+USER_PACING+TAI: `notify_frame_done` with `status = DROPPED`. Otherwise the transport re-sends the previous frame or skips epochs silently **[inferred]**. | Drop visible only in the narrow flag combination. **[verified]** |
| TX: what is my latency budget? | `st20_tx_get_pacing_params` (tr_offset, trs, vrx) for st20/st20p only. | No "deadline to put_frame for epoch N" query. **[verified]** |
| RX: frame complete / reconstructed / corrupted? | Yes, per frame: `status`, `pkts_total`, `pkts_recv[]`, st40 per-port `port_seq_lost`. Incomplete frames delivered only with `*_RECEIVE_INCOMPLETE_FRAME`; otherwise dropped and counted in `stat_frames_incomplete`. | Default hides loss except via stats. **[verified]** |
| RX: frame dropped because I was slow? | Only `stat_frames_dropped` counter (`include/st_api.h:443-450`). | No event, no sequence gap signal in the next delivered frame. **[verified]** |
| RX: arrival time / network latency? | `timestamp_first_pkt`/`timestamp_last_pkt` (st20), `receive_timestamp` (pipelines), timing-parser `latency`/`fpt` with `*_TIMING_PARSER_META`. HW timestamps only with `MTL_FLAG_ENABLE_HW_TIMESTAMP`, "PF on E810" (`include/mtl_api.h:369-373`). | Good for video; absent in `st22_rx_frame_meta`. **[verified]** |
| Session health (link, PTP, recovery)? | `ST_EVENT_RECOVERY_ERROR`/`FATAL_ERROR` from TX video only; `ptp_sync_notify` global. | No per-session health for audio/ANC/FMD or any RX. **[verified]** |

## 6. What is good and must not be lost

- **Pipeline get/put**: a 4-call loop (`doc/design.md:338-346`) covering conversion, frame lifecycle, optional blocking, `put_frame_abort` for error paths. It is what most apps and all ecosystem plugins use. **[verified]** / usage share **[inferred]**.
- **Ext-frame concept** including multi-plane `st_ext_frame`, RX `query_ext_frame` (app supplies the destination per frame, zero-copy to GPU/app memory) and the st20p two-phase release (`ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE`). **[verified]**
- **User pacing and user timestamp as separate knobs**, with EXACT vs. VRX-aligned pacing and the well-documented MEDIA_CLK unwrap contract (`include/st_api.h:559-587`). **[verified]**
- **Rich RX integrity metadata**: per-port `pkts_recv[]`, reconstructed vs. corrupted status, st40 per-port and merged sequence gaps, first/last packet TAI, per-frame ST 2110-21 timing parser (`st20_rx_tp_meta`). **[verified]**
- **Carefully documented stats semantics** in `st_rx_port_stats`/`st_rx_user_stats` (pre/post redundancy, reorder vs. duplicate, `stat_pkts_unrecovered <= stat_lost_packets`, `include/st_api.h:266-461`). **[verified]**
- **Online source/destination update** without session re-creation (`doc/design.md:551-568`). **[verified]**
- **RTP-level escape hatch** (mbuf get/put) used for ST 2022-6 and custom payloads (`doc/design.md:326-330`). **[verified]**
- **Extension points**: codec/convert plugins with capability bitmasks, app tasklets on MTL schedulers (`mtl_sch_api.h`), app lcore borrowing, user DMA (`mtl_udma_*`). **[verified]**
- **Handle type-guarding** (`MT_HANDLE_GUARD`, `st20_pipeline_tx.c:317`) rejects a wrong handle at runtime. **[verified]**
- **Slice mode** (`query_frame_lines_ready`/`notify_slice_ready`) for sub-frame latency. **[verified]**

## 7. Documentation drift noticed (not fixed — outside this note's write scope)

- `doc/design.md:384` references `ST40P_TX_FLAG_EXT_FRAME`, which does not exist in any header. **[verified]**
- `doc/design.md:539-544` (§6.11) repeats the USER_TIMESTAMP paragraph twice and never describes USER_PACING. **[verified]**
- `doc/design.md:555-568` (§6.13) lists update functions for st20/22/30/40 and st20p/22p only; st41, st30p, st40p also have them. **[verified]**
- `.github/copilot-docs/mtl-knowledge-base.md:41` lists pipeline prefixes `st20p_/st22p_/st30p_`, omitting `st40p_`. **[verified]**

## Open questions for the maintainer

1. **ABI policy for the new API.** Today there is no soname, version script or struct versioning (§4.1). Why it matters: every extensible-struct choice in the redesign (size-prefixed structs à la `struct_size`, opaque attr objects à la libfabric `fi_info`, or getter/setter handles) follows from this. Options: (a) keep "rebuild against each release", document it; (b) soname +
   version script, append-only structs with `size` as first member; (c) fully opaque attribute objects with typed setters.
2. **Keep four frame-exchange models or converge?** (§3.1) Options: (a) one get/put + completion-queue model for all media and both directions, with callbacks as an optional notification layer; (b) keep session callbacks as the low-level tier and pipelines on top; (c) Rivermax-style chunk/stride API as the single low-level primitive with frame helpers above. Matters for how
   much of the ~370 non-convert functions can be deleted.
3. **Which thread may call what, and what may a callback do?** (§4.2) Should callbacks remain on the tasklet (lowest latency, fragile), move to a completion queue the app polls (libfabric CQ style), or be selectable per session? Needs a written re-entrancy and max-duration contract either way, and a decision whether BLOCK_GET-style waiting may ever touch a mutex from the datapath.
4. **What must "frame done" report on TX?** (§5) Candidates: status {sent-on-time, sent-late, dropped, rejected}, scheduled TAI, actual first/last packet TX time (SW or HW timestamp), lateness in ns. Matters because the NIC-release point (`tv_frame_free_cb`) is not the wire-time point and some statuses need new plumbing.
5. **One timestamp representation?** (§4.5) Options: always carry both `tai_ns` (u64) and `rtp_ts` (u32) as separate typed fields in every meta and drop `tfmt`; or keep `tfmt` but make direction-independent. Also: define `epoch` or remove it from the public surface.
6. **Should MEDIA_CLK be accepted for user pacing?** Today it silently falls back to epoch pacing (§3.4). Options: reject at put time, or unwrap it like USER_TIMESTAMP already does.
7. **Flags vs. typed fields.** 207 flag bits in 24 namespaces with no unknown-bit rejection (§4.4). Options: (a) one shared bit namespace across session/pipeline with strict unknown-bit rejection; (b) replace most flags with typed enum/bool fields in attr structs; (c) capability query (`fi_getinfo`-like) so apps learn what a session/NIC actually supports instead of setting
   flags that are silently ignored or rejected.
8. **Scope of external memory in v-next.** Should ext-frame/registered memory become a first-class, media-independent object (register once → memory handle with IOVA, like `fi_mr_reg` or Rivermax memory keys) usable by audio/ANC/FMD too, and should two-phase release be the only release model?
9. **Per-session start/stop?** Sessions are live from create to free (§2.1). A create→configure→start→stop→destroy lifecycle would let apps pre-allocate, update, and arm at a known epoch. Is that wanted, and should `update_destination/source` then be restricted to stopped sessions or given defined mid-stream semantics?
10. **Latency/buffering knobs.** Framebuffer count is capped at 8 and fixed at create; pipeline "late" uses a hidden one-frame grace window. Should the new API expose explicit target latency / queue depth / deadline-to-put per session and a query for "time left before epoch N"?
11. **What survives from pipeline conversion and plugins?** Conversion, codec plugins and the ~105 convert functions are in the same library and headers as the transport. Options: keep in-core; move to a separate `libmtl-convert`/plugin ABI; or keep only in pipelines. Affects header layout and ABI surface.
12. **Error reporting.** Create returns `NULL` with only a log line (§3.9). Options: return `int` + out-handle, set `errno`, or a per-instance last-error string. Also: should get-style calls distinguish timeout / not-started / destroying / no-frame?
13. **Health events.** Should the new API define session and instance events (link, PTP lock state, queue recovery, source loss, format change) for all media types, delivered through the same mechanism as frame completions?
14. **Debug/test knobs in production structs** (`port_packet_loss`, `st40_tx_test_config`, `*_SIMULATE_PKT_LOSS`). Move to a separate debug/attr API, compile-guard them, or keep inline?

