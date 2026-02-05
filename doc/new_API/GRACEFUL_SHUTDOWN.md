# Graceful Shutdown for MTL Sessions

Sessions don't own threads - your application does. This document describes how to safely shut down sessions when your application threads are using them.

## The Problem

```
Main Thread                     Worker Thread (yours)
───────────                     ─────────────────────
                                while (!stop) {
                                    buf = mtl_session_buffer_get(s, timeout);
                                    ...                    ▲
                                }                         │ BLOCKED waiting
                                                          │
mtl_session_destroy(s);  ────────────────────────────────►│ CRASH: use after free
```

If you call `mtl_session_destroy()` while your thread is blocked in `buffer_get()`, the session memory is freed while still in use → crash.

## The Solution: stop() before destroy()

```c
mtl_session_stop(session);
```
- Sets session to "stopped" state
- `mtl_session_buffer_get()` returns `-EAGAIN` immediately (no more blocking)
- Your worker thread sees `-EAGAIN`, checks its stop flag, exits
- Then you can safely call `mtl_session_destroy()`

Note: `stop()` is reversible - you can call `start()` to resume normal operation.

## Complete Example

```c
// Your worker thread
void* my_worker(void* arg) {
    my_ctx_t* ctx = arg;
    mtl_session_t* session = ctx->session;
    mtl_buffer_t* buf;
    int ret;
    
    while (!ctx->stop) {
        ret = mtl_session_buffer_get(session, &buf, 1000);
        
        if (ret == -EAGAIN) {
            // Session stopped OR no buffer available - check stop flag
            continue;
        }
        if (ret == -ETIMEDOUT) {
            continue;
        }
        if (ret < 0) {
            break;  // Error
        }
        
        // Process buffer
        process(buf);
        mtl_session_buffer_put(session, buf);
    }
    
    return NULL;
}

// Shutdown in main thread
void shutdown(my_app_t* app) {
    // 1. Signal your worker to stop
    app->ctx.stop = true;
    
    // 2. Make buffer_get() return -EAGAIN immediately
    mtl_session_stop(app->session);
    
    // 3. Wait for your worker to exit
    pthread_join(app->worker_thread, NULL);
    
    // 4. Now safe to destroy - no threads using session
    mtl_session_destroy(app->session);
}
```

## Timing Diagram

```
Main Thread              Worker Thread              Session
───────────              ─────────────              ───────
    │                         │                    [RUNNING]
    │                         │ buffer_get()
    │                         │ ═══════════►       (blocked/polling)
    │                         │
stop = true  ◄────────────────┤
    │                         │
stop() ───────────────────────────────────────►    [STOPPED]
    │                         │
    │                         │ ◄═══════════       returns -EAGAIN
    │                         │ checks stop
    │                         │ exits loop
    │                         │ returns
pthread_join() ◄──────────────┤
    │                         ╳
destroy() ────────────────────────────────────►    [DESTROYED]
    │
   DONE
```

## API Reference

### mtl_session_stop()

```c
int mtl_session_stop(mtl_session_t* session);
```

Set session to stopped state.
- Thread-safe, can call from any thread (main, signal handler)
- After this, `buffer_get()` and `event_poll()` return `-EAGAIN` immediately
- Session is still valid, just won't block
- Reversible: call `mtl_session_start()` to resume normal operation

### mtl_session_is_stopped()

```c
bool mtl_session_is_stopped(mtl_session_t* session);
```

Check if `stop()` was called (and not yet `start()`ed).

### mtl_session_start()

```c
int mtl_session_start(mtl_session_t* session);
```

Clear the stopped state, resume normal operation.
- Use this if you want to pause/resume rather than shutdown

### mtl_session_destroy()

```c
int mtl_session_destroy(mtl_session_t* session);
```

Destroy session and free resources.
- **PRECONDITION**: No threads using the session
- Call after `pthread_join()` on all your workers

## Multiple Sessions

```c
void app_shutdown(app_t* app) {
    // Signal all workers and stop all sessions
    for (int i = 0; i < app->num_sessions; i++) {
        app->workers[i].stop = true;
        mtl_session_stop(app->sessions[i]);
    }
    
    // Join all workers
    for (int i = 0; i < app->num_sessions; i++) {
        pthread_join(app->worker_threads[i], NULL);
    }
    
    // Now destroy all sessions
    for (int i = 0; i < app->num_sessions; i++) {
        mtl_session_destroy(app->sessions[i]);
    }
    
    // Now safe to uninit MTL
    mtl_uninit(app->mt);
}
```

## Signal Handler

```c
volatile bool g_stop = false;
app_t* g_app = NULL;

void sigint_handler(int sig) {
    (void)sig;
    g_stop = true;
    
    // stop() is thread-safe, can call from signal handler
    if (g_app) {
        for (int i = 0; i < g_app->num_sessions; i++) {
            mtl_session_stop(g_app->sessions[i]);
        }
    }
}
```

## Common Mistakes

### Wrong: Destroy without stop

```c
// WRONG - worker might be blocked in buffer_get
app->stop = true;
mtl_session_destroy(session);  // Crash!
pthread_join(worker, NULL);
```

### Wrong: Destroy before join

```c
// WRONG - worker might still be in buffer_get when destroy is called
app->stop = true;
mtl_session_stop(session);
mtl_session_destroy(session);  // Crash! Worker not done yet
pthread_join(worker, NULL);
```

### Wrong: Not checking return value

```c
// WRONG - buf is invalid when -EAGAIN returned
while (!stop) {
    mtl_session_buffer_get(session, &buf, timeout);
    process(buf);  // Crash if ret was -EAGAIN!
}
```

## Implementation Notes

The `stop()` function simply sets a `volatile bool stopped` flag in the session. The `buffer_get()` implementation checks this flag:

```c
int mtl_session_buffer_get(mtl_session_t* s, mtl_buffer_t** buf, uint32_t timeout_ms) {
    mtl_session_impl* impl = (mtl_session_impl*)s;
    
    // Check stopped flag first
    if (impl->stopped) {
        return -EAGAIN;
    }
    
    // Normal buffer get logic...
}
```

This is simple, lock-free, and safe to call from signal handlers.

## Design Rationale

This API follows standard patterns from POSIX and libfabric:

1. **Single-purpose functions** - `stop()` only sets the flag, `destroy()` only frees resources
2. **No mixed-mode flags** - Unlike poorly designed APIs that have `func_ex(flags)` variants
3. **Application controls timing** - Library doesn't enforce timeout policies; app decides how long to wait
4. **Predictable behavior** - `stop()` always does the same thing

If immediate shutdown is needed (e.g., watchdog timeout), the application simply:
```c
mtl_session_stop(session);
// Don't wait for threads - just proceed
mtl_session_destroy(session);  // May lose in-flight data, app accepts this
```

The library doesn't need special "force" mode - the app controls whether to wait or not.
