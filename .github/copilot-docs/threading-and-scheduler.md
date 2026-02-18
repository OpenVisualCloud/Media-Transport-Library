# Threading & Scheduler — Design Knowledge

## The Core Insight: Why Tasklets Exist

MTL needs to service potentially hundreds of media sessions (video, audio, ancillary) with microsecond-level timing. Creating a thread per session would be wasteful and would destroy cache locality. Instead, MTL uses a **cooperative tasklet model**: a small number of scheduler threads each poll a set of registered tasklet callbacks in a tight loop.

This means:
- **Tasklets are not threads** — they are function pointers called repeatedly by a scheduler thread
- **Tasklets must never block** — one blocked tasklet starves all others on the same scheduler
- **Tasklets share CPU time cooperatively** — they signal "I have work" (positive return) or "I'm idle" (return 0) to let the scheduler decide whether to sleep
- The scheduler-to-tasklet relationship is 1:many. A scheduler is assigned to a DPDK lcore (pinned) or a pthread (unpinned)

## Why Two Scheduler Modes?

- **Lcore mode** (default): DPDK lcores are CPU-pinned threads. Best for deterministic latency. Uses `rte_eal_remote_launch()`.
- **Pthread mode** (`MTL_FLAG_TASKLET_THREAD`): Regular pthreads, not pinned. Used when the caller can't dedicate cores or when running in containers without CPU isolation.

The scheduler loop code is identical in both modes — only thread creation differs.

## How Sessions Get Assigned to Schedulers

Sessions don't pick schedulers — the **quota system** decides. Each session type has a "weight" measured in "1080p-equivalents." A 4K60 session takes ~4× the quota of 1080p60. The system:
1. Searches existing schedulers for one with enough remaining quota on the right NUMA node
2. If none found, allocates a new scheduler from the pool (up to 18 total)
3. As sessions are freed, quota is returned. When a scheduler's refcount hits 0, it stops.

**Why this matters**: If you add a new session type, you MUST define its quota correctly or you'll overload schedulers. Audio and ancillary use fractional quota because they're lightweight.

## The Sleep/Wake Design Problem

Pure busy-polling wastes CPU when there's no work. Pure sleeping adds latency. MTL solves this with an adaptive sleep:
- Each tasklet declares `advice_sleep_us` — "how long I can tolerate being asleep"
- The scheduler takes the minimum across all tasklets as its sleep target
- Below 200µs it just yields (context switch cost would exceed the sleep)
- Above 200µs it uses `pthread_cond_timedwait()` which works in both lcore and pthread modes (DPDK lcores are pthreads), allowing sub-millisecond wake via `sch_sleep_wakeup()`
- `MTL_FLAG_TASKLET_SLEEP` does NOT require `MTL_FLAG_TASKLET_THREAD` — sleep works in both scheduler modes

**Gotcha**: If you register a tasklet with `advice_sleep_us = 0`, the scheduler will never sleep — 100% CPU usage.

## Builder vs Transmitter: The TX Video Split

TX video has TWO separate tasklets per scheduler, connected by an `rte_ring`:
1. **Builder** — reads frames from app, constructs RTP packets (header + extbuf payload), enqueues to ring
2. **Transmitter** — dequeues from ring, handles pacing timing, bursts to NIC

Why split? Because **pacing is timing-critical but packet building is not**. The ring decouples them so the transmitter can respond to precise timing windows without being blocked by packet construction. The ring also absorbs jitter — the builder can run ahead.

## CNI: The Control Plane Escape Hatch

DPDK bypasses the kernel, but MTL still needs ARP, DHCP, PTP, and IGMP. The CNI subsystem provides a "system queue" on each NIC that receives these non-media packets. Two modes:
- **Tasklet mode** (when PTP enabled): CNI runs as a tasklet on a scheduler — lowest latency for PTP
- **Thread mode** (default): CNI runs in its own pthread — simpler but adds latency

Packets the kernel needs (unrecognized protocols) are forwarded to a TAP/virtio_user interface that bridges back to the kernel network stack.

## Session Migration

When `MTL_FLAG_TX_VIDEO_MIGRATE` is set, the admin thread monitors scheduler CPU load. If a scheduler exceeds 95% busy, it moves the last session to a less-loaded scheduler. This requires:
1. Locking BOTH source and destination manager mutexes (ordering: target first, then source)
2. Moving the session pointer atomically (NULL old slot, set new slot)
3. Releasing quota from the old scheduler

**Why "last session"**: It's the easiest to move because no reordering is needed in the manager array.

## Thread Inventory (for reference)

| Thread | Purpose | Key Detail |
|--------|---------|------------|
| Scheduler (×up-to-18) | Tasklet polling | Either lcore-pinned or pthread |
| TSC Calibration (×1) | Measures TSC frequency at boot | Joins before any session created — blocking dependency |
| Admin (×1) | Watchdog, migration | Periodic, uses alarm to wake |
| Statistics (×1) | Stat dump | Configurable period via `dump_period_us` |
| CNI (×1) | ARP/PTP/DHCP/IGMP | Tasklet or thread mode based on PTP |
| Socket TX/RX (×4 each) | Kernel socket backend | Only when using `kernel:` backend |
| SRSS (×1 per instance) | Shared RSS polling | Only for NICs without flow director |
