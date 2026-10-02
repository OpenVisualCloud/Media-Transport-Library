# 08 — How the public API is actually used: consumers, patterns, pain, migration impact

**Topic:** every in-tree consumer of the MTL public API (samples, RxTxApp, FFmpeg, GStreamer, OBS, MXL POC, Python, Rust, plugins, KahawaiTest), public downstream users on GitHub, and API-usability issues

**Revision:** 1

**Date:** 2026-09-29

**Baseline:** `main` HEAD `545a266a` (2026-09-28). A few consumer files differ between HEAD and the working tree used for this research: four GStreamer files and a set of `tests/integration_tests/noctx/*` files. Where it matters, HEAD was re-read with `git show HEAD:<path>` and the difference is called out.

**Labels:** **[verified]** means cited `path:line` or URL. **[inferred]** means reasoned from verified facts. **[unknown]** means not established.

**Related:** `00-pr1610-design-review.md`, cited below as "review §N", for the buffer, memory and completion model.

---

## 0. Summary

1. **Every consumer uses the pipeline layer (`st20p/st22p/st30p/st40p`) with blocking get/put on a dedicated app thread.** The only exceptions are code that needs tasklet-context callbacks for zero-copy or sub-frame latency: the MXL bridge, the fwd samples, slice-mode users and the bobi.studio engine. **[verified]**, see §1.
2. **The notify-callback-to-condvar bridge** (callback → `pthread_cond_signal` → thread re-polls `get_frame`) is written again in about 10 samples, OBS, RxTxApp session apps, KahawaiTest, the convert plugin sample, Rust (a `Parker`) and one external repository (a 1 ms sleep poll). Most copies are subtly wrong (no predicate, non-atomic stop flag). **[verified]**, see §2 B1.
3. **Nothing distinguishes "timeout" from "error" from "stopped".** Every consumer treats a NULL `get_frame` as a timeout. FFmpeg maps it to `AVERROR(EIO)`, which ends the run. GStreamer maps it to EOS after N retries. **[verified]**, see §2 B3.
4. **The process-wide instance is re-implemented as a refcounted singleton** in FFmpeg and GStreamer, and worked around in two external projects. The root cause is that `mtl_init` cannot be called again after `mtl_uninit` (issue #1341, open). **[verified]**, see §2 B5.
5. **Zero-copy is the exception.**
   - FFmpeg copies every packet in both directions and has 5 `/* todo: zero copy with external frame mode */` comments.
   - GStreamer is zero-copy only for st20p, and only through the *conversion* path.
   - Python and Rust are copy-only.
   - The ext-frame API needs a lib slot first (`get_frame`, then `put_ext_frame`). Passthrough needs DMA-mapped (IOVA) memory, which framework buffers are not.
   - **[verified]**, see §1 and §4.
6. **Timing is the top source of external bug reports.** Many issues cover user pacing and user timestamps, RTP-vs-TAI, epoch alignment, late-frame policy and interlaced latency. They come mostly from one external product team that uses ext frames, user pacing, user timestamps and 2022-7 all at once. **[verified]**, see §3.
7. **Completion is not "exactly once".**
   - `notify_frame_done` fired twice (#1147).
   - One public downstream engine ships app-side watchdogs, plus 19 source patches to MTL/DPDK, because frames can be silently abandoned with no completion.
   - **[verified]**, see §3.3.
8. **Migration surface:**
   - About 1,500 session-API call sites in-tree, about half of them in KahawaiTest.
   - The ecosystem plugins are small: FFmpeg 61 call sites, GStreamer 58, OBS 10.
   - A pipeline-API compatibility shim over a new unified API looks feasible. Shimming the tasklet-context session callbacks and RTP/slice modes does not.
   - **[inferred]**, see §5.

---

## 1. Consumer inventory

### 1.1 Overview table

Call sites were counted with `git grep -E '(st20p?|st22p?|st30p?|st40p?|st41|st20rc)_(tx|rx)_[a-z_]+\(|struct …_(tx|rx)_ops'` at HEAD. LOC is `.c/.h/.cpp/.hpp/.rs/.py/.i` from `git ls-files`.

| Consumer | LOC | Session call sites / files | Layer | Threading | Memory | Timing | Errors / stats |
|---|---|---|---|---|---|---|---|
| `app/sample/` (33 C + 1 MSVC) | 10,409 | 260 / 35 | Mostly pipeline; session (legacy/), RTP/slice (low_level/), `st20rc` | BLOCK_GET + `wake_block` (12); callback + condvar (~10) | Lib; ext / dyn-ext / GPU BAR / DMA in `ext_frame/`, `fwd/`, `dma/` | USER_TIMESTAMP only in merge/split fwd; no USER_PACING | NULL = "timeout"; pass = `frames > 0`; no stats calls |
| `tests/tools/RxTxApp` | 16,126 | 156 / 19 | Pipeline st20p/22p/30p/40p; session for anc (st40), fmd (st41), legacy/, experimental/ | Thread per session; BLOCK_GET (pipeline) or callback + condvar ring (session) | Lib only; no ext frames (gap noted in #1321) | User pacing + timestamp via `st_app_user_time()` | `stat_dump_cb_fn` → port stats + per-session get/reset |
| FFmpeg plugin (libavdevice) | 3,019 | 61 / 6 | Pipeline st20p/st22p/st30p | Blocking get inside `read_packet` / `write_packet` | Copy both ways | RX pts is a frame counter. TX ignores pts | NULL → `AVERROR(EIO)`. No stats |
| GStreamer plugin | 5,893 | 58 / 8 | Pipeline st20p/st30p/st40p | Blocking get inside `create` / chain; st40p rx polls with a 1 ms sleep | st20p ext frames with conversion; audio and ANC copy | RX PTS = raw TAI ns. TX `use-pts-for-pacing` → USER_PACING | NULL → EOS after retries. No stats |
| OBS (`obs_mtl`) | 763 | 10 / 2 | st20p rx only (tx is `#if TODO_OUTPUT`) | Callback plus condvar | Hands `frame->addr` to OBS, which copies | `out.timestamp = frame->timestamp` (TAI) | none |
| MTL_with_MXL POC | 13,779 | 52 / 8 | Session `st20_rx` frame level plus dynamic `query_ext_frame`; st22 rx/tx; st20p | Tasklet callbacks → SPSC queue → bridge thread | Zero-copy: the NIC writes into `mtl_dma_map`'d MXL grains | Latency stamp written into the payload | Local counters |
| Python (SWIG) | 1,701 | 48 / 7 | Video only (st20/st20p/st22p) plus convert | BLOCK_GET polling; callbacks are unusable from Python | RX is a ctypes/numpy view; TX copies | none | none |
| Rust (`imtl-rs`) | 2,868 | 36 / 2 | st20 session plus st20p, st22p, st30p | Callbacks mutate a ring from lcore threads; user thread parks | Copy only | none | none |
| `plugins/` (st22 codecs, converter) | 1,343 | 0 (a separate plugin ABI) | `st22_encoder_*` / `st22_decoder_*` / `st20_converter_*` | One pthread per session; BLOCK_GET or its own condvar | Lib frames | n/a | none |
| KahawaiTest | 26,560 | 752 / 31 | Nearly everything (about 306 distinct public functions) except st41, st20rc, lcore_shm | Frame threads plus `std::condition_variable` | Lib, ext, dynamic ext, manual release | User pacing, user timestamp, exact pacing | Stats in st20p/st30p/st40p and noctx; SHA-256 checks |
| Unit tests (`tests/unit`) | 25,871 | 78 / 30 | Mostly internals; some pipeline and FFmpeg `mtl_common` harness | n/a | n/a | n/a | n/a |
| `ld_preload/` | 0 (stub) | 0 | **Removed.** The mudp/mufd stack was deleted in `2b182cd87` (#1647); only `meson.build`, which builds nothing, is left | – | – | – | – |
| `manager/` (MtlManager) | – | 0 | **Not an API consumer.** IPC peer over a UNIX socket (`manager/mtl_mproto.h`). The only public hook is `mtl_is_manager_alive()` (`include/mtl_api.h:1657`) | – | – | – | – |

**[verified]** Counts come from the commands above. ld_preload removal: `git show 2b182cd87 --stat`. Manager includes: `manager/mtl_manager.cpp:5-18`.

`CLAUDE.md` still describes `ld_preload/` as an "LD_PRELOAD UDP shim". That is stale.

### 1.2 Per-consumer notes

#### app/sample

- **Main skeleton, repeated in about 33 files.** Parse args, OR in `MTL_FLAG_DEV_AUTO_START_STOP`, `mtl_init`, create loop, `while (!ctx.exit) sleep(1)`, stop and join, free, `mtl_uninit`. **[verified]** e.g. `app/sample/tx_st20_pipeline_sample.c:145-278`.
- **Wrong flag namespace.** `rx_st30_pipeline_sample.c:163` sets `ST30P_TX_FLAG_BLOCK_GET` on *RX* ops; it works only because the bit value is the same. FFmpeg does the same with `ST20_RX_FLAG_DMA_OFFLOAD` on `st20p_rx_ops` (`mtl_st20p_rx.c:176`; BIT 17 in `st20_api.h:212` and `st_pipeline_api.h:616`). **[verified]**
  - Separate per-type flag namespaces with coincident values invite this mistake. **[inferred]**
- **Ext-frame samples.**
  - `ext_frame/tx_st20_pipeline_ext_frame_sample.c` frees its DMA memory (279) *before* `st20p_tx_free` (295).
  - `rx_st20_pipeline_dyn_ext_frame_sample.c` hands out buffers round-robin without checking whether the app still holds them.
  - Both show that buffer lifetime is left entirely to the user. **[verified]**, read with cited lines.
- **Fwd samples are the only in-tree zero-copy RX→TX pattern.**
  - `fwd/rx_st20p_tx_st20p_fwd.c:165` calls `put_ext_frame` with `mtl_hp_virt2iova`.
  - A frame ring is shared between the app thread and the lib `frame_done` callback with no lock (34-58).
  - The comment "frame ooo, should not happen" is at 105.
  - `fwd/rx_st20p_tx_st20p_split_fwd.c` never increments `fb_fwd`, so it always exits `-EIO` (253).
  - **[verified]**, read in the code.
- **Recurring bugs.**
  - `pthread_create` failure is tested with `ret < 0`, which never fires because pthread_create returns a positive errno.
  - `app[]` VLAs are freed on error paths without being zeroed first.
  - Return values of `put_frame`, `put_ext_frame` and `put_framebuff` are ignored everywhere except `tx_st40_pipeline_sample.c:160`.
  - **[verified]**, read in the code. These bugs show what an API should make hard to get wrong.

#### RxTxApp

- **Port setup is copy-pasted in about 16 files.** Example: `tests/tools/RxTxApp/src/tx_st30p_app.c:178-207`, identical in tx_st20p 265-294, tx_st22p 195-224, tx_st40p 231-264 and the rx variants. `parse_json.c` has one ~30-line block per session type, repeated 17 times (2653-3466). **[verified]**, read in the code.
- **User-time helper.** `st_app_user_time()` (`rxtx_app.c:673-703`) builds TAI as epoch-aligned base + `frame_time*n` + offset. It is the only in-tree implementation of "give me the TAI of frame n". **[verified]**
- **RX latency** (`mtl_ptp_read_time() - frame->timestamp`, with media-clock conversion) is duplicated in 4 files (rx_st20p 99-113, rx_st22p 47-61, legacy rx_video 341-355, experimental rx_st20r 172-186). **[verified]**
- **Heavy work in lib callbacks.** Legacy rx_audio does memcmp and memcpy inside `notify_frame_ready` (`legacy/rx_audio_app.c:202-227`), even though the callback contract says tasklet context, non-blocking. **[verified]**
- **Stats handling is inconsistent across media types.**
  - `app_stat` omits tx_st22p, st30p io stats, st40p, anc and fmd.
  - Per-session io_stat calls `*_get_session_stats` per port and then resets once.
  - **[verified]**, read in the code. This matches #1305.

#### FFmpeg plugin

Four demuxers (st20p, st22, st22p, st30p) and five muxers (the same plus `st30p_pcm16`) in libavdevice, packet based (rawvideo / codestream / PCM). No AVFrame, hwframe or ext-frame path. **[verified]** (`ecosystem/ffmpeg_plugin/7.0/*.patch`; `mtl_st20p_rx.c:184-190`).

- **Instance.** A global handle with refcount and mutex (`mtl_common.c:25-28`).
  - A new context must pass `mtl_dev_params_compatible()`, a `memcmp` of the whole `mtl_init_params` apart from the PTP bits (`mtl_common.c:166-181`). Otherwise the open fails with "shared handle configuration mismatch" (`:192`).
  - Every input and output must therefore repeat identical device options.
  - When the refcount reaches 0 it calls `mtl_uninit` (`:229-233`). After that, opening another context in the same process would need a new `mtl_init`, which #1341 reports as broken. **[verified]** code, **[inferred]** consequence.
- **Blocking.** `ST2xP_*_FLAG_BLOCK_GET` everywhere.
  - RX `timeout_s` → `st*_rx_set_block_timeout`. TX has no timeout option.
  - A NULL frame returns `AVERROR(EIO)`, e.g. `mtl_st20p_rx.c:278-280`. Nothing returns `EAGAIN` or `EOF`. **[verified]**
- **Copies.**
  - RX: `av_new_packet` + `mtl_memcpy` + immediate `put_frame` (`mtl_st20p_rx.c:290-299`).
  - TX: `mtl_memcpy(frame->addr[0], pkt->data, …)` (`mtl_st20p_tx.c:196`).
  - Only `addr[0]` is used; planar formats are assumed contiguous.
  - Five `/* todo: zero copy with external frame mode */` comments (`mtl_st20p_rx.c:297`, `mtl_st20p_tx.c:195`, `mtl_st22p_rx.c:352`, `mtl_st22p_tx.c:257`, `mtl_st30p_rx.c:228`). **[verified]**
- **Timestamps.**
  - RX `pkt->pts = frame_counter++` with time_base = 1/fps (`mtl_st20p_rx.c:193,301`). `frame->timestamp` and `rtp_timestamp` are ignored.
  - TX ignores `pkt->pts`, and fps comes from `avg_frame_rate`. **[verified]**
- **Audio re-framing.** st30p TX accumulates arbitrary packet sizes into fixed 10 ms MTL frames (`mtl_st30p_tx.c:162-216`). **[verified]**
- **Options mirror ops fields one-to-one:** `p_port`, `p_sip`, `p_rx_ip`, `udp_port`, `payload_type`, `fb_cnt`, `fps`, `pix_fmt`, `timeout_s`, … (`mtl_common.h:86-368`). **[verified]**
- **GPU direct.** `ops_rx.gpu_context` points at a stack local (`mtl_st20p_rx.c:207,217`). **[verified]**, a probable lifetime bug.

#### GStreamer plugin

- **Structure.** Six elements.
  - Sources derive from GstBaseSrc and implement only `start`, `negotiate` and `create`. There is no `stop`, `unlock`/`unlock_stop`, `query`, `decide_allocation` or `set_live`.
  - Sinks replace the pad chain and event functions, so no base-sink sync, preroll, QoS or latency applies. They also force `PLAYING` from inside `start()`, e.g. `gst_mtl_st20p_tx.c:225,246`.
  - **[verified]**, read with line cites.
- **Instance.** A global refcounted handle in the shared `libgstmtl_common` (`gst_mtl_common.c:12-18,669-771`). The second element's device args are silently ignored. **[verified]**
- **st20p rx.**
  - The `zero_copy` condition is `transport_fmt != st_frame_fmt_to_transport(output_fmt)` (`gst_mtl_st20p_rx.c:274` at HEAD). For the two accepted formats (v210, I422_10LE) that function returns `ST20_FMT_MAX` (`lib/src/st2110/st_fmt.c:899`), so the condition is always true and the memcpy path is dead.
  - `query_ext_frame` allocates a plain `gst_buffer_new_allocate` plus video meta per frame and lets MTL *convert* into it (`:569-614`). **[verified]**
  - The buffer leaks if MTL drops the frame after the query. **[inferred]** from `lib/src/st2110/pipeline/st20_pipeline_rx.c:205-229`, read.
- **st20p tx.**
  - Sets `EXT_FRAME | EXT_FRAME_MANUAL_RELEASE`. For each GstMemory it does `st20p_tx_get_frame` then `st20p_tx_put_ext_frame` (`gst_mtl_st20p_tx.c:370-375,682`).
  - `notify_frame_done` unrefs the GstBuffer via a parent/child refcount (`:547-597`).
  - This is true zero-copy, but only because the conversion path is CPU and needs no IOVA. The passthrough (`derive`) path passes `ext_frame->iova[0]` to `st20_tx_set_ext_frame` (`lib/src/st2110/pipeline/st20_pipeline_tx.c:958-966`), which needs DMA-mapped memory. **[verified]** code, **[inferred]** constraint.
- **Timing.**
  - RX `GST_BUFFER_PTS = frame->timestamp` (raw TAI ns; `gst_mtl_st20p_rx.c:477,513`). There is no base-time or running-time conversion, no GstClock, no latency query and no `set_live`. **[verified]**
  - TX `use-pts-for-pacing` does `frame->timestamp = GST_BUFFER_PTS(buf) += pts_for_pacing_offset` (`gst_mtl_st20p_tx.c:650,718`). This mutates the buffer's PTS, once per GstMemory. **[verified]**
- **Blocking and teardown.** BLOCK_GET plus a retry loop, then EOS after about retry × 1 s (`gst_mtl_st20p_rx.c:463-473`). There is no `unlock` vfunc and no `st20p_rx_wake_block` call anywhere in the plugin (grep), so flushing and state changes wait for the timeout. **[verified]**
  - HEAD holds no lock across the blocking get. The working tree adds `GST_OBJECT_LOCK` around it (`git diff`); that is not a HEAD fact.
- **st40p rx polls** with `g_usleep(1ms)`. Its comment reads "Use non-blocking mode – blocking causes preroll timeout" (`gst_mtl_st40p_rx.c:499`). **[verified]**
- **Audio re-framing.** st30p TX packs arbitrary buffers into fixed frames. Its comment reads "This could be done with GstAdapter" (`gst_mtl_st30p_tx.c:544`). **[verified]**

#### OBS

- A per-source `mtl_init` with no sharing (`ecosystem/obs_mtl/linux-mtl/mtl-input.c:317-338`). **[verified]**
- Callback plus condvar loop (`:86-124`). There is a stray `pthread_mutex_unlock` on a mutex that is not held (`:125`). **[verified]**
- `out.data[i] = frame->addr[0] + plane_offsets[i]` with hard-coded linesizes, and `out.timestamp = frame->timestamp` (`:127-129`). **[verified]**
- The TX (output) plugin was never finished (`mtl-output.c:7`, `#if TODO_OUTPUT`). **[verified]**

#### MTL_with_MXL (POC)

- Uses low-level `st20_rx` (frame level) with dynamic `query_ext_frame`. MXL grains are mapped as one coalesced `mtl_dma_map` region "to avoid exhausting DPDK's memseg list". **[verified]**, read in `poc/src/sender/mxl_bridge.c:213-262`.
- **Key fix** (`mxl_bridge.c:489-499`): call `st20_rx_put_framebuff` *immediately after dequeue*, while the grain is still in use for RDMA. This relies on an undocumented guarantee that "MTL does not touch the ext_frame buffer after put_framebuff". Holding MTL slots until RDMA completion caused "slot get frame fail". **[verified]**
  - Consequence: the app wants to decouple *slot* lifetime from *buffer* lifetime, which is exactly the review §7 distinction between buffer and submission. **[inferred]**
- **Incomplete frames.** The app must release the grain it claimed in `query_ext_frame` itself, "otherwise it leaks permanently" (`poc/src/sender/mtl_rx.c:25-36`). Dynamic ext frames also require `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`. **[verified]**
- **Patches.** Needs an MTL patch that disables `--remap-lcore-ids` for multi-process use, plus hand-tuned `data_quota_mbs_per_sch` (`ecosystem/MTL_with_MXL/README.md:258-276`, `patches/`). **[verified]**, read in the code.

#### Python (SWIG)

- `python/swig/pymtl.i:15-32` wraps only `mtl_api.h`, `st_api.h`, `st20_api.h`, `st_pipeline_api.h`, `st_convert_api.h` and `st_convert_internal.h`. That means no audio or ANC, and an internal test header is exposed. **[verified]**
- There is no `-threads` or `%thread`, so a blocking `get_frame` holds the GIL for up to the timeout. **[verified]** (grep), **[inferred]** effect.
- The C headers carry Python-only helpers (`mtl_para_*_set` at `mtl_api.h:879-905`; `st_frame_addr_cpuva` at `st_pipeline_api.h:2426-2436`). They exist because SWIG cannot handle fixed arrays or pointers ergonomically. **[verified]**

#### Rust (`imtl-rs`)

- **Dangling `priv`.**
  - `ops.priv_ = &self as *const VideoTx as *mut c_void` is taken inside a by-value `create(mut self)`, and `self` is later moved out as the return value (`rust/src/imtl/video.rs:482-484,535-538`; same in audio). **[verified]** that the pointer is taken from a local.
  - The C callbacks then dereference a stale address. **[inferred]** undefined behaviour.
- **Lifetime and ownership.** `Mtl` derives `Clone` and implements `Drop` (double `mtl_uninit`). Sessions do not borrow `Mtl`. The ring is mutated from lcore threads with no synchronisation. **[verified]**, read in the code.
- **Brittle raw bindings.** `imtl-sys/examples/no_std.rs` builds `mtl_init_params` as a full struct literal, which breaks whenever a field is added. **[verified]**

#### plugins/

- The plugin ABI: `st_plugin_get_meta` / `create` / `free` are loaded via `dlopen`. `create_session` returns a request with `resp_flag = BLOCK_GET`, and the plugin runs its own pthread per session doing `st22_encoder_get_frame` → encode → `put_frame` (`plugins/sample/st22_plugin_sample.c:39-116`). **[verified]**
- The same thread-per-session plus blocking-get pattern as apps, with the same bugs (plain `bool stop`; `pthread_create` checked with `< 0`). **[verified]**
- `ST_PLUGIN_MAGIC` shifts by 16 where 8 is intended (`include/st_pipeline_api.h:71`). It is harmless only because both sides use the same macro. **[verified]**

#### KahawaiTest

- **Callbacks before create returns.** Callbacks guard with `if (!ctx->handle) return -EIO; /* not ready */` 37 times in 11 files (grep at HEAD), because they can fire before `*_create()` returns the handle. The guard itself is a data race on a plain `void*`. **[verified]**
- **Sleeps instead of readiness.** About 110 `sleep`/`usleep` calls stand in for "session is live" or "all frames done". For example, `st20p_test.cpp:1211` reads `st_usleep(1000*100); /* wait all fb done */`. `st22p_test.cpp:1664-1678` pumps frames for up to 15 s because "the first frames race session negotiation / link-up". **[verified]**, read in the code.
- **Ops initialisation.** `*_ops_init()` helpers in every file repeat the port/IP block, and repeat it again by hand for `MTL_SESSION_PORT_R` (`st20p_test.cpp:2093-2101,2142-2150`). **[verified]**
- **Scope of an API change.** About 306 distinct public functions are referenced, 70 of them colour conversions in `cvt_test.cpp`, across 558 TEST macros. This is the single biggest migration cost. **[verified]**, counted in the code.

### 1.3 Downstream users on GitHub

`gh search code` works with the configured token.

- `st20p_tx_create`: 30 hits.
- `st20p_rx_create`, `st30p_tx_create`, `st22p_tx_create` and `st20_tx_create` were searched too, and non-MTL repos were filtered.
- `gh search repos "Media-Transport-Library"` returns only MTL itself and unrelated projects. There are no public forks with significant activity in the top 20.
- **[verified]**

| Repository (public) | What it uses | Relevance |
|---|---|---|
| [bobistudio-plugin-2110_io][bobi] (2026-06, active) | 4,238-line C engine `mtl_rx.c`: st20p rx/tx, raw `st20_tx`/`st20_rx` in **slice mode**, st30p, st40p, `mtl_ptp_read_time_raw`, port stats; flags BLOCK_GET, TIMING_PARSER_META, RECEIVE_INCOMPLETE_FRAME, PHC | **Most demanding public user.** Ships **19 source patches** to libmtl/DPDK (`docker/patch_*.py`), §3.3 |
| [directview-led-software-toolkit][dvled] | Vendors the FFmpeg plugin `mtl_dev_get` and adds `mtl_tx_init()`, because otherwise "DPDK EAL is initialised by the first `avformat_write_header()` call with only one port" (`include/mtl/mtl_tx.h` header comment) | Instance and port-set pain |
| [NUDA9A/ST2110-OBS-PLUGIN](https://github.com/NUDA9A/ST2110-OBS-PLUGIN) | A C++ worker. st20p rx with BLOCK_GET, `st20p_rx_wake_block` on stop, per-plane memcpy into shared memory, copies every field of `st20_rx_user_stats` into its own struct (`apps/st2110_mtl_rx_worker/mtl_video_rx_session.cpp:511-560`) | Wrapper boilerplate |
| [CorangesS/GPT_mtl_encode_sdk](https://github.com/CorangesS/GPT_mtl_encode_sdk) | st20p, st30p, st30 raw. The comment reads "no blocking inside MTL callback threads", then it *polls* a ready queue with `sleep_for(1ms)`. Falls back to synthetic timestamps when `mtl_ptp_read_time` returns 0 (`mtl_sdk/src/mtl_backend_mtl.cpp:118-147`) | Bridge and timestamp boilerplate |

**Private consumers.** The token also surfaced about 10 private repositories (Intel innersource and private OpenVisualCloud): media proxy / mesh (st20/st22/st30), NMOS integration, a vendor origin service, an LED-wall receiver, validation frameworks. **Not inspected**; names withheld. **[verified]** they exist; **[unknown]** how they use the API (see open question 10).

[bobi]: https://github.com/bob-integration/bobistudio-plugin-2110_io
[dvled]: https://github.com/OpenVisualCloud/directview-led-software-toolkit

---

## 2. Boilerplate that every consumer repeats (candidates to absorb)

| # | Boilerplate | Who repeats it (evidence) | What the new API could absorb |
|---|---|---|---|
| B1 | **Callback-to-condvar wake**: callback signals a cond; thread re-polls `get_frame`, waits on NULL | ~10 samples (`tx_st20_pipeline_ext_frame_sample.c:110-146`), MSVC, OBS `mtl-input.c:86-124`, RxTxApp `rx_st40p_app.c:22-55`, KahawaiTest `st20p_test.cpp:209-214`, convert plugin, Rust, CorangesS. **[verified]** | Correct-by-construction `acquire(timeout)` + pollable eventfd |
| B2 | **Stop protocol**: set `stop`, `*_wake_block()`, `pthread_join` | Every BLOCK_GET consumer: 12 samples, RxTxApp `tx_st20p_app.c:176-184`, KahawaiTest `st20p_test.cpp:1206-1209`, plugins `st22_plugin_sample.c:101-116`, NUDA9A. GStreamer does **not** do it and waits out the timeout. **[verified]** | `session_stop()` / cancel that makes a blocked acquire return a distinct `-ECANCELED` |
| B3 | **NULL = "timeout, try again"** | All consumers. FFmpeg converts it to EIO (ends the run), GStreamer to EOS after N retries, samples to `warn` + `continue`. **[verified]** | Return an error code (`-ETIMEDOUT` / `-EAGAIN` / `-ECANCELED` / `-EIO` fatal) and a session state |
| B4 | **Port/IP block**: dip/ip, port name, udp_port, PT, again for port R, optional `USER_P_MAC` | ~30 samples (`tx_st20_pipeline_sample.c:186-203`), ~16 RxTxApp files, KahawaiTest `*_ops_init`, FFmpeg `mtl_common.c:251-315`, GStreamer, NUDA9A, CorangesS; see #687. **[verified]** | Reusable flow-endpoint descriptor; P/R as an array; default interface = instance port |
| B5 | **Process-wide instance singleton** with refcount and "compatible params" check | FFmpeg `mtl_common.c:25-28,166-239`, GStreamer `gst_mtl_common.c:12-18,669-771`, directview-led `mtl_tx_init()`. Root cause: no re-init (#1341, open). **[verified]** | Library-owned refcounted default instance, or add ports/queues after init; at minimum a working uninit → init |
| B6 | **Format / fps / size mapping tables** | FFmpeg AV_PIX_FMT↔`st_frame_fmt`↔`st20_fmt` (~10 rows), GStreamer (2 formats), OBS, Rust `Fps::to_float` (wrong for 100/119.88/120), bobi `fps_to_rational`; all audio users call `st30_calculate_framebuff_size(…, 10 ms)`. **[verified]** | Rational fps; size/linesize query before create; published pix-fmt mapping; audio duration in time units |
| B7 | **TAI-for-frame-n computation** for user pacing | RxTxApp `st_app_user_time()` (`rxtx_app.c:673-703`), GStreamer PTS + offset, bobi epoch shift, #1185 and #1170 workarounds. **[verified]** | Session helpers: `next_epoch_tai()`, `frame_index ↔ TAI ↔ RTP` conversions, and a declared TROFF / epoch offset |
| B8 | **RX latency** = PTP now − frame timestamp (media-clock conversion) | 4 RxTxApp files, bobi `lat_sum`, CorangesS. **[verified]** | Put receive timestamp, RTP-derived TAI and the latency stat in the frame metadata and stats |
| B9 | **Stats copying and reset** | NUDA9A copies every field. RxTxApp gets and resets per port. Stats differ by media type (#1305, #1157, #1560). **[verified]** | One stats struct shape for all essences; a snapshot-and-delta API instead of get+reset |
| B10 | **Callback-before-handle guard** `if (!ctx->handle) return -EIO` | KahawaiTest (37 in 11 files), OBS `mtl-input.c:89`, fwd samples (`if (!s->ready)`). **[verified]** | Pass the session handle to every callback, or guarantee no callbacks before create returns or before explicit start |
| B11 | **Audio re-framing** of arbitrary-size input into fixed-ptime MTL frames | FFmpeg `mtl_st30p_tx.c:162-216`, GStreamer `gst_mtl_st30p_tx.c:494-578`. **[verified]** | A byte/sample-stream write for st30 TX, with the library doing the packetisation |
| B12 | **Media-file loaders** (mmap → `mtl_hp_malloc` → memcpy) | 5 samples, 4 RxTxApp files. **[verified]** | Not an API concern; keep in a sample utility library |

---

## 3. Pain points and workarounds

### 3.1 From in-tree code

- **Zero-copy is hard to reach.**
  - It needs `get_frame` then `put_ext_frame` (the slot must be in `ST20P_TX_FRAME_IN_USER`; `lib/src/st2110/pipeline/st20_pipeline_tx.c:945-950`). **[verified]**
  - Passthrough needs IOVA. **[verified]**
  - RX dynamic ext frames need `RECEIVE_INCOMPLETE_FRAME`. **[verified]**
  - As a result FFmpeg never did zero-copy (5 TODOs), RxTxApp has no ext-frame mode (an external user complained in #1321), and GStreamer only uses the conversion path. **[verified]**
- **Callback context.** Callbacks run on the scheduler lcore. Consumers that do real work in them (legacy rx_audio memcmp, samples doing mmap in `notify_detected`) are violating the contract, and nothing enforces it. **[verified]**
- **Flags.** More than 20 per-type flag bits exist, with separate namespaces that share numeric values, and they get misused (st30 rx sample; FFmpeg DMA flag). **[verified]**
- **Frame struct semantics are implicit.**
  - `timestamp` + `tfmt` + `epoch` + `rtp_timestamp` + `receive_timestamp` coexist in `struct st_frame` (`include/st_pipeline_api.h:288-323`).
  - GStreamer and OBS pass `timestamp` as if it were the capture time. FFmpeg ignores it. **[verified]**
  - It is **[unknown]** which field a consumer should use for A/V sync on RX; #1204 shows audio used the last packet's timestamp until it was fixed.
- **Teardown.**
  - No flush or drain before free existed until #870 (FFmpeg lost the last frames).
  - DMA memory is freed before session free in a sample.
  - GStreamer has no `stop` vfunc; teardown happens in `finalize`.
  - **[verified]**

### 3.2 From GitHub issues (OpenVisualCloud/Media-Transport-Library)

| Theme | Issues | Takeaway for the API |
|---|---|---|
| User pacing / timestamp semantics | [#1211] (RTP offset with USER_PACING; "Solution 2": pass **both** TAI and RTP), [#1185] (st30 ts rounded to packet), [#1170] ("any timestamp (with discontinuity)"), [#1208], [#1337] (wrong rtp_timestamp in frame_done), [#1325], [#1318] | Independent `tx_time` (TAI) and `rtp_timestamp` per frame, one derivation rule. **[inferred]** |
| Late-frame policy | [#1276] (100 ms pause → unrecoverable "error user timestamp"), [#1424], [#1357] (DROP_WHEN_LATE dropped the wrong frame, or never), [#1370] (8 sessions not recovering), [#1378] | Explicit late-policy enum (drop / send-asap / shift) with guaranteed notification and recovery |
| Blocking semantics | [#1678] (`get_frame` returned after about 500 ns instead of 1 s; a regression) | The timeout contract needs a test and a distinct return code |
| Ext-frame completion | [#1147] (`notify_frame_done` called twice with ext frames) | Completion must be exactly-once, and tested |
| Stats | [#1305] (st30p/st40 stats incomplete), [#1157] (err_packets nonsense with 2022-7), [#1560] (video lost_packets stuck) | One stats model across essences; per-port redundancy semantics defined |
| Lifecycle | [#1341] (**open**: init after uninit fails; long-running encoder reconfigured over HTTP), [#1139] (session stop/start loses signal), [#870] (last frames lost at close; asked for "wait until flushed"), [#1620] (**open**: other audio/anc sessions lose packets on connect/disconnect) | Reconfigurable instance, explicit drain, session create/free isolated from siblings |
| Redundancy / link | [#1222] (**open**: TX 2022-7 stops on one-leg link loss), [#1242], [#1321] | Link and leg state as observable events; the hitless contract documented |
| Config / ergonomics | [#948] (st30p `fifo_size` ignored), [#687] (misleading `sip_addr`), [#657] (log callback; done), [#1239] (**open**: `mcast_sip_addr` ineffective), [#1179] (**open**: `lcores`/`main_lcore` undocumented), [#1176] (stale lcore shm; asks to disable coordination) | Reject unknown/unused fields; fewer, better-named fields; opt-out of cross-process coordination |
| Back-pressure diagnosis | [#1517] ("slot get frame fail" was app-side back-pressure) | The app should get a "frame dropped: no free buffer" event, not only a log line |

**Who files these.** Timing, redundancy and stats issues come mostly from three reporters (cwhite102, DianaEs2, ZeroDawn); one comment says "We use external frames, user pacing, user timestamping" with P+R ports ([#1157]). **[verified]** That combination (ext frames + user pacing + user timestamps + 2022-7) is barely exercised in-tree; RxTxApp has no ext frames. **[verified]**

[#1211]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1211
[#1185]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1185
[#1170]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1170
[#1208]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1208
[#1337]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1337
[#1325]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1325
[#1318]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1318
[#1276]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1276
[#1424]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1424
[#1357]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1357
[#1370]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1370
[#1378]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1378
[#1678]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1678
[#1147]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1147
[#1305]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1305
[#1157]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1157
[#1560]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1560
[#1341]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1341
[#1139]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1139
[#870]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/870
[#1620]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1620
[#1222]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1222
[#1242]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1242
[#1321]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1321
[#948]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/948
[#687]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/687
[#657]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/657
[#1239]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1239
[#1179]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1179
[#1176]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1176
[#1517]: https://github.com/OpenVisualCloud/Media-Transport-Library/issues/1517

### 3.3 From a public downstream engine (bobi.studio)

`bobistudio-plugin-2110_io/docker/patch_*.py` patch MTL and DPDK source before build. The headers were read with `gh api` on 2026-09-29. **[verified]** that the files exist and say what is listed below; each patch's technical claim is the author's and is **[unknown]** as to correctness.

| Patch | Claimed problem | API-relevant lesson |
|---|---|---|
| `patch_epoch_shift.py` | TX can only start at an epoch boundary, so a frame ready about 5 ms after the epoch pays +1 frame of latency. They add a TROFF shift and encode it as a **negative `rtp_timestamp_delta_us`** "to avoid a new API field" | TROFF / epoch offset belongs in the API explicitly |
| `patch_ptp_stable_getter.py`, `patch_ptp_offset_getter.py`, `patch_ptp_gm_export.py` | No public getter for PTP lock state, offset, or grandmaster identity/domain (needed for SDP `a=ts-refclk`). TX start blocks in `mt_ptp_wait_stable` (up to 180 s), and the app's watchdog cannot tell that apart from a hang | A public PTP status API and session state ("waiting for PTP") |
| `patch_tx_frame_inflight_reclaim.py`, `patch_tx_builder_famine_recovery.py` + app watchdog (`mtl_rx.c:2445-2470`) | Frames abandoned without `notify_frame_done` leave a slot stuck forever, then `get_next_frame` keeps returning -EBUSY and output drops to 0 fps; the session "dies in silence" | Exactly-once completion **with a status**, and a session fatal/error event |
| `patch_rx_resetting_guard.py`, `patch_tx_hang_resetting_guard.py`, `patch_tm_hierarchy.py`, `patch_ice_tm_move_retry.py` | Creating one RL-paced TX session commits the TM hierarchy, which stops the whole port and disturbs or kills *other* live sessions. Also an 8-queue RL limit | Session create must not disturb siblings, or the API must say it does. Relates to #1620 |
| `patch_afxdp_tx_link_drop.py` | On AF_XDP, a dead link on one 2022-7 leg freezes the whole redundant session | Per-leg failure isolation. Relates to #1222 |
| `patch_st40_afxdp_port.py` | AF_XDP ANC packets carry `mbuf->port = UINT16_MAX`, so every ANC packet is dropped | Backend-specific bug, not API |
| `patch_icmp_echo.py`, `patch_igmp_router_alert.py`, `patch_ptp_mcast_flow.py`, `patch_dev_link_wait.py`, `patch_ptp_adjust_freq.py` | ICMP echo, IGMP Router Alert, PTP multicast flow rule, link-wait budget, PHC frequency servo | Control-plane gaps, mostly outside the session API |

The same engine documents two scheduling pains in `mtl_rx.c:560-600` **[verified]**:

- **FIFO serving.** In slice mode the library serves the *oldest* ready framebuffer, so a fresher one waits two epochs. They added `serve_newest`.
- **Phase lock.** The library solicits a frame once per epoch, and the source-paced producer settles into a fixed phase relationship with it (about 27 ms wait). They added `publish_lead_us`.

Both point to an API need: tell the app *when* the next frame will be fetched, and/or let the app choose latest-wins over FIFO. **[inferred]**

---

## 4. What FFmpeg and GStreamer specifically need

### 4.1 Today vs. ideal

| Need | FFmpeg today | GStreamer today | What an ideal MTL API would give them |
|---|---|---|---|
| **RX zero-copy** | Copies into `av_new_packet` **[verified]** | st20p: MTL converts into a per-frame `gst_buffer_new_allocate`; audio/ANC copy | Lend buffers: the app holds them across threads and releases out of order (`av_buffer_create` free_cb; GstMemory dispose), with a max-held count and starvation policy. **[inferred]** Cross-thread out-of-order `put_frame` today: **[unknown]** |
| **TX zero-copy** | Copies (`mtl_st20p_tx.c:196`) | st20p: `put_ext_frame` per GstMemory, CPU-conversion path only | (a) **Import**: register foreign memory once (review §7 memory region) so passthrough is DMA-able. (b) **Export**: an MTL pool for GStreamer `propose_allocation` / an FFmpeg `get_buffer2`-style pool. One acquire-then-submit instead of get + put_ext |
| **Completion** | n/a (copy) | Parent/child refcount across GstMemories (`gst_mtl_st20p_tx.c:107-118,547-597`) | Exactly-once completion carrying the app's `opaque`, including on drop and teardown |
| **Blocking and cancel** | BLOCK_GET; NULL → EIO | BLOCK_GET; no `unlock`; flush waits for the timeout | `acquire(timeout)` with `-EAGAIN` / `-ETIMEDOUT` / `-ECANCELED`, plus `session_cancel()` for `GstBaseSrc::unlock`. A pollable fd would let a GstPushSrc or FFmpeg non-blocking read (`AVFMT_FLAG_NONBLOCK` → `EAGAIN`) integrate cleanly |
| **Clock / PTS** | pts = frame counter | PTS = raw TAI ns; no GstClock, live, or latency | (a) Per-frame `tai`, `rtp_timestamp`, `receive_timestamp` with documented meaning. (b) PTP read usable for a GstClock subclass or FFmpeg `start_time_realtime`. (c) RTP ↔ TAI helpers with wrap handling |
| **TX timing** | Ignores pts; fps from stream | `use-pts-for-pacing` requires PTS already in TAI (README 157-184) | `tx_time` (TAI) per frame, with a helper from running time to TAI given a base, and a late-policy enum. The late notification maps to a GStreamer QoS event, and `drop` maps to FFmpeg dropped-frame stats |
| **Latency query** | n/a | Not answered | A session query: min latency = pipeline depth × frame period (+ TROFF), max = framebuff_cnt × period |
| **Instance** | memcmp-compatible singleton; re-init broken | Singleton; the second element's dev args are ignored | A refcounted default instance in the library, or add-port-at-runtime. GStreamer and FFmpeg both load several independent elements or contexts into one process |
| **Caps / format negotiation** | Hand table; YUV420P is MTL-to-MTL only | Two formats; property-driven caps | A query of supported (transport_fmt ↔ frame_fmt) conversions and per-plane linesize and size **before** create, so `get_caps` / `negotiate` can be implemented truthfully |
| **Audio** | Re-frames to 10 ms | Re-frames; "could be done with GstAdapter" | A sample-count-based write/read API, or at least `frame_duration` in time units |

### 4.2 Constraints the new API must respect (from these frameworks)

- **Framework threads.**
  - GStreamer streaming threads may block, but must be unblockable, so `unlock` must be supported. **[inferred]**
  - FFmpeg `read_packet` may block, but `AVIOInterruptCB` is the expected cancel path. The plugin does not use it today. **[verified]** by grep: no `interrupt_callback`.
- **Buffer holding.** Both frameworks may hold buffers for many frames (queues, encoders), so an RX pool must be sized, or must degrade predictably. The MXL POC shows the failure mode: slot starvation leads to "slot get frame fail". **[verified]**
- **IOVA.** Framework memory is not hugepage. Passthrough zero-copy needs either IOVA-as-VA plus `mtl_dma_map` of framework memory, which the MXL POC warns can exhaust the memseg list, or an MTL-provided pool. **[verified]** that the MXL warning exists; **[inferred]** that it applies to framework pools.

---

## 5. Migration impact

### 5.1 Size by consumer

| Consumer | Session call sites | Effort if the pipeline API is replaced with no shim | Notes |
|---|---|---|---|
| KahawaiTest | 752 in 31 files, plus about 306 distinct functions | **Very large.** Weeks of work; tests are also the regression net for the migration itself | Keep the old API alive during the transition so the old tests keep validating the shim. **[inferred]** |
| app/sample | 260 in 35 files | Medium; mostly mechanical, and many samples could be dropped or merged | A good place for the new APIs canonical examples |
| RxTxApp | 156 in 19 files, plus a 3.5 kLOC JSON parser | Medium to large; the acceptance pytest suite depends on it | The JSON schema could stay as is, with only the ops-building code changing. **[inferred]** |
| FFmpeg plugin | 61 in 6 files | Small (about 3 kLOC total); a rewrite would *gain* zero-copy | There are external forks/vendors (directview-led). **[verified]** |
| GStreamer plugin | 58 in 8 files | Small to medium; a rewrite is an opportunity to add clock, latency, unlock and bufferpool | |
| OBS | 10 in 2 files | Trivial | Arguably unmaintained (tx never done) |
| MXL POC | 52 in 8 files | Medium; it depends on low-level `st20_rx` + dynamic ext frames + tasklet callbacks | It would stress any new buffer model; a good validation target |
| Python | 48 in 7 files, plus the `.i` | Small; SWIG regenerates, but the Python-only helpers must be carried over | A smaller opaque-handle C API with setters would make SWIG output better. **[inferred]** |
| Rust | 36 in 2 files | Small; the safe wrapper has UB and needs a rewrite anyway | |
| plugins (st22/convert ABI) | 0 session calls | None, **if** the plugin ABI (`st_plugin_*`, `st22_encoder_*`) is out of scope | Needs a decision (open question 8) |
| External public users | bobi (about 4 kLOC engine, including slice mode), NUDA9A, CorangesS, directview-led | Unknown; they are pinned to specific MTL versions and patch sets | bobi patches source; its migration cost is dominated by its patches, not the API. **[inferred]** |

### 5.2 Is a compatibility shim feasible?

**Option A: old pipeline API implemented on top of the new unified API.** Feasible for the common path. **[inferred]**

- `st2Xp_{tx,rx}_create(ops)` translates to new-session create. `get_frame` / `put_frame` map to acquire / submit (or release).
- BLOCK_GET maps to acquire with a timeout, `wake_block` to cancel, and `notify_frame_available` to an internal wake hook.
- Ext frames map to import + submit, and `query_ext_frame` to the RX provisioning callback.
- `struct st_frame` must stay binary-identical, because consumers read `addr[]`, `linesize[]`, `timestamp`, `tfmt`, `rtp_timestamp`, `user_meta`, `tp[]`, `receive_timestamp`, `opaque`, `status`, `second_field` directly (FFmpeg, GStreamer, OBS, RxTxApp, KahawaiTest). **[verified]**
  - The shim therefore needs a per-frame adapter struct: one copy of metadata, not of payload. **[inferred]**
- Flag bits need a translation table. Some flags would lose meaning (`DISABLE_BULK`, `STATIC_PAD`, `FORCE_NUMA`) or keep their old, bug-prone semantics.

**Option A is hard for the session and low-level layers.** **[inferred]**

- `get_next_frame` / `notify_frame_done` / `query_frame_lines_ready` / `notify_rtp_ready` / `notify_slice_ready` are *called on the tasklet* and are expected to be non-blocking and immediate.
- If the new API does not expose tasklet-context callbacks, a shim must add a thread hop. That changes latency, which is exactly what slice-mode users (bobi) and zero-copy forwarders (MXL, fwd) chose these APIs for.
- RTP-level (`get_mbuf`/`put_mbuf`) exposes `rte_mbuf`-backed buffers and has no natural mapping to a buffer/submission model.
- Recommendation: keep these as legacy headers backed by the existing implementation for a deprecation period, rather than shimming them. **[inferred]**

**Option B: new API on top of old.** This is useful only as a prototype. It cannot fix the contracts consumers are asking for: exactly-once completion, cancel, error codes, instance re-init, explicit TAI+RTP. Those require changes below the pipeline layer. **[inferred]**

**Language bindings.**

- SWIG (Python) and bindgen (Rust) regenerate from headers automatically, but both suffer from large by-value structs with fixed arrays. Examples: Python needs `mtl_para_*_set` helpers, and the Rust `no_std` example breaks whenever a field is added.
- An opaque-handle + setter / `struct_size`-versioned design helps both. The review §9 item 27 already calls for structure-size compatibility.
- **[verified]** for the symptoms, **[inferred]** for the remedy.

---

## 6. Open questions for the maintainer

1. **Callback execution context.** Should the new API still offer callbacks that run on the scheduler tasklet?
   - Why it matters: every thread-based consumer wraps them into condvars, often wrongly (B1). The low-latency consumers (MXL bridge, fwd zero-copy, bobi slice mode) depend on tasklet-context calls for immediacy.
   - Options: (a) tasklet callbacks kept but marked "expert", with a checked contract; (b) no user code on tasklets, only acquire/submit plus a pollable wait object; (c) both, with (b) as the default and documented.
2. **Pollable wait object.** Should sessions expose an fd (eventfd) or a wait-set, in addition to blocking acquire?
   - Why it matters: GStreamer `unlock`, FFmpeg non-blocking reads / `AVIOInterruptCB`, async Rust and Python asyncio all integrate with fds rather than callbacks. Today the GStreamer st40p source polls with a 1 ms sleep.
   - Options: (a) eventfd per session; (b) one per instance plus a ready-list; (c) none, blocking acquire plus cancel only.
3. **Instance model and re-init.** Should the library own a refcounted process-wide instance, and must `mtl_uninit` → `mtl_init` work (#1341)?
   - Why it matters: FFmpeg, GStreamer and two external projects re-implement a fragile singleton. FFmpeg's memcmp-compatibility rule and GStreamer's "ignore the second element's ports" are both user-hostile.
   - Options: (a) keep explicit handles but support re-init; (b) add `mtl_instance_get_default()` with refcount plus add-port-at-runtime; (c) document "one init per process" and provide the refcount helper only.
4. **Buffer lending and out-of-order release.** Will the new RX acquire allow the app to hold N buffers across threads and release them in any order, and what happens when all buffers are held?
   - Why it matters: this is the only way FFmpeg (AVBufferRef) and GStreamer (GstMemory) get zero-copy RX. The MXL POC hit starvation and relied on an undocumented "MTL doesn't touch ext buffer after put" rule.
   - Options: fixed pool with a drop-newest / drop-oldest / block policy enum; separate "slot" from "buffer" lifetime (review §7).
5. **Framework-memory import vs MTL-pool export for TX.** Which one is first-class?
   - Why it matters: GStreamer `propose_allocation` wants an MTL pool it can offer upstream. FFmpeg and MXL want to register their own memory once. Passthrough needs IOVA either way.
   - Options: (a) export pool only; (b) import registration only; (c) both, as in review §8.1/§8.2.
6. **Timestamp contract.** Should a TX frame carry independent `tx_time` (TAI) and `rtp_timestamp` fields, and should RX document exactly which packet or epoch each field refers to?
   - Why it matters: this is the largest cluster of external bugs (#1211, #1185, #1170, #1204, #1208, #1337). The #1211 reporter explicitly proposed "pass both". bobi encodes TROFF as a negative `rtp_timestamp_delta_us` because there is no field for it.
   - Options: (a) two explicit fields plus a derivation rule when one is absent; (b) keep `tfmt` + flags; (c) add a session-level TROFF / epoch offset in either case.
7. **Error, state and completion semantics.** Should acquire return errno-style codes, and should the session expose a state machine and events (waiting-for-PTP, live, leg down, late, dropped-no-buffer, fatal)?
   - Why it matters: every consumer conflates NULL with timeout. bobi ships watchdogs and patches because sessions die silently or wait for PTP invisibly. #1147 shows completion is not exactly-once.
   - Options: (a) return codes plus a poll-able state getter; (b) an event callback / queue; (c) both. Plus a hard "exactly once, with status" completion guarantee, backed by tests.
8. **Scope of the plugin ABI** (`st_plugin_*`, `st22_encoder/decoder_*`, `st20_converter_*`). Is it part of the redesign?
   - Why it matters: it is a second public ABI with its own thread-per-session blocking-get pattern and the same bugs. The st22p pipeline depends on it; `plugins/` and `st22_avcodec` implement it.
   - Options: freeze it as is; redesign it with the same acquire/submit primitives; or drop it for an in-process codec callback.
9. **Fate of the session-level and RTP/slice APIs.** Should they be legacy (kept, frozen), shimmed, or removed?
   - Why it matters: in-tree they are used by legacy samples, RxTxApp legacy/anc/fmd, KahawaiTest and the MXL POC. Externally, bobi uses slice mode on both TX and RX for latency. Shimming them over a thread-hop API changes their latency.
   - Options: (a) freeze behind `legacy/` headers for N releases; (b) fold slice and RTP into the unified API as "sub-frame" and "packet" granularity; (c) remove after a deprecation window.
10. **Poll private and external consumers before freezing the design?**
    - Why it matters: about 10 private repos (media proxy / mesh, NMOS, a vendor origin service, an LED-wall receiver, validation) plus the external product team behind the #11xx–#13xx issues are the real ABI users. Their usage was not inspected here.
    - Options: send a short survey (which layer, which flags, ext frames?, callbacks vs blocking); or ask for read access so a follow-up note can inspect them.
11. **Configuration ABI style.** Should ops and init params remain large by-value structs, or move to opaque objects with setters (or `struct_size`-versioned structs)?
    - Why it matters: FFmpeg memcmps the whole `mtl_init_params`, Rust builds struct literals, Python needs `*_para_set` helpers, and fields are silently ignored (#948 `fifo_size`).
    - Options: (a) versioned structs with `struct_size` and strict validation of unknown or unused fields; (b) builder/setter functions; (c) key-value (string) config usable directly from GStreamer properties and FFmpeg AVOptions.
12. **Audio I/O granularity.** Should st30 offer a sample- or byte-stream read/write instead of fixed-ptime frames?
    - Why it matters: both framework plugins re-frame audio by hand (B11), and `fifo_size` / latency tuning for audio has been a recurring complaint.
    - Options: (a) a stream API in the library; (b) keep frames and provide a helper; (c) keep as is.
13. **What becomes the canonical in-tree example set?**
    - Why it matters: 33 samples with copy-pasted bugs (wrong pthread checks, unzeroed arrays, wrong-namespace flags) are what users copy. The new APIs samples will be copied the same way.
    - Options: rewrite a small set of about 6 samples (tx/rx × pipeline/zero-copy/user-timing) on the new API and move the rest to `legacy/`, or drop them.
