# Unified Polymorphic Session API - Design Document

## Problem Statement

Current MTL has separate APIs for each media type, causing code duplication:

```c
// Current API - repetitive functions for each media type
st20p_tx_get_frame() / st20p_tx_put_frame()
st22p_tx_get_frame() / st22p_tx_put_frame()  
st30p_tx_get_frame() / st30p_tx_put_frame()
st40p_tx_get_frame() / st40p_tx_put_frame()
```

This leads to:
- Duplicated code in library implementation
- Duplicated code in applications
- Harder to learn API (many similar but different functions)
- Harder to test (separate tests for each type)

---

## The Polymorphic Solution

### Key Design Principle

**ONE session type for ALL media. Same operations everywhere. Only creation differs.**

```c
// NEW API - one set of functions for everything
mtl_session_buffer_get(session, &buffer, timeout);  // Works for video, audio, ancillary!
mtl_session_buffer_put(session, buffer);            // Same function, any media type
```

### API Structure

```
┌─────────────────────────────────────────────────────────────────┐
│                    TYPE-SPECIFIC CREATION                       │
│  mtl_video_session_create()     - Creates video session         │
│  mtl_audio_session_create()     - Creates audio session         │
│  mtl_ancillary_session_create() - Creates ancillary session     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                 UNIFIED mtl_session_t HANDLE                    │
│         (opaque - works for any media type)                     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   POLYMORPHIC OPERATIONS                        │
│  mtl_session_start(session)                                     │
│  mtl_session_stop(session)                                      │
│  mtl_session_buffer_get(session, &buf, timeout)                 │
│  mtl_session_buffer_put(session, buf)                           │
│  mtl_session_event_poll(session, &event, timeout)               │
│  mtl_session_destroy(session)                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Core Changes from Current API

### 1. Removal of "frame" and "rtp" modes

- Pipeline mode becomes the only API
- Simplifies API surface significantly
- Migration path: pipeline already wraps frame/rtp internally

### 2. Polymorphic Session Classes

- Unified `mtl_session_t` for all media types
- VTable-based dispatch internally (no performance penalty)
- Type-safe configuration structs per media type

### 3. Event Polling Replaces Callbacks

| Old (Callbacks) | New (Polling) |
|----------------|---------------|
| `notify_frame_available` callback | `mtl_session_event_poll()` |
| Called from library thread | Application controls timing |
| Hard to integrate with event loops | Easy epoll/select integration |

Optional callbacks still available for low-latency use cases.

### 4. Clear Buffer Ownership Model

**Library-owned** (simple):
```c
mtl_session_buffer_get(session, &buf, timeout);
// Use buf->data
mtl_session_buffer_put(session, buf);
```

**User-owned** (zero-copy):
```c
mtl_session_mem_register(session, my_memory, size, &handle);
mtl_session_buffer_post(session, my_data, size, my_ctx);
// Wait for completion via event_poll
```

### 5. Explicit Session Lifecycle

```
CREATED ──start()──► RUNNING ──stop()──► STOPPED
                         │                  │
                         └──shutdown()──────┴──► DRAINING ──► DESTROYED
```

- No auto-start on create (predictable behavior)
- Explicit `shutdown()` for graceful drain

---

## Unified Buffer Structure

```c
typedef struct mtl_buffer {
    /* Common fields - sufficient for 90% of use cases */
    void* data;             // Buffer data pointer
    size_t size;            // Total buffer size
    size_t data_size;       // Valid data size
    uint64_t timestamp;     // Presentation timestamp
    
    /* Type-specific fields (optional) */
    union {
        struct { uint32_t width, height; ... } video;
        struct { uint32_t samples, channels; ... } audio;
        struct { uint16_t line_number; ... } ancillary;
    };
} mtl_buffer_t;
```

Same buffer structure for all media types. Generic code uses common fields; type-specific code can access the union.

---

## Internal Implementation

**IMPORTANT ARCHITECTURE CHANGE**: The new API does NOT wrap the pipeline API.
It REPLACES the pipeline API and directly uses the low-level session structures.

```
OLD ARCHITECTURE:
┌─────────────────────────┐
│  Application            │
│  (uses st20p_* API)     │
└──────────┬──────────────┘
           ↓
┌─────────────────────────┐
│  Pipeline Layer         │ ← WILL BE REMOVED
│  (st_pipeline_api.h)    │
│  st20p_tx_*, st_frame   │
└──────────┬──────────────┘
           ↓
┌─────────────────────────┐
│  Low-Level Session      │ ← STAYS
│  (st_header.h)          │
│  st_tx_video_session_impl, st_frame_trans
└─────────────────────────┘

NEW ARCHITECTURE:
┌─────────────────────────┐
│  Application            │
│  (uses mtl_session_* API)
└──────────┬──────────────┘
           ↓
┌─────────────────────────┐
│  NEW Unified API        │ ← THIS IS THE NEW LAYER
│  (mtl_session_api.h)    │
│  mtl_session_t, mtl_buffer_t
└──────────┬──────────────┘
           ↓
┌─────────────────────────┐
│  Low-Level Session      │ ← STAYS (same as before)
│  (st_header.h)          │
│  st_tx_video_session_impl, st_frame_trans
└─────────────────────────┘
```

The internal session structure directly points to the low-level impl:

```c
struct mtl_session_impl {
    const mtl_session_vtable_t* vt;  // Dispatch table
    
    union {
        /* Direct pointers to low-level session impl */
        struct st_tx_video_session_impl* video_tx;
        struct st_rx_video_session_impl* video_rx;
        struct st_tx_audio_session_impl* audio_tx;
        struct st_rx_audio_session_impl* audio_rx;
    } inner;
};
```

**Zero overhead**: One indirect call through vtable, same as current function pointer callbacks.

---

## Benefits Summary

| Aspect | Current API | New Unified API |
|--------|-------------|-----------------|
| Functions to learn | 40+ (10 per media type) | ~15 total |
| App code duplication | High (separate handlers) | Low (generic handlers) |
| Library code duplication | High | Medium (shared via vtable) |
| Testing | Separate tests per type | One test framework |
| Adding new media type | Copy-paste all functions | Add vtable + create fn |

---

## Files in this Directory

| File | Description |
|------|-------------|
| `List-of-changes.md` | This document |
| `mtl_session_api_improved.h` | **Public API** - Unified session interface |
| `mtl_session_internal.h` | **Internal** - VTable, impl structs for library |
| `samples/` | Example usage code |

---

## API Wrapping Feasibility - Video Session (ST20/ST22)

The new unified API **replaces** the pipeline API and **directly wraps** the low-level
session structures from `st_header.h`. This section documents what from `st20_api.h`
can be wrapped and why.

### Low-Level Structures We Wrap

| Low-Level (stays) | New Unified API |
|-------------------|-----------------|
| `st_tx_video_session_impl` | `mtl_session_impl.inner.video_tx` |
| `st_rx_video_session_impl` | `mtl_session_impl.inner.video_rx` |
| `st_frame_trans` | `mtl_buffer_impl.frame_trans` |
| `st20_tx_ops` callbacks | Converted to polling events |
| `st20_rx_ops` callbacks | Converted to polling events |

### TX Operations Mapping (st20_tx_* → mtl_session_*)

| Current `st20_api.h` Function | New Unified API | Notes |
|------------------------------|-----------------|-------|
| `st20_tx_create()` | `mtl_video_session_create()` | Config translation |
| `st20_tx_free()` | `mtl_session_destroy()` | Polymorphic |
| `st20_tx_get_framebuffer()` | `mtl_session_buffer_get()` | Get by index → get next free |
| `st20_tx_update_destination()` | `mtl_session_update_destination()` | Direct wrap |
| `st20_tx_set_ext_frame()` | Via `query_ext_frame` callback | Unified ext frame model |
| `st20_tx_get_framebuffer_size()` | Via `mtl_buffer_t.size` | Part of buffer struct |
| `st20_tx_get_framebuffer_count()` | Via config | Set at creation |
| `st20_tx_get_pacing_params()` | Could add if needed | Low priority |
| `st20_tx_get_session_stats()` | `mtl_session_stats_get()` | Polymorphic |
| `st20_tx_reset_session_stats()` | `mtl_session_stats_reset()` | Polymorphic |
| `st20_tx_get_sch_idx()` | Internal only | Not exposed |
| `st20_tx_get_mbuf()` / `st20_tx_put_mbuf()` | **NOT WRAPPED** | RTP-level only |

### RX Operations Mapping (st20_rx_* → mtl_session_*)

| Current `st20_api.h` Function | New Unified API | Notes |
|------------------------------|-----------------|-------|
| `st20_rx_create()` | `mtl_video_session_create()` | Config translation |
| `st20_rx_free()` | `mtl_session_destroy()` | Polymorphic |
| `st20_rx_put_framebuff()` | `mtl_session_buffer_put()` | Polymorphic |
| `st20_rx_update_source()` | `mtl_session_update_source()` | Direct wrap |
| `st20_rx_get_queue_meta()` | `mtl_session_get_queue_meta()` | For DATA_PATH_ONLY |
| `st20_rx_timing_parser_critical()` | Config + events | Via TIMING_REPORT event |
| `st20_rx_get_session_stats()` | `mtl_session_stats_get()` | Polymorphic |
| `st20_rx_reset_session_stats()` | `mtl_session_stats_reset()` | Polymorphic |
| `st20_rx_pcapng_dump()` | Could add if needed | Debug feature |
| `st20_rx_get_mbuf()` / `st20_rx_put_mbuf()` | **NOT WRAPPED** | RTP-level only |

### Callback → Event Mapping

| `st20_tx_ops` / `st20_rx_ops` Callback | New Event Type | Notes |
|---------------------------------------|----------------|-------|
| `get_next_frame()` | Internal | Library manages frame selection |
| `notify_frame_done()` | `MTL_EVENT_BUFFER_DONE` | TX completion |
| `notify_frame_ready()` | `MTL_EVENT_BUFFER_READY` | RX frame available |
| `notify_frame_late()` | `MTL_EVENT_FRAME_LATE` | TX missed epoch |
| `notify_event(VSYNC)` | `MTL_EVENT_VSYNC` | Epoch boundary |
| `notify_detected()` | `MTL_EVENT_FORMAT_DETECTED` | Auto-detect result |
| `notify_slice_ready()` | `MTL_EVENT_SLICE_READY` | Slice mode RX |
| `query_frame_lines_ready()` | Via config callback | Slice mode TX |
| `query_ext_frame()` | Via base config callback | User-owned buffers |
| `uframe_pg_callback()` | **NOT SUPPORTED** | Rare use case |
| `notify_rtp_done()` / `notify_rtp_ready()` | **NOT SUPPORTED** | RTP-level only |

### Flags Mapping

| `ST20_TX_FLAG_*` / `ST20_RX_FLAG_*` | New `mtl_video_config_t` | Notes |
|------------------------------------|--------------------------|-------|
| `ST20_TX_FLAG_EXT_FRAME` | `ownership = USER_OWNED` | Unified model |
| `ST20_TX_FLAG_USER_PACING` | Via `flags` | Preserved |
| `ST20_TX_FLAG_USER_TIMESTAMP` | Via `flags` | Preserved |
| `ST20_TX_FLAG_ENABLE_VSYNC` | Via `flags` | Enables VSYNC events |
| `ST20_TX_FLAG_ENABLE_RTCP` | Via `flags` | Preserved |
| `ST20_TX_FLAG_FORCE_NUMA` | Via `socket_id` config | Explicit NUMA |
| `ST20_RX_FLAG_DATA_PATH_ONLY` | Via `flags` | App manages flow rules |
| `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | Via `flags` | Required for slice |
| `ST20_RX_FLAG_DMA_OFFLOAD` | Via `flags` | DMA copy offload |
| `ST20_RX_FLAG_AUTO_DETECT` | `enable_auto_detect` config | Explicit |
| `ST20_RX_FLAG_TIMING_PARSER_*` | `enable_timing_parser` config | Explicit |
| `ST20_RX_FLAG_HDR_SPLIT` | Via `flags` | Preserved |

### What is NOT Wrapped (by design)

| Excluded Feature | Reason |
|-----------------|--------|
| `ST20_TYPE_RTP_LEVEL` | Too low-level; use frame mode |
| `st20_tx_get_mbuf()` / mbuf API | RTP-level only |
| `uframe_pg_callback` | Very rare use case |
| `st20_pgroup` utilities | Helper functions, not session API |
| Pixel format conversion APIs | Separate utility, not session |

### What IS Supported (new features)

| Feature | New API Support |
|---------|----------------|
| **Slice-level (video)** | `mode = MTL_VIDEO_MODE_SLICE`, `mtl_session_slice_*()` |
| **ST22 Plugins** | `plugin_device`, `codec`, `codec_thread_cnt` in config |
| **Plugin info query** | `mtl_session_get_plugin_info()` |
| **Event FD for epoll** | `mtl_session_get_event_fd()` |
| **Explicit start/stop** | `mtl_session_start()` / `mtl_session_stop()` |

---

## Slice-Level Support (Ultra-Low Latency)

Slice-level mode enables sub-frame latency by processing video line-by-line.

### Configuration

```c
mtl_video_config_t config = {
    // ... standard fields ...
    .mode = MTL_VIDEO_MODE_SLICE,  // Enable slice mode
    
    // TX only: callback for library to query ready lines
    .query_lines_ready = my_query_lines_callback,
};
```

### TX Workflow

```c
mtl_buffer_t* buf;
mtl_session_buffer_get(session, &buf, timeout);

// Fill lines progressively
for (int line = 0; line < height; line++) {
    fill_line(buf->data, line);
    // Notify library: lines 0..line are ready
    mtl_session_slice_ready(session, buf, line + 1);
}

mtl_session_buffer_put(session, buf);  // Complete frame
```

### RX Workflow (Event-Driven)

```c
mtl_event_t event;
while (mtl_session_event_poll(session, &event, timeout) == 0) {
    if (event.type == MTL_EVENT_SLICE_READY) {
        // Process newly received lines
        process_lines(event.slice.buffer, 
                     last_lines, 
                     event.slice.lines_ready);
        last_lines = event.slice.lines_ready;
    }
    if (event.type == MTL_EVENT_BUFFER_READY) {
        // Full frame complete
        mtl_session_buffer_put(session, event.buffer.buf);
    }
}
```

### RX Workflow (Polling)

```c
mtl_buffer_t* buf;
mtl_session_buffer_get(session, &buf, timeout);

uint16_t lines = 0;
while (lines < height) {
    uint16_t new_lines;
    mtl_session_slice_query(session, buf, &new_lines);
    if (new_lines > lines) {
        process_lines(buf->data, lines, new_lines);
        lines = new_lines;
    }
}

mtl_session_buffer_put(session, buf);
```

---

## Plugin Support (ST22 Codecs)

ST22 compressed video uses plugins for encoding/decoding (JPEGXS, H264, etc.).

### Configuration

```c
mtl_video_config_t config = {
    .compressed = true,           // Enable ST22
    .codec = ST22_CODEC_JPEGXS,   // Codec type
    .plugin_device = ST_PLUGIN_DEVICE_AUTO,  // Let library choose
    .quality = ST22_QUALITY_MODE_SPEED,      // Speed vs quality
    .codec_thread_cnt = 4,        // Parallel encode threads
    .codestream_size = ...,       // Target bitrate for CBR
};
```

### Querying Plugin Information

```c
mtl_plugin_info_t info;
if (mtl_session_get_plugin_info(session, &info) == 0) {
    printf("Using plugin: %s v%s (%s)\n", 
           info.name, info.version,
           info.device == ST_PLUGIN_DEVICE_GPU ? "GPU" : "CPU");
}
```

### Note on Plugin Registration

Plugin registration (`st22_encoder_register()`, etc.) remains a separate API at the `mtl_handle` level. Sessions simply specify which plugin device to use; the library selects an appropriate registered plugin.

### Events Supported

| Event | Source in Current API |
|-------|----------------------|
| `MTL_EVENT_BUFFER_READY` | `notify_frame_available` |
| `MTL_EVENT_BUFFER_DONE` | User-owned buffer completion |
| `MTL_EVENT_ERROR` | Various error conditions |
| `MTL_EVENT_VSYNC` | `notify_event(ST_EVENT_VSYNC)` |
| `MTL_EVENT_FRAME_LATE` | `notify_frame_late` callback |
| `MTL_EVENT_FORMAT_DETECTED` | `notify_detected` (RX auto-detect) |
| `MTL_EVENT_TIMING_REPORT` | `notify_timing_parser_result` |

---

## Migration from Current API

```c
// OLD: Separate creation and operations per media type
st20p_tx_handle h = st20p_tx_create(mt, &ops);
struct st_frame* f = st20p_tx_get_frame(h);
st20p_tx_put_frame(h, f);
st20p_tx_free(h);

// NEW: Unified operations, type-specific creation only
mtl_session_t* s;
mtl_video_session_create(mt, &config, &s);
mtl_buffer_t* b;
mtl_session_buffer_get(s, &b, timeout);
mtl_session_buffer_put(s, b);
mtl_session_destroy(s);
```

---

## Conclusion: Video Session Wrapping Feasibility

### ✅ FULLY FEASIBLE

The new unified API can fully wrap the video session functionality:

| Aspect | Status | Notes |
|--------|--------|-------|
| **Frame-based TX** | ✅ Full | `buffer_get/put` maps to `get_next_frame/put_frame` |
| **Frame-based RX** | ✅ Full | `buffer_get/put` maps to `get_frame/put_frame` |
| **Slice mode TX** | ✅ Full | `slice_ready()` maps to `notify_frame_lines_ready` |
| **Slice mode RX** | ✅ Full | `slice_query()` + events map to `query_frame_lines_ready` + `notify_slice_ready` |
| **Library-owned buffers** | ✅ Full | Default mode, internal alloc |
| **App-owned buffers** | ✅ Full | `MTL_SESSION_FLAG_EXT_BUFFER` + callback |
| **All video formats** | ✅ Full | Pass-through `st20_fmt` enum |
| **All timing/pacing** | ✅ Full | Config fields map 1:1 |
| **All events** | ✅ Full | Callbacks → event queue |
| **Statistics** | ✅ Full | `get_stats()` polymorphic |
| **ST22 plugins** | ✅ Full | Codec config fields |

### Why This Works

1. **Low-level API is stable**: `st_tx_video_session_impl` and `st_frame_trans` provide all needed functionality
2. **No functionality loss**: Every callback has an event equivalent
3. **Polymorphic dispatch**: VTable allows type-specific implementation without user-visible complexity
4. **Direct access**: Wrap low-level, not pipeline - no double-wrapping overhead

### What the New API Provides Over Low-Level

| Feature | Low-Level | New API |
|---------|-----------|---------|
| Polymorphic operations | ❌ Per-type functions | ✅ Same functions for all types |
| Event system | Callbacks | Unified event queue + poll |
| Buffer abstraction | `st_frame_trans*` raw | `mtl_buffer_t` with metadata |
| Error reporting | Scattered | Centralized with events |
| API surface | ~50 functions per type | ~15 unified functions |

---

## Next Steps

1. Review `mtl_session_api_improved.h` for complete API definition
2. Review `mtl_session_internal.h` for implementation structure
3. Review `samples/` for usage examples
4. Implement video session wrapper as first type
5. Add audio and ancillary session wrappers
6. Create migration guide for existing applications