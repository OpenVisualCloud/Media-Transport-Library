# K3 — Safe shutdown, teardown order and crash cleanup: prior art for MTL in a Kubernetes pod

| | |
|---|---|
| Status | Research for the revision-4 design, 2026-10-01. Nothing here is implemented or decided; the design that uses it is [16](../16-kubernetes-and-crash-safety.md) |
| Question | The owner asked for an API that "can safely work in a kubernetes pod", whether "how we destroy the sessions is good enough", and whether "we have something that will provide safe closing and destroying the objects" |
| Baseline | [REVISION-4.md](../REVISION-4.md); in [sketch/include/mtl/experimental/](../../sketch/include/mtl/experimental/): `mtl.h` (R1–R7, session and instance close, interrupt, abort), `mtl_mem.h` (`mtl_mem_close`), `mtl_queue.h`; [03 §6–§7](../03-object-model-and-lifecycle.md), [04 §4.5](../04-threading-and-execution.md) |
| Method | Read the R4 headers and design text; read MTL code at `545a266a`; read kernel, DPDK, SPDK, libxdp and rdma-core sources and docs (web access was available). **[verified]** = I read the code or document cited; **[inferred]** = from my own knowledge or reasoning, not checked against a source in this study |
| Pronouns | people are "they/them" |

## 0. The answer in one page

What R4 has is enough for the **in-process** part of teardown. `mtl_session_close(s, timeout)` drains, then flushes, then
retires, and returns 1 while leases or device references remain. The instance outlives its sessions (03 §7.1). Region
close also returns 0 or 1. Interrupts and abort are async-signal-safe. Those rules match the best prior art: Vulkan
child-before-parent, libfabric `-FI_EBUSY`, io_uring deferred unpin, DOCA's stopping state.

The gaps are what the pod changes. In Kubernetes **SIGKILL is a normal ending, not a bug**: the kubelet sends it once
the grace period runs out. Only the kernel runs then. A design is "safe in a pod" when every resource that outlives the
process is either released by the kernel (it is tied to a file descriptor) or reconciled by the next process that
starts. Four of today's mechanisms fail that test, and R4 lacks three pieces:

| # | Gap | Kind |
|---|---|---|
| G1 | No instance-level "shut down by this deadline and tell me what is left". `mtl_instance_close` only drops a reference, so one leaked lease keeps the instance alive until `exit()` | API (R4) |
| G2 | No device-removal contract. MTL registers no ethdev event callbacks at all, so a removed or reset VF is never seen | API (R4) + code |
| G3 | No liveness signal for the busy-poll threads that a liveness probe could read | API (R4) |
| G4 | Lcore arbitration without MtlManager uses SysV shm plus `kill(pid, 0)` and a hostname match. In pods it either arbitrates nothing (separate IPC namespaces) or frees a live pod's lcores (`hostIPC` + `hostNetwork`) | code |
| G5 | XDP programs are attached through netlink by MtlManager, and by libxdp in manager-less AF_XDP. They outlive a crash, and the libxdp refcount leaks | code |
| G6 | IGMP membership on the DPDK path is never left after SIGKILL; the switch keeps flooding the port until the querier ages it out | inherent; mitigate |
| G7 | No fork rule. A forked child holding the parent's DMA memory is undefined | API (R4) |

Patterns P-1…P-12 (§3) close them. The contract table is §4, and the small API deltas they imply are §5.

## 1. What MTL does today and what R4 already says

### 1.1 Findings in the code (`545a266a`)

| ID | Finding | Evidence |
|---|---|---|
| F-1 | The library registers **no** `rte_eth_dev_callback_register` for any event: no `INTR_RMV`, `INTR_RESET`, `ERR_RECOVERING` or `RECOVERY_*`. The only hotplug call is `rte_eal_hotplug_add` for vdevs | grep of `lib/src` **[verified]** (`dev/mt_dev.c:1558`) |
| F-2 | Manager-less lcore arbitration: one `flock` on `/tmp/kahawai_lcore.lock` serialises a SysV shm table (`ftok("/dev/null", 21)`). An entry is "dead" when `kill(pid, 0) != 0` and its hostname and user match the caller's | `mt_sch.c:505-605`, `:686-698`, `:1310-1330`; `mt_platform.h:57` **[verified]** |
| F-3 | Consequences of F-2 in pods. SysV IPC is per IPC namespace: pods without `hostIPC` each get their own table, so nothing is arbitrated. With `hostIPC` + `hostNetwork` (same hostname, both root), `kill()` on another PID namespace's PID gives ESRCH, so a live pod's lcores are freed. `EPERM` (alive) also counts as dead; PIDs are reused | **[inferred]** from the code |
| F-4 | MtlManager releases a client's lcores, queues and flows in `~mtl_instance()` when the client's socket reads EOF. This is crash-robust and works in any namespace. But identity is the PID and UID the client writes in its register message, not `SO_PEERCRED` (also SF-47 in 15 §7) | `manager/mtl_manager.cpp:186-198`, `manager/mtl_instance.hpp:66-89`, `:191-194` **[verified]** |
| F-5 | `mtl_interface::load_xdp` sets `xdp_mode = XDP_MODE_SKB` after the SKB fallback, then unconditionally `xdp_mode = XDP_MODE_NATIVE`. So `unload_xdp` detaches an SKB-mode program with the wrong mode, and the program is likely left attached | `manager/mtl_interface.hpp:428-430`, `:450-452` **[verified]** code; the effect is **[inferred]** |
| F-6 | Manager-less AF_XDP lets libxdp load its default xsk program (`INHIBIT_PROG_LOAD` only when `has_ctrl`). libxdp keeps that program's refcount in a BPF map and detaches only when the count reaches 0, so after a SIGKILL the count never returns to 0 and the program stays | `lib/src/dev/mt_af_xdp.c:417-419`; libxdp `xsk.c` `xsk_release_xdp_prog` **[verified]** |
| F-7 | EAL runs `--in-memory` (anonymous hugepages, no `rtemap_*` files) with `--file-prefix` | `dev/mt_dev.c:337-345` **[verified]** |
| F-8 | Legacy `mtl_abort` only sets `instance_aborted`. `mtl_uninit` calls `rte_eal_cleanup()`, so EAL cannot be initialised again in that process | `mt_main.c:747-757`; research/13 §1 **[verified]** |

### 1.2 What R4 specifies

- `mtl_session_close(s, timeout)` stops with DRAIN until the deadline, then FLUSH, destroys the session and waits for it
  to retire. It always consumes `s`. It returns 0 when retired, or 1 when leases or device-held units remain;
  `MTL_WAIT_RETIRED` and `SESSION_RETIRED` report the end.
- Destroy sequence (03 §6.2): a CAS moves the session to DESTROYING, then flush, scheduler detach with ack, in-flight
  DP callers drain out, and device references go (bounded `tx_done_cleanup`, then queue stop/start, then PORT_RESET
  escalation, 04 §4.5). Only then does retire run.
- `mtl_instance_close` drops a reference. The instance lives until its last session retires. Process exit without
  close is supported, with no `atexit` handler (03 §7.1).
- `mtl_instance_interrupt(mt, 1)` (AS) makes data waits return `-MTL_ECANCELED`. `mtl_instance_abort` (AS) stops TX
  at the next packet, flushes queued units and still requires close.
- `mtl_mem_close` returns 0 when retired and 1 while referenced; `REGION_RELEASED` reports the end. The application
  must not unmap the memory before 0 (15 §2).
- `mtl_queue_close` unbinds sessions and retires the queue.

Missing for a pod: G1–G3 and G7 in §0.

## 2. Prior art

### 2.1 RDMA verbs (libibverbs, kernel uverbs)

- **The kernel cleans up on fd close, in dependency rounds.** `ib_uverbs_close()` calls
  `uverbs_destroy_ufile_hw(file, RDMA_REMOVE_CLOSE)`. That loops `while (!list_empty(&ufile->uobjects) &&
  !__uverbs_cleanup_ufile(...))`: each pass destroys every object it can, and an object that is still busy gets another
  pass. A final pass with `RDMA_REMOVE_DRIVER_FAILURE` forces the rest. `enum rdma_remove_reason` separates DESTROY
  (user, "could fail"), CLOSE (context deletion, "should delete the actual object"), DRIVER_REMOVE (hot-unplug), ABORT
  and DRIVER_FAILURE. Sources: `drivers/infiniband/core/uverbs_main.c:990-1027`, `rdma_core.c:927-967`,
  `include/rdma/ib_verbs.h:1529-1543` **[verified]**. Destroying an MR releases its `ib_umem`, which unpins the pages
  **[inferred]**.
- **The user-side destroy order is child before parent, enforced with EBUSY.** A uobject whose `usecnt` is not 0
  fails with `-EBUSY` (`rdma_core.c:111-114` **[verified]**). `ibv_dealloc_pd` fails while QPs or MRs still use the
  PD **[inferred]**.
- **Device removal is a disassociation, not a wait.** `ib_uverbs_remove_one()`: "We disassociate HW resources and
  immediately return. Userspace will see a EIO errno for all future access … active clients can still issue commands
  and close their open files." Without driver support for disassociation, removal **waits** for every client to close
  (`wait_for_completion`). User mmaps are zapped, and later faults get "a dummy writable zero page"
  (`uverbs_main.c:1256-1283`, `:695-730`, `:954-969` **[verified]**).
- **Async events gate destroy.** "destroying an object (CQ, SRQ or QP) will wait for all affiliated events for the
  object to be acknowledged". `IBV_EVENT_DEVICE_FATAL` means "CA is in FATAL state" (ibv_get_async_event(3)
  **[verified]**).
- **Fork safety.** `ibv_fork_init()` uses `MADV_DONTFORK` on registered memory; it fails with EINVAL if memory was
  already registered; `RDMAV_FORK_SAFE` does the same (ibv_fork_init(3) **[verified]**). Kernels since about 5.12 copy
  pinned anonymous pages early at fork, so the parent keeps its DMA pages without `DONTFORK` **[inferred]**.

### 2.2 DPDK

- `rte_eal_cleanup()`: "must be called to release any internal resources … After this call, no DPDK function calls may
  be made … just before terminating the process" (`rte_eal.h`, 26.07 **[verified]**). `rte_eth_dev_close()`: "Close a
  stopped Ethernet device. The device cannot be restarted!" `rte_eth_dev_reset()` stops the port and reruns the PMD
  uninit and init, and may return `-EAGAIN` "if the reset temporarily failed" (`rte_ethdev.h` **[verified]**).
- **Crash leftovers.** With hugetlbfs files, "backing files may persist after the application terminates in case of a
  crash"; "EAL ensures this behavior by removing existing backing files at startup". `--in-memory` "is free of filename
  conflict and leftover file issues" and does not allow multi-process (EAL guide **[verified]**). MTL uses
  `--in-memory` (F-7).
- **Multi-process.** Nothing is said about primary death. "Running a secondary process in a different container
  namespace from the primary is not supported"; "There is no privilege separation" (multi-process guide **[verified]**).
  R4 is right to keep NG5: one process, no secondaries.
- **Device removal.** `RTE_ETH_EVENT_INTR_RMV` is the "device removal event". Per the EAL guide, the PMD keeps
  callbacks safe after the "PCI mappings [are] unmapped"; the callback runs on the interrupt thread, and "Care must be
  taken not to close the device from the interrupt handler context … reschedule such closing" **[verified]**.
  `INTR_RESET` asks for `rte_eth_dev_reset()`. `ERR_RECOVERING` then `RECOVERY_SUCCESS` or `RECOVERY_FAILED` is the
  proactive mode. The callback docs warn against taking locks in callbacks because of deadlocks (`rte_ethdev.h:4207-4292`
  **[verified]**). `rte_dev_event_monitor_start()` and `rte_dev_hotplug_handle_enable()` exist (`rte_dev.h:393`, `:413`
  **[verified]**). The latter installs a SIGBUS handler that remaps a removed BAR so a stray MMIO access does not kill
  the process **[inferred]**.
- **Port ownership.** `rte_eth_dev_owner_new/set` marks which component owns a port inside one process
  (`rte_ethdev.h:2280-2292` **[verified]**). It is in-process only.

### 2.3 VFIO

- **On device-fd release** (`vfio_pci_core_close_device` → `vfio_pci_core_disable`, Linux master **[verified]**), in
  this order:
  1. back to D0;
  2. `pci_clear_master()` ("Stop the device from further DMA");
  3. all IRQs off;
  4. ioeventfds and BAR maps released;
  5. `needs_reset = true`;
  6. PCI_COMMAND written with INTx disabled, which also clears bus master and decode;
  7. `__pci_reset_function_locked()` (FLR or an equivalent) if `reset_works`;
  8. config space restored;
  9. a bus or slot reset if the device is still dirty and no other device in the set is open.
- **On open**, `vfio_pci_core_enable` calls `pci_try_reset_function()` **[verified]**. So a restarted container starts
  from a reset VF even after a SIGKILL.
- **IOMMU unmap order.** Type1 and iommufd pin pages with page references. Container and IOAS teardown unmaps and
  unpins them after the device is detached, because the device holds a reference on its group and container, and DMA
  is already off by then. So pinned pages are never reused while the device can still reach them **[inferred]**; the
  VFIO driver-API doc covers only the `dma_unmap` callback **[verified]**.
- **No-IOMMU mode** is "intended for usages where unsafe DMA can be performed by userspace drivers … w/o physical IOMMU
  protection" (vfio.rst **[verified]**). It taints the kernel and needs CAP_SYS_RAWIO **[inferred]**. **No crash
  guarantee holds**. In `do_exit`, `exit_mm` runs before `exit_files`, so anonymous hugepages (`--in-memory`) go back to
  the pool while the VF can still DMA until its fd closes **[inferred, needs a spike]**.
- **Hot-unplug request.** The kernel asks the user to release the device through the request callback ("when device is
  going to be unregistered", vfio.rst **[verified]**), which is the `VFIO_PCI_REQ_IRQ_INDEX` eventfd **[inferred]**.

### 2.4 Other user-space device libraries

| Library | Teardown contract | Source |
|---|---|---|
| SPDK | `spdk_env_fini()`: "Release any resources … After this call, no SPDK env function calls may be made", either before exit or before re-initialising. `spdk_nvme_detach_async()` + `spdk_nvme_detach_poll_async()` is a detach you poll; "called from a single thread while no other threads are actively using the NVMe device" | `include/spdk/env.h:163-170`, `nvme.h:1172-1210` **[verified]** |
| Rivermax | `rmx_output_media_cancel_unsent_chunks()`, then retry `rmx_output_media_destroy_stream()` while it returns `RMX_BUSY` (chunks in flight). Memory registrations outlive streams | research/10 §2.8, §3 **[verified]** there |
| DOCA | `doca_ctx_stop()` enters a **stopping** state: "all in-flight tasks are guaranteed to fail" (flushed). The app must "progress all in-flight tasks until completion before destroying"; a state-changed callback reports stopping → idle. "All contexts must be destroyed before the PE"; stop then destroy for mmap and inventory | DOCA Core guide **[verified]** |
| libfabric | `fi_close(ep)`: "Outstanding operations … will be discarded … with no completions reported"; buffers stay unusable until a completion or until close returns. Scalable EP with open contexts → `-FI_EBUSY`; MRs bound to an EP close first | fi_endpoint(3) **[verified]**; research/09 **[verified]** |
| CUDA | `cudaDeviceReset()` destroys every allocation and context state of the process at once; the caller must make sure no other thread uses the device. Calling CUDA from `atexit` or static destructors fails once the runtime is unloading | **[inferred]** |
| Vulkan | Child objects must be destroyed before `vkDestroyDevice`; `vkDeviceWaitIdle` first. After `VK_ERROR_DEVICE_LOST`, commands that wait return in finite time, objects must still be destroyed, and destroy always succeeds | **[inferred]**; the spec chapter fetched was truncated |
| io_uring | "need not unregister buffers explicitly before shutting down … shutdown processing may run asynchronously … not guaranteed that pages are immediately unpinned". Resource tags: "after the resource had been unregistered and it's not used anymore, a CQE will be posted" | io_uring_register(2) **[verified]** |
| AF_XDP / libxdp | `xsk_umem__delete` is `-EBUSY` while sockets use it; the default program's refcount is a BPF map value, detach at 0. Attach is netlink (`bpf_xdp_attach`), not an XDP `bpf_link`; components are freplace links **pinned in bpffs** (`dispatch-IFINDEX-DID`), dir locked by `flock`. Stale pins: `libxdp_clean_references()`, on request only | libxdp `xsk.c`, `libxdp.c` **[verified]** |
| AF_XDP kernel | The UMEM stays pinned until the last socket and the umem's fd go, with deferred release for the driver. A netlink-attached XDP program **survives process exit**; an XDP `bpf_link` is detached when its last fd closes, unless pinned | **[inferred]** |

### 2.5 Frameworks, runtimes and orchestrators

- **GStreamer.** State changes go down one step at a time: PLAYING → PAUSED → READY → NULL. `GstBaseSrc::unlock` /
  `unlock_stop` release a blocked `create` before `stop`, and a FLUSH_START/STOP pair discards data in flight. A
  deactivated `GstBufferPool` frees each outstanding buffer when it comes back. This is R4's sticky interrupt and its
  deferred retire **[inferred]**; 04 §5.4 maps it.
- **PipeWire.** The daemon detects client death by socket close and reclaims its memfd-shared buffers.
  `pw_stream_disconnect` comes before destroy **[inferred]**.
- **gRPC and Envoy.** Go `Server.GracefulStop()` stops accepting and waits for pending RPCs; the usual idiom races it
  against a timer that calls `Stop()`, which cancels everything. Envoy drains listeners (`--drain-time-s`, admin
  `/drain_listeners?graceful`) before exiting **[inferred]**. Two stages, the second one bounded.
- **Java and Go.** `Runtime.addShutdownHook` runs on normal exit and on SIGTERM, SIGINT and SIGHUP, never on SIGKILL;
  hooks "should … finish their work quickly". Go has no hooks: `signal.NotifyContext` cancels a context, and `os.Exit`
  skips defers **[inferred]**. Cleanup in a hook is best-effort, never the guarantee.
- **systemd.** On stop it sends SIGTERM, waits `TimeoutStopSec` (default 90 s), then sends SIGKILL to the whole cgroup.
  `sd_notify("EXTEND_TIMEOUT_USEC=…")` extends the deadline while work progresses; `WatchdogSec` +
  `sd_notify("WATCHDOG=1")` is a liveness heartbeat **[inferred]**.
- **Kubernetes.** On deletion the endpoints are marked terminating, the `preStop` hook runs, then the stop signal
  (SIGTERM by default) goes to PID 1 of each container. At `terminationGracePeriodSeconds` (default 30 s, including
  `preStop`) SIGKILL follows; a `preStop` that overruns gets one 2-second extension. Sidecars stop after the main
  containers, in reverse order **[inferred]**; the fetched page was truncated before that section.
  - PID 1 ignores signals it has no handler for, so an app run as PID 1 without `tini` or a handler never sees SIGTERM.
  - A container restart (not a pod restart) keeps the pod's VF, `emptyDir` volumes and hugepage volumes.
  - The SR-IOV device plugin hands the same VF to the next pod with no deallocate hook.
  - The CPU manager's static policy gives Guaranteed pods exclusive CPUs through the cpuset.
  - All four bullets above are **[inferred]**.

### 2.6 "Quiesce" or "shutdown with a deadline" distinct from per-object destroy (question 4b)

| API | Shape | Partial success reported as |
|---|---|---|
| `vkDeviceWaitIdle` + per-object destroy | quiesce the device, then destroy children, then the parent | `VK_ERROR_DEVICE_LOST` from the wait; destroy cannot fail **[inferred]** |
| DOCA `doca_ctx_stop` | async: stopping → idle, progressed by the app | `DOCA_ERROR_IN_PROGRESS` / the stopping state; the state-changed callback marks the end **[verified]** state model |
| SPDK `spdk_nvme_detach_async` / `_poll_async` | a detach you poll | `-EAGAIN` from poll until done **[verified]** prototypes, the `-EAGAIN` value **[inferred]** |
| Rivermax `destroy_stream` | retried | `RMX_BUSY` while chunks are in flight **[verified]** (research/10) |
| io_uring ring close | close returns; the unpin is asynchronous | tag CQEs per resource; nothing after close **[verified]** |
| gRPC `GracefulStop` / `Stop` | drain, then hard stop at a deadline | no report; the app counts what it cancelled **[inferred]** |
| systemd stop | SIGTERM, deadline, SIGKILL | the unit result `timeout` vs `success` **[inferred]** |
| uverbs on removal | disassociate now, release later | EIO on every later call; the fd stays open until the app closes it **[verified]** |
| `cudaDeviceReset` | instant teardown of all state | none **[inferred]** |

None of them reports "device quiesced, memory still referenced" as one value. They split it in two:

- the **device** stage ends in bounded time: wait idle, stop, or disassociate;
- **memory** release is deferred and notified per resource: io_uring tags, DOCA idle, R4's `REGION_RELEASED`.

MTL should do the same at instance level (P-3).

### 2.7 Liveness of busy-polling data planes

- **VPP.** The main thread raises a barrier and spins until `workers_at_barrier == count`. Past the deadline it prints
  "worker thread deadlock" and calls `os_panic()`; the timeout is configurable (`cpu { barrier-timeout }`)
  (`src/vlib/threads.c` **[verified]**). Workers check in by an atomic increment.
- **OVS.** RCU threads publish a seqno when they quiesce. `ovsrcu_synchronize` warns "blocked %u ms waiting for %s to
  quiesce" at 1 s and doubles the threshold after each warning (`lib/ovs-rcu.c` **[verified]**). `dpif-netdev/pmd-stats-show`
  reports idle and processing cycles per PMD **[inferred]**.
- **DPDK.** `rte_lcore_register_usage_cb` feeds busy and total cycles into `rte_lcore_dump` and the `/eal/lcore/info`
  telemetry endpoint (`rte_lcore.h:360-372` **[verified]**). Telemetry is read through a Unix socket, without locking
  the lcore **[inferred]**.
- **systemd and Kubernetes.** A watchdog heartbeat; liveness, readiness and startup probes **[inferred]**.

What they share: a **monotonic counter per polling thread**, written with a relaxed store, read by anyone without a
lock. A **supervisor** compares two readings against a deadline. The **escalation** goes warn → event → (VPP) panic.

### 2.8 Crash-robust cross-process arbitration

| Mechanism | Released on death by | Across PID ns | Across IPC ns | Across user ns / pods | Notes |
|---|---|---|---|---|---|
| `flock` | the kernel, at the last close of the open file description | yes | yes | yes, if the same inode is visible (bind-mounted host dir) and permissions allow | not reliable over NFS **[inferred]**; libxdp uses it on bpffs **[verified]** |
| OFD lock (`F_OFD_SETLK`) | the kernel, at the last close of the OFD | yes | yes | yes, as flock | byte-range: one file, one byte per CPU; `F_OFD_GETLK` reports `l_pid = -1`, so the owner is unknown **[inferred]** |
| POSIX `fcntl` lock | the kernel at exit, **or at any close of that file by the process** | yes; `l_pid` is translated, 0 when invisible **[inferred]** | yes | as flock | the close-any-fd trap makes it unsafe inside a library |
| Unix socket to a daemon (MtlManager) | the kernel closes it; the daemon sees EOF or HUP | yes | yes | yes if the socket file is bind-mounted | identity via `SO_PEERCRED` (PID translated into the reader's namespace) or `SO_PEERPIDFD` (6.5+) **[inferred]**; MtlManager does this today, minus the identity part (F-4) |
| Robust mutex in shared memory | the kernel walks the dying thread's robust list; the next locker gets `EOWNERDEAD` | death detection yes; the stored TID means nothing elsewhere; PI futexes break **[inferred]** | needs a shared file mapping, not SysV | needs shared memory | data state unknown after `EOWNERDEAD`; no `pthread_mutex_consistent` → `ENOTRECOVERABLE` (man page **[verified]**) |
| `pidfd` | the pidfd polls readable at exit; immune to PID reuse | needs the PID visible, or a pidfd passed over `SCM_RIGHTS` | — | yes when passed | **[inferred]** |
| `kill(pid, 0)` on a stored PID | nothing; a poll | **no** | — | **no** | PID reuse; EPERM ≠ dead (MTL today, F-2/F-3) |
| SysV shm + `shm_nattch` | the attach count drops at exit; contents stay | — | **no**: one segment per IPC ns | — | MTL today (F-2) |
| cgroup v2 `cgroup.events` (`populated 0`) | the kernel, when the cgroup empties | yes | yes | needs the host cgroupfs (a node agent) | how a DaemonSet learns a pod is fully gone **[inferred]** |
| kubelet CPU manager / device plugin | the kubelet, at pod deletion | n/a | n/a | node-wide | the cluster's own arbiter for CPUs and VFs **[inferred]** |

## 3. Patterns to adopt for MTL

### P-1 The process is the unit of cleanup; anchor everything external on a descriptor

- **Pattern.** Every resource that lives outside the process (device DMA, NIC queues, CPU leases, kernel programs,
  daemon-held state) is owned through a file descriptor. Then the kernel releases it on any death, SIGKILL included.
  Anything that cannot be tied to an fd is listed and reconciled at the next start (P-11).
- **From.** uverbs file close (§2.1); VFIO device release (§2.3); AF_XDP UMEM; OFD locks; MtlManager's socket-EOF
  cleanup (F-4).
- **MTL.**
  - Device: the VFIO device fd, so bus master is off, FLR runs and the IOMMU is unpinned.
  - Lcores and queues: the manager socket, or OFD lockfiles (P-6).
  - XDP: a `bpf_link` fd (P-7).
  - Wait handles: eventfds.
  - Imported regions: pinned through VFIO, released when the container closes.
  - Not fd-anchored: IGMP membership on the DPDK path, manager-installed ethtool ntuple rules and netlink XDP while
    the manager is down, PF-side VF settings (MAC, VLAN, trust; the CNI's job), and PHC adjustments by built-in PTP.
    §4 says what holds for each.
- **Pitfalls.** fds must be `O_CLOEXEC`, or an exec'd child keeps the device or lease alive. Any daemon-held state is
  only as robust as the daemon, so the manager must reconcile on its own restart. `SO_PEERCRED` must replace the
  self-reported PID, or one pod can release another's resources.

### P-2 Two-stage shutdown with one deadline: drain, then a bounded hard stop

- **Pattern.** One call takes an absolute deadline.
  - Stage 1: stop admitting work, drain what is queued, leave multicast groups.
  - Stage 2, at the deadline: flush everything and quiesce the device.
  - Stage 2 never waits on application code, application leases or the network.
- **From.** gRPC `GracefulStop` + `Stop`, Envoy drain, systemd `TimeoutStopSec`, the Kubernetes grace period (§2.5),
  DOCA stopping (§2.4).
- **MTL.** `mtl_instance_shutdown(mt, flags, timeout_ns, &report, size)` (§5.1).
  1. Every session gets `-MTL_ESHUTDOWN` on acquire, submit and dequeue.
  2. TX stops with DRAIN at a frame boundary.
  3. RX stops; IGMP leaves go out.
  4. At the deadline (or at once with `MTL_SHUTDOWN_ABORT`) the `mtl_instance_abort` path runs, then the
     device-reference steps of 04 §4.5.

  It is the instance-level counterpart of `mtl_session_close`, which keeps its own semantics. In a pod, `main()` calls
  it from the SIGTERM path, with `timeout = grace − margin`.
- **Pitfalls.**
  - The drain budget is small in media time. Four queued 59.94 Hz frames are 67 ms, but an RL queue stall can hold
    units until the 04 §4.5 reset. Size the margin for the reset, not for the drain.
  - A SHARED instance used by plugins (GStreamer, FFmpeg) must not be shut down by one element. Make the call legal
    only for a non-shared handle or with `MTL_SHUTDOWN_PROCESS`.
  - It must be idempotent and callable from any non-tasklet thread.
  - A second SIGTERM should map to `MTL_SHUTDOWN_ABORT`, not to a second drain.

### P-3 Quiesce the device first; report memory release separately

- **Pattern.** Shutdown has two outcomes reported apart.
  - *Device quiesced*: no NIC or DMA engine can read or write any application or library memory, and no library
    thread will call application code.
  - *Retired*: every object is freed.

  The first is bounded; the second may wait on the application, and its end is notified.
- **From.** `vkDeviceWaitIdle` then destroy; VFIO's bus-master-off before reset and unmap; io_uring's asynchronous
  unpin with tag CQEs; DOCA stopping → idle; uverbs disassociate (§2.1–§2.4, §2.6).
- **MTL.** Return values of `mtl_instance_shutdown`:
  - `0`: retired;
  - `1`: quiesced, still retiring (`report.leases_out`, `regions_referenced`, `sessions_retiring`);
  - `-MTL_ETIMEDOUT`: some port could not be quiesced (`report.ports_unquiesced`), so the caller should exit the
    process, letting the kernel's VFIO release finish the job.

  R4's per-object 0/1 stays. `REGION_RELEASED` and `SESSION_RETIRED` are the tags. Library threads that call
  application code (queue dispatch, log sink, plugin threads; R6) are joined in stage 2, so "quiesced" includes "no
  callbacks".
- **Pitfalls.** "Quiesced" must mean descriptor rings stopped (`rte_eth_dev_tx_queue_stop` or a port stop), not "no
  new submits". Whether iavf and ice release chained external mbufs on queue stop is still spike S8 (04 §4.5). If the
  queue cannot be stopped, report it rather than claim quiescence. A plugin thread stuck in a codec cannot be joined:
  report it as `plugins_unjoined`.

### P-4 Children before parents, enforced by references rather than errors, with a forced last round

- **Pattern.** Normal destroy follows dependency order, and the library makes a premature parent close safe instead
  of failing it: the parent lives until its children retire. Process-level teardown runs the kernel's "destroy in
  rounds until empty, then force" loop.
- **From.** Vulkan and libfabric (EBUSY, or undefined when the order is wrong); uverbs cleanup rounds with a
  `DRIVER_FAILURE` final pass (§2.1); GStreamer refcounts.
- **MTL.** R4 already picks references: instance ← session ← region/timeline/queue; a session over another's pool
  returns 1 until the attached sessions close. Add a forced round as stage 2 of P-2:
  - sessions are destroyed in reverse attach order;
  - leases still held become stale (`-MTL_ESTALE` on release, never a crash);
  - **library-pool memory under a stale lease is not freed**: it stays mapped until process exit, counted in
    `report.bytes_leaked_until_exit`. Freeing it would turn an application bug into a use-after-free.

  Imported regions are unmapped from the device (quiesced) but stay the application's, and `mtl_mem_close` then
  returns 0.
- **Pitfalls.** The forced round must not run while application threads are inside DP calls on those sessions. Use
  the in-flight counter of 03 §2.3, and give up at the deadline with the session counted in `sessions_retiring`.
  Unmapping an imported region from the IOMMU while the device is not quiesced is the one ordering that must never
  happen (§2.3).

### P-5 Device loss is a state with a contract, not a crash

- **Pattern.** When the device disappears or must be reset:
  - every later call fails fast with one code;
  - every wait returns in bounded time;
  - close and release always succeed and never touch the device;
  - memory the device could reach is released by the kernel's fd close, not by the library.
- **From.** uverbs disassociate ("Userspace will see a EIO errno for all future access", zero-page mmaps); Vulkan
  `VK_ERROR_DEVICE_LOST`; DPDK `INTR_RMV` and the rule not to close in the callback; VFIO request IRQ (§2.1–§2.3).
- **MTL.**
  - Register `RTE_ETH_EVENT_INTR_RMV`, `INTR_RESET`, `ERR_RECOVERING` and `RECOVERY_SUCCESS/FAILED` per port (F-1);
    the callbacks only post to the admin worker.
  - On RMV the port becomes REMOVED. Its legs go oper-down. A 2022-7 session with a surviving leg continues degraded
    (`LEG_DOWN`). A session with no leg left goes ERROR, reason `DEVICE_REMOVED`, and its waits return
    `-MTL_ENODEV`.
  - Close skips the 04 §4.5 device steps for a removed port.
  - On INTR_RESET, run `rte_eth_dev_reset` from the worker and restore the queues; sessions report `PORT_RESET` and
    resume.
  - Add `MTL_EVENT_PORT_REMOVED` under `MTL_SUB_PORT`.
- **Pitfalls.**
  - `rte_dev_hotplug_handle_enable` installs a process-wide SIGBUS handler. A library must not do that behind the
    application's back, so make it an instance option, default off.
  - A removed VF's mbufs (external buffers over application memory) must not run free callbacks that touch the
    device.
  - On a PF reset, ice resets every VF, so "reset" happens far more often in practice than "removed". Test both.

### P-6 Fd-anchored CPU and queue leases; never PIDs; defer to the kubelet where it already arbitrates

- **Pattern.** A cross-process lease is a lock or a connection the kernel drops on death. Ownership data next to it
  (PID, pod, time) is diagnostic only, never the liveness test.
- **From.** flock and OFD locks, libxdp's flock on bpffs, MtlManager's socket EOF, the kubelet CPU manager (§2.8).
- **MTL.**
  1. **In a Guaranteed pod with the static CPU policy**, the cpuset *is* the lease. The instance takes lcores only from
     `sched_getaffinity()` and skips cross-process arbitration (`MTL_INSTANCE_LCORES_FROM_AFFINITY`, a proposal).
  2. **With MtlManager** (a DaemonSet with its socket bind-mounted), keep socket-EOF release. Add `SO_PEERCRED` /
     `SO_PEERPIDFD` identity, reconciliation on manager restart (clients re-announce, as 07 §7 says), and cgroup
     `populated 0` as a second signal that a pod is gone.
  3. **Without a manager**, replace SysV shm + `kill(pid, 0)` (F-2, F-3) with one OFD lock per CPU on
     `/run/mtl/lcore.lock` (byte *n* = CPU *n*) in a host directory shared by the pods. The kernel releases the lock on
     death, in any PID, IPC or user namespace.
- **Pitfalls.** OFD and flock locks need the same inode: a bind mount of one host directory, not a per-pod `emptyDir`.
  Permissions across user namespaces need a shared GID. Never place the file on NFS. CPU numbers inside a pod are host
  CPU numbers, which is correct for locks. The SysV path must be removed, not kept as a fallback: it silently
  arbitrates nothing in pods.

### P-7 Kernel programs and rules: kernel-detached by default, or pinned on purpose and reconciled

- **Pattern.** Attach kernel-resident state (XDP programs, flow rules) through an fd whose close detaches it (an XDP
  `bpf_link`). Otherwise pin it deliberately under a known name, and have the owner reconcile at start.
- **From.** libxdp (netlink attach that persists, pinned dispatchers, explicit `libxdp_clean_references`, refcount in
  a map); bpf_link semantics (§2.4).
- **MTL.**
  - The manager owns XDP. Either attach with `bpf_link_create(BPF_XDP)` held by the manager, so a manager crash
    detaches it (and AF_XDP clients see `MANAGER_LOST` and their sockets stop receiving, which is a visible failure),
    or keep libxdp and reconcile on start: `libxdp_clean_references`, detach programs it does not own, delete ntuple
    rules in its own rule-location range.
  - Fix F-5.
  - Manager-less AF_XDP (F-6) cannot be made crash-clean while libxdp loads the program. Document `ip link set dev X
    xdp off` as the cleanup, or require the manager for AF_XDP in pods.
- **Pitfalls.** A netlink attach and a bpf_link on the same interface exclude each other. The libxdp dispatcher uses
  netlink, so changing it means not using libxdp's multiprog for MTL's program. Native mode availability depends on
  the driver.

### P-8 Heartbeat counters, read without locks, with a reporting ladder (the library never kills)

- **Pattern.** Every polling thread bumps a per-thread counter each loop and stamps the time of its last loop. A
  reader compares two readings. Warnings back off by doubling, events are raised, and the application or the
  orchestrator decides to restart.
- **From.** VPP's barrier timeout, OVS RCU's "blocked … to quiesce" with a doubling threshold, DPDK lcore usage
  telemetry, the systemd watchdog, Kubernetes liveness probes (§2.7).
- **MTL.**
  - Stats keys `sched.<n>.loops`, `sched.<n>.last_loop_ns` and `sched.<n>.busy_ratio` (`mtl_observe.h` registry,
    already lock-free per D-80), and `session.<id>.last_progress_ns`.
  - An admin-worker check raises `MTL_EVENT_SCHED_STALLED` (under `MTL_SUB_INSTANCE`) when `last_loop_ns` is older
    than `instance.stall_ns` (default 100 ms, the same as `cmd_ack_timeout_ns` in 04 §3.2). It warns at 1×, 2×, 4×.
  - Optionally a one-call `mtl_instance_health(mt)` (DP) returns a bitmask: `SCHED_STALLED`, `PORT_DOWN`,
    `PORT_REMOVED`, `TIME_UNLOCKED`, `MANAGER_LOST`.
  - Example: a liveness probe fails on `SCHED_STALLED` or `PORT_REMOVED` only; a readiness probe also fails on
    `TIME_UNLOCKED` and `PORT_DOWN`.
- **Pitfalls.** With `TASKLET_SLEEP` an idle scheduler sleeps, so the counter must also advance on wake-ups and timer
  ticks, or idle looks stalled. Do not make liveness depend on PTP lock or link state, or a grandmaster outage
  restarts every pod. VPP's `os_panic` is the wrong default for a library.

### P-9 Signals: no handlers and no `atexit` in the library; async-signal-safe kicks; the work runs on a normal thread

- **Pattern.** The library installs no signal handler and no `atexit`. It gives AS-safe "wake everyone" calls. The
  application turns SIGTERM into a call on an ordinary thread, through a self-pipe, `signalfd`, or the handler setting
  a flag.
- **From.** DPDK (no handlers by default); Java and Go shutdown semantics; systemd and Kubernetes signal flow; PID-1
  behaviour (§2.5).
- **MTL.**
  - R4 has `mtl_instance_interrupt` and `mtl_instance_abort` (AS), and 03 §7.1 already forbids `atexit`.
  - Add an example (`ex_k8s_shutdown.c`): SIGTERM → interrupt(1) → the main thread runs `mtl_instance_shutdown(...,
    grace − 2 s)` → `exit(0)`. A second SIGTERM → `mtl_instance_abort`.
  - Document `terminationGracePeriodSeconds ≥ drain + 04 §4.5 reset budget + 2 s`, and running under `tini` or with a
    handler when the app is PID 1.
  - `preStop` is for a deregistration that must happen before SIGTERM (NMOS IS-04 unregister, for example), not for
    MTL teardown.
- **Pitfalls.** `mtl_instance_abort` must stay AS-safe: atomics and an eventfd write only. A plugin (GStreamer,
  FFmpeg) must not install handlers either; the host application owns signals. Do not rely on SIGTERM ever arriving:
  the design must be correct for SIGKILL (P-1).

### P-10 A fork rule

- **Pattern.** After `fork()` the child may only `exec` or `_exit` with respect to MTL. Calls in the child fail
  cleanly, and the parent's DMA memory is never shared copy-on-write.
- **From.** `ibv_fork_init` and `MADV_DONTFORK` (§2.1); DPDK multi-process is unsupported across namespaces (§2.2).
- **MTL.** Add a rule **R8** in `mtl.h`: "An instance belongs to the process that opened it; in a forked child every
  call returns `-MTL_EBADF` except close, which only drops local state." It is implemented with a
  `pthread_atfork` child handler that sets a flag, or a cached PID compared at handle validation (CP only). Library
  hugepages and pools get `MADV_DONTFORK`. Imported regions do not (the application owns their semantics), but the
  docs warn about them.
- **Pitfalls.** `system()` and `popen()` in frameworks fork implicitly. `posix_spawn` with vfork semantics is fine.
  `MADV_DONTFORK` on a mapping the child touches makes the child segfault, which is the intended failure.

### P-11 Startup reconciliation: assume the previous incarnation died uncleanly

- **Pattern.** Open never trusts state left behind. It resets what it owns and removes stale artefacts by name.
- **From.** EAL "removing existing backing files at startup"; VFIO's reset at enable; `libxdp_clean_references`;
  MtlManager's re-announce (§2.2–§2.4).
- **MTL.**
  - The kernel already resets the VF at VFIO open.
  - The instance flushes `rte_flow` rules and RL/TM configuration at port start, and does not assume a clean ICE TM
    tree.
  - The manager, on start, reconciles XDP and ntuple rules and drops lcores whose client socket is gone (P-7).
  - The OFD lockfiles need no cleanup by design (P-6).
  - The instance logs every reconciliation it performs, as one line per kind with a count.
- **Pitfalls.** Never reset PF-side state a CNI owns (VF MAC, VLAN, spoof check). Reconcile only names and ranges MTL
  created.

### P-12 Say who holds what when close cannot finish

- **Pattern.** When a close returns "still retiring", the library can name the holders: which session, which slot,
  the lease's age, which thread acquired it.
- **From.** Vulkan validation layers, GStreamer's leaks tracer, io_uring's per-resource tags.
- **MTL.** `mtl_session_close` returning 1 logs one warning per session with its count of held leases and the oldest
  lease's age, and fills stats keys `session.<id>.leases_out` and `oldest_lease_ns`. The shutdown report (P-3) carries
  the totals.
- **Pitfalls.** No per-lease allocation on the data path: the age comes from the acquire timestamp already in the
  lease table. Warn once, not every poll.

## 4. Proposed contract: what holds after each kind of ending

Legend:

- (a) orderly: `mtl_instance_shutdown` returned 0 or 1, then the process exited;
- (b) SIGKILL at any instant, including inside a library call;
- (c) device removal, or a reset that does not recover, while the process keeps running.

**K** kernel, **L** library, **M** MtlManager, **A** application, **O** orchestrator (kubelet, CNI, device plugin).

| Resource | (a) orderly | (b) SIGKILL | (c) device removal | By |
|---|---|---|---|---|
| Instance (schedulers, admin and waker threads) | threads joined; nothing calls app code; later calls `-MTL_ESHUTDOWN` | gone with the process | stays; affected ports REMOVED; health bit set; close works | L / K |
| TX session | DRAIN to a frame boundary until the deadline, then FLUSH; every accepted unit has a result | wire stops at once (bus master off); a partial frame on the wire; no results | ERROR `DEVICE_REMOVED`, or degraded leg (2022-7); units FLUSHED; waits `-MTL_ENODEV` | L / K |
| RX session | stopped; IGMP leave sent; completed units deliverable until close | gone; **no IGMP leave**: the switch floods until the querier ages it out (G6) | ERROR or degraded leg; incomplete unit force-completed with status | L / K |
| Lease held by app | after shutdown, release returns 0 or `-MTL_ESTALE`; never a crash | gone | release still works; slot never reused for that device | L |
| Library pool (hugepages, `--in-memory`) | freed at retire; or kept until exit if a lease is stale (P-4) | freed by the kernel (anonymous); with IOMMU, pinned pages outlive the DMA | not freed under a lease; freed at retire | L / K |
| Imported region | the device unmaps it before `mtl_mem_close` returns 0; 1 = still referenced | IOMMU unmap and unpin after DMA is off; the memory is the app's and dies with it | the removed port's mapping goes with the VFIO fd; other ports as (a) | L / K |
| No-IOMMU mode | as (a) | **no guarantee**: DMA may hit freed pages | **no guarantee** | — |
| Queue (`mtl_queue`), wait handles | unbound; dispatch thread joined; eventfds closed | closed by the kernel | stays; delivers `PORT_REMOVED` and `SESSION_STATE` | L / K |
| Timeline | closed with its last session | process-local, gone | unaffected (time source may degrade: `TIME_STATE`) | L |
| Codec plugin | `stop` called; threads joined, or counted as unjoined | gone | sessions ERROR; plugin stopped as in (a) | L / A |
| Lcores (manager) | returned with `put_lcore` | released when the manager sees socket EOF | unaffected | M / K |
| Lcores (no manager) | OFD byte locks released | released by the kernel at the last close (P-6); today a SysV entry leaks or is stolen (F-3) | unaffected | K |
| Lcores (Guaranteed pod) | nothing to release | nothing | unaffected | O |
| VF queues, RL/TM nodes, `rte_flow` on the VF | queues stopped, flows destroyed, port stopped | FLR at fd release; the PF clears the VF's queues and filters **[inferred]**; reconciled at next open (P-11) | gone with the device; nothing to undo | L / K |
| Manager ntuple rules, XDP program, xsk map entry | removed by the manager at client EOF | removed by the manager at EOF; if the **manager** is killed they persist until it restarts and reconciles (P-7) | the manager drops them when the netdev goes | M |
| XDP, manager-less AF_XDP | detached by libxdp at refcount 0 | **persists**; the refcount leaks (F-6) | goes with the netdev | — |
| VF DMA and bus mastering | queues stopped, then fd closed | `pci_clear_master` and FLR in `vfio_pci_core_disable` | already gone | K |
| MtlManager registration | `put` messages, then close | socket EOF → `~mtl_instance()` releases everything | unaffected | M / K |
| Built-in PTP / PHC | client stops; PHC left free-running | PHC free-running at its last frequency | gone with the port | L |
| Files and IPC | none left (`--in-memory`); lockfiles stay, unlocked by design | the same | unaffected | L / K |
| The VF itself | returned to the device plugin's pool at pod deletion | the same | the device plugin reports it unhealthy | O |

Rules that make the table hold:

1. **Bounded stage 2.** `mtl_instance_shutdown` ends by `timeout_ns` plus the 04 §4.5 reset budget, never waiting on
   the application or the network.
2. **Close after removal.** After (c), close, release and every getter succeed without touching the device.
3. **Bounded waits.** After (c), every wait returns in bounded time.
4. **SIGKILL-safe state.** Nothing the library leaves outside the process after (b) needs a human: it is released by
   the kernel, released by the manager, or reconciled at the next open. G6 (IGMP) and no-IOMMU are the documented
   exceptions.

## 5. API deltas this implies (sketch level, for review; no header is changed by this study)

### 5.1 Instance shutdown (P-2, P-3, P-4, P-12)

```c
#define MTL_SHUTDOWN_ABORT 0x1u   /* skip the drain: stage 2 now */
#define MTL_SHUTDOWN_PROCESS 0x2u /* required on a SHARED instance: affects every holder */
struct mtl_shutdown_report {      /* output; the size argument versions it */
  uint32_t sessions, sessions_retired, sessions_retiring, plugins_unjoined;
  uint64_t leases_out, regions_referenced, units_flushed, bytes_leaked_until_exit;
  uint64_t ports_quiesced, ports_unquiesced; /* bit = port index */
  int64_t elapsed_ns;
};
/* Drain until timeout_ns, then abort and quiesce every port. 0: retired. 1: quiesced (no
   device access to any memory, no library thread calls app code) but leases or regions
   still referenced. -MTL_ETIMEDOUT: some port could not be quiesced; exit the process.
   Afterwards every call but close, release, get_state and the stats returns
   -MTL_ESHUTDOWN. Idempotent. CP. */
MTL_API_CP int mtl_instance_shutdown(mtl_instance_h mt, uint64_t flags, int64_t timeout_ns,
                                     struct mtl_shutdown_report* r, size_t r_size);
```

- It does not replace `mtl_instance_close`, which stays "drop my reference".
- `mtl_instance_abort` stays as the AS kick.
- 33 → 34 core functions.

### 5.2 Device removal and liveness (P-5, P-8)

- `MTL_EVENT_PORT_REMOVED`, `MTL_EVENT_SCHED_STALLED` in `mtl_queue.h`; reasons `DEVICE_REMOVED` and `SCHED_STALLED`
  in `mtl_reasons.h`.
- Port state `REMOVED`.
- `-MTL_ENODEV` for data calls on a session with no surviving leg.
- Options `instance.hotplug_sigbus` (default off) and `instance.stall_ns` (default 100 ms).
- Stats keys `sched.<n>.loops`, `sched.<n>.last_loop_ns`, `session.<id>.last_progress_ns`, `session.<id>.leases_out`.
- Optional `MTL_API_DP uint64_t mtl_instance_health(mtl_instance_h mt)`.

### 5.3 Rules (P-9, P-10)

- **R8 (fork)** as in P-10.
- **R9 (signals)**: the library installs no signal handler and no `atexit` handler, and calls no `exit`. AS calls are
  `mtl_instance_interrupt(…, 1)`, `mtl_instance_abort`, `mtl_session_interrupt(…, 1)` and `mtl_queue_interrupt(…, 1)`.
  Everything else runs on an application thread.

### 5.4 Implementation items, not API (track as side findings)

| ID | Item |
|---|---|
| SF-K3-1 | Fix the XDP mode overwrite (F-5, `manager/mtl_interface.hpp:430`) |
| SF-K3-2 | Register ethdev RMV, RESET and RECOVERY callbacks; post to the admin worker (F-1) |
| SF-K3-3 | Replace the SysV shm and `kill(pid, 0)` lcore allocator with OFD locks or affinity-only mode (F-2, F-3) |
| SF-K3-4 | MtlManager: `SO_PEERCRED` / `SO_PEERPIDFD`, reconciliation on start, XDP through `bpf_link` or explicit cleanup (F-4, P-7) |
| SF-K3-5 | `O_CLOEXEC` audit of every fd the library opens (eventfds, sockets, VFIO through EAL, lockfiles) |
| SF-K3-6 | Spike: anonymous hugepages vs DMA window at SIGKILL in no-IOMMU mode (§2.3); spike S8 (queue stop releases external mbufs) gates P-3 |
| SF-K3-7 | Manager-sent IGMP leaves on client EOF for groups the client registered (G6), when the manager knows them |

## 6. Questions for the owner

| ID | Question | Recommendation |
|---|---|---|
| Q-K3-1 | Adopt `mtl_instance_shutdown` with the 0 / 1 / `-MTL_ETIMEDOUT` report (§5.1)? | yes; it is the "safe closing" the owner asked for, and the per-object closes stay |
| Q-K3-2 | Forced retire with stale leases: keep library-pool memory mapped until exit (safe, leaks) or free it (reuses memory, turns app bugs into use-after-free)? | keep mapped; report `bytes_leaked_until_exit` |
| Q-K3-3 | Manager-less lcore arbitration: OFD lockfiles in a shared host directory, affinity-only, or require MtlManager in pods? | affinity-only for Guaranteed pods; MtlManager otherwise; OFD files as the manager-less fallback; remove SysV |
| Q-K3-4 | Device removal: a session with no surviving leg goes ERROR (stays closeable) or is closed by the library? | ERROR; the application closes it (Vulkan, uverbs) |
| Q-K3-5 | Should the library ever install DPDK's SIGBUS hotplug handler? | only by option, default off |
| Q-K3-6 | Refuse no-IOMMU in pods unless an explicit option is set? | yes: `instance.allow_noiommu`, with a startup warning naming the crash risk |
| Q-K3-7 | IGMP after SIGKILL (G6): accept the querier timeout, or have MtlManager send leaves for dead clients? | accept and document for v1; manager leaves later |
| Q-K3-8 | Liveness: stats keys only, or also `mtl_instance_health()`? | both; the getter is one call a probe can make |

## 7. Sources

Kernel (Linux master, read 2026-10-01):

- `drivers/vfio/pci/vfio_pci_core.c`: <https://raw.githubusercontent.com/torvalds/linux/master/drivers/vfio/pci/vfio_pci_core.c> **[verified]**
- `drivers/infiniband/core/uverbs_main.c` and `rdma_core.c`; `include/rdma/ib_verbs.h`: <https://github.com/torvalds/linux/tree/master/drivers/infiniband/core> **[verified]**
- VFIO driver API: <https://docs.kernel.org/driver-api/vfio.html> **[verified]**

Man pages:

- ibv_fork_init(3): <https://man7.org/linux/man-pages/man3/ibv_fork_init.3.html> **[verified]**
- ibv_get_async_event(3): <https://man7.org/linux/man-pages/man3/ibv_get_async_event.3.html> **[verified]**
- io_uring_register(2): <https://man7.org/linux/man-pages/man2/io_uring_register.2.html> **[verified]**
- pthread_mutexattr_setrobust(3): <https://man7.org/linux/man-pages/man3/pthread_mutexattr_setrobust.3.html> **[verified]**
- fcntl(2): <https://man7.org/linux/man-pages/man2/fcntl.2.html>. The lock details are in fcntl_locking(2), which was not read, so the OFD lock claims above are **[inferred]**

DPDK:

- 26.07 headers `rte_eal.h`, `rte_ethdev.h`, `rte_dev.h`, `rte_lcore.h` in `/usr/local/include` **[verified]**
- EAL guide: <https://doc.dpdk.org/guides/prog_guide/env_abstraction_layer.html> **[verified]**
- Multi-process guide: <https://doc.dpdk.org/guides/prog_guide/multi_proc_support.html> **[verified]**

SPDK:

- <https://github.com/spdk/spdk/blob/master/include/spdk/env.h> and `nvme.h` **[verified]**

libxdp (xdp-tools main):

- `lib/libxdp/xsk.c`, `libxdp.c`, `README.org`: <https://github.com/xdp-project/xdp-tools/tree/main/lib/libxdp> **[verified]**

DOCA:

- DOCA Core: <https://networking-docs.nvidia.com/doca/sdk/doca+core> **[verified]**

libfabric:

- fi_endpoint(3): <https://ofiwg.github.io/libfabric/main/man/fi_endpoint.3.html> **[verified]**

VPP and OVS:

- VPP: <https://github.com/FDio/vpp/blob/master/src/vlib/threads.c> **[verified]**
- OVS: <https://github.com/openvswitch/ovs/blob/main/lib/ovs-rcu.c> **[verified]**

Not re-read in this study (**[inferred]**, from knowledge):

- Kubernetes pod lifecycle: <https://kubernetes.io/docs/concepts/workloads/pods/pod-lifecycle/> (the fetched page was truncated before "Termination of Pods")
- Vulkan "Devices and Queues" chapter (the fetch was truncated)
- CUDA runtime API `cudaDeviceReset`
- gRPC `GracefulStop`, Envoy drain, Java `Runtime.addShutdownHook`, Go `os/signal`, systemd.service(5)
- PipeWire, GStreamer base classes

In-repo:

- `lib/src/mt_sch.c`, `lib/src/mt_platform.h`, `lib/src/dev/mt_dev.c`, `lib/src/dev/mt_af_xdp.c`, `lib/src/mt_main.c`
- `manager/mtl_manager.cpp`, `manager/mtl_instance.hpp`, `manager/mtl_interface.hpp`, `manager/mtl_mproto.h`
- research/09, research/10, research/13; 03 §6–§7; 04 §4.5, §5.4; 15 §7–§8
