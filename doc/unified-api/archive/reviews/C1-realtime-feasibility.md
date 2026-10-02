# C1 — Real-time and implementation feasibility review

| | |
|---|---|
| Lens | Senior DPDK / MTL data-plane engineer. Can the design be built on today's engines without breaking pacing or throughput, under the rule that nothing called from the API runs in, or blocks, a pinned tasklet? |
| Date | 2026-09-29 |
| Revision reviewed | Untracked draft `doc/unified-api/` in the working tree (files dated 2026-09-29 12:07–12:34), over `main` @ `545a266a`. Read in full: 00, 02, 03, 04, 05, 07, 08, 13, 14, DECISIONS, the Threading/Architecture/Completions sections of OPEN-QUESTIONS, research 03. Read in part: 06, 10, side-findings, research 04 and 05 |
| Code basis | Every `path:line` is at HEAD, read from a `git archive HEAD` export. The working tree has unrelated uncommitted edits in `lib/src` |
| Verdict | The direction and the rules in 04 are right. Two things cannot be built as written: ordered, exactly-once results over *unmodified* pipelines (Phase 1 adapter), and the armed-waiter protocol with the memory orders given. Several "free" claims have a real cost. The engine work is larger than "thin adapter" and belongs in Phase 1 |

Severity: **blocker**, the design as written cannot be implemented or is incorrect. **major**, it can be built, but only with an undisclosed engine change, a real cost, or a correctness fix. **minor**, a wording issue, a contradiction, or a small gap.

Evidence labels: **[verified]** means I read the code at the cited line. **[inferred]** means it follows from verified code plus DPDK, driver or kernel behaviour I did not re-read here. **[unknown]** means I could not establish it.

---

## Findings

### 1. The Phase 1 adapter cannot implement "free callback marks DONE, session tasklet publishes in order" over unmodified pipelines (blocker)

- **Where:** [02 §2.2](../02-architecture.md) (adapter table), [02 §4.1](../02-architecture.md), [04 §4.1](../04-threading-and-execution.md), [03 §4.1](../03-object-model-and-lifecycle.md) (DONE → FREE by the session tasklet), [05 §7](../05-memory-and-buffers.md) D-18, G-05, G-09, [14](../14-implementation-roadmap.md) Phase 1.
- **Issue:** the design needs three engine facts that the pipelines do not provide.
  - **There is no "session tasklet" for L2 to run in.** **[verified]**
    - The pipelines have no tasklet of their own (R03 §1).
    - The only per-frame pipeline code on a tasklet is `tx_st20p_next_frame`, and the builder calls it only in the `WAIT_FRAME` state (`st_tx_video_session.c:1922`). A publish step hooked there runs once per frame, so a result can wait up to one frame.
    - A per-iteration walk needs a new hook in `tvs_tasklet_handler` (`st_tx_video_session.c:2673-2700`) and in each of the other session managers. Or it needs a new L2 tasklet on the transport's scheduler, which must also follow migration.
  - **The pipeline frees the slot before anyone can mark DONE.** **[verified]**
    - `tx_st20p_frame_done` stores `FREE` (`st20_pipeline_tx.c:270-271`) *before* it calls the user's `notify_frame_done` (`:286`), which is where the adapter's hook would run.
    - `st20p_tx_get_frame` picks the slot itself: the first FREE index (`:47-58`, `:773`). So an app-thread `acquire` can be handed slot *k* before the previous use of *k* is DONE or published. That breaks G-05.
    - `EXT_FRAME_MANUAL_RELEASE` parks the slot in `IN_USER` only on the derive path, and `st20p_tx_notify_ext_frame_free` is a no-op with an internal converter (`:1044-1051`).
  - **Converting sessions never get a transport-done callback.** **[verified]**
    - With plugin conversion, `notify_frame_done` fires when the converter finishes (`:383-384`, plugin thread).
    - With an internal converter and ext frames, it fires inside `put_ext_frame` (`:998-999`, app thread).
    - In both cases `frame_done_cb_called = true` then suppresses the callback at transport done (`:280-286`). D-18 ("terminal result after both storage access and transport outcome") has no signal to hang on.
  - **On copy paths the free callback is not "transport done".** **[verified]**
    - No-chain ST20 and ST22 call `tv_frame_free_cb` at the end of build, before the transmitter has sent anything (`st_tx_video_session.c:2130-2134`, `:2655-2659`).
    - ST30 fires `notify_frame_done` at the end of build too (`st_tx_audio_session.c:923-930`).
    - A terminal result "after the transport outcome" therefore needs a transmitter-side "last packet of unit handed to the NIC" hook for every essence. That is E4, but it is a precondition for D-18, not a Phase 3 nicety.
  - **The RX tasklet cannot produce RX results for converting sessions.** With the internal converter, the unit is converted inside `st20p_rx_get_frame` on the app thread (`st20_pipeline_rx.c:853-878`). The RX tasklet cannot write a "READY in app format" CQ entry for such sessions. **[verified]**
- **Suggested change:**
  - **Materialise the CQ on the consumer side.**
    - The lease table lives in hugepage memory.
    - Whatever context finishes a unit does only a release store `DONE{gen, status, times}` into the lease, plus the wake step of finding 3. That context may be the free callback, a pipeline hook, a plugin thread or the app itself.
    - `mtl_cq_read`, `mtl_rx_dequeue`, `mtl_tx_reap` and `mtl_tx_acquire` then walk in-flight leases in submission order, write the entries, and move DONE → FREE.
  - This gives a single producer by construction: the producer is the consumer thread. It gives FIFO order, needs no L2 tasklet and no new per-iteration tasklet work, and makes migration (Q-THR-6) irrelevant to the CQ. Credits become "pool slots", and shared CQs stay poll sets.
  - **State the L0 changes Phase 1 needs, per pipeline** (st20p, st22p, st30p, st40p):
    - a "held" state, so a finished slot is not FREE until L2 releases it;
    - an internal completion hook that fires once at transport done, whatever `frame_done_cb_called` says, with status and meta;
    - a CAS reclaim of `CONVERTED` / `READY` for FLUSHED (finding 20).
  - Rename "thin adapter" and re-scope spike S3 to measure these hooks.
- **New open question:**
  - Q-ARCH-1a: *Phase 1 over the pipelines needs engine changes in all four `st*p` TX/RX paths: a held slot state, a once-only internal completion hook, and a reclaim for flush. Accept them as Phase 1 scope (recommended), or wrap the session layer (Q-ARCH-1 b), where the same hooks are needed in `tv_*`/`rv_*` anyway?*

### 2. TX results arrive about `nb_tx_desc` packets late, and never once the app stops submitting (major)

- **Where:** [04 §4.1](../04-threading-and-execution.md) ("one extra tasklet iteration of latency (microseconds)"), [03 §3.4](../03-object-model-and-lifecycle.md) DRAIN, [03 §6.2](../03-object-model-and-lifecycle.md) ("TX pad flush if needed"), G-03, G-31.
- **Issue:**
  - In chain mode the frame is DONE only when the last chain mbuf is freed, and that happens only inside a later `rte_eth_tx_burst` that recycles descriptors. **[verified]** mechanism, `st_tx_video_session.c:116-141`, `:235-237`. **[inferred]** driver behaviour: ice/iavf clean only when `nb_tx_free < tx_free_thresh`.
  - With `MT_DEV_TX_DESC = 512` (`dev/mt_dev.h:12`) that is about 512 packets after the frame's last packet. At 1080p59.94 (about 4320 packets per frame, TRS about 3.7 µs) this is about 1.9 ms plus the vertical blank. At 4K it is about 0.5 ms; at 720p about 2.8 ms. **[inferred]** numbers.
  - The transmitter never recycles while idle. The only flushes are in teardown (`tv_uinit_hw` `:2725`) and recovery (`:4261`, `:4287`). **[verified]**
  - So after the last submitted frame, its result never arrives. G-03 ("submit one unit, never call anything else, observe the result") fails on the default ST20 path. Stop(DRAIN) cannot finish without a flush.
  - The existing flush cannot run on a tasklet: `mt_dpdk_flush_tx_queue` logs at `info()` and busy-loops pad bursts with 1 ms timeouts (`dev/mt_dev.c:1782-1799`). **[verified]**
- **Suggested change:**
  - In the transmitter tasklet, when a session has frames IN_FLIGHT and its build ring is empty, call the non-blocking `mt_txq_done_cleanup` (`datapath/mt_queue.c:216-219` → `rte_eth_tx_done_cleanup`), rate-limited, for example once per TRS × 64.
  - For TSQ this takes the shared-queue spinlock (`mt_shared_queue.c:625-633`, R03 H9). Allow it only on dedicated queues, or accept the H9 exposure explicitly.
  - Report `completion_latency_ns` (expected and max) in `mtl_session_get_info`. Replace "microseconds" in 04 §4.1.
  - Make stop(DRAIN) and destroy use the cleanup, never the busy flush.
- **New open question:**
  - Q-CMP-6: *Idle descriptor recycling: may the transmitter call `rte_eth_tx_done_cleanup` on an idle session so that the last results and the drain complete? Rate, and TSQ policy?*

### 3. The armed-waiter protocol loses wake-ups with the memory orders written (major)

- **Where:** [04 §5.1](../04-threading-and-execution.md).
- **Issue:**
  - As written, the tasklet side is "publish entry (release store of tail)" then "load `armed` (seq_cst)". In C11, a release store followed by a seq_cst load does not order StoreLoad. On x86 the tail store can sit in the store buffer while the `armed` load reads 0. The app then stores `armed = 1`, re-reads the old tail, and sleeps. That is a lost wake-up.
  - The design cites `mt_handle_guard.h` as "the same pattern". That file says the opposite: "ALL FOUR ops must be SEQ_CST" (`mt_handle_guard.h:48-53`) **[verified]**.
  - The cost line "Tasklet cost: one atomic OR" (04 §5.2) is incomplete. The fence is paid on every publish, whether or not anyone is armed.
- **Suggested change:**
  - Tasklet: tail/DONE store (release), then `atomic_thread_fence(memory_order_seq_cst)` (or a seq_cst store), then the relaxed load of `armed`.
  - App: seq_cst store of `armed`, then a seq_cst fence, then the tail load.
  - With finding 1's consumer-side CQ the "publish" is the DONE store, so the fence is one per unit, not per iteration.
  - State the cost: about 30–100 cycles per unit **[inferred]**.
  - Add a litmus-style unit test (two threads, forced interleavings) to G-30.
- **New open question:** none.

### 4. The W3 waker thread costs more and wakes later than the design states (major)

- **Where:** [04 §5.2](../04-threading-and-execution.md), Q-THR-2, [13 §8](../13-guarantees-and-tests.md) ("p99 ≤ poll interval + 10 µs").
- **Issue:**
  - **Timer slack.** An unprivileged `SCHED_OTHER` thread has a default timer slack of 50 µs, so a 20 µs sleep usually lasts about 70 µs. Add the futex wake to the app thread's scheduling latency. **[inferred]**
  - **Polling is the steady state.** "While at least one waiter is armed" is the normal case for any app using blocking waits: one arm per frame per session. The waker then wakes 20k–50k times per second, which is about 5–15 % of a core in hrtimer and context-switch overhead **[inferred]**. The arm-first path adds two syscalls per wait (app wakes waker, waker wakes app).
  - **Affinity.** A thread created with `pthread_create` after `rte_eal_init` inherits the main lcore's affinity. MTL sets affinity only in `mtl_bind_to_lcore` (`mt_main.c:737-738`) **[verified]**; the inheritance is DPDK EAL behaviour **[inferred]**. The "unpinned" waker would share a CPU with the app's main thread and MTL's admin/stat threads.
  - **Short audio packet times.** With 125 µs audio packet time, wake latency is at least one packet time, so W3 is unusable. The docs must say W0 is required there.
  - **Contention.** One instance-wide wake bitmap, OR-ed by the tasklets of up to 18 schedulers (`MT_MAX_SCH_NUM`) and cleared by the waker, bounces one cache line across pinned cores.
- **Suggested change:**
  - **Deadline-driven waker.** The tasklet already knows when a unit's result is due: scheduled launch plus finding 2's completion latency, or the RX frame epoch plus the flush deadline. It writes a per-session `next_due_tsc`, and the waker sleeps until the earliest due time and then polls briefly. At frame rate that is near-zero CPU.
  - **Idle-path piggyback.** `sch_tasklet_sleep` already enters the kernel (`mt_sch.c:87`, `:89-95`, `rte_eal_alarm_set` + condvar) **[verified]**. When a scheduler is about to sleep, it can drain its own pending wakes first, at no added cost. This helps only with `MTL_FLAG_TASKLET_SLEEP`, where it gives zero added latency.
  - **Contention and scheduling.** Use one wake word per scheduler on its own cache line, and let the waker scan 18 lines. Give the waker `PR_SET_TIMERSLACK=1`, optionally `SCHED_FIFO`, and an explicit housekeeping affinity (not inherited). Use `rte_power_monitor` (UMWAIT) where WAITPKG exists.
  - Spike S1 must report CPU % and p99.9 with and without these measures.
- **New open question:**
  - Q-THR-2a: *Waker scheduling contract: CPU budget, affinity source (MtlManager, a config cpuset, or inherited), `SCHED_FIFO` allowed, and deadline-driven versus fixed-interval polling.*

### 5. Q-THR-5's recommendation (state gate + QSBR) is not "a relaxed load", and v1 gains nothing from it (major)

- **Where:** [03 §2.2](../03-object-model-and-lifecycle.md), Q-THR-5, [04 §9](../04-threading-and-execution.md).
- **Issue:**
  - **A state gate alone is not safe.** A reader that loaded `RUNNING` and was then preempted is still inside the call when destroy proceeds. A grace period needs every reader to announce itself.
  - **`rte_rcu_qsbr` is a poor fit for arbitrary app threads.** **[inferred]** from DPDK rcu.
    - It needs a registered thread ID below `max_threads` and online/quiescent reporting.
    - A thread that stays online and then blocks elsewhere stalls every destroy, which is realistic for GStreamer or Python threads.
    - The safe pattern for arbitrary app threads is online/offline around each call, and `rte_rcu_qsbr_thread_online` includes a seq_cst fence, which costs the same as today's RMW.
  - **v1 saves nothing.** Every adapter call goes through pipeline entry points that already run `MT_HANDLE_GUARD` (`st20_pipeline_tx.c:763`, `:832`) **[verified]**.
  - **The real problem is false sharing.** R03 §8 measured cost from `lc_refcnt` sharing a line with tasklet-read `impl`/`idx`/`type` (`st20_pipeline_tx.h:35-42`) **[verified]**. At frame rate, an RMW on an app-owned line costs about 20 ns.
- **Suggested change:**
  - Recommend a per-object in-flight counter on its own cache line (or per-thread epoch slots, each on its own line), plus generation-tagged handles and type-stable slot memory, so that a stale handle never dereferences freed memory.
  - Revisit QSBR only if a packet-rate DP API (Q-MODE-1) appears.
  - Update the Q-THR-5 recommendation and the 03 §2.2 cost table.
- **New open question:** none; amend Q-THR-5.

### 6. Stats are not single-writer today, and the v1 stats path still takes the session spinlock (major)

- **Where:** [08 §2.4](../08-observability.md), [04 §8](../04-threading-and-execution.md), G-40, Phase 1 exit ("stats snapshot skeleton (seqlock)").
- **Issue:** counters that the design treats as tasklet-owned are written from several contexts today. **[verified]** for all of these:
  - `ctx->stat_drop_frame++` is written plainly by the builder tasklet (`st20_pipeline_tx.c:145`) and by the app thread (`:920`).
  - The stat thread copies *and resets* tasklet counters in `tv_stat_collect` (`st_tx_video_session.c:3557-3601`, via `tx_video_session_get_timeout` `:3945`).
  - `reset_session_stats` memsets them from the app thread (`:4778-4781`).
  - `stat_max_notify_frame_us` is written in whatever context runs the free callback (`:110-111`).
  - The admin thread takes the *blocking* session spinlock S on every video session every cycle, regardless of the migrate flag (`mt_admin.c:30`, `:40`, called from `:338`).
  - The v1 read path, `st20p_tx_get_session_stats` → `st20_tx_get_session_stats`, takes S (`st20_pipeline_tx.c:1311` → `st_tx_video_session.c:4760`). Tasklets `trylock` S (`st_tx_video_session.h:29-35`), so every stats read makes the builder and transmitter skip the session for an iteration. That is a pacing hazard under TSC pacing.
  - 08 §2.4 then puts "pipeline and transport counters written by their single owner into the same seqlock-protected area": two writers on one seqlock.
- **Suggested change:**
  - Use per-writer counter blocks, each on its own cache line: tasklet, app-thread (pipeline), completion context, admin. Each is written with relaxed stores by its one writer, and the reader sums them. Use a seqlock only for grouped per-unit values written by one tasklet.
  - Replace dump-and-reset with reader-side deltas, and compute CPU busy without S.
  - This is L0 work in `tv_*`/`rv_*` and in every pipeline, and it is needed for G-40 in Phase 1. Move it from Phase 5 to Phase 1, or drop G-40 from the Phase 1 exit.
  - Add "tasklets `trylock` S; any app/admin/stat holder makes them skip" to the risk table.
- **New open question:** none.

### 7. "The tasklet never makes a syscall" is false for two backends and for built-in PTP time reads (major)

- **Where:** [02 §1](../02-architecture.md) rule 2, [04 §2](../04-threading-and-execution.md) TK row, G-39, [04 §8](../04-threading-and-execution.md) / E9.
- **Issue:** **[verified]** unless marked.
  - **Kernel-socket TX makes syscalls on the tasklet.** Without the socket thread ring, `mt_tx_socket_burst` calls `sendto`/`sendmsg` directly on the tasklet (`datapath/mt_dp_socket.c:421-445`, `:79-80`, `:132`, `:148`).
  - **AF_XDP TX makes one too.** It kicks the socket with `send()` on the tasklet (`dev/mt_af_xdp.c:522-526`).
  - **Built-in PTP reads the PMD on every call.**
    - With built-in PTP, every `mt_get_ptp_time` on a tasklet dispatches to `ptp_from_eth` → `rte_eth_timesync_read_time` (`mt_main.h:1957-1959`, `mt_ptp.c:393-402`, `:104-116`).
    - Callers include late checks and pacing (`st20_pipeline_tx.c:133`, `st_tx_video_session.c:615`, `:695`, `:1797`, `st_video_transmitter.c:573`, `:628`), so this is a PMD register read per call.
    - **[inferred]** On an iavf VF this may be a virtchnl round-trip to the PF.
  - 04 §8 replaces only the user `ptp_get_time_fn`.
- **Suggested change:**
  - Scope G-39 per backend. DPDK PMD: no syscalls. Socket and AF_XDP: "backend I/O syscalls on the scheduler thread" as a granted, reported property.
  - Extend E9's published time base to *every* time source: tasklets always compute TAI as TSC × ratio + offset from a seqlocked record, and the PTP/admin context refreshes it.
  - Add both to the 14 risk table.
- **New open question:**
  - Q-THR-7a: *Should the published time base replace built-in PTP PHC reads on tasklets too (recommended), accepting TSC-extrapolation error between servo updates?*

### 8. EQ coalescing in an SPSC ring races, and the EQ has more producers than stated (major)

- **Where:** [07 §3.3–3.4](../07-completions-events-and-errors.md), [07 §2](../07-completions-events-and-errors.md) (BACKPRESSURE event from `acquire`).
- **Issue:**
  - **Coalescing races.** "Events of the same (type, source) that are still unread merge into one record" means the producer rewrites a slot the consumer may be reading at that moment. That is not possible in a plain SPSC ring without per-slot sequencing.
  - **BACKPRESSURE and user events.** BACKPRESSURE is posted by the app thread inside `acquire`, and `mtl_eq_post` is app-side too, so a per-session ring gains a second producer.
  - **The instance ring gets a tasklet producer.** The PTP/CNI code runs as a tasklet on `main_sch` unless the CNI thread mode is set (R03 §1, `dev/mt_dev.c:1954`). Its TIME_STATE events would enter the "instance ring" that admin and CNI threads also write.
  - **The spin hazard.** With an MP `rte_ring` (default or HTS), a preempted non-pinned producer makes the tasklet producer wait in the tail update. That is the R03 §5 hazard. Today's audio HTS ring is safe only because all its producers are tasklets (`st_tx_audio_session.c:1704`) **[verified]**.
- **Suggested change:**
  - For tasklet-produced events, use per-source pending state instead of a ring: a pending-type bitmask, a per-type seqlocked "latest payload", and occurrence counters. The EQ reader turns them into records. Coalescing is then free and wait-free.
  - Keep a ring only for non-pinned producers, one ring per producer class (admin, CNI thread, app), inside the poll set.
- **New open question:** none.

### 9. The performance gates in 13 §8 miss the metrics that detect pacing damage (major)

- **Where:** [13 §8](../13-guarantees-and-tests.md), Phase 1 exit.
- **Issue:**
  - "submit → pick-up ≤ `put_frame` → `next_frame`" is tautological in v1, because it *is* that path (finding 1).
  - "W3 p99 ≤ interval + 10 µs" is not achievable on a default kernel (finding 4).
  - "Loop time unchanged within noise" is a mean. Pacing is damaged by tails.
  - Completion latency, waker CPU and 2110-21 compliance are not gated at all.
- **Suggested change:** gate on:
  - p99.99 and max tasklet iteration time per scheduler (`MTL_FLAG_TASKLET_TIME_MEASURE` already exists, `mt_sch.c:203-209`);
  - completion latency (last packet → entry visible to the app), expected versus measured (finding 2);
  - waker CPU % and wake latency p50/p99/p99.9 per waker mode;
  - DP call cost *excluding* conversion;
  - ST 2110-21 narrow compliance (timing parser with HW timestamps) for TSC and RL pacing, with a stats reader in a tight loop and 64 armed waiters. This makes G-27's measurement a Phase 1 regression gate, not a Phase 3 claim.
- **New open question:** none.

### 10. The update_destination command is feasible for the PMD, but it is not a pointer swap and misses two consumers (major)

- **Where:** [04 §3.2](../04-threading-and-execution.md), D-26, [09 §1.1](../09-media-modes-and-backends.md).
- **Issue:**
  - **What happens today.** **[verified]**
    - `tv_update_dst` rewrites the embedded header template `s->s_hdr[]` in place under S, including the ARP wait (`st_tx_video_session.c:3818-3840`, `:903-935`).
    - The builder copies that template into every packet (`:1062`, `:1107`, `:1213`, …).
    - The fix is a double-buffered template applied by the builder at a frame boundary, for both legs together, plus the ARP moved to a worker. That is feasible and small.
  - **Kernel-socket GSO keeps the old destination.** It sends to `t->send_addr`, fixed when the queue is created (`datapath/mt_dp_socket.c:236`, `:148`) **[verified]**. The header change does not redirect those packets **[inferred]**, so the update needs a TX-queue re-create, which is a recovery-style swap.
  - **RTCP keeps the old header.** The RTCP TX header is copied from `s_hdr` at init (`:1023-1025`) and `tv_update_dst` does not touch it **[verified]**, so retransmits keep the old destination **[inferred]**.
  - **"Ack" is not "the wire switched".** Packets already in `s->ring[]` and in NIC descriptors still carry the old header.
- **Suggested change:**
  - Specify the command as `{flow, activation media index or NOW}`. The ack returns "first unit on the new destination = k"; that also matches NMOS IS-05 scheduled activation.
  - For backends where the destination lives in the queue, the worker creates the new queue and the tasklet swaps it at a boundary.
  - For RX `update_source`, add the new flow rule to the same queue before removing the old one, which avoids a queue swap. Do the IGMP work on the worker.
  - Log the two gaps as side findings.
- **New open question:** none.

### 11. Moving recovery and auto-detect off the tasklet is feasible; the quiesce handshake needs to be specified (minor)

- **Where:** [04 §2](../04-threading-and-execution.md) WK row, R03 C6, Phase 5.
- **Issue:**
  - **Recovery touches more than the transmitter.** It swaps `s->queue`, recreates mempools, and forces DONE on in-flight frames (`st_tx_video_session.c:4231-4331`) **[verified]**.
  - **A worker can quiesce with S.** Builder and transmitter both hold S per iteration and only `trylock` it. A worker that takes S with a blocking lock therefore quiesces the session without spinning a pinned core.
  - **Two paths can signal DONE.** `tv_frame_free_cb` reads `refcnt`, notifies, then decrements (`:127-135`), while recovery reads, notifies and resets (`:4295-4300`). Both can report DONE for the same frame when the free runs in another context (TSQ, socket thread). That is issue #1147's class, and moving recovery to a worker widens the window.
  - **Audio recovery spins on a tasklet.** It takes a blocking S on the tasklet (`st_tx_audio_session.c:2742`).
  - **Latency is not a concern.** The hang threshold is 1 s (`st_tx_video_session.c:3327-3329`), so a worker hop adds nothing that matters.
  - **The worker needs a wake-up.** The tasklet cannot wake it, so it needs the waker (finding 4).
- **Suggested change:**
  - Specify the handshake:
    1. the tasklet sets `RECOVERY_REQUESTED[port]`, stops building and bursting that port, and posts to the worker;
    2. the worker takes S, does the swap and re-create, and releases S;
    3. per-frame completion becomes a CAS claim (1 → 0), so it runs exactly once.
  - Do the same for RX auto-detect (`rv_init_sw`, `st_rx_video_session.c:2839`). RX queue overflow during reallocation is acceptable.
- **New open question:** none.

### 12. The engine has lost-completion paths that G-01 will hit (minor)

- **Where:** G-01, [14 §0](../14-implementation-roadmap.md).
- **Issue:**
  - `tx_st20p_next_frame` claims a frame (`CONVERTED` → `IN_TRANSMITTING`, `st20_pipeline_tx.c:210-213`). The builder then returns without building if `refcnt != 0` or the user meta is too large (`st_tx_video_session.c:1936-1953`). **[verified]**
  - The pipeline slot then stays `IN_TRANSMITTING` forever with no result. **[inferred]**
  - The `refcnt != 0` case is reachable through SF-05's ordering when the free callback runs on another thread. **[inferred]**
- **Suggested change:** add a "rejected at pick-up" engine callback that returns the slot with a `DROPPED/INVALID_PAYLOAD`-style status. Add this to the Phase 0 list next to SF-05.
- **New open question:** none.

### 13. The DP class promises O(1), but `submit` / `dequeue` convert in the caller (minor)

- **Where:** [04 §3](../04-threading-and-execution.md) versus [05 §6.3](../05-memory-and-buffers.md). C3 P1-5 raises the same point.
- **Issue:** with an internal converter, `put_frame` runs the converter inline (`st20_pipeline_tx.c:869-872`), which is O(frame). The same is true of RX `get_frame`. This is off the tasklet, so it is fine for the pinned cores, but it is not "O(1), bounded time".
- **Suggested change:** add a DP subclass "DP-convert", bounded by frame size, and exclude it from the 13 §8 DP cost gate.
- **New open question:** none.

### 14. The documents disagree on which log levels a tasklet may emit (minor)

- **Where:** [02 §1](../02-architecture.md) rule 2 and [04 §2](../04-threading-and-execution.md) ("no logging above DEBUG") versus [08 §6](../08-observability.md) and G-44 ("nothing at INFO or below").
- **Issue:**
  - "INFO or below" forbids DEBUG and allows WARNING/ERR, which is the reverse of 04.
  - Today tasklets log at every level: `info()` on a late-drop batch (`st20_pipeline_tx.c:197`), `warn()` in the free callback (`st_tx_video_session.c:129`), `err()` on hang detection (`st_video_transmitter.c:38`). **[verified]**
- **Suggested change:** in release builds tasklets never format or print at any level. Everything goes through the binary log ring, rate-limited per (code, session). Fix G-44's wording.
- **New open question:** none.

### 15. "Never allocate" needs a precise definition (minor)

- **Where:** [04 §2](../04-threading-and-execution.md) TK row, G-39.
- **Issue:**
  - Tasklets take mbufs from mempools for every packet, and `st20_frame_tx_start` allocates one per frame (`st_tx_video_session.c:4350`). **[verified]**
  - In `MTL_FLAG_TASKLET_THREAD` mode the scheduler is a non-EAL thread with no mempool cache, so every get/put hits the shared ring (KB §2). **[inferred]**
- **Suggested change:** define the rule as no `malloc`/`rte_malloc`/mempool or ring create, while mempool get/put is allowed. G-39's hook must cover `rte_malloc*`, not only libc.
- **New open question:** none.

### 16. RX `ZERO_MISSING` fill must not run on the RX tasklet (minor)

- **Where:** [05 §8](../05-memory-and-buffers.md), Q-MEM-6.
- **Issue:** the fill is described as "a write per missing range at delivery", with no statement of where it runs. If a leg is lost on a single-leg session, the missing range is the whole frame (about 5 MB at 1080p 10-bit). A memset that size on the RX tasklet takes hundreds of µs and drops packets for every other session on that core. **[inferred]**
- **Suggested change:** do the fill in the consumer's `dequeue`, as caller-context work, or on a worker, and report it as a data-path cost.
- **New open question:** none.

### 17. "Always-on" timing-parser summary counters add per-packet cost (minor)

- **Where:** [08 §1](../08-observability.md) ("always on (cheap)"), Q-OBS-3.
- **Issue:**
  - `rv_tp_on_packet` does double-precision divisions for every packet (`st_rx_timing_parser.c:13-60`). **[verified]**
  - The timestamp comes from `mt_mbuf_time_stamp`, which falls back to a time read when there is no HW timestamp (`mt_ptp.c:1635-1638`). **[verified]**
  - At 4K60 (about 1.1 Mpps per stream) that is measurable. **[inferred]**
- **Suggested change:** keep the parser opt-in, or compute the summary only for HW-timestamped packets using integer math. Measure before promising "always on".
- **New open question:** none.

### 18. The observed TX time taken at `tx_burst` is enqueue time in RL mode (minor)

- **Where:** [06 §7.5](../06-timing-pacing-and-sync.md) `observed_first`, E4, S5.
- **Issue:**
  - RL mode enqueues warm-up pads and then bursts packets as long as the NIC ring accepts them (`st_video_transmitter.c:100-140`, `:150-175`). **[verified]**
  - The TSC at `tx_burst` therefore precedes the wire time by the ring occupancy, up to about 512 × TRS. **[inferred]**
- **Suggested change:** name the SW value `enqueued_first`. Base ON_TIME / LATE on the admission decision, not on SW observation. Only HW TX timestamps populate `observed_*`.
- **New open question:** none; fold into Q-TIME-10.

### 19. Internal contradictions (minor)

- **Submission ring:** [02 §4.1](../02-architecture.md) and [02 §6](../02-architecture.md) describe a submission ring the tasklet pulls from. [02 §2.2](../02-architecture.md) maps `submit` onto `put_frame`. v1 has no ring, and S3 measures a ring that will not be built-in Phase 1.
- **Start/destroy handshake:** [03 §3.3](../03-object-model-and-lifecycle.md) and [03 §6.2](../03-object-model-and-lifecycle.md) say start and destroy use "the register/ack handshake that already exists in `mt_sch.c`". **[verified]**
  - Video sessions attach to the manager's `sessions[]` under S (`tv_mgr_attach`, `st_tx_video_session.c:3753`) and do not register a tasklet.
  - The `mt_sch.c:873-966` handshake is for tasklets.
- **Completion latency:** [04 §4.1](../04-threading-and-execution.md)'s "microseconds" contradicts [05 §4.1](../05-memory-and-buffers.md)'s own `nb_tx_desc` reasoning (finding 2).
- **Suggested change:** reconcile the texts after Q-ARCH-1a.

### 20. Stop(ABORT) → FLUSHED needs a pipeline reclaim (minor)

- **Where:** [03 §3.4](../03-object-model-and-lifecycle.md).
- **Issue:**
  - Queued units sit in `CONVERTED` (or `READY` while waiting for a plugin). The pipelines only have `put_frame_abort` for `IN_USER` (`st20_pipeline_tx.c:902-927`). **[verified]**
  - A CAS `CONVERTED` → FLUSHED is safe against the builder's CAS (`:210-213`).
  - `IN_CONVERTING` belongs to the plugin, so an abort is bounded by the plugin.
- **Suggested change:** add the reclaim to finding 1's L0 list, and document "abort waits for in-progress conversion".
- **New open question:** none.

### 21. `mt_in_sch_thread` misses other pinned contexts (minor)

- **Where:** [04 §3.3](../04-threading-and-execution.md).
- **Issue:** the RX packet lcore (`USE_MULTI_THREADS`, which fires callbacks, `st_rx_video_session.c:2470-2479`) and the TAP lcore are pinned busy loops, but not scheduler threads. **[verified]** R03 §1.
- **Suggested change:** set the thread-local flag in every library busy loop, not only in `sch_tasklet_func`.
- **New open question:** none.

## Risks missing from the 14 risk table

| Risk | Phase | Mitigation |
|---|---|---|
| The pipelines must change: a held slot state, a once-only completion hook, a flush reclaim (finding 1) | 1 | budget L0 work in Phase 1; UB tests against the real `st*p` code |
| TX completion latency is about `nb_tx_desc` packets and unbounded while idle (finding 2) | 1 | idle `tx_done_cleanup`; report the latency |
| A lost wake-up if the fences are wrong (finding 3) | 1 | seq_cst fence; litmus test |
| Waker CPU cost, timer slack, inherited affinity (finding 4) | 1 | deadline-driven waker; explicit affinity; S1 reports CPU |
| Tasklets `trylock` S, so every app/admin/stat holder makes them skip; stats have several writers (finding 6) | 1 | per-writer counters; no S on read paths |
| Syscalls on tasklets for socket/AF_XDP and PHC reads for built-in PTP (finding 7) | 1–5 | per-backend G-39; published time base for every source |
| EQ producers include tasklets and preemptible threads on one ring (finding 8) | 1 | per-source pending state; per-class rings |
| Destination updates do not reach socket GSO or RTCP (finding 10) | 2 | queue swap on update; side findings |

## Things the design got right

- It forbids BLOCK_GET and its mutex/condvar on the tasklet, and it answers H3 with an armed waiter plus an unpinned waker, with W0/W2 as explicit opt-ins.
- It routes destination/source updates through commands applied at a unit boundary, so no update holds S across ARP. Today's model (H6) is broken.
- It uses per-session single-producer rings plus poll sets instead of MPSC CQs, and keeps shared CQs out of the tasklet's concern.
- It sizes completion capacity to the pool, so the tasklet never waits for the app.
- It makes a CP/WT call from a scheduler thread fail with `-EDEADLK` via a thread-local flag, which turns H2-class self-deadlocks into test failures.
- It recognises that the free callback runs in many contexts (H8) and that migration breaks single-producer assumptions.
- It uses stats epochs instead of reset-on-read, a published time base instead of a per-read user callback, and a binary log ring for tasklets.
- Phase 0 includes the SF-05 free-callback ordering fix that everything in finding 1 depends on.
