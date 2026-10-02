# 04 — Threading, execution contexts and progress

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-THR-1…5, R-CMP-7, R-LIFE-3 |
| Research | [R03 scheduler & threading](research/03-scheduler-threading.md) (primary), [R09 libfabric §7](research/09-libfabric.md), [R11 io_uring §4.3](research/11-media-io-prior-art.md); reviews [C1](reviews/C1-realtime-feasibility.md), [C5 §4](reviews/C5-adversarial-user-review.md) and the [C5 response](reviews/C5-response.md) (A9, A10 normative) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

The maintainer's hard requirement: **MTL runs tasklets on pinned cores; everything called
from the API must run outside those tasklets and must never block the pinned cores.**
This document turns it into rules that can be checked in code review and in tests.

Revision 3 changes, in short: data-plane calls from a busy-loop thread take a named
inline-safe path (§3.3); commands come in two classes with an ack even when idle (§3.2);
destroy has a defined path for a stalled queue (§4.5); the lease slot is split by writer
(§4.1); a shared CQ has a ready summary (§4.3); waiters are per target and the MP-safe verbs
are named (§4.2, §5); the RX side gets a deadline hook (§4.4); the time base has a refresh,
a servo and slewing (§8); `MTL_FLAG_TASKLET_THREAD` is answered (§7.1). Numbers that were
never measured are now marked as spike outputs. Items marked **r3** changed in this
revision.

## 1. What is wrong today (short)

From research note 03, all **[verified]** at `main` @ `545a266a`:

| # | Today | Why it breaks the requirement |
|---|---|---|
| H1 | Almost every user callback (`get_next_frame`, `notify_*`, `query_ext_frame`, `uframe_pg_callback`, `notify_detected`) runs on the pinned tasklet **with the session spinlock held**¹. `get_next_frame` is polled every loop iteration while the app has nothing ready. | user code on pinned cores; one slow callback stalls every session on the core |
| H2 | Calling a same-session stats getter from inside a callback spins forever on that non-recursive spinlock (`st_tx_video_session.c:4760`, `st20_pipeline_tx.c:1311`). | self-deadlock of a pinned core |
| H3 | Pipeline BLOCK_GET: the tasklet does `pthread_mutex_lock` + `pthread_cond_signal` on every frame event; the app holds that mutex while scanning, so the pinned core can sleep in `FUTEX_WAIT` (`st20_pipeline_tx.c:29-45`, `:774-789`). | syscall and priority inversion on a pinned core |
| H4 | TX-hang recovery runs on the transmitter tasklet: queue re-acquire under a pthread mutex, mempool re-create, busy flush, INFO logs (`st_video_transmitter.c:681` → `st_tx_video_session.c:4231-4332`). | control-plane work on the data plane |
| H5 | RX auto-detect allocates on the tasklet (`rv_init_sw`, `mt_rte_zmalloc_socket`). | allocation on the data plane |
| H6 | `update_destination` holds the session spinlock across an ARP wait of up to 60 s (`st_tx_video_session.c:3848-3855`, `mt_arp.c:171-199`). | session silently stops transmitting |
| H7 | `ptp_get_time_fn`, built-in-PTP PHC reads (`rte_eth_timesync_read_time`) and the log printer (with `localtime_r`) run on the hot path. | user code, driver calls and libc on pinned cores |
| H8 | TX `notify_frame_done` can fire on the transmitter tasklet, the builder, the app thread, a plugin thread; migration changes the lcore at runtime. | no stable thread identity; single-producer queues are not safe |
| H9 | Tasklets `trylock` the session spinlock; any app, admin or stat thread holding it (stats read, admin CPU-busy scan every cycle, `mt_admin.c:30`) makes the tasklet skip that session for an iteration. | pacing hazard under SW pacing `[C1 #6]` |

¹ `st_tx_video_session.c:2682`, `st_rx_video_session.c:3516`.

## 2. Execution contexts in the new design

| Context | Pinned? | May run | Must never |
|---|---|---|---|
| **TK** scheduler tasklet (lcore or `MTL_FLAG_TASKLET_THREAD`)² | yes (lcore mode) | packet build, pacing, RX reassembly, mempool get/put, lock-free ring ops, the inline-safe DP subset (§3.3)² | block, sleep, take a lock an app thread can hold, allocate (§2.1), log (format or print), run app code (unless §6), make a syscall (except §2.2 backend I/O and, if accepted, the W2 wake §5.2) |
| **WK** library worker threads (not on scheduler cores) | no | pixel conversion when not in the caller, recovery rebuilds, auto-detect re-init, ARP/flow/IGMP work, queue stop/start after a stall (§4.5), deferred-destroy retire, pcap writes | spin on tasklet-owned state |
| **WA** waker thread (one per instance) | housekeeping cpuset (§5.2) | wake-up syscalls for armed waiters whose completion was produced on a tasklet | anything else |
| **AD** admin / stat / CNI / EAL alarm threads (exist today) | no | periodic work, PTP servo, time-base publication (§8), NIC counter polling, link monitoring (09), event posting | take a lock a tasklet takes (the admin CPU-busy scan must stop taking S, H9) |
| **APP** application threads | app's choice | every public API call; conversion in the caller when granted | — (a busy-loop thread is not an APP thread, §3.3) |

² TK also covers every other library busy loop (RX packet lcore, TAP lcore) and user tasklets
registered with `mtl_sch_register_tasklet` (`mt_sch.c:925-966`). It may also do
relaxed/release stores, fences, and one atomic RMW on a line the app rarely writes.

### 2.1 What "never allocate" means

Tasklets already take mbufs from mempools for every packet and allocate one mbuf per
frame at TX start (`st_tx_video_session.c:4350`). The rule is therefore precise: **no
`malloc`/`calloc`/`rte_malloc*`, no mempool/ring/heap creation or destruction** on a
tasklet; mempool get/put and ring enqueue/dequeue are allowed. In debug builds this is
enforced (§3.4). Mempool contention between tasklets and app threads is §9.1.

### 2.2 Syscalls on tasklets, per backend

"The tasklet makes no syscall" holds for the DPDK PMD backend only. Two backends need
I/O syscalls on the tasklet by design `[C1 #7]`:

| Backend | Syscalls on the tasklet | Status in the new API |
|---|---|---|
| DPDK PMD | none on the data path | **promise** (G-39), in lcore mode and with W3; the W2 wake option adds one `write()` per wake (§5.2) and is reported |
| Kernel socket | `sendto`/`sendmsg` in `mt_tx_socket_burst` (`datapath/mt_dp_socket.c:421-445`) | reported as a granted property `backend_syscalls_on_tasklet`; G-39 scoped out |
| Native AF_XDP | `send()` kick (`dev/mt_af_xdp.c:522-526`) | same |
| Built-in PTP time reads | `rte_eth_timesync_read_time` per `mt_get_ptp_time` (a PMD register read; on iavf possibly a PF round-trip **[inferred]**) | replaced by the published time base (§8) |

## 3. Call classes

Every public function carries exactly one class, stated in the header next to its
prototype with an annotation macro (`MTL_API_CP`, `MTL_API_DP`, `MTL_API_DPC`,
`MTL_API_WT`, `MTL_API_AS`) and enforced in debug builds (§3.4, G-50). `[R03 §7.5]`

| Class | Contract | Examples |
|---|---|---|
| **CP** control plane | APP threads only. May block, allocate, do IPC, log. Thread-safe with respect to each other on the same object (internally serialised). Never holds a tasklet-visible lock while blocking. | create, attach, start, stop, destroy, `discard_queued`, `update_flows`, `set_leg_enabled`, `reconfigure`, region import, capability and info queries |
| **DP** data plane | APP threads, and the inline-safe subset from busy-loop threads (§3.3). Lock-free with respect to tasklets, bounded, no allocation, no syscall except non-blocking eventfd `read`/`write` on app-side wait objects (§5.1, §5.3), never from a busy-loop thread. Returns `-MTL_EAGAIN`, never waits (§4.2). | `mtl_tx_acquire` (timeout 0), `mtl_tx_submit` (no conversion)³ |
| **DPC** data plane with caller-context work | as DP, but bounded by the unit size rather than O(1): the call performs conversion, zero-fill of missing RX ranges, or an ST30/40/41 copy in the caller. The cost is reported (stats `caller_work_ns`). | `mtl_tx_submit` / `mtl_rx_dequeue` when the granted `convert_context = CALLER`; `mtl_tx_write`; `mtl_rx_dequeue` with `rx_fill = ZERO_MISSING` |
| **WT** wait | APP threads only; refuses busy-loop threads with `-MTL_EDEADLK`. Bounded by a timeout⁴. Returns `-MTL_ETIMEDOUT`, or `-MTL_ECANCELED` / `-MTL_ESHUTDOWN` / `-MTL_EIO` (07 §5). Built only on DP calls + the armed-waiter protocol (§5). | `mtl_tx_acquire` / `mtl_rx_dequeue` / `mtl_tx_reap` / `mtl_cq_read` / `mtl_eq_read` with a timeout, `mtl_session_wait` |
| **AS** async-signal-safe | callable from a signal handler: atomics and at most one `write()` of an eventfd (the waker's; the waker fans out the wake-ups to every affected wait object, §5.4). | `mtl_session_interrupt`, `mtl_cq_interrupt`, `mtl_eq_interrupt`, `mtl_instance_interrupt_all` |

³ Also `mtl_tx_publish`, `mtl_tx_release`, `mtl_tx_withdraw`, `mtl_rx_dequeue` (timeout 0),
`mtl_rx_release`, `mtl_cq_read` / `mtl_eq_read` (timeout 0), `*_trywait`, stats/status
snapshots (`get_stats`, port/instance status), `mtl_session_get_state`, `mtl_time_now`.

⁴ `MTL_TIMEOUT_INFINITE` = −1; 0 = non-blocking, which makes the call DP (or DPC). Every call
that takes a timeout carries the single WT annotation, including DPC-work calls
(`mtl_tx_write`, a converting `mtl_rx_dequeue`) (10, G-50). "WT is DP when `timeout == 0`" is
checked dynamically in debug builds (§3.4).

### 3.1 Why DP calls never wait for a tasklet

A DP call that needs something from the tasklet (a free buffer, a result) fails with
`-MTL_EAGAIN`. It never spins on a tasklet-owned lock and never waits for a tasklet to
acknowledge. Conversely a tasklet never waits for an application thread: an empty
submission means "idle", and completions cannot overflow because they are materialised
on the reader's side (§4.1).

### 3.2 Commands: two classes, always acknowledged (r3)

Control operations that change tasklet-owned state are **commands** posted to the session's
command word and acknowledged by the tasklet `[C5 §4.2]`. Revision 2 applied every command
"at the first unit boundary", which never comes for an RX session without packets, makes a
TX stop wait up to 1 s for a launch time (`video_trs_rl_target_reached` accepts
`delta ≤ NS_PER_S`, `st_video_transmitter.c:180-199`), and never wakes a sleeping
scheduler. Revision 3:

| Class | Commands | Applied by the tasklet |
|---|---|---|
| **Immediate** | stop (DRAIN begin, FLUSH), `discard_queued`, detach (stop, destroy, ERROR entry) | on its next visit of the session, outside a packet burst — every iteration, whether or not a unit is in progress |
| **Boundary** | `update_flows`, `set_leg_enabled` (a leg never switches mid-unit), `reconfigure` of timing fields that change the packet grid, `discard_queued` with `MTL_DISCARD_REBASE` | at the first unit boundary ≥ the activation (media index k, TAI t, or NOW = the next boundary) |

- **Where the check runs.** Each session tasklet visit loads the command sequence once
  (relaxed) and compares it with the last acknowledged one. The per-session loop already
  runs every iteration for every attached session, with or without packets: the RX video
  handler visits each session (`rvs_pkt_rx_tasklet_handler`, `st_rx_video_session.c:3506-3535`)
  and the TX transmitter returns to the scheduler while it waits for a launch
  (`st_video_transmitter.c:188-191`).
- **A TX waiting for its launch.** Stop(FLUSH) and discard are immediate, so the unit that is
  waiting (built packets in the session ring, none handed to the NIC) is flushed: the ring
  is cleaned as recovery does today (`mt_ring_dequeue_clean`, `st_tx_video_session.c:4250-4253`)
  and the unit's outcome is `FLUSHED/STOP_FLUSH` or `FLUSHED/DISCARD` (03 §3.4).
- **A sleeping scheduler.** Posting a command calls `sch_sleep_wakeup` (`mt_sch.c:52-56`)
  when the scheduler has `MTL_FLAG_TASKLET_SLEEP`; otherwise the command waits up to the
  1 s safety-net sleep (`mt_sch.c:86-96`). The mutex and condvar are taken by the posting CP
  thread, never by the tasklet.
- **A detached session** (CREATED, STOPPED, ERROR after detach): no tasklet walks it, so the
  CP applies the command itself; there is nothing to acknowledge.
- **The ack.** The tasklet stores the acknowledged sequence (release) plus a result (for
  boundary commands, the first media index on the new state). The CP waits by polling with a
  back-off (50 µs growing to 1 ms); the tasklet never makes a syscall for it.
- **Ack timeout.** `mtl_instance_params.cmd_ack_timeout_ns` (default 100 ms). On expiry the
  session enters ERROR with reason `CMD_TIMEOUT` (03 §3.6), and the CP falls back to today's
  lock-based detach: it takes the session spinlock, which the tasklet only `try_get`s
  (`st_tx_video_session.c:2682`, `:3798-3816`), so the detach succeeds as soon as the tasklet
  is outside the session. If even that fails within a second timeout, the session is
  quarantined (its memory is never freed; `get_status().reason` stays `CMD_TIMEOUT`). There is
  no infinite wait.
- Today detach is exactly that lock, not a handshake (`tv_mgr_detach`,
  `st_tx_video_session.c:3798-3816`), which is why it works while the session is idle; the
  command path keeps that property through the CP-applies rule.

Boundary commands keep the revision-2 shape for flow updates:

```text
APP (CP)                                   WK / AD                          TK
build new header templates for every       ARP resolve, flow create,
leg off to the side (no session lock)      IGMP join, RTCP header update,
                                           socket-backend queue re-create
                                           all-or-nothing across legs; on failure
                                           nothing is committed (09 §7.1)
                                           publish {templates, gen,
                                           activation} ───────────── command ──▶ at the first unit
                                                                                  boundary ≥ activation:
                                                                                  swap every leg at once
wait for ack (bounded) ◀──────────────────────────────────────────────────── ack {gen, first index on new}
old resources released by WK after ack
```

- The builder copies the header template into every packet (`st_tx_video_session.c:1062`,
  `:1107`, `:1213`); a double-buffered template swapped at a frame boundary for both legs
  together replaces the in-place rewrite under the spinlock (H6). `[C1 #10]`
- Packets already in rings and NIC descriptors still carry the old header; the ack
  therefore reports **the first media index sent to the new destination**, which also
  matches NMOS IS-05 scheduled activation.
- Backends where the destination lives in the queue (kernel-socket GSO sends to
  `t->send_addr`, fixed at queue creation, `datapath/mt_dp_socket.c:236`) need a new
  queue created by the worker and swapped at the boundary. The RTCP TX header (copied
  from `s_hdr` at init, `:1023-1025`) must be updated too — both are side findings
  today (SF-36, SF-37).
- RX `update_flows` adds the new flow rule to the same queue before removing the old one;
  with an `AT_TAI t` activation, packets matching the new rule are accepted for units whose
  media time ≥ t (03 §3.5); IGMP work runs on the worker.

### 3.3 Busy-loop threads: the inline-safe DP subset (r3)

Every library busy loop — the scheduler loop (and therefore user tasklets), the RX packet
lcore (`USE_MULTI_THREADS`), the TAP lcore — sets a thread-local `mt_in_busy_loop` flag.
Revision 2 checked it only in CP and WT entry points, so a DP `mtl_rx_dequeue` from a user
tasklet (the zero-hop forwarder of §6) could block the pinned core on the reaper lock held by
a preempted app thread, and its eventfd drain broke G-39 `[C5 §4.1]`. Now (A9):

| From a busy-loop thread | Behaviour |
|---|---|
| `mtl_tx_acquire` / `_buffer`, `mtl_rx_dequeue`, `mtl_tx_reap`, `mtl_cq_read` with `timeout == 0`; `mtl_tx_submit` (no conversion), `mtl_tx_publish`, `mtl_tx_release`, `mtl_rx_release`; `get_state`, `get_stats` | **inline-safe path**: the reaper lock is only *trylocked* (contended → `-MTL_EAGAIN`); no eventfd drain or write (the next APP-thread call drains, the waker wakes); no log, no alloc |
| any WT call with `timeout ≠ 0`, any DPC call that would do caller work, any CP call | `-MTL_EDEADLK` |
| AS calls (`*_interrupt`, `mtl_instance_interrupt_all`) | allowed: a signal can be delivered to any thread, including a pinned lcore thread, and the handler must still work; the call is atomics plus one eventfd `write()` from a context that is already in a signal handler |

This also turns today's silent self-deadlock (H2) and the 1 s sleep when a tasklet
unregisters itself (`mt_sch.c:886-902`) `[R03 §4.1]` into a clear error `[C1 #21]`.

### 3.4 Enforcement in debug builds (A9)

- Every entry point sets a thread-local "current class" for its duration.
- `mt_rte_zmalloc*`, `mt_pthread_mutex_lock`, `mt_sleep_ms`, and `info()`/`warn()` assert that
  the current class is not DP, DPC or AS, and that the thread is not a busy-loop thread.
- A WT call with `timeout == 0` runs under class DP.
- A signal-safety test raises the signal inside every CP call while a `malloc` and
  `pthread_mutex_lock` interposer is armed, and calls every AS function from the handler.

## 4. Data-path structures and their concurrency

### 4.1 The lease table and consumer-side completion

Review C1 showed that "the free callback marks DONE, the session tasklet publishes"
cannot be built over the pipelines: they have no tasklet of their own, the builder calls
into the pipeline only while waiting for a frame, and completions come from several
different contexts `[C1 #1]`. The design needs no L2 tasklet at all.

**r3: the slot is split by writer** `[C5 §4.7]`. Revision 2 put state, generation, cookie and
the result in one slot line, which the completing tasklet and the app both write — the
`st20_pipeline_tx.h:35-42` false-sharing pattern it condemns in §9.

```text
                 per-session LEASE TABLE (hugepage memory, scheduler's NUMA node)
  slot i, tasklet half (own cache line):  { state (atomic), result record (status, reason, times, pkts),
                                            RX: hold count (atomic u8, 05 §5.4; +1 at a TX submit
                                            with .hold, −1 at that unit's terminal outcome) }
  slot i, app half     (own cache line):  { lease generation, submission seq, cookie,
                                            TX: the held RX lease (.hold), control }

  COMPLETING CONTEXT (§4.1.1)                                    READER (APP thread; reaper lock unless
  ─────────────────────────────                                  MTL_SESSION_SINGLE_READER)
  1. write the result record into the tasklet half                 reap / cq_read / dequeue scan the
  2. store state = DONE           (release)                        tasklet halves: DONE/READY → copy the
  3. atomic_thread_fence(seq_cst)                                  result (published in submission order)
  4. load the target's armed word (relaxed)                        into the unread ring or the caller →
  5. if armed: tasklet → set this session's bit in its             FREE
     scheduler's wake word (or the shared CQ's summary, §4.3);
     other context → wake the waiter directly
```

The app writes the state word only at hand-over points (acquire, submit, free) — one line
transfer per hand-over, which is inherent; the generation, cookie and control words the app
writes never share a line with the result the tasklet writes.

Properties:

- **Single producer per slot.** A slot is completed by exactly one context (the engine hook
  claims completion with a CAS 1 → 0, which also closes the double-DONE window between the
  free callback and recovery, `st_tx_video_session.c:127-135` vs `:4295-4300` `[C1 #11]`).
  Results are *materialised* by the reader, so there is no CQ ring the tasklet writes and
  nothing that can overflow.
- **r3: slot reuse is decoupled from result order** `[C5 §4.11]`. In NONE, `acquire` takes a
  DONE slot directly, out of order. In ALL/EXCEPTIONS the reader copies each DONE result into
  the per-session **unread-results ring** (capacity = pool count) and frees the slot at
  once; if the ring is full the slot stays DONE with its result, and `acquire` reports
  `blocked_on = RESULTS`. A unit that completes early therefore never waits for its
  predecessors. The ring publishes results in **submission order** (indexed by `seq`): a
  result whose predecessors are not yet done waits in the ring, its slot already FREE, so
  G-09 holds without head-of-line blocking of slots (03 §4.1 "Order").
- **Migration-proof.** No state depends on which lcore produced the completion.
- **Tasklet cost per unit:** one release store, one seq_cst fence, one relaxed load; the
  bit-set only when a waiter is armed. The fence runs inside the PMD free callback inside
  `rte_eth_tx_burst`, where an `mfence` drains the store buffer ahead of descriptor writes;
  revision 2's "30–100 cycles" was never measured and is now a spike S3 output
  `[C5 §4.11 "Unverified numbers"]`.
- **The reaper lock** serialises the application threads that read results (reap,
  `cq_read`, `dequeue`). It is a plain mutex or spinlock between application threads;
  tasklets only ever trylock it (§3.3). `acquire`, `tx_release` and `rx_release` never take
  it (§4.2). With `MTL_SESSION_SINGLE_READER` it is elided.
- **RMW budget per unit** `[C5 §4.11]`: today ≈ 7 atomic RMWs per frame (`get_frame` +
  `put_frame`); the new path adds the acquire CAS, the submit CAS (MT_SUBMIT only), the free
  store, the in-flight counter (2 per call) and the reaper lock (2 per read). With
  `MTL_SESSION_SINGLE_READER` and without `MTL_SESSION_MT_SUBMIT` the reaper lock and the
  in-flight counter are elided (03 §2.3), leaving the acquire CAS and the completion hook's
  CAS. The count at 8 kHz × 512 audio sessions per scheduler is a spike S3 output.
- **Progressive RX** progress entries coalesce: at most one pending `ready_rows` per unit is
  kept, updated in place, so a progressive session never needs more than one unread entry per
  slot plus the final one.

#### 4.1.1 Every completing context (r3)

`[C5 §4.11 "Completing contexts"]` — each needs the §4.1 protocol and, if it can run while a
waiter is armed, a wake path:

| Context | Where today | Wake path |
|---|---|---|
| PMD free callback inside `rte_eth_tx_burst`, on the transmitter tasklet (chain mode) | `tv_frame_free_cb`, `st_tx_video_session.c:116-143`, via `sh_info` (`:235-237`, `:1295-1298`) | scheduler wake word |
| builder, no-chain ST20 and ST22 ("done" at the end of build) | `st_tx_video_session.c:2130-2134`, `:2655-2659` | scheduler wake word; the "last packet handed" hook of §4.4 moves the completion later |
| TX recovery (on the transmitter today, on a worker after Q-THR-9) | `st_tx_video_session.c:4293-4301` | tasklet: wake word; worker: direct |
| **the destroying app thread** via `tv_uinit_hw → mt_txq_flush`, whose pad bursts run `tv_frame_free_cb` | `st_tx_video_session.c:2722-2728`, `dev/mt_dev.c:1782-1799` | removed: destroy uses the §4.5 path instead of the busy flush |
| queue stop/start after a stall, on a worker (new, §4.5) | — | direct |
| the app thread inside `put_frame` / `put_ext_frame` when converting internally | `st20_pipeline_tx.c:991-1000` | direct |
| a plugin converter or encoder thread | `st20_pipeline_tx.c:380-385` | direct |
| RX tasklet, with the session spinlock held | `st_rx_video_session.c:3516` → `rv_slot_full_frame` (`:1850-1858`) | scheduler wake word |
| **the RX packet lcore** (`USE_MULTI_THREADS`) | `rv_pkt_lcore_func`, `st_rx_video_session.c:2470-2482` | **its own wake word**, scanned by the waker like a scheduler's (it has none today) |
| the DMA completion drain on RX | `rv_dma_dequeue` (on the RX tasklet) | scheduler wake word |
| **r3** the last-reference drop on a HELD RX slot: the TX completing context (any row above) or the app thread in `mtl_rx_release` | CAS to FREE (05 §5.4) over `rv_put_frame`'s atomic decrement (`st_rx_video_session.c:222-232`) | none for the slot (no waiter); in a deferred destroy the last drop posts the retire to a worker (03 §6.2) |

**Recovery never touches `sh_info`** `[C5 §4.11]`. Today recovery does
`rte_atomic32_dec(&frame->refcnt); rte_mbuf_ext_refcnt_set(&frame->sh_info, 0)`
(`st_tx_video_session.c:4299-4300`) for every frame with a reference; any chain mbuf of that
frame still in a descriptor (the other leg's queue, which recovery only pads,
`:4284-4288`) then decrements a zeroed counter on its next PMD free: DPDK's external-buffer
free callback fires on the 1 → 0 transition, so the counter wraps and the callback never
fires, or fires for a re-used frame **[inferred]**. The rule: recovery claims completion with
the §4.1 CAS, drops only the references it owns, and lets the PMD free the rest; `sh_info`
is written only at frame setup. This is a side finding today.

### 4.2 Concurrency contracts (r3)

`[C5 §4.11, §7.3, §3.6]`, A10. Revision 2 said "one submitting context per session", which
every pipeline with a thread boundary violates (`videoconvert ! queue ! mtl_st20p_tx`: the
upstream thread acquires frame N+1 while the sink thread submits frame N).

| Verb | Default contract | Option |
|---|---|---|
| `mtl_tx_acquire`, `mtl_tx_acquire_buffer` | **MP-safe** (a CAS on the slot) | — |
| `mtl_tx_release`, `mtl_rx_release` | **MP-safe** (a CAS on the slot, or a hold-count decrement) | — |
| `mtl_tx_submit`, `mtl_tx_publish`, `mtl_tx_write`, `mtl_rx_transfer` (the TX side) | one submitting context at a time ("externally serialised") | `MTL_SESSION_MT_SUBMIT`: MP-safe (a CAS on the submission tail); **required** for exported pools (03 §4.4) |
| `mtl_tx_withdraw` | as submit | as submit |
| `mtl_tx_reap`, `mtl_rx_dequeue`, `mtl_cq_read`, `mtl_eq_read` | MP-safe under the reaper lock | `MTL_SESSION_SINGLE_READER`: one reader context; the lock is elided |
| WT waits | several waiters per target allowed (§5.1) | — |

| Structure | Producer | Consumer |
|---|---|---|
| Submission hand-off (v1: the pipeline's own frame slots via `put_frame`; later a submission ring) | APP (one context, or MP with MT_SUBMIT) | TK |
| Lease table, tasklet half | completing context (release store) | APP reader |
| Lease table, app half | APP (acquire, submit, free) | TK reads cookie/seq at pick-up |
| Unread-results ring | APP reader | APP CQ reader (the same lock) |
| RX release / hold count | APP (any thread), TX completion | TK / reader |

Debug builds detect contract violations by **overlap**, not by thread identity: each
externally serialised entry point sets an owner flag with a CAS for the duration of the call
and asserts if it finds it set. A GstBufferPool export without MT_SUBMIT whose acquire and
submit never overlap is legitimate and must not trip it `[C3 P2-10]`.

### 4.3 Shared completion queues (r3)

A shared CQ is a **poll set** over the lease tables of its member sessions. Revision 2's
reader walked every member on every read and armed every member on every wait, which is
O(members): with up to 1024 RX audio sessions per scheduler (`ST_SCH_MAX_RX_AUDIO_SESSIONS`,
`st_header.h:50`) that is ≈ 1000 mostly remote line loads per `mtl_cq_read` `[C5 §4.6]`.

- **Ready summary.** The CQ owns one summary word per scheduler (and per RX packet lcore),
  each on its own cache line, with one bit per member session on that scheduler (a second
  level of words when a scheduler has more than 64 members). The completing context sets its
  session's bit (one atomic OR) after step 2 of §4.1; the reader exchanges each non-zero word
  with 0 and visits only the flagged members.
- **One armed word.** Sessions bound to a CQ point their armed-word pointer at the CQ's
  single armed word, so arming a wait on a 1000-member CQ is one store and one fence.
- Reads return entries tagged with their session (`hdr.session`), at most a bounded batch per
  member per read, round-robin across flagged members.
- Shared CQs are in v1 (Q-CMP-5 (a)); the summary costs one extra atomic OR per completion
  on sessions bound to a shared CQ only.

### 4.4 Engine hooks this requires (L0, Phase 1)

The consumer-side design is only as good as the completion signal. Reviews C1 and C5 found
that today's engines and pipelines do not provide one `[C1 #1, #2, #12, #20; C5 §4.9, §4.11]`.
The hooks are **new pipeline entry points that L2 calls** — the slot interface of 02 §2.2:
`st20p_tx_hold_slot` / `st20p_tx_release_slot`, `st20p_tx_submit_slot` (which carries `seq`,
assigned at submit), `st20p_tx_set_done_hook`, `st20p_tx_reclaim_queued`, and on RX
`st20p_rx_hold_ready` / `st20p_rx_release_slot` / `st20p_rx_force_complete`, with their
st22p/st30p/st40p twins — not L2 writing another layer's private state: the held state *is*
pipeline state, and these entry points are the extracted engine boundary `[C5 §4.11]`.

| Hook | Why | Where |
|---|---|---|
| **held slot state**: a finished slot is not FREE until L2 releases it | st20p stores FREE *before* calling `notify_frame_done` (`st20_pipeline_tx.c:264-287`), so `get_frame` can hand the slot out again before its result exists | st20p/st22p/st30p/st40p TX and RX |
| **once-only internal completion hook at transport done**, with status and meta, independent of `frame_done_cb_called` | converting sessions never get a transport-done callback (`:280-286`, `:380-385`, `:995-999`) | all four pipelines |
| **"last packet handed to the NIC" hook for copy paths** | no-chain ST20, ST22 and ST30 fire "done" at the end of *build*, before anything is sent (`st_tx_video_session.c:2130-2134`, `:2655-2659`; `st_tx_audio_session.c:923-930`) | transmitters |
| **rejected-at-pick-up callback** | the builder can claim a frame and return without building it (`refcnt != 0`, oversize user meta, `st_tx_video_session.c:1936-1953`), leaving the slot IN_TRANSMITTING forever | builder |
| **CAS reclaim `CONVERTED`/`READY` → FLUSHED** | stop(FLUSH) and discard need to take back queued units; pipelines only have `put_frame_abort` for IN_USER (`st20_pipeline_tx.c:902-927`) | all four pipelines |
| **r3 sequence at submit** | st20p numbers frames at `get_frame` (`st20_pipeline_tx.c:806-808`) and the builder picks the lowest sequence among CONVERTED (`tx_st20p_newest_available`, `:62-77`, misnamed: it returns the *oldest*); acquire A, B, submit B, A → sent A, B: G-08 fails for a GstBufferPool. The hook numbers at submit | all four TX pipelines |
| **idle descriptor recycling** | in chain mode a frame is DONE only when a *later* `rte_eth_tx_burst` recycles its descriptors, about `nb_tx_desc` packets later, and never while the session is idle — so the last frame's result never arrives and DRAIN cannot finish | transmitter: when frames are in flight and the build ring is empty, call the non-blocking `rte_eth_tx_done_cleanup`⁵ |
| **r3 RX deadline** | an RX frame completes only when full (`st_rx_video_session.c:1850-1858`) or evicted by a newer timestamp (`:1214-1221`), so the waker has no RX due time and DRAIN cannot deliver a partial unit `[C5 §4.9]` | RX tasklet, each iteration: the due time below; force-complete when due or on DRAIN/discard (`st20p_rx_force_complete`). Phase 1 |
| **r3 `notify_frame_ready` failure keeps the frame** | on `notify_frame_ready < 0` the frame is put back and silently lost (`st_rx_video_session.c:939-944`; ST22 `:1044-1049`) | RX: the L2 hook never fails; the unit becomes READY or is counted as `rx_reject` (side finding SF-45) |
| **r3 tasklet-side CAS READY → RECEIVING for `RECLAIM_OLDEST_READY`** | no pipeline has a reclaim of a READY slot today | RX pipelines (03 §4.2) |
| **r3 DMA-busy drop is counted** | `dma_previous_busy` silently drops the next frame's first packets (`st_rx_video_session.c:1202-1208`) with no result | RX: reject counter `MTL_RX_REJECT_DMA_BUSY` (08) and `pkts_*` in the unit result (side finding SF-46) |
| **r3 incomplete delivery always enabled internally** | incomplete frames are delivered only with `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (`st_rx_video_session.c:976-982`), and derive + `query_ext_frame` *require* it (`st20_pipeline_rx.c:589-594`) | L2 sets the flag on every transport it creates and applies `rx_incomplete` (DELIVER or DISCARD) itself |
| **r3 `BY_INDEX` lookup** | `rv_get_frame` scans for a free frame | RX: slot = `media_index mod count`, an index lookup |
| **r3 ST22 TX state transition as a CAS** | `tx_st22p_frame_done` checks `framebuff->stat` then stores FREE as two separate atomic accesses (`st22_pipeline_tx.c:263-265`; the field is `_Atomic uint32_t`, `st22_pipeline_tx.h:23`), so a concurrent late drop or abort can interleave | st22p TX: compare-exchange, as st20p's claim does (side finding) |

⁵ Via `mt_txq_done_cleanup`, rate-limited; dedicated queues only in v1 (Q-CMP-7).

The RX due time (06 §11.7) is first-packet arrival (earliest leg) + unit period +
`rx_flush_offset_ns`, capped at `presentation_tai_ns` when a link offset is set (session or
group); `rx_flush_offset_ns` 0 means `rx_skew_budget_ns` with two legs and 1 ms with one.

`mtl_session_get_info` reports the expected **completion latency** so apps size pools and
deadlines. Revision 2 gave "~1.9 ms at 1080p59.94, ~2.8 ms at 720p"; those depend on the
iavf/ice `tx_rs_thresh`/`tx_free_thresh`, not only on `nb_tx_desc`, and are a spike S6 output
`[C5 §4.11]`.

### 4.5 Destroy and ERROR on a stalled queue (r3)

`[C5 §4.3]`. `rte_eth_tx_done_cleanup` frees only descriptors the NIC has completed. With the
link down or an RL queue stalled nothing completes; today `mt_dpdk_flush_tx_queue` pushes pads
to force completion (`dev/mt_dev.c:1782-1799`, each pad bounded by a 1 ms busy retry,
`:1760-1780`) and `mt_dev_tx_queue_fatal_error` only *marks* the queue (`:1840-1869`); there is
no `rte_eth_dev_tx_queue_stop` anywhere in `lib/` **[verified]**. So after a bounded wait,
imported memory could still be referenced by live descriptors. The timeout branch:

1. The bounded idle cleanup (`rte_eth_tx_done_cleanup`, rate-limited) runs for up to
   `max(2 × completion latency, 10 ms)`.
2. **Dedicated queue** (every RL queue, and the default for video): a worker calls
   `rte_eth_dev_tx_queue_stop` then `rte_eth_dev_tx_queue_start` on the session's queue. The
   PMD releases every mbuf still in the ring; their external-buffer free callbacks run on the
   worker, which is therefore a completing context (§4.1.1); each completion claims with the
   §4.1 CAS, so a unit is reported once, as `FAILED/<reason>` or `FLUSHED/DESTROY`. Whether
   iavf and ice release chained external mbufs on queue stop is spike S8 **[inferred]**.
3. **Shared queue** (TSQ): the session's frames are marked; the queue is reset only when its
   last user is gone, and until then the session stays DESTROYING (its buffers stay
   referenced, `mtl_mem_destroy` returns `-MTL_EBUSY`).
4. If the queue cannot be stopped or restarted, the port escalates to `PORT_RESET` handling
   (Q-LIFE-10), which resets every queue of the port.
5. `SESSION_RETIRED` is posted only after the device references are gone.

ERROR entry (03 §3.6) runs the same steps 1–4 before it drops references. Only process exit
bypasses them; `mtl_mem_destroy` stays `-MTL_EBUSY` until then, and this is documented
(03 §6.4).

## 5. Waiting and waking

### 5.1 Wait targets and the armed-waiter protocol (r3)

`[C5 §3.6, §7.3]`, A10. A session has one **armed word per wait target**:

| Target | Armed by | Signalled when |
|---|---|---|
| BUFFERS | `mtl_tx_acquire` with a timeout; `MTL_WAIT_ACQUIRE` | a TX slot becomes acquirable |
| RESULTS | `mtl_tx_reap` / `mtl_cq_read` with a timeout; `MTL_WAIT_RESULTS` | a result becomes readable |
| RX_READY | `mtl_rx_dequeue` with a timeout; `MTL_WAIT_DEQUEUE` | an RX unit becomes READY |
| EVENTS | `mtl_eq_read` with a timeout on an EQ | an event is posted |

The armed word **counts waiters**, so several threads may wait on one target (for example two
producer threads with `MTL_SESSION_MT_SUBMIT` both blocked in `acquire`), and the wake is a
broadcast to that target's waiters; the natural P1 layout — a producer in `mtl_tx_acquire`
and a second thread in `mtl_tx_reap` — waits on two targets and never interferes. An exported
wait object (§5.3) is one per target and has one consumer: the app's event loop.

```text
APP (WT call, target T)                             COMPLETING CONTEXT
───────────────────────                             ──────────────────
1. DP attempt → got something? return it            a. write result, store state = DONE (release)
2. fetch_add(armed[T], 1)          (seq_cst)        b. atomic_thread_fence(seq_cst)
3. atomic_thread_fence(seq_cst)                     c. load armed[T] (relaxed)
4. DP attempt again → got something?                d. if armed[T] > 0: tasklet → set wake bit (atomic OR);
      fetch_sub(armed[T], 1); return it                other context → wake directly
5. sleep on T's wait object with the remaining timeout
6. woken → fetch_sub(armed[T], 1); goto 1
```

The fences make the store→load pairs (2→4 and a→c) ordered: either the app sees DONE in
step 4, or the completing context sees the waiter in step c. A release store followed by a
seq_cst load is **not** enough — that was the revision-1 draft's bug; `mt_handle_guard.h:48-53`
itself notes that all four operations must be sequentially consistent `[C1 #3]`. A
litmus-style unit test (two threads, forced interleavings) is part of G-52.

### 5.2 The waker and the wake options (r3)

> **Addendum K (2026-10-01).** In `MTL_INSTANCE_TASKLET_THREAD` mode the schedulers are now
> pinned to one CPU each, like lcores ([16 §5](16-kubernetes-and-crash-safety.md)). Where this
> section argues W2 for thread mode from unpinned schedulers, the argument becomes: a pinned
> thread-mode scheduler may still be preempted by the kernel, so the eventfd write costs no
> more there than in lcore mode, and W2 stays allowed in thread mode.

A tasklet that completes a unit for an armed waiter must not, by the rule above, make the
wake syscall; the waker thread does it. Review C1 showed a fixed 20–50 µs poll is both slow
(timer slack turns 20 µs into ~70 µs) and expensive (5–15 % of a core, because some waiter is
almost always armed) `[C1 #4]`. The waker:

| Measure | Effect |
|---|---|
| **Deadline-driven sleep.** The tasklet writes a per-session `next_due_tsc` (TX: launch + completion latency; **r3** RX: the §4.4 deadline). The waker sleeps until the earliest, then polls briefly. **r3: with no due time anywhere (RX signal loss, idle TX) its maximum sleep is 1 ms** `[C5 §4.9]` | near-zero CPU at frame rate; a bounded wake latency when nothing is due |
| **One wake word per scheduler and per RX packet lcore**, each on its own cache line; the waker scans at most 18 + the packet lcores | no cache-line bouncing across pinned cores |
| **Idle-path piggyback.** When a scheduler is about to sleep (`MTL_FLAG_TASKLET_SLEEP`), its idle path — which already enters the kernel (`mt_sch.c:86-96`) — performs its own pending wakes first | zero added latency in sleep mode |
| `PR_SET_TIMERSLACK = 1`, optional `SCHED_FIFO`, explicit housekeeping affinity from config or MtlManager (not inherited from the main lcore), `rte_power_monitor` (UMWAIT) where available | predictable latency, no sharing with the app's main thread |

| Wake option | Cost on the completing tasklet | Added wake latency | Where it fits |
|---|---|---|---|
| W0 app busy-polls DP calls (timeout 0) on its own core | 0 | 0 | lowest latency; costs one app core per polling thread (a shared CQ serves many sessions) |
| W1 app spins then sleeps with back-off (no wake) | 0 | up to the back-off | simple fallback |
| **W3 waker thread (deadline-driven)** | fence + load; bit-set when armed | the waker's timer and futex wake, 10–70 µs p99 **[spike S1]** | **default in lcore mode for unit periods ≥ 1 ms** |
| W2 the completing context writes the eventfd, only when armed | one non-blocking `write()` per wake: ≈ 1–2 µs if the waiter's core is awake, 3–6 µs from a deep C-state **[unverified; S1]**; ≤ one per unit per armed target (≤ 8 000/s per session at 125 µs) | lowest with a sleeping waiter | **default with `MTL_FLAG_TASKLET_THREAD`; lcore mode below 1 ms: recommended, pending M6 (Q-THR-2)** |

**r3 auto-selection, and what it costs** `[C5 §4.8, §4.10]`. At a 125 µs audio packet time,
W3's 10–70 µs is 10–50 % of the period, and W0 adds a mandatory app core for every
low-latency audio or line-mode user where today's callbacks cost none. The designer's
recommended default is therefore W2 for sessions with a unit period < 1 ms, selected
automatically unless the session sets `MTL_SESSION_WAKE_WAKER`. This conflicts with the
maintainer's rule as written: in lcore mode **W2 is a syscall on a pinned core** — one per
wake, microseconds each, bounded by one per unit per armed target, never a blocking call,
never while no waiter is armed. It is therefore a recommendation, not a settled default:
maintainer decision M6 (Q-THR-2) accepts it or picks either alternative instead:

- **W0 polling** for sub-millisecond sessions (zero tasklet cost, one app core), with W3 for
  the rest; or
- **W3 everywhere**, accepting the waker's latency as stated in `mtl_session_get_info()`
  (`expected_wake_latency_ns`, from S1).

In `MTL_FLAG_TASKLET_THREAD` mode the scheduler thread is **not pinned** (§7.1), so W2 there
does not conflict with the requirement; it is the default in that mode regardless of the unit
period. `MTL_SESSION_WAKE_DIRECT` forces W2 and `MTL_SESSION_WAKE_WAKER` forces W3 per session.
Spike S1 reports CPU % and p50/p99/p99.9 wake latency for W0, W2 and W3 at 125 µs and 1 ms
unit periods, with the waiter's core idle and busy. The waker's CPU budget, affinity source
and scheduling class are Q-THR-2a.

### 5.3 Pollable wait objects (r3)

- `mtl_session_get_wait_object(s, mask, &wo)` with `mask` = `MTL_WAIT_ACQUIRE` |
  `MTL_WAIT_DEQUEUE` | `MTL_WAIT_RESULTS`, so an epoll loop, GstPoll, asyncio or Rust
  `AsyncFd` can wait for "acquire would succeed" even in completion mode NONE `[C3 P2-5]`;
  `mtl_cq_get_wait_object`, `mtl_eq_get_wait_object` for queue objects. The out-struct is
  `struct mtl_wait_object { intptr_t native; uint32_t kind; uint32_t reserved; }` with
  `kind` = `MTL_WAIT_FD` (an eventfd on Linux) or `MTL_WAIT_WIN_HANDLE` (an event `HANDLE` on
  Windows) (A5).
- **r3: `*_trywait` returns 1, 0 or < 0** `[C5 §2.10]`: **1** = something is already
  available (do not block, read now); **0** = armed, safe to block on the wait object;
  **< 0** = an error (`-MTL_ECANCELED` when interrupted, `-MTL_ESHUTDOWN`, `-MTL_EIO`,
  `-MTL_EBADF`). It never returns `-MTL_EAGAIN`, so `if (trywait() == 0) epoll_wait(…)`
  cannot be inverted by accident — revision 2 used `-EAGAIN` for "ready" next to read verbs
  where it meant "nothing", libfabric's `fi_trywait` footgun.
- **Draining:** the objects are eventfds in counter mode; `*_trywait` and every successful
  read/dequeue/acquire *on an APP thread* drain the counter only when it was signalled (one
  non-blocking `read()`, which the DP contract allows as the call's own wait-object
  syscall, §3), so a level-triggered epoll does not spin. Calls from busy-loop threads never
  drain (§3.3). The app never `read()`s the object itself.
- The first `get_wait_object` call per target creates the eventfd (CP).

### 5.4 Interrupting waiters (r3)

Interruption is **sticky** so frameworks can implement GStreamer's `unlock` /
`unlock_stop` without races `[C3 P2-1]` (renamed from cancel/resume, A6):

- `mtl_session_interrupt(s)` (AS) sets the session's interrupt flag and signals the waker
  once; the waker wakes every waiter on the session's targets and private EQ. **While the
  flag is set, every WT call with `timeout ≠ 0` on the session returns `-MTL_ECANCELED`
  immediately**, also calls that start after the interrupt and calls that would find data;
  `timeout == 0` calls are unaffected.
- `mtl_session_uninterrupt(s)` (CP) clears it (`unlock_stop`).
- `mtl_cq_interrupt(cq)` / `mtl_eq_interrupt(eq)` do the same for one shared queue only;
  interrupting one element never wakes another element's waiters.
- **r3 instance level** `[C5 §8.12]`: `mtl_instance_interrupt_all(mt)` (AS) sets one
  instance-wide sticky flag that every WT call checks (one extra relaxed load) and writes the
  waker's eventfd once; `mtl_instance_uninterrupt_all(mt)` (CP) clears it. It never writes
  per-session flags, so it cannot race a destroy writing them, and after a SIGINT a framework
  element that never saw the signal is released by one call instead of one per session.
  The effective state of a waiter is "session flag **or** queue flag **or** instance flag".
- **Ordering with stop and destroy.** Destroy wins: `mtl_session_interrupt` on a DESTROYING
  session is a no-op that returns 0, and a waiter woken by both returns `-MTL_ESHUTDOWN`.
  When several conditions hold, a WT call returns the first of: `-MTL_EBADF`,
  `-MTL_ESHUTDOWN` (DESTROYING, or DRAINING/FLUSHING with nothing left), `-MTL_EIO` /
  `-MTL_ENODEV` (ERROR), `-MTL_ECANCELED`, then the normal result or `-MTL_ETIMEDOUT`.
- Today `*_wake_block()` does not make `get_frame` return early — the predicate loop goes
  back to sleep until the deadline (`st20_pipeline_tx.c:781-788`) — so shutdown takes up to
  the 1 s block timeout `[R13 §1.4]`.
- FFmpeg's `AVIOInterruptCB` is *polled* by the demuxer (`ff_check_interrupt`); the fit
  there is a short timeout plus a check, or non-blocking reads (`AVFMT_FLAG_NONBLOCK`, a
  demuxer flag) → `-MTL_EAGAIN` → `AVERROR(EAGAIN)`. The muxer's `write_packet` may block and
  uses `mtl_tx_write(…, timeout)` (11 §6.2).

## 6. Inline hooks (optional, expert)

Some users chose today's callbacks precisely because they run on the tasklet with zero
hop: slice producers, zero-copy forwarders, the MXL bridge `[R08 §1.2]`. The new API does
not need callbacks for correctness (pre-attached pools and `MTL_RX_SLOT_BY_INDEX` replace
`query_ext_frame`; `publish` replaces `query_frame_lines_ready`). A zero-hop forwarder can
also run as a user tasklet calling the inline-safe subset (§3.3). If a zero-hop notification
is still wanted, it is an explicit opt-in:

```c
/* runs in data-plane context on the completing thread */
typedef void (*mtl_inline_notify_fn)(void* user, mtl_session_h s, uint32_t what);
```

Rules (Q-THR-1):

- wait-free, bounded (budget measured, counters exposed; the hook is disabled with an
  `INLINE_HOOK_DISABLED` event if it exceeds the budget repeatedly);
- may call only the inline-safe DP subset of §3.3 — never CP, DPC or WT;
- runs without any session lock held, so it cannot self-deadlock (H2).

Recommendation: ship v1 without inline hooks and measure whether W0 busy polling closes
the latency gap for these users first.

## 7. Progress model

| Model | libfabric analogue | Proposal |
|---|---|---|
| Automatic: library tasklets drive all data-path progress | `FI_PROGRESS_AUTO` | **the only model in v1** |
| Manual: the app owns a scheduler thread and calls `mtl_sch_run_once()` | `FI_PROGRESS_MANUAL` | later option (Q-THR-3); natural for GStreamer/OBS apps that own their threads and for non-video media; unsafe for hardware-paced video, where a late app loop breaks ST 2110-21 `[R09 §7, R03 §7.4]` |

The existing `mtl_sch_api.h` (app-created schedulers and tasklets) stays as it is; a user
tasklet is a busy-loop thread for §3.3.

### 7.1 `MTL_FLAG_TASKLET_THREAD` and `MTL_FLAG_TASKLET_SLEEP` (r3, answers Q-THR-10 for Phase 1)

> **Addendum K (2026-10-01).** Thread-mode schedulers are pinned, one CPU each, and threads
> that are not schedulers run on `instance.main_lcore` ([16 §5](16-kubernetes-and-crash-safety.md)).

`[C5 §4.8]`. In thread mode `sch_start` creates a plain pthread (`mt_sch.c:285-287`,
`sch_tasklet_thread` at `:252-257`); `rte_thread_register` appears nowhere in `lib/src`
**[verified]**, so the "tasklet" has `LCORE_ID_ANY`, no mempool cache, and is preemptible.
That is a different threat model — containers are this flag's main user:

| Topic | lcore mode (default) | `MTL_FLAG_TASKLET_THREAD` |
|---|---|---|
| Pinned? | yes | no: a normal, preemptible thread |
| Wake | W3 (W2 for < 1 ms if maintainer decision M6 accepts it, Q-THR-2, §5.2) | **W2 by default**: the scheduler thread writes the eventfd itself, only when a waiter is armed; the waker indirection would only add latency |
| Mempool cache | per lcore | scheduler threads call `rte_thread_register` at start so they get an lcore ID and a mempool cache; without it two scheduler threads sharing a port pool contend on the MP ring tail against each other (§9.1) |
| G-39 | measured as stated | measured per mode; "no syscall" is not claimed in thread mode |
| `MTL_FLAG_TASKLET_SLEEP` | the idle path performs pending wakes before it sleeps, and posting a command wakes it (§3.2) | same |

## 8. Other hot-path hooks today

| Hook | Today | Proposal |
|---|---|---|
| Time on the tasklet (user `ptp_get_time_fn`, built-in PTP PHC reads, `CLOCK_REALTIME`) | a callback, a PMD register read or a vDSO call on every pacing computation⁶ | **one published time base for every source**, specified in §8.1 |
| Log printer | any thread, incl. tasklets, with `localtime_r` | in release builds tasklets **never format or print at any level**; they write fixed binary records (code + arguments) into a lock-free per-scheduler log ring, rate-limited per (code, session); a library thread formats and calls the printer (R-OBS-5) `[C1 #14]` |
| Stats | copied under the session spinlock; several writers | per-writer counter blocks, reader sums; no lock shared with tasklets ([08 §2.4](08-observability.md)) |
| Admin CPU-busy scan | takes the blocking session spinlock on every video session every cycle (`mt_admin.c:30`, `:40`) | tasklet-written busy counters read without the lock |

⁶ `dev/mt_dev.c:1603-1608`, `mt_ptp.c:393-403`.

### 8.1 The published time base (r3)

`[C5 §4.5]`. Today `tv_sync_pacing` reads PTP *once per frame* and paces the frame on TSC
(`st_tx_video_session.c:692-745`, the read at `:695`); with built-in PTP that read is a PHC
register (`ptp_from_eth`, `mt_ptp.c:401-403`), so frame-start error is read latency only.
Replacing it by an extrapolated record must not make pacing worse without saying by how
much. The record, per instance and **per CPU socket**:

```text
{ seq, tsc_base, tai_base_ns, ratio (TAI ns per TSC tick, 32.32 fixed point),
  monotonic_base_ns, realtime_base_ns, state, accuracy_ns }       seqlocked; one writer
```

| Property | Rule |
|---|---|
| Refresh | every ≤ 100 ms, by the PTP servo context or the admin thread (the app, for a user time source), never on a tasklet |
| Ratio | from a **servo**: a least-squares fit over the last N (default 16) PHC/TSC cross-timestamps, each a PHC read bracketed by two TSC reads and rejected when the bracket is too wide. The PHC servo disciplines the NIC clock, not TSC; today `impl->tsc_hz` is a one-shot calibration (`mt_main.c:116`, `:578`), and 1 ppm of error is 1 µs per second |
| Publication | **slewed, never stepped**: a new record starts on the previous line at the switch instant (`tai_base_ns` = the old record at `tsc_base`), with a ratio that meets the new estimate within one refresh period, so two frames straddling a refresh see one continuous grid. The only steps are declared clock steps (`TIME_STEP`, step policy 06 §2.4) |
| Per socket | TSC is not guaranteed synchronised across sockets, and the scheduler and admin thread may sit on different ones; the writer produces one record per socket from cross-timestamps taken on a thread of that socket, and each tasklet reads its socket's record |
| Conversions | the `monotonic_base_ns` / `realtime_base_ns` cross-timestamps serve `mtl_time_convert` and `mtl_time_cross_timestamp` (06) |
| Reader | wait-free: `tai = tai_base_ns + (tsc − tsc_base) × ratio`; a seqlock retry only while the writer is mid-update |

The accuracy bound — frame-start error of the extrapolated base against a direct PHC read
at the same instant, under RL and TSC pacing, across a refresh — is **spike S7**'s output,
and the bound goes into 13 §8 as a performance gate. (Q-THR-7a.)

## 9. Other hazards to fix along the way

### 9.1 Mempools and app threads (r3)

`[C5 §4.4]`. Revision 2 said "app-side buffer returns go through rings the tasklet drains".
That is right for returns — `rte_ring` consumers never wait on producers; only same-side
peers wait in `__rte_ring_update_tail`, so a tasklet *dequeuing* an app-filled ring is safe —
but it said nothing about app-side *allocation*, and it over-applied to frame pools:

| Case | Rule |
|---|---|
| frame pools (video/cvideo/audio/ANC/fastmeta buffers) | keep today's atomic release: RX release is an atomic decrement (`rv_put_frame`, `st_rx_video_session.c:222-232`), not a mempool operation, and needs no ring |
| app threads and MP/MC mempools | an app thread never gets from or puts to an MP/MC mempool a tasklet also uses: a preempted app thread mid-dequeue stalls the tasklet in the ring tail update (`mt_util.c:516-545`, default `ring_mp_mc` **[inferred from DPDK]**); RTS/HTS only bounds the window. `st20_tx_get_mbuf` (`st_tx_video_session.c:4628`) is such a path and stays legacy-only |
| a future mbuf-level API (Q-MODE-1) | a tasklet-refilled per-consumer allocation ring plus a return ring the tasklet drains |
| non-EAL app threads | have no mempool cache (`rte_lcore_id() == LCORE_ID_ANY`); every get/put would hit the shared ring, which is one more reason for the rule above |

### 9.2 Other hazards

| Hazard | Evidence | Fix |
|---|---|---|
| `lc_refcnt` RMW on every call shares a cache line with tasklet-read fields | `st20_pipeline_tx.h:35-42` | a per-object in-flight counter on its own cache line (Q-THR-5) |
| NUMA: lease tables live on the scheduler's socket; app threads on the other socket pay cross-socket latency | R03 C8 | `mtl_session_get_info` reports the socket; docs recommend placing polling threads on it |
| RX zero-fill of missing ranges could be a 5 MB memset on the RX tasklet when a whole leg is lost | `[C1 #16]` | the fill runs in the consumer's `dequeue` (DPC) or on a worker |
| Builder `pending` overwritten instead of summed (sleep mode) | `st_tx_video_session.c:2694-2698` | side finding SF-08 |
| `USE_MULTI_THREADS` RX: tasklet and packet lcore can process one session concurrently when the packet ring is full | `st_rx_video_session.c:2901-2907` **[inferred race]** | side finding SP-03 |
| Recovery zeroes `sh_info` while chain mbufs may still be in descriptors | `st_tx_video_session.c:4299-4300` | §4.1.1; side finding |

## 10. Thread-safety table (v1 surface, r3)

"Session-serialised" = one caller at a time per session (externally serialised, §4.2).
Names follow A6; the header (10) is normative.

| Function | Class | Concurrency | Blocks? | Signal handler? |
|---|---|---|---|---|
| `mtl_<media>_session_create`, `mtl_<media>_session_query` | CP | any threads, any sessions | yes (never on ARP/IGMP, 03 §3.3) | no |
| `mtl_session_attach_buffers` / `detach_buffers`, `bind_cq`, `bind_eq`, `set_option`, `reconfigure` | CP | serialised per session | short | no |
| `mtl_session_start` / `stop` / `discard_queued` / `destroy` | CP | serialised per session; concurrent destroy → one wins, others `-MTL_EBUSY` | yes (bounded by the command ack, §3.2) | no |
| `mtl_session_update_flows`, `mtl_session_set_leg_enabled` | CP | serialised per session | yes (bounded; ARP/IGMP off the tasklet) | no |
| `mtl_session_interrupt`, `mtl_cq_interrupt`, `mtl_eq_interrupt`, `mtl_instance_interrupt_all` | AS | any | no | **yes** |
| `mtl_session_uninterrupt`, `mtl_cq_uninterrupt`, `mtl_eq_uninterrupt`, `mtl_instance_uninterrupt_all` | CP | any | no | no |
| `mtl_session_get_wait_object`, `mtl_cq_get_wait_object`, `mtl_eq_get_wait_object` | CP (the first call per target creates the eventfd) | any | no | no |
| `mtl_session_trywait`, `mtl_cq_trywait`, `mtl_eq_trywait` | DP | one consumer per wait object | no | no |
| `mtl_session_wait` | WT | several waiters per target | yes | no |
| `mtl_tx_acquire`, `mtl_tx_acquire_buffer` | WT (DP when timeout 0; inline-safe) | **MP-safe** | when timeout ≠ 0 | no |
| `mtl_tx_acquire_dynamic` (Phase 4) | WT (DP when timeout 0) | **MP-safe** | when timeout ≠ 0 | no |
| `mtl_tx_submit` | DPC (DP work unless it converts in the caller; inline-safe then) | session-serialised, or MP with `MTL_SESSION_MT_SUBMIT` | no | no |
| `mtl_tx_publish`, `mtl_tx_withdraw` | DP | as submit | no | no |
| `mtl_tx_release` | DP (inline-safe) | **MP-safe** | no | no |
| `mtl_tx_write` | WT (DPC work) | as submit | when timeout ≠ 0 | no |
| `mtl_tx_reap` | WT (DP when timeout 0; inline-safe) | MP-safe under the reaper lock; one context with `MTL_SESSION_SINGLE_READER` | when timeout ≠ 0 | no |
| `mtl_rx_dequeue` | WT (DP/DPC when timeout 0; inline-safe when DP) | as reap | when timeout ≠ 0 | no |
| `mtl_rx_release` | DP (inline-safe) | **MP-safe**, any thread, any order | no | no |
| `mtl_rx_wait_progress` | WT | the lease holder | yes | no |
| `mtl_rx_transfer` (RX lease → TX lease) | DP | the lease holder + the target's submit side | no | no |
| `mtl_cq_read`, `mtl_eq_read` | WT (DP when timeout 0; `cq_read` inline-safe) | MP-safe under the queue's reader lock | when timeout ≠ 0 | no |
| `mtl_eq_post` | DP | any thread (USER ring, 07 §3.3) | no | no |
| `mtl_cq_create` / `destroy`, `mtl_eq_create` / `destroy` / `subscribe` | CP | any; serialised per object | short | no |
| `mtl_session_get_cq`, `mtl_instance_get_eq` | DP | any | no | no |
| `mtl_session_get_eq` | CP (takes an app reference on the private EQ, 07 §3.4) | any | no | no |
| `mtl_instance_acquire_default` / `open` / `release`, `mtl_instance_from_legacy` | CP | any (refcounted, internally serialised) | yes | no |
| `mtl_instance_get_info`, `mtl_instance_get_mem_status`, `mtl_instance_list_sessions`, `mtl_port_count` / `find` / `get_caps` / `get_capacity` | CP | any | no | no |
| `mtl_session_get_state` | DP (inline-safe) | any | no | no |
| `mtl_session_get_stats`, `mtl_session_stat_get`, `mtl_port_get_status`, `mtl_port_stat_get`, `mtl_instance_get_status`, `mtl_instance_stat_get`, `mtl_time_get_status`, `mtl_sched_get_status` | DP (`get_stats` inline-safe) | any threads | no (seqlock retry) | no |
| `mtl_session_get_status` | CP (A6: the full copy; never blocks on a tasklet) | any threads | no (seqlock retry) | no |
| `mtl_session_get_info`, `get_buffer_requirements`, `mtl_stat_list` | CP | any threads | no | no |
| `mtl_video_layout_query`, `mtl_video_format_enum` | CP | any (pure) | no | no |
| `mtl_mem_alloc` / `import` / `destroy` / `get_info` / `map_device`, `mtl_buffer_create` / `destroy`, `mtl_session_get_pool_region` | CP | any | yes | no |
| `mtl_buffer_get_view`, `mtl_buffer_index`, `mtl_lease_buffer`, `mtl_lease_slot` | DP | any | no | no |
| `mtl_buffer_hold` / `mtl_buffer_unhold` | DP | any (a counter on the buffer, 05 §4.5) | no | no |
| `mtl_timeline_create` / `open` / `destroy`, `mtl_group_*` (except `get_state`) | CP | any; serialised per object | yes | no |
| `mtl_timeline_epoch`, `mtl_timeline_get_anchor`, `mtl_group_get_state` | DP | any | no | no |
| `mtl_time_now`, `mtl_time_convert`, `mtl_time_cross_timestamp` | DP | any | no | no⁷ |
| `mtl_last_error`, `mtl_call_seq` | DP | any (thread-local) | no | no |
| `mtl_version_num`, `mtl_error_name`, `mtl_reason_name` | AS | any (constant) | no | **yes** |

⁷ A seqlock read retries while the time base is being written; a signal that interrupts the
writer thread would spin.
