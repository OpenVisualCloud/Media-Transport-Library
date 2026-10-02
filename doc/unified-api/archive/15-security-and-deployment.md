# 15 — Security considerations and deployment

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5); new in revision 3 |
| Date | 2026-09-30 |
| Baseline | `main` @ `545a266a` |
| Requirements | R-SEC-1, R-TEST-1…3, R-OPS-6, R-ABI-2 |
| Why | review C5 found no security consideration of the new surfaces, no Windows stance beyond an open question, and no deprecation policy `[C5 §1.10]` |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

MTL runs with privileges most libraries never get: it owns NIC queues through VFIO or a
PMD, pins memory, maps it for device DMA, runs busy-loop threads on isolated cores and
talks to a root daemon. The new API adds surfaces on top of that. This document states,
for each, what it exposes, what the design does about it, and what stays the deployer's
responsibility. The threat model is **a local process boundary**: MTL does not defend a
process against code running inside it (the app, its plugins, its bindings), but it must
not let one process, or one mistake, reach memory, devices or cores it did not ask for.

## 1. Surfaces at a glance

| Surface | New in the unified API? | What can go wrong | Section |
|---|---|---|---|
| imported memory mapped into the IOMMU | yes (`mtl_mem_import`) | the NIC reads or writes memory the app did not mean to share; DMA into freed memory | §2 |
| wait objects (eventfd / `HANDLE`) | yes | fd leaks across `fork`/`exec`; an app draining the library's fd loses wake-ups | §3 |
| waker thread, optional `SCHED_FIFO`, affinity | yes | starvation of other threads; landing on an isolated scheduler core | §4 |
| handle table and generations | yes | handle confusion across sessions | §5 |
| null backend, test time source, debug API | yes | fault injection or a fake clock in a production process | §6 |
| MtlManager socket | existing | any local user steering lcores, XDP maps and flows | §7 |
| default instance, multi-process | extended | one component's instance settings imposed on another | §8 |

## 2. Imported memory and the IOMMU

`mtl_mem_import` (05 §3) takes application memory — anonymous, `memfd`, POSIX shmem,
hugetlbfs files, GPU-pinned host memory — pins it and maps it into every device on the
session's path (both 2022-7 ports and the DMA engine, R-MEM-4). From then on the device
can **read** it (TX) or **write** it (RX) without the CPU.

What an import exposes, and the rules that bound it:

- **Page alignment (r3).** `va` and `length` must be page-aligned (hugepage-aligned for
  hugetlbfs), otherwise the import fails with `-MTL_EINVAL`, reason `UNALIGNED`. The
  IOMMU maps whole pages, so rounding an unaligned range out to page boundaries would map
  the neighbouring bytes too: another object's heap data, readable by the NIC on TX
  and — worse — writable by it on RX. The design never rounds; plane offsets inside an
  aligned region stay byte-granular (G-96, `[C5 §6.5]`). The accepted page sizes are in
  `mtl_port_get_caps`.
- **Access is enforced at attach.** A region imported `MTL_MEM_READ` cannot be attached to
  an RX session (which needs WRITE); a TX attach needs READ `[C5 §6.10]`. The device
  mapping itself is made with the matching direction where the IOMMU supports it.
- **Lifetime.** The mapping lives until `mtl_mem_destroy` returns 0, which it does only
  when no buffer, conversion, packet or DMA reference remains (R-MEM-3). The app must not
  `munmap`, `free` or truncate the memory before that. If it does, the device may DMA into
  pages the kernel has reused: a device-level use-after-free the library cannot detect.
  The teardown rule is "wait for `SESSION_RETIRED`, then destroy buffers, then the region"
  ([05](05-memory-and-buffers.md)).
- **Shared memory is shared.** A `memfd` or shmem region imported for TX can be modified
  by every process that maps it, including while a unit is IN_FLIGHT: that is an
  integrity property the sharing process accepts, not a library bug. For RX, every peer
  sees the data the NIC writes. Recommended: seal `memfd` regions with
  `F_SEAL_SHRINK | F_SEAL_GROW` before import, so a peer cannot truncate the pinned
  pages away.
- **Regular files** mapped with `mmap` are accepted only as COPY (never mapped for DMA),
  detected from the backing filesystem and reported in `mtl_mem_get_info().backing`;
  hugetlbfs files are DIRECT.
- **PA mode** (no IOMMU, `--iova-mode=pa`) gives no protection at all: the device can
  reach any physical address. It is reported in `mtl_port_get_caps` and deployment guides
  should treat it as trusted-host only.
- **Budget.** Each import consumes IOMMU mappings and DPDK memseg entries; the per-port
  `max_regions` and `-MTL_ENOSPC` with reason `REGION_BUDGET` stop a process exhausting
  them `[C5 §6.4]`.
- **Pinning.** Pinned memory counts against `RLIMIT_MEMLOCK`; a failed pin is a clean
  `-MTL_ENOMEM`, never a partial mapping.

## 3. Wait objects

`mtl_<obj>_get_wait_object` (A5) returns `{ native, kind }`: an eventfd on Linux, an event
`HANDLE` on Windows.

- The library **owns** the object. The app may poll or wait on it but must not read,
  write or close it; the library drains it (G-52). An app that reads the eventfd itself
  consumes a wake-up the library's try-wait protocol expects.
- Every fd is created `EFD_CLOEXEC | EFD_NONBLOCK`, so it does not leak into a child
  after `exec`. After `fork`, the child must not use the parent's instance at all (DPDK
  does not support it); its copies of the fds are inert.
- Wait objects carry no data and grant no access: writing to one only causes a spurious
  wake-up, which every wait loop tolerates.

## 4. Threads: the waker, priorities and affinity

- The **waker** (04 §5.2) runs at normal priority (`SCHED_OTHER`) by default. An optional
  `SCHED_FIFO` priority is an explicit instance setting; it needs `CAP_SYS_NICE`, and
  failure to get it is reported, never fatal. The waker never spins: it sleeps to the next
  due time (at most 1 ms with no due time), so a real-time priority cannot starve the
  host.
- **Affinity** is explicit. The waker and every library worker are placed on the
  instance's non-scheduler CPU set; they never inherit the creating thread's affinity and
  never run on a scheduler lcore. Placement is reported in `mtl_sched_get_status`.
- **`MTL_FLAG_TASKLET_THREAD`** (containers, no isolated cores) changes the threat model:
  scheduler threads are ordinary preemptible threads. W2 (the scheduler thread writes the
  eventfd when a waiter is armed) is the default there, scheduler threads call
  `rte_thread_register` so they get an lcore ID and a mempool cache, and G-39 is measured
  per mode `[C5 §4.8]`.

## 5. Handles

- Handles are values (`index | generation`), validated against a grow-only chunked table;
  no call dereferences a pointer the app gave it. ID 0 is null for every type (G-71).
- Lease generations are seeded randomly per session and skip 0, and a lease names its
  session index, so a foreign or stale lease fails deterministically (`-MTL_EBADF` or
  `-MTL_ESTALE`) instead of acting on another session's slot (G-07).
- Handles are **not capabilities**. They are meaningful only inside the process that
  created them, carry no authority across processes, and the randomness defends against
  mistakes (reuse after destroy, cross-session mix-ups), not against hostile code in the
  same process.

## 6. Null backend, test time source and debug API — build and runtime gating

| Piece | Built | Usable when | Why |
|---|---|---|---|
| null backend (`null:<n>`) | always (experimental) | any instance whose port spec asks for it | it touches no device and no clock: harmless, and needed by bindings and doc tests on release builds |
| test time source (`mtl_time_test_source`, `mtl_time_test_advance`, in `mtl_debug.h`) | only with `-Denable_debug_api=true` | any instance of such a build | a fake clock re-times every session of the instance |
| fault injection (`mtl_debug_inject`, `mtl_debug.h`) | only with `-Denable_debug_api=true`; off by default, never in release packages | any instance of such a build | it can force ERROR, drop packets, fake link loss and manager loss |

- The meson option is independent of `buildtype`; `./build.sh debug` does not turn it on.
  Today's test-only TX mutation helpers are compiled in for every debug buildtype
  (`-DMTL_SIMULATE_PACKET_DROPS`, `lib/meson.build:91-96`); the debug API replaces that
  pattern with an explicit opt-in.
- A release build keeps the `mtl_debug.h` entry points as stubs that return
  `-MTL_ENOTSUP`, so bindings generated from the full header still load, and it links no
  fault or test-clock code; CI checks both (G-93).
- A library built with the debug API logs a warning at instance open, and
  `mtl_instance_get_status` reports it, so a debug build in production is visible.

## 7. The MtlManager socket

MtlManager is a root daemon that grants lcores across processes, loads XDP programs, adds
flows and UDP filters on interfaces, and hands out XDP socket-map fds. At `545a266a`:

- the socket `/var/run/imtl/mtl_manager.sock` (`manager/mtl_mproto.h:16`) is made
  world-accessible ("Allow all users to connect (which might be insecure)",
  `manager/mtl_manager.cpp:95-96`);
- a client's identity is the `pid` and `uid` it writes into its own register message
  (`lib/src/mt_instance.c:213-214`, read at `manager/mtl_instance.hpp:192-193`); the
  manager does not use `SO_PEERCRED`;
- so any local user can obtain an interface's XSK map fd over `SCM_RIGHTS`
  (`manager/mtl_instance.hpp:244-269`), add or remove flows, and take lcores (SF-47);
- the manager's own `send`/`sendmsg` pass no `MSG_NOSIGNAL` and it does not ignore
  SIGPIPE (`manager/mtl_instance.hpp:58`, `:269`), so a client that disconnects early can
  kill it (SF-48, **[inferred]**).

The design's position:

- the **library side** handles manager loss without dying (`MSG_NOSIGNAL`, open PR #1770;
  G-98); addendum K removes the shm allocator and `MTL_INSTANCE_MANAGER_OPTIONAL` (D-92):
  without a manager, the affinity mask or OFD locks arbitrate ([16 §5](16-kubernetes-and-crash-safety.md));
- the **manager side** needs, independently of the new API: socket mode `0660` with a
  configurable group (for example `mtl`), identity from `SO_PEERCRED` instead of the
  message, per-uid ownership of lcores, flows and filters so one client cannot release
  another's, and SIGPIPE ignored. These are listed as side findings for the manager's
  owner, not as unified-API work;
- after a manager restart, clients re-register and re-announce the lcores they hold, so
  a restarted manager does not grant them again ([07](07-completions-events-and-errors.md)).

## 8. Deployment

### 8.1 Processes and the default instance

- One EAL per process (`--in-memory`, NG5): no secondary processes, no session sharing
  between processes. A process has at most one instance per port set.
- `mtl_instance_acquire_default` gives the components of one process (a GStreamer
  element, an FFmpeg device, the app) one refcounted instance. The merge table decides
  which parameters must match (invariant), which are merged (ports must already be open;
  queue counts summed up to the hardware maximum at first open) and which a second caller
  cannot change (ignored, reported in `mtl_instance_get_info`) ([03 §7](03-object-model-and-lifecycle.md)).
  One component therefore cannot silently re-configure another's instance.
- Separate processes (one per essence) share time, not state: the epoch timeline plus the
  index helpers ([06](06-timing-pacing-and-sync.md)); MtlManager arbitrates lcores and
  queues between them.

### 8.2 Containers

> **Addendum K (2026-10-01).** Kubernetes pods are designed in [16](16-kubernetes-and-crash-safety.md):
> a bounded shutdown, crash-only rules, health for probes, CPUs from the cpuset, a privilege
> profile per backend and a pod spec (§10.4). It replaces the bullets below where they differ:
> a pod needs no MtlManager by default, and `TASKLET_THREAD` is pinned too.

- Use `MTL_FLAG_TASKLET_THREAD` (§4) where cores are not isolated.
- The container needs the VFIO device nodes of its VFs, hugepages mounted, a
  `RLIMIT_MEMLOCK` large enough for its pools and imports, and — if it uses MtlManager —
  the manager socket bind-mounted with a group the container user belongs to (§7). Without
  the manager no flag is needed (D-92).
- The null backend needs none of this and is the recommended way to run an app's own CI
  inside an ordinary container.

### 8.3 Hugepage budgeting

- Each session reports its memory in `mtl_session_get_info`: the pool
  (`pool_count × unit_bytes`), the library-internal transport frames (`internal_bytes`,
  which doubles memory on CONVERT paths), and the per-essence capacities.
- `mtl_instance_get_mem_status` reports, per NUMA node, the hugepage size, total, free,
  used by this instance and the largest free contiguous segment.
- A deployer sizes hugepages as Σ(pool + internal) per node plus the instance's fixed cost
  (mempools, rings) reported at open, with headroom for recovery cycles (a recovering TX
  session creates a new mempool before the old one is freed). A create that does not fit
  fails with `-MTL_ENOMEM` and the node in `mtl_last_error`, never by crashing.
- `numa_mismatch` in `get_info` flags a pool on the wrong node; `MTL_MEM_NUMA_REQUIRED`
  turns that into a failure.

### 8.4 Windows

- **Stance (r3, proposed answer to Q-ABI-6):** the unified header is one ABI on every OS.
  Wait objects carry an event `HANDLE` (`MTL_WAIT_WIN_HANDLE`), error codes are `MTL_E*`
  constants with fixed values (private values where UCRT lacks the errno) `[C5 §2.13]`,
  and flags are plain integer literals.
- **v1 commitment:** a compile-only CI job builds the unified headers and the examples
  with MSVC/MinGW. Runtime support follows the legacy library's Windows support; two of the
  eleven open issues today are Windows build failures (#1672, #1301), which is where that
  support stands.

## 9. Release, deprecation and support policy

The policy is stated in [14 §7](14-implementation-roadmap.md). In short: the unified API
ships as `libmtl_unified.so.0.<rev>` with no compatibility promise until the ABI freeze;
at the freeze it moves to `MTL_1.0` and the replaced legacy APIs are deprecated; a
deprecated API is removed no earlier than two `vYY.MM` releases later. Security fixes to
the manager (§7) and to import validation (§2) are backported to the legacy API because
they are not API changes.
