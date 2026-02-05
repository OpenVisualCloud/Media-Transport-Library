# New Unified Session API - Samples

Sample code demonstrating the new unified polymorphic session API for MTL.

## Sample Organization

### Basic Samples (Self-Terminating)

These samples demonstrate API usage patterns without signal handling complexity.
Each runs for 100 frames then exits cleanly.

| Sample | Description |
|--------|-------------|
| [sample-rx-lib-owned.c](sample-rx-lib-owned.c) | Basic RX with library-owned buffers |
| [sample-tx-lib-owned.c](sample-tx-lib-owned.c) | Basic TX with library-owned buffers |
| [sample-rx-app-owned.c](sample-rx-app-owned.c) | RX with user-provided buffers (zero-copy) |
| [sample-tx-app-owned.c](sample-tx-app-owned.c) | TX with user-provided buffers (zero-copy) |
| [sample-rx-slice-mode.c](sample-rx-slice-mode.c) | RX with slice mode (ultra-low latency) |
| [sample-tx-slice-mode.c](sample-tx-slice-mode.c) | TX with slice mode (ultra-low latency) |
| [sample-tx-st22-plugin.c](sample-tx-st22-plugin.c) | TX with ST2022-6 and plugin support |

### Signal Handling Sample

| Sample | Description |
|--------|-------------|
| [sample-signal-shutdown.c](sample-signal-shutdown.c) | Graceful shutdown with SIGINT/SIGTERM |

This dedicated sample shows proper signal handler integration with MTL sessions.

## Buffer Ownership Models

### Library-Owned (Recommended for most use cases)
```c
mtl_video_config_t config = {
    .base = {
        .ownership = MTL_BUFFER_LIBRARY_OWNED,
        .num_buffers = 3,
    },
    ...
};

// Simple: get buffer → fill/process → put
mtl_session_buffer_get(session, &buffer, timeout);
// use buffer->data
mtl_session_buffer_put(session, buffer);
```

### User-Owned (Zero-Copy)
```c
mtl_video_config_t config = {
    .base = {
        .ownership = MTL_BUFFER_USER_OWNED,
        .num_buffers = NUM_BUFFERS,
    },
    ...
};

// Register memory region
mtl_session_mem_register(session, my_buffers, size, &dma_handle);

// Post buffers to library
mtl_session_buffer_post(session, buf->data, buf->size, user_ctx);

// Poll for completion
mtl_session_event_poll(session, &event, timeout);
if (event.type == MTL_EVENT_BUFFER_DONE) {
    // Buffer can be reused
}
```

## Session Lifecycle

```
create() → start() → [use] → stop() → destroy()
```

1. **create()**: Allocate resources, validate config
2. **start()**: Begin transmission/reception
3. **use**: buffer_get/put loop or event_poll loop
4. **stop()**: Signal stop, blocking calls return -EAGAIN
5. **destroy()**: Free all resources

## Common Patterns

### Simple TX Loop
```c
while (frame_count < MAX_FRAMES) {
    err = mtl_session_buffer_get(session, &buffer, 1000);
    if (err == -ETIMEDOUT) continue;
    if (err < 0) break;
    
    // Fill buffer
    memset(buffer->data, 0x80, buffer->data_size);
    
    mtl_session_buffer_put(session, buffer);
    frame_count++;
}
```

### Simple RX Loop
```c
while (frame_count < MAX_FRAMES) {
    err = mtl_session_buffer_get(session, &buffer, 1000);
    if (err == -ETIMEDOUT) continue;
    if (err < 0) break;
    
    // Process buffer->data
    
    mtl_session_buffer_put(session, buffer);
    frame_count++;
}
```

### Event-Driven (User-Owned)
```c
while (running) {
    err = mtl_session_event_poll(session, &event, 1000);
    if (err == -ETIMEDOUT) continue;
    if (err < 0) break;
    
    switch (event.type) {
    case MTL_EVENT_BUFFER_READY:
        // RX: buffer has received data
        break;
    case MTL_EVENT_BUFFER_DONE:
        // TX: buffer transmission complete
        break;
    case MTL_EVENT_SLICE_READY:
        // Slice mode: more lines available
        break;
    }
}
```

## Error Handling

| Return Value | Meaning |
|--------------|---------|
| 0 | Success |
| -ETIMEDOUT | Operation timed out (normal, retry) |
| -EAGAIN | Session stopped (exit gracefully) |
| -EINVAL | Invalid parameter |
| -ENOMEM | Out of memory |
| -EIO | I/O error |

## Building

These are conceptual samples. To build real applications:

1. Include MTL headers
2. Link against libmtl
3. Initialize DPDK and MTL before creating sessions
