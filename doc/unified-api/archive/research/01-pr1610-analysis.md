# Research note 01: PR #1610 "New API draft - rebased", complete analysis

| | |
|---|---|
| Topic | What PR #1610 actually implements, which design decisions it makes, which defects are real, what to keep, and whether to rebase it |
| PR head | `pr1610` = `14a1f80c` (16 commits, author "Sakoram", opened 2026-06-10, last update 2026-09-16, state OPEN) |
| Merge-base | `74c9af0e` (2026-06-08) |
| `main` compared | `545a266a` (283 commits ahead of the merge-base) |
| Cross-checked against | `doc/unified-api/research/00-pr1610-design-review.md` (the maintainer's review, cited as "review §N") |
| Date | 2026-09-29 |
| Method | Read every commit message and diff, the full public header, all of `lib/src/new_api/`, all five design docs, the four samples, the RxTxApp migration, the unit tests, and the PR body and single comment. No builds were run. Trial merge by `git merge-tree` only. |

Evidence labels: **[verified]** means I read the code at the named revision and cite `path:line`. **[inferred]** means reasoning from verified code, not executed. **[unknown]** means not established.
Unless stated otherwise, `lib/src/new_api/...` paths and `include/mtl_session_api.h` are at `pr1610`. `@base` means `74c9af0e`, `@main` means `545a266a`.

---

## 0. Summary

- The PR adds a parallel public API (`include/mtl_session_api.h`, 808 lines, 29 functions) and ~3.3 kLOC of implementation in `lib/src/new_api/`. **Only ST20 video TX/RX in frame mode is implemented.** Audio, ancillary, ST22, slice mode, flush, plugin info and queue meta return `-ENOTSUP`, or are silent no-ops. **[verified]**
- It does **not** wrap `st20p`. It wraps the low-level `st20_tx_create`/`st20_rx_create` session layer and re-implements pipeline behavior on top of it: conversion, frame state machine, drop-when-late, blocking get. **[verified]**
  - To do that it reaches into transport internals (`handle_impl->impl`, `st20_frames[]`, `refcnt`, `tv_meta`).
  - Evidence: `mt_session_video_tx.c:943-946`, `:205`, `:384-401`. Commit `8ae7ef6d5` says "zero pipeline-symbol usage".
- **All nine claims of the maintainer's review check out.** The worst one is **stronger than the review states**: on TX, user pacing, user timestamp and `user_meta` never reach the wire at all, because the transport overwrites `tv_meta` from the `meta` that `get_next_frame` leaves untouched. The migrated RxTxApp therefore silently loses `user_pacing` and TX SHA metadata. **[verified]**
- I found further defects the review does not mention:
  - RX "user-owned" does a full-frame copy or color conversion **inside the RX tasklet**.
  - RX VSYNC and RX FORCE_NUMA are never enabled.
  - `compressed = true` silently creates an ST20 session.
  - `event_poll` refuses to deliver already-queued ownership completions after `stop()`.
  - A double `buffer_put` corrupts TX slot state.
  - The zero-copy samples mmap page-cache files and pass them to the NIC with no DMA mapping, then unmap them before `destroy`.
- **Rebase recommendation: do not rebase. Restart.** **[inferred]**
  - Salvage the header vocabulary, the event/eventfd mechanism, the completion-contract wording, the shutdown design doc, the sample and parity-test structure, and the RxTxApp migration shape.
  - Textual conflicts are small: 7 files, all build or test files. **[verified]**
  - The semantic drift is large. `main` has since fixed or redesigned in the pipeline most of what the PR re-implements: FIFO by sequence number, handle guards, blocking waits, drop-when-late `DROPPED` completion, and user-pacing timestamps. **[verified]**

---

## 1. What the API is

### 1.1 Object model

```text
mtl_handle (existing instance)
   |
   +-- mtl_video_session_create(cfg) --+
   +-- mtl_audio_session_create(cfg) --+--> mtl_session_t*  (one opaque type for all media)
   +-- mtl_ancillary_session_create() -+        |
                                                 |  vtable dispatch (internal)
                                                 v
              +---------------------+     +---------------------+
              | video_tx vtable     |     | video_rx vtable     |   audio/anc: create returns
              | st20_tx_create()    |     | st20_rx_create()    |   -ENOTSUP; vtables declared
              | FRAME_LEVEL + cbs   |     | FRAME_LEVEL + cbs   |   extern but never defined
              +---------------------+     +---------------------+
Per session: event ring (64 x mtl_event_t, value-backed) + eventfd,
             buffer-wrapper pool (1 per transport frame), user-buffer ring (32 entries),
             up to 8 "DMA registrations", per-frame TX state array.
```

- `mtl_session_t` is `struct mtl_session_impl` cast directly (`MTL_SESSION_IMPL` = cast, `mt_session.h:328`). It is validated only by a magic number read from the handle memory (`mt_session.h:320-325`). **[verified]**
- `mtl_buffer_t` is a public struct embedded as `pub` in `struct mtl_buffer_impl`. It is recovered with `container_of` (`MTL_BUFFER_IMPL`, `mt_session.h:334-335`). A buffer pointer that the app copied or forged therefore yields a wild pointer. **[verified]**
- `mtl_dma_mem_t` is an opaque `struct mtl_dma_mem_impl {addr,size,iova,hp_mapped}` (`mt_session.h:162-168`). **[verified]**

### 1.2 Condensed header listing (`include/mtl_session_api.h@pr1610`)

```c
/* enums */
mtl_session_dir_t      { MTL_SESSION_TX, MTL_SESSION_RX };                      /* :67 */
mtl_media_type_t       { VIDEO, AUDIO, ANCILLARY, FASTMETA };                   /* :73 */
mtl_buffer_ownership_t { MTL_BUFFER_LIBRARY_OWNED, MTL_BUFFER_USER_OWNED };     /* :81 */
mtl_video_mode_t       { FRAME, SLICE };                                        /* :87 */
mtl_event_type_t       { NONE, BUFFER_READY, BUFFER_DONE, ERROR, VSYNC, FRAME_LATE,
                         FORMAT_DETECTED, TIMING_REPORT, SLICE_READY };         /* :93 */
mtl_frame_status_t     { COMPLETE, INCOMPLETE, CORRUPTED };                     /* :106 */
MTL_SESSION_FLAG_* bits 0..18: EXT_BUFFER, USER_PACING, USER_TIMESTAMP, ENABLE_VSYNC,
  ENABLE_RTCP, FORCE_NUMA, DATA_PATH_ONLY, RECEIVE_INCOMPLETE_FRAME, DMA_OFFLOAD,
  HDR_SPLIT, BLOCK_GET, USER_P_MAC, USER_R_MAC, EXACT_USER_PACING,
  RTP_TIMESTAMP_EPOCH, DISABLE_BULK, STATIC_PAD_P, USE_MULTI_THREADS, DROP_WHEN_LATE /* :113-131 */
MTL_BUF_FLAG_EXT, MTL_BUF_FLAG_INCOMPLETE                                       /* :211-212 */

/* mtl_buffer_t (:157-208) */
data, iova, size, data_size, timestamp, epoch, rtp_timestamp, flags, status,
priv /*DO NOT TOUCH*/, user_data, user_meta, user_meta_size, tfmt,
union { video{planes[],linesize[],width,height,fmt,interlaced,second_field,
              pkts_total,pkts_recv[2]}; audio{...}; ancillary{...}; _reserved[96] };

/* mtl_event_t (:219-275) */
type, status, timestamp, ctx,
union { vsync{epoch,ptp_time}; frame_late{epoch_skipped}; format_detected{...};
        timing{...}; slice{lines_ready,lines_total,buffer}; error{code};
        buffer{buf}; _reserved[64] };

/* configs */
mtl_session_base_config_t { direction, ownership, num_buffers, name, priv, flags,
  socket_id, notify_buffer_ready(), notify_event(), query_ext_frame() };        /* :285 */
mtl_video_config_t { base; union{st_tx_port tx_port; st_rx_port rx_port};
  width,height,fps,interlaced,frame_fmt,transport_fmt,pacing,packing,linesize,
  mode, query_lines_ready(), compressed,codec,codestream_size,plugin_device,
  quality,codec_thread_cnt, tx_dst_mac[][], start_vrx,pad_interval,
  rtp_timestamp_delta_us, rx_burst_size, enable_timing_parser, enable_auto_detect }; /* :316 */
mtl_audio_config_t, mtl_ancillary_config_t                                      /* :425, :450 */

/* functions (29) */
mtl_{video,audio,ancillary}_session_create(mt, cfg, &session);                  /* :472-485 */
mtl_session_start / stop / is_stopped / destroy / get_type;                     /* :498-545 */
mtl_session_buffer_get(s, &buf, timeout_ms) / buffer_put(s, buf);               /* :564, :576 */
mtl_session_buffer_post(s, void* data, size_t size, void* user_ctx);            /* :592 */
mtl_session_buffer_flush(s, timeout_ms);                                        /* :598 */
mtl_session_mem_register(s, addr, size, &dma) / mem_unregister(s, dma);         /* :608, :614 */
mtl_session_event_poll(s, &ev, timeout_ms);                                     /* :628 */
mtl_session_stats_get / stats_reset / get_frame_size;                           /* :648-659 */
mtl_session_io_stats_get(s, void*, size_t) / io_stats_reset;                    /* :671, :677 */
mtl_session_pcap_dump; update_destination; update_source;                       /* :688-707 */
mtl_session_slice_ready / slice_query; get_plugin_info; get_queue_meta;         /* :728-780 */
mtl_session_get_event_fd; mtl_session_set_block_timeout(s, timeout_us);         /* :791, :802 */
```

The header includes `mtl_api.h`, `st_api.h`, `st20_api.h`, `st30_api.h` **and `st_pipeline_api.h`** (`:52-56`). It reuses `st_tx_port`, `st_rx_port`, `st_frame_fmt`, `st22_codec`, `st_plugin_device` and similar types, so it is not a self-contained new surface. **[verified]**

### 1.3 Implementation status per function (video, `pr1610`)

| Function | TX | RX | Evidence |
|---|---|---|---|
| create | yes (`st20_tx_create`) | yes (`st20_rx_create`) | `mt_session.c:57-130` |
| audio/anc create | `-ENOTSUP` | `-ENOTSUP` | `mt_session.c:132-150` |
| start / stop | vtable no-ops; only toggle the `stopped` flag | same | `mt_session_video_tx.c:407-415`, `mt_session_video_rx.c:414-422` |
| buffer_get / put | slot scan + CAS, `usleep(100)` poll | MC ring, `usleep(100)` poll | `tx.c:445-485`, `rx.c:460-495` |
| buffer_post | bind on app thread, backlog ring | enqueue for tasklet | `tx.c:512-561`, `rx.c:508-524` |
| buffer_flush | NULL, so `-ENOTSUP` | NULL | `tx.c:705`, `rx.c:676` |
| mem_register | address/size record, IOVA lookup | same (copy-paste) | `tx.c:567-605`, `rx.c:530-566` |
| event_poll / get_event_fd | ring + eventfd `poll()` | same | `mt_session_video_common.c:174-228` |
| stats / io_stats | yes | yes | `tx.c:630-684`, `rx.c:591-647` |
| pcap_dump | NULL | yes | `rx.c:649-654` |
| update_destination / source | yes / n.a. | n.a. / yes | `tx.c:656`, `rx.c:619` |
| slice_ready / slice_query | `-ENOTSUP` | `-ENOTSUP` | `tx.c:686-692`, `rx.c:656-663` |
| get_plugin_info, get_queue_meta | NULL | NULL | vtables `tx.c:698-722`, `rx.c:669-693` |
| set_block_timeout | returns 0, does nothing | same | `mt_session.c:600-608` |

(`tx.c`/`rx.c` = `mt_session_video_{tx,rx}.c`.) **[verified]**

### 1.4 Lifecycle

```text
create --(st20_*_create: data path is LIVE immediately)--> CREATED
start  : clear 'stopped', state=STARTED, vt->start (no-op)
stop   : state=STOPPED, stopped=1 (release), write(eventfd), vt->stop (no-op)
destroy: vt->destroy (st20_*_free, free ctx) -> mtl_session_free (rings, eventfd, wrappers, magic=0, rte_free)
```

- **Auto-start.** Frames flow from create; `start()` gates nothing. The PR documents this itself (`doc/new_API/CURRENT_STATE.md:212-216`). **[verified]**
- **`stop()` does not stop the data path.** TX keeps transmitting READY slots and RX keeps filling the ready ring until it overflows. Only app-facing calls return `-EAGAIN` (`mt_session.c:182-202`, `mt_session_video_common.c:178`). **[verified]**
- If `vt->start` failed, the state would already be STARTED (`mt_session.c:168-179`), and a later `start()` returns 0 at `:164`. Today it is latent only, because `vt->start` cannot fail. **[verified]**
- `destroy` has no guard. `MTL_SESSION_VALID` reads `magic` from memory that may already be freed (`mt_session.c:49-50`, `:215`). **[verified]**

### 1.5 Threading model (as implemented)

| Call | Concurrency | Evidence |
|---|---|---|
| TX `buffer_get` | multi-thread (CAS FREE to APP_OWNED) | `tx.c:332-362` |
| RX `buffer_get` | multi-consumer ring | `rx.c:353-358`, ring flags `rx.c:707` |
| TX `buffer_post` | **single producer only**, by comment and not enforced | `tx.c:501-503` |
| `event_poll` | **single consumer only** (`RING_F_SC_DEQ`), not enforced | `mt_session_event.c:34-35`, `common.c:176-177` |
| `notify_buffer_ready` user callback | runs on the tasklet inside `event_post` | `mt_session_event.c:101-103` |
| `query_ext_frame` user callback (RX) | runs on the tasklet | `rx.c:204-226` |
| `stop()` | claimed async-signal-safe | `mt_session_api.h:510`. In release builds it does atomics and `write()`. `dbg()` compiles out unless `DEBUG` (`lib/src/mt_log.h:43-52@base`). **[verified]** |

### 1.6 Event model

- Producer: transport callbacks such as `notify_frame_done`, `notify_frame_late`, `notify_frame_ready`, `notify_event(VSYNC)` and `notify_detected`. They copy an `mtl_event_t` into a 64-entry `rte_ring_create_elem` ring. On a full ring they drop the event and bump `events_dropped`, then `write()` to a non-blocking eventfd (`mt_session_event.c:19, 34-35, 82-106`). **[verified]**
- Consumer: `event_poll` dequeues. With a timeout it `poll()`s the eventfd, and falls back to `usleep(100)` if there is no eventfd (`mt_session_video_common.c:174-224`). **[verified]**
- Events actually posted:
  - `BUFFER_READY`: RX only.
  - `BUFFER_DONE`: user-owned TX only.
  - `VSYNC`: effectively TX only (see D12).
  - `FRAME_LATE`: TX, for both transport-late and drop-when-late.
  - `FORMAT_DETECTED`: RX.
- Never posted: `ERROR`, `TIMING_REPORT`, `SLICE_READY` (grep of `lib/src/new_api` at `pr1610`, and `CURRENT_STATE.md:286-289`). **[verified]**
- `base.notify_event` is never stored or called. Only `notify_buffer_ready` is wired (`mt_session.c:78-79`). `events_dropped` is not exposed. **[verified]**

### 1.7 How it maps onto existing code

| New API concept | Backed by | Evidence |
|---|---|---|
| Video TX session | `st20_tx_create`, `ST20_TYPE_FRAME_LEVEL`, own `get_next_frame`/`notify_frame_done`/`notify_frame_late` | `tx.c:859-964` |
| Video RX session | `st20_rx_create`, own `notify_frame_ready`, a `ready_ring` of 32 | `rx.c:702-713, 815-914` |
| Library-owned TX buffer | transport `st20_frames[i]` (derive), or a separate `app_bufs[i]` converted on `buffer_put` on the app thread | `tx.c:299-323, 368-379, 465-485` |
| Library-owned RX buffer | transport frame (derive), or conversion into `app_bufs[i]` inside `buffer_get` on the app thread | `rx.c:311-344` |
| User-owned TX (derive) | `ST20_TX_FLAG_EXT_FRAME` + `st20_tx_set_ext_frame` on the app thread | `tx.c:130-136, 774-775` |
| User-owned TX (convert) | conversion into the internal transport frame on the app thread | `tx.c:137-141` |
| User-owned RX (default) | internal frame, then **memcpy/convert into the user buffer on the tasklet** | `rx.c:118-153` |
| User-owned RX (with `query_ext_frame`) | real `st20` ext-frame RX, with incomplete-frame delivery forced | `rx.c:765-771` |
| Pixel conversion | `st_frame_get_converter` / `video_convert_frame` (the pipeline's internal converters) | `mt_session_video_common.c:27-168` |
| drop-when-late | re-implemented in `get_next_frame` | `tx.c:181-203` |
| Handle guard | none (magic number only) | `mt_session.c` throughout |

---

## 2. Design decisions and assessment

| # | Decision (E = explicit, I = implicit) | Where | Assessment | Why |
|---|---|---|---|---|
| 1 | One opaque session type for all media, with media-specific `*_create` (E) | header `:10-47` | **Keep** | This is the core value of the PR and it matches review §executive. Creation is the only place the media types really differ. |
| 2 | Internal vtable dispatch (E) | `mt_session.h:51-102` | **Keep as an internal detail** | Cheap and hidden. Do not promise "no performance penalty" in the public doc (`header:46`), because dispatch is not the cost that matters. |
| 3 | The new layer **replaces** the pipeline and wraps the session layer directly (E) | `List-of-changes.md:160-219` | **Change** | It duplicates pipeline semantics that `main` has since hardened: FIFO by sequence, guards, blocking, `DROPPED` completion, ST22 plugins. It also writes transport internals (`refcnt`, `tv_meta`), which caused D1/D9. |
| 4 | Remove frame and RTP modes; the pipeline becomes the only API (E) | `List-of-changes.md:82-86, 344-352` | **Open** | This removes the path used by advanced users (RTP-level, slice, `uframe_pg`). The decision needs product input. |
| 5 | Session-wide `ownership` enum (E) | header `:81-84` | **Drop** | Verified in §3 (R1): one bit switches allocation, verbs, zero-copy versus copy, callback use, and completion semantics. |
| 6 | Two verb sets: `buffer_get`/`put` versus `buffer_post` plus events (E) | header `:551-598` | **Change** | Unify into one buffer handle and one submit/dequeue contract (review §2, §8). |
| 7 | Fat public `mtl_buffer_t` with a `container_of` back-link (E) | header `:157-208` | **Change** | The author already flags it as "huge" (PR body note 1). Use an opaque handle with accessors, or `struct_size` with an append-only layout. |
| 8 | Value-backed event ring + eventfd wakeup, with drop-on-full (E) | `mt_session_event.c` | **Keep the mechanism, change the policy** | Allocation-free producer and epoll integration are good. But ownership completions must not share a drop-on-full ring (R4). The ring size (64) must follow `num_buffers`, and drop counters must be exposed. |
| 9 | Polling first, with optional callbacks (E) | `List-of-changes.md:94-102` | **Keep** | The callback context (tasklet) must be documented and constrained. Today `notify_buffer_ready` runs on the tasklet and `notify_event` is dead. |
| 10 | Per-call `timeout_ms` (uint32) where 0 means non-blocking (E) | header `:561` | **Keep, clarify** | RxTxApp passes `-1` and relies on wrap-around (`rx_st20p_app.c`, `tx_st20p_app.c @pr1610`). Define `MTL_WAIT_FOREVER`. Also, `buffer_get` uses `usleep(100)` polling rather than the eventfd (`tx.c:459`, `rx.c:475`). |
| 11 | `-EAGAIN` means "stopped", `-ETIMEDOUT` means "nothing yet" (E) | header `:562, :626` | **Keep the distinction, reconsider the code** | `-EAGAIN` conventionally means "retry". A distinct shutdown code avoids confusion. Minor. |
| 12 | `stop()` is reversible, signal-safe and wakes waiters; the data path auto-starts (E and I) | `mt_session.c:156-202` | **Change** | Lifecycle must be normative: whether create starts, what stop does to TX/RX, drain versus abort. Today stop is an app-side flag only. |
| 13 | `destroy()` as the single safe primitive with a guard (E, doc only) | `GRACEFUL_SHUTDOWN.md` | **Keep the design, not the claims** | Not implemented (`14a1f80c` message: "the guard itself is not yet wired"). It promises a generation-tagged table that is stronger than the existing `mt_handle_guard.h`, whose `mt_handle_acquire` dereferences `*type` at `lib/src/mt_handle_guard.h:81@main`. |
| 14 | Flag bitmask mirroring `ST20_TX/RX_FLAG_*`, with TX-only and RX-only bits in one namespace (E) | header `:113-131` | **Drop** | The PR plans the same (`CURRENT_STATE.md:316`). Two flags (`EXT_BUFFER`, `BLOCK_GET`) are unused. |
| 15 | Direction-dependent `union { st_tx_port; st_rx_port; }` in the config (E) | header `:320-323` | **Change** | This is a union selected by a separate field, so it is easy to misuse and ties the API to pipeline types. |
| 16 | Conversion inside the session (`frame_fmt` versus `transport_fmt`) (E) | common `:27-75` | **Keep the capability, make the path explicit** | Whether a path is direct, converted or copied must be reportable (review §10). Where conversion runs (app thread, tasklet or worker) is a policy decision (D2). |
| 17 | Abstract stats plus `io_stats_get(void*, size)` passthrough (E) | header `:635-677` | **Change** | Use typed per-media stats with `struct_size`. `pkts_received`/`pkts_redundant` are never written by `video_tx_stats_get` (`tx.c:630-654`). |
| 18 | Per-session memory registration, max 8, no DMA map (E) | `tx.c:567-628` | **Drop and replace** | Needs an instance-level region object with a refcount, real `mtl_dma_map`, and multi-port support (R3). |
| 19 | drop-when-late in the new layer (E) | `tx.c:181-203` | **Keep the feature, reuse the pipeline semantics** | `main` pipeline now reports a drop as `notify_frame_done` with `ST_FRAME_STATUS_DROPPED` (`include/st_pipeline_api.h@main`). The PR posts FRAME_LATE plus BUFFER_DONE, two events per drop. |
| 20 | ST22 selected by `bool compressed` in the video config (E) | header `:365-383` | **Change** | Not implemented, yet accepted silently (D5). ST22 needs codec config and plugin lifetime; decide between a separate create and a sub-struct. |
| 21 | Slice mode as `mode` + `query_lines_ready` + `slice_ready`/`slice_query` (E) | header `:342-358, :716-743` | **Keep the concept, re-specify** | This is progressive line operation (review §12). The current API mixes a pull callback and a push call for the same thing. |
| 22 | Magic-number handle validation (I) | `mt_session.h:308-325` | **Drop** | It is undefined behavior after free, and the doc itself notes a typo in it (`CURRENT_STATE.md:111`). |
| 23 | Samples + parity unit suites (old API versus new API) + RxTxApp migration as proof (E) | commits `633b123`, `a693810`, `f23158c` | **Keep the approach** | Parity suites are a good migration safety net. However, the tests `#include` the production `.c` and stub the transport, so they cannot catch callback-boundary bugs (D1, D9). |

---

## 3. Verification of the maintainer review's claims

Verdict summary: all nine claims verified. R3, R4 and R7 are worse than stated.

| # | Review claim | Verdict |
|---|---|---|
| R1 | Ownership enum conflates allocation, lifetime, access, registration, data path (review §2, §4.1) | **Verified** |
| R2 | `buffer_post(data, size, ctx)` lacks layout (review §4.2) | **Verified** |
| R3 | `mem_register` does not DMA-map, no refcount (review §4.3) | **Verified, worse** |
| R4 | Ownership-bearing BUFFER_DONE dropped when the event ring is full (review §4.4) | **Verified, worse** |
| R5 | User TX backlog advances only on a later `buffer_post` (review §4.5) | **Verified** |
| R6 | Slot-index scanning can reorder FIFO (review §4.5) | **Verified by code reading** |
| R7 | `tv_meta` metadata overwritten before the transport callback (review §4.6) | **Verified, worse** |
| R8 | No `struct_size` ABI evolution (review §4.7) | **Verified** |
| R9 | Destroy lacks call/callback guards (review §4.7) | **Verified** |

### R1: ownership conflates five axes

One enum value drives all of the following:

- whether `buffer_post` is accepted at all (`tx.c:514`, `rx.c:510`);
- whether `ST20_TX_FLAG_EXT_FRAME` is set. That happens only when the formats match (`tx.c:774-775`); otherwise user memory is only a converter source (`tx.c:137-141`);
- on RX, a copy on the tasklet versus a direct ext frame, depending on whether `query_ext_frame` is also given (`rx.c:118`, `rx.c:765-771`);
- whether BUFFER_DONE is emitted (`tx.c:246`);
- whether `user_ctx` travels in the event (RX default, `rx.c:150`) or only in the buffer (RX ext, `rx.c:169`, `rx.c:394-400`).

A second, unused switch, `MTL_SESSION_FLAG_EXT_BUFFER`, duplicates it (header `:113`). **[verified]**

### R2: `buffer_post` has no layout

The signature is at header `:592-593`. It has no planes, stride, format, timestamp, `user_meta` or memory domain. User-owned TX therefore **cannot** carry a pacing timestamp at all, which the author also notes in PR comment 1. **[verified]**

### R3: registration is a lookup, not a mapping

- `tx.c:567-605` (and its RX copy) only calls `rte_mem_virt2iova`/`mtl_hp_virt2iova`. It never calls `mtl_dma_map` (`include/mtl_api.h:1301@base`). **[verified]**
- In IOVA-as-VA mode, `rte_mem_virt2iova` returns the VA for **any** pointer (DPDK 26.07 `lib/eal/linux/eal_memory.c:149-154`). Registering `malloc` or `mmap` memory therefore "succeeds" with no IOMMU mapping. **[verified]**
- `mtl_session_lookup_iova` also falls back to `rte_mem_virt2iova` for **unregistered** addresses (`mt_session_buffer.c:297-301`), so the header's "must be from registered memory region" (`:584`) is not enforced. **[verified]**
- `reg->iova + offset` assumes IOVA contiguity across the whole region (`mt_session_buffer.c:290-294`). That is wrong in PA mode for non-hugepage memory. **[verified code; impact inferred]**
- `mem_unregister` frees the record without checking for in-flight use (`tx.c:610-628`). There is a hard limit of 8 regions per session (`mt_session.h:271`). **[verified]**

### R4: ownership completions are lossy, and not drainable after stop

- Drop-on-full is at `mt_session_event.c:89-94`, and the ring holds 64 entries (`:19`). The return value of `event_post` is ignored for BUFFER_DONE (`tx.c:159`, `:193`, `:251`) and for RX user-owned BUFFER_READY. That event carries the only reference to the user buffer (`rx.c:150`). **[verified]**
- The same ring carries VSYNC and FRAME_LATE, so an app that does not drain the ring loses completions to informational traffic. **[verified]**
- **Additional:** after `stop()`, `event_poll` returns `-EAGAIN` **before** dequeueing (`mt_session.c:356-358`, `mt_session_video_common.c:178-180`). Completions that are already queued cannot be reaped until `start()`. The shutdown recipe in `GRACEFUL_SHUTDOWN.md:219-224` ("wait until your outstanding BUFFER_DONE count matches") therefore cannot be carried out after `stop()`. **[verified]**

### R5: backlog progress

The backlog is drained only inside `video_tx_buffer_post` (`tx.c:523-535`). `notify_frame_done` does not drain it (`tx.c:218-255`). The contract comment admits this (`tx.c:508-510`). **[verified]**

### R6: reorder

- The app claims the lowest FREE index (`tx.c:106-111`, `:336-340`), and the tasklet takes the lowest READY index (`tx.c:176-180`). There is no sequence number. **[verified]**
- Trace with 3 slots:
  1. A, B and C bind to slots 0, 1 and 2.
  2. The tasklet takes slot 0 (A), then at the end of A it takes slot 1 (B).
  3. A completes, slot 0 becomes free, and D binds to it.
  4. At the end of B the scan finds slot 0 (D) before slot 2 (C).
  5. Wire order is **A B D C**. **[inferred]**
- The reorder needs `num_buffers >= 3` and an app that runs ahead of transmission. The user-owned samples do both (`USER_BUF_CNT 4`). With RxTxApp's `num_buffers = 2` I believe no reorder occurs. **[inferred]**
- The `main` pipeline avoids this by picking the smallest `seq_number` (`lib/src/st2110/pipeline/st20_pipeline_tx.c:62-77@main`). **[verified]**

### R7: TX metadata never reaches the transport

- `tx_apply_buffer_metadata` writes `timestamp`, `tfmt` and `user_meta` into `ft->tv_meta` (`tx.c:384-401`). `video_tx_get_next_frame` ignores its `meta` out-parameter (`tx.c:171`). **[verified]**
- The transport then does the following (`st_tx_video_session.c@base`; the same assignment is at `:1945@main`):
  1. `tv_init_next_meta(&meta)` fills in defaults (`:1861`).
  2. It calls `get_next_frame` (`:1865`).
  3. It executes `frame->tv_meta = meta;` (`:1888`).
  4. It copies `user_meta` only from `meta.user_meta` (`:1890-1901`).
  5. It paces from `meta.tfmt`/`meta.timestamp` (`:1913-1924`). **[verified]**
- Result: **USER_PACING, USER_TIMESTAMP and user_meta are silently ignored for every TX frame.** Only the PR's drop-when-late check sees the stamp, because it reads `tv_meta` before the overwrite. **[verified]**
- The migrated RxTxApp relies on all three (`tests/tools/RxTxApp/src/tx_st20p_app.c@pr1610`: `buf->timestamp`, `buf->tfmt`, `buf->user_meta = shas`). As a side issue, `shas` is a stack array whose pointer is stored with no copy. **[verified]**
- The unit suites cannot see this, because they `#include` the production `.c` and stub the transport (`tests/unit/new_api/st20_tx_harness.c:1-30`). **[verified]**

### R8: ABI evolution

There is no size or version field in `mtl_session_base_config_t`, `mtl_video_config_t`, `mtl_buffer_t`, `mtl_event_t` or `mtl_session_stats_t` (header `:157-310`, `:635-646`). The unions have `_reserved` padding, but there is no negotiation. The only size-checked call is `io_stats_get` (`:671`). **[verified]**

### R9: destroy guard

- `mtl_session_destroy` has no refcount and no destroying flag (`mt_session.c:212-229`). **[verified]**
- A thread still in the `usleep` loop of `video_tx_buffer_get` dereferences `s->inner.video_tx` (`tx.c:333`, `:336`) after `video_tx_destroy` has set it to NULL and freed `ctx` (`tx.c:430-442`). **[verified]**
- `mtl_session_free` closes the eventfd while another thread may still be in `poll()` on it (`mt_session_event.c:61-64`). **[verified]**
- The header requires "stop + join first" (`:534-535`). `GRACEFUL_SHUTDOWN.md:3-9` claims the opposite, and that claim is not implemented. **[verified]**
- Transport callbacks are ordered correctly: `st20_tx_free` runs before `ctx` is freed (`tx.c:425-441`). **[verified]**

Claims I could **not** confirm or refute:

- Whether R6 actually happens with RxTxApp's default of 2 buffers. I believe it cannot happen **[inferred]**. Only a hardware run would confirm it **[unknown]**.
- Whether the merged tree compiles on `main`. All symbols the PR uses still exist at `main` (checked by `git grep`), but no build was run **[unknown]**.

---

## 4. Additional defects not in the review

| ID | Severity | Defect | Evidence |
|---|---|---|---|
| D1 | High | Same as R7 (TX metadata lost). Listed again because it breaks RxTxApp parity for `user_pacing`, `user_timestamp` and `sha_check`. | see R7 |
| D2 | High | **RX user-owned (default path) copies or converts a whole frame inside the RX tasklet** (note 1). | `rx.c:118-153` |
| D3 | High | **The "zero-copy" samples are unsafe**: an unmapped page-cache mmap is used for DMA, then unmapped before `destroy` (note 2). | `app/sample/new_api/tx_video_user_owned_sample.c:92, 302, 362-369` |
| D4 | Medium | **Double `buffer_put` or foreign `buffer_put` is not rejected.** TX put does not check that `frame_state == APP_OWNED` (`tx.c:465-485`). A second put can store READY over TRANSMITTING, which re-queues a frame in flight. `MTL_BUFFER_IMPL` trusts any pointer (`mt_session.h:334`). | as cited |
| D5 | Medium | **`compressed = true` silently creates an ST20 session.** The flag is stored and logged as "ST22" (`mt_session.c:80, 124-126`), but init always uses `st20_tx_create`/`st20_rx_create`. The codec fields are never read (grep of `lib/src/new_api`). | as cited |
| D6 | Medium | **RX VSYNC never fires.** RX sets `ops->notify_event` but not `ST20_RX_FLAG_ENABLE_VSYNC` (`rx.c:758-760`; the flag exists at `include/st20_api.h:161@base`). `CURRENT_STATE.md:136` claims VSYNC works on RX. | as cited |
| D7 | Medium | **RX FORCE_NUMA is ignored.** RX sets `socket_id` without `ST20_RX_FLAG_FORCE_NUMA` (`rx.c:784-785`; flag at `st20_api.h:176@base`). The RxTxApp `force_rx_video_numa` option regresses. TX sets the flag (`tx.c:784-787`). | as cited |
| D8 | Medium | **User-owned TX claim ignores the transport `refcnt`**, so an ext-frame bind can fail spuriously (note 3). **[inferred race, narrow window]** | `tx.c:102-114` versus `:337` |
| D9 | Medium | **`get_next_frame` force-zeroes the transport `refcnt`** (`rte_atomic32_set(..., 0)`, `tx.c:205`, present since the import commit `8ae7ef6d5`). This defeats the transport's own "refcnt not zero" safety check (`st_tx_video_session.c:1879-1886@base`) and can mask D8 or state corruption. | as cited |
| D10 | Low | `DATA_PATH_ONLY` is mapped (`rx.c:778-779`), but `get_queue_meta` is NULL, so the app cannot learn its queue IDs and the flag cannot be used. | as cited |
| D11 | Low | `enable_timing_parser` sets `ST20_RX_FLAG_TIMING_PARSER_STAT` (`rx.c:788-789`), but no `TIMING_REPORT` is ever posted (acknowledged in `f23158c1`). | as cited |
| D12 | Low | Two meanings share one event: transport "epoch skipped" and "dropped by drop-when-late" both post `MTL_EVENT_FRAME_LATE` (`tx.c:195-197` versus `:266-269`). BUFFER_DONE's `timestamp` field is filled with an epoch counter (`tx.c:249`). | as cited |
| D13 | Low | RX derive copy silently truncates to `entry.size` and still reports success (`rx.c:130-132`). RX conversion failure re-enqueues the user buffer at the tail, which reorders it (`rx.c:139-140`). | as cited |
| D14 | Low | `set_block_timeout` returns success and does nothing (`mt_session.c:600-608`). `MTL_SESSION_FLAG_BLOCK_GET` has no effect. `buffer_get` busy-polls with `usleep(100)` regardless of mode (`tx.c:459`, `rx.c:475`). | as cited |
| D15 | Low | The RX `ready_ring` is `RING_F_SP_ENQ` (`rx.c:707`). With `USE_MULTI_THREADS`, frame completion may run on more than one thread, which would break the single-producer assumption. **[inferred, unverified]** | as cited |
| D16 | Low | `mtl_session_get_type()` returns `MTL_TYPE_VIDEO` for an invalid handle (`mt_session.c:234`). Dead helpers `mtl_session_get_frame_trans`, `mtl_session_put_frame_trans` and `mtl_buffer_fill_from_frame_trans` contain a non-atomic read-then-increment claim (`mt_session_buffer.c:180-186`). | as cited |
| D17 | Doc | `GRACEFUL_SHUTDOWN.md` describes a guard that is not implemented, and contradicts the header's precondition. `CURRENT_STATE.md` links to `NEW_API_PHASE2_HANDOFF.md` and `NEW_API_REVIEW.md`, which do not exist in the PR tree (`git ls-tree`). `List-of-changes.md:535` still says app-owned buffers work via `MTL_SESSION_FLAG_EXT_BUFFER`, which is unused. | as cited |
| D18 | Scope | The PR changes unrelated agent tooling (`.github/copilot-instructions.md`, `.github/skills/mtl-build/SKILL.md`; commit `1f2fb61d`). This should be dropped from any API work. | `git diff 74c9af0e pr1610 --stat -- .github` |

Notes to the table:

1. **D2.** `notify_frame_ready` calls `mtl_memcpy` or `video_convert_frame` (`rx.c:127-147`).
   - The transport calls `notify_frame_ready` from its frame-completion path (`rv_notify_frame_ready`, `st_rx_video_session.c:713-720, 904@base`), so the copy runs on the tasklet. This is a two-world violation: a 4K copy is milliseconds of work on a shared scheduler.
   - The RX sample passes a `MAP_SHARED` file mapping as the destination (`rx_video_user_owned_sample.c:66-99`), so the tasklet can page-fault on file I/O.
   - Commit `b400c5ec` moved conversion off the tasklet for TX only.
2. **D3.** The TX sample mmaps a read-only file and "registers" it (`:92`, `:302`).
   - No DMA mapping is created (R3), so with VFIO/IOMMU the NIC reads unmapped IOVAs. **[inferred]**
   - Teardown runs `mem_unregister`, then `munmap`, then `destroy` (`:362-369`), after a `stop()` that does not stop transmission (§1.4).
3. **D8.** The transport publishes `notify_frame_done`, which sets the slot FREE, **before** it decrements `refcnt` (`st_tx_video_session.c:132-133@base`).
   - `st20_tx_set_ext_frame` rejects `refcnt != 0` (`:4354-4359@base`).
   - The result is either a spurious synchronous `-EINVAL`, or a backlog entry returned through BUFFER_DONE without ever being transmitted.

---

## 5. Good ideas worth preserving

1. **Completion contract wording** (`tx.c:505-507`, `CURRENT_STATE.md:143-150`): "a 0 return yields exactly one asynchronous completion; a negative return yields none." This is the right normative rule. It needs a reliable queue behind it (R4).
2. **Value-backed, allocation-free producer.** Events and user-buffer entries are copied by value into `rte_ring_create_elem` rings, with no `malloc` on the tasklet (commits `80d5b7ae`, `b400c5ec`). This respects the two-world rule.
3. **eventfd wakeup, and `stop()` waking blocked consumers** (`mt_session_event.c:71-80`, `mt_session.c:192-193`). It gives epoll integration (`get_event_fd`) and a prompt wake from a signal handler. The `poll()` timeout rounds up so it never spins (`common.c:205-210`).
4. **Claim-then-pop bind on the app thread** (`tx.c:521-535`). It reserves a slot before dequeuing, so a failed bind never rotates the ring head. Moving TX conversion and binding off the tasklet is the right direction; RX needs the same treatment (D2).
5. **GRACEFUL_SHUTDOWN.md as a target spec.**
   - Its good parts: `destroy()` is the single safe primitive; destroy races resolve to one winner with `-EBUSY` for the others; stop is a small async-signal-safe quiesce; and flush is explicitly separate from stop and destroy.
   - The generation-tagged, validated handle it promises is a real improvement over `mt_handle_guard.h`, which reads `*type` through the raw pointer at `lib/src/mt_handle_guard.h:81@main`. It must be designed and implemented, not just claimed.
6. **Distinct return codes** for "nothing yet" (`-ETIMEDOUT`) and "session stopped". Keep the distinction; the exact code for "stopped" is an open question.
7. **Polymorphic app code.** RxTxApp's st20p TX/RX migrated with a mostly mechanical diff (`c67d3a1e`). The config builder and the `buf->video.*` accessors were enough, which is evidence that the verb set is learnable.
8. **Parity test suites.** Old-API and new-API suites pin the same producer/consumer contract (commits `633b1235`, `a693810c`, `f23158c1`). The idea is worth keeping for migration. The next version needs **callback-boundary** tests against the real transport tasklet, because stubbed tests missed R7 and D6.
9. **An honest feature matrix** (`CURRENT_STATE.md` §4 and §8) that separates "defined in header" from "implemented". Any future header should ship with this discipline, or `-ENOTSUP` must be mandatory for anything unimplemented (D5, D14).
10. **The author's own follow-ups** (PR body and comment of 2026-07-17): hide `mtl_buffer_t` behind accessors; port the pipeline destroy guard; a way to supply timestamps for app-owned buffers; two timestamps (pacing time versus RTP/media timestamp); and cross-stream A/V sync. All of these are consistent with review §11 and §13.

---

## 6. Rebase assessment

### 6.1 What moved on `main` since `74c9af0e`

`git diff 74c9af0e..main --stat` over the requested paths shows **181 files, +23280/-4004**. The relevant parts:

| Area | Change on main | Impact on PR |
|---|---|---|
| `lib/src/st2110/st_tx_video_session.c` | Pacing reworked, e.g. `65b16656` "Honor application TX pacing timestamps", `7058bed0` interlaced field grid, `442847c0` timestamp/RTP basis, `76b4158b`, `5650e25b` | `frame->tv_meta = meta` is still present (`:1945@main`), so R7 persists. Once `meta` is filled, main's new pacing semantics must be matched. **[verified]** |
| `lib/src/st2110/st_rx_video_session.c` | +272 lines | The PR uses only `st20_frames[]`, `rv_meta`, `notify_frame_ready`/`query_ext_frame` and `st20_rx_put_framebuff`. These are present. **[verified by grep]** |
| `lib/src/st2110/pipeline/*` | st20p TX +319 lines, all of st20p, st22p, st30p and st40p reworked: handle guard, seq-number FIFO, condvar blocking, `ST_FRAME_STATUS_DROPPED` drop-when-late, ext-frame release | This is what the PR re-implements. `main`'s version is now the reference, and the PR's copy is behind it. **[verified]** |
| `include/` | `st_pipeline_api.h` +55 (drop-when-late done-callback semantics, `ST22P_RX_FLAG_DISABLE_BOXES`), `st_api.h` +44 (`st10_media_clk_to_tai`), `st20_api.h` +47, `st40_api.h` +185, `mtl_api.h` (`rl_burst_size`), deprecated `mudp*` removed | Nothing the PR header depends on was removed. **[verified]** |
| `tests/unit/` | Large harness restructure (`ut_common`, st20/st30/st40 TX harnesses, pipeline blocking and concurrency suites, a new `st20p_tx_harness.{c,h}` on main) | The PR's `tests/unit/pipeline/st20p_tx_harness.{c,h}` is an add/add conflict, and the PR's `new_api` harnesses `#include` production `.c` files whose internals moved. **[verified conflicts, inferred breakage]** |
| `tests/tools/RxTxApp` | Small changes (`rx_st20p_app.c` 4 lines) | Auto-merges. **[verified]** |

### 6.2 Trial merge

`git merge-tree --write-tree main pr1610` reports **7 textual conflicts**:

- `.github/skills/mtl-build/SKILL.md`
- `include/meson.build`
- `lib/src/meson.build`
- `tests/unit/meson.build`
- `tests/unit/pipeline/st20p_harness.c`
- `tests/unit/pipeline/st20p_tx_harness.c` (add/add)
- `tests/unit/pipeline/st20p_tx_harness.h` (add/add)

All of them are build or test plumbing. `lib/src/new_api/*` and the RxTxApp files merge cleanly. **[verified]**

### 6.3 Recommendation

**Do not rebase for merge. Supersede the PR with a fresh design and implementation, and cherry-pick concepts rather than commits.** **[inferred]** Reasons:

1. **The public contract is the thing under redesign.** The ownership enum, `buffer_post`, the fat buffer struct, registration, the event policy, the lifecycle and ABI evolution are all slated to change (review §16, and §2 above). Rebasing preserves an ABI we already intend to break.
2. **The implementation strategy duplicates the pipeline**, and the pipeline on `main` has overtaken the PR (§6.1). Rebasing forces the maintainer to port main's FIFO, guard, drop-when-late and pacing fixes into a second copy.
3. **Correctness debt concentrates in the backend.** R4, R6, R7, R9, D2 and D8/D9 are all in `lib/src/new_api/*.c`. Fixing them in place means rewriting most of `mt_session_video_tx.c` and `mt_session_video_rx.c` anyway.
4. **Salvage list:**
   - the header vocabulary (`mtl_session_t`, create per media, `event_poll`, `get_event_fd`);
   - `mt_session_event.c` (with changed policy);
   - the completion-contract wording;
   - `GRACEFUL_SHUTDOWN.md` as a target spec;
   - the RxTxApp migration shape;
   - the four samples, after rewriting their memory handling;
   - the parity-suite idea.
  The `.github` tooling changes should be dropped.

---

## Open questions for the maintainer

1. **Which backend does the first unified implementation wrap: the `st20p`/`st22p`/`st30p`/`st40p` pipelines, the low-level session layer (as PR #1610 does), or a new shared core that the pipelines are refactored onto?**
   - Why it matters: the PR's choice produced duplicate semantics and the R7, D8 and D9 class of bugs. Main's pipelines now hold the hardened behavior: guards, FIFO, blocking and drop-when-late.
   - Options:
     - (a) Wrap the pipelines first. This is the fastest route to all media types, and it inherits the fixes.
     - (b) Wrap the session layer. This is leaner but duplicates behavior.
     - (c) Extract a common core and migrate both.
2. **Are frame-level, slice-level and RTP-level session APIs removed, kept as an "expert" API, or exposed as data-path variants of the unified session?**
   - Why it matters: `List-of-changes.md:82-86` proposes "pipeline mode becomes the only API". That strands RTP-level and `uframe_pg` users and pushes slice mode into a re-spec.
   - Options: (a) keep the `st20_*`/`st30_*` APIs frozen alongside; (b) unify frame and slice as progressive readiness (review §12) and drop RTP; (c) a unified RTP-level variant.
3. **Does `create` start the data path, and what exactly does `stop()` do to it?**
   - Why it matters: today create auto-starts, and `stop()` only gates app calls while TX and RX keep running (§1.4). That breaks the samples' teardown and makes "pause" meaningless.
   - Options:
     - (a) Create is idle, `start()` attaches the tasklets, and `stop()` detaches them after a defined drain or abort.
     - (b) Keep auto-start and redefine `stop()` as an app-side quiesce only, documented as such.
4. **After `stop()` (or while destroying), must already-queued ownership completions still be deliverable through `event_poll`/dequeue?**
   - Why it matters: today they are not (R4 addendum), so buffers can be stranded, and the documented "drain at application level" recipe cannot work.
   - Options: (a) completions are always drainable, and only new submissions are refused; (b) destroy returns all outstanding buffers through a final completion burst; (c) destroy documents buffer loss, which is unacceptable for imported memory.
5. **How strong must handle validation be: the existing `mt_handle_guard.h` (refcount plus destroying flag, still reading through the raw pointer) or the generation-tagged session table promised in `GRACEFUL_SHUTDOWN.md`?**
   - Why it matters: only the table makes use-after-destroy a clean `-EINVAL` rather than use-after-free. It costs a global table and an indirection on every call.
   - Options: (a) reuse `mt_handle_guard` and document use-after-destroy as undefined behavior; (b) a handle table with generation tags; (c) a table only in debug builds.
6. **Is the buffer a public struct with `struct_size` evolution, or an opaque handle with accessors, as the PR author suggests?**
   - Why it matters: `mtl_buffer_t` is large, its union is media-dependent, and the struct is recovered through `container_of` (D4). This choice drives ABI stability and misuse resistance.
   - Options: (a) opaque handle + getters/setters; (b) public header struct with `struct_size` + opaque `impl`; (c) public read-only view + setter calls for fields that go to the transport.
7. **What timestamps does a submitted buffer carry: one, or two separate ones (pacing/launch time versus RTP or media timestamp, as in PR comment 2), and in which clock domain?**
   - Why it matters: R7 shows the current single `timestamp` + `tfmt` pair never reached the transport. Main now distinguishes the pacing and RTP-timestamp semantics of `USER_PACING` and `USER_TIMESTAMP`, with TAI versus media-clock caveats (`include/st_pipeline_api.h@main`).
   - Options: (a) `launch_time_tai` + `media_timestamp` with an explicit format; (b) a single timestamp + mode flag; (c) per-submit option struct.
8. **Is multi-stream A/V alignment (PR comment 3) in scope for the first version of the unified API, or deferred?**
   - Why it matters: it shapes whether sessions can be grouped under a shared epoch or clock object at create time, which is hard to retrofit.
   - Options: (a) defer, and rely on exact user pacing; (b) an optional "sync group" object given at create; (c) a library-side epoch alignment helper only.
9. **Where does imported memory live: per session (as in the PR) or per instance, and must registration perform a real `mtl_dma_map` on every port the session uses?**
   - Why it matters: R3 shows registration is currently a lookup, not a mapping. `mtl_dma_map` maps only the primary port and has alignment constraints (review §5).
   - Options: (a) instance-level region object with a refcount and multi-port mapping; (b) per-session, with mapping done at session create; (c) both, with an instance region attached to sessions.
10. **What is the event-queue policy: separate reliable completion queues from lossy informational events, how are they sized, and are library-thread callbacks still allowed?**
    - Why it matters: R4 (lost ownership), the fixed ring of 64, and the fact that `notify_buffer_ready` runs on the tasklet.
    - Options: (a) completion queue sized to the maximum outstanding buffers (it cannot overflow by construction) + a lossy info ring with exposed drop counters; (b) one queue with backpressure; (c) no callbacks, eventfd only.
11. **Is transmit order strictly submission order (FIFO), or may it follow timestamps when user pacing is on?**
    - Why it matters: R6 shows order currently depends on slot indices. Main's pipeline uses the lowest sequence number.
    - Options: (a) strict FIFO always; (b) FIFO by default, with timestamp order as an opt-in policy; (c) implementation-defined (not recommended).
12. **Where may pixel conversion (and RX copy) run: on the app thread inside the put/get call, on a dedicated converter worker, or on a plugin device, and must the chosen path be reported per session or per buffer?**
    - Why it matters: the PR converts on the app thread for TX but on the tasklet for RX user-owned (D2). The pipeline uses converter plugins. This affects latency and the two-world rule.
    - Options: (a) never on the tasklet, app thread by default; (b) a worker pool; (c) a query API that reports the direct/convert/copy path (review §10).
13. **Is ST22 a flag inside the video config (`compressed`) or a separate create call and config?**
    - Why it matters: codec, plugin and codestream settings differ materially. Today `compressed = true` silently yields ST20 (D5).
    - Options: (a) `mtl_st22_session_create`; (b) a `codec` sub-struct selected by an enum, where unsupported values must return `-ENOTSUP`.
14. **What should happen to PR #1610 itself: close it as superseded with credit, keep it as a draft reference, or rebase pieces into a new series?**
    - Why it matters: it avoids duplicated effort, and it preserves the parity-test and sample work.
    - Options: (a) close it with a pointer to the new design and cherry-pick the tests, samples and event code by hand; (b) keep it open as a reference until the new header lands; (c) rebase only the RxTxApp migration once the new header exists.
15. **Which error code signals "session stopped or destroying": keep `-EAGAIN`, or use a distinct code such as `-ESHUTDOWN` or `-ECANCELED`?**
    - Why it matters: `-EAGAIN` conventionally means "retry", and RxTxApp already treats every negative return as "retry" (`continue`), which is ambiguous with shutdown.
    - Options: (a) `-EAGAIN` as today; (b) `-ESHUTDOWN` for stopped and destroyed, with `-EAGAIN` reserved for non-blocking "no buffer"; (c) the same codes plus a query call for the state.
