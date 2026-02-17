# Concurrency & Locking — Design Knowledge

## The Two-Tier Locking Architecture

MTL has a strict two-tier locking design driven by a single constraint: **tasklets must never block**.

| Tier | Lock Type | Where Used | Why |
|------|-----------|------------|-----|
| **Data plane** | `rte_spinlock_t` | Tasklet handlers, shared queues, transmitter | Tasklets run in busy-poll loops — a mutex would context-switch and destroy latency |
| **Control plane** | `pthread_mutex_t` | Session create/destroy, queue alloc/free, manager operations | These happen infrequently, may take milliseconds, and need to interoperate with condvars |

**The cardinal rule**: Never hold a spinlock across a blocking operation. Never use a mutex inside a tasklet.

## The Session Get/Put Pattern

This is the most important concurrency pattern in the codebase. Every tasklet handler does:

```
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

## Atomic Operations

MTL uses `mt_atomic32_t` (defined in `lib/src/mt_atomic.h`) for lightweight counters where locking is overkill. This is a project-owned wrapper over C11 `__atomic_*` GCC/Clang builtins, replacing the deprecated DPDK `rte_atomic32_t` API.

### Memory Ordering Policy

The API provides **default (RELAXED)** and **ordered** variants. Choosing the right one depends on what the atomic protects:

| API | Ordering | When to use |
|-----|----------|-------------|
| `mt_atomic32_read` | RELAXED | Stats, counts under external lock, diagnostics |
| `mt_atomic32_read_acquire` | ACQUIRE | Polling stop flags, checking refcnt before frame reuse, reading `instance_started` |
| `mt_atomic32_set` | RELAXED | Init-time zero, stat resets, values under external lock |
| `mt_atomic32_set_release` | RELEASE | Signaling stop flags, publishing data (mac_ready), setting `instance_started` |
| `mt_atomic32_inc` | RELAXED | Stats counters (hot path!), session counts under mutex, refcount inc under spinlock |
| `mt_atomic32_dec` | RELAXED | Session counts under mutex, shared queue entry counts under spinlock |
| `mt_atomic32_dec_release` | RELEASE | Frame refcnt release — ensures all frame data writes are visible before counter drop |
| `mt_atomic32_dec_and_test` | ACQ_REL | Destroy-on-zero pattern (scheduler refcnt) |

**Design rationale**: The default is RELAXED because the majority of atomics are either stats counters in the hot path or values protected by an external lock. Only three patterns need ordering:

1. **Reference counting** (frame refcnt): `dec_release` + `read_acquire` form an acquire/release pair. The release on dec ensures frame data writes are visible; the acquire on read ensures the reusing thread sees them.
2. **Flag signaling** (stop threads, lifecycle, publish): `set_release` + `read_acquire` form a publish/consume pair. The release ensures prior setup/teardown is visible when the reader sees the flag change.
3. **Destroy-on-zero** (scheduler refcnt): `dec_and_test` with ACQ_REL — the release orders prior accesses, and if zero the implicit acquire orders subsequent cleanup.

**Gotcha**: `inc` is always RELAXED because every inc site either (a) runs in a hot-path stats counter with no ordering dependency, or (b) operates under an external lock that provides the necessary ordering. If you add a new use of `mt_atomic32_inc` that is NOT under a lock and DOES gate access to shared data, you need a new `inc_acquire` variant.

**Why not `rte_atomic32_t`?** DPDK deprecated it in 21.11 and plans to remove it. MTL's own `mt_atomic32_t` decouples from DPDK's deprecation cycle and uses portable C11 builtins.

## The Shared Queue Contention Design

Shared TX queue (`TSQ`) serializes all sessions onto one NIC queue using a spinlock:
```
spinlock_lock(tsq_lock);
rte_eth_tx_burst(queue, mbufs, count);
spinlock_unlock(tsq_lock);
```

This is a **hot contention point** by design — the tradeoff is fewer NIC queues (some NICs have limited queues) at the cost of serialized TX. When you see TSQ performance issues, the answer is usually "use dedicated queues instead."

Shared RX queue (`RSQ`) is less contentious: one thread polls the shared queue and dispatches to per-session `rte_ring` buffers. Each session then drains its ring without contention.

## Race Conditions to Know About

1. **Create/destroy during active traffic**: The manager mutex protects the session array, but packets in flight during destroy are handled by checking `session == NULL` after get. The session is set to NULL in the array **before** the actual free, so tasklets see NULL and skip.

2. **Port reset guard**: A port reset (link down/up) requires quiescing all queues. The admin thread signals `MTL_FLAG_PORT_RESET` and waits for all sessions to notice it. Sessions check this flag in their tasklet handler and skip TX/RX if set. This is a cooperative protocol — there's no hard fence.

3. **Stats thread reading mid-update**: Stats counters are not locked. The stats thread may read a partially-updated counter. This is acceptable — stats are advisory, not authoritative.

## Debugging Concurrency Issues

- **Deadlock**: If two schedulers stop making progress, check lock ordering. Run with `MT_LOG_LEVEL=DBG` to see lock acquisition traces.
- **Use-after-free**: Usually a refcnt bug — a frame was freed while NIC still had DMA references. ASAN builds catch this.
- **Silent data corruption**: Usually a missing get/put — two tasklets modifying the same session simultaneously. Add assertions to check lock is held.
- **Performance cliff**: If throughput suddenly drops when adding sessions, the shared queue spinlock is likely saturated. Switch to dedicated queues.
