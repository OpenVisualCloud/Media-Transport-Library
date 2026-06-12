# New Unified Session API - Samples

Sample code demonstrating the new unified polymorphic session API for MTL.

> **Note:** the real, compiling samples live in
> [app/sample/new_api/](../../../app/sample/new_api/) and are built by the normal
> app build. The snippets in this file are conceptual walkthroughs of the same
> patterns. For an up-to-date feature matrix see
> [../CURRENT_STATE.md](../CURRENT_STATE.md).

## Sample Organization

Four real samples cover the implemented ST20 video paths (library-owned and
user-owned / zero-copy), for both TX and RX. Executable names are in
parentheses.

| Sample | Description |
|--------|-------------|
| [tx_video_lib_owned_sample.c](../../../app/sample/new_api/tx_video_lib_owned_sample.c) | TX, library-owned `buffer_get`/`put` loop (`NewApiTxVideoLibOwned`) |
| [rx_video_lib_owned_sample.c](../../../app/sample/new_api/rx_video_lib_owned_sample.c) | RX, library-owned `buffer_get`/`put` loop (`NewApiRxVideoLibOwned`) |
| [tx_video_user_owned_sample.c](../../../app/sample/new_api/tx_video_user_owned_sample.c) | TX, user-owned zero-copy: `mem_register` + `buffer_post` + completion events (`NewApiTxVideoUserOwned`) |
| [rx_video_user_owned_sample.c](../../../app/sample/new_api/rx_video_user_owned_sample.c) | RX, user-owned zero-copy (`NewApiRxVideoUserOwned`) |

> Slice mode, ST22 compressed video, and audio/ancillary samples are **not**
> provided because those features are not yet implemented (`-ENOTSUP`). See
> [../CURRENT_STATE.md §8](../CURRENT_STATE.md#todo).

The user-owned samples each run a producer thread (posting buffers) and an event
thread (reaping `MTL_EVENT_BUFFER_DONE`), demonstrating proper graceful shutdown
via `mtl_session_stop()` before `pthread_join()` — see
[../GRACEFUL_SHUTDOWN.md](../GRACEFUL_SHUTDOWN.md).

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
2. **start()**: Clear the *stopped* state and (re)enable the data path. Note: the
   current build auto-starts at create time, so `start()` is primarily the
   resume-after-`stop()` operation (see [../CURRENT_STATE.md §8](../CURRENT_STATE.md#todo)).
3. **use**: buffer_get/put loop or event_poll loop
4. **stop()**: Signal stop; blocking calls return -EAGAIN and a blocked
   `event_poll()` is woken via the eventfd
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
    if (err == -EAGAIN) break;       // session stopped
    if (err == -ETIMEDOUT) continue;
    if (err < 0) break;
    
    switch (event.type) {
    case MTL_EVENT_BUFFER_READY:
        // RX: buffer has received data
        break;
    case MTL_EVENT_BUFFER_DONE:
        // TX user-owned: event.ctx is the user_ctx passed to buffer_post();
        // the buffer is now free to reuse
        break;
    /* MTL_EVENT_SLICE_READY is defined but not yet emitted (slice mode TODO) */
    }
}
```

> The event queue blocks on a level-triggered `eventfd`; `mtl_session_get_event_fd()`
> returns it for `epoll`/`select` integration. A blocked `event_poll()` is woken
> immediately by `mtl_session_stop()`.

## Error Handling

| Return Value | Meaning |
|--------------|---------|
| 0 | Success |
| -ETIMEDOUT | Operation timed out (normal, retry) |
| -EAGAIN | Session stopped (exit gracefully) |
| -ENOSPC | User-owned TX ring full (`buffer_post`) |
| -EINVAL | Invalid parameter |
| -ENOMEM | Out of memory |
| -EIO | I/O error |

## Building

The four samples above are built by the normal app build (`./build.sh`) and
appear as `NewApiTxVideoLibOwned`, `NewApiRxVideoLibOwned`,
`NewApiTxVideoUserOwned`, and `NewApiRxVideoUserOwned`.

To build your own application:

1. `#include <mtl/mtl_session_api.h>`
2. Link against `libmtl`
3. `mtl_init()` before creating sessions; `mtl_uninit()` after destroying them
