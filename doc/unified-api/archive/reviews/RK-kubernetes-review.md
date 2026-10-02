# RK — Adversarial review of the Kubernetes and crash-safety design (16, R8, health, shutdown)

| | |
|---|---|
| Status | Review, 2026-10-01. Read-only: no file other than this one was changed |
| Scope | [16](../16-kubernetes-and-crash-safety.md); `sketch/include/mtl/experimental/` `mtl.h` (R8, open, close, abort, flags), `mtl_observe.h` (health, shutdown), `mtl_options.h` 2020–2028, 2123–2125, 2207–2208, `mtl_reasons.h`, `mtl_queue.h` 23–25, `mtl_util.h`; `ex11_signal.c`, `ex01_tx_video.c` |
| Inputs checked against | K1 (K-REQ-1…20), K2 (H-K-1…28, §9), K3 (P-1…P-12, §4, §6); 03 §2.2, §3.4, §6, §7.1; 04 §4.5, §5.2, §5.4, §7.1; 15 §8; MTL `lib/` at `545a266a`; DPDK 26.07 source on this host |
| Legend | **[verified: source]** = I read it in this session (file:line, or the URL). **[inferred]** = reasoning or knowledge not re-checked here; treat as a hypothesis |

## 0. Verdict

The direction is right: one bounded, network-first shutdown; crash-only rules; liveness that ignores the network;
CPUs and time taken from the pod; fail-fast open. The design is not yet safe as written. Four things would bite
in a real pod:

1. **The example pod does not work.** A non-root container does not get `IPC_LOCK` from `capabilities.add`, and
   the device plugin's `/dev/vfio/N` is root-owned (RK-1). The startup probe uses readiness, so a grandmaster
   outage at start crash-loops the pod (RK-2). One SR-IOV resource for both 2022-7 legs can put both legs on one
   PF (RK-3).
2. **The shutdown order releases MtlManager grants before the schedulers and AF_XDP sockets stop** (RK-4). That
   recreates the lcore double-booking K2 §5.1 found in today's code.
3. **Implicit closing is not backed by the object model.** Handle tables are per instance and freed at teardown
   (03 §2.2), yet sessions, regions and queues closed by the instance must stay callable afterwards (RK-5). A
   threads-in-DP-call drain step is missing (RK-6). An AS `mtl_instance_abort` during step 6 can touch freed
   memory or write into a recycled eventfd number (RK-7).
4. **R8 promises what the stack does not do.** DPDK opens the VFIO group and container fds and its hugepage memfds
   without `O_CLOEXEC`, and swaps the process SIGBUS handler on every heap growth. `O_CLOEXEC` also does nothing
   for a fork that does not exec, and such a child keeps the VFIO fds, the manager socket and the OFD CPU locks
   alive (RK-8, RK-9).

Health is close to right. Two liveness rules would cause restarts the design says it avoids: `PORT_REMOVED` on one
2022-7 leg (RK-10), and `WORKER_STALLED` during a PF reset that hits every VF on the node at once (RK-11).

## 1. Findings

Severity: **Critical** = UAF, DMA into freed memory, or the documented deployment cannot work; **High** = a hang
past the deadline, lost results, a restart storm, or a direct contradiction of an existing rule; **Medium** =
wrong under a specific but realistic condition; **Low** = wording, drift, polish.

| ID | Sev | Location | Finding (short) | Fix (short) |
|---|---|---|---|---|
| RK-1 | Critical | 16 §10.3, §10.4 | Non-root + `add: [IPC_LOCK]` leaves CapEff empty, and vfio nodes are root-owned: the pod spec cannot pin DMA memory | Document the three ways that work; set `runAsUser`/`runAsGroup`; require runtime device ownership; open fails with `CAPABILITY_MISSING` naming the cause |
| RK-2 | High | 16 §10.4 `startupProbe` | The startup probe hits `/ready`, so a GM or link outage at start kills the container after 240 s | A third endpoint `/started` = phase ≥ LINKS (or DEVICES) and no liveness bit |
| RK-3 | High | 16 §10.4, §7 | One resource `e810_media` for both legs gives no leg-to-PF guarantee; `#0`/`#1` order is the plugin's, not the leg order | One resource per network (red, blue); join IP to PCI through `device-info` |
| RK-4 | High | 16 §2.2 steps 4–5; mtl.h:452-463 | Manager grants (lcores, AF_XDP queues, XDP, flows) are returned before schedulers stop and XSK sockets close | Move "close the manager socket" after step 5 (or to process exit); add an explicit "stop and join schedulers" step |
| RK-5 | Critical | 16 §2.1; 03 §2.2, §6.1, §7.1 | Implicitly closed sessions, regions, queues and timelines must stay callable after the instance retired, but the tables that resolve their handles are per instance and freed at teardown | Process-global, never-freed handle tables (or tombstones that outlive the instance); update 03 §2.2/§6.1/§7.1 |
| RK-6 | High | 16 §2.2 table | No step waits for application threads inside DP/DPC calls (03 §6.2 "wait for DP callers"), dropped from K3 P-4 | Add step 0b: drain the in-flight counters to the deadline; count leftovers in `sessions_retiring`; never free what they reach |
| RK-7 | Critical | mtl.h:467-470; mtl_observe.h:262-268; ex11 | `mtl_instance_abort` (AS) during shutdown races the frees and the eventfd closes: a UAF, or 8 bytes written into whatever reused the fd (for example `/dev/termination-log`) | Keep AS-reachable state in the never-freed handle slot; an AS in-flight counter the closer waits on before close(fd) |
| RK-8 | High | mtl.h R8 (69-77); 16 §2.5 | DPDK 26.07 opens the VFIO group and container fds and its memfds without CLOEXEC, and swaps the SIGBUS handler on every heap growth: R8 is false as stated | E-15 must patch or post-fix DPDK fds; preallocate the heap at open, or state the SIGBUS exception in R8 |
| RK-9 | High | mtl.h R8; 16 §3 rows "Lcores, lockfiles", "MtlManager" | CLOEXEC does not act on fork. A child that does not exec keeps the VFIO fds, the manager socket and the OFD locks, so SIGKILL of the parent does not release them | `pthread_atfork` child handler closes every MTL-tracked fd; say so in R8 and drop "safe" for non-exec children |
| RK-10 | High | 16 §8 table; mtl_observe.h:197 | `PORT_REMOVED` is liveness, so a pod still sending on its surviving 2022-7 leg is restarted (16 §4 says it continues degraded) | Liveness only when some started session has no leg left, or every port is gone; else readiness |
| RK-11 | High | 16 §8; mtl_observe.h:196 | `WORKER_STALLED` (liveness) trips while the admin worker runs a synchronous `rte_eth_dev_reset`; a PF reset resets every VF, so every pod on the node can restart together | Stall limit for the worker ≥ reset budget, or heartbeat exempt inside bounded known-blocking steps |
| RK-12 | High | mtl.h:458-459 vs mtl_observe.h:259; 16 §2.3 vs §2.4 | "freed when they are returned" contradicts "never freed" (`bytes_kept_until_exit`) and "no library thread outlives the call"; a DP release cannot free (03 §6.2) | Pick one: kept until exit (Q-K8S-2) and fix mtl.h, or define which later CP call frees it |
| RK-13 | High | 16 §2.3; mtl_observe.h:240-244 | `threads_unjoined > 0` with ports stopped fits none of the three outcomes: "quiesced" promises no library thread runs app code | Add an outcome, or define 1 as "device quiesced" only and report the threads apart; never 0 with threads unjoined |
| RK-14 | High | mtl_queue.h:134-138; mtl_observe.h:284-290 | Closing or shutting down the instance from a dispatch or log-sink callback self-joins in step 3; mtl_queue.h allows it (only session and queue calls are `-MTL_EDEADLK`) | Add instance close/shutdown to the `-MTL_EDEADLK` lists; mtl.h close says "not from a library thread" |
| RK-15 | High | 16 §2.2 step 3; mtl.h close "Unread results are discarded" | Results flushed in step 1 are dropped once the dispatch thread is joined; a zero-copy framework that unrefs buffers on results leaks them and its own pool teardown hangs outside MTL | Step 3 delivers pending results and terminal events to callbacks within the budget, then joins; report `results_discarded` |
| RK-16 | High | 03 §7.1; 16 §2.3; mtl.h:460-461 | Re-open in the same process after close is undefined: EAL is kept (03 §7.1), but after `-MTL_ETIMEDOUT` the port is quarantined, and a GStreamer NULL→PLAYING re-opens the SHARED instance | Specify: 0 or 1 → re-open allowed; quarantined ports → `-MTL_EBUSY` (`QUEUE_QUARANTINED`) until exit; EAL args fixed at first open |
| RK-17 | Medium | 16 §2.2 "never runs past its deadline" | `rte_eth_dev_reset` and queue stop are synchronous and can start just before the deadline; the call then overruns. K3 rule 1 had "timeout + reset budget" | Start a reset only if the remaining budget ≥ the reset budget, else quarantine; publish the budget (stat `caps.reset_budget_ns`) |
| RK-18 | Medium | 16 §2.2 Grace period; ex11:60-62 | `grace − 2 s` is measured from the shutdown call, not from SIGTERM; preStop time (which 16 itself recommends for NMOS) and worker joins are not deducted | Record the SIGTERM time in the handler (`clock_gettime` is AS) and pass `deadline − now`; say "grace − preStop − margin" |
| RK-19 | Medium | mtl_observe.h:239 (`MTL_SHUTDOWN_DRAIN`) | DRAIN "bounded by the deadline" can use the whole deadline, leaving no time for leaves and quiesce: network-first is lost | Drain stops at `deadline − reserve` (leaves + quiesce + reset budget) |
| RK-20 | Medium | mtl.h:293; 16 §2.2 step 1 | "A unit partly sent is finished": with `tx.rows_late = STALL` a rows unit waits for rows that the departed workers will never publish, until the deadline | Shutdown overrides STALL with TRUNCATE (or PAD) |
| RK-21 | Medium | mtl.h R2 vs close:460 | `-MTL_ETIMEDOUT` means "drain incomplete, safe" for stop and "device not quiesced, exit now" for instance close | Use a distinct code or reason for the unsafe case (for example `-MTL_EIO` + `QUEUE_QUARANTINED`); keep ETIMEDOUT for the benign one |
| RK-22 | Medium | mtl_observe.h:238; mtl.h:398, :462 | SHARED handles are values: one component's double close drops another's reference, and after `ALL_REFERENCES` the others' handles are undefined | Per-reference handle IDs; after ALL_REFERENCES the others' close returns 0 and their calls `-MTL_ESHUTDOWN` (`INSTANCE_SHUTDOWN`) |
| RK-23 | Medium | ex11:53-56, 62; mtl.h close "consumes mt" | The probe thread calls `get_health(mt)` while the main thread consumes `mt`: liveness then returns 503 (EBADF) during and after shutdown | Handle readable until close returns; ex11 maps EBADF after shutdown to readiness 503 and liveness 200 |
| RK-24 | Medium | 16 §8; mtl_observe.h:195 | Stopped schedulers in step 5 look stalled; under a CFS quota (Burstable pods, clusters without `DisableCPUQuotaWithExclusiveCPUs`) the 100 ms default equals the CFS period | Mask `SCHED_STALLED` once shutdown begins; liveness at ≥ 10× `stall_ns` (or ≥ 1 s), events at 1×/2×/4× |
| RK-25 | Medium | 16 §5 "Busy polling on shared CPUs" | "A CFS quota is present" is true for every Guaranteed pod on clusters with the gate off; WARN is noise and REFUSE refuses good pods | Flag only `quota / period < CPUs in the affinity mask` |
| RK-26 | Medium | 16 §5 Pinning; mtl_options.h:144-145, :161 | Housekeeping defaults to the last CPU, but EAL's main lcore is the first CPU of the mask and never a scheduler; the last CPU may get a scheduler. `sys_lcore`, `main_lcore` and `housekeeping_cpus` overlap | One knob; default housekeeping = the main lcore's CPU, excluded from schedulers; report placement (K-REQ-9) |
| RK-27 | Medium | 16 §5; 04 §5.2, §7.1 | 04 says TASKLET_THREAD schedulers are unpinned and uses that to justify W2 eventfd writes there; 16 pins them | Update 04 §5.2/§7.1, or re-argue W2 for pinned thread mode |
| RK-28 | Medium | 16 §10.1; mtl.h:399; 15 §8.2 | `MTL_INSTANCE_MANAGER_OPTIONAL` implies the manager is required by default; 16 says a pod needs none, and `cpu_arbitration` auto picks the manager whenever its socket exists, against K-REQ-8 | Remove the flag; auto = no arbitration when the cpuset is exclusive, and the manager only for AF_XDP grants |
| RK-29 | Medium | 16 §10.2; mtl_options.h:195-197; §11 | `port.xsk_map` without the manager needs an engine change: native AF_XDP refuses to start without MtlManager today. No E-item covers it | Add E-19: accept an XSK map (bpffs/fd/uds) without the manager |
| RK-30 | Medium | 16 §6 `time.phc_trust` detect | PHC agreeing with CLOCK_TAI cannot tell a GM-locked PHC from a node in holdover or free-run: phc2sys keeps CLOCK_REALTIME on the drifting PHC, so MTL reports LOCKED | Detect only as a start condition, plus `mtl_time_set_reference` state; doc says detect cannot see GM loss |
| RK-31 | Medium | 16 §7 Ports; mtl_util.h:44-55 | `network-status` may be absent or stale when the container starts (written by Multus, refreshed by the kubelet); `env:` names alone carry no IP | Helper returns `-MTL_EAGAIN` (`PORT_ENV_UNSET`) for a missing entry, app retries; document `env:VAR#n=ip/prefix` |
| RK-32 | Medium | mtl.h:403-406 (`PREFLIGHT`) | Hugepages, CPUs and memlock are per container, so an init container checks its own limits, not the app's; it also skips every device check | Drop the flag from the core; offer a tool or `mtl_util` call, documented as node-level checks only |
| RK-33 | Medium | ex11:25-35 | Handlers are installed after open; as PID 1 a SIGTERM during open is discarded, not pending, and the pod waits for SIGKILL | Block SIGTERM/SIGINT before open, install handlers, then unblock |
| RK-34 | Medium | 16 §3 table, residuals | Rows dropped from K3 §4: timeline, codec plugin, `mtl_queue`. Residual missing: AF_XDP `tx_maxrate` after SIGKILL (H-K-19, written by the library) | Add the rows; add the residual with owner M (reset on queue grant) |
| RK-35 | Medium | 16 §10.4; K1 §1.3 | A Deployment RollingUpdate runs old and new senders on one group; K1 recommended `strategy: Recreate`, and 16 dropped it | Add the guidance to §10.4 |
| RK-36 | Low | 16 §10.3 table | AF_XDP `NET_RAW` for non-root has the same capability problem as RK-1; `SYS_NICE` gates `get_mempolicy`/`set_mempolicy`/`mbind` under RuntimeDefault | State both; without `SYS_NICE`, libnuma reports NUMA unavailable and DPDK's NUMA placement is silently off |
| RK-37 | Low | mtl_observe.h:230 | `detail[96]` cannot be "atomics only"; it needs a seqlock or double buffer | Seqlock with bounded retries, else return flags with detail ""; or compose detail in the caller from atomic fields |
| RK-38 | Low | 16 header row; mtl_options.h:155 | 2028 is the existing `instance.profile`, not a pod key | Fix the range in 16 |
| RK-39 | Low | mtl.h:393 (`mac`) | An output field in an input struct: round-tripping `mtl_port_get_spec` into open is `-MTL_EINVAL` under R3 | "Ignored on input" |
| RK-40 | Low | mtl.h close; R2 | `timeout_ns = 0` and `MTL_FOREVER` are undefined for close; FOREVER reintroduces unbounded waits | 0 = abort semantics now; FOREVER is capped at the reset budget per step |
| RK-41 | Low | LEARN.md:59; mtl_legacy.h:22 | One-argument `mtl_instance_close(mt)` still in LEARN.md; the legacy wrapper's close vs implicit session close is undefined | Update both |
| RK-42 | Low | mtl_options.h:200 | `port.igmp_report_ms` default 1000 ms changes today's 10 s and is not RFC 3376 behaviour (unsolicited reports repeat Robustness times, then the querier drives) | Keep 10 s until a query is seen; then answer queries |
| RK-43 | Low | 16 §9 Memlock | "Inside a user namespace it is the limit of the namespace's owner" is not how vfio accounting works; the point is that `IPC_LOCK` in a user namespace does not lift it (K1 §3.2) | Reword; also mention `dma_entry_limit` (K-REQ-10) |

## 2. Q1 — K-REQ coverage (16 §12 claims all twenty)

| K-REQ | Verdict | Why |
|---|---|---|
| 1 bounded shutdown | PARTIAL | Deadline-bounded in intent, but resets can overrun (RK-17); preStop not deducted (RK-18); the K1 "documented typical cost" lacks the reset budget number |
| 2 network first | PARTIAL | Manager release precedes device stop (RK-4); DRAIN can eat the leave budget (RK-19) |
| 3 signal-safe trigger | PARTIAL | K-REQ-3 asks to document DPDK's temporary SIGBUS handler; R8 says no handler (RK-8); abort during shutdown is unsafe (RK-7); ex11 PID-1 race (RK-33) |
| 4 crash-only | PARTIAL | Fork inheritance (RK-9), DPDK fds without CLOEXEC (RK-8), `tx_maxrate` residual missing (RK-34) |
| 5 restart in place | MET | SysV and `kill(pid,0)` removed (E-6); reconcile at open (E-16) |
| 6 fail fast | MET | Reasons 600–614 and one-line detail; no example writes the open failure to the termination log (Low, see RK-33 fix) |
| 7 health | PARTIAL | Startup probe on readiness (RK-2); liveness bits that restart healthy pods (RK-10, RK-11); handle validity (RK-23) |
| 8 CPUs from the mask | PARTIAL | Manager arbitration turns on whenever the socket exists (RK-28); "lcore IDs are CPU IDs" needs `--remap-lcore-ids` dropped, which no E-item names (`dev/mt_dev.c:444` [verified]) |
| 9 every thread pinned | PARTIAL | Housekeeping and main-lcore conflict (RK-26); 04 contradiction (RK-27); "placement is reported" (K-REQ-9) is not specified |
| 10 cgroup budget | PARTIAL | `dma_entry_limit` dropped (RK-43); cgroup v1 filenames not mentioned (Low) |
| 11 ports from Kubernetes | PARTIAL | Leg mapping (RK-3), annotation timing and IP (RK-31) |
| 12 VF caps probed | PARTIAL | spoofchk (K-REQ-12) is missing from the 16 §7 probe list; the "17th group" is hard-coded although MTL also adds `01:00:5e:00:00:01` (`mt_mcast.c` [verified in K2 :484-485]); probe the count |
| 13 no VF history | MET | E-16 and "never sets the MAC" |
| 14 IOMMU policy | MET | `allow_noiommu`, `NO_IOMMU`; behaviour change acknowledged in Q-K8S-6 |
| 15 time | PARTIAL | Detect heuristic cannot see GM loss (RK-30); "who disciplines it" (K-REQ-15) has no field in `mtl_health` or a named stat |
| 16 privilege profile | NOT MET | The profile as written cannot pin memory as non-root (RK-1, RK-36) |
| 17 MtlManager | PARTIAL | E-5 covers the manager; library-side order (RK-4) and default (RK-28) do not |
| 18 AF_XDP node-owned | PARTIAL | API is right; the engine fix is missing (RK-29) |
| 19 multi-container | PARTIAL | K-REQ-19's "a sidecar gateway's bounded close must also cover clients that never said goodbye" is waved away in 16 §12 |
| 20 multicast | MET | Leaves on every leg, window documented; default interval changed without reason (RK-42) |

Quietly dropped from the inputs:

- K3 P-4 pitfall, threads inside DP calls during the forced round (RK-6).
- K3 P-3 pitfall: "quiesced" depends on spike S8, whether iavf/ice queue stop releases chained external mbufs (04 §4.5 [verified]). 16 lists SF-K3-6 but not S8 as a gate for the outcome-1 claim.
- K3 P-2 pitfall: idempotency and "callable from any non-tasklet thread" (RK-14).
- K3 §4 rows: timeline, codec plugin, manager-less XDP, files/IPC (RK-34).
- K2 §5.1: lcores released before schedulers stop, reintroduced by step 4 (RK-4).
- K1 §1.3: RollingUpdate and duplicate senders (RK-35). K1 §1.5: init containers do not share the app's devices or limits (RK-32).

## 3. Q2 — crash and shutdown correctness, details

**RK-4 (order).** Step 4 closes the manager connection, and the manager's `~mtl_instance()` returns lcores, queues
and flows at EOF ([verified: K2 §4, `manager/mtl_instance.hpp:66-89` as cited]). Schedulers are never stopped
explicitly in 16 §2.2, and the AF_XDP sockets and their UMEM go only in step 5. Between steps 4 and 5:

- the manager may grant this process's still-busy CPUs to another client;
- it may tear down the XDP program and ntuple rules while this process's XSK sockets are still bound;
- it may reset `tx_maxrate` on a queue still in use.

K1 K-REQ-2 put manager release third because "device teardown can be cut short", but releasing a lease before its
user stops is the bug K2 §5.1 found in `mt_sch_mrg_uinit` [verified: K2 §5.1]. The manager socket is fd-anchored,
so it can simply close at exit, or last.

**RK-5 (handles after implicit close).** Several sources disagree:

- 16 §2.1: a session the app did not close returns `-MTL_ESHUTDOWN` on data calls, and a later close returns 0.
- 03 §2.2: object tables are per instance, "never freed before instance teardown" [verified: 03:105-110].
- 03 §6.1 item 4: a retired session's calls return `-MTL_EBADF` [verified: 03:608-611].
- The handle layout (`type|reserved|index|generation`) carries no instance ID, so a lookup cannot find a per-instance table at all once more than one instance can exist.

After outcome 0 every table is freed, so the promised "returns 0" reads freed memory. The same applies to
`mtl_mem_close` (mtl_mem.h:51-54), `mtl_queue_close`, `mtl_timeline_close` and outstanding leases ("release
returns 0 or `-MTL_ESTALE`, never a crash"). The fix is process-global, grow-only tables with a per-slot state
`CLOSED_BY_INSTANCE`, which also gives RK-7 its never-freed slot.

**RK-6 (in-flight callers).** Step 0 makes new calls return `-MTL_ESHUTDOWN`, but a thread already inside
`mtl_tx_submit` (DPC: conversion of a UHD frame takes ms) or a WT call between its wake and its return still
dereferences the session. 03 §6.2 has "wait for DP callers to leave (in-flight counter, §2.3)" [verified:
03:626]. 16 omits it, and with `MTL_SESSION_SINGLE_READER` the counter is elided (03:144-149 [verified]). That
elision rests on "never concurrent with destroy", which an instance shutdown from another thread breaks. Either
forbid shutdown while elided sessions have a caller (the app must join its workers first, as ex11 does), or keep
the counter.

**RK-7 (abort during shutdown).** ex11 sends the second signal to `mtl_instance_abort(g_mt)`. A process-directed
signal can run on any thread, concurrently with step 6. `mtl_instance_abort` is "atomics and one eventfd write"
(16 §2.5). If the instance struct or the waker eventfd was already freed or closed, the handler dereferences
freed memory, or `write()`s 8 bytes into whatever reused the fd number. ex11 itself opens `/dev/termination-log`
right after shutdown, the likeliest recipient. Fix:

- keep the flags and the eventfd number in the never-freed slot (RK-5);
- the closer sets `closed`, waits for an `as_inflight` counter to reach 0, then closes;
- the AS path increments, checks `closed`, writes, decrements.

**RK-12, RK-13 (outcomes).** mtl.h:458-459 says leased memory "is freed when they are returned, or at exit".
mtl_observe.h:259 says `bytes_kept_until_exit`: "never freed". 16 §2.4 says no library thread outlives the call,
and 03 §6.2 says a DP release cannot free and posts the retire to a library worker [verified: 03:643-644]. Those
cannot all hold. Also, "quiesced" = no device access **and** no library thread in app code; a log-sink thread
blocked on a full stdout pipe makes `threads_unjoined = 1` with every port stopped. No outcome describes that.

**RK-14 (self-join).** mtl_queue.h:134-138 lists the calls a dispatch `fn` may not make; instance close and
shutdown are not among them [verified]. mtl_observe.h:263-264 says shutdown is "any thread but a library thread",
and mtl.h close says nothing. A PORT_REMOVED handler that shuts down from the callback deadlocks until the
deadline and then reports itself unjoined.

**RK-15 (lost results).** Step 1 creates FLUSHED results; step 3 joins the dispatch thread; mtl.h close discards
unread results. 03 §6.1 item 3 guarantees every accepted submission a terminal outcome "in a shared CQ if one is
bound" [verified: 03:606-607]. A framework that releases a GstBuffer or AVBuffer on its result never sees it.
Its own pool deactivation then waits outside MTL, so the process does not reach `exit()` before SIGKILL.

**RK-16 (re-open).** 03 §7.1 keeps EAL alive across the last release so the next open works [verified:
03:721-723]. 16 adds three outcomes but does not say what a later `mtl_instance_open` in the same process gets
after each. A GStreamer app that cycles NULL→PLAYING closes and re-opens the SHARED instance. EAL arguments
(`-l`, `-a`, `--in-memory`) are fixed at first init [verified: `dev/mt_dev.c:330-450`], so a re-open naming other
lcores or ports cannot be honoured without hotplug.

**RK-9 (fork).** `O_CLOEXEC` closes on `exec`, not on `fork`. OFD locks belong to the open file description,
which the child shares. So a parent SIGKILLed while a non-exec child lives keeps its CPU locks and its manager
grants (no EOF). The child also holds the VFIO group fd, which blocks the next container's open, though in a
container restart the child dies too [inferred]. A `pthread_atfork` child handler that closes the fds MTL tracks
(manager socket, lock fds, eventfds, and the VFIO fds DPDK exposes) fixes it. Closing a dup in the child does not
touch the parent's device.

**Already right.**

- `u.hold` across sessions: holds go with the flushed units in step 1.
- Quarantine: "nothing the queue can reach" covers other sessions' pools and imported regions. Unmapping a region
  from the IOMMU under a quarantined queue only produces DMAR faults, and the memory is not corrupted [inferred].
- Attached pools across sessions follow mtl_mem.h:112-115.
- Timelines are process-local.

## 4. Q3 — Kubernetes facts

| Claim in 16 | Verdict |
|---|---|
| Grace default 30 s, preStop inside it, one 2 s extension | **correct** [verified: kubernetes/site `pod-lifecycle.md`, fetched 2026-10-01] |
| PID 1 ignores SIGTERM without a handler | **correct** [inferred: pid_namespaces(7); K1 §1.6]. Note: a blocked signal stays pending for init, an ignored-by-default one is dropped (RK-33) [inferred: kernel `sig_ignored`] |
| CPU manager static policy, Guaranteed + integer CPUs, cpuset = affinity | **correct** [verified via K1 S12]; `DisableCPUQuotaWithExclusiveCPUs` exists, Beta, default on since 1.33 [verified: feature-gates page], hence RK-25 |
| hugetlb cgroup `hugetlb.<size>.max` | **correct for cgroup v2** (sizes spelled `2MB`, `1GB`) [inferred]; v1 is `hugetlb.<size>.limit_in_bytes` |
| Downward API exposes one annotation as a file or env | **correct**, `metadata.annotations['<KEY>']` [verified: kubernetes.io downward-api page]; a key with `/` and `.` inside the quotes is fine [inferred] |
| `PCIDEVICE_<resource>` | **correct**: `PCIDEVICE_` + prefix + `_` + name, `.`→`_`, upper case, comma-separated IDs, plus `_INFO` JSON [verified: sriov-network-device-plugin `pkg/resources/pool_stub.go` GetEnvs]. So `intel.com/e810_media` → `PCIDEVICE_INTEL_COM_E810_MEDIA` |
| `IPC_LOCK` outside Baseline PSS | **correct** [verified via K1 S14 list] |
| Non-root + `add: [IPC_LOCK]` works | **wrong**: added capabilities land in bounding/inheritable only for UID ≠ 0; CapEff is 0 [verified: kubernetes/kubernetes#56374, open, frozen]. With `allowPrivilegeEscalation: false` (no_new_privs) file capabilities do not help either [inferred: prctl(2)] |
| Non-root can open the plugin's `/dev/vfio/N` | **not by default**: device nodes keep host ownership unless the runtime sets `device_ownership_from_security_context = true` and the pod sets `runAsUser`/`runAsGroup` [inferred; tracking issue kubernetes/kubernetes#92211 named on the k8s blog 2021-11-09, verified title only] |
| Seccomp RuntimeDefault suffices | **mostly**: containerd's default allows `get_mempolicy`, `mbind`, `set_mempolicy` only with `CAP_SYS_NICE`; `move_pages` never [verified: containerd `contrib/seccomp/seccomp_default.go`]. DPDK then sees NUMA as unavailable (`eal_memalloc.c:142-148`, `:616-630` [verified: DPDK 26.07 on host]) (RK-36) |
| readOnlyRootFilesystem with `--in-memory` | **plausible**: in-memory uses `memfd_create(MFD_HUGETLB)` (`eal_memalloc.c:224-247` [verified]); the runtime directory attempt is ignored in no-shconf mode per K2 §1.1 [not re-read]. `--file-prefix` is the fixed `MT_DPDK` today, not per instance (`dev/mt_dev.c:337-340` [verified]) |
| `terminationMessagePath` default `/dev/termination-log` | **correct**; 4096 B per container, 12 KiB per pod; `FallbackToLogsOnError` exists [verified: `determine-reason-pod-failure.md`]. Recommend that policy for crashes that write nothing |
| network-status readable at start | **unverified race** (RK-31) [inferred] |

Pod YAML (16 §10.4), structure: valid. Flow mappings, the `intel.com/e810_media` key, requests equal to limits for
cpu, memory, hugepages and the extended resource, and the `fieldRef.fieldPath` quoting are all fine. Semantics:

- No `runAsUser`, so `runAsNonRoot` needs a numeric `USER` in the image, or the kubelet refuses to start it
  [inferred].
- RK-1, RK-2 and RK-3 as above.
- `MTL_PORTS` with `env:` names gives no IP to a DPDK port, while the volume for the helper is mounted too. Show
  one path.
- `GRACE_SECONDS` must duplicate `terminationGracePeriodSeconds`, which the downward API does not expose
  [inferred]. Say so.

## 5. Q4 — health

- **Split.** Right in principle (K3 P-8). Wrong for `PORT_REMOVED` with 2022-7 (RK-10) and for `WORKER_STALLED`
  under a PF reset (RK-11). `DEVICE_FAULT` as liveness is right: quarantined memory never comes back.
- **TASKLET_SLEEP.** The sleep is bounded by `sch_default_sleep_us` = 1 ms (`mt_main.c:487` [verified]), but
  `sch_force_sleep_us` can be set larger (`mt_sch.c:66-73` [verified]). Open should reject
  `stall_ns ≤ 2 × max sleep`.
- **Lcore vs thread mode.** The heartbeat works in both. In thread mode on shared CPUs a 100 ms preemption is
  normal, so there it needs the RK-24 thresholds.
- **Undetected.** A TX session that stops progressing while its scheduler loops (a stalled RL queue in RUNNING)
  sets no bit until it goes ERROR. An app thread wedged in a CP call holding the instance mutex is seen only if
  the admin worker needs that mutex. Both are acceptable if documented; `session.last_progress_tai_ns` is the
  exporter signal.
- **Lock-free.** All counts are atomics. `detail[96]` needs a seqlock (RK-37). DP with a 96-byte copy is fine.
- **Probes.** A third endpoint is missing (RK-2). The library should also expose the housekeeping CPU set (stat
  `instance.housekeeping_cpus`), so the app can pin its probe server off the busy-poll CPUs. Otherwise, with
  `cpu-load-balancing.crio.io: disable`, the probe thread may share a CPU with a scheduler and miss the 1 s
  default `timeoutSeconds` [inferred].

## 6. Q5 — library claims at HEAD (`545a266a`)

| Claim (K2, relied on by 16) | Result |
|---|---|
| `kill(pid, 0)` decides lcore-entry liveness | holds: `mt_sch.c:692`, `:1325` [verified] |
| Lockfile opened `O_CREAT`, read-only fallback, no `O_CLOEXEC` | holds: `mt_sch.c:506-509` [verified] |
| `--file-prefix MT_DPDK --match-allocations --in-memory` | holds: `dev/mt_dev.c:337-345` [verified] |
| `main_lcore` 0 injected into `-l` when `lcores` is set | holds: `snprintf("%u,%s", p->main_lcore, p->lcores)` at `dev/mt_dev.c:434`, `--main-lcore` only when non-zero `:419-426` [verified] |
| `--remap-lcore-ids` on DPDK ≥ 25.11 | holds: `dev/mt_dev.c:443-445` [verified] |
| phc2sys steers `CLOCK_REALTIME` and never restores | holds: `clock_adjtime(CLOCK_REALTIME)` with `ADJ_SETOFFSET` `mt_ptp.c:175-183`, `ADJ_FREQUENCY`/`ADJ_TICK` `:214-224`; only `phc2sys_init` (`:1203`), no restore [verified] |
| `mt_mcast_uinit` leaves no groups | holds: clears lists and cancels the alarm, no `MCAST_LEAVE` (`mt_mcast.c:526-559`); leaves only in the per-group path `:735-743` [verified] |
| `sch_stop` waits forever | holds: `while (stopped == 0) mt_sleep_ms(10)` under `sch_lock` (`mt_sch.c:316-319`) [verified] |
| Unregister times out at 1 s with `-EIO` | holds: `mt_sch.c:886-899` [verified] |
| Native AF_XDP needs MtlManager | holds: `dev/mt_af_xdp.c:730-733` [verified]. So K3 F-6 (manager-less libxdp load) is unreachable at HEAD: K2 and K3 disagree, K2 is right (RK-29) |
| DPDK fds and SIGBUS | VFIO group/container `open(…, O_RDWR)` without CLOEXEC (`eal_vfio.c:367`, `:379`, `:1297`); `memfd_create` without `MFD_CLOEXEC` (`eal_memalloc.c:224-247`); `sigaction(SIGBUS)` swap in `huge_register_sigbus` (`:100-121`) [verified: DPDK 26.07 on host] |

## 7. Q6 — simplicity

- **close vs shutdown.** Two entry points with "the same steps and return values". Keep `mtl_instance_close` in the
  core. Replace `mtl_instance_shutdown` with a thread-local `mtl_last_shutdown_report(&r, size)` in
  `mtl_observe.h`, the pattern `mtl_last_error` already uses, and make DRAIN an option (`instance.shutdown_drain`).
  `ALL_REFERENCES` is the one flag with no other home. Keep it only with RK-22 per-reference handles, or as
  `mtl_instance_close_all(mt, timeout)` for the `main()` owner.
- **`MTL_INSTANCE_PREFLIGHT`.** Low value in a pod (RK-32) and a core-header flag. Move it to a tool.
- **`MTL_INSTANCE_MANAGER_OPTIONAL`.** Redundant with `instance.cpu_arbitration` auto, and contradicts 16 (RK-28).
  Remove it.
- **CPU knobs.** `instance.sys_lcore`, `instance.main_lcore` and `instance.housekeeping_cpus` describe one thing
  (RK-26). Keep one.
- **`instance.cpu_shared = SLEEP`.** Duplicates `MTL_INSTANCE_TASKLET_SLEEP`. Keep WARN and REFUSE.
- **Health.** `STARTING` duplicates `phase < READY`, and `MTL_PHASE_SHUTDOWN` duplicates `SHUTTING_DOWN`. Keep the
  bits, which is what probes read.
- **Two port-discovery paths.** `env:` plus the network-status helper. Keep both, but document `env:` with `=ip`,
  and the helper as the 2022-7 path.

What a deployer still needs:

- the reset budget as a number and a stat (RK-17);
- a startup endpoint recipe (RK-2);
- the non-root recipe (RK-1);
- the housekeeping CPU set to pin app threads on;
- writing `mtl_last_error().detail` to the termination log when open fails (K-REQ-6, not in ex11);
- Recreate for senders (RK-35);
- `terminationMessagePolicy: FallbackToLogsOnError`.

## 8. Suggested order of work

1. Fix the pod recipe (RK-1, RK-2, RK-3) before anyone copies it.
2. Rewrite 16 §2.2 as: admission off → drain in-flight callers → TX finish → leaves → deliver results and join
   dispatch → stop schedulers and queues (04 §4.5, budget-checked) → close the manager socket → free. Settle the
   outcome table (RK-4, RK-6, RK-12, RK-13, RK-15, RK-17).
3. Make handle tables process-global, with AS-safe slots (RK-5, RK-7, RK-22); update 03 §2.2, §6.1, §7.1 in the same
   change.
4. Correct R8 (RK-8, RK-9) and its engine item E-15.
5. Health thresholds (RK-10, RK-11, RK-24, RK-25).
