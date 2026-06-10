# Shutting Down MTL Sessions

> **TL;DR** — `mtl_session_destroy()` is the single, safe teardown call. It is
> thread-safe, idempotent against concurrent/double calls, and it **wakes and
> drains** any of your threads blocked in `buffer_get()` / `event_poll()` before
> freeing anything. You do **not** need to call `stop()` first, and you do
> **not** need to `join()` first for safety. `stop()` is an *optional* tool for
> pause/resume and for quiescing from a signal handler — see
> [When to use stop()](#when-stop).

---

## The Lifecycle Guard

Safety is enforced **inside the library**, not by application discipline. Every
public `mtl_session_*()` entry point is protected by a lock-free handle guard —
the same `mt_handle_guard.h` mechanism the low-level `st20_*` API already uses —
plus a validated handle that is **always safe to dereference**. Together these
make it **impossible to segfault** from any sequence of API calls, including
misuse.

The guard carries, per session:

- `_Atomic uint32_t lc_refcnt` — number of public-API callers currently inside
  the session.
- `_Atomic uint32_t lc_destroying` — set once, via a single-winner CAS, when
  teardown begins.
- a generation tag, so a handle that names a since-destroyed (or never-valid)
  session is **detected and rejected**, never dereferenced as live memory.

### How a call is protected

```
int mtl_session_<op>(handle, ...) {
  s = resolve(handle);                 // validate WITHOUT trusting caller memory
  if (!s) return -EINVAL;              // bad / stale / destroyed handle → error
  if (acquire(&s->lc_refcnt,
              &s->lc_destroying) < 0)  // tearing down → refuse cleanly
    return -EAGAIN;
  ... do the work ...
  release(&s->lc_refcnt);             // every exit path releases
  return ret;
}
```

`resolve()` validates the handle against the library's own session table; it
**never reads through a raw application pointer**, so even a garbage or
already-freed handle yields `-EINVAL` instead of a fault.

### How destroy is protected

```
int mtl_session_destroy(handle) {
  s = resolve(handle);
  if (!s) return -EINVAL;                 // double-destroy / stale → clean error
  if (begin_destroy(&s->lc_destroying))   // CAS 0 → 1; loser:
    return -EBUSY;                        //   another thread already won
  signal_eventfd(s);                      // wake any blocked buffer_get/event_poll
  drain(&s->lc_refcnt);                   // spin until refcnt == 0
  ... inner teardown (st20_*_free) ...    // tasklet already detached under lock
  retire(handle);                         // bump generation; slot stays mapped
}
```

This closes the classic use-after-free race by construction:

```
Thread A (worker)                 Thread B (shutdown)
─────────────────                 ───────────────────
buffer_get(s, &b, 1000)
  guard: refcnt 0 → 1
  ... blocked in poll() ...
                                  mtl_session_destroy(s)
                                    CAS destroying 0 → 1   (winner)
                                    signal eventfd  ───────► poll() returns
  guard: refcnt 1 → 0  ◄──────────  drain: wait refcnt → 0
  returns -EAGAIN                   inner teardown + retire
```

### Safety guarantees

Because of the guard and the validated handle, **no API call sequence can crash**:

- **Destroy while a worker is blocked** → the worker is woken and drained first.
- **Double `destroy()` / concurrent `destroy()`** → the CAS picks one winner; the
  losers return `-EBUSY` and free nothing.
- **A call that races teardown** → returns `-EAGAIN` instead of touching memory
  mid-free.
- **A call on an already-destroyed handle** → the generation tag no longer
  matches, so the call returns `-EINVAL`. The memory is **not** dereferenced as a
  live session.
- **A call with a garbage / never-valid handle** → rejected by validation,
  returns `-EINVAL`.

> In short: every public function either does the work or returns a negative
> errno. None of them can fault on a bad, stale, or concurrently-destroyed
> handle.

---

## The Normal Shutdown Pattern

`destroy()` is sufficient on its own. The only thing you owe your *own* worker
threads is a way to leave their loops — they must treat `-EAGAIN` as "exit":

```c
/* Your worker thread */
void* my_worker(void* arg) {
  my_ctx_t* ctx = arg;
  mtl_buffer_t* buf;

  while (!ctx->stop) {
    int ret = mtl_session_buffer_get(ctx->session, &buf, 1000);
    if (ret == -EAGAIN) break;       /* session tearing down — exit */
    if (ret == -ETIMEDOUT) continue; /* no buffer in time — retry   */
    if (ret < 0) break;              /* error                        */

    process(buf);
    mtl_session_buffer_put(ctx->session, buf);
  }
  return NULL;
}

/* Shutdown */
void shutdown(my_app_t* app) {
  app->ctx.stop = true;                  /* ask your worker to exit   */
  pthread_join(app->worker_thread, NULL);/* optional: wait for it     */
  mtl_session_destroy(app->session);     /* safe: wakes + drains      */
}
```

Notes:

- The `pthread_join()` is **optional and for your benefit**, not for safety. If
  your worker is still inside `buffer_get()` when you call `destroy()`, the guard
  wakes and drains it — no crash. Joining first simply gives you deterministic
  control over when your own thread stops touching application state.
- You may call `destroy()` from any thread.

### Multiple sessions

Just destroy them. Order does not matter for safety.

```c
for (int i = 0; i < app->n; i++) mtl_session_destroy(app->sessions[i]);
mtl_uninit(app->mt);
```

---

## When to use stop() <a name="when-stop"></a>

`stop()` is **not** required for a safe shutdown. It exists for two specific
situations where `destroy()` is the wrong tool:

### 1. Signal handlers (SIGINT / SIGTERM)

You **cannot** call `mtl_session_destroy()` from a signal handler — it tears down
DPDK resources and is not async-signal-safe. `mtl_session_stop()`, by contrast,
only sets a flag and performs a single non-blocking 8-byte `write()` to the
eventfd, both of which *are* async-signal-safe. So the idiomatic Ctrl+C pattern
is: **quiesce in the handler with `stop()`, do the heavy `destroy()` back in
normal context.**

```c
static volatile sig_atomic_t g_running = 1;
static app_t* g_app = NULL; /* set before installing the handler */

static void on_sigint(int sig) {
  (void)sig;
  g_running = 0;
  /* stop() is async-signal-safe: wakes blocked workers so they exit promptly */
  if (g_app)
    for (int i = 0; i < g_app->n; i++) mtl_session_stop(g_app->sessions[i]);
}

int main(...) {
  signal(SIGINT, on_sigint);
  /* ... create + start sessions, spawn workers ... */

  while (g_running) pause();          /* or your main loop */

  /* Back in normal context — safe to tear down */
  for (int i = 0; i < g_app->n; i++) {
    pthread_join(g_app->workers[i], NULL);
    mtl_session_destroy(g_app->sessions[i]);
  }
}
```

Calling `stop()` here makes the blocked `buffer_get(timeout=1000)` in each worker
return `-EAGAIN` immediately instead of waiting out its timeout, so shutdown feels
instant. (`destroy()` would also wake them — but you can't call it from the
handler, which is the whole point.)

### 2. Pause / resume

`stop()` is reversible. If you want to suspend a session and later resume it
*without* tearing it down and recreating it, use `stop()` / `start()`:

```c
mtl_session_stop(session);   /* buffer_get/event_poll return -EAGAIN */
/* ... session idle, still fully allocated ... */
mtl_session_start(session);  /* resume normal operation             */
```

If you are not pausing and not handling signals, you never need `stop()`.

---

## What stop() and destroy() do **not** do: flush

Neither `stop()` nor `destroy()` guarantees that frames you already submitted are
fully transmitted/received before the session closes. `destroy()` tears down
promptly and may drop in-flight or queued frames; `stop()` only sets the stop
flag.

If you need a "transmit everything I submitted, then close" guarantee (e.g. to
avoid truncating a recording or a stream), that is a distinct **`buffer_flush()`**
operation. It is **not yet implemented** — see
[CURRENT_STATE.md §8](CURRENT_STATE.md#todo). Until it exists, applications
that care about completeness must drain at the application level (stop producing,
wait until your outstanding `BUFFER_DONE` count matches your posted count) before
calling `destroy()`.

---

## API Reference

### mtl_session_destroy()

```c
int mtl_session_destroy(mtl_session_t* session);
```

The safe teardown primitive.

- Thread-safe; may be called from any thread.
- Idempotent against races: concurrent/duplicate calls have one winner; others
  return `-EBUSY` and free nothing.
- Wakes any caller blocked in `buffer_get()` / `event_poll()` and drains all
  in-flight public-API callers before freeing.
- **You do not need to call `stop()` or `join()` first for safety.**
- After it returns, the handle is retired: any later call on it returns
  `-EINVAL` — it will **not** crash.

### mtl_session_stop()

```c
int mtl_session_stop(mtl_session_t* session);
```

Optional. Set the session to the stopped state and wake blocked callers.

- Async-signal-safe — callable from a SIGINT/SIGTERM handler.
- After this, `buffer_get()` / `event_poll()` return `-EAGAIN` immediately.
- Reversible: `mtl_session_start()` resumes normal operation.
- Does **not** free anything and does **not** flush in-flight frames.

### mtl_session_start()

```c
int mtl_session_start(mtl_session_t* session);
```

Clear the stopped state and resume. Use with `stop()` for pause/resume.

### mtl_session_is_stopped()

```c
bool mtl_session_is_stopped(mtl_session_t* session);
```

Returns whether the session is currently in the stopped state.

---

## Common Questions

**Do I have to call `stop()` before `destroy()`?**
No. `destroy()` is self-sufficient. Call `stop()` only for signal-handler
quiesce or pause/resume.

**Is it safe to `destroy()` while my worker is blocked in `buffer_get()`?**
Yes. The worker is woken (returns `-EAGAIN`) and drained before any memory is
freed.

**Is it safe to call `destroy()` twice, or from two threads?**
Yes. One call wins; the others return `-EBUSY`. Nothing is freed twice.

**What happens if I use the handle after `destroy()` returns?**
The call returns `-EINVAL`. The handle is validated against the library's session
table (with a generation tag), so a retired handle is detected and rejected — it
is never dereferenced as live memory and **cannot crash**.

**How do I make sure all my frames went out before closing?**
Drain at the application level today (match `BUFFER_DONE` count to posted count),
then `destroy()`. A library `buffer_flush()` is planned but not yet available.

---

## Design Rationale

The lifecycle guard puts the *safety* guarantee in the library, where it belongs,
rather than relying on the application to perform a precise `stop → join →
destroy` dance. This follows the lead of the low-level `st20_*` API, which
already guards its handles with the same mechanism.

The guarantee is deliberately stronger than "don't crash if you follow the
rules": a **validated, generation-tagged handle** means the library never trusts
a raw application pointer, so *no* call sequence — double destroy, use after
destroy, garbage handle, or a race against teardown — can fault. The worst an
application can do is get a negative errno back.

`stop()` is deliberately kept as a small, single-purpose, async-signal-safe
primitive — it sets a flag and wakes waiters, nothing more. It is *not* overloaded
into the destroy path, so there is no "mixed-mode" teardown function with flags.
Applications that need pause/resume or signal-driven quiesce get exactly that
primitive; everyone else just calls `destroy()`.
