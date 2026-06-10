# MTL Unified Session API — Current Implementation State

**Last updated:** 2026-06-10
**Branch:** `new-api-rebased`
**Scope of this revision:** brought in line with the code after the Phase 1
(crash/correctness) and Phase 2 (data-plane / tasklet-purity) hardening work.
See [NEW_API_PHASE2_HANDOFF.md](../../NEW_API_PHASE2_HANDOFF.md) for the change
log and [NEW_API_REVIEW.md](../../NEW_API_REVIEW.md) for the architectural review
that drove it.

> **Status in one line:** ST20 (uncompressed video) TX **and** RX are
> implemented and unit-tested (library-owned and user-owned/zero-copy). Audio
> (ST30), ancillary (ST40) and ST22 compressed video are **not implemented**
> (`-ENOTSUP`). A hardware (VF) integration gate for the Phase 2 data-plane
> changes is still pending.

---

## Table of Contents

1. [Overview](#overview)
2. [Source Layout](#source-layout)
3. [Architecture](#architecture)
4. [What Works Today](#what-works)
5. [How To Use the API](#how-to-use)
6. [User-Owned (Zero-Copy) Path](#user-owned)
7. [Event Queue](#event-queue)
8. [Not Implemented / TODO](#todo)
9. [Samples](#samples)
10. [Build & Test](#build)

---

## 1. Overview <a name="overview"></a>

The unified session API wraps the existing low-level `st20_tx/rx_create()`
sessions behind a single **polymorphic** `mtl_session_t` handle. The goal is one
buffer/event API that is identical regardless of media type, replacing the four
per-media pipeline APIs (`st20p_*`, `st22p_*`, `st30p_*`, `st40p_*`).

**Key principle:** the new API sits *on top of* the existing low-level API. It
does not replace the internal session manager, scheduler, or data path. It:

- translates `mtl_video_config_t` → `st20_tx_ops` / `st20_rx_ops`,
- wraps `st_frame_trans` as `mtl_buffer_t`,
- bridges the ST20 transport callbacks → a unified `mtl_event_t` queue,
- manages frame lifecycle through an explicit per-frame state machine.

Dispatch is **vtable-based**: `mtl_session_buffer_get/put/post()`,
`mtl_session_event_poll()`, etc. call through `s->vt->...`, so the same public
function works for any media type that defines a vtable.

The public header is shipped at [include/mtl_session_api.h](../../include/mtl_session_api.h).

---

## 2. Source Layout <a name="source-layout"></a>

All new-API library code now lives under **`lib/src/new_api/`** (it was moved out
of `lib/src/` and `lib/src/st2110/` during the rebase).

| File | Purpose |
|------|---------|
| [lib/src/new_api/mt_session.h](../../lib/src/new_api/mt_session.h) | Internal types: `mtl_session_impl`, `mtl_buffer_impl`, the vtable, magic numbers, the TX frame-state enum, helper decls |
| [lib/src/new_api/mt_session.c](../../lib/src/new_api/mt_session.c) | Polymorphic dispatch for all `mtl_session_*()` public functions; create/start/stop/destroy |
| [lib/src/new_api/mt_session_buffer.c](../../lib/src/new_api/mt_session_buffer.c) | Buffer wrapper pool; `st_frame_trans` ↔ `mtl_buffer_t`; value-backed user-buffer ring |
| [lib/src/new_api/mt_session_event.c](../../lib/src/new_api/mt_session_event.c) | Event queue: value-backed `rte_ring` + `eventfd` wakeup; drop-and-count on overflow |
| [lib/src/new_api/mt_session_video_common.h](../../lib/src/new_api/mt_session_video_common.h) | Shared TX/RX video helpers (event poll, deadlines, conversion ctx) |
| [lib/src/new_api/mt_session_video_common.c](../../lib/src/new_api/mt_session_video_common.c) | Shared implementations: blocking `event_poll`, format-conversion helper, stats |
| [lib/src/new_api/mt_session_video_tx.c](../../lib/src/new_api/mt_session_video_tx.c) | Video TX: wraps `st20_tx_create`; library- and user-owned buffer paths; pacing-tasklet callbacks |
| [lib/src/new_api/mt_session_video_rx.c](../../lib/src/new_api/mt_session_video_rx.c) | Video RX: wraps `st20_rx_create`; ready-ring delivery; format auto-detect |

Unit tests and harnesses live under
[tests/unit/new_api/](../../tests/unit/new_api/) (`st20_tx_*`, `st20_rx_*`).

Samples live under [app/sample/new_api/](../../app/sample/new_api/) — see
[§9](#samples).

---

## 3. Architecture <a name="architecture"></a>

```
┌──────────────────────────────────────────────────────────┐
│                     User Application                      │
│  mtl_video_session_create() → buffer_get()/buffer_post()  │
│  → buffer_put() / event_poll() → stop() → destroy()       │
└────────────────────────┬─────────────────────────────────┘
                         │  public API: mtl_session_api.h
┌────────────────────────┴─────────────────────────────────┐
│             mt_session.c  (dispatch layer)                │
│  Validates magic → selects vtable → dispatches            │
└────────────────────────┬─────────────────────────────────┘
                         │  vtable (mtl_session_vtable)
          ┌──────────────┼──────────────┐
    ┌─────┴─────┐  ┌──────┴────┐  ┌──────┴──────┐
    │ video_tx  │  │ video_rx  │  │ audio/anc   │
    │  vtable   │  │  vtable   │  │ (NOT IMPL.) │
    └─────┬─────┘  └─────┬─────┘  └─────────────┘
          │              │
    ┌─────┴─────┐  ┌─────┴─────┐
    │st20_tx_   │  │st20_rx_   │
    │create()   │  │create()   │
    └───────────┘  └───────────┘
```

### Magic numbers (runtime validation)

| Magic | Type | Notes |
|-------|------|-------|
| `0x4D564458` | Video TX | comment says "MVTX" but the bytes spell "MVDX" — a benign typo (the value is internally consistent); slated for cleanup in Phase 5 |
| `0x4D565258` | Video RX | "MVRX" |
| `0x4D415458` | Audio TX | "MATX" |
| `0x4D415258` | Audio RX | "MARX" |
| `0x4D4E5458` | Ancillary TX | "MNTX" |
| `0x4D4E5258` | Ancillary RX | "MNRX" |

---

## 4. What Works Today <a name="what-works"></a>

| Feature | TX | RX | Notes |
|---------|----|----|-------|
| Video session create/destroy | ✅ | ✅ | `mtl_video_config_t` → `st20_tx/rx_ops` → `st20_tx/rx_create` |
| Library-owned `buffer_get`/`buffer_put` | ✅ | ✅ | TX: fill & submit; RX: receive & release |
| User-owned `buffer_post` (zero-copy) | ✅ | ✅ | `mem_register` + `buffer_post`; completion via `BUFFER_DONE` event |
| `mem_register` / `mem_unregister` | ✅ | ✅ | Up to 8 DMA regions per session |
| Format conversion (frame_fmt ↔ transport_fmt) | ✅ | ✅ | Auto-detected; *derive* (zero-copy) when formats match. TX user-owned conversion runs on the app thread (not the tasklet). |
| Event queue + `event_poll` | ✅ | ✅ | Value-backed ring + `eventfd`; blocking with timeout (see [§7](#event-queue)) |
| `get_event_fd` (epoll integration) | ✅ | ✅ | Returns the level-triggered `eventfd` |
| Start / stop / is_stopped | ✅ | ✅ | `stop()` is reversible and signals the eventfd to wake a blocked consumer |
| Statistics (`stats_get`) | ✅ | ✅ | processed / dropped / bytes / epochs-missed / free / in-use |
| Per-port IO stats | ✅ | ✅ | wraps `st20_tx/rx_get_session_stats` |
| Online destination update (TX) | ✅ | — | `st20_tx_update_destination` |
| Online source update (RX) | — | ✅ | `st20_rx_update_source` |
| VSYNC events | ✅ | ✅ | when `MTL_SESSION_FLAG_ENABLE_VSYNC` set |
| Frame-late notification (TX) | ✅ | — | posts `MTL_EVENT_FRAME_LATE` |
| `drop_when_late` (TX) | ✅ | — | `MTL_SESSION_FLAG_DROP_WHEN_LATE` (requires user pacing) |
| Format auto-detect (RX) | — | ✅ | posts `MTL_EVENT_FORMAT_DETECTED` |
| Frame size query | ✅ | ✅ | `mtl_session_get_frame_size` (app-visible size) |
| Pcap dump (RX) | — | ✅ | `st20_rx_pcapng_dump` |

### Completion-event contract (user-owned TX)

`mtl_session_buffer_post()` returning **0** means the buffer was accepted and
will produce **exactly one** asynchronous `MTL_EVENT_BUFFER_DONE` (carrying the
`user_ctx` you passed). A **negative** return means the buffer was rejected
synchronously and produces **no** completion event. In **library-owned** mode no
per-frame `BUFFER_DONE` is posted — a transmitted frame returns to the free pool
implicitly and is reused by the next `buffer_get()`.

---

## 5. How To Use the API <a name="how-to-use"></a>

The canonical, **compiling** usage is in [app/sample/new_api/](../../app/sample/new_api/).
A minimal library-owned TX loop looks like this:

```c
#include <mtl/mtl_session_api.h>

mtl_video_config_t config;
memset(&config, 0, sizeof(config));
config.base.direction  = MTL_SESSION_TX;
config.base.ownership  = MTL_BUFFER_LIBRARY_OWNED;
config.base.num_buffers = 3;
config.base.name        = "my_tx";
config.base.flags       = MTL_SESSION_FLAG_BLOCK_GET; /* block in buffer_get */

config.tx_port = /* st_tx_port: ports, dip_addr, udp_port, payload_type */;
config.width = 1920; config.height = 1080; config.fps = ST_FPS_P59_94;
config.frame_fmt     = ST_FRAME_FMT_YUV422PLANAR10LE; /* app pixel format   */
config.transport_fmt = ST20_FMT_YUV_422_10BIT;        /* wire format        */
config.pacing  = ST21_PACING_NARROW;

mtl_session_t* session = NULL;
int ret = mtl_video_session_create(mt_handle, &config, &session);
if (ret < 0) { /* error */ }

size_t frame_size = mtl_session_get_frame_size(session);
mtl_session_start(session);

while (running) {
  mtl_buffer_t* buf = NULL;
  ret = mtl_session_buffer_get(session, &buf, 1000 /* ms */);
  if (ret == -EAGAIN) break;        /* session stopped               */
  if (ret == -ETIMEDOUT) continue;  /* no buffer within timeout       */
  if (ret < 0) break;               /* error                          */

  memcpy(buf->data, my_frame, frame_size);
  mtl_session_buffer_put(session, buf);
}

mtl_session_stop(session);
mtl_session_destroy(session);
```

RX is symmetric: `buffer_get()` returns a received frame, you read
`buf->data`/`buf->status`/`buf->timestamp`, then `buffer_put()` returns it to the
library.

### Return-value conventions

| Value | Meaning |
|-------|---------|
| `0` | Success |
| `-ETIMEDOUT` | Timed out with no buffer/event (normal — retry) |
| `-EAGAIN` | Session stopped — exit the loop cleanly |
| `-ENOSPC` | User-owned ring full (TX `buffer_post`) |
| `-EINVAL` / `-ENOMEM` / `-EIO` | Bad arg / OOM / I/O error |

> **Auto-start note.** The current implementation **auto-starts** the data path
> at create time; `mtl_session_start()` clears the *stopped* flag and is the
> documented way to resume after `stop()`. An option to *initialize without
> starting* is planned (see [§8](#todo)) but not yet present, so do not rely on
> "no frames flow until start()" today.

---

## 6. User-Owned (Zero-Copy) Path <a name="user-owned"></a>

For zero-copy TX/RX the app owns the memory and hands page pointers to the
library:

```c
config.base.ownership = MTL_BUFFER_USER_OWNED;

/* 1. register a DMA-capable region (mmap'd file, hugepage, etc.) */
mtl_dma_mem_t* dma = NULL;
mtl_session_mem_register(session, region, region_size, &dma);

/* 2. post buffers that point into the registered region (no copy) */
mtl_session_buffer_post(session, frame_ptr, frame_size, my_ctx);

/* 3. reclaim on completion */
mtl_event_t ev;
if (mtl_session_event_poll(session, &ev, 100) == 0 &&
    ev.type == MTL_EVENT_BUFFER_DONE) {
  my_ctx_t* done = ev.ctx;   /* the user_ctx passed to buffer_post */
  /* buffer is free to reuse */
}

/* 4. cleanup */
mtl_session_mem_unregister(session, dma);
```

### TX data-plane behavior (post-Phase 2)

- The posted buffer is bound to a free transmit slot and converted (if
  `frame_fmt != transport_fmt`) **on the calling app thread**, inside
  `buffer_post()`. The pacing tasklet only picks an already-READY slot — it does
  no conversion, allocation, or ring traffic.
- The user-buffer ring is **value-backed** (no per-post malloc).
- If no transmit slot is free, the buffer is deferred in submission order
  (strict FIFO at the bind stage) and bound by a later `buffer_post()`. There is
  currently **no `buffer_flush()`**, so an app must keep posting (or stop the
  session) to drain a backlog — see [§8](#todo).

A complete, runnable example is
[app/sample/new_api/tx_video_user_owned_sample.c](../../app/sample/new_api/tx_video_user_owned_sample.c),
which mmaps a source file and DMA-transmits its pages with no `memcpy`.

---

## 7. Event Queue <a name="event-queue"></a>

The event queue was redesigned in Phase 2.1 to respect the producer/consumer
split (the tasklet producer must never block or allocate; the app-thread
consumer may block):

- **Storage:** a **value-backed** `rte_ring` (`rte_ring_create_elem`,
  single-consumer dequeue). Events are copied into the ring by value — no
  per-event heap allocation.
- **Producer** (transport callbacks, tasklet context): `event_post` enqueues by
  value and never blocks. On a full ring it **drops** the event and bumps an
  atomic `events_dropped` counter, returning `-ENOSPC`.
- **Consumer** (`mtl_session_event_poll`, app thread): blocks on the `eventfd`
  via `poll()` honoring `timeout_ms`. `timeout == 0` is non-blocking
  (`-ETIMEDOUT` when empty); a stopped session returns `-EAGAIN`.
- **Wakeup:** a level-triggered `eventfd`. `mtl_session_stop()` signals it so a
  consumer already blocked in `poll()` wakes promptly and returns `-EAGAIN`
  instead of waiting out the timeout.
- **Epoll integration:** `mtl_session_get_event_fd()` returns the `eventfd` for
  apps that drive their own `epoll`/`poll` loop.

Event types actually posted by the library today: `BUFFER_READY` (RX),
`BUFFER_DONE` (user-owned TX), `VSYNC`, `FRAME_LATE` (TX), `FORMAT_DETECTED`
(RX). `MTL_EVENT_ERROR`, `MTL_EVENT_TIMING_REPORT`, and `MTL_EVENT_SLICE_READY`
are defined in the header but **not yet emitted**.

> `events_dropped` is currently internal only; it is not yet surfaced through the
> public stats API (planned — see [§8](#todo)).

---

## 8. Not Implemented / TODO <a name="todo"></a>

### Not implemented (returns `-ENOTSUP` or absent)

| Item | Where | Notes |
|------|-------|-------|
| Audio (ST30) sessions | `mtl_audio_session_create` | `-ENOTSUP`; no `mt_session_audio_*` |
| Ancillary (ST40) sessions | `mtl_ancillary_session_create` | `-ENOTSUP` |
| Audio/anc vtables | `mt_session.h` | declared `extern`, **never defined** — calling create for these media types must stay gated |
| ST22 compressed video | video config `compressed` path | encoder/decoder plugin wiring not present |
| Slice mode (`slice_ready` / `slice_query`) | video TX/RX | `-ENOTSUP` |
| `buffer_flush` | vtable slot | `NULL`; user-owned backlog drains only on the next `buffer_post` |
| `get_plugin_info` / `get_queue_meta` | vtable slots | `NULL` |
| `set_block_timeout` | `mt_session.c` | not reconciled with per-call `timeout_ms` |

### Planned next (Phase 4 — design decisions already taken)

- **Optional no-auto-start.** Keep auto-start as the default but add a typed
  config field to initialize *without* starting; then `start()` becomes a real
  gate on the data path.
- **Drop the `MTL_SESSION_FLAG_*` bitmask** in favor of typed config fields.
- **Remove dead `mtl_event_t` union members** (`error{int code}`,
  `buffer{mtl_buffer* buf}`) — never written by the library.
- **Add `buffer_flush`** for user-owned backlog drain at stream end.
- **Surface `events_dropped`** through the stats API.

See [NEW_API_PHASE2_HANDOFF.md](../../NEW_API_PHASE2_HANDOFF.md) §3 for the full
remaining plan, and [List-of-changes.md](List-of-changes.md) for the original
design intent (note: some of that intent — e.g. "no auto-start" — is not yet the
implemented behavior and is annotated there).

---

## 9. Samples <a name="samples"></a>

Real, compiling, meson-wired samples live in
[app/sample/new_api/](../../app/sample/new_api/). They are built as part of the
normal app build (executable names in parentheses):

| Sample | Pattern |
|--------|---------|
| [tx_video_lib_owned_sample.c](../../app/sample/new_api/tx_video_lib_owned_sample.c) | TX, library-owned `buffer_get`/`put` loop (`NewApiTxVideoLibOwned`) |
| [rx_video_lib_owned_sample.c](../../app/sample/new_api/rx_video_lib_owned_sample.c) | RX, library-owned `buffer_get`/`put` loop (`NewApiRxVideoLibOwned`) |
| [tx_video_user_owned_sample.c](../../app/sample/new_api/tx_video_user_owned_sample.c) | TX, user-owned zero-copy: `mem_register` + `buffer_post` + completion events (`NewApiTxVideoUserOwned`) |
| [rx_video_user_owned_sample.c](../../app/sample/new_api/rx_video_user_owned_sample.c) | RX, user-owned zero-copy (`NewApiRxVideoUserOwned`) |

The conceptual walkthroughs and diagrams in
[samples/README.md](samples/README.md) and [samples/diagrams.md](samples/diagrams.md)
illustrate the same patterns. (Slice-mode and ST22 diagrams there describe the
*intended* API for features that are not yet implemented — see [§8](#todo).)

For graceful shutdown, see [GRACEFUL_SHUTDOWN.md](GRACEFUL_SHUTDOWN.md).

---

## 10. Build & Test <a name="build"></a>

```bash
# Library (canonical)
./build.sh

# Unit tests (no NIC required)
meson configure build_unit -Denable_unit_tests=true
ninja -C build_unit
./build_unit/tests/unit/UnitTest --gtest_filter='St20NewApi*'

# Samples are produced by the normal app build as
# NewApiTxVideoLibOwned / NewApiRxVideoLibOwned /
# NewApiTxVideoUserOwned / NewApiRxVideoUserOwned
```

The new-API sources compile into `libmtl.so`; no separate library is produced.

> Host-specific build quirks (which `ninja` to use, the `clang-format-14`
> whole-file churn trap on these headers, stale-deps false crashes) are recorded
> in the repo's `.github` build notes.
