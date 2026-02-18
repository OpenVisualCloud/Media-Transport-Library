# Concurrency & Locking — Design Knowledge

## The Two-Tier Locking Architecture

(Part of the broader "two-world" design pattern described in `architecture-and-design-philosophy.md`.)

MTL has a strict two-tier locking design driven by a single constraint: **tasklets must never block**.

| Tier | Lock Type | Where Used | Why |
|------|-----------|------------|-----|
| **Data plane** | `rte_spinlock_t` | Tasklet handlers, shared queues, transmitter | Tasklets run in busy-poll loops — a mutex would context-switch and destroy latency |
| **Control plane** | `pthread_mutex_t` | Session create/destroy, queue alloc/free, manager operations | These happen infrequently, may take milliseconds, and need to interoperate with condvars |

**The cardinal rule**: Never hold a spinlock across a blocking operation. Never use a mutex inside a tasklet.

## The Session Get/Put Pattern

This is the most important concurrency pattern in the codebase. Every tasklet handler does:

```c
session = session_try_get(mgr, idx);  // rte_spinlock_trylock
if (!session) return;                  // another thread has it — skip
// ... do work on session ...
session_put(session);                  // rte_spinlock_unlock
```

**Why trylock, not lock?** Because if two tasklet handlers contend on the same session (e.g., builder and transmitter both touch the same session array), the loser MUST NOT spin-wait — that would block the whole scheduler. Instead, it returns immediately and tries again next iteration (microseconds later).

**Why this exists at all**: Session create/destroy happens on the control-plane thread. Without this lock, the tasklet could be reading session fields while the control thread is freeing the session. The get/put spinlock serializes access.

## Pipeline vs Session Locking: Why They Differ

Pipeline operations (`st20p_tx_get_frame`, `st20p_tx_put_frame`) use `pthread_mutex_t`, not spinlock. Why?

Because pipeline functions are called from the **application's thread**, not from a tasklet. The app thread CAN block — it's the app's problem. The mutex also integrates with condvar for `st20p_rx_get_frame()` blocking mode (wake when frame ready).

**Gotcha**: If your code runs in a tasklet context, you MUST use spinlock. If it runs in an app or control-plane thread, you CAN use mutex. Check the call chain carefully.

## Lock Ordering Constraints

When multiple locks must be held simultaneously, MTL follows these ordering rules to prevent deadlock:

1. **Manager mutex before session spinlock**: `mgr_mutex.lock() → session_try_get()`. Never reverse.
2. **Migration ordering**: When migrating a session between schedulers, lock **target manager first**, then source manager. This is arbitrary but consistent — both directions would work as long as it's always the same order.
3. **Queue allocation**: The tx/rx queue mutex (`mt_dev_impl.tx_q_mutex`) is independent of session locks. Never hold both.

## Atomic Operations — `mt_atomic32_t` (C11 built-ins)

MTL defines its own atomic type `mt_atomic32_t` (`int32_t` typedef) in `lib/src/mt_atomic.h`, using GCC/Clang `__atomic` built-ins. This **replaces the deprecated DPDK `rte_atomic32_t` API** which DPDK has deprecated since 21.11.

### Memory Ordering Policy

| Wrapper | Ordering | When to use |
|---------|----------|-------------|
| `mt_atomic32_read` / `set` / `inc` / `dec` | RELAXED | Stats counters, session counts (protected by external lock), init/teardown |
| `mt_atomic32_read_acquire` | ACQUIRE | Polling a stop flag or checking refcnt before reuse |
| `mt_atomic32_set_release` / `dec_release` | RELEASE | Signaling a stop flag, publishing data, releasing refcnt |
| `mt_atomic32_dec_and_test` | ACQ_REL | Destroy-on-zero refcount pattern (returns `true` when result is 0) |

**Rule of thumb**: If two threads communicate *only* through the atomic (no external lock), use acquire/release. If the atomic is protected by a mutex/spinlock or is a stats counter where staleness is fine, RELAXED suffices.

### Usage Patterns in the Codebase

- **Stop flags** (`stop_thread`, `request_stop`, `admin_stop`, `stat_stop`): Writer uses `mt_atomic32_set_release(flag, 1)`, polling loop uses `mt_atomic32_read_acquire(flag) == 0`. This ensures the loop sees the signal promptly and that any data written before the signal is visible.
- **Scheduler started/stopped** (`sch->started`, `sch->stopped`): Same acquire/release pattern — the thread that sets `stopped=1` uses release; the thread polling it uses acquire.
- **Instance state flags** (`instance_started`, `instance_in_reset`, `instance_aborted`): Read with `mt_atomic32_read_acquire` in inline helpers `mt_started()`, `mt_in_reset()`, `mt_aborted()`.
- **Frame refcnt** (`st_frame_trans.refcnt`): Uses `mt_atomic32_dec_and_test` (ACQ_REL) for destroy-on-zero — ensures all prior DMA accesses are visible before cleanup proceeds.
- **Scheduler refcnt** (`sch->ref_cnt`): Same `mt_atomic32_dec_and_test` pattern in `mt_sch_put()` — when refcnt hits zero, the scheduler is stopped and freed.
- **Session counts** (`st20_tx_sessions_cnt` etc.): RELAXED inc/dec — these are always modified under the manager mutex, so the mutex provides ordering.
- **Stats counters** (`stat_frame_cnt`, `stat_frames_received`, `cbs_build_timeout`, etc.): RELAXED — incremented in hot path, read periodically by stats thread. Stale reads are acceptable.
- **Plugin device refcnt** (`st22_encode_dev_impl.ref_cnt` etc.): RELAXED — protected by plugin manager mutex during attach/detach.
- **Transmitter started/clients** (`transmitter_started`, `transmitter_clients` in audio/anc/fmd managers): RELAXED — guarded by manager spinlock.


## The Shared Queue Contention Design

Shared TX queue (`TSQ`) serializes all sessions onto one NIC queue using a spinlock:
```c
spinlock_lock(tsq_lock);
rte_eth_tx_burst(queue, mbufs, count);
spinlock_unlock(tsq_lock);
```

This is a **hot contention point** by design — the trade-off is fewer NIC queues (some NICs have limited queues) at the cost of serialized TX. When you see TSQ performance issues, the answer is usually "use dedicated queues instead."

Shared RX queue (`RSQ`) is less contentious: one thread polls the shared queue and dispatches to per-session `rte_ring` buffers. Each session then drains its ring without contention.

## Race Conditions to Know About

1. **Create/destroy during active traffic**: The manager mutex protects the session array, but packets in flight during destroy are handled by checking `session == NULL` after get. The session is set to NULL in the array **before** the actual free, so tasklets see NULL and skip.

2. **Port reset guard**: A port reset (link down/up) requires quiescing all queues. The admin thread signals `MTL_FLAG_PORT_RESET` and waits for all sessions to notice it. Sessions check this flag in their tasklet handler and skip TX/RX if set. The `mt_interface.resetting` atomic uses acquire/release ordering to ensure visibility across threads. This is a cooperative protocol — there's no hard fence.

3. **Stats thread reading mid-update**: Stats counters are not locked. The stats thread may read a partially-updated counter. This is acceptable — stats are advisory, not authoritative.

## Debugging Concurrency Issues

- **Deadlock**: If two schedulers stop making progress, check lock ordering. Run with `MT_LOG_LEVEL=DBG` to see lock acquisition traces.
- **Use-after-free**: Usually a refcnt bug — a frame was freed while NIC still had DMA references. ASAN builds catch this.
- **Silent data corruption**: Usually a missing get/put — two tasklets modifying the same session simultaneously. Add assertions to check lock is held.
- **Performance cliff**: If throughput suddenly drops when adding sessions, the shared queue spinlock is likely saturated. Switch to dedicated queues.
