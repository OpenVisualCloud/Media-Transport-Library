# MTL Unified Session API — Current Implementation State

**Last updated**: 2026-02-06
**Branch**: `new_api_draft`
**Build status**: ✅ Clean build — 0 errors, 0 warnings (`./build.sh`: lib, manager, RxTxApp)

---

## Table of Contents

1. [Overview](#overview)
2. [Files Created / Modified](#files-created)
3. [Architecture & Design](#architecture--design)
4. [What Currently Works (Video TX/RX)](#what-currently-works)
5. [How to Use the API](#how-to-use-the-api)
6. [Phase 2: RxTxApp Migration (COMPLETE)](#phase-2-rxtxapp-migration)
7. [What Is NOT Implemented Yet (TODOs)](#todos--not-yet-implemented)
8. [Internal Details & Conventions](#internal-details--conventions)
9. [Build Instructions](#build-instructions)
10. [Next Steps (Phases 3-4)](#next-steps)

---

## 1. Overview <a name="overview"></a>

We implemented **Phase 1** of the unified session API — the core library layer that wraps
the existing low-level `st20_tx/rx_create()` functions behind a single polymorphic
`mtl_session_*` API — and **Phase 2** — full migration of the RxTxApp test tool from
the pipeline API (`st20p_tx/rx_*`) to the new unified API.

The new API uses a **vtable-based dispatch** pattern so that
`mtl_session_buffer_get()`, `mtl_session_buffer_put()`, etc. work identically regardless
of whether the underlying session is Video TX, Video RX, Audio, or Ancillary.

**Key principle**: The new API sits *on top of* the existing low-level API.  It does
NOT replace the internal session manager, scheduler, or data path.  It is a thin
abstraction layer that:
- Translates `mtl_video_config_t` → `st20_tx_ops` / `st20_rx_ops`
- Wraps `st_frame_trans` as `mtl_buffer_t`
- Bridges ST20 callbacks → unified `mtl_event_t` event queue
- Manages frame lifecycle through refcnt conventions

---

## 2. Files Created / Modified <a name="files-created"></a>

### Public Header (installed to `<prefix>/include/mtl/`)

| File | Purpose |
|------|---------|
| `include/mtl_session_api.h` | Public API: all types, enums, structs, and 30+ function declarations |

### Internal Implementation (lib/src/)

| File | Purpose | Lines |
|------|---------|-------|
| `lib/src/mt_session.h` | Internal header: `mtl_session_impl`, `mtl_buffer_impl`, vtable (30 function pointers), macros | ~383 |
| `lib/src/mt_session.c` | Polymorphic dispatch: all `mtl_session_*()` public functions | ~620 |
| `lib/src/mt_session_buffer.c` | Buffer wrapper: `mtl_buffer_impl` ↔ `st_frame_trans` translation | ~200 |
| `lib/src/mt_session_event.c` | Event queue: `rte_ring`-based + `eventfd` signaling | ~117 |

### Media-Type Specific (lib/src/st2110/)

| File | Purpose | Lines |
|------|---------|-------|
| `lib/src/st2110/mt_session_video_tx.c` | Video TX: wraps `st20_tx_create`, format conversion, refcnt lifecycle | ~720 |
| `lib/src/st2110/mt_session_video_rx.c` | Video RX: wraps `st20_rx_create`, format conversion, ready-ring delivery | ~780 |

### RxTxApp Migration (Phase 2)

| File | Change |
|------|--------|
| `tests/tools/RxTxApp/src/app_base.h` | Changed `st20p_tx_handle handle` → `mtl_session_t* session` in both TX/RX structs; added `#include <mtl/mtl_session_api.h>` |
| `tests/tools/RxTxApp/src/tx_st20p_app.c` | **Fully migrated** — all `st20p_tx_*` calls replaced with `mtl_session_*` equivalents |
| `tests/tools/RxTxApp/src/rx_st20p_app.c` | **Fully migrated** — all `st20p_rx_*` calls replaced with `mtl_session_*` equivalents |

### Build System Changes

| File | Change |
|------|--------|
| `lib/src/meson.build` | Added `mt_session.c`, `mt_session_buffer.c`, `mt_session_event.c` |
| `lib/src/st2110/meson.build` | Added `mt_session_video_tx.c`, `mt_session_video_rx.c` |
| `include/meson.build` | Added `mtl_session_api.h` to `mtl_header_files` |

---

## 3. Architecture & Design <a name="architecture--design"></a>

```
┌──────────────────────────────────────────────────────────┐
│                     User Application                      │
│  mtl_video_session_create() → mtl_session_buffer_get()    │
│  → mtl_session_buffer_put() → mtl_session_destroy()       │
└────────────────────────┬─────────────────────────────────┘
                         │  (public API: mtl_session_api.h)
┌────────────────────────┴─────────────────────────────────┐
│               mt_session.c  (dispatch layer)              │
│  Validates → selects vtable → dispatches to type-specific │
└────────────────────────┬─────────────────────────────────┘
                         │  (vtable: mtl_session_vtable_t)
          ┌──────────────┼──────────────┐
          │              │              │
    ┌─────┴─────┐  ┌─────┴─────┐  ┌────┴──────┐
    │ video_tx  │  │ video_rx  │  │  audio_*  │
    │  vtable   │  │  vtable   │  │  (TODO)   │
    └─────┬─────┘  └─────┬─────┘  └───────────┘
          │              │
    ┌─────┴─────┐  ┌─────┴─────┐
    │st20_tx_   │  │st20_rx_   │
    │create()   │  │create()   │
    │(existing) │  │(existing) │
    └───────────┘  └───────────┘
```

### VTable Pattern

Each media type + direction defines a `const mtl_session_vtable_t` with function
pointers for all operations (30 entries).  The dispatch layer (`mt_session.c`) simply calls
`s->vt->buffer_get(s, buf, timeout)` etc.

Defined vtables:
- `mtl_video_tx_vtable` — in `mt_session_video_tx.c`
- `mtl_video_rx_vtable` — in `mt_session_video_rx.c`
- `mtl_audio_tx_vtable` — declared extern, **NOT DEFINED** (TODO)
- `mtl_audio_rx_vtable` — declared extern, **NOT DEFINED** (TODO)
- `mtl_ancillary_tx_vtable` — declared extern, **NOT DEFINED** (TODO)
- `mtl_ancillary_rx_vtable` — declared extern, **NOT DEFINED** (TODO)

### Magic Numbers (for runtime validation)

| Magic | ASCII | Type |
|-------|-------|------|
| `0x4D565458` | "MVTX" | Video TX |
| `0x4D565258` | "MVRX" | Video RX |
| `0x4D415458` | "MATX" | Audio TX |
| `0x4D415258` | "MARX" | Audio RX |
| `0x4D4E5458` | "MNTX" | Ancillary TX |
| `0x4D4E5258` | "MNRX" | Ancillary RX |

---

## 4. What Currently Works <a name="what-currently-works"></a>

### ✅ Fully Implemented

| Feature | TX | RX | Notes |
|---------|----|----|-------|
| **Video session create/destroy** | ✅ | ✅ | Translates `mtl_video_config_t` → `st20_tx/rx_ops`, calls `st20_tx/rx_create` |
| **Frame-level buffer_get/put** | ✅ | ✅ | TX: refcnt-based; RX: ready-ring based |
| **Format conversion (frame_fmt ↔ transport_fmt)** | ✅ | ✅ | Auto-detected via `st_frame_fmt_equal_transport()`. Uses `st_frame_get_converter()` internally. Derive mode (no conversion) when formats match. |
| **Event queue** | ✅ | ✅ | `rte_ring` + `eventfd`, posts BUFFER_READY/DONE/VSYNC/FRAME_LATE/FORMAT_DETECTED |
| **Session start/stop/is_stopped** | ✅ | ✅ | Atomic stopped flag, state machine |
| **Statistics** | ✅ | ✅ | `buffers_processed`, `bytes_processed`, `buffers_dropped`, `epochs_missed`, free/in-use counts |
| **Online destination update** | ✅ | — | Calls `st20_tx_update_destination()` |
| **Online source update** | — | ✅ | Calls `st20_rx_update_source()` |
| **VSYNC events** | ✅ | ✅ | Translates `ST_EVENT_VSYNC` → `MTL_EVENT_VSYNC` when `MTL_SESSION_FLAG_ENABLE_VSYNC` set |
| **Frame late notification** | ✅ | — | Posts `MTL_EVENT_FRAME_LATE` with skipped epoch |
| **Format auto-detection** | — | ✅ | Posts `MTL_EVENT_FORMAT_DETECTED` with width/height/fps/packing/interlaced |
| **Event polling with timeout** | ✅ | ✅ | `mtl_session_event_poll()` supports blocking with timeout, uses `usleep` busy-poll |
| **Event FD for epoll** | ✅ | ✅ | `mtl_session_get_event_fd()` returns `eventfd` FD for integration with `epoll`/`poll` |
| **Session type query** | ✅ | ✅ | `mtl_session_get_type()` returns `mtl_media_type_t` |
| **User metadata pass-through** | ✅ | ✅ | TX: `buf->user_meta`/`user_meta_size` → `ft->tv_meta.user_meta`; RX: `ft->user_meta` → `buf->user_meta` |
| **Timestamp format (tfmt)** | ✅ | ✅ | TX: `buf->tfmt`/`buf->timestamp` → `ft->tv_meta`; RX: `meta->tfmt`/`meta->timestamp` → `buf->tfmt`/`buf->timestamp` |
| **Per-port IO stats** | ✅ | ✅ | `mtl_session_io_stats_get()` wraps `st20_tx/rx_get_session_stats()`; `mtl_session_io_stats_reset()` wraps reset |
| **Frame size query** | ✅ | ✅ | `mtl_session_get_frame_size()` returns app-visible frame size (accounts for format conversion) |
| **Pcap dump** | — | ✅ | `mtl_session_pcap_dump()` wraps `st20_rx_pcapng_dump()` |

### ✅ Flag Mappings Implemented

These `MTL_SESSION_FLAG_*` flags are properly translated to `ST20_TX/RX_FLAG_*`:

| Flag | TX | RX |
|------|----|----|
| `MTL_SESSION_FLAG_USER_PACING` | ✅ `ST20_TX_FLAG_USER_PACING` | — |
| `MTL_SESSION_FLAG_USER_TIMESTAMP` | ✅ `ST20_TX_FLAG_USER_TIMESTAMP` | — |
| `MTL_SESSION_FLAG_ENABLE_VSYNC` | ✅ `ST20_TX_FLAG_ENABLE_VSYNC` | ✅ `ST20_RX_FLAG_ENABLE_VSYNC` |
| `MTL_SESSION_FLAG_ENABLE_RTCP` | ✅ `ST20_TX_FLAG_ENABLE_RTCP` | ✅ `ST20_RX_FLAG_ENABLE_RTCP` |
| `MTL_SESSION_FLAG_FORCE_NUMA` | ✅ socket_id set | ✅ socket_id set |
| `MTL_SESSION_FLAG_EXT_BUFFER` | ✅ `ST20_TX_FLAG_EXT_FRAME` | ✅ ext_frame mode |
| `MTL_SESSION_FLAG_DATA_PATH_ONLY` | ✅ | ✅ `ST20_RX_FLAG_DATA_PATH_ONLY` |
| `MTL_SESSION_FLAG_RECEIVE_INCOMPLETE_FRAME` | — | ✅ `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` |
| `MTL_SESSION_FLAG_DMA_OFFLOAD` | — | ✅ `ST20_RX_FLAG_DMA_OFFLOAD` |
| `MTL_SESSION_FLAG_HDR_SPLIT` | — | ✅ `ST20_RX_FLAG_HDR_SPLIT` |
| `MTL_SESSION_FLAG_USER_P_MAC` | ✅ `ST20_TX_FLAG_USER_P_MAC` + MAC copy | — |
| `MTL_SESSION_FLAG_USER_R_MAC` | ✅ `ST20_TX_FLAG_USER_R_MAC` + MAC copy | — |
| `MTL_SESSION_FLAG_EXACT_USER_PACING` | ✅ `ST20_TX_FLAG_EXACT_USER_PACING` | — |
| `MTL_SESSION_FLAG_RTP_TIMESTAMP_EPOCH` | ✅ `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` | — |
| `MTL_SESSION_FLAG_DISABLE_BULK` | ✅ `ST20_TX_FLAG_DISABLE_BULK` | — |
| `MTL_SESSION_FLAG_STATIC_PAD_P` | ✅ `ST20_TX_FLAG_ENABLE_STATIC_PAD_P` | — |
| `MTL_SESSION_FLAG_USE_MULTI_THREADS` | — | ✅ `ST20_RX_FLAG_USE_MULTI_THREADS` |

### ✅ Callback Wrappers

| Wrapper | Location | Purpose |
|---------|----------|---------|
| `video_tx_get_next_frame()` | `mt_session_video_tx.c` | Finds refcnt==2 frame (app-submitted), gives to library |
| `video_tx_notify_frame_done()` | `mt_session_video_tx.c` | Marks frame free (refcnt=0), posts BUFFER_DONE event |
| `video_tx_notify_frame_late()` | `mt_session_video_tx.c` | Posts FRAME_LATE event |
| `video_tx_notify_event()` | `mt_session_video_tx.c` | Translates vsync events |
| `video_tx_query_lines_ready_wrapper()` | `mt_session_video_tx.c` | Bridges user `uint16_t*` → `st20_tx_slice_meta*` |
| `video_rx_notify_frame_ready()` | `mt_session_video_rx.c` | Copies `*meta` → `ft->rv_meta`, enqueues received frame to ready_ring |
| `video_rx_notify_detected()` | `mt_session_video_rx.c` | Posts FORMAT_DETECTED event |
| `video_rx_notify_event()` | `mt_session_video_rx.c` | Translates vsync events |
| `video_rx_query_ext_frame_wrapper()` | `mt_session_video_rx.c` | Bridges user `st_ext_frame*` → `st20_ext_frame*` |

---

## 5. How to Use the API <a name="how-to-use-the-api"></a>

### Minimal TX Example

```c
#include <mtl/mtl_session_api.h>

// 1. Create session
mtl_video_config_t config = {0};
config.base.direction = MTL_SESSION_TX;
config.base.ownership = MTL_BUFFER_LIBRARY_OWNED;
config.base.num_buffers = 3;
config.base.name = "my_tx";
config.base.priv = my_app_ctx;
// config.base.notify_buffer_ready = my_callback;  // optional

// Set port (same as st_tx_dest_info from existing API)
config.tx_port = my_tx_dest_info;

// Set video format
config.width = 1920;
config.height = 1080;
config.fps = ST_FPS_P59_94;
config.frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
config.transport_fmt = ST20_FMT_YUV_422_10BIT;

mtl_session_t* session = NULL;
int ret = mtl_video_session_create(mt_handle, &config, &session);
if (ret < 0) { /* error */ }

// 2. Get buffer, fill data, put buffer (main loop)
while (running) {
    mtl_buffer_t* buf = NULL;
    ret = mtl_session_buffer_get(session, &buf, 100 /* ms */);
    if (ret < 0) continue;

    // Fill frame data
    memcpy(buf->data, my_frame_data, buf->size);

    // Optional: set user timestamp
    // buf->timestamp = my_ptp_timestamp;

    // Submit for transmission
    mtl_session_buffer_put(session, buf);
}

// 3. Cleanup
mtl_session_stop(session);
mtl_session_destroy(session);
```

### Minimal RX Example

```c
#include <mtl/mtl_session_api.h>

// 1. Create session
mtl_video_config_t config = {0};
config.base.direction = MTL_SESSION_RX;
config.base.ownership = MTL_BUFFER_LIBRARY_OWNED;
config.base.num_buffers = 3;
config.base.name = "my_rx";
config.rx_port = my_rx_source_info;

config.width = 1920;
config.height = 1080;
config.fps = ST_FPS_P59_94;
config.frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
config.transport_fmt = ST20_FMT_YUV_422_10BIT;

mtl_session_t* session = NULL;
int ret = mtl_video_session_create(mt_handle, &config, &session);

// 2. Receive loop
while (running) {
    mtl_buffer_t* buf = NULL;
    ret = mtl_session_buffer_get(session, &buf, 100);
    if (ret < 0) continue;

    // Process received frame
    process_frame(buf->data, buf->size);
    printf("Timestamp: %lu, Status: %d\n", buf->timestamp, buf->status);

    // Return buffer to library
    mtl_session_buffer_put(session, buf);
}

// 3. Cleanup
mtl_session_stop(session);
mtl_session_destroy(session);
```

### Event Polling Example

```c
// Poll for events (non-blocking or with timeout)
mtl_event_t event;
while (mtl_session_event_poll(session, &event, 0) == 0) {
    switch (event.type) {
        case MTL_EVENT_VSYNC:
            printf("VSync epoch=%lu ptp=%lu\n",
                   event.vsync.epoch, event.vsync.ptp_time);
            break;
        case MTL_EVENT_FORMAT_DETECTED:
            printf("Detected: %ux%u fps=%d\n",
                   event.format_detected.width,
                   event.format_detected.height,
                   event.format_detected.fps);
            break;
        case MTL_EVENT_FRAME_LATE:
            printf("Frame late! Skipped epoch: %lu\n",
                   event.frame_late.epoch_skipped);
            break;
        default: break;
    }
}

// Or use eventfd for epoll integration:
int efd = mtl_session_get_event_fd(session);
// Add efd to your epoll set...
```

### Key API Functions Reference

| Function | Purpose |
|----------|---------|
| `mtl_video_session_create()` | Create a video TX or RX session |
| `mtl_session_buffer_get()` | Get a buffer (TX: empty to fill; RX: received frame) |
| `mtl_session_buffer_put()` | Return a buffer (TX: submit for send; RX: release back) |
| `mtl_session_event_poll()` | Poll for events (vsync, frame late, format detected, etc.) |
| `mtl_session_start()` | Start the session (no-op currently, session starts at create) |
| `mtl_session_stop()` | Stop the session (sets stopped flag, buffer_get returns -EAGAIN) |
| `mtl_session_destroy()` | Destroy session and free all resources |
| `mtl_session_stats_get()` | Get statistics (buffers processed, dropped, etc.) |
| `mtl_session_update_destination()` | TX: change destination IP/port at runtime |
| `mtl_session_update_source()` | RX: change source IP/port at runtime |
| `mtl_session_get_event_fd()` | Get eventfd for epoll/poll integration |
| `mtl_session_is_stopped()` | Check if session is stopped |
| `mtl_session_get_type()` | Get media type (VIDEO, AUDIO, etc.) |
| `mtl_session_get_frame_size()` | Get app-visible frame size in bytes |
| `mtl_session_io_stats_get()` | Get per-port IO statistics (wraps `st20_tx/rx_get_session_stats`) |
| `mtl_session_io_stats_reset()` | Reset per-port IO statistics |
| `mtl_session_pcap_dump()` | RX: trigger pcap dump (wraps `st20_rx_pcapng_dump`) |

---

## 6. Phase 2: RxTxApp Migration (COMPLETE) <a name="phase-2-rxtxapp-migration"></a>

The RxTxApp test tool (`tests/tools/RxTxApp/src/`) has been **fully migrated** from the
pipeline API (`st20p_tx/rx_*`) to the new unified API (`mtl_session_*`). Both TX and RX
video sessions compile and link cleanly.

### Files Modified

| File | Status | Changes |
|------|--------|---------|
| `app_base.h` | ✅ Done | `st20p_tx_handle handle` → `mtl_session_t* session` in both TX/RX structs |
| `tx_st20p_app.c` | ✅ Done | All 8 functions migrated (display, build, thread, stop, free, io_stat, init, uinit) |
| `rx_st20p_app.c` | ✅ Done | All 6 functions migrated (consume, thread, uinit, io_stat, init, pcap) |

### API Mapping Used in Migration

| Old Pipeline API | New Unified API |
|------------------|----------------|
| `st20p_tx_create(st, &ops)` | `mtl_video_session_create(st, &config, &session)` |
| `st20p_tx_get_frame(handle)` | `mtl_session_buffer_get(session, &buf, -1)` |
| `st20p_tx_put_frame(handle, frame)` | `mtl_session_buffer_put(session, buf)` |
| `st20p_tx_free(handle)` | `mtl_session_destroy(session)` |
| `st20p_tx_wake_block(handle)` | `mtl_session_stop(session)` |
| `st20p_tx_frame_size(handle)` | `mtl_session_get_frame_size(session)` |
| `st20p_tx_get_session_stats(handle, &stats)` | `mtl_session_io_stats_get(session, &stats, sizeof(stats))` |
| `st20p_tx_reset_session_stats(handle)` | `mtl_session_io_stats_reset(session)` |
| `st20p_rx_create(st, &ops)` | `mtl_video_session_create(st, &config, &session)` |
| `st20p_rx_get_frame(handle)` | `mtl_session_buffer_get(session, &buf, -1)` |
| `st20p_rx_put_frame(handle, frame)` | `mtl_session_buffer_put(session, buf)` |
| `st20p_rx_free(handle)` | `mtl_session_destroy(session)` |
| `st20p_rx_wake_block(handle)` | `mtl_session_stop(session)` |
| `st20p_rx_frame_size(handle)` | `mtl_session_get_frame_size(session)` |
| `st20p_rx_pcapng_dump(handle, ...)` | `mtl_session_pcap_dump(session, ...)` |
| `struct st_frame*` | `mtl_buffer_t*` |

### Data Field Mapping (st_frame → mtl_buffer_t)

| Old Field | New Field |
|-----------|-----------|
| `frame->addr[0]` | `buf->data` |
| `frame->data_size` / `st_frame_plane_size(frame, 0)` | `buf->data_size` |
| `frame->timestamp` | `buf->timestamp` |
| `frame->tfmt` | `buf->tfmt` |
| `frame->user_meta` | `buf->user_meta` |
| `frame->user_meta_size` | `buf->user_meta_size` |
| `frame->fmt` | `buf->video.fmt` |
| `frame->interlaced` | `buf->video.interlaced` |
| `frame->second_field` | `buf->video.second_field` |
| `frame->pkts_total` | `buf->video.pkts_total` |
| `frame->pkts_recv[N]` | `buf->video.pkts_recv[N]` |

### Config Struct Mapping (st20p_tx/rx_ops → mtl_video_config_t)

| Old Field | New Field |
|-----------|-----------|
| `ops.name` | `config.base.name` |
| `ops.priv` | `config.base.priv` |
| `ops.framebuff_cnt` | `config.base.num_buffers` |
| `ops.socket_id` | `config.base.socket_id` |
| `ops.port` | `config.tx_port` / `config.rx_port` |
| `ops.width/height/fps` | `config.width/height/fps` |
| `ops.interlaced` | `config.interlaced` |
| `ops.input_fmt` / `ops.output_fmt` | `config.frame_fmt` |
| `ops.transport_fmt` | `config.transport_fmt` |
| `ops.transport_pacing` | `config.pacing` |
| `ops.transport_packing` | `config.packing` |
| `ops.device` | `config.plugin_device` |
| `ops.start_vrx` | `config.start_vrx` |
| `ops.pad_interval` | `config.pad_interval` |
| `ops.rtp_timestamp_delta_us` | `config.rtp_timestamp_delta_us` |
| `ops.rx_burst_size` | `config.rx_burst_size` |
| `ops.tx_dst_mac[port]` | `config.tx_dst_mac[port]` |

### Flag Mapping (Pipeline → Unified)

| Old Flag | New Flag |
|----------|----------|
| `ST20P_TX_FLAG_BLOCK_GET` / `ST20P_RX_FLAG_BLOCK_GET` | `MTL_SESSION_FLAG_BLOCK_GET` |
| `ST20P_TX_FLAG_USER_PACING` | `MTL_SESSION_FLAG_USER_PACING` |
| `ST20P_TX_FLAG_USER_TIMESTAMP` | `MTL_SESSION_FLAG_USER_TIMESTAMP` |
| `ST20P_TX_FLAG_USER_P_MAC` | `MTL_SESSION_FLAG_USER_P_MAC` |
| `ST20P_TX_FLAG_USER_R_MAC` | `MTL_SESSION_FLAG_USER_R_MAC` |
| `ST20P_TX_FLAG_EXACT_USER_PACING` | `MTL_SESSION_FLAG_EXACT_USER_PACING` |
| `ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH` | `MTL_SESSION_FLAG_RTP_TIMESTAMP_EPOCH` |
| `ST20P_TX_FLAG_DISABLE_BULK` | `MTL_SESSION_FLAG_DISABLE_BULK` |
| `ST20P_TX_FLAG_ENABLE_STATIC_PAD_P` | `MTL_SESSION_FLAG_STATIC_PAD_P` |
| `ST20P_TX_FLAG_ENABLE_RTCP` / `ST20P_RX_FLAG_ENABLE_RTCP` | `MTL_SESSION_FLAG_ENABLE_RTCP` |
| `ST20P_TX_FLAG_FORCE_NUMA` / `ST20P_RX_FLAG_FORCE_NUMA` | `MTL_SESSION_FLAG_FORCE_NUMA` |
| `ST20P_RX_FLAG_DMA_OFFLOAD` | `MTL_SESSION_FLAG_DMA_OFFLOAD` |
| `ST20P_RX_FLAG_HDR_SPLIT` | `MTL_SESSION_FLAG_HDR_SPLIT` |
| `ST20P_RX_FLAG_USE_MULTI_THREADS` | `MTL_SESSION_FLAG_USE_MULTI_THREADS` |
| `ST20P_RX_FLAG_TIMING_PARSER_STAT` | `config.enable_timing_parser = true` |

### Notable Changes from Original RxTxApp

1. **No `notify_event` callback for TX** — the pipeline's `app_tx_st20p_notify_event()` callback
   (handling `ST_EVENT_VSYNC`, `ST_EVENT_FATAL_ERROR`, `ST_EVENT_RECOVERY_ERROR`) was removed.
   In the new API, events are delivered via `mtl_session_event_poll()` or the `notify_event`
   callback in `mtl_session_base_config_t`.

2. **Direction must be set explicitly** — `config.base.direction = MTL_SESSION_TX` or
   `MTL_SESSION_RX` must be set (pipeline API inferred this from `st20p_tx_create` vs `st20p_rx_create`).

3. **Session creation returns error code** — `mtl_video_session_create()` returns `int` and
   writes `session` to output parameter, whereas pipeline API returned handle directly
   (NULL on error).

### Bugs Fixed During Migration

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| RX frame metadata was uninitialized in `buffer_get()` | `notify_frame_ready()` callback received `meta` as a temporary per-slot pointer; not copied | Added `*meta` copy into `ft->rv_meta` in `video_rx_notify_frame_ready()` |
| `meta->interlaced` compile error | `st20_rx_frame_meta` has no `interlaced` field (only `second_field`) | Use `ctx->interlaced` (session-level config) |
| Duplicate timestamp assignment in RX `buffer_get` | Two conflicting assignments: conditional TAI-only + unconditional raw | Kept only raw pass-through: `pub->tfmt = meta->tfmt; pub->timestamp = meta->timestamp` |

---

## 7. TODOs — Not Yet Implemented <a name="todos--not-yet-implemented"></a>

### 🔴 Critical (blocks Phase 2/3 migration)

| TODO | Location | Notes |
|------|----------|-------|
| Audio session create | `mt_session.c` `mtl_audio_session_create()` | Returns `-ENOTSUP`. Need `mt_session_audio_tx.c` / `mt_session_audio_rx.c` wrapping `st30_tx/rx_create()` |
| Ancillary session create | `mt_session.c` `mtl_ancillary_session_create()` | Returns `-ENOTSUP`. Need wrappers for `st40_tx/rx_create()` |
| Audio/Anc vtables | `mt_session.h` | `mtl_audio_tx_vtable`, `mtl_audio_rx_vtable`, `mtl_ancillary_tx_vtable`, `mtl_ancillary_rx_vtable` — declared `extern` but **never defined**. Will cause linker errors if audio/anc create is called. |

### 🟡 Medium Priority (needed for full feature parity)

| TODO | Location | Notes |
|------|----------|-------|
| `buffer_post()` (user-owned TX) | vtable `.buffer_post = NULL` in both TX/RX | For zero-copy TX where user provides their own buffer pointer directly. Currently only `buffer_get/put` is supported. |
| `buffer_flush()` | vtable `.buffer_flush = NULL` | Flush pending buffers on stop/destroy. |
| `mem_register()` / `mem_unregister()` | vtable `.mem_register = NULL` | DMA memory registration for user-owned buffers. |
| `get_plugin_info()` | vtable `.get_plugin_info = NULL` | ST22 compressed codec plugin info. |
| `get_queue_meta()` | vtable `.get_queue_meta = NULL` | Queue metadata for data-path-only mode. |
| `slice_query()` (RX) | `mt_session_video_rx.c` returns `-ENOTSUP` | Need to read internal slot line counters. |
| `slice_ready()` (TX) | `mt_session_video_tx.c` — no-op stub | Currently relies on `query_lines_ready` callback instead. |
| `set_block_timeout()` | `mt_session.c` — `(void)timeout_us; /* TODO */` | Blocking mode with configurable timeout. |
| Buffer `get_frame_trans` for audio/anc | `mt_session_buffer.c` | Only VIDEO type is handled, audio/anc log error and return NULL. |

### 🟢 Low Priority (polish / nice-to-have)

| TODO | Notes |
|------|-------|
| Event ring overflow handling | Currently drops silently (dbg-level log). Could grow ring or add backpressure. |
| `buffer_get` polling efficiency | Uses `usleep(100)` busy-poll loop. Could use `eventfd` + `poll()` for lower latency. |
| Thread safety of `video_rx_ctx.ready_ring` | Uses `RING_F_SP_ENQ | RING_F_SC_DEQ` — safe for single-producer (callback thread) single-consumer (app thread). May need review for multi-consumer. |
| `notify_buffer_ready` return type | Public API declares `int (*)()`, internal stores as `int (*)()`. Matching now, but callers ignore return value. |

---

## 8. Internal Details & Conventions <a name="internal-details--conventions"></a>

### TX Frame Refcnt State Machine

The Video TX session uses `st_frame_trans.refcnt` for frame lifecycle:

```
  refcnt=0 (FREE)
     │
     ▼ buffer_get() claims it
  refcnt=1 (APP-OWNED, being filled)
     │
     ▼ buffer_put() submits it
  refcnt=2 (READY for transmission)
     │
     ▼ get_next_frame() callback picks it up
  refcnt=3 (TRANSMITTING by library)
     │
     ▼ notify_frame_done() callback
  refcnt=0 (FREE, cycle repeats)
```

### RX Frame Delivery

The Video RX session uses a `rte_ring` ("ready_ring") to queue received frames:

```
  Library calls notify_frame_ready(frame_addr)
     │
     ▼ Enqueue frame_addr to ready_ring
     │  + post MTL_EVENT_BUFFER_READY
     │
  App calls buffer_get()
     │
     ▼ Dequeue frame_addr from ready_ring
     │  Find matching st_frame_trans by address
     │  Fill mtl_buffer_t from frame metadata
     │
  App calls buffer_put()
     │
     ▼ st20_rx_put_framebuff(handle, addr)
       (returns frame to library for reuse)
```

### Event Queue

- `rte_ring` with 64 entries, single-consumer dequeue
- Events are heap-allocated copies (`mt_rte_zmalloc`)
- `eventfd` signaled on every post (for `epoll` integration)
- `notify_buffer_ready` callback invoked for `MTL_EVENT_BUFFER_READY` events

### Key Internal Structs

**`struct mtl_session_impl`** (`mt_session.h`):
- `vt` — vtable pointer (set at create time, never changes)
- `magic` — validation magic number
- `type` / `direction` — media type + TX/RX
- `parent` — back-pointer to `struct mtl_main_impl`
- `inner` — union of internal session impl pointers:
  - `.video_tx` → `struct st_tx_video_session_impl*`
  - `.video_rx` → `struct st_rx_video_session_impl*`
- `video` — session-level video metadata cache:
  - `.compressed` — ST22 mode flag
  - `.mode` — FRAME or SLICE
  - `.frame_fmt` — app pixel format (may differ from transport)
  - `.derive` — `true` if frame_fmt == transport_fmt (no conversion)
- `buffers` — array of `mtl_buffer_impl` wrappers
- `event_ring` / `event_fd` — event queue
- `stats` / `stats_lock` — statistics

**`struct mtl_buffer_impl`** (`mt_session.h`):
- `pub` — the public `mtl_buffer_t` (returned to user, at known offset for `MTL_BUFFER_IMPL()` macro)
- `session` — back-pointer to owning session
- `frame_trans` — pointer to the actual `st_frame_trans` from the low-level session

**`struct video_tx_ctx`** (`mt_session_video_tx.c`):
- `session` — back-pointer
- `handle` — `st20_tx_handle` for calling `st20_tx_*` functions
- `frame_size` — transport framebuffer size
- `derive` — `true` if no conversion needed (frame_fmt == transport_fmt)
- `frame_fmt` — app pixel format (e.g., `ST_FRAME_FMT_YUV422PLANAR10LE`)
- `transport_fmt` — wire format (e.g., `ST20_FMT_YUV_422_10BIT`)
- `converter` — cached `st_frame_converter` with `convert_func` pointer
- `src_frame_size` — app-format buffer size per frame
- `width`, `height`, `interlaced` — cached video dimensions
- `src_bufs` — array of per-framebuffer source buffers in app format (only when `!derive`)
- `src_bufs_cnt` — number of source buffers
- `user_query_lines_ready` — user's slice callback (if any)

**`struct video_rx_ctx`** (`mt_session_video_rx.c`):
- `session` — back-pointer
- `handle` — `st20_rx_handle`
- `frame_size` — transport framebuffer size
- `derive` — `true` if no conversion needed
- `frame_fmt` — app pixel format (output format)
- `transport_fmt` — wire format (input format)
- `converter` — cached `st_frame_converter` with `convert_func` pointer
- `dst_frame_size` — app-format buffer size per frame
- `width`, `height`, `interlaced` — cached video dimensions
- `dst_bufs` — array of per-framebuffer destination buffers in app format (only when `!derive`)
- `dst_bufs_cnt` — number of destination buffers
- `ready_ring` — `rte_ring*` for received frames
- `user_query_ext_frame` — user's ext_frame callback (if any)

---

## 8.5. Format Conversion (frame_fmt ↔ transport_fmt) <a name="format-conversion"></a>

### Overview

The new API directly integrates color/pixel format conversion, replacing the pipeline
layer's conversion functionality. When `frame_fmt != transport_fmt`, the library
automatically converts between the app's pixel format and the wire transport format.

### How It Works

**Detection**: At session creation time, `st_frame_fmt_equal_transport(frame_fmt, transport_fmt)`
is called. If `true`, we use "derive" mode (zero-copy, no conversion). If `false`, we look
up a converter via `st_frame_get_converter()` from the internal converter table.

**TX Path** (app format → wire format):
```
  buffer_get() returns → src_bufs[i] (in frame_fmt, e.g. YUV422PLANAR10LE)
                         ↑ app fills this buffer
  buffer_put() triggers → converter.convert_func(&src_frame, &dst_frame)
                         ↓ converts into transport framebuffer
                         transport frame (in transport_fmt wire format)
                         ↓ marked ready (refcnt=2), picked up by TX engine
```

**RX Path** (wire format → app format):
```
  Library receives → transport frame (in transport_fmt wire format)
                     ↓ enqueued to ready_ring
  buffer_get() triggers → converter.convert_func(&src_frame, &dst_frame)
                         ↓ converts into destination buffer
                         dst_bufs[i] (in frame_fmt, e.g. YUV422PLANAR10LE)
                         ↑ app reads this buffer
  buffer_put() → returns transport frame to library
```

**Derive mode** (frame_fmt matches transport_fmt):
- No conversion needed, no extra buffers allocated
- `buffer_get/put` work directly with transport framebuffers
- Examples: `ST_FRAME_FMT_YUV422RFC4175PG2BE10` + `ST20_FMT_YUV_422_10BIT`

### Buffer Allocation

When conversion is needed, per-framebuffer conversion buffers are allocated:
- **TX**: `src_bufs[0..fb_cnt-1]` — each `st_frame_size(frame_fmt, w, h, interlaced)` bytes
- **RX**: `dst_bufs[0..fb_cnt-1]` — each `st_frame_size(frame_fmt, w, h, interlaced)` bytes

These are allocated from DPDK hugepage memory (`mt_rte_zmalloc_socket`) on the correct
NUMA node, and freed on session destroy.

### Supported Format Pairs

The converter supports all format pairs registered in the internal `converters[]` table
in `lib/src/st2110/st_convert.c`. Common pairs include:

| Source Format | Destination Format | Direction |
|---------------|-------------------|-----------|
| `YUV422PLANAR10LE` | `YUV422RFC4175PG2BE10` | TX (app → wire) |
| `YUV422RFC4175PG2BE10` | `YUV422PLANAR10LE` | RX (wire → app) |
| `V210` | `YUV422RFC4175PG2BE10` | TX |
| `YUV422RFC4175PG2BE10` | `V210` | RX |
| `UYVY` | `YUV422RFC4175PG2BE10` | TX |
| `YUV422RFC4175PG2BE10` | `UYVY` | RX |
| And ~20 more pairs... | | |

### Key Internal Functions Used

- `st_frame_fmt_equal_transport()` — derive mode detection
- `st_frame_get_converter()` — converter lookup from static table
- `st_frame_fmt_from_transport()` — maps `st20_fmt` → `st_frame_fmt`
- `st_frame_init_plane_single_src()` — initializes multi-plane addr/linesize from contiguous buffer
- `st_frame_size()` — computes total frame buffer size for a format

---

## 9. Build Instructions <a name="build-instructions"></a>

```bash
# From repo root
cd /home/gta/mkasiew/repos/mtl

# Full build (lib + manager + RxTxApp) — recommended
./build.sh

# Or build components individually:

# Library only
cd build && ninja

# RxTxApp only (requires library to be installed first)
cd build && sudo ninja install  # install headers + lib
cd tests/tools/RxTxApp/build && ninja
```

The new files are compiled as part of `libmtl.so`. No separate library or binary is needed.
RxTxApp finds the new header via pkg-config (`/usr/local/include/mtl/mtl_session_api.h`).

---

## 10. Next Steps (Phases 3-4) <a name="next-steps"></a>

### ✅ Phase 1: Core Library Layer (COMPLETE)

Implemented the unified session API library layer with vtable dispatch, video TX/RX,
format conversion, event queue, and all features needed for RxTxApp.

### ✅ Phase 2: RxTxApp Migration (COMPLETE)

Migrated RxTxApp TX and RX video sessions from pipeline API to unified API.
All code compiles and links cleanly. Identified and fixed 3 bugs during migration.

### Phase 3: Integration Tests

Migrate `tests/integration_tests/st20p_test.cpp` to use new API.

- Task 3.1: Create test compatibility layer
- Task 3.2: Migrate create/destroy tests
- Task 3.3: Migrate data path tests
- Task 3.4: Migrate digest (SHA verification) tests
- Task 3.5: Migrate remaining tests (interlace, RTCP, auto-detect)
- Task 3.6: Run full test suite (`./tests/KahawaiTest --gtest_filter=St20p.*`)

### Phase 4: Cleanup

- Task 4.1: Remove compatibility layer / old code
- Task 4.2: Documentation update
- Task 4.3: Code review, `./format-coding.sh`, final testing

### Validation Criteria

| Phase | Done When |
|-------|-----------|
| Phase 1 | ✅ Core library builds, vtable dispatch works |
| Phase 2 | ✅ RxTxApp compiles with new API, all pipeline calls replaced |
| Phase 2+ | ⬜ Manual test: RxTxApp receives video frames with new API, no ASAN errors |
| Phase 3 | All `St20p.*` tests pass, no regressions, no TSAN/ASAN errors |
| Phase 4 | `./format-coding.sh` passes, documentation complete |

### Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Callback timing differences | Extensive logging during migration |
| Thread safety issues | Run with TSAN, careful review of locks |
| Performance regression | Benchmark before/after |
| External buffer mode broken | Dedicated test cases |
| Memory leaks | ASAN in CI, explicit free checks |

### Resources in `doc/new_API/`

#### API Design

| File | Description |
|------|-------------|
| [mtl_session_api_improved.h](mtl_session_api_improved.h) | **Original public API design** — used as reference when creating `include/mtl_session_api.h` |
| [mtl_session_internal.h](mtl_session_internal.h) | **Original internal design** — used as reference when creating `lib/src/mt_session.h` |
| [GRACEFUL_SHUTDOWN.md](GRACEFUL_SHUTDOWN.md) | Shutdown design rationale and patterns |
| [List-of-changes.md](List-of-changes.md) | Summary of API changes vs current pipeline API |
| [CURRENT_STATE.md](CURRENT_STATE.md) | This file — comprehensive state of the implementation |

#### Sample Code

All samples in [samples/](samples/) are self-contained demonstrations:

| Sample | Pattern |
|--------|---------|
| [sample-rx-lib-owned.c](samples/sample-rx-lib-owned.c) | Basic RX with library-owned buffers |
| [sample-tx-lib-owned.c](samples/sample-tx-lib-owned.c) | Basic TX with library-owned buffers |
| [sample-rx-app-owned.c](samples/sample-rx-app-owned.c) | RX with user-provided buffers (zero-copy) |
| [sample-tx-app-owned.c](samples/sample-tx-app-owned.c) | TX with user-provided buffers (zero-copy) |
| [sample-rx-slice-mode.c](samples/sample-rx-slice-mode.c) | RX slice mode (ultra-low latency) |
| [sample-tx-slice-mode.c](samples/sample-tx-slice-mode.c) | TX slice mode (ultra-low latency) |
| [sample-tx-st22-plugin.c](samples/sample-tx-st22-plugin.c) | TX with ST2022-6 codec plugins |
| [sample-signal-shutdown.c](samples/sample-signal-shutdown.c) | Signal handler shutdown pattern |

See [samples/README.md](samples/README.md) for detailed usage patterns and [samples/diagrams.md](samples/diagrams.md) for architecture diagrams.

---

## Quick Reference: File → Function Map

### `include/mtl_session_api.h` — Public API
All `mtl_*` function declarations, config structs, enums, buffer/event types.
New in Phase 2: `user_meta`/`user_meta_size`/`tfmt` in `mtl_buffer_t`; 7 new flags (bits 11-17);
5 new config fields (`tx_dst_mac`, `start_vrx`, `pad_interval`, `rtp_timestamp_delta_us`,
`rx_burst_size`); `mtl_session_get_frame_size()`, `mtl_session_io_stats_get/reset()`,
`mtl_session_pcap_dump()`.

### `lib/src/mt_session.c` — Dispatch Layer
- `mtl_video_session_create()` — line ~55
- `mtl_audio_session_create()` — STUB, line ~140
- `mtl_ancillary_session_create()` — STUB, line ~150
- `mtl_session_start()` — line ~165
- `mtl_session_stop()` — line ~195
- `mtl_session_destroy()` — line ~225
- `mtl_session_buffer_get()` — line ~255
- `mtl_session_buffer_put()` — line ~280
- `mtl_session_event_poll()` — line ~370
- `mtl_session_get_frame_size()` — line ~432
- `mtl_session_io_stats_get()` — line ~448
- `mtl_session_io_stats_reset()` — line ~461
- `mtl_session_pcap_dump()` — line ~474

### `lib/src/st2110/mt_session_video_tx.c` — Video TX
- `mtl_video_tx_session_init()` — line ~522 (config → ops translation + st20_tx_create + conversion setup)
- `video_tx_get_next_frame()` — line ~36 (callback: finds app-ready frame)
- `video_tx_notify_frame_done()` — line ~75 (callback: frame transmitted)
- `video_tx_buffer_get()` — line ~225 (finds free frame, conversion-aware, timeout loop)
- `video_tx_buffer_put()` — line ~294 (format conversion + user_meta/timestamp pass-through)
- `video_tx_get_frame_size()` — line ~394 (returns app-visible size)
- `video_tx_io_stats_get/reset()` — line ~401
- `mtl_video_tx_vtable` — line ~493 (exported vtable, 30 entries)

### `lib/src/st2110/mt_session_video_rx.c` — Video RX
- `mtl_video_rx_session_init()` — line ~580 (config → ops translation + st20_rx_create + conversion setup)
- `video_rx_notify_frame_ready()` — line ~65 (callback: copies meta → `ft->rv_meta`, enqueues to ready_ring)
- `video_rx_notify_detected()` — line ~100 (callback: format auto-detect)
- `video_rx_buffer_get()` — line ~261 (dequeues, format conversion, fills buffer with meta/user_meta)
- `video_rx_buffer_put()` — line ~400 (returns frame to library)
- `video_rx_get_frame_size()` — line ~445 (returns app-visible size)
- `video_rx_io_stats_get/reset()` — line ~452
- `video_rx_pcap_dump()` — line ~466
- `mtl_video_rx_vtable` — line ~550 (exported vtable, 30 entries)

### `lib/src/mt_session_buffer.c` — Buffer Management
- `mtl_session_buffers_init()` — allocates buffer wrapper pool
- `mtl_buffer_fill_from_frame_trans()` — fills mtl_buffer_t from st_frame_trans
- `mtl_session_get_frame_trans()` — finds free frame by refcnt
- `mtl_session_put_frame_trans()` — decrements frame refcnt

### `lib/src/mt_session_event.c` — Event Queue
- `mtl_session_events_init()` — creates rte_ring + eventfd
- `mtl_session_event_post()` — enqueues event, signals fd
- `mtl_session_events_uinit()` — drains and frees
