# MTL Unified Session API — Current Implementation State

**Last updated**: 2026-02-06
**Branch**: `new_api_draft`
**Build status**: ✅ Clean build — 0 errors, 0 warnings (debug mode, gcc 11.4)

---

## Table of Contents

1. [Overview](#overview)
2. [Files Created](#files-created)
3. [Architecture & Design](#architecture--design)
4. [What Currently Works (Video TX/RX)](#what-currently-works)
5. [How to Use the API](#how-to-use-the-api)
6. [What Is NOT Implemented Yet (TODOs)](#todos--not-yet-implemented)
7. [Internal Details & Conventions](#internal-details--conventions)
8. [Build Instructions](#build-instructions)
9. [Next Steps (Phases 2-4)](#next-steps)

---

## 1. Overview <a name="overview"></a>

We implemented **Phase 1** of the unified session API — the core library layer that wraps
the existing low-level `st20_tx/rx_create()` functions behind a single polymorphic
`mtl_session_*` API.  The new API uses a **vtable-based dispatch** pattern so that
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

## 2. Files Created <a name="files-created"></a>

### Public Header (installed to `<prefix>/include/mtl/`)

| File | Purpose |
|------|---------|
| `include/mtl_session_api.h` | Public API: all types, enums, structs, and 25+ function declarations |

### Internal Implementation (lib/src/)

| File | Purpose | Lines |
|------|---------|-------|
| `lib/src/mt_session.h` | Internal header: `mtl_session_impl`, `mtl_buffer_impl`, vtable, macros | ~383 |
| `lib/src/mt_session.c` | Polymorphic dispatch: all `mtl_session_*()` public functions | ~563 |
| `lib/src/mt_session_buffer.c` | Buffer wrapper: `mtl_buffer_impl` ↔ `st_frame_trans` translation | ~200 |
| `lib/src/mt_session_event.c` | Event queue: `rte_ring`-based + `eventfd` signaling | ~117 |

### Media-Type Specific (lib/src/st2110/)

| File | Purpose | Lines |
|------|---------|-------|
| `lib/src/st2110/mt_session_video_tx.c` | Video TX: wraps `st20_tx_create`, refcnt-based frame lifecycle | ~505 |
| `lib/src/st2110/mt_session_video_rx.c` | Video RX: wraps `st20_rx_create`, ready-ring frame delivery | ~564 |

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
pointers for all operations.  The dispatch layer (`mt_session.c`) simply calls
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

### ✅ Callback Wrappers

| Wrapper | Location | Purpose |
|---------|----------|---------|
| `video_tx_get_next_frame()` | `mt_session_video_tx.c` | Finds refcnt==2 frame (app-submitted), gives to library |
| `video_tx_notify_frame_done()` | `mt_session_video_tx.c` | Marks frame free (refcnt=0), posts BUFFER_DONE event |
| `video_tx_notify_frame_late()` | `mt_session_video_tx.c` | Posts FRAME_LATE event |
| `video_tx_notify_event()` | `mt_session_video_tx.c` | Translates vsync events |
| `video_tx_query_lines_ready_wrapper()` | `mt_session_video_tx.c` | Bridges user `uint16_t*` → `st20_tx_slice_meta*` |
| `video_rx_notify_frame_ready()` | `mt_session_video_rx.c` | Enqueues received frame to ready_ring |
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

---

## 6. TODOs — Not Yet Implemented <a name="todos--not-yet-implemented"></a>

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
| `mtl_session_get_frame_size()` | Declared in header but implementation forwards to vtable | Not in any vtable currently. |
| `mtl_session_pcap_dump()` | Not implemented | Mentioned in migration plan but not in current API |

### 🟢 Low Priority (polish / nice-to-have)

| TODO | Notes |
|------|-------|
| Event ring overflow handling | Currently drops silently (dbg-level log). Could grow ring or add backpressure. |
| `buffer_get` polling efficiency | Uses `usleep(100)` busy-poll loop. Could use `eventfd` + `poll()` for lower latency. |
| Thread safety of `video_rx_ctx.ready_ring` | Uses `RING_F_SP_ENQ | RING_F_SC_DEQ` — safe for single-producer (callback thread) single-consumer (app thread). May need review for multi-consumer. |
| `notify_buffer_ready` return type | Public API declares `int (*)()`, internal stores as `int (*)()`. Matching now, but callers ignore return value. |

---

## 7. Internal Details & Conventions <a name="internal-details--conventions"></a>

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
- `frame_size` — cached frame size
- `user_query_lines_ready` — user's slice callback (if any)

**`struct video_rx_ctx`** (`mt_session_video_rx.c`):
- `session` — back-pointer
- `handle` — `st20_rx_handle`
- `frame_size` — cached
- `ready_ring` — `rte_ring*` for received frames
- `user_query_ext_frame` — user's ext_frame callback (if any)

---

## 8. Build Instructions <a name="build-instructions"></a>

```bash
# From repo root
cd /home/gta/mkasiew/repos/mtl

# Configure (first time or after meson.build changes)
meson setup build --reconfigure

# Build
cd build && ninja

# Verify clean build
ninja clean && ninja 2>&1 | grep -ic "warning\|error"
# Should output: 0
```

The new files are compiled as part of `libmtl.so`. No separate library or binary is needed.

---

## 9. Next Steps (Phases 2-4) <a name="next-steps"></a>

### Phase 2: RxTxApp Validation

Migrate the RxTxApp test tool to validate the new API works end-to-end.

**Task 2.1**: Migrate TX side in `app/src/tx_st20p_app.c`
- Replace `st20p_tx_create` → `mtl_video_session_create`
- Replace `st20p_tx_get_frame` → `mtl_session_buffer_get`
- Replace `st20p_tx_put_frame` → `mtl_session_buffer_put`

**Task 2.2**: Migrate RX side in `app/src/rx_st20p_app.c`
- Same pattern as TX

**Task 2.3**: Manual testing with RxTxApp

### Phase 3: Integration Tests

Migrate `tests/integration_tests/st20p_test.cpp` to use new API.

- Task 3.1: Create test compatibility layer
- Task 3.2: Migrate create/destroy tests
- Task 3.3: Migrate data path tests
- Task 3.4: Migrate digest (SHA verification) tests
- Task 3.5: Migrate remaining tests (interlace, RTCP, auto-detect)
- Task 3.6: Run full test suite

### Phase 4: Cleanup

- Task 4.1: Remove compatibility layer / old code
- Task 4.2: Documentation update
- Task 4.3: Code review, `./format-coding.sh`, final testing

### Design Documents (for reference)

All design materials are in `doc/new_API/`:

| File | Description |
|------|-------------|
| `IMPLEMENTATION_PLAN.md` | Full 4-phase plan with time estimates |
| `mtl_session_api_improved.h` | Original API design (reference) |
| `mtl_session_internal.h` | Original internal design (reference) |
| `GRACEFUL_SHUTDOWN.md` | Shutdown patterns |
| `List-of-changes.md` | API change summary vs old pipeline API |
| `samples/*.c` | 8 sample programs showing all usage patterns |
| `samples/README.md` | Sample code documentation |
| `samples/diagrams.md` | Architecture diagrams |

---

## Quick Reference: File → Function Map

### `include/mtl_session_api.h` — Public API
All `mtl_*` function declarations, config structs, enums, buffer/event types.

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

### `lib/src/st2110/mt_session_video_tx.c` — Video TX
- `mtl_video_tx_session_init()` — line ~390 (config → ops translation + st20_tx_create)
- `video_tx_get_next_frame()` — line ~36 (callback: finds app-ready frame)
- `video_tx_notify_frame_done()` — line ~75 (callback: frame transmitted)
- `video_tx_buffer_get()` — line ~155 (finds free frame, timeout loop)
- `video_tx_buffer_put()` — line ~233 (marks frame ready, refcnt=2)
- `mtl_video_tx_vtable` — line ~375 (exported vtable)

### `lib/src/st2110/mt_session_video_rx.c` — Video RX
- `mtl_video_rx_session_init()` — line ~470 (config → ops translation + st20_rx_create)
- `video_rx_notify_frame_ready()` — line ~37 (callback: enqueues to ready_ring)
- `video_rx_notify_detected()` — line ~72 (callback: format auto-detect)
- `video_rx_buffer_get()` — line ~200 (dequeues from ready_ring, fills buffer)
- `video_rx_buffer_put()` — line ~262 (returns frame to library)
- `mtl_video_rx_vtable` — line ~385 (exported vtable)

### `lib/src/mt_session_buffer.c` — Buffer Management
- `mtl_session_buffers_init()` — allocates buffer wrapper pool
- `mtl_buffer_fill_from_frame_trans()` — fills mtl_buffer_t from st_frame_trans
- `mtl_session_get_frame_trans()` — finds free frame by refcnt
- `mtl_session_put_frame_trans()` — decrements frame refcnt

### `lib/src/mt_session_event.c` — Event Queue
- `mtl_session_events_init()` — creates rte_ring + eventfd
- `mtl_session_event_post()` — enqueues event, signals fd
- `mtl_session_events_uinit()` — drains and frees
