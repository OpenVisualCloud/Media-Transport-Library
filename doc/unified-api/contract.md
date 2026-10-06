# Unified API contract

| | |
|---|---|
| Status | Normative behaviour of every call of the unified API, the Kubernetes lifecycle and the NMOS/IPMX extensions included. Nothing is implemented. MS1–MS6 port today's functionality; items marked "(Phase 7, later)" come after MS7, and everything of Phase 7, created timelines included, is declared only under `MTL_LATER` |
| Date | 2026-10-02 |

This file states what every call does, in every state, by area. It is the reference for
implementers and test writers. Names, types, layouts and call classes come from the headers
in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/). **If this file and a
header disagree, the header wins** and this file is fixed. Media time, RTP, launch and late
policy are in [timing.md](timing.md). How the contract sits on the core (the bindings, the slot
table, the deferred wake, commands) is in [core.md](core.md), and on today's engines (the
stalled-queue path) in [engine.md](engine.md). Guarantee IDs (G-xx) name the contract test that pins a rule; the test
plan is in [implementation-plan.md](implementation-plan.md), the full list in
[requirements.md](requirements.md) §4.

**The library.** There is one library, libmtl. It exports the unified functions in symbol version
nodes per milestone, `MTL_UNIFIED_EXPERIMENTAL_<rev>_MSn` and its parts, each inheriting the
previous and sealed at every release, all renamed together on every incompatible change until the
freeze, and `MTL_1.0` from MS7 (the bridge `mtl_instance_from_legacy` in its own node,
`..._BRIDGE`, never in `MTL_1.0`); the API
shell (`lib/src/unified/`) is compiled into libmtl and pkg-config stays `mtl`. In MS1 a version
script puts the unified functions in their nodes and leaves every other symbol exported as today; the soname, the `MTL_LEGACY` node of the legacy functions and
the hiding of internals come in MS3 (D-23, D-108).

**The surface.** A function is exported from the milestone its sketch comment names, `(MS1)` to
`(MS7)`, which is also the argument of its call-class macro (`MTL_API_DP(1)`); the installed
header declares the whole design. `MTL_LEVEL` in `mtl.h` is the last milestone whose exit passed: a
call to a function above `MTL_TARGET_LEVEL` (default `MTL_LEVEL`) fails to compile with GCC and
Clang 14 or later, naming the function and its milestone, and fails at link time elsewhere (D-106).
One tagged `(Phase 7)` or `(later)` is declared only under `MTL_LATER`, for design checks, and may
change in any release. The values of an exported call are declared from the start and return
`-MTL_ENOTSUP` until their milestone (R1, D-106). The functions per header, per call class and per
milestone, and the number of frozen names, are printed by `sketch/check.sh`, the one source of those
counts. Everything else is `static inline` and exports no symbol: the typed wrappers of the object
verbs ([§1, Object verbs](#object-verbs)), and helpers built only on public calls
(`mtl_session_open`, `mtl_stat_get`, `mtl_port_find`, `mtl_flow_ipv4`, `mtl_flow_parse`,
`mtl_time_convert`, `mtl_rx_align`, `mtl_fps_rational`, `mtl_error_name`, `mtl_struct_init`,
`mtl_audio_bytes`, all of `mtl_util.h`).

## 1. The rules R1–R8

The top of `mtl.h` carries eight rules that every call follows. They are stated once here; the sections below only add what is specific to a call.

### R1 Returns

- Every call returns 0, or a count or mask > 0, on success, and a negative `MTL_E*` code on failure.
- A getter that returns a state (`mtl_session_get_status`, and its wrapper `mtl_session_get_state`) or flags (`mtl_time_now`, `mtl_instance_get_health`) returns them as the non-negative result.
- **Not implemented yet.** A function of a later milestone is declared in the installed header but
  not exported: a call to it fails to compile with GCC and Clang 14 or later, naming its
  milestone, and at link time elsewhere (`MTL_LEVEL`, `mtl.h`). A known value of an exported call that the running library does not implement
  yet (a flag, enum value, update part, `when` kind, wait mask, option key or port prefix) is
  `-MTL_ENOTSUP` with reason `NOT_IMPLEMENTED`, and `mtl_last_error().field` names it. An unknown
  value is `-MTL_EINVAL`. An output field not computed yet is zero with its VALID flag clear.
- A call that succeeds never writes `mtl_last_error()` (errno semantics, §8.2). A helper of `mtl_util.h` that detects a failure itself (a bound, a malformed payload) returns its code without setting it, so a program prints the reason and the field of `mtl_last_error()` only when its `code` equals the code it got.
- Codes are Linux errno values on every OS, one meaning each (§8.1). Programs compare with `MTL_E*`, never with `errno.h`.

### R2 Timeouts and "nothing now"

- A timeout is the last argument, `int64_t` ns: 0 = do not wait, `MTL_FOREVER` (−1) = no limit. A
  timeout below −1 is `-MTL_EINVAL`. A timeout is a duration on `CLOCK_MONOTONIC`, fixed at entry:
  a step of the time base, PTP loss or `clock_settime` never changes it.
- A data call (acquire, dequeue, reap, read, wait) that finds nothing, now or by its timeout, returns `-MTL_EAGAIN`. There is no separate "timed out" code for data calls.
- A read that returns a count returns at least 1; it never returns 0.
- `-MTL_EAGAIN` sets only `code` and `reason` in `mtl_last_error()` (no detail is formatted on the data path).
- `-MTL_ETIMEDOUT` is only for a control-plane deadline: a DRAIN `mtl_session_stop` that missed it. No close returns it: `mtl_session_close` returns 0 or 1, `mtl_instance_close` 0, 1, `-MTL_EIO` (`QUEUE_QUARANTINED`) or `-MTL_EDEADLK` (`LIBRARY_THREAD`).
- **Waiting.** With timeout 0 a data call tries once and changes no wait state; with a timeout
  it sleeps on its object (§7.1). Event loops wait on a queue (§7.2). An application that never
  sleeps never causes a wake-up syscall. A completing tasklet never makes the syscall itself: its
  scheduler wakes after its handler loop, a bounded number per iteration (D-142).

### R3 Structs

- Input structs start with `uint32_t struct_size`. `MTL_INIT(&s)` zero-fills and sets it through the inline `mtl_struct_init(p, size)`; bindings do the same in their own language (zero-fill, then the first `uint32_t` = the size).
- Zero in every field but `struct_size` is the default: no input field has a non-zero default (G-73).
- The library reads `min(struct_size, known)`. Non-zero bytes beyond what it knows are `-MTL_EINVAL` (`NONZERO_TAIL`); unknown flag bits are `-MTL_EINVAL` (`UNKNOWN_BITS`) (G-33, G-51).
- `struct_size` 0, or below the first published size, is `-MTL_EINVAL` (`NONZERO_TAIL`): a zero-initialised struct from a binding (SWIG `new_*()`, Rust `Default`) must never silently mean "all defaults"; it goes through `MTL_INIT`, `mtl_struct_init()` or the binding's equivalent.
- Output structs carry no `struct_size`. The library fills them up to the `size` argument and zeroes what it does not know.
- A struct the library writes and reads back (`struct mtl_unit`) uses its own `struct_size`.
- Optional indices: a field, argument or output where 0 means the default, all or none holds
  `MTL_INDEX(i)` = i + 1 (ports, schedulers, NUMA nodes, option scopes; `mtl_mem_info.numa`, 0 =
  mixed); an index that is always present (object references, event indices, leg numbers in
  records, `mtl_leg_info.port`) is 0-based; an option value is literal.
- Every pointer in an input struct is read during the call and deep-copied, strings included. The caller may free it on return. (The plugin device table of `mtl_plugin.h` is the one exception; see [migration.md](migration.md).)
- Embedded structs (`mtl_flow`, `mtl_raster`, the essence members) are fixed size and version with their parent.
- Enumerated fields are `uint32_t`; no `_MAX` sentinel is exported. **Status enums start at 1** (`MTL_TX_ON_TIME`, `MTL_RX_COMPLETE`): 0 is never a terminal status, so a zeroed or half-filled record never reads as success (G-57).

### R4 Handles

- Handles are 64-bit values of distinct C types: `mtl_instance_h`, `mtl_session_h`, `mtl_lease_h`, `mtl_region_h`, `mtl_queue_h` (MS2), plus `mtl_plugin_h` (`mtl_plugin.h`).
- 0 is the null handle of every type (`MTL_NULL(T)`, `MTL_IS_NULL(h)`). `MTL_SAME(a, b)` compares two lvalue handles of one type; mixing types warns in C and fails in C++ (G-71).
- A closed handle is never reissued.
- A call with an out handle writes the null handle on failure.
- **Close is idempotent.** `mtl_close` returns `MTL_RETIRING` (1) while the object retires and 0
  once it retired. Calling it again on the same handle polls (0 or `MTL_RETIRING`), never `-MTL_EBADF`, and a null handle returns
  0, so cleanup paths need no checks. From the first close on, the only other calls valid on the
  handle are `mtl_session_get_status` (state `MTL_STATE_CLOSING`, then `MTL_STATE_RETIRED`), the
  release of a lease taken before the close (retirement waits for it, §4.9), `mtl_interrupt`
  (AS; a no-op that returns 0), and on a queue an `mtl_queue_wait` already in progress, which
  returns `-MTL_ESHUTDOWN` (a call made after the close is `-MTL_EBADF`).
- A stale or foreign handle fails with `-MTL_EBADF`. A lease already returned fails with `-MTL_ESTALE`. Neither changes any state (G-07). The one exception: `mtl_instance_get_health` on an instance handle consumed by close or shutdown returns `-MTL_ESHUTDOWN`, so a probe thread racing the close reads "shutting down" (liveness 200, readiness 503), never a liveness failure (§2.7).
- Handle slots are process-wide and never freed. A handle therefore stays safe to pass after its instance is gone: on an object the instance closed, data calls return `-MTL_ESHUTDOWN`, its close returns 0 and a lease's release returns 0 (§2.4).
- A buffer is named by its pool slot index (`uint32_t`), never by a handle. The lease stays its own type, so a slot can never be passed where access moves.

### R5 Times

- Every time is `int64_t` ns since 1970-01-01 TAI on the instance clock.
- A time is valid only when its flag says so (`MTL_TIMEF_*`, `MTL_UNITF_TAI_VALID`, `MTL_TXR_*`). Zero is a valid time when flagged (G-17).
- While the time base is locked, times are TAI. When it is not (a free-running clock without PTP), they are flagged ESTIMATED (`MTL_TIMEF_ESTIMATED`, `MTL_TXR_ESTIMATED`).
- A UTC value read as TAI (a wrapper's default, TSC or UTC user clock; the built-in client before
  its first Announce) is flagged `MTL_TIMEF_UTC` with ESTIMATED, never with
  `MTL_TIMEF_ARB_TIMESCALE`; the sessions' media times and RTP are on that scale (§2.8).
- When the grandmaster runs an ARB timescale (`clockClass` 220 or 228, or `timeSource` F0h, ST
  2059-2 §5.5.4), times are common to its PTP domain but are not TAI: `mtl_time_now` flags
  `MTL_TIMEF_ARB_TIMESCALE`, and `time.arb_timescale` reads 1. The built-in client reads it from
  Announce; with another source the application passes `clock_class` and `time_source` in
  `mtl_time_set_reference` ([timing.md](timing.md) §2.3).
- An RX value on the sender's clock is flagged `MTL_UNITF_SENDER_TIME` (IPMX; Phase 7, later).
- Details: [timing.md](timing.md).

### R6 Call classes

| Class | Contract |
|---|---|
| CP | control plane: application threads; may allocate and block; serialised per object inside the library. It never holds a lock a tasklet can take while it blocks: ARP, IGMP, flow and queue work run off to the side and are published by a command ([core.md](core.md) §5) |
| DP | data plane: O(1); no allocation, no lock a tasklet takes, no logging; only the syscalls listed below |
| DPC | data plane that does work in the caller: copy, conversion, zero-fill; bounded by the unit size |
| WT | wait; DP when the timeout is 0, except `mtl_queue_wait`, whose timeout 0 arms its queue (§7.2); with a timeout it sleeps on its object, or a queue on its descriptor |
| AS | async-signal-safe: callable from a signal handler at any time, also during and after close and in a forked child; atomics, the futex call and `write()` only; it keeps `errno` and never writes `mtl_last_error()` (its code is its return value) |

- **The DP syscalls.** A DP call, or the final attempt of a WT call, makes syscalls only on its
  own object, and only these: when it makes a target ready for a sleeper, one wake: a futex wake
  for a call with a timeout, or a `write()` of a queue the object is armed on. This covers a
  release of an acquired lease, a reap that frees result space, a failed in-caller conversion,
  submit's own flush when the session stops (D-121), and an arm that finds its object ready.

  The debug-build class check (G-50; from MS3, D-05) allows exactly these. Every syscall is raw,
  so no DP, WT or AS call is a cancellation point. A tasklet never makes a wake-up syscall:
  `mt_wake()` marks the object, and the loop wakes a bounded number of marked objects and queues
  per iteration after its handler loop (W2-deferred, G-39, G-142, [core.md](core.md) §6.2). A
  wake from any other thread is direct.
- Every exported function carries exactly one class annotation (`MTL_API_CP`, `MTL_API_DP`, `MTL_API_DPC`, `MTL_API_WT`, `MTL_API_AS`), and debug builds assert it (G-50).
- **No application code ever runs on an MTL tasklet** (G-38, G-39), and the library calls
  application code only from the one log thread that runs the log sinks (`mtl_log_add_sink()`,
  MS2a) and, for codec plugins, from their own threads. One exception, which ends with the bridge:
  on a wrapper of a legacy instance (`mtl_legacy.h`), the legacy `ptp_get_time_fn` runs on
  tasklets, as legacy does, and from MS2a also on the wrapper's time thread (D-144, §2.8). The
  bridge is not part of `MTL_1.0` and leaves the public set with `mtl_init`
  ([migration.md](migration.md) §8.3), so the frozen rule has no exception. An opt-in inline
  notify on the completing context (`mtl_session_set_inline_notify`) is reserved for later
  (`MTL_LATER`): it is added only if busy polling (W0) does not close the latency gap to today's
  tasklet callbacks (D-04).
- Those library threads may not open, close or shut down the instance (close and shutdown join them): `-MTL_EDEADLK` (`LIBRARY_THREAD`).
- **Busy polling.** An application thread may poll the DP calls, and the WT calls with timeout 0, in
  a tight loop (W0, the lowest-latency wake-up, [core.md](core.md) §6): they never block or
  sleep, and a poller of the DP calls, or of a queue without a descriptor, causes no wake-up
  syscall. The library's own busy loops (scheduler loops, the RX packet lcore) run no application
  code, so no public call is ever made from one; user schedulers and tasklets are cut (D-112).

### R7 Options

Tuning knobs are options (`mtl_options.h`): absent means the documented default, present is literal. §12.

### R8 The process

- The library installs no signal handler and registers no `atexit`. The exceptions are DPDK's own SIGBUS handlers: held while DPDK grows its heap (MTL allocates pools at create, so only then), and installed by the option `instance.hotplug`.
- A signal handler may call the AS functions only, at any time.
- Framework plugins (GStreamer, FFmpeg) and codec plugins install no handlers either: the host application owns signals. The library never calls `exit`. The application turns SIGTERM into work on an ordinary thread (self-pipe, `signalfd`, or a flag; ex11 uses the handler plus `mtl_instance_interrupt`). Nothing may rely on SIGTERM arriving: the design is correct for SIGKILL.
- Every descriptor MTL or DPDK opens for it is close-on-exec.
- An instance belongs to the process that opened it. In a `fork()`ed child MTL at once closes the descriptors it tracks (VFIO, MtlManager, CPU locks, and the queue descriptors, which are otherwise kept while the process runs, §7.2).
  Every call then returns `-MTL_EBADF` (reason `FORKED`) except close, which drops local state only. Library memory is `MADV_DONTFORK`. Imported regions are the application's.
- Everything MTL holds outside the process (device DMA, queues, CPUs, kernel programs, MtlManager grants) is tied to a descriptor the kernel closes at exit, or reconciled at the next open. Nothing is found by PID. A SIGKILL at any instant therefore leaves nothing that blocks a restart (§2.5).

### Object verbs

A verb that several objects have is one exported function over `struct mtl_object`, with `static inline` typed wrappers that keep each object's own name, so programs read the same:

| Verb | Objects | Typed wrappers |
|---|---|---|
| `mtl_close(o, timeout_ns)` (CP) | instance, session, region, plugin, queue (closes at once) | `mtl_instance_close`, `mtl_session_close`, `mtl_mem_close`, `mtl_plugin_unload`, `mtl_queue_close` |
| `mtl_interrupt(o, mode, targets)` (AS; CP with `MTL_INTR_OFF`) | instance, session, queue (targets 0); `MTL_INTR_ABORT` instance only, targets 0 | `mtl_instance_interrupt`, `mtl_instance_abort`, `mtl_session_interrupt`, `mtl_queue_interrupt` |
| `mtl_wait(o, mask, timeout)` (WT) | session; instance (`MTL_WAIT_EVENTS` only) | `mtl_session_wait` |
| `mtl_reap(o, rec, rec_size, max, timeout)` (WT) | TX session | `mtl_tx_reap` (`struct mtl_tx_result`), `mtl_tx_reap_full` (`struct mtl_tx_result_full`, `mtl_observe.h`) |
| `mtl_read_events(o, ev, ev_size, max, timeout)` (WT) | session, instance | `mtl_session_read_events`, `mtl_instance_read_events` |

- `mtl_release(s, lease)` (DP) is not an object verb: it takes a session and a lease, and is the one export behind `mtl_tx_release` and `mtl_rx_release` (§5.4).
- A kind without the verb is `-MTL_EINVAL`. The rules of a verb for one kind are stated at its wrapper and in the section of that object; this file names the wrappers.
- The record wrappers take a typed pointer and pass `sizeof(*r)`; the exported `mtl_reap` and `mtl_read_events` keep `rec_size`, so a larger record version is filled further.
- `timeout_ns` of `mtl_close` bounds the instance and session closes only; the other kinds never wait.
- Created timelines (`mtl_timeline_h`) are reserved for later (`MTL_LATER`); they are not objects of these verbs in v1.

**Objects and ownership.** The picture below shows what each object owns. The instance owns its
ports, sessions and regions. A session owns its flows (one per leg), its pool of slots and its
slot table. A lease is access to one slot, now; a slot itself is named by its index (R4). An
attached pool lies over a region (dotted arrow, §9.3).

```mermaid
flowchart TB
    I["mtl_instance_h<br/>ports, schedulers,<br/>time source, events;<br/>one reference per open<br/>if SHARED"]
    I --> P["ports 0..n<br/>link, pacing class,<br/>time state"]
    I --> S["mtl_session_h<br/>one essence,<br/>one direction,<br/>unique name,<br/>the SMPTE epoch"]
    I --> R["mtl_region_h<br/>memory MTL may DMA,<br/>refcounted"]
    S --> F["flows 0..1 = legs<br/>admin, oper,<br/>flow state"]
    S --> PL["pool: slots by index<br/>library or attached"]
    S --> LT["slot table and<br/>descriptor ring,<br/>results, events,<br/>status, stats,<br/>its event word"]
    PL --> L["mtl_lease_h<br/>access to one slot, now"]
    PL -.->|"attached over"| R
```

## 2. Instance

### 2.1 Open

`mtl_instance_open(p, &mt)` (CP) opens the ports of `struct mtl_instance_params` (legacy `mtl_init`).

| Field | Rule; zero means |
|---|---|
| `port_count`, `ports` | the ports, copied at open. `p` NULL or no port: the environment variable `MTL_PORTS` lists them, `"0000:af:01.0=192.168.1.10/24,0000:af:01.1=192.168.2.10"` or `"null:1"`. No port at all: `-MTL_EINVAL` naming `"ports"`. In a set-uid process `MTL_PORTS` and `env:` ports are `-MTL_EINVAL` |
| `lcores` | CPU IDs `"2-5,8"`; NULL = the opening thread's affinity mask (the pod's cpuset). A CPU outside the mask: `-MTL_EINVAL`, `CPU_NOT_ALLOWED`, before EAL starts |
| `time_source` | `enum mtl_time_source`; 0 = AUTO: `CLOCK_TAI` if its kernel offset is set, else `SYSTEM_TAI` (estimated), chosen once at open; from MS6 a disciplined NIC PHC first. Built-in PTP runs only when named (`PTP_BUILTIN`): on a PF MTL owns it disciplines the PHC, on a VF only MTL's software time base ([timing.md](timing.md) §2.2) |
| `flags` | `MTL_INSTANCE_SHARED`, `MTL_INSTANCE_TASKLET_THREAD` (schedulers are pinned pthreads, not EAL lcores), `MTL_INSTANCE_TASKLET_SLEEP` (idle schedulers sleep), `MTL_INSTANCE_HW_TIMESTAMP` (sessions may then require NIC timestamps) |
| `options`, `option_count` | instance keys, copied at open; session keys here are defaults for the instance's sessions (§12.4) |

**Port names** (`struct mtl_port_spec.name`):

| Form | Backend |
|---|---|
| PCI BDF `"0000:af:01.0"` | DPDK PMD |
| `"kernel:<ifname>"` | kernel socket |
| `"native_af_xdp:<ifname>"` | native AF_XDP |
| `"null:<n>"` | no NIC, no root, no hugepages: units complete on the instance clock; the substrate of unit tests and examples (G-92). An RX session receives the TX units of the same instance whose flow (destination IP and UDP port) it matches: loopback without packets |
| `"env:<VAR>[#n]"` | the n-th (from 0) PCI address in the environment variable, as the SR-IOV device plugin sets `PCIDEVICE_<resource>`; `-MTL_EINVAL`, `PORT_ENV_UNSET`, if absent |
| `"dpdk_af_xdp:"`, `"dpdk_af_packet:"` | removed (D-112): open fails with `-MTL_ENOTSUP`, `BACKEND_REMOVED`, and `detail` names the replacement (`native_af_xdp:`, `kernel:`) |

Port spec defaults: `ip_family` 0 = IPv4 (bytes 0..3, the rest zero); `prefix_len` 0 = 24 (IPv6: 64); `sip` all zero = DHCP on kernel backends; `gateway` all zero = none; `tx_queues`, `rx_queues` 0 = auto; `numa` 0 = the device's socket, else node + 1. `mac` is output only. **MTL never sets a port's MAC**: a VF keeps the one its PF or CNI gave it.

**Fail fast.** Every check that needs no device runs before any device is touched. A misconfigured pod fails at once with one reason and a one-line `mtl_last_error().detail` fit for a termination message:

| Check | Reason on failure |
|---|---|
| each `env:` port resolves | `PORT_ENV_UNSET` |
| the device node exists in the container, is accessible, and is bound to vfio-pci | `DEVICE_NODE_MISSING`, `DRIVER_MISMATCH`, `CAPABILITY_MISSING` |
| an IOMMU is present (not no-IOMMU, not PA mode), unless `instance.allow_noiommu` | `NO_IOMMU` |
| hugepages within the **cgroup** hugetlb limit and the free pages | `HUGEPAGES_LIMIT` |
| `RLIMIT_MEMLOCK` covers the pools, or `CAP_IPC_LOCK` is effective | `MEMLOCK_LIMIT` |
| the capabilities the backend needs, in the effective set | `CAPABILITY_MISSING`, naming it |
| the CPUs: inside the affinity mask; busy polling under a CFS quota (`instance.cpu_shared`) | `CPU_NOT_ALLOWED`, `CPU_SHARED` |
| `instance.runtime_dir`, when set, is writable | `RUNTIME_DIR` |
| MtlManager, when the configuration needs it (AF_XDP on a shared netdev) | `MANAGER_REQUIRED` |
| AF_XDP: the socket or map from a node daemon (`port.xsk_map`) | `XSK_UNAVAILABLE` |
| the named time source can be read; `PTP_BUILTIN` is not asked to steer a PHC MTL does not own (a VF runs it on the software time base instead) | `TIME_SOURCE_UNAVAILABLE`, `CLOCK_NOT_OWNED` |

- After the devices, open probes what each VF allows (trust, multicast filter budget, PF rate limit, pacing, PHC) into `caps.*` keys. A session that needs more fails at create: the first group past an untrusted VF's filter budget is `-MTL_ENOSPC`, `MCAST_FILTERS`.
- **Open does not wait** for links, neighbours, time lock or a DHCP lease. `mtl_instance_get_health()` reports them (§2.7). Units sent before lock carry `MTL_TXR_ESTIMATED`.
- **Ports in MS1.** `mtl_instance_open` opens `null:` and `kernel:` ports in MS1. A PCI port (a
  BDF, or an `env:` port, which names one) and a `native_af_xdp:` port are `-MTL_ENOTSUP`
  (`NOT_IMPLEMENTED`, field `ports`) until MS2a, checked before any device is touched, with `detail`
  `"<name>: opened from MS2a; until then mtl_init + mtl_instance_from_legacy (mtl_legacy.h)"` (at
  most 128 bytes with a 15-byte interface name). The bridge is the MS1 way onto a NIC
  ([examples.md §3.1](examples.md#31-ms1-on-a-nic-the-legacy-bridge)).
- A port with `port.dhcp` stays in `MTL_PHASE_LINKS` until it has a lease, and `MTL_EVENT_PORT_ADDRESS` reports it (today open waits up to 49 × 100 ms and then fails, `mt_dhcp.c:549-564`).
  Timesync start and TSC calibration (about 1 s each) stay inside open.
- Open assumes the previous run died uncleanly: it flushes the VF's flow rules and rate-limit configuration and logs what it reconciled (`instance.reconciled{kind}`).
- **Re-open.** After a close in the same process, open works again on the same ports and on a subset of the first open's CPUs (EAL keeps its first arguments). A port quarantined by an earlier close is `-MTL_EBUSY` (`QUEUE_QUARANTINED`) until the process exits.
- **Null-only instances and the open order.** An instance whose ports are all `null:` never calls
  the legacy init: it initialises EAL itself (no hugepages, no PCI, in memory) or joins an EAL the
  process already initialised, and runs one library thread as its loop instead of pinned schedulers.
  EAL is initialised once per process, so after a null-only instance initialised it, an instance
  that names a NIC port fails open with `-MTL_EEXIST` (`INSTANCE_MISMATCH`, `detail` naming EAL) for
  the life of the process. A process that needs both opens the NIC instance first; a NIC instance
  may also carry `null:` ports.
- Files: MTL writes nothing to the filesystem unless `instance.runtime_dir` is set; it is then the only place. Details: [deployment.md](deployment.md).

### 2.2 Ports

- A port is an index into the instance's ports. `mtl_port_find(mt, name, &port)` (inline, over `mtl_port_get_spec`) finds one by name, BDF or IPv4 address. A component that joined a shared instance with a subset of its ports finds their indices with it (§2.3).
- `mtl_port_get_spec()` returns the port as granted: the address, prefix and gateway in use (a DHCP lease included) and the MAC on the wire. A changed address posts `MTL_EVENT_PORT_ADDRESS`.
- A port's capabilities, status and counters are the stats keys `caps.*`, `port.*`, `time.*`, `nic.*` (§11).
- A port whose link is down at open is opened anyway, with its links reported down.

**`MTL_PORTS` grammar.** The environment variable is read only when `p` is NULL or names no
port, and is `-MTL_EINVAL` in a set-uid process. Open parses it with `mtl_port_parse()`, the
exported parser, so applications and plugins with a ports property or argument use the same
grammar.

```text
ports := [ws] port { [ws] "," [ws] port } [ws]
port := name [ "=" address [ "/" prefix ] [ "@" gateway ] ]
name := bdf | kernel | xdp | null | env
bdf := a PCI BDF, for example "0000:af:01.0"
kernel := "kernel:" ifname
xdp := "native_af_xdp:" ifname
null := "null:" id
env := "env:" VAR [ "#" n ]
address := IPv4 dotted quad | "[" IPv6 "]"
gateway := an address of the same family
prefix := 1–32 (IPv6: 1–128); absent = 24 (IPv6: 64); 0 is invalid (it would read as the default)
id := 0–255, a label: each "null:" token is one null port
ws := spaces and tabs
```

- A name is at most 63 bytes and holds none of ',', '=', '@' or whitespace. An interface whose
  name holds one is opened through `mtl_instance_params.ports`.
- Unknown prefixes are copied for open to judge (`BACKEND_REMOVED`, `-MTL_EINVAL`).
- **Normalised names.** A name is stored normalised, and duplicates are found on the normalised
  form:
  - a BDF in lower case with its domain (`AF:01.0` and `af:01.0` become `0000:af:01.0`);
  - a null ID in decimal without leading zeros (`null:01` becomes `null:1`);
  - `kernel:` and `native_af_xdp:` names as written.
- A duplicate name, an empty port (`",,"` or a trailing ',') or an invalid address or prefix is
  `-MTL_EINVAL`, field `ports`, with detail giving the port's position and its token.
- Without an address, `sip` stays zero, and open applies the port-spec rule:
  - DHCP on the kernel backends, `kernel:` and `native_af_xdp:`;
  - `-MTL_EINVAL` (naming the port) on a PCI port without `port.dhcp`;
  - null ports need none.
- `env:VAR#n` may appear inside `MTL_PORTS`; open resolves it (`PORT_ENV_UNSET`).
- A port's index is its position. When `p` names ports, `MTL_PORTS` is ignored, never merged.

### 2.3 Shared instance

- A process has at most one instance per port set (one EAL per process). Components share a port
  through `MTL_INSTANCE_SHARED` (MS2a); a second open of a port already open in the process without
  it is refused with `-MTL_EEXIST` (`INSTANCE_MISMATCH`).
- With `MTL_INSTANCE_SHARED` the first open creates the process-wide instance and later opens join
  it. A wrapper of a legacy instance is the process-wide instance too (§2.8): a shared open after
  `mtl_instance_from_legacy` joins the wrapper.
- Each open returns its own reference handle: `MTL_SAME` is false between two, and every call
  accepts any live reference.
- **What a joining open may ask.** A field it leaves zero is not a request, and settings are never
  merged between opens:
  - **ports**: its ports (from `p`, or from `MTL_PORTS` when `p` names none) must be a subset of
    the live instance's, matched by normalised name (§2.2). An address, prefix, gateway or `numa`
    that it sets must equal the live port's. An open with no port, and no `MTL_PORTS`, joins with
    every port. A port keeps the instance's index, whatever its position in the request, so a
    component finds its own with `mtl_port_find` (MS2, which lands with the shared open);
  - **lcores, time source, flags other than `MTL_INSTANCE_SHARED`, options**: equal to the live
    instance's, or zero;
  - anything else is `-MTL_EEXIST` (`INSTANCE_MISMATCH`), with the field at fault. For a missing
    port the detail is `"port <name> is not in the instance; list it in MTL_PORTS or the first
    open"`.
- **On a wrapper** the live settings are the legacy ones. A joining open names ports and
  `log_level` only; a non-zero `lcores`, time source, instance flag or instance key is
  `INSTANCE_MISMATCH` (detail `"the instance is a wrapper of a legacy mtl_init"`).
- Components that share an instance (framework elements, plugins) leave `lcores` and the port queue
  counts zero and put what is theirs in session keys (`session.tx_queue`, `session.migrate`), so
  their opens never disagree; each session takes its queues at create (`-MTL_ENOSPC`,
  `CAPACITY_*_QUEUES`, when none is left).
- `mtl_instance_close` on a reference that is not the last only drops that reference and returns 0
  (G-77). On a wrapper, the last reference's close retires the wrapper and leaves the legacy
  instance running.
- `mtl_instance_shutdown(mt, MTL_SHUTDOWN_ALL_REFERENCES, …)` shuts the instance down for every
  component in the process (GStreamer elements, an FFmpeg device, the application). The application
  that owns `main()` uses it on SIGTERM; a plugin never does. The other references then get
  `-MTL_ESHUTDOWN` and their close returns 0. On a wrapper it ends every unified session; the legacy
  devices stop at `mtl_uninit`.
- **Legacy and unified code in one process.** The legacy instance opens first and is wrapped. A
  legacy `mtl_init` in a process where a unified instance initialised EAL fails, as a second
  `mtl_init` does today (it returns NULL, `dev/mt_dev.c:498-501`). A component that keeps legacy
  paths beside unified ones holds one legacy instance and gives its unified code references through
  `mtl_instance_from_legacy` or a shared open (the FFmpeg plugin from MS3 to MS6,
  [migration.md](migration.md) §6.2, §11.4).
- A queue created through one reference (`mtl_queue_create(mt, …)`) belongs to the underlying
  instance: `mtl_queue_arm` accepts an object of that instance through any reference, and the queue
  lives until its own close or the instance's.

### 2.4 Close and shutdown

`mtl_instance_close(mt, timeout_ns)` (CP; `mtl_close` on the instance) drops this reference. The last reference shuts the instance down within `timeout_ns`, network first; calling close again on the same reference polls (0 or 1, R4). `mtl_instance_shutdown(mt, flags, timeout_ns, &report, size)` (in `mtl_observe.h`, MS3) does the same with flags and a report.

| Step | What | Bounded by |
|---|---|---|
| 0 | New data calls get `-MTL_ESHUTDOWN`; health shows `SHUTTING_DOWN`, so readiness fails. Calls already inside a data call are waited for: every session counts its calls in flight | the deadline |
| 1 | TX: the unit whose first packet left is sent to its end at its pace and gets its normal status; the wire never carries a partial unit (rows units end by `tx.rows_late`, STALL as TRUNCATE). Queued units become `MTL_TX_FLUSHED` with reason `CLOSE`. With `MTL_SHUTDOWN_DRAIN` every queued unit is sent instead, until the deadline minus what the later steps need | the deadline |
| 2 | RX: an IGMP/MLD leave on every leg before the queues close; incomplete units are discarded and counted | never waits on the network |
| 3 | The codec threads are joined; the log thread when no sink and no other instance remain, after this instance's queued lines reached the sinks | the deadline |
| 4 | Schedulers, queues and ports stop; flows are destroyed; imported regions are unmapped. A queue that will not complete runs the stalled-queue steps ([engine.md](engine.md)), a reset only if the budget remains (`caps.reset_budget_ns`), otherwise it is quarantined | the deadline |
| 5 | MtlManager grants are returned by closing the connection; after step 4, so no grant is returned while still in use | socket close |
| 6 | Library memory is freed, except slots under a lease the application still holds | — |

- Queues, sessions and regions still open are closed by the shutdown: its queues are closed first (§7.2), then its sessions. Their handles stay safe (R4): data calls return `-MTL_ESHUTDOWN`, blocked waits wake with it, `mtl_session_close` returns 0, and `mtl_rx_release`/`mtl_tx_release` of a lease taken before return 0.
- To send queued TX units, close the sessions first: `mtl_session_close` drains (§4.9). Instance close flushes them.
- `timeout_ns` 0 means abort semantics: no drain. `MTL_FOREVER` still bounds every step by its own budget. No step starts that its remaining budget cannot finish.
- From a library thread (log sink, codec) the call returns `-MTL_EDEADLK` and has no effect.

| Return | Meaning | Application |
|---|---|---|
| 0 | retired: every object freed, every library thread joined | exit, or open again |
| 1 | quiesced: no device can reach any memory, but leases or regions are still referenced, or a library thread is still in application code past the deadline (counted; its memory is kept) | exit; or return the leases. A slot's memory is freed by the process's next control-plane call after its last lease returns, or at exit |
| `-MTL_EIO`, reason `QUEUE_QUARANTINED` | a port could not be stopped; nothing it can reach is freed | exit now; the kernel's VFIO release stops the device |

- `-MTL_ETIMEDOUT` is never used for the unsafe case.
- `struct mtl_shutdown_report` (filled whatever the call returns) counts each step: `sessions_closed`, `sessions_retiring`, `units_flushed`, `results_discarded`, `groups_left`, `leases_out`, `regions_referenced`, `ports_unquiesced` (bit per port), `threads_unjoined`, `bytes_kept`, and `references_left` > 0 when only this reference was dropped. `summary` is one line for a termination message.
- **Abort.** `mtl_instance_abort(mt)` (AS; legacy `mtl_abort`; a second SIGTERM) interrupts every wait and stops every TX session at its next packet: the cut unit is `MTL_TX_FLUSHED`, reason `ABORTED`, with `MTL_TXR_PKT_SHORT`; queued units are `FLUSHED`/`ABORTED`. During close it skips to the hard stop; after close it does nothing. Close still has to follow.
- **Interrupt.** `mtl_instance_interrupt(mt, 1)` (AS) makes every data wait of every session return `-MTL_ECANCELED` until `mtl_instance_interrupt(mt, 0)` (CP). Stop and close still work (§7.3).
- **The budget counts from SIGTERM.** The signal handler records the time and calls `mtl_instance_interrupt(mt, 1)`; the main thread passes `grace − preStop − margin − time spent`. ex11 is the recipe ([examples.md](examples.md)).
- **Signals in the recipe.** Block SIGTERM and SIGINT before open and before any thread is created; install the handlers after open; then unblock. A signal arriving during open stays pending.
- **The AS path is safe after close.** The interrupt flags live in the instance's never-freed handle slot, and MTL never closes a queue's descriptor while the process runs (§7.2), so a late signal never writes into a recycled descriptor.

### 2.5 What holds after each kind of ending

| Resource | Orderly close, then exit | SIGKILL at any instant | Device removal, process keeps running |
|---|---|---|---|
| TX session | unit on the wire finished, rest `FLUSHED`; every accepted unit has a result | the wire stops at descriptor release; a partial frame may be on the wire | ERROR `DEVICE_GONE`, or a degraded 2022-7 leg; waits `-MTL_ENODEV` |
| RX session | leave sent on every leg | no leave: the switch keeps the group until the querier ages it out (about 260 s with IGMP defaults) | ERROR or degraded leg |
| application lease | release returns 0; the slot is freed after its last lease | gone | release still works |
| library pools | freed slot by slot as leases come back | freed by the kernel | not freed under a lease |
| imported regions | unmapped before `mtl_mem_close` returns 0 | IOMMU unmap after DMA is off | the removed port's mapping goes with its VFIO descriptor |
| no-IOMMU mode | as orderly | **no guarantee: DMA may hit freed pages**; refused unless `instance.allow_noiommu` | no guarantee |
| CPUs | nothing to release in a pod (the cpuset is the lease); MtlManager grants on socket close; OFD locks released | released by the kernel or at manager socket EOF | — |
| VF queues, rate-limit nodes, flow rules | stopped and destroyed | FLR at descriptor release; flushed again at the next open | gone with the device |

The residual effects of a SIGKILL, none of which blocks the next start: the stale IGMP membership; a partial frame; XDP state and `tx_maxrate` while MtlManager itself is down; a PHC frequency offset left by the built-in PTP client on a PF it owns (impossible on a VF, where it steers only MTL's software time base). Details: [deployment.md](deployment.md) §4.5.

### 2.6 Device removal and reset

| Event | MTL | The application sees |
|---|---|---|
| reset (a PF reset resets every VF) | the admin worker resets the port, restores queues and flows, re-joins groups; a bounded device step that counts as worker progress | `MTL_EVENT_PORT_RESET`; sessions report `MTL_EVENT_RECOVERY` and resume; units in the gap are `MTL_TX_FAILED`, reason `PORT_RESET` |
| removed, or a reset that fails | the port becomes removed; its legs go oper-down. A 2022-7 session with a surviving leg continues degraded; one with no leg left enters ERROR (`DEVICE_GONE`) | `MTL_EVENT_PORT_REMOVED`; health `MTL_HEALTH_DEGRADED`, and `MTL_HEALTH_SESSION_LOST` only when a started session has no leg left or every port is gone; calls on a session with no leg return `-MTL_ENODEV` |
| close after removal | skips every device step for that port | 0 |

The library never closes the application's sessions on removal; they stay closeable in ERROR.

TX units in a port-reset gap are `MTL_TX_FAILED`, reason `PORT_RESET` ([deployment.md](deployment.md) §4.6), not the TX-hang outcome (`MTL_TX_DROPPED`, `RECOVERY`, §4.10).

### 2.7 Health

`mtl_instance_get_health(mt, &h, size)` (DP) returns the `MTL_HEALTH_*` flags; `h` may be NULL.
It is lock-free from any thread at any rate, even when the control plane is wedged, and returns a
consistent snapshot, never torn (`detail` is "" if it changed while being copied). During close
it reports `MTL_HEALTH_SHUTTING_DOWN` and masks `MTL_HEALTH_SCHED_STALLED`; after close it
returns `-MTL_ESHUTDOWN`, the one exception to R4's `-MTL_EBADF`: the handle slot is never freed, so the answer stays defined. `MTL_EVENT_HEALTH` reports every change.

| Bit | Liveness | Readiness | Set when |
|---|---|---|---|
| `MTL_HEALTH_SCHED_STALLED` | yes | yes | a scheduler loop is older than `instance.stall_ns` (default 1 s). The heartbeat advances on wake-ups and timer ticks, so a sleeping idle scheduler is not stalled. `MTL_EVENT_SCHED_STALLED` warns at 1/8, 1/4 and 1/2 of the limit |
| `MTL_HEALTH_WORKER_STALLED` | yes | yes | the admin worker missed its heartbeat outside a bounded device step (a port reset is progress) |
| `MTL_HEALTH_SESSION_LOST` | yes | yes | a started session lost every leg to removed ports, or every port is gone |
| `MTL_HEALTH_DEVICE_FAULT` | yes | yes | a queue was quarantined; its memory is never freed |
| `MTL_HEALTH_STARTING` | | yes | the phase is below `MTL_PHASE_READY` (`MTL_PHASE_LINKS`, `MTL_PHASE_TIME`) |
| `MTL_HEALTH_NO_LINK` | | yes | no port has a link |
| `MTL_HEALTH_TIME_UNLOCKED` | | yes | time state ACQUIRING or LOST; FREERUN and HOLDOVER are ready |
| `MTL_HEALTH_MANAGER_LOST` | | yes | MtlManager is gone (AF_XDP stops receiving) |
| `MTL_HEALTH_SHUTTING_DOWN` | | yes | close or shutdown has begun |
| `MTL_HEALTH_DEGRADED` | | | information only: a port's link is down or removed, or a started session is in ERROR or has an enabled leg not resolved or joined |

- `MTL_HEALTH_LIVENESS` and `MTL_HEALTH_READINESS` are the two masks.
- **Liveness never depends on packets, links or PTP lock**, so a grandmaster outage or a switch reboot never restarts a pod; a degraded 2022-7 session is not a liveness failure.
- The library never kills anything; the orchestrator decides. The application serves the probes: startup and liveness 503 until open returns, then on any liveness bit; readiness 503 on any readiness bit; after shutdown began, liveness 200 and readiness 503.
- `struct mtl_health` also carries `phase`, `reason` (of the first bit set), `time_state`, port, scheduler and session counts, `oldest_loop_age_ns` and `time_error_ns`.

### 2.8 Legacy bridge

`mtl_instance_from_legacy(legacy, &mt)` (CP, `mtl_legacy.h`, MS1) wraps a legacy `mtl_handle`,
so legacy and unified sessions share one instance; there is no call in the other direction (the
application that bridges already holds the legacy handle). The wrapper's ports are the legacy
ports in their order (port 0 is `MTL_PORT_P`). From MS2a the wrapper is the process's shared
instance (§2.3): another `mtl_instance_from_legacy` of the same handle returns another reference
(`-MTL_ENOTSUP`, `NOT_IMPLEMENTED`, in MS1), and a shared open joins it. `mtl_instance_close()`
on the wrapper closes the unified sessions, queues and regions made through it, but never stops the
legacy instance's devices or legacy sessions; `mtl_uninit()` does that. Details:
[migration.md](migration.md).

- **Teardown order:** close the wrapper (`mtl_instance_close`) first, then `mtl_uninit()`.
- While a reference to a wrapper of the instance has not retired (its close has not returned 0) `mtl_uninit()`
  returns `-EBUSY` and stops nothing; the two calls exclude each other. Today `mtl_uninit` with live sessions self-deadlocks (SP-01, [engine.md](engine.md)); the legacy teardown order is fixed as a bugfix.
- **Before the legacy `mtl_start()`.** On a bridged instance, create works at any time; a start before the legacy `mtl_start()` is `-MTL_EBUSY` (`WRONG_STATE`), because no tasklet runs until it (`dev/mt_dev.c:2113-2125`). RxTxApp and KahawaiTest start the legacy instance first (or set `MTL_FLAG_DEV_AUTO_START_STOP`).
- **Time.** The wrapper runs on the legacy clock of port P, which its legacy sessions pace on; the
  video TX binding's index math reads the same function on the tasklet (the R6 exception, D-144).
  `mtl_time_now` stays DP. MS1: the default clock and `MTL_FLAG_PTP_SOURCE_TSC` are read
  directly (VALID, ESTIMATED, UTC); a `ptp_get_time_fn` or the built-in PTP client is
  `-MTL_ENOTSUP` until MS2a. From MS2a: a two-record seqlocked offset to `CLOCK_MONOTONIC`,
  refreshed every 5 ms by a wrapper library thread (created by `mtl_instance_from_legacy`, joined
  by the wrapper's close), with the flags and states of the table below; a record older than
  100 ms adds ESTIMATED. TAI is not slewed until E9.
  - **The wrapper's core clock in MS1.** On a wrapper, the core's instance-context clock (C0,
    [core.md](core.md) §2.4) is `mt_get_ptp_time(impl, MTL_PORT_P)`:
    - the core's launch decision (`st_core_admit`, from the video TX binding's tasklet) reads it
      on the tasklet;
    - the core's CP calls that need "now" (the start's A = now + lead) read it on the calling
      thread, as legacy `mtl_ptp_read_time` does;
    - no DP call reads it, so `mtl_time_now` is the only "now" an application reads, and on a
      wrapper it is the direct read or `-MTL_ENOTSUP` of the MS1 table.
  - **TAI on a wrapper.** The offset record makes MONOTONIC and REALTIME exact. TAI follows the
    legacy clock and is not slewed. At a refresh it moves by at most age × |rate(legacy) −
    rate(MONOTONIC)|, ≤ 2.5 µs at 5 ms and 500 ppm, forward or backward, plus any step of the
    legacy clock itself (G-126).
  - **MS1 classification order**, the order of `dev/mt_dev.c:2280-2290`:
    1. `MTL_FLAG_PTP_SOURCE_TSC`: the TSC row. It stays TSC even with `MTL_FLAG_PTP_ENABLE`,
       because the PTP client then skips the switch (`mt_ptp.c:1062-1063`).
    2. Else `ptp_get_time_fn`: `-MTL_ENOTSUP` (MS1).
    3. Else PTP service enabled: `-MTL_ENOTSUP` (MS1); it will switch to `ptp_from_eth`.
    4. Else the default row.
  - `mtl_instance_from_legacy` returns after the TSC calibration (about 1 s after `mtl_init`).

| Legacy clock | Flags | `time.state` | `time.source` |
|---|---|---|---|
| default or TSC | VALID, ESTIMATED, UTC | FREERUN | LEGACY |
| `ptp_get_time_fn` within 1 s of `CLOCK_REALTIME` at wrap time | VALID, ESTIMATED, UTC | FREERUN | LEGACY |
| other `ptp_get_time_fn` | VALID, ESTIMATED; VALID after `mtl_time_set_reference` with `locked` = 1 | FREERUN; LOCKED | LEGACY |
| PTP service on, before the first Announce | VALID, ESTIMATED, UTC | ACQUIRING | PTP_BUILTIN |
| `ptp_from_eth`, not locked | VALID, ESTIMATED | ACQUIRING | PTP_BUILTIN |
| `ptp_from_eth`, locked, a master result within H | VALID | LOCKED | PTP_BUILTIN |
| `ptp_from_eth`, locked, no master result for H | VALID, ESTIMATED, HOLDOVER | HOLDOVER | PTP_BUILTIN |

- **Options** on a wrapper:

| Class | MS1 | MS2a (C-BRIDGE) |
|---|---|---|
| instance keys marked C | `mtl_set_option` `-MTL_EBUSY` (`OPTION_STATE`), the general rule after open; `mtl_get_option` `-MTL_ENOTSUP` | `mtl_get_option` returns the legacy value where migration.md §4.2/§4.3 maps one, else `-MTL_ENOTSUP` |
| instance keys marked R | `-MTL_ENOTSUP` on a wrapper | through the legacy setters, legacy sessions included: `instance.sched_sleep_us` scope 0 → `mtl_sch_set_sleep_us` + `mtl_sch_enable_sleep`; every other R key `-MTL_ENOTSUP` |
| session keys and typed defaults set on the instance | accepted (the general §12.4 rule) for later unified sessions | the initial defaults come from the legacy settings: `pacing` → `caps.pacing`; `pkt_udp_suggest_max_size` → the instance default of the typed `sc.max_udp_payload` (a session's 0 takes it) |
| session keys and typed defaults, continued | | `SHARED_TX_QUEUE` → `session.tx_queue = MTL_TXQ_SHARED`; the migrate flags → `session.migrate`; the source-port flags → `session.src_port_mode`; `TX_NO_CHAIN` → `caps.tx_copy` |

## 3. Session configuration

### 3.1 One typed config

- `struct mtl_session_config` describes one stream of one essence in one direction. It is typed only: enums and defines, no spec strings (D-97).
- `MTL_INIT(&sc)`; set `direction`, `essence`, `flows[0]` and the essence's required fields; everything else defaults.
- The member for `essence` is read (`sc.video`, `sc.cvideo`, `sc.audio`, `sc.anc`, `sc.fastmeta`, `sc.rtp`; plus `sc.packet` when `unit = MTL_UNIT_PACKETS`). The other essence members must stay zero: `-MTL_EINVAL`, `OTHER_ESSENCE`.
- A required field left zero is `-MTL_EINVAL`, `FIELD_REQUIRED`, and `mtl_last_error().field` names it.
- Enums follow the legacy order: legacy + 1 where 0 means "not set" (`mtl_fps`, `mtl_video_format`, `mtl_audio_format`, `mtl_ptime`, `mtl_app_format`), the same values where 0 is a real default (`mtl_packing` = `st20_packing`, `mtl_sender_type` = `st21_pacing`). The field map from the legacy ops is in [migration.md](migration.md).

### 3.2 Common fields

| Field | Rule; zero means |
|---|---|
| `direction` | `MTL_TX` or `MTL_RX`; required |
| `essence` | `MTL_VIDEO`, `MTL_CVIDEO`, `MTL_AUDIO`, `MTL_ANC`, `MTL_FASTMETA`, `MTL_RTP` (generic RTP, packet units only); required |
| `unit` | `MTL_UNIT_FRAME` (0), `MTL_UNIT_ROWS` (video), `MTL_UNIT_PACKETS` (§13); fixed for the session's life |
| `name` | unique per instance (`-MTL_EEXIST`, `NAME_EXISTS`, against live and closing sessions); "" = generated `<essence>_<tx or rx>_<n>`. Copied at create; in `mtl_session_info`, the log prefix and every event's `origin_name` (G-88); kept by `mtl_session_update()`. Re-create: below |
| `flows[MTL_MAX_LEGS]` | `flows[0]` required; a `flows[1]` that exists is the ST 2022-7 leg (§14) |
| `flags` | `MTL_SESSION_*` (§3.4) |
| `pool_count` | 0 = by essence: video TX `max(min_count_direct, 3)`; video RX n + 2 (below the table), so 3 with one leg and 4 with two; the others 4; the limit is `mtl_session_info.max_count` (8 for video and cvideo until engine change E11) |
| `legs_disabled` | a bit per existing leg (others `-MTL_EINVAL`): admin down (§14.2). A reserved leg and every existing leg set (mute) are Phase 7, `-MTL_ENOTSUP` until then |
| `media_mode` | `enum mtl_media_mode`; 0 = `MTL_MEDIA_AUTO`, the next feasible index ([timing.md](timing.md) §4.1). Every session runs on the SMPTE epoch |
| `ssrc` | TX: 0 = one random SSRC for the session, the same on every leg (ST 2022-7); RX: 0 = no check, else the SSRC every leg must carry |
| `tsmode` | TX: what the session claims for the SDP's `TSMODE` (`info.tsmode`); 0 = no claim, an SDP without `TSMODE`, which receivers read as NEW (ST 2110-10 §8.7); see §12.6's notes for which claim a producer makes. RX: 0, else `-MTL_EINVAL` |
| `max_udp_payload` | bytes of RTP per packet. TX: 0 = `instance.max_udp_payload`, else 1452; signalled as `MAXUDP` = this + 8 only above 1452. RX: the sender's `MAXUDP` − 8; 0 = no `MAXUDP` in its SDP, so 1452 (§13.2) |
| `payload_type` | TX: 0 = the essence default (video and cvideo 112, audio 111, ANC 113, fastmeta 115), the same on every leg; else a dynamic type 96–127 (ST 2110-10 §6.2, ST 2110-41 §5.2; `MTL_RTP`: 1–127), otherwise `-MTL_EINVAL`; RX: 0 = no check |
| `min_tx_delay_ns` | TX: the earliest send after the media time. 0 = playback (content exists before its media time); a capture producer (camera, encoder, RX → TX) sets one frame period plus the pick-up lead, a launch delay of 1 ([timing.md](timing.md) §5.2) |
| `media_time_offset_ns` | TX, TAI mode: the producer's declared latency, which moves the index; 0 = none. RTP stays the index's `floor(N × period × rate)`, so the ST 2110-10 §7.6.3 ±TFRAME bound does not apply. AUTO and INDEX: 0, else `-MTL_EINVAL`; the legacy RTP trim is `tx.rtp_trim_ns` ([timing.md](timing.md) §4.5) |
| `options`, `option_count` | session keys, deep-copied (§12) |

The video RX default `pool_count` is n + 2, n the units in reception at once: 1 with one leg,
min(the engine's out-of-order slots, ceil(F / U) + 1) with two (F the effective
`rx.flush_offset_ns`, U the unit period; [engine.md](engine.md) §2.14).

To keep an identity across a re-create, close and create with the same name. A closing session
still owns its name, so that create is `-MTL_EEXIST` until the old session is RETIRED (call
`mtl_session_close` again, with a timeout, until it returns 0).

**Flow fields** (`struct mtl_flow`, fixed size, one per leg):

| Field | Zero means |
|---|---|
| `port` | the leg's own instance port (leg i → port i); else `MTL_INDEX(port)`; every leg on ports of one kind, the null ports or the NIC ports (`-MTL_EINVAL`, `INVALID_ARGUMENT`, naming `flows[i].port`) |
| `ip_family` | IPv4 in bytes 0..3; 6 = IPv6 |
| `ip` | TX destination; RX group (multicast), or the port's own address or all zero (unicast) |
| `source_filter` | RX source-specific multicast; all zero = none; with unicast RX it checks the sender |
| `udp_port` | required; a leg with `udp_port` 0 does not exist (a reserved leg is Phase 7, §14.1) |
| `udp_src_port` | TX: `udp_port`; RX: any |
| `dscp` | TX: into the IP TOS; 0 = CS0. Implemented with the bindings: video in MS1, the other essences in MS4. On the kernel-socket backend, where MTL writes no IP header, it is set with `setsockopt(IP_TOS)`. The IPMX profile's defaults (AF41 audio, AF42 others) and `MTL_FLOWF_DSCP_LITERAL` are Phase 7 |
| `ttl` | 64 |
| `dst_mac` | used only with `MTL_FLOWF_USER_MAC` (no ARP) |
| `vlan` | reserved, 0 |

A flow carries no RTP identity: SSRC and payload type are the session's (`sc.ssrc`,
`sc.payload_type`), the same on every leg. `mtl_flow_ipv4(&f, a, b, c, d, udp_port)` fills an IPv4
flow (legacy `dip_addr[i]`, `udp_port[i]`); `mtl_flow_parse(&f, "239.1.1.1:20000")`, with an
optional `@source` for a source-specific group, sets `ip`, `udp_port` and `source_filter` from text
(IPv4 only; `-MTL_EINVAL` for anything else). The granted values (SSRC, source port, payload type,
DSCP, TTL, MACs) are in `mtl_session_info.leg[]`; `dst_mac` is zero while the neighbour is
unresolved.

### 3.3 Essence members

| Member | Required | Zero means |
|---|---|---|
| `video` (`struct mtl_video_config`) | `raster` (with `detect` ON: the maximum); `format` or `app_format` | `format` 0 = from `app_format`; `app_format` 0 = the transport format itself, no conversion; `packing` 0 = `MTL_PACKING_BPM` (the legacy default); `sender_type` 0 = `MTL_SENDER_N` (below); `detect` 0 = `MTL_DETECT_AUTO`, off for video; `linesize[]` 0 = packed (library pools) |
| `video`, colour and timing | — | `colorimetry` 0 = UNSPECIFIED (still rendered: SDP requires it), `tcs` 0 = SDR, `range` 0 = narrow (SDP and conversion metadata, never on the wire); `troffset_us` 0 = TRODEFAULT (below) |
| `cvideo` | `raster`, `codec`, `codestream_bytes` (CBR bytes, or the VBR ceiling, per unit) | `rate_mode` 0 = `MTL_CVIDEO_CBR`; `app_format` 0 = the application gives the codestream, else a codec plugin encodes; colour as video; no TROFFSET (ST 2110-22 §5.3: no virtual receiver buffer); `sender_type` shapes the network model only: N on the ST 2110-21 §6.3.1 formats, NL elsewhere (MS4b, D-143) |
| `audio` | `format`, `sample_rate`, `channels` | `ptime` 0 = 1 ms; `unit_samples` 0 = 10 ms in whole packets (RX unit, TX pool capacity) |
| `anc` | `video.fps` (and scan), unless a video session is in its start | `video` all zero = the raster and launch delay of the first video session of its start (`-MTL_EINVAL`, `FIELD_REQUIRED`, if none); `max_packets`, `max_udw_words`, `word_mode` and `timing_model` below; `detect` 0 = `MTL_DETECT_AUTO`, on for ANC (interlace from the F bits) |
| `fastmeta` | as ANC; with `MTL_FASTMETA_FREE_RUNNING` the own rate in `video.fps`, always required | `buffer_capacity_bytes` 0 = 64 KiB (bounds the whole unit, below); RX filters on `data_item_type` and `k_bit` only with `MTL_FASTMETA_RX_MATCH_DIT` / `MTL_FASTMETA_RX_MATCH_K` |
| `rtp` | `clock_rate` (ST 2022-6: 27000000) | `profile` 0 = `MTL_RTP_LINEAR`; `unit` = unit rate and scan, all zero = no unit rate (§13.6); `bitrate_bps` 0 = derived; `encoding` is the SDP rtpmap name; `"SMPTE2022-6"` adds the rules of §13.6; `troffset_us` as video's, with a unit rate (ST 2022-8 §6, §7.1, §8.1), else 0 |
| `packet` | — | §13.2 |

- `video.format` 0 takes the natural transport of `app_format` from a fixed table, not a guess:
  `MTL_APP_V210`, `MTL_APP_Y210`, `MTL_APP_YUV422P10LE` → `MTL_YUV422_10`; `MTL_APP_UYVY`,
  `MTL_APP_YUV422P8` → `MTL_YUV422_8`; `MTL_APP_YUV420P8`, `MTL_APP_NV12` → `MTL_YUV420_8`;
  `MTL_APP_RGBA`, `MTL_APP_BGRA`, `MTL_APP_RGB8` → `MTL_RGB_8`; `MTL_APP_YUV444P10LE` →
  `MTL_YUV444_10`; `MTL_APP_GBRP10LE` → `MTL_RGB_10`; an RFC 4175 pixel-group app format → its own
  transport. Both 0 is `-MTL_EINVAL`, `FIELD_REQUIRED`.
- ANC `timing_model` (TX): 0 = CTM pacing with no `TM` in the SDP, which receivers read as CTM;
  `MTL_ANC_CTM` or `MTL_ANC_LLTM` is signalled as `TM` (ST 2110-40 §6.4, §6.5, §7). RX: 0.
- ANC capacity (TX and RX): `max_packets` 0 = 32, 1–65 535 (OI-78); `max_udw_words` 0 = max(255,
  32 × `max_packets`), 1 024 at the default, at most 255 × `max_packets`; a unit's entries use at
  most `max_udw_words` words in all, a shared word counted once per entry that uses it. Captions,
  timecode, AFD and VPID need about 4 packets and 110 words a frame; a relay of a whole SDI ANC
  space, HDR dynamic metadata or long SCTE-104 messages raise both. `word_mode` 0 =
  `MTL_ANC_WORDS_8BIT`. With an attached pool
  `sc.pool_count` is required (`FIELD_REQUIRED`). TX: `sc.max_udp_payload` 348–1452,
  `tx.underrun_policy` not SKIP, and create refuses a capacity the link cannot carry in the
  window, 2·C_cap > TFRAME − 3·TLINE − ε (`-MTL_EINVAL`, field `anc.max_packets`;
  [timing.md](timing.md) §9.2).
- Audio `format`, `sample_rate` and `channels` stay required on purpose: a wire format that silently defaults is a receive-garbage bug, and 48 kHz / L24 / 2 channels is common, not universal.
- `struct mtl_raster`: any width and height up to 32767 (ST 2110-20 §7.2) and a frame rate
  `fps` that is an exact rational (interlaced: frames, not fields).
  - `{0, 0}` means not set: `FIELD_REQUIRED` where the rate is required (video, cvideo,
    free-running fastmeta); for `anc.video` and `fastmeta.video` the start's video (their other
    raster fields must then be 0); for `rtp.unit` no unit rate (its scan must be progressive);
    with `video.detect` ON, `{0, 0}` is the engine maximum, 120 frames/s, for the quota.
    Exactly one zero term is `-MTL_EINVAL`.
  - The library reduces the fraction to lowest terms; the limits apply to the reduced value:
    num ≤ 4194303 and den ≤ 1023, the widths of the rate fields of the IPMX Media Info Block
    (VSF TR-10-2 §10), else `-MTL_EINVAL` naming the field, with the limits in the detail. They
    keep num × den below 2^32, which makes every rate conversion exact in 64-bit integers
    ([timing.md](timing.md) §3.5).
  - With `MTL_INTERLACED` or `MTL_PSF`, a reduced fps above 30 frames per second
    (`num > 30 × den`) is `-MTL_EINVAL`, reason `FIELD_RATE`, naming the field. Legacy `fps` and
    names like 1080i59.94 count fields; every interlaced and PsF format of ST 125, ST 274 and
    BT.709 runs at 30 frames per second or less *(inferred: ST 274 and BT.709)*. ST 2110-20 sets
    no such limit: this is MTL's guard against the field-rate trap, not a conformance rule. Ports
    use `mtl_raster_from_legacy()` (`mtl_legacy.h`).
  - The reduced value is what `mtl_session_info.raster` and the SDP carry (an integer when den is
    1, ST 2110-20 §7.2). A named rate is `mtl_fps_rational(MTL_FPS_*)`.
  - Capability (`-MTL_ENOTSUP`, `NOT_IMPLEMENTED`): 1 to 120 frames per second for every scan.
    Video takes the rates of the legacy `enum st_fps` in MS1 (interlaced: the frame rates whose
    field rate is one of them) and any rate in that range from MS2a; fastmeta any rate from
    MS4a1, ANC from MS4a2; cvideo from MS4b, with JPEG XS only n/1 and n·1000/1001 (`-MTL_EINVAL`).
- `enum mtl_scan`: `MTL_PROGRESSIVE`, `MTL_INTERLACED` (the unit is a field), `MTL_PSF` (paced as interlaced, one index per frame).
- **RX format detection** (`video.detect` ON, MS3; D-155). `raster` is the maximum:
  - width ≤ `raster.width` and height ≤ `raster.height` (the frame height: interlaced and PsF
    frames count both fields), which size the pool;
  - frame rate ≤ `raster.fps`, in frames, compared as reduced rationals, which sizes the
    scheduler quota with the size (`{0, 0}`: 120 frames per second);
  - any scan; `raster.scan` must be `MTL_PROGRESSIVE`.

  `video.format` (or `app_format`) is required and never detected. Detection measures width (SRD
  offsets and the format's pgroup), height, scan and packing (engine.md E14), and the rate with
  the detector of [timing.md](timing.md) §11.9. A rate matching no candidate is published with
  `fps_approx` = 1.

  Pool, RX queue and quota are granted for the maximum at create, for the session's life.
  Choose the real ceiling: a 2160p59.94 maximum reserves 4 × 1080p59.94 of a scheduler's 12, so
  a 16-input multiviewer of 1080 sources pins about 6 schedulers instead of 2, and 4 × 20.7 MB
  of hugepages per session, twice that with conversion.

  **Publication.** Every format within the maximum is published without application action:
  the first after each start, and each change.
  - Its first unit carries `MTL_UNITF_FORMAT_CHANGED`.
  - `mtl_rx_detail` reports `format_seq` (1 at each start, + 1 per published format), `raster`,
    `packing` and `fps_approx`, so a held unit's format is never read from stats.
  - Planes are laid out for that format (§9.1).

  **Losses are not changes.** While a format is published, an interval of m periods counts m − 1
  missing units, and a packet outside the geometry is rejected
  (`rx.pkts_rejected{cause=offset}`).

  Re-detection starts on any of:
  - packets outside the geometry, or a contradicting F bit, in two consecutive complete units
    (marker seen, no sequence gap);
  - a smaller measured geometry in two consecutive complete units;
  - eight consecutive intervals with no single-period interval (the rate fell);
  - two consecutive intervals that are no whole number of periods (the rate rose).

  Those units are dropped (`rx.units_format_dropped{cause=straddle}`). The next format is
  published after the detector's window ([timing.md](timing.md) §11.9):
  - geometry changes: from 10 units to about 20 with skipped frames;
  - a rate that fell: about 16 intervals;
  - an approximate rate: 2 + 32 frames.

  **When nothing is published.** Dequeue returns `-MTL_EAGAIN`, with:
  - `MTL_STATUS_RX_DETECTING` while measuring;
  - `MTL_STATUS_FORMAT_CHANGED` with `format_reason` `RASTER_MISMATCH` (above the maximum) or
    `DETECT_FAILED` (the geometry is inconsistent).

  Publication resumes by itself when the stream fits again. `MTL_UPDATE_MEDIA` in STOPPED
  changes the maximum.
- With detect OFF, `raster` is the rate and size expected. A stream that differs is
  `MTL_STATUS_FORMAT_CHANGED` (`RASTER_MISMATCH`), with the measured values in `rx.detected.*`.
- Formats outside RFC 4175 (`MTL_YUV422P10LE_NONSTD`, `MTL_V210_NONSTD`) are kept for existing deployments and set `MTL_INFO_NON_COMPLIANT`.
- **4:2:0 is progressive only** (ST 2110-20 §6.2.5): a 4:2:0 `format` with `MTL_INTERLACED` or `MTL_PSF` is `-MTL_EINVAL`, naming `video.format`.
- **No KEY transport format.** Colorimetry ALPHA belongs to KEY sampling (ST 2110-20 §6.2.6, §7.4.1), which `enum mtl_video_format` does not carry, so `MTL_COLOR_ALPHA` is declared only under `MTL_LATER` and the value 8 is `-MTL_EINVAL` (an unknown value, R1) until a KEY format exists.
- **Sender types are granted** (D-143, from MS2a; MS1 sends every type as the legacy API does,
  reports no type, and sets `MTL_INFO_NON_COMPLIANT` for NL, for N off the §6.3.1 formats and for
  interlaced or PsF N). The gapped schedule exists only for the formats of ST 2110-21 §6.3.1: 720 ×
  480/486 interlaced at 30000/1001; 720 × 576 interlaced at 25; 1920 × 1080 interlaced at 25,
  30000/1001, 30 and PsF at 24000/1001, 24, 25, 30000/1001, 30; 1280 × 720 and 1920 × 1080
  progressive at 24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60; 3840 × 2160 and 7680 ×
  4320 progressive at those and 100, 120000/1001, 120. 1080p at 100, 120000/1001 and 120 is not a
  §6.3.1 format: N there is W until MS5, then NL, and MS1 flags it non-compliant (OI-76). Create
  grants a type, reported in `info.sender_type` for the SDP's `TP=` ([timing.md](timing.md) §5.1):

  | Requested | §6.3.1 format, before E5a | other format, before E5a | §6.3.1 format, from E5a (MS5) | other format, from E5a |
  |---|---|---|---|---|
  | N (0) | N; interlaced and PsF with the TLINE cap below | **W**, emitted as N (`ops.pacing = NARROW`, N's VRX0) | N | NL |
  | NL | N (every receiver type obliged to take NL is obliged to take N, ST 2110-21 §7.2.3–§7.2.5) | `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, "NL needs the linear schedule, MS5") | NL | NL |
  | W | W (today's W emission) | W (today's W emission) | W (linear) | W (linear) |

  Until MS5, W is sent on the gapped schedule and checked against the W model (ST 2110-21 §6.5,
  §6.6.2, §7.1.4) with this bound and these inputs:

  ```text
  VRX0 + ceil((1 − RACTIVE_e)·NPACKETS) + bulk ≤ VRXFULL_W  and  2·VRX0 + bulk + 1 ≤ VRXFULL_W
  ```

  - NPACKETS: per frame as the engine builds them (`st20_total_pkts`, BPM by default; interlaced:
    both fields);
  - VRXFULL_W = MAX(INT(1500 × 720 / MAXUDP), INT(NPACKETS / (300 × TFRAME))), MAXUDP 1500 under
    the Standard UDP Size Limit, else the signalled value (§7.1.4);
  - RACTIVE_e: the engine's emission ratio (1080/1125; 487/525 for 480 lines, 576/625 for other
    heights ≤ 576, interlaced), not the §6.3.3 value;
  - VRX0: after the bulk and RL compensation, or `video.start_vrx` when present;
  - bulk: 4 (the pacing way is known only after RL training).

  The binding lowers VRX0 through `ops.start_vrx` when needed and fails `-MTL_ENOTSUP` when
  VRX0 = 1 does not meet the bound. At or above 900 000 packets/s, W is judged against NL's CMAX,
  which `info.cmax` reports and the SDP carries as `CMAX=`. A W stream near that threshold can fall
  on either side of it by packing: 4096 × 2160p50 is 972 k packets/s with GPM and 878 k with BPM;
  `info.cmax` follows the packet rate as built. Interlaced and PsF N cap VRX0 by TLINE/2 until
  E5b, with k = ceil(TLINE / (2 × TRS)) (= ceil(NPACKETS/2160) at 1125 lines):

  1. cap = VRX_N − 3 − k. If cap ≥ 1, `ops.start_vrx = cap`.
  2. Else (8 642–9 718 packets at 1080i50, up to 9 876 at 1080PsF23.98/24, where VRX_N is the
     floor of 8 and k is 5) the binding sets `ST20_TX_FLAG_DISABLE_BULK` (TSC bulk 1) and
     `ops.start_vrx = VRX_N − 1 − k` (2 there). The −1 keeps the RL burst of 2 inside VRXFULL.
  3. If that is still < 1, `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, "needs E5b's TLINE/2"). No
     1125-line format at 23.98–30 frames per second reaches this case.

  `start_vrx` is never 0, which the engine reads as "unset". A present `video.start_vrx` or
  `video.disable_bulk` is not overwritten: the binding checks it against these steps and the W
  bound, and a value they refuse is `-MTL_EINVAL`, `OPTION_RANGE`, naming the key, with the cap in
  `detail` (D-172). SD interlaced N sets
  `MTL_INFO_NON_COMPLIANT` until E5b (OI-77). Where an interlaced or PsF W or NL grant has no
  Table 1 system, `info.troffset_default` is 0 and the SDP carries `TROFF`. cvideo: N on the
  §6.3.1 formats, NL elsewhere (ST 2110-22 §5.3).
- **`troffset_us`** (video, and generic RTP with a unit rate): 0 = TRODEFAULT, or, where ST 2110-21
  defines none (an interlaced or PsF linear grant outside its Table 1 systems, §6.4), the library's
  offset, signalled as `TROFF` from `info.troffset_ns` ([timing.md](timing.md) §5.1). Otherwise whole
  microseconds with TROFFSET below TFRAME, else `-MTL_EINVAL` naming the field: TROFF is signalled
  as a positive integer of µs and is mandatory when TROFFSET is not the default (ST 2110-21 §6.2,
  §8.2; ST 2022-8 §7.1), so TROFFSET 0 cannot be offered, and microseconds make an unsignallable
  value impossible to write. TX sends and signals it; RX takes it as the sender's TROFF for the
  timing parser (§6.5). The first packet still
  respects VRX0 ≤ floor(TROFFSET / TRS) ([engine.md](engine.md)). `info.troffset_ns` reports the
  value in use.
- **Audio samples.** Plane 0 of an audio unit holds the samples as on the wire: interleaved by
  channel, each sample in network byte order (big-endian; L16 two bytes, L24 three, AM824 the
  4-byte subframes). MTL copies the bytes into the packets and out of them and never swaps them,
  as today's engine does (`st_tx_audio_session.c:496`, `:530`; `st_rx_audio_session.c:510`).
- **Audio.** `MTL_AM824` counts AES3 subframes in `channels`, two per AES3 signal, so an odd count
  is `-MTL_EINVAL` (ST 2110-31 §6.1). `MTL_PTIME_80US` is the ST 2110-31 0.08 packet time: 4 samples
  at 48 kHz (8 at 96 kHz), a packet every 83⅓ µs (ST 2110-31 Table 1; the legacy engine sends one
  every 80 µs, SF-35); an AM824 SDP writes the Table 1 strings, `0.12` for `MTL_PTIME_125US` and
  `0.08`. `MTL_PCM8` (L8) sets `MTL_INFO_NON_COMPLIANT`: ST 2110-30 takes the AES67 formats L16 and
  L24 *(inferred: AES67 not read)*.
- **Fastmeta.** A unit goes out as data items of at most 359 words (1436 B), one per RTP packet,
  because a data item lies wholly inside one packet under the 1460 B Standard UDP Size Limit (ST
  2110-41 §5.4; the 9-bit length would allow 511 words); `used` 0 sends one RTP packet with no data
  item, the keep-alive (§5.1). A session carries one `data_item_type` and `k_bit`: several item
  types in one stream (§5.1) and Annex A segmentation are not offered (packet units carry them). The
  payload type is dynamic, 96–127 (§5.2), and the marker is 0 on every packet. A unit of several
  packets leaves within the §7 burst limit: at most CMAX = 4 packets back to back, then one per
  TDRAIN, 1.25 ms below 727 packets/s.

### 3.4 Session flags

| Flag | Meaning |
|---|---|
| `MTL_SESSION_RESULTS` | results for a library pool (application memory always has them, §6.1) |
| `MTL_SESSION_POOL_ATTACHED` | slots come from `mtl_session_attach` (§9.3) |
| `MTL_SESSION_REQUIRE_DIRECT` | fail at create if any unit would be copied (§9.7) |
| `MTL_SESSION_RX_BY_INDEX` | RX slot = media index mod `pool_count` (§9.8) (frame and rows units; `-MTL_EINVAL` on packet units, [core.md](core.md) §2.2) |
| `MTL_SESSION_RX_LATEST` | RX: a full pool reclaims the oldest unread unit (frame and rows units; `-MTL_EINVAL` on packet units, [core.md](core.md) §2.2) |
| `MTL_SESSION_RX_NO_FILL` | RX, library pools: do not zero what lost packets left out. Attached pools are never zero-filled (§9.8) (frame and rows units; `-MTL_EINVAL` on packet units, [core.md](core.md) §2.2) |
| `MTL_SESSION_MT_SUBMIT` | several threads submit (acquire is MP-safe without it) |
| `MTL_SESSION_EXACT_LAUNCH` | TX: the session admits `MTL_SUBMIT_EXACT` units; its other units start at their launch index's first-packet time. Without it, `MTL_SUBMIT_EXACT` is `-MTL_EINVAL` (§5.2) |
| `MTL_SESSION_TX_SRC_PLANES` | TX, converting library pools: every submit carries `MTL_SUBMIT_SRC_PLANES`, so MTL allocates no app-format planes (the pool holds only the transport frames); acquire gives the layout with null plane addresses; a submit without the flag is `-MTL_EINVAL` (`INVALID_ARGUMENT`, field `"flags"`); on any other session create is `-MTL_EINVAL` (MS2; §5.2, §9.1) |

A framework pool whose slots several threads submit sets `MTL_SESSION_MT_SUBMIT`; one whose only submitter is the sink's `render` needs neither it nor results ([migration.md](migration.md) §12.4). Reaps and dequeues are MP-safe on every session: each data call counts itself in flight, and stop and close always drain that count.

### 3.5 Query and info

- `mtl_session_query(mt, &sc, flags, &info, size, &req, size)` (CP) is a dry run of create: it validates and grants without allocating. `info` and `req` may be NULL; `req` gives the layout an attached pool needs (§9.4). ANC and fastmeta rasters taken from a start read 0 until then.
- `MTL_QUERY_CHECK_CAPACITY` also checks free capacity and runs the real placement without reserving: `-MTL_ENOSPC` with the limiting resource as reason (`CAPACITY_*`). If it succeeds and nothing else changes the host, the create succeeds (G-89).
- The limits a controller runs into today: 18 schedulers per instance (`MT_MAX_SCH_NUM`, `mt_main.h:49`); 60 video TX and 60 video RX sessions per scheduler (`ST_SCH_MAX_TX_VIDEO_SESSIONS`, `st_header.h:33-34`);
  a data quota per scheduler (`instance.sched_quota_mbs`, default 12 × the 1080p59.94 4:2:2 10-bit bandwidth, `ST_QUOTA_TX1080P_PER_SCH`, `st_header.h:23`; `mt_sch_add_quota`, `mt_sch.c:1060`); TX and RX queue counts fixed when the port is configured; the rate-limited queues of each port. Today a create that finds no scheduler fails with an `err` log only ("no free sch", `mt_sch.c:378`, `:1171`).
- The `capacity.*` and `port.free_*` keys are a snapshot: another creator can take the capacity first. A reservation object (hold capacity for N sessions, then create into it) is later; no milestone carries it.
- `mtl_session_get_info()` (CP) returns the granted configuration: `pacing_class` (`enum
  mtl_pacing`), `pool_count`, `max_count`, `leg_count`, `pkts_per_unit`, `unit_samples`,
  `buffer_capacity_bytes`, `meta_capacity`, `codestream_bytes` (in whole packets), `raster` (the
  granted raster, fps reduced; ANC and fastmeta from their start; with `video.detect` ON the
  configured maximum, fps `{0, 0}` included), `sched_index`, `unit_bytes`, `pool_slot_pitch`,
  `created_tai_ns`, `leg[]`, and `wire_bps`, the bandwidth of one leg on the wire, headers
  included (legacy `st20_get_bandwidth_bps`; the dry run fills it too).
- It also gives `min_submit_lead_ns` (submit at least this long before the media time; negative
  when the deadline falls after it, [timing.md](timing.md) §6.1), `convert_ns` and `flags`.
  - `convert_ns` is the caller's work per unit inside dequeue or submit: conversion, and on TX
    without conversion the copy of `MTL_SUBMIT_SRC_PLANES`. Create calibrates it with one unit's
    work; from then on it is the largest measured over the last second; query reports 0.
  - `flags`: `MTL_INFO_NON_COMPLIANT` (also for the sender-type cases of §3.3),
    `MTL_INFO_RESULTS`: results are produced, `MTL_INFO_DIRECT`: no unit is copied,
    `MTL_INFO_JTNM_DEFAULT_WINDOW_EXCEEDED`: the launch delay puts the stream outside the JT-NM
    Tested default windows, `MTL_INFO_RTP_OFF_GRID`: a `tx.rtp_trim_ns` that is not a multiple of
    the index period puts RTP off the `N × period` grid ([timing.md](timing.md) §4.5, §5.2), plus
    `MTL_INFO_LATENCY_INFEASIBLE` (`latency_max_ns < latency_min_ns`), and
    `MTL_INFO_LEGS_SHARE_PORT`: both legs on one instance port (§14.1).
- **`latency_min_ns`, `latency_max_ns`** (D-156) are for framework latency queries, never
  negative, and recomputed by every `mtl_session_get_info` from the current format and options.
  U is the unit period: 1 / the rate in use, half for fields. That is the configured
  `raster.fps`; with detection, the current unit's `mtl_rx_detail.raster.fps`, and before the
  first unit the maximum's, with `{0, 0}` read as 120/1.
  - **RX min.**
    - With `rx.link_offset_ns` set, it is that offset (ST 2110-10's link offset, equal across a
      start group; later units carry `MTL_RXF_LATE_FOR_PRESENTATION`).
    - Otherwise it is S + U + F + W:
      - S, the sender's offset: video and cvideo, the RX TROFFSET (`troffset_us`; 0 =
        TRODEFAULT); ANC and fastmeta, their timing model's target delay; audio, one packet
        time;
      - F, the effective `rx.flush_offset_ns`;
      - W, `info.expected_wake_latency_ns`.
    - Rows units: S + the time of `rx.rows_step` rows + W. Packet units: U is
      `packet.rx_max_wait_ns`.
    - A sender with a launch delay arrives later: set `rx.link_offset_ns`.
    - With two legs F defaults to `rx.skew_budget_ns` (10 ms, ST 2022-7 class A), so a redundant
      receiver reports about 10 ms more than a one-leg one: that is how long it keeps a unit for
      the late leg, and it is what keeps the units of a failover of the leading leg on time
      downstream. A deployment whose path differential is smaller sets `rx.skew_budget_ns` to its
      bound (`leg.observed_skew_ns{window=60s}` shows the skew seen), which lowers F, the due time
      and this value together. F never comes from a measurement: a framework's latency must not
      change while it runs.
  - **RX max** = (`pool_count` − 1) × U.
    - A unit dequeued and released before its media time + max never makes the pool miss a unit.
      Units in assembly are inside the bound.
    - It assumes no packet arrives more than one unit period before its media time, which every
      ST 2110-21 sender on the epoch meets. It does not hold under `MTL_STATUS_TIMEBASE_SUSPECT`
      or with an `rx.rtp_offset` that does not match the sender.
    - With `MTL_SESSION_RX_LATEST` it is `INT64_MAX`.
  - **TX min** = max(0, `min_submit_lead_ns`), excluding submit's own work (`convert_ns`). **TX
    max** = `tx.horizon_ns`, the largest lead accepted; queueing more than `pool_count − 1`
    units blocks acquire.
  - Detect sessions use the detected format, and before it the maximum's rate.
  - With `MTL_INFO_LATENCY_INFEASIBLE`, a `pool_count` of ceil(`latency_min_ns` / U) + 1 makes
    it feasible. `mtl_rx_reserve(&info, NULL, 0, 0)` (`mtl_util.h`) returns that number. Add one for each unit a consumer keeps past its release (a GStreamer sink's last
    sample, an aggregator; [migration.md](migration.md) §12.8).
- The `info.*` keys (SDP timing values such as `info.trs_ps`, `info.troffset_ns`, `info.vrx_full`, and the transport, placement and wake diagnostics) exist only on a created session: the dry run returns the typed `struct mtl_session_info` but no keys. A controller that needs them before start creates the session (CREATED sends nothing) and reads them there; NMOS publishes the SDP after create.
- There is no configuration getter: the application keeps the `struct mtl_session_config` it created the session with, and `mtl_session_update()` reads only the members its `parts` name (§4.7). Options are read with `mtl_get_option()`.

### 3.6 Features per essence

What each essence gets in MS1–MS6 (yes, or the milestone), later, or not at all (—). Today's asymmetry is accidental; the target is the same verbs and outcomes for every essence. Each legacy mode's unified home and milestone: [coverage.md](coverage.md).

| Feature | video | cvideo | audio | ANC | fastmeta |
|---|---|---|---|---|---|
| frame units, ST 2022-7 legs, library and attached pools | yes | yes | yes | yes | yes |
| direct TX (any stride ≥ row) | yes | — (copy) | — (copy) | — (copy) | — (copy) |
| media modes and late policies | yes | yes | AUTO yes; TAI/INDEX MS6 | yes | AUTO yes; TAI/INDEX MS6 |
| `MTL_SUBMIT_NOT_BEFORE` / `MTL_SUBMIT_EXACT` | yes | yes | MS6 | MS6 | MS6 |
| underrun policy (`tx.underrun_policy`) default | SKIP | SKIP | SKIP; `MTL_UNDERRUN_SILENCE` as an option | `MTL_UNDERRUN_EMPTY_ANC` | `MTL_UNDERRUN_KEEPALIVE` |
| underrun REPEAT_LAST | later | later | later | later | — |
| results, events, one stats schema | yes | yes (no stats today) | yes | yes | yes |
| RX shape ([core.md](core.md) §2.2): frame units on core slots | yes | yes | yes | yes | yes (MS4a1; RTP only today) |
| `RX_BY_INDEX`, `RX_LATEST`, the due time, discard on frame units | MS2 | MS4b | MS4a1 | MS4a2 | MS4a1 |
| start arrays (MS6) | yes | yes | yes | yes | yes (on the grid like ANC) |
| pixel conversion (`app_format`) | yes | codec plugin | — | — | — |
| RX auto-detect | `sc.video.detect` (off) | — | — | `sc.anc.detect` (interlace, on) | — |
| RX timing parser (`rx.timing_parser`) | opt-in | — | opt-in | — | — |
| TX ST 2110-21 self-check | yes | network model only | — | — | — |
| RX DMA offload | yes (reported) | — | — | — | — |
| NACK retransmission (`rtx.*`) | yes | yes | — | — | — |
| rows units (`MTL_UNIT_ROWS`) | MS2 (progressive and interlaced) | — | — | — | — |
| packet units (`MTL_UNIT_PACKETS`, also `MTL_RTP`) | MS5 | MS5 | MS5 | MS5 | MS5 |
| header split | — (cut, D-112) | — | — | — | — |

Until MS6 a launch flag on audio, ANC or fastmeta is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, R1).
Packet units are ring-RX on every essence: `RX_BY_INDEX`, `RX_LATEST` and `RX_NO_FILL` are
`-MTL_EINVAL`, and a chunk is bounded by `packet.rx_max_wait_ns` (§13).

## 4. Lifecycle and states

### 4.1 States

| State | Meaning |
|---|---|
| `MTL_STATE_CREATED` | validated; queues, scheduler quota, slot table and flow-rule capacity reserved; nothing on a tasklet; nothing sent |
| `MTL_STATE_ARMED` | started, start instant ahead. TX accepts preroll; RX is joined and discards units whose media time is before the start instant |
| `MTL_STATE_RUNNING` | sending or receiving |
| `MTL_STATE_DRAINING` | `stop(DRAIN)`: queued units are still being sent |
| `MTL_STATE_FLUSHING` | `stop(FLUSH)` or a drain deadline: queued units are being flushed; units the device holds are not yet released (ERROR entry flushes inside ERROR, §4.8) |
| `MTL_STATE_STOPPED` | like CREATED, with history: slots attached, RX memberships and rules kept, counters kept; can start again |
| `MTL_STATE_ERROR` | the library cannot continue without the application: only stop and close leave it; data calls return the error status; release, reap, status, events, wait and interrupt still work (§4.2) |
| `MTL_STATE_CLOSING` | closed, retiring: leases or device references remain |
| `MTL_STATE_RETIRED` | closed and retired; the handle stays retired |

The picture below shows the machine. Inside the outer box are the states of an open session.
The inner box, *started*, holds the four states a start leads to: a start from CREATED or STOPPED
enters ARMED when its instant is ahead and RUNNING when it is now, and a drain or a flush ends in
STOPPED. A fatal fault in any started state enters ERROR, which only stop and close leave.
`mtl_session_close` from any open state is the one arrow out of the outer box: it enters CLOSING,
and the session ends in RETIRED (§4.9 shows the steps inside CLOSING).

<!-- BEGIN TABLE state-picture: lib/src/st2110/core/st_core_states.def; gen_api_doc.py writes this region once that file exists (README §1) -->

```mermaid
stateDiagram-v2
    state "open session" as OPEN {
        state "started" as STARTED {
            [*] --> ARMED: instant ahead
            [*] --> RUNNING: now
            ARMED --> RUNNING: start instant reached
            ARMED --> FLUSHING: stop
            RUNNING --> DRAINING: stop DRAIN
            RUNNING --> FLUSHING: stop FLUSH
            DRAINING --> FLUSHING: deadline missed,<br/>or stop FLUSH
            DRAINING --> [*]: every unit final
            FLUSHING --> [*]: device released<br/>every unit
        }
        [*] --> CREATED
        CREATED --> STARTED: start
        STOPPED --> STARTED: start
        STARTED --> STOPPED: stopped
        STARTED --> ERROR: fatal fault,<br/>in any started state
        ERROR --> STOPPED: stop, or the stop<br/>in progress
    }
    [*] --> OPEN: mtl_session_create
    OPEN --> CLOSING: close, from any state
    CLOSING --> RETIRED: last lease and<br/>device reference gone
    RETIRED --> [*]
```

<!-- END TABLE state-picture -->

`mtl_session_close` from ARMED, RUNNING or DRAINING first stops (DRAIN until the deadline, then FLUSH) inside CLOSING, then retires (§4.9). `mtl_session_get_status(s, NULL, 0)` (DP; the inline `mtl_session_get_state(s)`) returns the state as its result; with a status struct it gives the full picture (§4.10).

### 4.2 What each state allows

`-EBUSY` is `-MTL_EBUSY`, and so on; "yes" means the call does its normal work. A call not listed for ERROR fails with the session's error code (`status.error`, `-MTL_EIO` or `-MTL_ENODEV`) (G-49).

<!-- BEGIN TABLE state-calls: lib/src/st2110/core/st_core_states.def; gen_api_doc.py writes this region once that file exists (README §1) -->

| Call | CREATED, STOPPED | ARMED | RUNNING | DRAINING | FLUSHING | ERROR | CLOSING | RETIRED | closed by the instance |
|---|---|---|---|---|---|---|---|---|---|
| `mtl_session_attach` (NULL detaches), `mtl_set_options` (C, S keys) | yes | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` (stop first) | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_update` `MTL_UPDATE_MEDIA`, `MTL_UPDATE_POOL` | yes, applied during the call (MS3) | `-EBUSY` | `-EBUSY` (colorimetry, tcs, range: posted for the boundary, MS5; `-ENOTSUP` before) | `-EBUSY` | `-EBUSY` | `-EIO` | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_update` `MTL_UPDATE_FLOWS`, `MTL_UPDATE_LEGS`, R options | yes, applied during the call (MS3) | posted for the boundary (MS5; `-ENOTSUP` before) | posted for the boundary (MS5; `-ENOTSUP` before) | `-EBUSY` | `-EBUSY` | `-EIO` | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_start` | yes; from STOPPED after ERROR it re-reserves or fails `-ENODEV` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EBUSY` | `-EIO` (stop first) | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_stop` | 0 (no-op) | yes | yes | FLUSH escalates; a second DRAIN waits for the first | waits for the flush | yes → STOPPED | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_discard` | yes: queued preroll units `FLUSHED`/`DISCARD` | yes | yes | `-EBUSY` | 0 (no-op) | 0 (no-op) | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_tx_acquire`, `mtl_tx_acquire_slot` | yes (preroll) | yes | yes | `-ESHUTDOWN` | `-ESHUTDOWN` | error code | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_tx_submit` | yes, held until start | yes, held until the instant | yes | `-ESHUTDOWN` | `-ESHUTDOWN` | error code | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_tx_release`, `mtl_rx_release` | yes | yes | yes | yes | yes | yes | **yes** | — (no lease can be out) | 0; the last one makes it RETIRED (§4.9) |
| `mtl_tx_reap`, `mtl_session_read_events` | yes | yes | yes | yes | yes | yes | `-ESHUTDOWN` (unread results discarded) | `-EBADF` | `-ESHUTDOWN` |
| `mtl_rx_dequeue` | remaining ready units, then `-EAGAIN` | `-EAGAIN` / waits | yes | force-completed and ready units, then `-ESHUTDOWN` | ready units, then `-ESHUTDOWN` | ready units, then the error code | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_wait` | yes | yes | yes | yes | yes | yes | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_queue_arm(q, s, …)` | yes | yes | yes | yes | yes | yes | `-ESHUTDOWN` (detached) | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_interrupt` | yes | yes | yes | yes | yes | yes | 0 (no-op: close wins) | 0 (no-op) | 0 (no-op) |
| `mtl_session_get_status(s, NULL, 0)` (`mtl_session_get_state`) | state | state | state | state | state | state | `MTL_STATE_CLOSING` | `MTL_STATE_RETIRED` | `MTL_STATE_CLOSING` while a lease is out, then `MTL_STATE_RETIRED` |
| `mtl_session_get_status` with a struct | yes | yes | yes | yes | yes | yes | the state; the struct holds the last snapshot | the state; the struct holds the last snapshot | the state; the last snapshot, with `reason` `INSTANCE_SHUTDOWN` |
| `get_info`, stats | yes | yes | yes | yes | yes | yes | `-ESHUTDOWN` | `-EBADF` | `-ESHUTDOWN` |
| `mtl_session_close` | yes | yes | yes | yes | yes | yes | polls: 1 while retiring, 0 once retired | 0 | 0 |

<!-- END TABLE state-calls -->

- **A WT call that starts in CREATED, ARMED or STOPPED waits normally** and ends with `-MTL_EAGAIN`, so a concurrent dequeue during a stop/update/start cycle never reads as end of stream.
- A WT call that is already blocked when the session enters DRAINING, FLUSHING, ERROR or CLOSING is woken with the code of the table once nothing remains for it (G-30).
- `-MTL_ESHUTDOWN` only ever means "the application asked" (stop, discard, close, instance close). `-MTL_EIO` means the session failed; the reason is in the status. `-MTL_EAGAIN` means nothing yet (G-70).
- **A lease the application holds stays valid across stop and close.** Stop never takes memory the application is reading or writing; close retires only after the leases come back.
- **Results stay drainable** until close; close discards what is unread.
- After close only `mtl_session_get_status` (`mtl_session_get_state`), close itself (which polls), the release of leases taken before and `mtl_session_interrupt` (a no-op that returns 0) are defined on the handle; any other call gets the code of the CLOSING or RETIRED column (§8.4).
- **Closed by the instance** is the last column: a session that its instance's close or shutdown
  closed (§2.4, R4). It reads CLOSING until its last lease returns and RETIRED after, with
  `status.reason` `INSTANCE_SHUTDOWN`; data calls return `-MTL_ESHUTDOWN` instead of the CLOSING
  and RETIRED columns' codes, and close and release return 0. It is a condition of the handle
  entry (core.md §4.7), not a tenth state, and G-49 tests it like a state.

### 4.3 Create and open

- `mtl_session_create(mt, &sc, &s)` (CP) validates and reserves queues, scheduler quota, the slot table and flow-rule capacity; nothing runs on a tasklet and nothing is sent. Failures surface here, so start is fast. Flow-rule capacity is checked with `rte_flow_validate` and nothing is installed; the rule (and the RX join) comes at the first start.
- **A failed create leaves nothing behind**: no session, mapping, flow, scheduler quota or handle; `*out` is the null handle (G-32).
- **Create never waits for ARP or IGMP.** Neighbour resolution runs on a worker. An unresolved TX leg is `MTL_FLOW_WAITING_NEIGHBOUR` and its units are not sent on it (reason `WAITING_NEIGHBOUR`; `MTL_TX_DROPPED` when every leg is unresolved); resolution resumes sending without an API call. `MTL_FLOWF_USER_MAC` bypasses ARP (G-90).
- `mtl_session_open(mt, &sc, &s)` (inline) is create and start now: the whole setup of a stream that needs nothing else. On failure `*s` is the null handle, the created session is closed, and the failing step's error stays in `mtl_last_error()` (the close succeeds, so it does not write it, R1).
- A create whose configuration needs MtlManager (an lcore, an AF_XDP queue) while MtlManager is lost returns `-MTL_EBUSY`, reason `MANAGER_LOST`; running sessions continue (G-98).

### 4.4 Start

`mtl_session_start(s[], n, when, &t0)` (CP) starts n sessions, all or none (G-23).

- The sessions must share one direction, and with n > 1 no TX session may use `MTL_MEDIA_AUTO` (AUTO stamps whatever slot a unit gets and cannot be synchronised): `-MTL_EINVAL`, `START_SET_MIXED`.
- **Every session runs on the SMPTE epoch**: index k is at k × its index period from 1970 TAI (a frame or field for video and ANC, a sample for audio). T0 is resolved once, on the common grid of the sessions' index periods, at or after `when` (NOW: the first feasible instant). `t0` (may be NULL) receives T0.
- Without `MTL_WHEN_ORIGIN` the media indices stay epoch indices and T0 is the first unit's media time. With `MTL_WHEN_ORIGIN` in `when->flags`, index 0 of every session started here is at T0, so a file's frame and sample counts are media indices as they are; it changes the numbering only, never a media time or an RTP timestamp. `MTL_WHEN_ORIGIN` on RX or with `MTL_AT_INDEX` is `-MTL_EINVAL`.
- `when` NULL means now. `struct mtl_when`: `kind` (`MTL_NOW`, `MTL_AT_TAI`, `MTL_AT_INDEX`: a media index of `s[0]`), `flags` (`MTL_WHEN_*`), `value`, `preroll_ns` (extra lead before the first unit).
- ANC and fastmeta sessions with an all-zero video raster take the raster and the launch delay of the first video session of the array.
- Milestones: n = 1 with `when` NULL or `MTL_NOW` in MS1; `MTL_AT_TAI` and `MTL_AT_INDEX` (ARMED) in MS3; start arrays (n > 1) and `MTL_WHEN_ORIGIN` in MS6. Until then each is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, R1).
- Start re-checks what can have changed since create (pool complete, layouts, device resources after an ERROR) and either reaches ARMED/RUNNING or fails with nothing changed.
- A session cannot start until its pool is complete and validated: with `MTL_SESSION_POOL_ATTACHED` and `pool_count` set, `-MTL_EINVAL`, `POOL_TOO_SMALL`, until that many slots are attached (G-35).
- If a preroll unit lies beyond the horizon measured from the resolved start instant, start fails atomically with `-MTL_ERANGE`, `BEYOND_HORIZON`, and no session starts (G-94). Preroll units whose media time precedes the start instant become `MTL_TX_FLUSHED`, reason `BEFORE_START`.
- **RX:** `when` is the earliest media time delivered. The first start installs the flow rule and sends the join on a worker, not awaited: `MTL_EVENT_FLOW_STATE` reports `MTL_FLOW_JOINED` or `MTL_FLOW_JOIN_FAILED`. Rule and membership are kept across stop until close or an update.
- RX in ARMED: every unit with media time before the instant is discarded and counted (`rx.units_before_start`), never delivered (G-76).
- Start posts the attach to the scheduler as a command and never runs on a tasklet. The T0 formula and the common grid are in [timing.md](timing.md) §3.

### 4.5 Stop

`mtl_session_stop(s[], n, mode, timeout_ns)` (CP) stops n sessions; arrays may mix directions; the sessions can be started again. `mode` is `enum mtl_stop_mode`:

- `MTL_STOP_DRAIN` (0): every queued unit is sent at its launch index.
- `MTL_STOP_FLUSH`: queued units are `FLUSHED` (`STOP_FLUSH`); a unit whose first packet left is sent to its end at its pace and gets its normal status.
- `-MTL_ETIMEDOUT` if DRAIN missed the deadline: the rest became `FLUSHED`, reason `STOP_TIMEOUT`. Units already handed to the device get their result when the device releases them, and the session stays FLUSHING until then.
- Stop and discard are immediate commands: a TX unit waiting up to 1 s for its launch does not delay them. RX stop ends assembly at once; rule and membership stay, so a later start is instant, and packets that arrive while the session is STOPPED (or CREATED after an update) are dropped by the queue, never delivered.
- A slot a codec or conversion plugin is converting belongs to the plugin: stop FLUSH, discard and
  abort wait for that conversion, so the flush is bounded by the plugin's conversion time (the
  transform state's claim and done on the slot, D-100; today's pipeline reclaim CAS CONVERTED →
  FLUSHED is safe against the builder's pick-up CAS the same way, `st20_pipeline_tx.c:210-213`). Every command is acknowledged within one tasklet iteration, even
  with no unit and no packet; an ack not received within the fixed ack timeout (D-48) puts the session in ERROR,
  reason `CMD_TIMEOUT`; no call waits forever (G-80). The acknowledged commands come with the
  session's `tick` in MS2.
- After stop returns 0, or `-MTL_EIO` for a fault during the drain or flush (§4.8), every accepted unit has a terminal outcome (G-31).

**Stop and flush outcomes**:

| | `mtl_session_discard` | stop DRAIN | stop FLUSH | ERROR entered |
|---|---|---|---|---|
| State after | stays (RUNNING, ARMED, CREATED, STOPPED) | STOPPED | STOPPED | ERROR, then stop → STOPPED |
| New TX submits | accepted after the call; the next is an implicit DISCONTINUITY; with `MTL_DISCARD_REBASE` `first_index` maps to the next feasible index | `-ESHUTDOWN` | `-ESHUTDOWN` | the error code |
| Queued, not picked up | `FLUSHED`/`DISCARD` | sent in order at their launch indices | `FLUSHED`/`STOP_FLUSH` | `FLUSHED`/`SESSION_ERROR` |
| Picked up, no packet sent yet | `FLUSHED`/`DISCARD` | sent | `FLUSHED`/`STOP_FLUSH` | `FLUSHED`/`SESSION_ERROR` |
| First packet left | completes | completes | sent to its end at its pace, normal status (rows units: `tx.rows_late`, STALL as TRUNCATE) | `FAILED`/reason (for example `TX_QUEUE_FATAL`) once every device reference is gone |
| Deadline reached | — | the rest `FLUSHED`/`STOP_TIMEOUT`; `-ETIMEDOUT` | — | — |
| RX unit being assembled | dropped, counted (`rx.units_flushed`) | force-completed and delivered with its status | discarded, counted | discarded, counted |
| RX ready, not dequeued | discarded, counted (`rx.units_flushed`) | stays dequeuable | stays dequeuable | stays dequeuable |
| Blocked callers | unaffected | woken with `-ESHUTDOWN` once nothing remains | woken with `-ESHUTDOWN` | woken with the error code |
| Returns | 0 | 0, or `-ETIMEDOUT` | 0 | — (`MTL_EVENT_SESSION_STATE` with the reason) |

Discard drops the RX unit being received and the units that are ready but not dequeued, and counts them (`rx.units_flushed`), so the first dequeue after a seek is a new unit, which a GStreamer source's `FLUSH_STOP` expects. A unit already dequeued stays the application's lease. Stop DRAIN, stop FLUSH and ERROR keep the ready units dequeuable.

**Close and abort outcomes**:

| | `mtl_session_close` | instance close or shutdown | `mtl_instance_abort` |
|---|---|---|---|
| Queued units | sent (DRAIN) until the deadline, then `FLUSHED`/`STOP_TIMEOUT` | `FLUSHED`/`CLOSE`; sent with `MTL_SHUTDOWN_DRAIN` | `FLUSHED`/`ABORTED` |
| First packet left | completes | sent to its end at its pace | cut at the next packet: `FLUSHED`/`ABORTED` with `MTL_TXR_PKT_SHORT` |
| Unread results | discarded | discarded, counted in `results_discarded` | as instance close |

### 4.6 Discard

`mtl_session_discard(s, flags, first_index)` (CP): queued units become `FLUSHED` (`DISCARD`) and the
session stays RUNNING: GStreamer `FLUSH_STOP` and seek. RX drops the unit being received and the
ready units not yet dequeued (`rx.units_flushed`, §4.5). With `MTL_DISCARD_REBASE`, `first_index` maps to the next feasible index. A taken-back unit that a plugin is converting bounds the call by that conversion.

### 4.7 Update

`mtl_session_update(s, &sc, parts, &when, &planned_tai_ns)` (CP) is one atomic change, applied at `when` on every leg. It replaces today's `update_destination`/`update_source`, reconfigure and per-leg enable. One call, not one per part: NMOS IS-05 patches transport parameters and `master_enable` together under one activation, so one atomic change fits it.

| Part | Changes | States |
|---|---|---|
| `MTL_UPDATE_FLOWS` | `sc->flows` (IS-05) | any started state but DRAINING/FLUSHING; applied during the call in CREATED and STOPPED |
| `MTL_UPDATE_LEGS` | `sc->legs_disabled` | as FLOWS |
| `MTL_UPDATE_MEDIA` | the essence member, `tsmode` and `max_udp_payload` | CREATED or STOPPED; colorimetry, tcs and range also while running |
| `MTL_UPDATE_POOL` | `pool_count` | CREATED or STOPPED |

From MS3 every part works in CREATED and STOPPED; in ARMED and RUNNING the call is `-MTL_ENOTSUP`
(`NOT_IMPLEMENTED`, field `state`) until MS5.

The contract:

- **All or nothing.** Resources (rules, queues, joins, header templates, a kernel-socket queue) are reserved before commit; on any failure nothing changes (G-75).
- Neighbours are resolved from commit on and not awaited: a leg without one waits in `MTL_FLOW_WAITING_NEIGHBOUR`, and the update still applies.
- The update reads only the members its `parts` name, and `sc->options`; every other member of `sc` is ignored, so the application passes the config it created the session with, edited. `direction`, `essence` and `unit` never change.
- Every key in `sc->options` must equal its current value (`-MTL_EBUSY`, `OPTION_STATE`), except that a key that may change while running (R) applies at the same boundary: Phase 7; until then a changed R key is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`), and `mtl_set_options` changes it at the next unit boundary instead.
- `when` applies to FLOWS, LEGS, those options, and colorimetry/tcs/range under MEDIA while running. It is NULL otherwise. In ARMED a `when` before T0 means T0; an `MTL_AT_TAI` or `MTL_AT_INDEX` already past means now.
- **The switch happens at the index boundary by the clock, whether a unit is there or not.** TX: the first index at or after `when`; all legs switch on the same unit, and no unit goes to a mix of old and new destinations. RX: units with media time at or after `when`, or by local arrival time for unlocked clocks. Audio: a packet boundary, so a salvo lands within one packet time.
- RX prepares the new rules beside the old ones and removes the old ones after the switch; joins go out at the call (Phase 7: at `max(call, when − rx.join_lead_ns)`).
- A port change while running is made before break (Phase 7), or `-MTL_EBUSY` (`PORT_CHANGE_NEEDS_STOP`) on a backend that cannot, and on every backend until then: stop, update, start.
- An update while an earlier one is pending is `-MTL_EBUSY` (`WRONG_STATE`).
- **Cancel.** `sc` NULL with `parts` 0 cancels a pending update (IS-05 activation mode null): it
  returns 0 when it cancelled one, 1 when none was pending, and `-MTL_EBUSY` (`UPDATE_COMMITTING`)
  when the update will still apply. Phase 7; `-MTL_ENOTSUP` until then.
- A pending update fails (`TIME_STEP`) if the time base steps.
- The call returns once the change is posted. `planned_tai_ns` (may be NULL) receives the media time of the boundary. In CREATED and STOPPED it applies during the call and planned = applied = now.
- `status.update_state` (`enum mtl_update_state`: NONE, PENDING, APPLIED, FAILED with `update_reason`), `status.update_seq` (+1 per posted update; read it after the call to match events), `status.update_applied_tai_ns` (INT64_MIN until applied) and `MTL_EVENT_UPDATE` report when it applied.
- The update keeps the handle, name, SSRC and counters; library pools are re-created under MEDIA or POOL, attached pools are re-validated (G-87).
- Re-creating a library pool needs every slot FREE (no lease or hold out): `-MTL_EBUSY` otherwise. The pacing values (TRS, VRX, rate-limit rate) and the scheduler weight are re-derived from the new configuration as a dry run first; a weight or rate that no longer fits the scheduler or the queue is `-MTL_ENOSPC`, with nothing changed.
- A MEDIA or POOL update fails with nothing changed, `-MTL_EINVAL`, `LAYOUT_MISMATCH`, when an attached pool no longer fits the new layout (G-87).

**Update states.** `status.update_state` reports the last update call, in the states of the
picture below. An update in CREATED or STOPPED applies during the call; one posted while ARMED or
RUNNING is PENDING until the index boundary at or after `when`, then APPLIED, or FAILED (for
example `TIME_STEP`), which keeps the old configuration. Each posted update adds 1 to
`update_seq`, so a Node reads `update_seq` after the call and matches it with `MTL_EVENT_UPDATE`
(new = the state, value[0] = the applied TAI, value[1] = the seq).

```mermaid
stateDiagram-v2
    direction LR
    [*] --> NONE
    NONE --> PENDING: posted while<br/>ARMED or RUNNING
    NONE --> APPLIED: update in<br/>CREATED or STOPPED
    PENDING --> APPLIED: the index boundary<br/>at or after when
    PENDING --> FAILED: e.g. the time base<br/>stepped (TIME_STEP)
```

The states are `MTL_UPDATE_STATE_*`: NONE and APPLIED are reported from MS3, PENDING and FAILED
from MS5; REPLACED and CANCELLED are Phase 7.

**What is Phase 7.** MS3 delivers the update in CREATED and STOPPED (every part, applied during
the call, the session re-created inside its handle). MS5 delivers the atomic update while running:
today's destination and source update, per-leg enable and disable, the planned instant,
`status.update_*`, `MTL_EVENT_UPDATE` and the switch by the clock. These are Phase 7, later
(D-98): mute (every leg disabled), reserved legs, port changes while running, R options at the
boundary, and `rx.join_lead_ns`. Each is `-MTL_ENOTSUP` until then; `MTL_STATUS_MUTED` and
`rx.join_lead_ns` are declared only under `MTL_LATER`, as are the re-apply and dry-run modifiers,
cancel and the REPLACED and CANCELLED update states. An NMOS Node
can be built on MS3 with stop, update and start, and on MS5 without the stop. The IS-05 mapping is in
[nmos-ipmx.md](nmos-ipmx.md).

### 4.8 ERROR

A session enters ERROR only for faults it cannot recover from without the application. Every entry has one reason in `status.error_reason`, and `status.error` is the code data calls return.

| Reason | Trigger |
|---|---|
| `TX_QUEUE_FATAL` | TX hang recovery could not get a new queue or mempool |
| `DEVICE_GONE` | NIC removal, or a reset after which queues and flows cannot be re-reserved |
| `CMD_TIMEOUT` | a command was not acknowledged within the fixed ack timeout (D-48) |
| `BACKEND_FAILED` | a kernel-socket or AF_XDP failure after retries |
| `FORCED` | `mtl_debug_inject(…, MTL_FAULT_FORCE_ERROR, …)` (debug builds) |

- On entry, in this order: the state is marked and blocked callers wake with the error code; the session is detached from its scheduler; the TX queue is reset or quarantined so that no descriptor references application memory; every device reference is dropped; the units get the ERROR column of §4.5; `MTL_EVENT_SESSION_STATE` is posted.
- A fault in DRAINING or FLUSHING enters ERROR like a fault in RUNNING. A stop that was waiting
  for the drain or the flush then finishes the job: once the entry steps are done it moves the
  session from ERROR to STOPPED and returns `-MTL_EIO`, with the fault's reason in
  `mtl_last_error()`. Every accepted unit has its outcome (the ERROR column of §4.5), as after
  any stop (G-31). A close that was draining continues to retire.
- Leaving ERROR: `mtl_session_stop` → STOPPED. `mtl_session_start` from there re-reserves what the fault invalidated (queues, flow rules, memberships, the rate-limit shaper) or fails `-MTL_ENODEV` with a reason and nothing changed. `mtl_session_update` (for example a new `flow.port`) may come before the start.

### 4.9 Close

`mtl_session_close(s, timeout_ns)` (CP) stops (DRAIN until the deadline, then FLUSH), destroys, and waits up to `timeout_ns` for the session to retire.

The picture below shows the steps inside CLOSING: every step before RETIRED is the state
CLOSING. A session that is sending drains first; one that is not goes straight to the detach. A
queue that will not release its descriptors takes the stalled-queue path
([engine.md §4](engine.md#4-close-and-error-on-a-stalled-queue)).

```mermaid
stateDiagram-v2
    state "drain" as Drain
    state "flush" as Flush
    state "detach" as Detach
    state "device references" as DeviceRefs
    state "leases and holds" as Leases
    [*] --> Drain: close in ARMED,<br/>RUNNING or DRAINING
    [*] --> Detach: close in CREATED, STOPPED,<br/>FLUSHING or ERROR
    Drain --> Flush: deadline passed, rest<br/>FLUSHED (STOP_TIMEOUT)
    Drain --> Detach: every queued unit sent
    Flush --> Detach: queued units FLUSHED
    Detach --> DeviceRefs: tasklet ack,<br/>data callers left
    DeviceRefs --> Leases: last NIC and DMA<br/>reference gone
    Leases --> RETIRED: no lease or hold left
    RETIRED --> [*]
    note right of Leases
        close returns 1 here if leases are out;
        the last release posts the retire to a worker
    end note
```

- 0: retired; no memory, lease, hold or device reference remains.
- `MTL_RETIRING` (1): still retiring (leases out, or units held by the device).
  - **Retirement completes on the last release, with no further call.** A source over library
    memory closes with timeout 0 and returns. Buffers downstream still holds release their
    leases later, from any thread.
  - After the instance closed, such a release returns 0 and `mtl_session_get_status` then reads
    RETIRED (the last column of §4.2). The release cannot free (DP) and the instance's worker is
    gone, so the memory goes to the process's orphan list, which the next control-plane call of
    the process frees before its own work; without one it lasts until exit (R4, D-116).
  - Calling close again only polls, or waits with a timeout.
  - A session over application memory still finishes §9.10 before that memory is freed.
- **Close is idempotent.** Calling it again on the same handle polls: 1 while retiring, 0 once retired, never `-MTL_EBADF`. Waiting for retirement is calling `mtl_session_close(s, timeout)` again; there is no retirement wait target and no retirement event. A null handle returns 0.
- From the first call on, the only other calls valid on `s` are `mtl_session_get_status` (`mtl_session_get_state`: `MTL_STATE_CLOSING`, then `MTL_STATE_RETIRED`), the release of leases taken before the close (retirement waits for it), and `mtl_session_interrupt` (AS; a no-op that returns 0); every other call gets the CLOSING or RETIRED column of §4.2.
- A close that returns 1 logs one warning for the session (once, not on every poll) with its count of held leases and the oldest lease's age, taken from the acquire time already in the slot table (no per-lease allocation on the data path); `session.leases_out` and `session.oldest_lease_ns` hold the same.
- Unread results are discarded.
- Sessions attached over this session's pool must close first: 1 until they do.
- After retirement: no library thread calls application code for `s`; no thread or device reads or writes memory the application gave `s`, including after a stalled or hung TX queue; threads blocked in calls on `s` have returned (G-37, G-65, G-81). No queue reports `s` once its close has returned, and its retirement detaches it (§7.2).
- **Close cannot race an active call or a completion into freed memory** (G-29): every data call counts itself in flight, and close waits for that count to drain before anything is freed. Wakes and interrupts need no count: they touch only never-freed words, and a queue's descriptor stays open while its entry exists ([core.md](core.md) §6).
- The last release or hold drop of a closing session is DP: it stores RETIRED and hands the rest of the retirement to the control plane (a library worker, or, once the instance is gone, the orphan drain of the next control-plane call). Retirement never runs inside a data call, so it never waits for its own caller.

### 4.10 Status and recoverable incidents

`mtl_session_get_status(s, &st, size)` (DP: a published snapshot, never torn, lock-free from any
thread) says why the session is where it is: `state`, `reason` of the last transition,
`error_reason` and `error` in ERROR, `blocked_on` (§5.5), `flags`, `timing_reason` with
`shortfall_ns` and `suggested_min_tx_delay_ns`, `leg[]` (§14.2), the update fields of §4.7,
`format_reason` (with `MTL_STATUS_FORMAT_CHANGED`) and `provide_gen`: provide sessions, 1 + the
hand-backs so far (§9.11).

| `flags` bit | Meaning |
|---|---|
| `MTL_STATUS_RX_SIGNAL` | RX: packets are arriving |
| `MTL_STATUS_FORMAT_CHANGED` | RX: the stream does not fit the configuration (detect OFF: differs from `raster`; detect ON: above the maximum, or not identified); `format_reason` says which (`RASTER_MISMATCH`, `DETECT_FAILED`); `rx.detected.*` hold the measurement |
| `MTL_STATUS_RX_DETECTING` | RX, detect ON: measuring the stream's format; no unit is published until it is known |
| `MTL_STATUS_PACING_DOWNGRADED` | TX: running below the granted pacing class |
| `MTL_STATUS_TIMING_WARNING` | TX: `timing_reason` and `shortfall_ns` are set |
| `MTL_STATUS_MUTED` | every leg admin-disabled; RUNNING, nothing sent (Phase 7, `MTL_LATER`) |
| `MTL_STATUS_TIMEBASE_SUSPECT` | RX: on a DIRECT stream arrival and media time differ by more than 1 s ([timing.md](timing.md) §11.1); the getter of `MTL_EVENT_RX_TIMEBASE_SUSPECT` |

Recoverable incidents never enter ERROR; they are events plus per-unit results:

| Incident | Outcome |
|---|---|
| TX hang fixed by recovery | `MTL_EVENT_RECOVERY` begin and end; units in the gap `DROPPED`/`RECOVERY`, never `ON_TIME`; `tx.recoveries_ok` is the count of record |
| link down on one 2022-7 leg, or on the only leg | stays RUNNING; `MTL_EVENT_LEG_STATE`, `MTL_EVENT_PORT_LINK`; units not sent on that leg (`LINK_DOWN`); resumes on link up |
| port reset that restores | §2.6 |
| PTP holdover or loss | `MTL_EVENT_TIME_STATE`; timing flags in results |
| MtlManager lost | running sessions continue; `MTL_EVENT_MANAGER_LOST` |
| unresolved neighbour, failed join | `MTL_EVENT_FLOW_STATE` |

## 5. Data path

### 5.1 The unit

One `struct mtl_unit` is what acquire and dequeue lend and what submit reads.

- `MTL_INIT(&u)` once. Acquire and dequeue write every field up to `struct_size` except `struct_size` itself, so the init can stay outside the loop. On failure they leave `*u` unchanged.
- `lease` is the access token; `slot` the pool slot, a stable buffer identity; `plane[]` the planes (`addr` is NULL in device memory without a CPU mapping); `meta`, `meta_capacity` the slot's meta area (§9.9).
- Per-use fields: TX inputs, zeroed by acquire; RX outputs. `used`, `flags`, `rtp`, `media_index`, `media_tai_ns`, `cookie`, `hold`, `launch_tai_ns`, and RX `status`, `missed_before`.
- **TX acquire resets the meta area**: it writes an `MTL_META_NONE` header at the start of the slot's meta area (frame and row units), so a slot never carries the previous use's records (§9.9).
- **As a template** (`mtl_tx_write`, `mtl_tx_send_slot`) a unit contributes `media_index`,
  `media_tai_ns`, `cookie`, `hold`, `launch_tai_ns`, `meta` and the TX bits of `flags` (0–31,
  `MTL_SUBMIT_MASK`); `mtl_tx_send_slot` also takes `used`, because the slot already holds the
  content; every other field is ignored. A received unit is a valid template. A received unit's
  `cookie` is 0 unless it was received into a provided destination (§9.11); a forwarder over such a
  session sets `how.cookie` (its own, or 0) before it sends.
- **`mtl_tx_write` returns the bytes it accepted**, or the first call's error when it accepted
  none. Inside one call each unit after the first takes the next index of `mtl_tx_get_next`.
  After a partial write (fewer bytes than asked, for example when acquire timed out on a full
  pool), call it again with the rest and `how.media_index` (or `media_tai_ns`) set to
  `next_media_index` (`next_media_tai_ns`) of `mtl_tx_get_next`: the first index at or after the
  end of the last submitted unit (for audio its first sample plus its samples), so the stream stays
  contiguous. It takes one-plane units; an ANC unit is `-MTL_EINVAL`.

**Units per essence**:

| Essence | Unit | `used` counts | Index period |
|---|---|---|---|
| video, frame units | a frame; a field when interlaced (`rows` = height / 2); one per frame for PsF | rows; TX 0 = the whole unit | a frame, or a field |
| video, `MTL_UNIT_ROWS` | a frame or field whose rows are published progressively | rows ready; 0 is legal (the slot is claimed before row 0 exists) | a frame, or a field |
| cvideo | a codestream per frame or field | bytes, literal | a frame, or a field |
| audio | a run of samples: `unit_samples` per RX unit, the TX pool capacity per acquire | bytes, literal: a whole number of sample frames (channels × bytes per sample; else `-MTL_EINVAL`), so 0 is `-MTL_EINVAL`; samples = `used` / (channels × bytes per sample) | one sample: `media_index` is the unit's first sample |
| ANC | the ANC packets of one frame or field (one per frame for PsF): plane 0 the packet table, plane 1 the words, RAW plane 2 the header words (§5.7) | table entries; 0 = no ANC packet: the empty RTP packet that keeps the stream alive | the frame or field of the video it follows |
| fastmeta | one data item group | bytes, literal; 0 = one RTP packet with no data item, the keep-alive (§3.3) | as ANC, or its own rate with `MTL_FASTMETA_FREE_RUNNING` |
| generic RTP, any essence with `MTL_UNIT_PACKETS` | a chunk of packet slots | packets, literal | the essence's; the unit rate for generic RTP |

### 5.2 TX

- `mtl_tx_acquire(s, &u, timeout)` (WT) lends a writable slot: 0, or `-MTL_EAGAIN` (none free; `status.blocked_on` says why), `-MTL_ECANCELED`, `-MTL_ESHUTDOWN`, `-MTL_EIO`.
- `mtl_tx_submit(s, &u)` (DPC; for ANC it encodes the unit, §5.7) hands the unit to MTL. Exactly one result follows when results are on (§6).
- **A failed first submit returns the slot to the pool without a result**, except `-MTL_EBADF` and `-MTL_ESTALE`, which change no state. No loop needs a release on the failure path (D-88). "Fix and resubmit" means acquire and fill again.
- `mtl_tx_release(s, lease)` (DP) returns an acquired, unsubmitted lease; no result. On a submitted lease it is `-MTL_ESTALE` and changes nothing (G-78).
- **Rows units**: submit the same lease again with a larger `used` to publish more rows. A submit
  publishes the contiguous prefix `[0, used)` with release ordering, and the engine reads only
  published rows with acquire ordering: the bytes written before the call are what is sent. Waiting
  for a late row never moves the unit: its launch index and RTP are fixed at the first submit. Once a submit
  was accepted, a later failing submit ends the unit where its rows stopped (by `tx.rows_late`),
  consumes the lease, and the unit's one result follows when its packets have left. Unpublished rows
  are never read (G-12). The other per-use fields must equal the first submit's.
- **Order**: units are sent in submit order, never slot order and never acquire order (G-08).
- An accepted unit progresses without any further API call, the last one before the application goes idle included (G-03).

**Submit flags** (`unit.flags` is `uint64_t`: TX bits 0–31, RX bits 32–63):

| Flag | Meaning |
|---|---|
| `MTL_SUBMIT_DISCONTINUITY` | media time jumps (seek, new clip) |
| `MTL_SUBMIT_RTP_TS` | send `unit.rtp` as the RTP timestamp; not with `MTL_MEDIA_AUTO` (`-MTL_EINVAL`, `RTP_TS_AUTO`) |
| `MTL_SUBMIT_NOT_BEFORE` | first packet not before `launch_tai_ns` |
| `MTL_SUBMIT_EXACT` | first packet at `launch_tai_ns`; non-compliant; only on a session created with `MTL_SESSION_EXACT_LAUNCH`, else `-MTL_EINVAL` |
| `MTL_SUBMIT_UNIT_END` | packet units: the last chunk of its unit |
| `MTL_SUBMIT_SRC_PLANES` | `u.plane[]` names caller memory (with its stride and rows) that submit reads during the call and copies or converts into the slot's own planes (ANC: encodes from the caller's planes into the wire area; the slot's planes are not written; `MTL_TXR_COPIED` is set); the result carries `MTL_TXR_COPIED`; `-MTL_EINVAL` only with `MTL_SESSION_REQUIRE_DIRECT` |

- `MTL_SUBMIT_NOT_BEFORE` and `MTL_SUBMIT_EXACT` exclude each other; `MTL_SUBMIT_UNIT_END` on a unit that is not a packet unit is `-MTL_EINVAL`. `MTL_SUBMIT_SENDER_TIME` (RTP and the sender report's time from the received unit) is Phase 7 and declared only under `MTL_LATER`.
- **Exact launch is a session property.** The engine pins every frame of a session to its launch or to none (`ST20_TX_FLAG_EXACT_USER_PACING` is a session flag, [engine.md](engine.md) §2.12), so a TX session admits `MTL_SUBMIT_EXACT` units only with `MTL_SESSION_EXACT_LAUNCH`; its other units start at their launch index's first-packet time, which the binding computes ([timing.md](timing.md) §6.8).
- **TX from caller memory.** With `MTL_SUBMIT_SRC_PLANES` the slot stays the admission and
  back-pressure token: acquire it, set `u.plane[]` to the caller's planes, submit; the bytes are
  read before submit returns, so the caller may reuse its memory at once. On a converting library
  pool whose every submit does this, set `MTL_SESSION_TX_SRC_PLANES`: the pool then holds no
  app-format planes (at 2160p 33 MB a slot). On any session, direct
  ones included, submit copies or converts the planes into the slot during the call and the result
  carries `MTL_TXR_COPIED`; only a session created with `MTL_SESSION_REQUIRE_DIRECT` returns
  `-MTL_EINVAL`. With an asynchronous converter device, submit converts in the
  caller or returns `-MTL_ENOTSUP`. On any session the inline `mtl_unit_copy_plane_in` /
  `mtl_unit_copy_plane_out` (`mtl_util.h`) copy a whole plane, row by row, from or to caller memory
  with its own stride.
- `launch_tai_ns` is read with NOT_BEFORE or EXACT, and always with `MTL_PKT_PACE_LAUNCH`.
- Submit validates synchronously what it can know: a non-zero `cookie` without results (`COOKIE_WITHOUT_RESULTS`), a backward `MTL_SUBMIT_DISCONTINUITY` while RUNNING (`MEDIA_TIME_BACKWARDS`), a missing hold on a pool over another session's pool (`HOLD_REQUIRED`), a cvideo codestream above the granted size (`-MTL_ENOSPC`, `CODESTREAM_OVERSIZE`, G-66).
- Lateness is never a submit error: it is decided at pick-up and reported in the result.
- The meta area is validated and copied at submit, so later writes never reach the wire. Plane bytes are read at send time (ANC: at submit).
- **MTL never writes a TX buffer.** After submit its bytes are unchanged and it stays mapped until it is acquired again (G-100).
- `mtl_tx_get_next()` (`mtl_sync.h`) says where the next unit lands; submitting before `submit_deadline_tai_ns` yields `ON_TIME` in that slot (G-53). `mtl_tx_row_deadline()` gives a row's latest submit time.

### 5.3 RX

- `mtl_rx_dequeue(s, &u, timeout)` (WT) returns a received unit, in delivery order. Missing packets read as zero in library pools, and `u.status` says so.
- It is DPC when it works on the unit in the caller: packet units without `MTL_PKT_RX_LEND`,
  which it copies; `video.app_format` conversion; ANC units, which it decodes (§5.7); and the
  zero fill of a library-pool unit that lost packets (§9.8). It claims the unit under the
  session's reaper lock and does this work after releasing it, so several threads that dequeue
  one session work on consecutive units at once (§5.6).
- `mtl_rx_release(s, lease)` (DP) from any thread, in any order (G-56).
- The unit's memory is valid while the lease is held, across stop; after release MTL may write the slot again.
- `MTL_UNITF_PARTIAL` (rows units): more rows follow. `mtl_rx_wait_rows(s, lease, min_rows, &rows, timeout)` (`mtl_sync.h`) waits until at least `min_rows` are complete or the unit ends, and a sleeping call is woken once then, whatever `rx.rows_step` is (D-163); `rx.rows_step` sets how often the binding advances the rows.
- A unit whose due time (first-packet arrival + unit period + `rx.flush_offset_ns`) passes is force-completed within one scheduler iteration and delivered or discarded per `rx.incomplete`, with no further packet needed (G-82).
- **The tail.** In a video or cvideo library pool, the `MTL_RX_TAIL_BYTES` (64) bytes after a
  dequeued unit are zero when dequeue returns. For video the unit ends at `plane[0].addr` + Σ
  `stride × rows` over its planes, the size a framework gives its buffer; for cvideo it ends at
  `plane[0].addr + used`. Dequeue writes the tail every time, so it holds whatever an earlier use
  or the application wrote there. FFmpeg's `AV_INPUT_BUFFER_PADDING_SIZE` is 64. Audio, ANC,
  fastmeta and packet units, attached pools and provided destinations have no tail (D-37).
- **Wrapping.** A dequeued unit may be wrapped in a framework buffer and released from its free
  callback on any thread, in any order: after stop, after the session's close, and after the
  instance's close (R4, §4.9). The callback keeps the session and lease by value, never a pointer
  into the element (ex05). A consumer that lets downstream hold units keeps at least 2 slots free
  by copying instead of wrapping when needed ([migration.md](migration.md) §12.8).

### 5.4 Lease rules

The application sees a slot in three ways: free, yours (leased to the application, to write or
to read) or MTL's (sending or receiving). The two pictures below show how a slot moves between
them, first on TX, then on RX; the seven slot states behind these views are the slot table's
([core.md §3.1](core.md#31-the-slot-table)).

On TX, acquire makes a free slot yours, submit makes it MTL's, and the recorded result frees it.
A slot you acquired and do not submit goes back with release.

```mermaid
stateDiagram-v2
    direction LR
    state "Free" as F
    state "Yours: you write it" as Y
    state "MTL's: sending" as M
    [*] --> F
    F --> Y: mtl_tx_acquire
    Y --> M: mtl_tx_submit
    M --> F: sent,<br/>result recorded
    Y --> F: mtl_tx_release
```

On RX the order is the other way round: MTL takes a free slot when packets arrive, dequeue makes
it yours, and release frees it. A slot that TX units still hold stays HELD after the release
until the last hold drops (rule 8, §9.6).

```mermaid
stateDiagram-v2
    direction LR
    state "Free" as F
    state "MTL's: receiving" as M
    state "Yours: you read it" as Y
    [*] --> F
    F --> M: packets arrive
    M --> Y: mtl_rx_dequeue
    Y --> F: mtl_rx_release
```

The rules:

1. A lease is one access grant on one pool slot, from acquire or dequeue until submit or release.
2. A pool slot is never re-acquirable before its outcome is recorded (G-05). "Reusable" means no reader or writer remains: no converter, encoder, DMA descriptor or NIC (G-06).
3. Submit and release end a lease; any later use is `-MTL_ESTALE`. A lease of another session is `-MTL_EBADF`, deterministically (G-07).
4. A TX lease is the application's to write; a submitted one is MTL's until its result.
5. An RX lease is the application's to read; release returns it, from any thread.
6. A lease held across stop and close stays valid; close retires the session only after it returns (G-65).
7. A slot's planes never change between uses; per-use values live only in the unit (G-48).
8. A TX unit may hold an RX lease (`unit.hold`) until its result (§9.6).
9. A rejected call (any validation failure) produces no outcome (G-02; D-88: a failed first submit returns the slot to the pool, except `-MTL_EBADF` and `-MTL_ESTALE`).

### 5.5 Back-pressure

`status.blocked_on` (`enum mtl_blocked_on`) says why acquire returns `-MTL_EAGAIN`:

| Value | Meaning | What to do |
|---|---|---|
| `MTL_BLOCKED_NONE` | not blocked | — |
| `MTL_BLOCKED_BUFFERS` | every slot is queued or in flight: normal back-pressure | wait |
| `MTL_BLOCKED_RESULTS` | unread results fill the ring | reap |
| `MTL_BLOCKED_APP_LEASES` | every slot is leased by the application | submit or release what you hold |

`MTL_EVENT_BACKPRESSURE` reports each begin (NONE → X) and end (X → NONE); `tx.acquire_blocked{on=…}` counts them.

### 5.6 Concurrency

| Function | Class | Concurrency | Signal handler |
|---|---|---|---|
| `mtl_instance_open`, `mtl_instance_close`, `mtl_instance_shutdown` | CP | any application thread; never from a library thread (`-MTL_EDEADLK`) | no |
| `mtl_interrupt` with `MTL_INTR_ON` or `MTL_INTR_ABORT` (`mtl_instance_interrupt` (on 1), `mtl_instance_abort`, `mtl_session_interrupt` (on 1)) | AS | any thread, any time, also during and after close | yes |
| the same with `MTL_INTR_OFF` (on 0) | CP | any application thread | no |
| `mtl_session_create`, `open`, `query`, `start`, `stop`, `update`, `discard`, `close`, `attach`, `detach` | CP | any thread; serialised per session inside the library | no |
| `mtl_tx_acquire`, `mtl_tx_acquire_slot` | WT | MP-safe | no |
| `mtl_tx_submit`, `mtl_tx_write`, `mtl_tx_send_slot` | DPC, WT | one submitting thread at a time; MP-safe with `MTL_SESSION_MT_SUBMIT` | no |
| `mtl_tx_release`, `mtl_rx_release` | DP | any thread, any order | no |
| `mtl_tx_reap`, `mtl_tx_reap_full`, `mtl_session_read_events`, `mtl_instance_read_events` | WT | MP-safe; each call counts itself in flight | no |
| `mtl_rx_dequeue` | WT (DPC when it works on the unit, §5.3) | MP-safe; each call counts itself in flight; concurrent dequeues of one session claim consecutive units under a lock that covers only the claim, and convert, decode, copy or fill them in parallel outside it; the claim order is the delivery order | no |
| `mtl_wait` (`mtl_session_wait`), `mtl_rx_wait_rows`, `mtl_queue_wait` | WT | any number of waiters per target and on any targets (G-95); a queue: any number of callers, each report to one | no |
| `mtl_session_get_status` (`mtl_session_get_state`), `mtl_last_error`, `mtl_time_now` (and the inline `mtl_time_convert` over it), `mtl_stat_read`, `mtl_instance_get_health`, `mtl_rx_get_detail`, `mtl_session_get_slot`, `mtl_tx_get_next` | DP | any thread | no |
| `mtl_reason_name`, `mtl_library_version`, `mtl_option_list`, `mtl_option_find`, `mtl_epoch_index_at`, `mtl_media_ticks`, `mtl_media_tai`, `mtl_format_describe`, `mtl_format_parse`; the pure inlines (`mtl_error_name`, `mtl_library_version_num`, `mtl_struct_init`, `mtl_fps_rational`, `mtl_flow_ipv4`, `mtl_flow_parse`, `mtl_rx_align`, the ANC helpers) | AS | any thread; pure | yes |

Debug builds detect a violation of "one submitting thread" by overlap, not by thread identity, so a framework pool whose acquire and submit never overlap is legal.

One 4320p 4:2:2 10-bit conversion takes about 10–20 ms on one core *(est.)*, near the 16.7 ms
frame period at 59.94p: two threads dequeueing the session keep up, one may not (`convert_ns`
says which).

### 5.7 ANC units

An ANC unit (`MTL_ANC`, frame units) is the ANC packets of one frame or field (one per frame for
PsF) in the planes of one slot: plane 0, `anc.max_packets` rows of `struct mtl_anc_packet` (16 B);
plane 1, `anc.max_udw_words` user data words of the session's word mode (`MTL_ANC_WORDS_8BIT` 1
byte, `_10BIT` and `_RAW` 2 bytes); with `_RAW` plane 2, four `uint16_t` per table row (DID, SDID or
DBN, Data_Count, Checksum_Word). `used` counts entries; entry i's words are rows `udw_offset`
onwards (`udw_count` of them). `mtl_anc_table`, `mtl_anc_words`, `mtl_anc_raw_hdr` and `mtl_anc_put`
(`mtl_util.h`) give and fill them. Entries may share words. Each slot also has a wire area in the
session's private allocation, which no call exposes: submit writes the unit's RTP payloads there and
RX's packets land there.

**Word modes.** 8-bit: the user data word's value b7–b0; TX adds b8 = even parity and b9 = NOT b8
(the convention of the 8-bit payload documents, not ST 291-1 §6.6's 8-bit applications). 10-bit: all
ten bits as given. RAW (relays): the user data words and plane 2's header words, ten bits each, sent
and received as on the wire, bit for bit for well-framed packets.

**What MTL writes.** The RFC 8331 header word (C, Line_Number, Horizontal_Offset, S, StreamNum, as
in the entry: `line` 0 is sent as 0); outside RAW, DID, SDID or DBN and Data_Count with b8/b9, the
8-bit words' b8/b9, the checksum; always word_align, and per RTP packet the RTP header, Extended
Sequence Number, Length, ANC_Count, F (00 progressive; interlaced 10 or 11 by the field; PsF by the
segment) and the reserved bits. The marker is on the unit's last RTP packet and on every empty one
(ST 2110-40 §5.5 prevails over RFC 8331 §2 by -40 §5.2.1).

**Submit** (DPC) first claims the lease (APP → XFORM); a stale or second submit returns `-MTL_EBADF`
or `-MTL_ESTALE` and writes nothing. It checks the unit and encodes it in one pass into the
session's private wire area, which no region exposes, reading each entry once and checking its word
range against plane 1 before reading a word: `used` ≤ `max_packets`; the entries' `udw_count`
summed ≤ `max_udw_words`; `flags` ≤ 7Fh (RX-only flags
ignored), `reserved` 0, `stream` ≤ 127, `line` ≤ 7FFh, `hoffset` ≤ FFFh; the word range within plane
1; outside RAW, `did` ≠ 00h and a Type 2 `sdid` ≠ 00h (ST 291-1 §6.1, §6.2); 10-bit, no protected
word (ST 291-1 §9.1); RAW, every word ≤ 3FFh and plane 2's DID, SDID and DC low bytes equal to the
entry's; the exact lines (1–7FCh) of entries without `MTL_ANCF_AS_IS` in raster order, counted from
the vertical alignment line ([timing.md](timing.md) §9.2; on 525 lines, lines 1–3 end the frame),
and on an interlaced session inside the unit's field (1125 lines: 1–563 and 564–1125; 625: 1–312,
313–625; 525: 4–265, 266–525 and 1–3). Failures: `-MTL_EINVAL` (`INVALID_ARGUMENT`, `field` `"used"`
or `"anc[i].<name>"`); the slot returns without a result and **nothing is queued**. A unit that
passes is queued whole and its RTP packets are built from the wire area only, so writes to the
planes after submit never reach the wire. An entry with `MTL_ANCF_AS_IS` is sent as written and
timed as unlocated; RX sets the flag on such entries, so **every entry RX delivers passes submit**.
MTL does not check the DID registry, DBN continuity, the order within a line, or ST 2110-40 §5.2.1's
"should not" for embedded audio and EDH. A second unit for an index is `DROPPED`
(`DUPLICATE_INDEX`).

**Packing.** In table order, at most 255 ANC packets and `sc.max_udp_payload` − 20 bytes of ANC data
per RTP packet (1432 B by default; ST 2110-40 §5.2.1, ST 2110-10 §6.3), a new one at
`MTL_ANCF_NEW_RTP` and at a PsF segment change; an empty unit or segment gets one empty RTP packet.
An ANC packet of n user data words takes `mtl_anc_rfc8331_bytes(n)` ≤ 328 bytes. RTP packets leave
at the times of [timing.md](timing.md) §9.2, at most 16 per tasklet call, and the hint's deadline
(`mtl_tx_get_next`) is timing.md §9.2's; the late policy is decided once, when MTL picks the unit up
(DEFER under AUTO, DROP under INDEX and TAI), and a unit whose first RTP packet has left is sent to
its end; the result follows the last.

**RX.** MTL copies each RTP packet's payload into the slot's wire area in sequence order (records
are indexed by sequence distance). `mtl_rx_dequeue` (DPC) decodes them into the planes: entries in
wire order, each with `rtp_index` (the sequence distance from the packet after the previous unit's
marker) and `MTL_ANCF_NEW_RTP` on the first entry of each RTP packet. 8- and 10-bit sessions skip
and count (the total in `mtl_rx_detail.anc_skipped`, each cause in `anc.pkts_skipped{cause}`)
DID or SDID parity errors, checksum errors, DID 00h, Type 2 SDID 00h, 10-bit protected words and 8-bit words that break the
parity convention; a Data_Count parity error or a packet past Length skips the rest of its RTP
packet. RAW skips only a packet past Length and flags the others (`MTL_ANCF_PARITY_ERR`,
`MTL_ANCF_CHECKSUM_ERR`). Packets beyond capacity (`max_packets` entries, `max_udw_words` words,
or a wire area that is full) are counted (`anc.pkts_truncated`, `anc_truncated`). `MTL_ANCF_GAP_BEFORE` and `MTL_ANCF_AS_IS` are computed on the finished unit,
after every leg's packets: the entry after skipped, truncated or missing packets carries
`GAP_BEFORE`; entries out of raster order or outside the unit's field carry `AS_IS`. So no mode
re-checksums a corrupt packet: 8- and 10-bit drop it, RAW sends it as it was. A unit ends at its
marker (PsF: a marker on F = 11, or on F = 00 when no packet of its timestamp follows within TSFO;
never on F = 10), at a new timestamp, or at its due time (§5.3); with two legs a marker with a
sequence gap waits for the gap at most `rx.skew_budget_ns`. A packet of a published unit's timestamp
after its marker is dropped (`rx.pkts_rejected{cause=after_marker}`); an F = 01 packet too
(`cause=interlace`). COMPLETE when every sequence number from the packet after the previous marker
to the unit's marker is present and nothing was skipped or truncated, else INCOMPLETE.
`rx.incomplete` DISCARD acts on sequence and truncation status; units with skipped packets are
delivered INCOMPLETE. An empty RTP packet is a unit with `used` 0; ANC units are never zero-filled.

**From caller memory.** `MTL_SUBMIT_SRC_PLANES`: `plane[0]` the caller's entries (`rows` ≥ `used`,
`stride` ≥ 16), `plane[1]` its words, RAW `plane[2]`; submit encodes from them directly.

## 6. Results

### 6.1 When there are results

- **A session whose slots are application memory always produces results** (rule MEM3): a slot is never reused before the application has read the result that frees it. This covers slots imported by an attach and slots over a region the application imported or allocated; a pool over another session's library pool counts as library memory.
- A library pool produces results only with `MTL_SESSION_RESULTS`. `MTL_INFO_RESULTS` in `mtl_session_info.flags` says what was granted.
- A pool attached over another session's library pool submits every unit with a hold (`HOLD_REQUIRED`); the hold, not a result, returns the bytes, so it may run without results (§9.6).
- Without results, outcomes go to counters and coalesced events; a non-zero cookie is `-MTL_EINVAL`, `COOKIE_WITHOUT_RESULTS`, in every build (G-47). A unit used as a template (`mtl_tx_write`, `mtl_tx_send_slot`) on such a session carries cookie 0, so a forwarder that passes a received unit clears its cookie first.
- There are no completion modes beyond results on or off: lateness also reaches counters and events, so reporting only exceptions would save reap bandwidth, not safety.

### 6.2 Lossless, ordered, exactly once

- **Every accepted TX submission has exactly one terminal outcome**: a result, or a counter increment when results are off (G-01, G-45).
- **Results cannot be lost.** The results ring holds `pool_count` entries and acquire reserves one, so producing a result never waits. With the results never read, acquire eventually reports `MTL_BLOCKED_RESULTS`; then every result is readable (G-04).
- **Results are published in submission order**, whatever context completed them. A unit that completes before an older one frees its slot at once; its result waits for its predecessors (G-09). `seq` is assigned at submit.
- `mtl_tx_reap(s, r, max, timeout)` (WT; `mtl_reap` with `sizeof(struct mtl_tx_result)`) returns up to `max` results: a count ≥ 1, or `-MTL_EAGAIN` (G-72). `mtl_tx_reap_full` reads the full records (§6.4).
- Identities a test can check at every instant: accepted = results published + results suppressed + units not yet terminal; at quiescence, accepted = published + suppressed; RX slots = free + receiving + ready + leased + held. The gauges satisfy `entries − exits = gauge` per lease state (G-43).

### 6.3 Statuses and reasons

Status 0 is never terminal (R3): a zeroed record never reads as `ON_TIME`.

The picture below shows which of the four statuses an accepted TX unit ends in. A unit removed
while queued is FLUSHED. A unit that cannot be sent is DROPPED. A unit whose launch index is no
longer feasible at pick-up gets `tx.late_policy`: DROP (the default for INDEX and TAI) or DEFER
(the default for AUTO). A unit that was built and paced is ON_TIME, or FAILED on a device or queue
failure. The table after it gives the reasons of each status.

```mermaid
flowchart TB
    S["accepted submit:<br/>seq assigned"] --> X{"removed while<br/>queued, or not<br/>sendable?"}
    X -->|"stop FLUSH, discard,<br/>close, abort, ERROR"| FL["MTL_TX_FLUSHED"]
    X -->|"not sendable:<br/>no neighbour, link down,<br/>every leg disabled"| DR["MTL_TX_DROPPED<br/>its index stays empty"]
    X -->|"no"| D{"launch index<br/>still feasible<br/>at pick-up?"}
    D -->|"no, DROP<br/>(INDEX, TAI)"| DR
    D -->|"no, DEFER<br/>(AUTO)"| RS["a later index,<br/>MTL_TXR_DEFERRED"]
    D -->|"yes"| B["built and paced"]
    RS --> B
    B -->|"sent"| OT["MTL_TX_ON_TIME"]
    B -->|"device or<br/>queue failure"| FA["MTL_TX_FAILED"]
```

| Status | Sent? | Meaning | Reasons |
|---|---|---|---|
| `MTL_TX_ON_TIME` | yes, on at least one leg | picked up by its deadline and scheduled at its launch index (an admission verdict; wire accuracy is reported, not graded); an AUTO unit that missed its index and took a later one is ON_TIME with `MTL_TXR_DEFERRED` | `NONE`; per leg `leg_reason` in the full record |
| `MTL_TX_DROPPED` | no; its index stays empty on the wire | not sent | `TOO_LATE`, `SNAP_COLLISION`, `DUPLICATE_INDEX`, `BEHIND`, `WAITING_NEIGHBOUR`, `LINK_DOWN`, `RECOVERY`, `INVALID_PAYLOAD`, `ENCODER_FAILED`, `REJECTED_AT_PICKUP`, `NO_KEY` (Phase 7), `LEG_DISABLED` (muted, Phase 7) |
| `MTL_TX_FLUSHED` | no | removed by stop, discard, close, abort or ERROR | `STOP_FLUSH`, `STOP_TIMEOUT`, `DISCARD`, `CLOSE`, `BEFORE_START`, `SESSION_ERROR`, `ABORTED` |
| `MTL_TX_FAILED` | partial or unknown | a device or queue failure; `error` has the code | `TX_QUEUE_FATAL`, `DEVICE_GONE`, `PORT_RESET`, `SESSION_ERROR` |

A muted session (every existing leg disabled, §14.2; Phase 7, later) counts no drops: its units retire at their indices with `MTL_TX_DROPPED`, reason `LEG_DISABLED`, and count only in `tx.units_muted`, never in `tx.units_dropped{reason}` (`mtl.h`, `legs_disabled`).

There is no late-sent status in v1: by default (`tx.late_policy`) a late INDEX or TAI unit is DROPPED and a late AUTO unit is ON_TIME at a later index; with `MTL_LATE_DROP` any mode gives `DROPPED`/`TOO_LATE`. A status for bounded send-late (`MTL_TX_LATE`, with `MTL_LATE_SEND_LATE`) is Phase 7 and declared only under `MTL_LATER`.

Under DROP, dropping one unit never shifts later units of it or of other sessions (G-24). A late unit gets the policy's outcome and its margins (G-28). Late policy, snapping and the horizon: [timing.md](timing.md) §6.

### 6.4 The records

`struct mtl_tx_result` (96 B) is the core: `status`, `reason`, `slot`, `flags`, `cookie`, `seq`,
`media_index`, `media_tai_ns`, `margin_ns` (deadline − submit; negative = late), `sent_tai_ns`
(first packet), `rtp` (first packet on the wire), `error` (for FAILED), `session`.
`mtl_tx_reap_full` (`mtl_observe.h`; `mtl_reap` with `sizeof(struct mtl_tx_result_full)`, 216 B)
fills the full record: submitted,
deadline, scheduled and enqueued times, observed times per leg, pick-up slack, snap error, packet
lateness, `indices_skipped_before`, audio sample counts, packets and reasons per leg, `path`, DMA
and partial-copy counts. A larger record is filled further; nothing is written beyond
`max × rec_size`.

| `flags` | Meaning |
|---|---|
| `MTL_TXR_MEDIA_VALID` | `media_index`, `media_tai_ns` |
| `MTL_TXR_MARGIN_VALID` | `margin_ns` |
| `MTL_TXR_SENT_VALID` | `sent_tai_ns` |
| `MTL_TXR_SENT_HW` | `sent_tai_ns` is a NIC timestamp |
| `MTL_TXR_ESTIMATED` | the time base was not locked |
| `MTL_TXR_SNAPPED` | a TAI media time moved to the grid |
| `MTL_TXR_DEFERRED` | an AUTO unit moved to a later index (`MTL_LATE_DEFER`) |
| `MTL_TXR_COPIED` | this unit took a copy path |
| `MTL_TXR_PKT_SHORT` | packet units, or a cut unit: fewer packets than declared |

### 6.5 RX outcomes

| Field | Meaning |
|---|---|
| `status` | `MTL_RX_COMPLETE`, or `MTL_RX_INCOMPLETE` (lost packets; library pools read them as zero). `rx.incomplete` = `MTL_RX_DELIVER` (default) or `MTL_RX_DISCARD` |
| `missed_before` | units the pool could not take since the last delivered one |
| `MTL_UNITF_INDEX_VALID`, `MTL_UNITF_TAI_VALID` | `media_index`, `media_tai_ns` are valid; arrival time is never substituted for media time (G-25) |
| `MTL_UNITF_USED_REDUNDANCY` | complete thanks to the other leg |
| `MTL_UNITF_DISCONTINUITY`, `MTL_UNITF_FORMAT_CHANGED` | the sender re-anchored; the stream differs from the config |
| `MTL_UNITF_RELOCKED` | with `MTL_UNITF_DISCONTINUITY`: the receiver relocked onto a restarted sender and `media_index` may go backwards ([timing.md](timing.md) §11.5) |
| `MTL_UNITF_SECOND_FIELD` | interlaced: the second field, from the stream |
| `MTL_UNITF_PARTIAL` | §5.3 |
| `MTL_UNITF_SENDER_TIME` | Phase 7 (`MTL_LATER`, R5) |

For a held unit: `mtl_rx_get_detail()` (DP) gives arrival per leg, presentation and delivery times,
packets expected, received and recovered, the format-changed mask and the path, and
`MTL_RXF_INCOMPLETE_BY_DUE` when the unit was force-completed at its due time rather than by a stop,
and the loss counts (`pkts_received[]`, `missing_ranges`: runs of lost packets); there is no
list of lost ranges. The detail's `timing[]` holds the ST 2110-21
results per leg when `rx.timing_parser` is on (`MTL_RXF_TIMING_LEG0`/`LEG1`; audio: DPVR, IPT,
TSDF).

`struct mtl_rx_timing_result` uses the names and formulas of RP 2110-25 §4 ([standards.md](standards.md) §6.3):

- `compliance`: `MTL_COMPLIANT_NARROW` (type N, gapped read schedule), `MTL_COMPLIANT_NARROW_LINEAR` (NL: linear, with the N limits), `MTL_COMPLIANT_WIDE` (W, linear) or `MTL_NOT_COMPLIANT` with `failed_cause`. NL and W are checked against the linear schedule (ST 2110-21 §7.1.3, §7.1.4), unlike the legacy parser, which knows only narrow and wide on the gapped one.
- The VRX read schedule starts at TVD = N × TFRAME + TROFFSET, TROFFSET = `sc.video.troffset_us` (the sender's SDP `TROFF`), 0 = TRODEFAULT (RP 2110-25 §4.9.2, shall); the legacy parser ignores TROFF.
- `fpt_ns` = TPA0 − TCF with TCF = N × TFRAME and N = round(TPA0 / TFRAME) (§4.4, §4.8.3), so it lies within ±½ frame and a packet slightly before N × TFRAME reads negative. EBU LIST and the legacy parser take the floor (`st_rx_timing_parser.c:21`); the binding converts (subtract TFRAME above TFRAME / 2).
- `rtp_offset_ticks` is RTPOFFSET (§4.8.4), `latency_ns` the video latency VL = TPA0 − RTP time (§4.8.5), `margin_ns` = TROFFSET − FPT (§4.8.6), `gap_ns` this unit's first packet minus the previous unit's last (§4.8.7; negative = reordering).
- `vrx_underflow` counts reads that found the buffer empty and `vrx_missing` packets absent at their read time (VRX_UNDERFLOW, VRX_PACKET_MISSING, §4.9.2), besides `vrx_min`, `vrx_max`, `vrx_avg_x100` and the CINST values.

## 7. Waiting

### 7.1 Calls with a timeout

- With timeout 0 a data call (acquire, dequeue, reap, read events, `mtl_wait`, wait rows) tries
  once; "nothing now" is `-MTL_EAGAIN`. It never sleeps and changes no wait state.
  `mtl_queue_wait` is the one call whose timeout 0 arms something: its queue's descriptor (§7.2).
- With a timeout it sleeps on its object until its target is ready, the timeout passes
  (`-MTL_EAGAIN`), it is interrupted (`-MTL_ECANCELED`, §7.3) or a state code holds (§7.4). A
  timeout is a duration on `CLOCK_MONOTONIC` fixed at entry; timer slack (50 µs by default) can
  lengthen it, never delays a wake-up.
- **Any number of threads** may sleep on one object, on one target or on different ones. A wake
  for a target reaches the threads sleeping on it and no other (G-95). There is no FIFO order: a
  woken thread may find the unit taken and sleep again. Beyond 127 threads on one target of one
  object, the others re-check every 1 ms instead of being woken ([D-167](decisions.md)). A
  sleeper is woken once its target is ready, whatever other threads do on the object (G-52).
- `mtl_wait(o, mask, timeout)` returns the ready subset of `mask` (> 0), consuming nothing. A mask
  of 0, or a bit not declared outside `MTL_LATER`, is `-MTL_EINVAL`; a declared bit of a later
  milestone is `-MTL_ENOTSUP` (R1). On an instance only `MTL_WAIT_EVENTS` applies (§10.1).

| Target | Ready when |
|---|---|
| `MTL_WAIT_ACQUIRE` | TX: acquire would succeed |
| `MTL_WAIT_DEQUEUE` | RX: dequeue would succeed |
| `MTL_WAIT_RESULTS` | TX: reap would return a result |
| `MTL_WAIT_EVENTS` | events are pending (`mtl_events.h`, MS3) |

`MTL_WAIT_RTCP` is Phase 7. Retirement has no wait target: poll it with `mtl_close` (§4.9).
`mtl_rx_wait_rows` (MS2) is woken when its `min_rows` are complete or the unit ends, and is
interrupted with `MTL_WAIT_DEQUEUE`.

- **Cancellation.** DP, DPC, WT and AS calls contain no cancellation point (raw `syscall()`). A
  thread sleeping in a call with a timeout is not ended by `pthread_cancel`: interrupt it (§7.3),
  then cancel or join it. Never leave an MTL call with `longjmp`, and never cancel a thread
  asynchronously inside one. A CP call may be a cancellation point (it can join threads, wait on a
  condition variable or write a log line): do not cancel a thread while it is inside one. A
  sleeping call that receives a cancel re-checks and sleeps again; it leaves by its target, its
  timeout, an interrupt or the state, releases its in-flight count normally, and the cancel then
  acts at the thread's next cancellation point (WH14b).
- **No sleeper, no syscall.** A thread that never sleeps causes no wake-up syscall (G-39), and a
  call that timed out costs nothing afterwards.

### 7.2 Queues (MS2)

- A **queue** (`mtl_queue_create`) tells an event loop which of its objects changed, through one
  descriptor: a Linux eventfd (poll `POLLIN`), a Windows manual-reset event from MS2a. A queue
  created without a descriptor is for a consumer that polls it on its own timer: it never costs a
  wake-up.
- **Arm what you want to hear about.** `mtl_queue_arm(q, o, targets, user)` arms targets of a
  session or of the instance. The first change of an armed target, or of `o`'s state, reports
  `o` once: `struct mtl_ready` gives `o`, `user` and what fired. A target already ready reports
  `o` at once. A queue reports only the objects armed on it; there are no user posts: an
  application wakes its own loop with `mtl_queue_interrupt` or its own descriptor.
- **A report disarms.** Serve `o` as far as you want with timeout-0 calls, then arm it again with
  the targets you want next. Nothing needs draining, because an arm that finds a target ready
  reports it at once. A sender limited by its source arms `MTL_WAIT_ACQUIRE` only while a frame
  waits. The arm returns 1 when a report of `o` is already on its way: it carries the new `user`,
  and you arm again after it.
- **One named slot.** `MTL_WAIT_ACQUIRE` reports any free slot, so a consumer that waits for one
  named slot (`mtl_tx_acquire_slot`, MS2b) sleeps in that call with a timeout, not on a queue: a
  queue would report it again on every arm while another slot is free.
- **State changes.** A start, a stop, the end of DRAIN or FLUSH, ERROR or a recovery reports
  an armed object once with `MTL_READY_STATE`. The object stays attached: read
  `mtl_session_get_status` and arm it again. Arming works in every state but CLOSING and
  RETIRED.
- **Detaching and `user`.** Targets 0 detaches. `mtl_queue_arm(q, o, 0, 0)` returns, and so does
  `o`'s close, only once every `mtl_queue_wait` that was handing out a report of `o` has returned;
  that wait is short and never covers a sleeping call. So no report carrying `o`'s `user` is
  returned after the detach or the close returned, and `user` may be freed then; free it only
  after one of them. A closing object is never reported; arming it returns `-MTL_ESHUTDOWN` and
  detaches it.
- **Sleep only after `-MTL_EAGAIN`.** `mtl_queue_wait(q, r, size, max, 0)` returns reports, or
  `-MTL_EAGAIN` with the descriptor armed; sleep on the descriptor only after that. Any epoll mode
  works, with any number of threads, if a loop leaves on `-MTL_ECANCELED` and `-MTL_ESHUTDOWN`. A
  wake-up with nothing to report costs one call. With a timeout, `mtl_queue_wait` sleeps on the
  descriptor itself. So "arm, call `mtl_queue_wait` until `-MTL_EAGAIN`, then sleep on the
  descriptor" never misses a report, with any number of threads on one queue, and a wake-up with
  nothing ready costs one call, never a busy loop (G-52).
- **Threads.** Each report goes to one caller. Any thread may call `mtl_queue_wait` at any time,
  also once and then never again. One write wakes every thread asleep on the descriptor; those
  that find nothing get `-MTL_EAGAIN` and sleep again. A pool of many threads on one busy queue
  therefore wakes as a herd: give each worker its own queue.
- A target is armed on at most one queue (`-MTL_EBUSY`), and an object on at most 4 queues
  (`-MTL_ENOSPC`). The targets of one object may go to different queues: acquire to one thread's
  queue, results to another's.
- **Cost.**
  - Reports and arms make no syscall, except an arm that reports at once to a sleeping queue (one
    `write()`).
  - A `-MTL_EAGAIN` with a descriptor costs one `read()`.
  - The first change after an arm costs the scheduler one `write()`, for the whole queue and every
    object that changes before the consumer comes back. A consumer that sleeps after each report
    therefore makes the scheduler write once per cycle.
  - A dense consumer (hundreds of sessions) polls a queue without a descriptor, or the sessions
    themselves, on its own timer.
- **Closing.** Closing a queue detaches every object, ends the waits in progress with
  `-MTL_ESHUTDOWN`, and returns 0; later calls on it, also a call that raced the close, are
  `-MTL_EBADF`. Remove the descriptor from your event loop first: MTL never closes a queue's
  descriptor while the process runs (a forked child closes it, R8), not even at the last
  `mtl_instance_close`, and hands it to a later queue, so the descriptors MTL holds are bounded by
  the peak number of live queues.
- The pattern is ex03; a framework with many sessions arms them all on one queue, and the
  instance's `MTL_WAIT_EVENTS` too (MS3).

The picture below shows one cycle of an event loop on a queue.

```mermaid
sequenceDiagram
    participant A as your event loop
    participant M as MTL
    A->>M: mtl_queue_arm(q, s, RESULTS | ACQUIRE, s)
    A->>M: mtl_queue_wait(q, r, size, 16, 0)
    M-->>A: -MTL_EAGAIN: the descriptor is armed
    A->>A: epoll_wait
    M->>M: a completion: s is reported, one write
    M-->>A: the descriptor is readable
    A->>M: mtl_queue_wait: report {s, fired}
    A->>M: serve s, then mtl_queue_arm(q, s, what you want next)
```

### 7.3 Interrupts

`mtl_interrupt(o, mode, targets)`: `mode` is `MTL_INTR_ON` (AS), `MTL_INTR_OFF` (CP) or
`MTL_INTR_ABORT` (instance only, targets 0, §2.4); `targets` is a `MTL_WAIT_*` mask, 0 = every
target. The checks:

- `-MTL_EINVAL`: an unknown mode, an undeclared bit, ABORT with targets or on a session or
  queue, and targets on a queue;
- `-MTL_ENOTSUP`: a declared bit of a later milestone.

They are checked first and returned without writing `mtl_last_error()`.

The picture below shows the ways in: the typed wrappers and a framework's unlock all call
`mtl_interrupt`, which sets a sticky flag on the selected targets and wakes their sleepers.

```mermaid
flowchart LR
    S["mtl_session_interrupt<br/>(s, 1)"] --> G["mtl_interrupt<br/>(o, MTL_INTR_ON, targets)"]
    I["mtl_instance_interrupt<br/>(mt, 1)<br/>signal handlers"] --> G
    Q["mtl_queue_interrupt<br/>(q, 1)"] --> G
    T["GStreamer unlock:<br/>MTL_WAIT_ACQUIRE"] --> G
    G --> F["sticky flag on the<br/>selected targets,<br/>their sleepers woken"]
    F --> W["calls with a timeout, mtl_wait<br/>and mtl_queue_wait return<br/>-MTL_ECANCELED until<br/>MTL_INTR_OFF"]
    C["stop or close"] --> X["-MTL_ESHUTDOWN:<br/>close wins over interrupt"]
    classDef app fill:#dbeafe,stroke:#2563eb,color:#111827
    classDef mtl fill:#dcfce7,stroke:#16a34a,color:#111827
    class S,I,Q,T,C app
    class G,F,W,X mtl
```

| Call | What returns `-MTL_ECANCELED` | Class |
|---|---|---|
| `mtl_session_interrupt(s, 1)` / `(s, 0)` | every call with a timeout on `s`, and `mtl_wait` with any timeout | AS / CP |
| `mtl_interrupt(MTL_OBJ_OF_SESSION(s), MTL_INTR_ON, MTL_WAIT_ACQUIRE)` | the acquire waits on `s` only | AS |
| `mtl_queue_interrupt(q, 1)` / `(q, 0)` | every `mtl_queue_wait` on `q`, with any timeout | AS / CP |
| `mtl_instance_interrupt(mt, 1)` / `(mt, 0)` | all of the above for the instance, its sessions and its queues | AS / CP |

- **Sticky.** While set, the waits above return `-MTL_ECANCELED`, at once if blocked, also when
  they start later and also when something is ready. Calls with timeout 0 other than `mtl_wait`
  and `mtl_queue_wait` are not interrupted: they do not wait.
- A thread whose `mtl_wait` mask holds an interrupted target reads `-MTL_ECANCELED` even if
  another target is ready: a reaper that must go on while acquire is interrupted waits on
  `MTL_WAIT_RESULTS` alone.
- **Scope.** Interrupting one session never wakes another's waiters, and with a target mask it
  ends only those targets' waits (G-64). A session's interrupt does not reach the queues it is
  armed on: interrupt the queue you sleep on, or the instance.
- **AS rules.** Atomics, the futex call and `write()`; `errno` kept; no thread-local state; never
  `mtl_last_error()`; in a `fork()`ed child `-MTL_EBADF`. The instance form visits every object
  the process created (at most 65 536), within the budget ST8 of
  [implementation-plan.md](implementation-plan.md) §8.4.
- **Interrupts cancel waits only**: stop and close still work and win. On a closing or retired
  handle a valid interrupt is R4's no-op (0); a stale generation is `-MTL_EBADF`.

### 7.4 Precedence

A wait returns the first of these that holds:

1. `-MTL_EBADF`;
2. `-MTL_ESHUTDOWN` (the object is closing, or draining or flushing with nothing left for the
   call; a queue closing under the wait);
3. the ERROR code (`-MTL_EIO`, `-MTL_ENODEV`);
4. `-MTL_ECANCELED`;
5. the result, or `-MTL_EAGAIN`.

`mtl_wait` and `mtl_queue_wait` apply `-MTL_ECANCELED` at any timeout, the other calls only with a
timeout. A queue reports a state change with `MTL_READY_STATE`, never as its own return.

### 7.5 Wake latency

A completing tasklet never makes the syscall: it marks the object or the queue, and its scheduler
wakes after its handler loop, a bounded number per iteration ([D-142](decisions.md)). Objects that
complete before the consumer comes back and are armed on one queue cost one wake-up together;
calls with a timeout on N objects completed in one iteration are woken `wake_k` per iteration
(D-142), so the k-th waits about ⌈k / `wake_k`⌉ × (one iteration + one wake): with `wake_k` 1
(MS1), 32 genlocked sessions × 17 µs ≈ 0.5 ms. Arm such objects on one queue to wake once.

## 8. Errors and reasons

### 8.1 Codes

| Code | Value | Meaning, and only this | Action |
|---|---|---|---|
| `MTL_EIO` | 5 | it failed: a session in ERROR, or a device that could not be stopped; the reason says why | report; stop then start, or close |
| `MTL_EBADF` | 9 | null, foreign or closed handle; any call in a forked child | bug |
| `MTL_EAGAIN` | 11 | nothing now, or by the timeout | wait, retry |
| `MTL_ENOMEM` | 12 | allocation failed | free memory |
| `MTL_EBUSY` | 16 | in use, or not allowed in this state | later, or change state |
| `MTL_EEXIST` | 17 | a session name in use, an incompatible shared open or EAL | another name, or match |
| `MTL_ENODEV` | 19 | device missing or removed, or its reset failed | another port, or give up |
| `MTL_EINVAL` | 22 | invalid argument; `mtl_last_error()` names the field | fix the call |
| `MTL_ENOSPC` | 28 | capacity: queues, lcores, regions, payload size, descriptors | free capacity |
| `MTL_ERANGE` | 34 | a time outside the accepted window, a pool above its maximum | fix the value |
| `MTL_EDEADLK` | 35 | not allowed from this thread: a library thread that the call would join or wait for (§10.4) | bug |
| `MTL_ENOTSUP` | 95 | a capability or option absent, or not implemented yet; a release build's `mtl_debug.h` | choose another |
| `MTL_ESHUTDOWN` | 108 | the application stopped or closed the session or instance | leave the loop |
| `MTL_ETIMEDOUT` | 110 | a control-plane deadline passed: a DRAIN stop only; never a close | the rest was flushed |
| `MTL_ESTALE` | 116 | a lease already returned | bug |
| `MTL_ECANCELED` | 125 | data waits interrupted; sticky until interrupt off | the framework's flushing path |

The reasons each code carries are the codes of §8.3's rows; each code has exactly this one
meaning, and each reason its documented trigger (G-57, G-70).

- `mtl_error_name(code)` (inline) and `mtl_reason_name(reason)` (AS) give the names for logs.
- A TX verb on an RX session, and the reverse, is `-MTL_EINVAL` and changes nothing (G-46).

### 8.2 Last error

- `mtl_last_error(&e, size)` (DP) copies the calling thread's last failure: `code`, `reason`, `field` (the config field or option at fault) and `detail`.
- **errno semantics**: a call that succeeds never writes it, so it stays valid until the thread's next failing call. A cleanup that succeeds after a failure (the close inside `mtl_session_open`) cannot clobber it (D-84). There is no call sequence counter.
- Data-path failures set `code` and `reason` only; `detail` stays empty.

### 8.3 Reasons

One vocabulary (`enum mtl_reason`, `mtl_reasons.h`) for `mtl_error_info.reason`, `status.reason` and `status.error_reason`, event reasons and TX result reasons. Values are grouped by hundreds so each group can grow, and are frozen once published.

The table is generated from `lib/src/unified/reasons.def` (README §1, from task H1b), which also
builds the enum of `mtl_reasons.h`, `mtl_reason_name()` and the G-57 trigger list: every value has
a trigger a test can produce, through `mtl_debug_inject` where no natural one exists (G-93). A
row marked `MTL_LATER` is declared only under `MTL_LATER`, with its value kept, and has no trigger
before its feature lands. 223 and 512 were never published and stay unused.
`MTL_FAULT_FORCE_ERROR` with its `reason` field set enters ERROR with that reason, the trigger of last resort for every ERROR reason. "Accompanies" names the code, event, status field or result that carries the reason.

<!-- BEGIN TABLE reasons: lib/src/unified/reasons.def; gen_api_doc.py writes this region once that file exists (README §1) -->

| Value | Name | Accompanies | Trigger a test can produce |
|---|---|---|---|
| 0 | `NONE` | — | no reason |
| **1–99** | **lifecycle** | | |
| 1 | `APP_REQUEST` | `MTL_EVENT_SESSION_STATE`, `status.reason` | a transition the application asked for: start, stop, close |
| 2 | `START_INSTANT` | `MTL_EVENT_SESSION_STATE` | ARMED → RUNNING when the start instant is reached: start `MTL_AT_TAI` ahead |
| 3 | `DRAIN_COMPLETE` | `MTL_EVENT_SESSION_STATE` | DRAINING → STOPPED: stop DRAIN with units queued |
| 4 | `DRAIN_TIMEOUT` | `MTL_EVENT_SESSION_STATE` (DRAINING → FLUSHING); the stop returns `-MTL_ETIMEDOUT` | stop DRAIN with a deadline shorter than the queue |
| 5 | `CMD_TIMEOUT` | ERROR (`status.error_reason`) | a command not acknowledged within the fixed ack timeout (D-48): a scheduler that stops looping, or `MTL_FAULT_FORCE_ERROR` |
| 6 | `FORCED` | ERROR | `mtl_debug_inject(…, MTL_FAULT_FORCE_ERROR, …)` (debug builds) |
| 7 | `WRONG_STATE` | `-MTL_EBUSY`, `-MTL_EIO`, `-MTL_ESHUTDOWN` | a call the state does not allow (§4.2), for example attach while RUNNING; an update while an earlier one is pending |
| 8 | `START_SET` | the start's failure, on the other sessions of the array | a start array in which one session cannot be armed: none starts (G-23) |
| 9 | `CLOSE_IN_PROGRESS` | `-MTL_ESHUTDOWN` | a call on a session in CLOSING (another thread called close) |
| 10 | `INSTANCE_SHUTDOWN` | `-MTL_ESHUTDOWN`, `MTL_EVENT_SESSION_STATE` | the instance's close or shutdown closed the session; any data call after it |
| 11 | `FORKED` | `-MTL_EBADF` | any call except close in a `fork()`ed child (R8) |
| 12 | `UPDATE_COMMITTING` | `-MTL_EBUSY` | Phase 7 (`MTL_LATER`): cancelling an update that is already committing; it applies |
| **100–199** | **device, port, network** | | |
| 100 | `TX_QUEUE_FATAL` | ERROR, `MTL_TX_FAILED` results | TX hang recovery that cannot get a new queue or mempool: `MTL_FAULT_TX_QUEUE_HANG` with the port's TX queues used up |
| 101 | `TX_QUEUE_HANG` | `MTL_EVENT_RECOVERY` | a TX hang recovery began (recoverable): `MTL_FAULT_TX_QUEUE_HANG` |
| 102 | `LINK_DOWN` | `MTL_EVENT_PORT_LINK`, `MTL_EVENT_LEG_STATE`, `MTL_TX_DROPPED` when no leg sent the unit | a link or leg goes down (recoverable): `MTL_FAULT_LEG_DOWN` |
| 103 | `PORT_RESET` | `MTL_EVENT_PORT_RESET`, `MTL_TX_FAILED` for units in the gap (§2.6) | a VF or PF reset: `MTL_FAULT_PORT_RESET` |
| 104 | `DEVICE_GONE` | ERROR, `-MTL_ENODEV`, `MTL_TX_FAILED` | the device is removed, or a reset after which queues and flows cannot be re-reserved: `MTL_FAULT_PORT_RESET` with `unrecoverable` |
| 105 | `BACKEND_FAILED` | ERROR | a kernel-socket or AF_XDP failure after retries; `MTL_FAULT_FORCE_ERROR` |
| 106 | `PORT_NOT_OPEN` | `-MTL_EINVAL` | a flow or option names a port the instance did not open |
| 107 | `WAITING_NEIGHBOUR` | `MTL_EVENT_FLOW_STATE`, the leg's `leg_reason`, `MTL_TX_DROPPED` when no leg is resolved | a unicast destination that does not answer ARP |
| 108 | `JOIN_FAILED` | `MTL_EVENT_FLOW_STATE` (`MTL_FLOW_JOIN_FAILED`) | the IGMP/MLD join or the flow rule failed at start or update |
| 109 | `LEG_DISABLED` | `MTL_EVENT_LEG_STATE`, the leg's `leg_reason` in the full TX record; with every leg disabled (mute) the `MTL_TX_DROPPED` result of each unit, counted in `tx.units_muted`, not in `tx.units_dropped` (§6.3) | a leg set in `legs_disabled` |
| 110 | `MANAGER_LOST` | `MTL_EVENT_MANAGER_LOST`, `-MTL_EBUSY` from a create whose configuration needs MtlManager | MtlManager unreachable: `MTL_FAULT_MANAGER_LOST` |
| 111 | `SCHED_OVERLOAD` | `MTL_EVENT_SCHED_OVERLOAD` | a scheduler above its busy threshold: more load than one scheduler sustains |
| 112 | `SCHED_STALLED` | `MTL_EVENT_SCHED_STALLED`, health `MTL_HEALTH_SCHED_STALLED` | a scheduler loop older than 1/8, 1/4, 1/2 and all of `instance.stall_ns` |
| 113 | `WORKER_STALLED` | health `MTL_HEALTH_WORKER_STALLED` | the admin worker misses its heartbeat outside a bounded device step |
| 114 | `QUEUE_QUARANTINED` | `-MTL_EIO` from instance close, `-MTL_EBUSY` from a later open on the port, health `MTL_HEALTH_DEVICE_FAULT` | a queue that cannot be stopped within the reset budget |
| 115 | `MCAST_FILTERS` | `-MTL_ENOSPC` | one group more than the VF's filter budget (`caps.mcast_filters_max`) |
| 116 | `DETECT_FAILED` | `status.format_reason` | detection cannot identify the stream's geometry; it keeps trying |
| **200–299** | **configuration and arguments** (`field` and `detail` name the culprit) | | |
| 200 | `INVALID_ARGUMENT` | `-MTL_EINVAL` | any argument check without a more specific reason |
| 201 | `UNKNOWN_BITS` | `-MTL_EINVAL` | an unknown flag bit |
| 202 | `NONZERO_TAIL` | `-MTL_EINVAL` | non-zero bytes beyond the known size, or `struct_size` 0 |
| 203 | `FIELD_REQUIRED` | `-MTL_EINVAL` | a required field left zero |
| 204 | `OTHER_ESSENCE` | `-MTL_EINVAL` | a non-zero member for another essence |
| 205 | `INSTANCE_MISMATCH` | `-MTL_EEXIST` | a shared open that names ports, lcores, time source or options that differ (§2.3); a NIC instance opened after a null-only instance initialised EAL (§2.1) |
| 206 | `OPTION_UNKNOWN` | `-MTL_EINVAL` | an unknown option key |
| 207 | `OPTION_RANGE` | `-MTL_EINVAL` | a value outside the key's range |
| 208 | `OPTION_STATE` | `-MTL_EBUSY` | `mtl_set_option` outside the key's C, S or R rule; a non-R key that differs in an update |
| 209 | `COOKIE_WITHOUT_RESULTS` | `-MTL_EINVAL` | a non-zero cookie on a session without results |
| 210 | `MEDIA_TIME_BACKWARDS` | `-MTL_EINVAL` | a backward jump flagged `MTL_SUBMIT_DISCONTINUITY` while RUNNING (a backward re-anchor needs stop and start, [timing.md](timing.md) §4.4) |
| 211 | `RTP_TS_AUTO` | `-MTL_EINVAL` | `MTL_SUBMIT_RTP_TS` with `MTL_MEDIA_AUTO` |
| 212 | `LAYOUT_MISMATCH` | `-MTL_EINVAL` | an attached layout that misses a requirement; an attached pool that no longer fits after a MEDIA or POOL update |
| 213 | `STRIDE_MISMATCH` | `-MTL_EINVAL` | attached slots of one pool with different strides |
| 214 | `ACCESS_MISMATCH` | `-MTL_EINVAL` | an RX pool over a region without `MTL_MEM_WRITE`, a TX pool without `MTL_MEM_READ` (G-101) |
| 215 | `UNALIGNED` | `-MTL_EINVAL` | an import or attach whose `va` or `length` is not page aligned (hugepage aligned for hugetlbfs) |
| 216 | `SPAN` | `-MTL_EINVAL` | a slot outside its region |
| 217 | `MIXED_BACKING` | `-MTL_EINVAL` | an import spanning mappings of different backings |
| 218 | `POOL_TOO_SMALL` | `-MTL_EINVAL`, `-MTL_ENOTSUP` | start with fewer slots attached than `pool_count`; a pool below `min_count_direct` with `MTL_SESSION_REQUIRE_DIRECT` |
| 219 | `HOLD_REQUIRED` | `-MTL_EINVAL` | a submit without `hold` on a pool attached over another session's pool |
| 220 | `NAME_EXISTS` | `-MTL_EEXIST` | a session name used by a live or closing session |
| 221 | `TIMELINE_MISMATCH` | `-MTL_EEXIST` | reserved for later (`MTL_LATER`): a named timeline created again with another config |
| 222 | `START_SET_MIXED` | `-MTL_EINVAL` | a start array that mixes directions, or holds an AUTO TX session |
| 224 | `PORT_CHANGE_NEEDS_STOP` | `-MTL_EBUSY` | an update that moves a leg to another port while running, on a backend that cannot make before break |
| 225 | `GRID_MISMATCH` | `-MTL_EINVAL` | reserved for later (`MTL_LATER`): a unit period that does not fit a created timeline's grid |
| 226 | `PKT_CONFIG` | `-MTL_EINVAL` | packet sizes or counts that do not fit the port or the pacing class |
| 227 | `LIBRARY_THREAD` | `-MTL_EDEADLK` | the instance's open, close or shutdown from a log-sink or codec thread |
| 228 | `NOT_IMPLEMENTED` | `-MTL_ENOTSUP` | a known function, flag, enum value, update part, `when` kind, wait mask, option key or port prefix this library does not implement yet; `field` names it (R1) |
| 229 | `RASTER_MISMATCH` | `-MTL_EINVAL` from start; `status.format_reason` | an ANC or grid-fastmeta session whose `anc.video` or `fastmeta.video` has another rate or raster than the first video session of its start ([timing.md](timing.md) §7.1); an RX stream outside the configured raster, or above the maximum with detection |
| 230 | `RX_TIMELINE_LAZY` | `-MTL_EINVAL` from create | reserved for later (`MTL_LATER`): an RX session on a lazily anchored timeline |
| 231 | `BACKEND_REMOVED` | `-MTL_ENOTSUP` from open | a port name with a removed prefix, `"dpdk_af_xdp:"` or `"dpdk_af_packet:"`; `detail` names the replacement (§2.1) |
| 232 | `OPTION_WITHDRAWN` | `-MTL_ENOTSUP` | a provisional option key this library withdrew; `field` names it (§12.6) |
| 233 | `FIELD_RATE` | `-MTL_EINVAL` | an interlaced or PsF `raster.fps` above 30 frames per second, a field rate given as the frame rate (§3.3) |
| 234 | `NOT_APPLICABLE` | `-MTL_EINVAL` | a key that does not apply to the session's essence, direction or unit, an instance or port key on a session, or a value outside the essences and directions it applies to; `field` names it (§12.1) |
| **300–399** | **capacity and memory** | | |
| 300 | `CAPACITY_TX_QUEUES` | `-MTL_ENOSPC` | no free TX queue (video never falls back to a shared one, §12.8) |
| 301 | `CAPACITY_RX_QUEUES` | `-MTL_ENOSPC` | no free RX queue or flow rule |
| 302 | `CAPACITY_RL_QUEUES` | `-MTL_ENOSPC`, `-MTL_ENOTSUP` | no free rate-limited queue |
| 303 | `CAPACITY_SCHED_QUOTA` | `-MTL_ENOSPC` | no scheduler has quota left (`instance.sched_quota_mbs`) |
| 304 | `CAPACITY_LCORES` | `-MTL_ENOSPC` | no free CPU for a new scheduler |
| 305 | `CAPACITY_SESSIONS` | `-MTL_ENOSPC` | a scheduler's session table is full (60 video TX, §3.5) |
| 306 | `REGION_BUDGET` | `-MTL_ENOSPC` | an import or attach beyond `caps.max_regions` (G-102) |
| 307 | `CODESTREAM_OVERSIZE` | `-MTL_ENOSPC` | a cvideo codestream larger than granted, at submit (G-66) |
| 308 | `HUGEPAGES` | `-MTL_ENOMEM` | a hugepage allocation fails at create or `mtl_mem_alloc` |
| 309 | `NUMA_MISMATCH` | `-MTL_EINVAL` with `MTL_MEM_NUMA_CHECK`; otherwise `info.numa_mismatch` only | memory not on the node it must be on |
| 310 | `DIRECT_IMPOSSIBLE` | `-MTL_ENOTSUP` | `MTL_SESSION_REQUIRE_DIRECT` with a unit that would be copied or converted |
| 311 | `PACING_UNAVAILABLE` | `-MTL_ENOTSUP` | `caps.pacing_req` = `MTL_REQ_REQUIRE` with a `caps.pacing` class the port cannot grant (§12.7) |
| 312 | `POOL_COUNT_MAX` | `-MTL_ERANGE` | `pool_count` above `max_count`, at query and create (G-103) |
| 313 | `RX_RING_BUDGET` | `-MTL_ENOSPC` | packet RX `rx_ring_packets` beyond the pool headroom |
| 314 | `PROVIDE_FULL` | `-MTL_ENOSPC` | `mtl_rx_provide` with `pool_count` destinations held (§9.11) |
| **400–499** | **timing** | | |
| 400 | `BEYOND_HORIZON` | `-MTL_ERANGE` from start or submit, else the late policy's result | a preroll unit or a launch beyond `tx.horizon_ns` from the resolved start (G-94) |
| 401 | `START_IN_PAST` | `-MTL_ERANGE` | `MTL_AT_INDEX` or `MTL_AT_TAI` whose instant is closer than now + lead |
| 402 | `LAUNCH_IN_PAST` | `-MTL_ERANGE` at submit when knowable, else the late policy's result | a NOT_BEFORE or EXACT launch in the past or inside the minimum lead (G-26) |
| 403 | `TIMING_SHORTFALL` | `status.timing_reason`, `MTL_EVENT_TIMING_INFEASIBLE` | units of a capture producer (a session with a non-zero `min_tx_delay_ns`) that arrive after their deadline |
| 404 | `LINK_OFFSET_BUDGET` | `status.timing_reason` | a launch delay above `tx.link_offset_budget_ns`; never a rejection |
| 405 | `TIME_ESTIMATED` | `status.timing_reason`, `MTL_EVENT_TIME_STATE` | an estimated time source: AUTO on a host with no disciplined PHC and a kernel TAI offset of 0 falls back to `SYSTEM_TAI` |
| 406 | `TIME_STEP` | `MTL_EVENT_TIME_STEP`, a pending update's `update_reason` | the time base steps: `MTL_FAULT_TIME_STEP` |
| 407 | `OFF_GRID_PHASE` | `status.timing_reason` | reserved for later (`MTL_LATER`): a locked-phase snap relocked or dropped off-grid units |
| **500–599** | **why a TX unit was not on time** (`mtl_tx_result.reason`) | | |
| 500 | `TOO_LATE` | `MTL_TX_DROPPED` | a unit picked up after its deadline: by default (`tx.late_policy`) an INDEX or TAI unit; with `MTL_LATE_DROP` any mode |
| 501 | `WOULD_OVERLAP` | `MTL_TX_DROPPED` | Phase 7 (`MTL_LATER`): a late unit that bounded send-late cannot fit before the next occupied slot |
| 502 | `SNAP_COLLISION` | `MTL_TX_DROPPED` | TAI: two units snapped to one media index (the launch decision, [timing.md](timing.md) §6.8) |
| 503 | `DUPLICATE_INDEX` | `MTL_TX_DROPPED` | INDEX: an index equal to the last one |
| 504 | `BEHIND` | `MTL_TX_DROPPED` | INDEX: an index smaller than the last one; TAI: a unit that snaps to a slot before the last one |
| 505 | `OFF_GRID` | `MTL_TX_DROPPED` | reserved for later (`MTL_LATER`): a TAI media time off a locked phase |
| 506 | `RECOVERY` | `MTL_TX_DROPPED` | a unit in the gap of a TX hang recovery |
| 507 | `INVALID_PAYLOAD` | `MTL_TX_DROPPED` | a payload the builder cannot packetise at pick-up (not an oversize codestream: that is `CODESTREAM_OVERSIZE` at submit) |
| 508 | `ENCODER_FAILED` | `MTL_TX_DROPPED` | the cvideo encoder plugin failed (a test codec device that fails) |
| 509 | `REJECTED_AT_PICKUP` | `MTL_TX_DROPPED` | the engine claimed the unit and did not build it ([engine.md](engine.md)) |
| 510 | `STOP_FLUSH` | `MTL_TX_FLUSHED` | `mtl_session_stop(…, MTL_STOP_FLUSH, …)` with units queued |
| 511 | `DISCARD` | `MTL_TX_FLUSHED` | `mtl_session_discard` with units queued |
| 513 | `CLOSE` | `MTL_TX_FLUSHED` | instance close or shutdown with units queued |
| 514 | `BEFORE_START` | `MTL_TX_FLUSHED` | a preroll unit whose media time precedes the start instant |
| 515 | `SESSION_ERROR` | `MTL_TX_FLUSHED`, `MTL_TX_FAILED` | the session entered ERROR with units queued or in flight |
| 516 | `PKT_COUNT` | `-MTL_EINVAL` at submit | packet units: a chunk that takes its unit past `packets_per_unit` |
| 517 | `PKT_INVALID` | the chunk's result | packet units: `MTL_PKT_TX_VALIDATE` found a malformed packet |
| 518 | `STOP_TIMEOUT` | `MTL_TX_FLUSHED` | a DRAIN stop or a session close whose deadline passed with units queued |
| 519 | `NO_KEY` | `MTL_TX_DROPPED` | Phase 7 (`MTL_LATER`): an encrypted session without a key for the unit |
| 520 | `ABORTED` | `MTL_TX_FLUSHED` | `mtl_instance_abort` |
| **600–699** | **the environment** (checked at open, [deployment.md](deployment.md)) | the code `mtl_instance_open` returns; `VF_UNTRUSTED` comes from create | |
| 600 | `NO_IOMMU` | `-MTL_ENOTSUP` | no-IOMMU or PA mode without `instance.allow_noiommu` |
| 601 | `DEVICE_NODE_MISSING` | `-MTL_ENODEV` | `/dev/vfio/N` not in the container |
| 602 | `DRIVER_MISMATCH` | `-MTL_ENODEV` | the device is not bound to the expected driver |
| 603 | `HUGEPAGES_LIMIT` | `-MTL_ENOMEM` at open and at create | the cgroup hugetlb limit or the free pages are short |
| 604 | `MEMLOCK_LIMIT` | `-MTL_ENOMEM` | `RLIMIT_MEMLOCK` short and `CAP_IPC_LOCK` not effective |
| 605 | `CAPABILITY_MISSING` | `-MTL_ENOTSUP` | a capability the backend needs is not effective; `detail` names it |
| 606 | `CPU_NOT_ALLOWED` | `-MTL_EINVAL` | a CPU outside the affinity mask |
| 607 | `CPU_SHARED` | `-MTL_ENOSPC` (with `MTL_CPU_SHARED_REFUSE`) | busy-poll CPUs under a CFS quota below the mask |
| 608 | `PORT_ENV_UNSET` | `-MTL_EINVAL` | `env:VAR` unset, or `#n` out of range |
| 609 | `RUNTIME_DIR` | `-MTL_EINVAL` | `instance.runtime_dir` missing or not writable |
| 610 | `MANAGER_REQUIRED` | `-MTL_ENOTSUP` | the configuration needs MtlManager and it is absent |
| 611 | `XSK_UNAVAILABLE` | `-MTL_ENODEV` | AF_XDP: no socket or map from the node daemon (`port.xsk_map`) |
| 612 | `VF_UNTRUSTED` | `-MTL_ENOTSUP` from create | a request that needs a trusted VF |
| 613 | `TIME_SOURCE_UNAVAILABLE` | `-MTL_ENOTSUP` | the named time source cannot be read (`CLOCK_TAI` with a kernel offset of 0) |
| 614 | `CLOCK_NOT_OWNED` | `-MTL_EINVAL` | `PTP_BUILTIN` asked to steer a PHC MTL does not own |
| 615 | `DESCRIPTOR_LIMIT` | `-MTL_ENOSPC` | no descriptor for a queue (`RLIMIT_NOFILE` or the system's file limit) |

<!-- END TABLE reasons -->

### 8.4 Codes per call

The codes each call can return besides `-MTL_EBADF`, which every call but close returns for a null, foreign or retired handle (bindings map each code to one exception). Codes drop the `-MTL_` prefix.
State-dependent codes follow §4.2; "W" means only with a timeout other than 0. Every call that takes a timeout returns `EINVAL` for one below −1 (R2). A specific reason is named where one code has one. The header does not list codes per function; this table is normative. It is generated from
`lib/src/unified/codes_per_call.def` (from task H1b), which debug builds also use to check every
return of an exported call, so every U test checks it.

<!-- BEGIN TABLE codes-per-call: lib/src/unified/codes_per_call.def; gen_api_doc.py writes this region once that file exists (README §1) -->

| Call | Codes (reasons) |
|---|---|
| `mtl_instance_open` | `EINVAL` (no port, `PORT_ENV_UNSET`, `CPU_NOT_ALLOWED`, `RUNTIME_DIR`, `CLOCK_NOT_OWNED`, `PORT_NOT_OPEN`, option reasons), `EEXIST` (`INSTANCE_MISMATCH`: a shared open that differs, or a NIC port after a null-only instance initialised EAL), `EBUSY` (`QUEUE_QUARANTINED`), `ENOMEM` (`HUGEPAGES_LIMIT`, `MEMLOCK_LIMIT`), `EDEADLK` (`LIBRARY_THREAD`) |
| `mtl_instance_open`, continued | `ENODEV` (`DEVICE_NODE_MISSING`, `DRIVER_MISMATCH`, `XSK_UNAVAILABLE`), `ENOSPC` (`CPU_SHARED`), `ENOTSUP` (`NO_IOMMU`, `CAPABILITY_MISSING`, `MANAGER_REQUIRED`, `TIME_SOURCE_UNAVAILABLE`, `BACKEND_REMOVED`, `NOT_IMPLEMENTED`: field `ports`, a PCI or `native_af_xdp:` port until MS2a). Each environment reason has exactly this one code (§8.3) |
| `mtl_instance_from_legacy` | `ENOTSUP` (`NOT_IMPLEMENTED`: a second wrap of the same handle, until MS2a) |
| `mtl_instance_close` | returns 0 or 1, also when called again (it polls); `EIO` (`QUEUE_QUARANTINED`); `EDEADLK` (`LIBRARY_THREAD`, no effect). Never `ETIMEDOUT` |
| `mtl_interrupt` (`mtl_instance_abort` and the instance, session and queue `_interrupt`) | `EINVAL` (mode, a target bit not declared outside `MTL_LATER`, ABORT with targets, ABORT on another kind, targets on a queue), `ENOTSUP` (a later milestone's target bit), `EBADF` (a stale generation, or a forked child); 0 on a closing or retired handle (R4); `mtl_last_error()` never written |
| `mtl_session_query`, `mtl_session_create` | `EINVAL` (config reasons), `EEXIST` (`NAME_EXISTS`), `ENOSPC` (`CAPACITY_*`, `MCAST_FILTERS`, `REGION_BUDGET`), `ERANGE` (`POOL_COUNT_MAX`), `ENOMEM` (`HUGEPAGES`, `HUGEPAGES_LIMIT`), `ENOTSUP` (`DIRECT_IMPOSSIBLE`, `PACING_UNAVAILABLE`, `CAPACITY_RL_QUEUES`, `POOL_TOO_SMALL`, `VF_UNTRUSTED`, `NOT_IMPLEMENTED`), `EBUSY` (`MANAGER_LOST`), `ESHUTDOWN` |
| `mtl_session_open` (inline) | the codes of create and of start |
| `mtl_session_get_info` | `EINVAL` (size), `ESHUTDOWN` (CLOSING) |
| `mtl_session_start` | `EINVAL` (`START_SET_MIXED`, `POOL_TOO_SMALL`, `RASTER_MISMATCH`, `FIELD_REQUIRED`: ANC without a raster or a video; `MTL_WHEN_ORIGIN` on RX or with `MTL_AT_INDEX`) |
| `mtl_session_start`, continued | `EBUSY` (`WRONG_STATE`), `EIO` (ERROR), `ENODEV` (re-reserve after ERROR failed), `ENOMEM` (re-reservation after ERROR), `ENOTSUP` (a capability lost since create, for example the pacing class; `NOT_IMPLEMENTED`), `ERANGE` (`BEYOND_HORIZON`, `START_IN_PAST`), `ENOSPC`, `ESHUTDOWN` |
| `mtl_session_stop` | `ETIMEDOUT` (a DRAIN deadline), `EINVAL` (mode), `EIO` (a fault during the drain or flush: the session is STOPPED and `mtl_last_error()` carries the fault's reason, §4.8), `ESHUTDOWN` |
| `mtl_session_update` | `EINVAL` (`LAYOUT_MISMATCH`, an invalid member named by `parts`), `EBUSY` (`WRONG_STATE`: the state, or an update pending; `OPTION_STATE`, `PORT_CHANGE_NEEDS_STOP`, library pool slots out), `EIO`, `ENODEV` (a leg's port gone), `ENOMEM`, `ENOSPC`, `ENOTSUP` (a flow or leg the backend cannot carry; `NOT_IMPLEMENTED`, field `state`: ARMED and RUNNING until MS5), `ESHUTDOWN` |
| `mtl_session_discard` | `EBUSY` (DRAINING), `ESHUTDOWN` |
| `mtl_session_close` | returns 0 or 1, also when called again on the same handle (it polls); never `EBADF` for a handle it closed |
| `mtl_session_get_status` (`mtl_session_get_state`) | the state (≥ 0); `EINVAL` (size) |
| `mtl_tx_acquire`, `mtl_tx_acquire_slot` | `EAGAIN`, `ECANCELED` (W), `ESHUTDOWN`, `EIO`, `ENODEV`, `EINVAL` (slot) |
| `mtl_tx_submit` | `EINVAL` (`COOKIE_WITHOUT_RESULTS`, `HOLD_REQUIRED`, `MEDIA_TIME_BACKWARDS`, `RTP_TS_AUTO`, `PKT_COUNT`; `MTL_SUBMIT_EXACT` without `MTL_SESSION_EXACT_LAUNCH`; `MTL_SUBMIT_SRC_PLANES` with `MTL_SESSION_REQUIRE_DIRECT`; a submit without `MTL_SUBMIT_SRC_PLANES` on an `MTL_SESSION_TX_SRC_PLANES` session; audio `used` 0) |
| `mtl_tx_submit`, continued | `ENOTSUP` (`NOT_IMPLEMENTED`; `MTL_SUBMIT_SRC_PLANES` with an asynchronous converter that cannot convert in the caller), `ENOSPC` (`CODESTREAM_OVERSIZE`), `ERANGE` (`BEYOND_HORIZON`, `LAUNCH_IN_PAST`), `EBUSY` (a 256th hold on one RX slot), `ESTALE`, `ESHUTDOWN`, `EIO` |
| `mtl_release` (`mtl_tx_release`, `mtl_rx_release`) | `ESTALE`, `EINVAL`; 0 on an object the instance closed (R4) |
| `mtl_session_get_slot` | `EINVAL` (slot), `ESHUTDOWN` |
| `mtl_tx_write`, `mtl_tx_send_slot` (inline) | the codes of acquire, `mtl_tx_get_next` and submit |
| `mtl_reap`, `mtl_read_events` (`mtl_tx_reap`, `mtl_tx_reap_full`, `mtl_session_read_events`, `mtl_instance_read_events`) | `EAGAIN`, `ECANCELED` (W), `EINVAL` (record size), `ESHUTDOWN` (CLOSING) |
| `mtl_rx_dequeue` | `EAGAIN`, `ECANCELED` (W), `ESHUTDOWN`, `EIO`, `ENODEV`, `EINVAL` |
| `mtl_wait` (`mtl_session_wait`), `mtl_rx_wait_rows` | `EAGAIN`, `ECANCELED` (`mtl_wait`: any timeout; `mtl_rx_wait_rows`: with a timeout), `ESHUTDOWN`, `EIO`, `ENODEV`, `EINVAL` (mask 0 or unknown), `ENOTSUP` (a later target), `ESTALE` (`mtl_rx_wait_rows`: lease) |
| `mtl_queue_create` | `ENOSPC` (`DESCRIPTOR_LIMIT`), `EINVAL` (flags) |
| `mtl_queue_arm` | `EBUSY` (a target armed on another queue), `ENOSPC` (the object on its limit of queues, §7.2), `ESHUTDOWN` (the object closing; it is detached), `EBADF` (the object retired, R4), `EINVAL` |
| `mtl_queue_wait` | `EAGAIN`, `ECANCELED` (any timeout), `ESHUTDOWN` (the queue closed during the wait), `EBADF` (a call after the close), `EINVAL` (a timeout on a queue without a descriptor) |
| `mtl_last_error`, `mtl_time_now`, `mtl_instance_get_health` | `EINVAL` (size); health `ESHUTDOWN` after close |
| `mtl_stat_read` | `ESTALE` (schema changed), `EINVAL` |
| `mtl_rx_get_detail` | `ESTALE` (lease), `EINVAL` |
| `mtl_mem_open` (`mtl_mem_import`, `mtl_mem_alloc`) | `EINVAL` (`UNALIGNED`, `MIXED_BACKING`, `NUMA_MISMATCH`), `ENOSPC` (`REGION_BUDGET`), `ENOMEM` (`HUGEPAGES`), `ENOTSUP` (`NOT_IMPLEMENTED` for `MTL_MEM_DEVICE` until MS6) |
| `mtl_close` for a region or plugin (`mtl_mem_close`, `mtl_plugin_unload`) | returns 0, or 1 for a region still referenced (again: it polls); a plugin in use `EBUSY` (not consumed) |
| `mtl_session_attach` (NULL detaches) | `EINVAL` (`SPAN`, `STRIDE_MISMATCH`, `UNALIGNED`, `ACCESS_MISMATCH`, `LAYOUT_MISMATCH`), `ENOSPC` (`REGION_BUDGET`), `EBUSY` (state; detach with leases or sessions over the pool), `ESHUTDOWN` |
| `mtl_set_options` (`mtl_set_option`) | `EINVAL` (`OPTION_UNKNOWN`, `OPTION_RANGE`), `EBUSY` (`OPTION_STATE`; nothing applied), `ENOTSUP` (`NOT_IMPLEMENTED`) |
| every function of `mtl_debug.h` in a release build | `ENOTSUP` (G-93) |

<!-- END TABLE codes-per-call -->

## 9. Memory

Memory is four separate ideas, in the picture below: a region is where the bytes live, a slot is
one buffer of a pool, a lease is who may touch a slot now, and a unit with its result is what this
use means. Who allocated the bytes, who may touch them now and what this use means are separate
questions, so library and application memory run one data path.

```mermaid
flowchart LR
    R["region:<br/>where bytes live<br/>VA, length,<br/>backing, NUMA,<br/>IOVA per device,<br/>refcount"] --> S["slot:<br/>one buffer of a pool,<br/>named by its index;<br/>planes and meta,<br/>layout fixed<br/>when attached"]
    S --> L["lease:<br/>who may touch it now<br/>the slot state:<br/>FREE, APP, QUEUED,<br/>XFORM, ENGINE,<br/>PUBLISHED, HELD;<br/>the generation<br/>in the lease"]
    L --> U["unit and result:<br/>this use<br/>media time, cookie,<br/>hold, launch;<br/>one terminal outcome"]
```

A session's slots come from one of the sources in the next picture, and every source ends in the
same slots and the same data path: the library pool (§9.1), a region (§9.2) that the application
attaches (§9.3), or memory imported by the attach itself (§9.3).

```mermaid
flowchart TB
    LP["library pool<br/>default, mtl.h"] --> SL["the session's slots"]
    subgraph REG["a region: mtl_region_h"]
        direction LR
        MA["mtl_mem_open,<br/>va NULL<br/>library hugepages"]
        MI["mtl_mem_open,<br/>va set<br/>application memory"]
        PR["mtl_session_get_<br/>pool_region<br/>another session's pool"]
        MD["mtl_mem_open,<br/>MTL_MEM_DEVICE<br/>GPU memory (MS6)"]
    end
    REG -->|"mtl_session_attach"| SL
    AV["mtl_session_attach<br/>with va: imported<br/>for this session"] --> SL
    SL --> V["one data path:<br/>acquire, submit,<br/>dequeue, release"]
```

Application memory always produces results (MEM3, §6.1); a pool over another session's library
pool submits every unit with a hold (`HOLD_REQUIRED`, §9.6). Per-acquire layouts
(`mtl_tx_acquire_layout`) and per-unit RX destinations (`mtl_rx_provide`) come in MS2 (§9.11),
device memory in MS6. On any session but a `MTL_SESSION_REQUIRE_DIRECT` one,
`MTL_SUBMIT_SRC_PLANES` copies one unit from caller memory into the slot during submit (§5.2).

### 9.1 Library pools

- By default MTL allocates the pool (`pool_count` slots) at create, on the session's NUMA node, and never later: no data-path call allocates (R6). Nothing from `mtl_mem.h` is needed. A converting TX pool with `MTL_SESSION_TX_SRC_PLANES` has no app-format planes (§3.4).
- `mtl_session_info.unit_bytes` is one unit in the application layout; `mtl_session_info.pool_slot_pitch` the distance between slots.
- `video.linesize[]` sets the bytes per row of a library pool (legacy `linesize`; 0 = packed).
- Library pools are EAL hugepage memory: they need no mapping and consume no region budget.
- `mtl_session_get_pool_region(s, &r)` exposes a library pool as a region for forwarding (§9.6).
- **Video and cvideo RX library slots** (D-151).
  - With `video.linesize[]` 0, the planes of a slot lie back to back at the packed stride: plane 0
    at the slot start, plane p + 1 at plane p + `stride[p]` × `rows[p]`, with `stride` =
    `row_bytes`. That is the legacy layout (`st_fmt.c:1283-1297`) and FFmpeg's
    `av_image_fill_arrays(..., align 1)`. With `linesize[]` set, `stride` = `linesize`, and planes
    follow the same rule.
  - Each slot is its own allocation of `round_up(unit_bytes + MTL_RX_TAIL_BYTES, 64)` bytes, 64 B
    aligned. `pool_slot_pitch` is that size. `unit_bytes` is that of the maximum with detection
    (§3.3); a smaller detected format is laid out from the slot start for its own size.
- **V210.** With `app_format` `MTL_APP_V210`, rows are ((width + 47) / 48) × 128 bytes, the last
  6-pixel group padded: the v210 row of GStreamer and FFmpeg. The legacy V210 frame is one
  contiguous run of 8/3 bytes per pixel (`st_fmt.c:559-565`), equal to these rows only when the
  width is a multiple of 48. Until the converter writes v210 rows (E15, MS2a), another width is
  `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, field `video.app_format`).
- **GStreamer.** Without a `GstVideoMeta`, downstream assumes `gst_video_info_set_format`'s layout
  (strides rounded to 4). The packed layout equals it when every plane's row bytes are a multiple
  of 4: UYVY at even widths, planar 4:2:2 at widths that are a multiple of 4, V210 by the rule
  above. Otherwise attach a `GstVideoMeta` with `offset[p]` = Σ over q < p of `stride[q]` ×
  `rows[q]` and MTL's strides, and copy when downstream does not list `GST_VIDEO_META_API_TYPE`
  ([migration.md](migration.md) §12.8).

### 9.2 Regions

| Rule | Detail |
|---|---|
| MEM1 | A region (`mtl_region_h`) is memory MTL may DMA: page aligned (hugepage aligned for hugetlbfs), refcounted, mapped into every port and DMA engine a session using it needs (G-10, G-16). `mtl_mem_close()` retires it once no session, slot, in-flight unit or DMA references it |
| MEM2 | A slot is one buffer of one session's pool, named by its index. Its layout is fixed when it is attached. Any stride ≥ `row_bytes` is direct (no copy) |
| MEM3 | A session whose slots are application memory always produces results |

- `mtl_mem_open(mt, &d, &r, &va)` (CP) registers `[d.va, d.va + length)`, or allocates library
  hugepages when `d.va` is NULL; `va` (`MTL_ADDR(void)*`, may be NULL) receives the start either
  way. The inline `mtl_mem_import` and `mtl_mem_alloc` name the two uses. `MTL_MEM_DEVICE`
  with `d.device` (a dma-buf fd or device address) is device memory, in MS6 (`-MTL_ENOTSUP`,
  `NOT_IMPLEMENTED`, until then). `struct mtl_mem_desc.numa`: allocation placement, or with `MTL_MEM_NUMA_CHECK` the node the pages must be on; node + 1, 0 = the ports'.
- Flags: `MTL_MEM_READ` (TX reads it), `MTL_MEM_WRITE` (RX writes it), 0 = both; `MTL_MEM_MAP_ALL` maps into every port now or fails; `MTL_MEM_NUMA_CHECK` fails unless the pages are on `numa` (`NUMA_MISMATCH`), and makes an attach fail the same way for a session on another socket than the pages. Without it a mismatch is reported (`mtl_mem_info.numa`, `info.numa_mismatch`), never rejected.
- `va` and `length` must be page aligned (hugepage aligned for hugetlbfs): `-MTL_EINVAL`, `UNALIGNED`, and nothing is mapped. MTL never rounds outward. An aligned import maps exactly `[va, va + length)` (G-96).
- A range spanning mappings of different backings is `-MTL_EINVAL`, `MIXED_BACKING`.
- Page-cache file mappings (`MTL_BACKING_FILE`) are accepted copy-only (`mtl_mem_info.direct` = 0); MTL never DMA-maps them.
- A read-only CPU mapping (`PROT_READ`) is accepted copy only (`direct` 0): DPDK maps every region read-write in VFIO, and the kernel's write-pin of a read-only mapping fails **[inferred]**.
- `MTL_MEM_READ` and `MTL_MEM_WRITE` are checked at attach (`ACCESS_MISMATCH`). They are a contract check, not IOMMU protection: the device mapping is read-write in the first implementation.
- **PA mode** (`caps.iova_mode`; accepted only with `instance.allow_noiommu`): imported memory other
  than EAL hugepages is copy only (today `mtl_dma_map` refuses PA mode, `mt_main.c:840`, `:901`);
  library pools copy every packet that crosses a page (`st_tx_video_session.c:2923`), counted in
  `tx.pkts_copied_partial`; the DMA engines are not available (`mtl_udma_create` refuses PA mode,
  `mt_main.c:1146`).
- An import beyond the region budget is `-MTL_ENOSPC`, `REGION_BUDGET`, never an opaque DPDK error (G-102). Import one region per pool arena, not one per buffer. Memlock and the VFIO mapping limit are checked per import.
- Mapping and unmapping are CP steps, never on the data path of a running session (G-36).
- `mtl_mem_close(r)` (CP; `mtl_close` on the region) drops the caller's reference. 0: retired, the application may unmap the memory. 1: still referenced; calling it again polls, and `MTL_EVENT_REGION_RELEASED` on the instance reports the end. A null handle returns 0. A region still referenced makes close return 1, never `-MTL_EBUSY` (G-11).
- `mtl_mem_get_info()` (CP) gives backing, NUMA, `va`, length, page size, `mapped_ports`, `direct` and `refs`. MTL exposes no IOVA: a device the application drives itself gets its memory from the application's own regions, and MTL's sessions reach that memory through `mtl_session_attach` (§9.3).

### 9.3 Attached pools

- With `MTL_SESSION_POOL_ATTACHED` the slots come from `mtl_session_attach(s, &a)` (CP), in CREATED or STOPPED. This is why a session is created stopped: an RX backend can never receive before the application's slots exist.
- `struct mtl_attach` appends `count` slots in the session's natural layout: slot i at `offset + i × pitch` (`pitch` 0 = `unit_bytes`), or at `slot_offset[i]` (woven interlaced fields, scattered surfaces).
- `plane_offset[]` is from the slot start (0 = after the previous plane). `stride[]` 0 = natural, one stride per pool.
- With a null `region`, `[va, va + length)` is imported for this session and released when it retires.
- `flags` take `MTL_ATTACH_META_IN_SLOT` (RX meta written into the slot at `meta_offset`, for MXL grain headers) and the `MTL_MEM_*` access and mapping bits.
- Attach validates span, stride, alignment, access and the region budget, each with its reason: `SPAN`, `STRIDE_MISMATCH`, `UNALIGNED`, `ACCESS_MISMATCH` (RX needs write access, TX read access; G-101), `REGION_BUDGET`, `LAYOUT_MISMATCH`.
- Calling attach again appends slots. With `pool_count` set, start is `-MTL_EINVAL` (`POOL_TOO_SMALL`) until that many slots are attached; with `pool_count` 0 the attached slots are the pool.
- If a layout satisfies every advertised requirement and capacity remains, attach and start never reject it later for an undisclosed reason.
- `mtl_session_attach(s, NULL)` (CP) detaches every slot: CREATED or STOPPED, with no lease, hold or session attached over this pool.
- ANC: at most `pool_count` slots (`POOL_COUNT_MAX`), natural strides (`STRIDE_MISMATCH`); an ANC
  session with an attached pool sets `sc.pool_count` (§3.3).
- `mtl_session_get_slot(s, slot, &u)` (DP) gives the static part of a slot (planes, meta); `lease` is null.

### 9.4 Requirements

`mtl_session_query(…, &req, sizeof(req))` returns `struct mtl_buffer_requirements` before create.

| Field | Meaning |
|---|---|
| `plane_count`, `plane[]` | per plane: `min_span` at the natural stride, `row_bytes`, `rows` per unit (a field: height / 2), natural `stride`, `stride_align` and `offset_align` (1 = byte granular: the extbuf attach, the DMA engine and the SIMD converters, with unaligned loads in `st_avx512.c`, accept any byte offset) |
| `min_count`, `max_count` | pool size limits; a count above `max_count` fails at query and create with `-MTL_ERANGE`, `POOL_COUNT_MAX`, never at start (G-103) |
| `min_count_direct` | video TX: slots needed to stay direct, because zero-copy slots are reusable only when the NIC recycles their descriptors, about `nb_tx_desc` packets later (formula below) |
| `meta_capacity`, `direct_possible`, `unit_bytes` | per slot |
| `internal_bytes` | every hugepage byte MTL allocates for the session besides the pool (below) |
| `completion_latency_ns` | expected delay from a unit's last packet to its result on the direct path; add it to framework pool sizing |

- `internal_bytes` is every hugepage byte MTL allocates for the session besides the pool:
  internal frames (a converting path holds `pool_count` transport frames); the session's DPDK
  mempools, each with its per-lcore cache table (about 1 MB), its mbufs and its ring (0 with
  `port.tx_mono_pool`); the slot table with its descriptor ring and meta areas; the stats blocks
  (engine.md §1.8); the ANC wire areas. The query computes it from the sizes create will use;
  `info.internal_bytes` sums what create allocated. A recovery adds the mempool part again while it
  runs (deployment.md §5). The session and queue entries are not hugepage bytes: they live in
  never-freed chunks of ordinary pages (core.md §4.7), outside every `internal_bytes`.
- Alignment: only the region needs page alignment (for mapping), not the planes inside it, so framework heap memory (64 B aligned) is still not importable as is. Internal frames are 64 B aligned; in PA mode a DMA-offloaded payload must not cross a page.
- The engines take one contiguous plane with one line size (`ops->linesize`, `st_rx_video_session.c:3343-3348`), which a direct slot's stride becomes. Multi-plane application formats are reached only through conversion, where per-plane layouts are fine because the converters use `addr[p]` and `linesize[p]` (`st_convert.c:48-62`).

**`min_count_direct`** = 1 + ceil(`nb_tx_desc` / packets per unit) for video TX. `nb_tx_desc` is 512 by default (`MT_DEV_TX_DESC`, `dev/mt_dev.h:12`; option `port.tx_desc`). Today `tv_pkts_capable_chain` falls back to copy, with only a warning, when the pool is smaller (`st_tx_video_session.c:2873`).

| Unit (GPM_SL) | Packets per unit | `min_count_direct` at 512 descriptors | `completion_latency_ns` ≈ 512 × TRS |
|---|---|---|---|
| 1080p59.94 4:2:2 10-bit | 4320 | 2 | ≈ 1.9 ms |
| 1080i59.94 field | 2160 | 2 | ≈ 3.8 ms (512 × the gapped TRS 7.4148 µs) |
| 2160p59.94 4:2:2 10-bit | 17280 | 2 | ≈ 0.5 ms |
| a small unit, for example a 480i field (about 360 packets) | 256–511 | 3 | ≈ 512 × TRS |
| a smaller unit | < 256 | 1 + ceil(512 / packets): 5 at 128 | ≈ 512 × TRS |
| cvideo, audio, ANC, fastmeta; every RX session | — | 0 (no direct TX path) | 0 |

The latency column is a model: the PMD's `tx_rs_thresh` and `tx_free_thresh` change it, and the spikes S0 and S6 measure it ([implementation-plan.md](implementation-plan.md)).

For ANC, `plane_count` is 2, and 3 with `MTL_ANC_WORDS_RAW`: plane 0 with `row_bytes` = `stride` =
16, `rows` = `max_packets`, `stride_align` 16 and `offset_align` 4; plane 1 with `row_bytes` =
`stride` = the word size (1 byte with `MTL_ANC_WORDS_8BIT`, 2 bytes with `_10BIT` and `_RAW`),
`rows` = `max_udw_words` and `offset_align` = the word size; with `_RAW` plane 2 with `row_bytes` =
`stride` = 8 and `rows` = `max_packets`. `meta_capacity` is 0 and `direct_possible` 0;
`internal_bytes` includes the wire areas: `pool_count` × round_up(16 × (P + 2) + B, 64) bytes, B =
min(328 × P, 13 × P + ⌈5 × W / 4⌉), P = `max_packets`, W = `max_udw_words`: a 16-byte record per RTP
packet (at most P + 2) and B bytes of RFC 8331 data, because an ANC packet of n words takes
`mtl_anc_rfc8331_bytes(n)` ≤ 13 + 1.25 × n bytes. That is 2 240 B per slot at the defaults (8 960 B
for 4 slots; 351 KB at P = 255 and W = 255 × P). One allocation at create for library and attached
pools alike (§5.7). RX appends each new RTP packet's ANC data to the area in arrival order; a packet
that does not fit is not stored and its ANC packets count in `anc.pkts_truncated`. Library
pools put each plane at the first 64-byte boundary after the previous one and pitch the slots to
64 bytes; `buffer_capacity_bytes` is plane 1's bytes.

### 9.5 Named slots

- `mtl_tx_acquire_slot(s, slot, &u, timeout)` (WT) leases that slot, for example framework surface i; `-MTL_EAGAIN` while it is not free. It leases exactly the named slot (G-60).
- `mtl_tx_send_slot(s, slot, &how, timeout)` (`mtl_util.h`, inline) acquires slot `slot` and submits it with the per-use fields of `how`, or does nothing (`-MTL_EAGAIN` while it is in flight).
- A queued unit is taken back only with the whole queue: `mtl_session_discard` (§4.6).
- An application that keeps displaying a slot after submit may read it, because MTL never writes a TX buffer (§5.2). To keep a slot from being reused after its result, it acquires its slots by name with `mtl_tx_acquire_slot` instead of taking any free one.

### 9.6 Holds and forwarding

The picture below follows one RX slot forwarded to four TX sessions, as in
[ex09](sketch/examples/ex09_split_forwarder.c): each TX unit holds the RX slot, the application's
release does not free it, and the last TX outcome does.

```mermaid
sequenceDiagram
    participant RX as RX session
    participant A as your code
    participant TX as four TX sessions
    RX-->>A: dequeue: slot j, lease
    A->>TX: mtl_tx_send_slot with hold = lease, four times
    Note over RX: hold count of slot j is 4
    A->>RX: mtl_rx_release(lease)
    Note over RX: slot j is HELD, not FREE
    TX-->>RX: each TX unit's terminal outcome drops one hold
    Note over RX: hold count 0: slot j is FREE
```

- `unit.hold` is an RX lease a TX unit reads from, kept until the TX unit's result (ANC: until the
  TX submit returns).
- Each accepted submit with a hold adds a reference to that RX slot; the TX unit's terminal outcome drops it. At most 255 holds per RX slot; one more is `-MTL_EBUSY`.
- The RX slot returns to free exactly once, when the application has released it and its hold count is 0, in any order. In between it is held: MTL never writes a slot that is held or leased (G-83).
- One RX unit may feed several TX sessions in the same frame period (split forward): each TX session's slots lie over the RX pool region with their own layout (sub-rectangle: offset and half-width rows, full stride), and each submits with `hold` = the RX lease.
- `mtl_session_get_pool_region(rx, &r)` (CP) returns a library pool as a region, so another session
  can attach over it. The region holds a reference; `mtl_mem_close()` drops it. Over today's
  pipelines each library frame is its own allocation, so the region may be one segment per slot:
  slot j starts at `j × pool_slot_pitch`, and a slot attached over it must lie inside one owner slot
  (`-MTL_EINVAL`, `SPAN`). The region lives until the owning session retires; it is EAL hugepage
  memory, so it needs no mapping and no region budget.
- A session attached over another's pool submits slot j only with `hold` = a lease of the owner's slot j (`-MTL_EINVAL`, `HOLD_REQUIRED`).
- The owner cannot change or detach its pool while attached sessions exist (`-MTL_EBUSY`), and its close returns 1 until they close.
- Every hold must be submitted before the application's own release of the RX lease; a hold naming a released lease is `-MTL_ESTALE`.
- MTL does not track byte overlap between slots of different sessions; `unit.hold` is how the application says which RX unit a TX unit reads. A TX slot over an RX slot the application did not hold is an application error (torn frames: the RX engine may write it while it is sent), not a memory-safety issue: the region stays mapped while any slot references it.

### 9.7 Direct and copy

- Video TX is direct by default when the NIC supports multi-segment packets, the layout allows it and the pool is at least `min_count_direct`: the NIC reads the slot.
- Any `stride` ≥ `row_bytes` is direct: a sub-rectangle or an interleaved field stays direct, the stride reaching the engine as its line size (G-84). Only a packet that crosses row padding (non-GPM_SL packing) or a PA-mode page is copied, each counted in `tx.pkts_copied_partial`.
- cvideo, audio, ANC and fastmeta packets are always built by copying; on AF_XDP and kernel sockets every TX path copies.
- RX places packets into the slot, by CPU or DMA engine (`rx.pkts_dma`, `rx.pkts_cpu`).
- Conversion (`app_format` different from `format`) runs in the caller (DPC), on a library worker or in a codec plugin, never on a tasklet, with one exception:
- **`rx.convert_per_packet`** (legacy `ST20P_RX_FLAG_PKT_CONVERT`) converts each packet's payload on the RX tasklet as it lands. It is library code, bounded per packet, and reported as `info.convert_context`; no application code runs. RX DMA offload is then off, and only the output formats the per-packet converter supports are accepted (three today; another is `-MTL_ENOTSUP` at create).
- The granted path is reported: `MTL_INFO_DIRECT` in `mtl_session_info.flags`, `tx.path`, `MTL_TXR_COPIED` per unit, `path` in the full records. It matches what happens (G-14).

| `enum mtl_path` | TX | RX |
|---|---|---|
| `MTL_PATH_DIRECT` | the NIC reads the slot (extbuf chain); packets crossing row padding or a PA-mode page are copied and counted | the CPU places each payload into the slot, no intermediate frame |
| `MTL_PATH_DIRECT_DMA` | — (no TX session uses a DMA engine at `545a266a`; TX `pkts_dma` stays 0) | a DMA engine places payloads, with a per-packet CPU fallback (`rx.pkts_dma`, `rx.pkts_cpu`) |
| `MTL_PATH_CONVERT` | a converter reads the slot and writes an internal transport frame, which is sent | an internal frame is filled; a converter writes the slot |
| `MTL_PATH_COPY` | packets built by copying (no chain, cvideo, audio, ANC, fastmeta, AF_XDP, kernel socket, copy-only regions) | an extra copy from an internal frame |
| `MTL_PATH_COPY_CONVERT` | a copy of a converted frame | — |

`path` is the granted session path; it changes only on a reported downgrade (`MTL_EVENT_PACING_CHANGED`, loss of a DMA engine). What each packet did is in the per-unit counters, because the RX DMA decision is per packet (`st_rx_video_session.c:1804-1827`). Header split, a third RX direct path, is cut (CUT-1, [coverage.md](coverage.md)).
- **`MTL_SESSION_REQUIRE_DIRECT` never copies silently**: query and create fail when a unit would be copied or converted (`-MTL_ENOTSUP`, `DIRECT_IMPOSSIBLE`; a pool below `min_count_direct` is `POOL_TOO_SMALL`) (G-13). An attach of a copy-only region (`MTL_BACKING_FILE`, a read-only mapping, imported memory in PA mode) to such a session fails the same way. `caps.tx_copy` with it is `-MTL_EINVAL`.
- Library-pool and attached-pool units follow the same timing and outcome accounting (G-15).

### 9.8 RX placement

- **Frameworks wrap library slots** (D-150): dequeue, wrap the unit's planes
  (`gst_buffer_new_wrapped_full`, `av_buffer_create`), and release from the free callback. That is
  zero copy from MS1, with no attach and no provide (migration.md §12.8). Provide is not the
  framework path: an FFmpeg demuxer gets no buffers from downstream, and a GStreamer downstream
  pool's buffers are separate heap allocations that cannot be mapped per buffer on the data path
  (§9.11).
- `MTL_SESSION_RX_BY_INDEX`: slot = media index mod `pool_count` (MXL rings), so unit k lands in slot k mod N (G-60). A unit whose slot is leased or held is dropped and counted in the next unit's `missed_before`, never written over. With `MTL_SESSION_RX_LATEST` an unread (not leased) unit in that slot is replaced.
- With `MTL_SESSION_RX_BY_INDEX`, size the pool with `N > H + 1 + ceil((rx.flush_offset_ns + skew) / unit period)`, H the units held at once.
- `MTL_SESSION_RX_LATEST`: a full pool reclaims the oldest unread unit (latest-only monitoring: `pool_count` 2, or 3 for a consumer that holds one while it dequeues the next). Leased and held slots are never reclaimed.
- Without either, a full pool drops the new unit and the next delivered unit carries `missed_before`.
- Missing packets read as zero in library pools; `MTL_SESSION_RX_NO_FILL` turns the fill off. The
  fill runs in the caller's `mtl_rx_dequeue`, which is then DPC (D-37), never on the RX tasklet or
  a worker: after the claim it zeroes the byte ranges of the missing packets, from the bitmap
  below; with conversion it zeroes them in the transport frame before converting. It is counted in
  `rx.caller_work_ns`, `rx.units_zero_filled` and `rx.bytes_zero_filled`, not in `convert_ns`,
  which plans normal operation. **Worst case**: a unit that kept one packet is filled almost
  whole, `unit_bytes`: 5.2 MB at 1080p 4:2:2 10-bit, 20.7 MB at 2160p, 83 MB at 4320p, about 0.5,
  2 and 8 ms at 10 GB/s on one core *(est.)*. A thread that dequeues several sessions stalls that
  long on one session's loss: give each 2160p or 4320p receiver its own dequeue thread, or set
  `MTL_SESSION_RX_NO_FILL` and read `u.status`.
- Attached pools are never zero-filled (D-37): the application owns the memory, and the unit arrives as `MTL_RX_INCOMPLETE` with the loss counts of `mtl_rx_get_detail` (§6.5); there is no list of lost ranges. `MTL_SESSION_RX_NO_FILL` therefore matters only for library pools.
- The fill reads a copy of the unit's packet bitmap that the RX binding takes into the slot
  table when an incomplete unit of a library pool with the fill on completes (≈ 540 B at 1080p,
  8.6 KB at 4320p, room preallocated per slot at create), because today's engine clears the
  reassembly slot's bitmap at reuse (`st_rx_video_session.c:1298-1299`, [engine.md](engine.md)
  §2.11). A complete unit, an attached pool and `MTL_SESSION_RX_NO_FILL` copy nothing, so the
  tasklet pays only on loss. It lands in MS2; until then a library-pool unit with lost packets is delivered `MTL_RX_INCOMPLETE` without the fill.

### 9.9 The meta area

- Every slot has a meta area, TX and RX alike, at `unit.meta`, `meta_capacity` bytes
  (`mtl_session_info.meta_capacity`). It is library memory, part of the slot table, for every slot
  of every pool, attached pools included, so an attached layout needs no meta region (except with
  `MTL_ATTACH_META_IN_SLOT`, where RX writes it into the slot). Sizes: video user meta 1332 B; audio,
  ANC and fastmeta 0 (an ANC packet table is plane 0 of its unit, §5.7).
- Frame and row units: records back to back, each a `struct mtl_meta_hdr` and its payload padded to 4 bytes, ended by kind `MTL_META_NONE` or the capacity; at most one record per (kind, tag).
- TX acquire writes an `MTL_META_NONE` header at the start of the area, so a slot starts each use
  with no records. The inline `mtl_meta_put(u, kind, tag, data, n)` (`mtl_util.h`) appends a record
  at the terminating NONE header and writes a new terminator after it (`-MTL_EEXIST` for a (kind,
  tag) already there, `-MTL_ENOSPC` beyond the capacity); `mtl_meta_find(u, kind, tag, &hdr)`
  returns 1 with the record's header, or 0.
- Packet units: no header; the packet table starts at `meta`, one entry per slot, `used` entries valid (§13).

| Kind | Payload |
|---|---|
| `MTL_META_USER` | user meta bytes; `tag` 0 = untagged. Video and cvideo only, at most 1332 B, one packet: `pkt_udp_suggest_max_size` − `sizeof(st20_rfc4175_rtp_hdr)` = 1352 − 20 (`st_tx_video_session.c:271-272`); the other essences report a capacity of 0 for it |
| `MTL_META_RTCP_MIB` | TX: Media Info Blocks for this unit's sender report only (Phase 7, `MTL_LATER`) |

TX: the meta area is validated and copied at submit, so later writes never reach the wire.

**The RFC 8331 codec.** Two exported functions of `mtl_packet.h` (AS, MS4; exported because the
decode parses network bytes, D-195) serve packet units, and the library's frame units use the same
two:

- `mtl_anc_rfc8331_encode(pkts, n_pkts, word_mode, words, udw_cap, raw_hdr, payload, cap, &len)`
  writes the ANC packets of one RTP packet (`n_pkts` ≤ 255) in one pass: each entry and its four
  RAW header words are read once and every word is checked as it is read, so on a failure the
  payload holds a partial write and must not be sent. It is `-MTL_ENOSPC` if `cap` is short, and
  `-MTL_EINVAL` for a word range outside `udw_cap`, a line, `hoffset` or stream out of range, an
  unknown flag or a non-zero `reserved`; outside RAW for DID 00h or a Type 2 SDID 00h (ST 291-1
  §6.1, §6.2) and, 10-bit, a protected word (000h–003h, 3FCh–3FFh, ST 291-1 §9.1); in RAW for a
  word above 3FFh, a missing `raw_hdr` or header words whose low bytes are not the entry's. The
  raster order and the field of located packets (ST 2110-40 §5.2.2) are the caller's.
- `mtl_anc_rfc8331_decode(data, len, anc_count, word_mode, gap_in, pkts, max_pkts, words, udw_cap,
  raw_hdr, &info)` stops after `ANC_Count` packets, so padding is never read as a packet. Outside
  RAW it skips a corrupt packet and counts it in `info.skipped`, one total over every cause (DID or
  SDID parity, checksum, reserved DID or SDID, a protected 10-bit word, an 8-bit word whose b8/b9
  are not its parity, past Length); in RAW it skips only a packet past Length and delivers the others with
  `MTL_ANCF_PARITY_ERR` or `MTL_ANCF_CHECKSUM_ERR`. A packet that does not fit counts in
  `info.truncated`. The first entry carries `MTL_ANCF_GAP_BEFORE` when `gap_in` is set, an entry
  after skipped or truncated packets always does, and `info.gap_after` says that the caller's next
  entry carries it. A `word_mode` above `MTL_ANC_WORDS_RAW`, or RAW without `raw_hdr`, is
  `-MTL_EINVAL` with `info` zeroed.

**Tagged user meta on the wire.** With `tag` 0 the `MTL_META_USER` payload is sent exactly as today, so legacy receivers see no change.
A non-zero `tag` prefixes the payload on the wire with 12 bytes, `{uint32_t magic = 'MTLM'; uint32_t tag; uint16_t tag_version; uint16_t length}`, which leaves 1320 of the 1332 bytes for user data. A received payload without the magic (a legacy sender) arrives with `tag` 0. Tag values below 0x80000000 are registered by MTL; the others are private.

### 9.10 Teardown order

1. Close the sessions that use the memory (for memory the application gave the session): `mtl_session_close`, and while it returns 1, call it again with a timeout until it returns 0.
2. Close the region: `mtl_mem_close`, and while it returns 1, poll it again, or wait for `MTL_EVENT_REGION_RELEASED` on the instance.
3. **Only then** unmap or free the memory. Earlier is undefined: the IOMMU may still hold the mapping. Memory imported by an attach with a null region may be freed once its session retired.

After a hung TX queue, retirement waits for the stalled-queue reset, so these steps never wait forever. Process exit is the one case that bypasses the order.

These steps are for memory the application gave a session (attached pools, imported regions,
provided destinations). A framework pool over such memory finishes them inside its own stop. A
source over a library pool does not wait: it closes with timeout 0 and returns, and its last
release retires the session (§4.9).

### 9.11 Later

Exported from MS2: `mtl_tx_acquire_layout` (a new layout per acquire, for framework buffers and
moving-cursor producers) and `mtl_rx_provide` (an RX destination per unit, legacy `query_ext_frame`,
for destinations chosen per unit; frameworks wrap library slots, §9.8). Device memory such as GPU
VRAM is the flag `MTL_MEM_DEVICE` of `mtl_mem_open`, in MS6, `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`)
until then.
GPU pinned host memory imports as host memory today.

**`mtl_tx_acquire_layout` rules (MS2).** One mechanism covers the moving-cursor ext frame, FFmpeg per-frame TX from one imported arena, and GStreamer upstream pools whose arena was imported once.

- The layout must lie in a region already imported and mapped into every device of the session (`MTL_MEM_MAP_ALL` or an earlier attach), else `-MTL_EINVAL`: mapping is CP work and never runs in a data call.
- Validation is the attach check (span, stride ≥ `row_bytes`, one stride per pool, access) in O(planes); the slot keeps the layout inline, so the call allocates nothing (WT; DP with timeout 0).
- The session always produces results (MEM3); the result's cookie tells the producer it may reuse its memory. `pool_count` slots bound what is in flight, and `min_count_direct` still applies to direct sends.

**`mtl_rx_provide` rules (MS2b; D-152).**

1. *Provide sessions.*
   - An RX video session with `MTL_SESSION_POOL_ATTACHED`, not `MTL_SESSION_RX_BY_INDEX`, and no
     slot attached; else `-MTL_EINVAL`.
   - The first successful provide makes it a provide session until `mtl_session_attach(s,
     NULL)`; attach is then `-MTL_EINVAL`.
   - A provide session starts with no slot. A start with neither attached slots nor a provide
     since create or the last detach is `-MTL_EINVAL` (`POOL_TOO_SMALL`), as for an attached
     pool.
2. *Held.* A destination is held from its provide until it is the application's again (rules 6
   and 7): queued, in assembly, ready, leased or held by a TX unit. At most `pool_count` (0 = the
   essence default) are held; one more is `-MTL_ENOSPC` (`PROVIDE_FULL`).
3. *What and when.*
   - `struct mtl_attach` with `count` 1: a layout inside a region already mapped into every
     device of the session (else `-MTL_EINVAL`, `SPAN`; mapping is CP work), and `cookie` ≠ 0
     (else `FIELD_REQUIRED`).
   - It is validated as an attach in O(planes), with no allocation (DP).
   - CREATED, ARMED, STOPPED and RUNNING; otherwise `-MTL_ESHUTDOWN` (DRAINING, FLUSHING,
     CLOSING) or the error code (ERROR), and nothing is taken.
   - One providing thread at a time (overlap detected in debug builds).
4. *Assignment.*
   - A unit takes the oldest destination held when its first packet lands.
   - A unit dropped before delivery (`rx.incomplete = MTL_RX_DISCARD`, a relock, an eviction)
     gives its destination back, to be taken first.
   - Delivery is in reception order (D-126), not provide order: the engine assembles up to two
     units and evicts the oldest at a third timestamp (`st_rx_video_session.c:1213-1221`).
   - `u.cookie` is the destination's cookie and the only identity; `u.slot` is `UINT32_MAX`.
5. *None held.* The unit is not received: it is counted in `rx.units_missed_pool_full` and the
   next unit's `missed_before`, and never placed in library memory (no mixed pool). With
   `MTL_SESSION_RX_LATEST` it takes the destination of the oldest unread unit
   (`rx.units_reclaimed`).
6. *Hand-backs.*
   - Every `mtl_session_stop` (a no-op stop included), `mtl_session_discard`,
     `mtl_session_attach(s, NULL)` and `mtl_session_close` that returns 0 or `-MTL_ETIMEDOUT`
     performs one hand-back.
     - It returns every queued destination of an older generation and the destination of each
       assembly it discards.
     - Discard, detach and close also return those of the ready units they drop
       (`rx.units_flushed`).
   - It runs on the tasklet at the command's acknowledgement (MS2a `tick`) when the session is
     on a scheduler, after the engine's DMA for the unit in assembly has drained. That drain is
     new code in the RX binding; today's `rv_slot_by_tmstamp` only refuses a new frame while DMA
     is in flight, `:1202-1208`. In CREATED and STOPPED it runs on the calling thread.
   - **Generations.**
     - A generation is 31-bit, from 1, wrapping 2^31 − 1 → 1. g is older than G when
       (G − g) mod (2^31 − 1) lies in [1, 2^30).
     - The hand-back first makes the state non-accepting (stop, close), then stores the
       internal tag G + 1, then sweeps every QUEUED entry older than G + 1 to RETURNED, then
       publishes `status.provide_gen` = G + 1 before it returns.
     - `mtl_rx_provide` returns the generation g ≥ 1 its destination is held under. After
       storing its entry it re-reads the tag and the state, and either re-tags it to the new
       generation, or withdraws it and returns `-MTL_ESHUTDOWN`, or (when a hand-back or a unit
       already took it) returns g.
   - **The ledger.** After a hand-back returns, every destination whose provide returned a g
     older than `provide_gen`, and that was not dequeued, is the application's again. After
     stop, dequeue to `-MTL_EAGAIN` first. Every other one is held. `-MTL_ESHUTDOWN` means
     nothing was taken. After close nothing is held.
   - No tasklet or DMA engine writes a returned destination. This holds whatever other threads
     provide meanwhile (the model [design/models/provide_model.py](design/models/provide_model.py)).
7. *Delivered destinations.* A destination is the application's again when its lease is
   released and every TX unit holding it (§9.6) has its result.
8. *Content.* Never zero-filled, no tail; loss shows as `MTL_RX_INCOMPLETE` with the counts of
   `mtl_rx_get_detail` (D-37). Rows units take one destination per unit, field units one each.
9. *Templates.* A unit received into a provided destination carries its cookie as a template
   (§5.1).

**Early source release (later, no header symbol yet; D-56).** A zero-copy framework sink
keeps its upstream buffer until the unit's result: wire time plus `completion_latency_ns`. Upstream
pools negotiated with `min_buffers = 2` (`videotestsrc`, `v4l2src`) then block in their own acquire;
today's GStreamer sink has the same exposure, holding the reference until `notify_frame_done`
(`gst_mtl_st20p_tx.c:547-597`). On copy and convert paths MTL stops reading the source long before
transport ends, so an opt-in session option posts an early "source released" result as soon as MTL
stops reading, flagged "more coming", and the terminal result follows (two completions, the io_uring
zero-copy send model). The results ring then grows by one entry per unit. A direct session rejects
the option (`-MTL_ENOTSUP`): the NIC reads the memory until completion. The sink then drops the
buffer one conversion time after submit instead of one completion latency. Without it: copy into an
acquired slot and drop the buffer at once (v4l2 mmap pools with a small fixed count).

**Not in any header, not before Phase 7.** Mixed pools (library and attached slots in one pool;
multi-region slots such as host-visible RTP headers with a GPU-resident payload; an imported source
plus an internal conversion destination as one slot; fallback capacity while an external producer
withholds buffers), header/payload split regions, and producer/consumer fences for device memory
(with `MTL_MEM_DEVICE`). Each would be a backend capability, enabled only once its
conformance tests pass, without changing the basic calls.

## 10. Events and queues

### 10.1 Events

The picture below shows where events come from and how they are read and waited on. A session
keeps its own events and the instance its own; a queue reports the sessions and the instance
armed on it for their events (`MTL_WAIT_EVENTS`), beside their results and units, so one queue's
descriptor waits on everything (§7.2).

```mermaid
flowchart LR
    SE["session events: state,<br/>legs, flows, RX signal<br/>and format, pacing,<br/>underrun, update"] --> SR["mtl_session_read_events"]
    SE -.->|"MTL_WAIT_EVENTS"| Q["a queue<br/>(mtl_queue_arm)"]
    RES["TX results, RX units<br/>MTL_WAIT_RESULTS,<br/>MTL_WAIT_DEQUEUE"] -.-> Q
    IE["instance events: ports,<br/>time, schedulers, health,<br/>MtlManager, regions"] -.->|"MTL_WAIT_EVENTS"| Q
    IE --> IR["mtl_instance_read_events"]
    Q --> EP["one descriptor:<br/>the queue's"]
```

- Events say that something changed. **Every state they report also has a getter, so a lost event loses nothing** (G-41; §10.2 names the getter). G-41 covers state events only; notices (`MTL_EVENT_EPOCH_TICK`, `MTL_EVENT_OVERFLOW`, …) are exempt. Its test overflows the events, then compares every getter with the true state.
- Events are MS3 (`mtl_events.h`). Each session keeps its own events, and the instance keeps the
  port, time, scheduler, health, manager and region events (an instance's reader never sees its
  sessions' events). Both are read with the object verb `mtl_read_events(o, ev, ev_size, max,
  timeout)` (WT) through its typed wrappers `mtl_session_read_events(s, ev, max, timeout)` and
  `mtl_instance_read_events(mt, ev, max, timeout)`, which pass `sizeof(struct mtl_event)`: a count ≥
  1 or `-MTL_EAGAIN`. Pending events make `MTL_WAIT_EVENTS` ready for `mtl_wait`, a call with a
  timeout and a queue (§7).
- Events coalesce. A record stands for `coalesced` occurrences and keeps the first and last state of a transition; nothing ever blocks a producer (G-58). On overflow the reader gets `MTL_EVENT_OVERFLOW` (value[0] = events lost) first, and should re-read the getters.
- `struct mtl_event`: `type`, `severity` (`enum mtl_severity` of `mtl.h`: `MTL_SEV_INFO`, `MTL_SEV_WARNING`, `MTL_SEV_ERR`),
  `reason`, `coalesced`, `seq` (per reader; a gap means overflow), `time_tai_ns` (valid with
  `MTL_EVF_TIME_VALID`), `origin` (`struct mtl_object`), `origin_name` (readable after the origin
  retired), `index` (leg or port), `old_value`, `new_value`, `value[2]`. The name is carried in the
  record, not as an index into an instance name table, so a reader needs no lookup that could race
  with the origin's retirement.

### 10.2 Event types

| Type | Origin | old → new; values | Getter |
|---|---|---|---|
| `MTL_EVENT_SESSION_STATE` | session | state | `mtl_session_get_state`, `status.reason`, `status.error_reason` |
| `MTL_EVENT_RECOVERY` | session | 0 → 1 begin, 1 → 0 end; value[0] units dropped | `tx.recoveries_ok`, `tx.recoveries_failed` |
| `MTL_EVENT_LEG_STATE` | session, index = leg | oper; value[0] admin | `status.leg[]` (`admin`, `oper`) |
| `MTL_EVENT_FLOW_STATE` | session, index = leg | `enum mtl_flow_state`; value[0] first index | `status.leg[].flow_state` |
| `MTL_EVENT_RX_SIGNAL` | session | 0/1 packets present (no unit for `rx.signal_timeout_ns`) | `MTL_STATUS_RX_SIGNAL` |
| `MTL_EVENT_RX_FORMAT` | session | old → new `format_seq` published; new 0 while none is (`format_reason`, `MTL_STATUS_RX_DETECTING`) | `MTL_STATUS_FORMAT_CHANGED`, `MTL_STATUS_RX_DETECTING`, `rx.detected.*`, `mtl_rx_detail.format_seq` |
| `MTL_EVENT_RX_TIMEBASE_SUSPECT` | session | value[0] arrival − media ns | `MTL_STATUS_TIMEBASE_SUSPECT` |
| `MTL_EVENT_PACING_CHANGED` | session | `enum mtl_pacing` | `MTL_STATUS_PACING_DOWNGRADED`, `tx.pacing_class` |
| `MTL_EVENT_TIMING_INFEASIBLE` | session | value[0] shortfall, value[1] suggested delay | `status.timing_reason`, `shortfall_ns`, `suggested_min_tx_delay_ns` |
| `MTL_EVENT_BACKPRESSURE` | session | `enum mtl_blocked_on` | `status.blocked_on` |
| `MTL_EVENT_TX_UNDERRUN` | session | value[0] slots filled by the underrun policy | `tx.indices_empty`, `tx.units_padded` |
| `MTL_EVENT_TIME_STATE` | port | `enum mtl_time_state`; value[0] offset ns | `time.state`, `mtl_instance_get_health` |
| `MTL_EVENT_TIME_STEP` | instance | value[0] step ns | notice (`time.step_count`) |
| `MTL_EVENT_PORT_LINK` | port | 0/1 up; value[0] Mb/s | `port.link_up`, `port.speed_mbps` |
| `MTL_EVENT_PORT_RESET`, `MTL_EVENT_PORT_REMOVED` | port | §2.6 | `port.resets`, `port.removed` |
| `MTL_EVENT_PORT_ADDRESS` | port | the granted address changed (DHCP) | `mtl_port_get_spec` |
| `MTL_EVENT_SCHED_OVERLOAD` | scheduler | value[0] busy % × 100 | `sched.busy_pct_x100` |
| `MTL_EVENT_SCHED_STALLED` | scheduler | value[0] loop age ns, at 1/8, 1/4, 1/2 of `instance.stall_ns` | `sched.last_loop_tai_ns`, `mtl_instance_get_health` |
| `MTL_EVENT_MANAGER_LOST` | instance | MtlManager connection state | `instance.manager` |
| `MTL_EVENT_HEALTH` | instance | old → new `MTL_HEALTH_*` flags | `mtl_instance_get_health` |
| `MTL_EVENT_REGION_RELEASED` | region | its last reference went | `mtl_mem_get_info` (`refs`) |
| `MTL_EVENT_EPOCH_TICK` | session | opt-in (`session.epoch_tick`); value[0] index | notice |
| `MTL_EVENT_CAPTURE_DONE` | session | a packet capture finished | notice |
| `MTL_EVENT_UPDATE` | session | an update left PENDING: new = `enum mtl_update_state`; value[0] applied TAI, value[1] update seq | `status.update_state`, `update_applied_tai_ns` |
| `MTL_EVENT_GRANDMASTER` | port | value[0] new ID, value[1] old | `time.grandmaster_id` |
| `MTL_EVENT_RTCP_INFO`, `MTL_EVENT_KEY_NEEDED` | session | Phase 7, later | — |
| `MTL_EVENT_OVERFLOW` | reader | value[0] events lost | notice |

Events whose origin is a port, a scheduler, a region or the instance are the instance's and are read with `mtl_instance_read_events`; the others are the session's.

### 10.3 Queues

Queues are §7.2. Results and events stay per session; a queue says which objects to serve.

### 10.4 Library threads that call application code

- **Log sinks** (`mtl_observe.h`, MS2a; D-153).
  - `mtl_log_add_sink(&p, &h)` (CP) adds a sink: `fn` and `priv`, its own `min_severity` (0 =
    INFO), and an instance filter. The filter is null for every instance and the process;
    otherwise it is that instance through any reference, with its ports, schedulers and
    sessions.
  - Several sinks may exist, also before any open. They get the EAL lines of an EAL the library
    starts; an EAL the process started keeps its own log stream.
  - Sink records live in never-freed handle slots (R4).
- **Without a sink.** Lines go to stderr in the legacy format (D-110).
  - A control-plane line is written by its caller, synchronously, so the last lines before a
    crash are not lost.
  - Tasklet lines go through the log thread (D-129); in MS1, through the legacy macros as today.
    From MS2a a tasklet line is checked against the threshold, a per-site limit of 10 lines a
    second and a free ring entry before it is formatted; a refused line is counted in `dropped`.
  - The threshold is `mtl_instance_params.log_level` (`enum mtl_severity`; 0 = leave it,
    INFO at process start; process-wide, so a non-zero value that differs from the one another
    open instance set is `-MTL_EEXIST`, `INSTANCE_MISMATCH`). On a bridged instance it is the
    legacy level (`mtl_init_params.log_level`, `mtl_set_log_level`), never overwritten.
  - It is one process-wide threshold, as the legacy one is (`g_mt_log_level`).
- **With sinks.**
  - Lines reach sinks only; `fn` NULL is the library's stderr writer with an optional `prefix`
    (U-020).
  - The production threshold is the lowest `min_severity` among the sinks whose filter matches.
    On a bridged instance the legacy level follows it, so legacy sessions' lines reach the sinks.
  - When the last sink goes, both return to the stderr threshold.
- **The record** (`struct mtl_log_record`, valid during the call):
  - `severity` (`enum mtl_severity`);
  - `dropped`: this sink's losses since its previous record, exact for an instance's rings. For
    process-wide lines, which have no origin to filter on, it is counted for every sink whose
    level takes them, an upper bound;
  - `origin`: session, port, scheduler or instance, with the instance's identity
    (`instance.identity`) for every reference of a shared instance; kind 0 = the process (EAL,
    DPDK, lines before an open);
  - `origin_name`: readable after the origin retired;
  - `realtime_ns`;
  - `text`: no prefix, no newline, at most 511 bytes.
- **One log thread.** Every `fn` runs on the one process-wide log thread, one call at a time
  across all sinks, never on a tasklet or an application thread (G-44). Records of one producing
  thread arrive in order.
  - The thread runs while a sink or an instance exists. An instance's close hands its queued
    lines to the sinks within its timeout.
  - The thread is joined when no sink and no instance remain.
- **Reentrancy.**
  - `fn` may call any function except instance open, close and shutdown (`-MTL_EDEADLK`,
    `LIBRARY_THREAD`).
  - Lines that `fn`'s own calls produce are dropped and counted in `dropped`, never delivered.
    That rules out feedback loops.
  - `mtl_log_remove_sink(h, timeout)` (`mtl_close` on `MTL_OBJ_LOG_SINK`) returns 0 once `fn` is
    not running and will not be called again; it returns `MTL_RETIRING` if `fn` is still running
    at the deadline. From inside `fn` it never waits: 0 for another sink, `MTL_RETIRING` for its
    own.
- **Fork.** In a forked child the sinks are never called. `mtl_log_add_sink` returns
  `-MTL_EBADF` (`FORKED`), and `mtl_log_remove_sink` drops the handle (R8).
- **No log level option.** Levels belong to the sinks and to `mtl_instance_params.log_level`,
  so components that share an instance never silently change each other's. The legacy
  `log_level` maps to that field (MS1) and to a sink's `min_severity` (MS2a). Every fact that is
  logged has a structured home first.
- A configuration without MtlManager is normal and logs nothing above INFO; the manager state is the `instance.manager` gauge and `MTL_EVENT_MANAGER_LOST`, never a log line (today `mtl_is_manager_alive()` logs an error on every call without one, SF-40).

## 11. Stats registry

### 11.1 Rules

- Every number MTL reports about an object (session, port, scheduler, instance, queue: the stats verbs accept `MTL_OBJ_QUEUE`) is a named `int64_t` value in one registry (`mtl_observe.h`).
- One value type, `int64_t`: gauges such as `audio.drift_samples` and `margin_min_ns` stay signed; counters lose one bit (63 bits of bytes at 100 Gb/s last about 23 years). Unit and kind are in the descriptor; a histogram's slots overlay as count, sum, min, max, buckets, and `bucket_width` gives the layout (0 = log2 buckets, the layout of every histogram).
- **The names are the stable contract.** A key not listed here is not part of it. A published name never changes meaning; adding names is never an ABI event.
- Names are checked, not compiled: no generated header of key names ships. Resolve keys once at setup with `mtl_stat_find` (it fails loudly). The catalogue of §11.3 is generated from `lib/src/unified/stats_keys.def` (from task A2b), and `Stats.catalogue` compares that file with `mtl_stat_list` on the null backend.
- `mtl_stat_list(o, d, desc_size, cap, &n, &gen)` (CP) lists the schema once: `struct mtl_stat_desc` gives `name`, `label`, `slot`, `width`, `kind`, `unit`, `flags` and `bucket_width`.
- `mtl_stat_read(o, gen, first, count, values, &snapshot_tai_ns)` (DP) reads values in bulk, one snapshot, and **never takes a lock a tasklet takes** (G-40).
- `mtl_stat_find(o, "name{label}", &slot)` (CP) resolves a key; the inline `mtl_stat_get(o, key, &value)` lists, finds and reads one value, again after a schema change.
- The schema can grow (a leg added, a reason seen for the first time). Its generation then changes, and a bulk read against an old generation fails with `-MTL_ESTALE`, so the reader lists again.
- A value a backend cannot produce is absent from the schema, never a silent zero. An inexact value is flagged `MTL_STAT_ESTIMATE`; one polled off the tasklet is flagged `MTL_STAT_CACHED`, and `<scope>.sampled_tai_ns` says when.
- **Counters are cumulative for the life of the object**: across stop and start, update and recovery. No call resets them, and two readers never interfere (G-42). Readers compute rates from their own previous snapshot. Exporters append `_total`; the library never does.
- Kinds: `MTL_STAT_COUNTER`, `MTL_STAT_GAUGE`, `MTL_STAT_CONST` (granted configuration and
  capabilities), `MTL_STAT_HIST` (4 + `MTL_HIST_BUCKETS` slots: count, sum, min, max, log2
  buckets). Units: `enum mtl_stat_unit`; `MTL_SU_TAI_NS` reads INT64_MIN
  for never; `MTL_SU_ID` is an identifier, an EUI-64 or a MAC in the low 48 bits
  (`time.grandmaster_id`, `port.mac`, `info.ts_refclk.gm_identity`).
- Objects are addressed with `struct mtl_object`: `MTL_OBJ_OF_SESSION(s)`, `MTL_OBJ_OF_PORT(mt, p)`, `MTL_OBJ_OF_INSTANCE(mt)`, `MTL_OBJ_OF_QUEUE(q)` (MS2a), and `MTL_OBJ_SCHED` with the scheduler index.
- Each histogram's `count` equals the number of published results; `queue.queued_media_ns` equals the sum of the queued units' durations (G-55).

### 11.2 Naming

- A key is `scope.metric`: lower case, `[a-z0-9_.]`, at most 47 characters. Lookup accepts `scope.metric{label}` (Prometheus syntax); one label string of `k=v[,k=v]`, at most 47 characters.
- Scopes: `tx`, `rx`, `leg`, `queue`, `tp`, `pkt`, `rtx`, `session`, `video`, `cvideo`, `audio`, `anc`, `fastmeta`, `info`, `wait` (session); `port`, `caps`, `nic`, `time` (port); `sched`; `instance`, `mem`, `capacity` (instance); `wq` (queue).
- A unit appears as a suffix when the value is not a plain count: `_ns`, `_ps`, `_bytes`, `_pct_x100`, `_mbps`.
- Label keys: `leg`, `reason` (lower-case `mtl_reason_name` without the prefix), `cause`, `state` (lease state), `window` (`1s`, `60s`), `numa`, `on`, `essence`, `dir`, `name`, `size`, `kind`, `version`.

### 11.3 Key catalogue

<!-- BEGIN TABLE stats-keys: lib/src/unified/stats_keys.def; gen_api_doc.py writes this region once that file exists (README §1) -->

G = gauge, C = const, H = histogram; the rest are counters.

**Session.**

| Scope | Keys |
|---|---|
| `tx.` units | `units_submitted`, `units_on_time`, `units_dropped{reason}`, `units_flushed`, `units_before_start`, `units_failed`, `units_suppressed`, `indices_empty`, `units_repeated`, `units_padded`, `units_muted` (Phase 7) |
| `tx.` work | `acquire_blocked{on=results\|buffers\|app_leases}`, `build_overrun`, `recoveries_ok`, `recoveries_failed`, `cmd_timeouts`, `bytes`, `pkts`, `pkts_dma`, `pkts_copied_partial`, `tpr_late_pkts`, `caller_work_ns` |
| `tx.` timing | `vrx_max{window}` G, `cinst_max{window}` G, `margin_min_ns{window}` G, `margin_ns` H, `launch_error_ns` H, `vrx` H, `cinst` H, `pacing_class` G, `path` G, `timing_warning_reason` G, `shortfall_ns` G, `suggested_min_tx_delay_ns` G, `snap_error_ns` G |
| `rx.` units | `units_delivered`, `units_complete`, `units_incomplete_delivered`, `units_incomplete_discarded`, `units_zero_filled`, `units_used_redundancy`, `units_missed_pool_full`, `units_reclaimed` |
| `rx.` units, continued | `units_flushed`, `units_before_start`, `units_stale`, `units_dropped_notify`, `format_changes`, `units_format_dropped{cause}` (`detecting`, `above_max`, `straddle`), `discontinuities`, `units_late_presentation` |
| `rx.` packets | `pkts_received`, `pkts_redundant`, `pkts_stale`, `pkts_dma`, `pkts_cpu`, `bytes_zero_filled`, `pkts_lost_est` (ESTIMATE when inexact), `cmd_timeouts`, `caller_work_ns`, `pkts_rejected{cause}` (pt, ssrc, len, interlace, seq_old, rtp_out_of_range, no_slot, dma_busy, wrong_port, fmd_filter, before_start, offset, stale) |
| `rx.` timing | `latency_max_ns{window}` G, `latency_ns` H, `delivery_ns` H, `link_offset_min_ns` G, `link_offset_max_ns` G |
| `rx.` format | `detected.width`, `.height`, `.fps_num`, `.fps_den`, `.fps_approx`, `.scan`, `.packing`, `.format_seq`, and `.format` (echoes the configured format; the format is never detected). All are G, contiguous in the schema, so one `mtl_stat_read` reads them as one snapshot; they are the getter of `MTL_EVENT_RX_FORMAT` |
| `tp.` (opt-in `rx.timing_parser`) | `narrow`, `narrow_linear`, `wide`, `fail`, `untrusted_pkts`, `vrx_underflow`, `vrx_missing`, `margin_min_ns{window}` G, `gap_max_ns{window}` G, `vrx_max{window}` G, `cinst_max{window}` G, `fpt_max_ns{window}` G, `latency_max_ns{window}` G; audio `dpvr_max_ns{window}` G, `ipt_max_ns{window}` G, `tsdf_max_ns{window}` G; thresholds `pass.*` C |
| `leg.` (`leg=N`) | `pkts`, `bytes`, `pkts_lost`, `pkts_reordered`, `pkts_duplicate_same_leg`, `pkts_skipped`, `pkts_late`, `igmp_reports`, `receiving` G, `last_packet_tai_ns` G, `observed_skew_ns{window}` G (TX: the launch difference; RX: the path differential, §14.4) |
| `queue.` | `gauge{state}` G (free, app_writable, queued, in_flight, done, receiving, ready, app_reading, held_by_tx), `entries{state}`, `exits{state}`, `unread_results` G, `queued_media_ns` G |
| `session.` | `last_progress_tai_ns` G, `leases_out` G, `oldest_lease_ns` G (they name the holder when a close returns 1) |
| `wait.` (sessions and the instance; MS2a) | `sleepers{lane}` G, `armed` G (attached targets), `interrupted` G, `fired` G, `wakes{lane}` |
| essence | `cvideo.padding_bytes`, `cvideo.oversize_rejected`, `audio.samples_padded`, `audio.samples_dropped`, `audio.samples_inserted`, `audio.drift_samples` G, `audio.rephase_count`, `fastmeta.items`, `fastmeta.keepalives` |
| essence, ANC | `anc.packets`, `anc.rtp_packets`, `anc.udw_words`, `anc.pkts_skipped{cause}` (parity, checksum, reserved, protected, udw_parity, length; the unit carries the total), `anc.pkts_truncated`, `anc.pkts_as_is`, `anc.pkts_located`, `anc.did_sdid_seen` G, `anc.units_over_window` |
| `pkt.` (packet units) | `chunks`, `units`, `pkts{leg}`, `bytes{leg}`, `stamp_violations{reason}`, `validate_violations{reason}`, `late_pkts`, `rx_ring_full`, `lend_to_copy`, `dedup_drops`, `seq_gaps` |
| `rtx.` (video, cvideo NACK) | `nacks_sent`, `nacks_received`, `retransmitted`, `recovered` |
| `info.` C, placement and pacing | `sched_index`, `pacing_profile`, `sender_type`, `tsmode`, `media_mode`, `tx_queue_kind`, `backend_syscalls_on_tasklet`, `numa`, `numa_mismatch`, `internal_buffer_count`, `internal_bytes`, `box_hdr_bytes`, `cvideo_rate_mode`, `cbr_headroom_bytes`, `anc_tm`, `anc_window_frame_offset`, `total_lines` |
| `info.` C, timing and conversion | `max_launch_delay`, `launch_delay`, `vrx_full`, `cmax`, `seq_restarted`, `troffset_ns`, `trs_ps`, `ptime_ps`, `tsdelay_ns`, `pickup_lead_ns`, `media_time_offset_ns`, `completion_latency_ns`, `expected_wake_latency_ns`, `tolerated_skew_ns`, `horizon_ns`, `rx_flush_offset_ns`, `convert_context`, `converter{name}` |
| `info.` C, wire and packet units | `wire_kbps{leg}`, `payload_kbps`, `max_udp_bytes`, `troffset_default`, `rx_path`, `pkts_per_chunk`, `slot_stride`; `ts_refclk.gm_identity{leg}` G, `ts_refclk.domain{leg}` G, `ts_refclk.kind{leg}` G |
| Phase 7 | `tx.rtcp_sr`, `tx.rtcp_info_version` G, `tx.f2f_pp_ns` G, `tx.units_no_key`, `rx.rtcp_sr`, `rx.rtcp_sr_dropped`, `rx.rtcp_info_version` G, `rx.sender_rate_ppb` G, `rx.crypto_auth_fail`, `rx.crypto_unknown_key{version}` |

**Port, scheduler, instance.**

| Scope | Keys |
|---|---|
| `port.` | `link_up` G, `speed_mbps` G, `rl_queues_in_use` G, `resets`, `rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`, `rx_errors`, `tx_errors`, `rx_missed`, `rx_nombuf`, `sampled_tai_ns` G, `free_tx_queues` G, `free_rx_queues` G, `free_rl_queues` G, `mcast_filters_used` G, `removed` G, `mac` C |
| `caps.` C | `backend` (`enum mtl_backend`), `pacing_classes`, `numa`, `tx_multi_seg`, `hw_rx_timestamp`, `hw_tx_timestamp`, `launch_time_offload`, `dma_engines`, `header_split`, `max_rl_queues`, `iova_va`, `iova_mode`, `backend_syscalls_on_tasklet` |
| `caps.` C, continued | `max_regions`, `link_source`, `page_size`, `hugepage_sizes`, `max_sessions{essence}`, `vf_trusted`, `mcast_filters_max`, `pf_tx_rate_mbps`, `phc_readable`, `reset_budget_ns` |
| `time.` | `source` G, `state` G, `ptp_domain` G, `offset_ns` G, `path_delay_ns` G, `last_sync_age_ns` G, `grandmaster_id` G, `utc_offset_s` G, `step_count`, `disciplined_by` G, `error_ns` G, `gm_traceable` G, `gm_clock_class` G, `gm_clock_accuracy` G, `gm_priority1` G, `gm_time_source` G, `gm_locking_status` G, `arb_timescale` G |
| `nic.` | driver xstats under the driver's names |
| `sched.` | `lcore` C, `sessions` G, `busy_pct_x100` G, `sleep_ratio_x100` G, `quota_used_1080p_x100` G, `loop_avg_ns` G, `loop_max_ns` G, `tasklet_p9999_ns` G, `tasklet_max_ns` G, `loops`, `wake_carried`, `wake_syscalls`, `last_loop_tai_ns` G (the heartbeat of `MTL_HEALTH_SCHED_STALLED`), `wake_k` G (the loop's flush bound, D-142), `marked` G (objects and queues marked, not yet flushed) |
| `instance.` | `api_version` C, `identity` C, `sched_count` C, `time_source` C, `flags` C, `debug_api` C, `simd_level` C (`enum mtl_simd`), `port_count` C, `iova_mode` C, `cpu_quota` C, `manager` G, `refcount` G, `wake_errors` |
| `instance.`, continued | `sessions` G, `sessions{essence,dir}` G, `sessions_closing` G, `regions` G, `regions_used` G, `regions_free` G, `lcores` G, `dma_devs` G, `health` G, `phase` G, `reconciled{kind}` |
| `mem.` (`numa=N`) | `hugepage_size` C, `hugepages_total` G, `hugepages_free` G, `library_bytes` G, `internal_bytes` G, `imported_bytes` G, `mempool_bytes` G, `largest_free_segment` G, `hugetlb_limit_bytes{size}` G, `hugetlb_usage_bytes{size}` G (the container's cgroup), `memlock_limit_bytes` C |
| `capacity.` | `free_lcores` G, `free_sessions_per_sched` G, `quota_free_1080p_x100` G, `quota_max_one_sched_x100` G |

**Queue.**

| Scope | Keys |
|---|---|
| `wq.` (queues; MS2a) | `armed` G, `reports`, `writes`, `writebacks`, `idle_slots` G (slots idle longer than 1 s: a forgotten re-arm) |

**Keys whose name does not say what they count.** A name without its meaning is half a contract; these are the definitions.

| Key | Meaning |
|---|---|
| `tx.units_suppressed` | outcomes counted instead of reported, because results are off |
| `tx.margin_ns` H | the results' signed `margin_ns` (negative = late), one sample per published result |
| `tx.launch_error_ns` H | `enqueued_tai_ns` (software) or `observed_first_tai_ns` (NIC timestamp) minus `scheduled_tai_ns` of the unit's first packet |
| `rx.latency_ns` H, `rx.latency_max_ns{window}` | first packet arrival minus the unit's media time |
| `rx.delivery_ns` H | `delivered_tai_ns` minus the unit's media time |
| `rx.units_before_start` | units discarded in ARMED, before the start's `when` |
| `tx.units_padded` | slots filled by the underrun policy: keep-alive, silence or empty ANC |
| `tx.build_overrun` | units the builder could not finish before their pick-up |
| `rx.units_dropped_notify` | deliveries the engine refused (SF-45) |
| `rx.units_reclaimed` | unread units replaced under `MTL_SESSION_RX_LATEST` |
| `leg.pkts_skipped` | TX packets not sent on a leg that is down or disabled |
| `leg.igmp_reports` | periodic membership renewals sent |
| `leg.pkts_late` | packets that arrived after their unit was delivered or flushed |
| `anc.did_sdid_seen` | up to 16 DID/SDID pairs seen, one slot each (Type 1 recorded as SDID 00h, RFC 8331 §3.1) |
| `anc.pkts_located` | entries with an exact line or S: the SDP then needs `VPID_Code` (ST 2110-40 §5.2.2) |
| `anc.units_over_window` | ANC units the launch rule could not fit in their window, their late packets counted (condition C, [timing.md](timing.md) §9.2) |
| `instance.cpu_quota` | the CFS quota in µs per period; 0 = none |
| `instance.identity` | the ID that log records (and events) carry for this instance: the handle of its first open, the same for every reference |
| `instance.manager` | the MtlManager connection, `enum mtl_manager_state`: `MTL_MANAGER_NONE` (not configured: normal, nothing logged), `MTL_MANAGER_CONNECTED`, `MTL_MANAGER_LOST`, `MTL_MANAGER_RECONNECTING` (sessions continue; reconnect in the background with back-off, re-registration and re-announcement of held lcores). A cached value, never a socket round trip |
| `capacity.free_lcores` | lcores the instance can still take (from MtlManager or the shared-memory allocator) |
| `capacity.free_sessions_per_sched` | free video session slots on the least-loaded scheduler (of 60, `ST_SCH_MAX_TX_VIDEO_SESSIONS`) |
| `capacity.quota_free_1080p_x100` | the scheduler data quota left over all schedulers, in 1080p59.94 equivalents × 100 |
| `capacity.quota_max_one_sched_x100` | the largest quota left on one scheduler: bounds the biggest single session |
| `caps.link_source` | how the link monitor learns the link state: LSC interrupt or polling (§14.2) |
| `info.tx_queue_kind` | a dedicated rate-limited TX queue, or a shared TX queue (software pacing) |
| `info.backend_syscalls_on_tasklet`, `caps.backend_syscalls_on_tasklet` | a mask of what makes syscalls on the tasklet: 1 the backend's packet I/O (kernel socket, AF_XDP), 2 the clock (a clocksource the vDSO cannot read, found at open); 0 none ([engine.md](engine.md) §1.3, §2.12) |
| `instance.reconciled{kind}` | what open cleaned up after an unclean exit (flow rules, rate-limit nodes, …) |
| `time.disciplined_by` | who disciplines the time base: the node, MTL, the application, none |
| `time.arb_timescale` | 1 when the grandmaster runs an ARB timescale (`clockClass` 220 or 228, or `timeSource` F0h, ST 2059-2 §5.5.4): times are common, not TAI (R5) |
| `time.gm_locking_status` | the SM TLV `masterLockingStatus` (ST 2059-2 §5.13): 0 not in use or unknown, 1 free run, 2 cold locking, 3 warm locking, 4 locked. 2 is the one standard warning that the grandmaster may step its time |
| `info.sender_type` | the granted ST 2110-21 sender type for `TP=` (§3.3, D-143); `-MTL_ENOTSUP` until MS2a |
| `info.tsmode` | the claimed `TSMODE` (`sc.tsmode`), `enum mtl_tsmode`; 0 = no claim (§3.2) |
| `info.cmax` | the CMAX of the granted sender type, the SDP's `CMAX`; for W at or above 900 000 packets/s, NL's CMAX (ST 2110-21 §7.1.3), always signalled |
| `info.troffset_default` | 1 when TROFFSET equals a TRODEFAULT ST 2110-21 defines for the format, 0 when the SDP must carry `TROFF` |
| `rx.detected.fps_approx` | 1 when the detected rate is an approximation with den ≤ 1023, not a candidate ([timing.md](timing.md) §11.9) |
| `time.source` on a wrapper | `MTL_TIME_SOURCE_LEGACY`, or `PTP_BUILTIN` for the legacy PTP clock (§2.8) |
| `caps.mcast_filters_max` | the groups left after MTL's own filters: all-hosts on every DPDK port (`mt_mcast.c:484`) and the PTP group when built-in PTP runs |
| `caps.reset_budget_ns` | the port-reset budget a shutdown must leave (§2.4 step 4) |
| `info.convert_context` | where conversion runs: caller, worker, plugin, or the RX tasklet with `rx.convert_per_packet` |
| `info.seq_restarted` | 1 when the RTP sequence was re-randomised at the last start |
| `info.ptime_ps` | audio: the granted packet time S / Fs in ps, rounded down; it differs from `audio.ptime` when that is not whole samples (`MTL_PTIME_80US` at 48 kHz: 4 samples, 83333333 ps; [timing.md](timing.md) §8) |
| `tx.snap_error_ns` | TAI media mode: the last unit's `snap_error_ns` |
| `info.pickup_lead_ns` | the pick-up lead the launch decision uses ([timing.md](timing.md) §6.1), not the builder ring depth |
| `sched.tasklet_p9999_ns` | the scheduler iteration's p99.99 since its first 30 s, from the log-linear iteration histogram (buckets at most 3.1 % wide, the bucket's upper edge); present only with `MTL_OPT_TASKLET_TIME_MEASURE` |
| `mem.mempool_bytes` | the DPDK mempools MTL created on the node, each with its per-lcore cache table: the ports' queue pools, the system and mono pools and every session's pools; summed at create and free, no walk |
| `rx.caller_work_ns`, `tx.caller_work_ns` | ns the data-path calls spent on work in the caller (conversion, ANC encode or decode, copies, the RX zero fill) |
| `rx.bytes_zero_filled` | bytes the RX zero fill wrote; with `rx.units_zero_filled`, the units it filled |
| `wait.armed`, `wait.fired` | read with plain loads at the moment of the read: a lane armed while its unit is ready, and still armed after the next flush, is the signature of a lost wake-up; a set `interrupted` explains a waiter that returns at once |

<!-- END TABLE stats-keys -->

Apart from the scheduler loop gauges the engine measures today (`sched.loop_avg_ns`), the library computes no averages: it exposes sums and counts, and readers divide.

The legacy stats fields map onto these keys in [migration.md](migration.md) §4.14. An exporter calls `mtl_instance_list_sessions()` (every live session, closing ones included), then lists and reads each session's schema.

## 12. Options

### 12.1 Presence

- An option is a tuning knob most applications never touch. It is absent by default, and **absent means the documented default; a present option is literal (0 means 0)**.
- `struct mtl_option`: `key` (`MTL_OPT_*`), `scope`, `value`, `str` (string keys).
- Pass options at open or create in an array (`mtl_instance_params.options`, `mtl_session_config.options`; deep-copied), or change them later with `mtl_set_options()` (several keys, all or none; the inline `mtl_set_option()` for one) where the key allows it.
- A later duplicate (key, scope) in one array wins.
- An unknown key is `-MTL_EINVAL`, `OPTION_UNKNOWN`; a value outside the key's range is `OPTION_RANGE`; detail names the key.
- A key that does not apply to the object is `-MTL_EINVAL`, `NOT_APPLICABLE`, naming the key: a
  session key whose `essence_mask`, `dir_mask` or `unit_mask` (§12.5) excludes the session, or an
  instance or port key, in a session's create array, in `mtl_session_query`, in `mtl_set_options`
  and `mtl_get_option` on a session. A key that does nothing is never accepted silently (D-175).
- Options are read on the control plane and copied into the object; no tasklet looks a key up.
- A present key is never overwritten by a value the library derives: where the two conflict,
  create fails with `OPTION_RANGE` naming the key. A key of a part not built yet that is changed
  after create is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`, R1).
- Option names and stats names are separate registries.
- `mtl_option_parse(name, value, &opt)` (AS, MS2a) fills one `struct mtl_option` from text (§12.5).
- Keys are stable or provisional (§12.6). Keys of Phase 7 are declared only under `MTL_LATER`; every other key is known from the start and is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`) until its milestone.

### 12.2 When a key may change

| Mark | Rule |
|---|---|
| C | at create (or open) only |
| S | also when STOPPED |
| R | also while running: at the next unit boundary, or at the boundary of the `mtl_session_update()` whose config carries it (Phase 7, later, for the update's boundary) |

`mtl_set_options(o, opts, n, flags)` with a key outside its rule is `-MTL_EBUSY`, `OPTION_STATE`,
naming the key, and applies none of them. With `MTL_OPTION_RESET` the listed keys return to
absent. `mtl_get_option(o, key, scope, &value, buf, cap)` returns the effective value, the derived default included; a string key goes to `buf` (NUL-terminated, `-MTL_ENOSPC` if `cap` is short) and the result is its length.

### 12.3 Scopes

Keys marked per port, per leg or per scheduler take a scope: 0 = every port, leg or scheduler the key applies to (or not scoped), else `MTL_INDEX(i)`. `mtl_option_desc.scope_kind` says which (`MTL_SCOPE_NONE`, `MTL_SCOPE_PORT`, `MTL_SCOPE_LEG`, `MTL_SCOPE_SCHED`).

### 12.4 Instance defaults for session keys

A session key set on the instance (`mtl_instance_params.options`, or `mtl_set_option` on the instance) is the default for that instance's sessions, as legacy instance-wide settings were. A key in the session's own array wins.
Example: `session.tx_queue` = `MTL_TXQ_SHARED` on the instance is the default TX queue placement of its sessions (legacy `MTL_FLAG_SHARED_TX_QUEUE`), and `session.migrate` on the instance lets the scheduler move its sessions (the legacy video migrate flags); so neither has an instance twin.
Such a default applies only to the sessions the key fits (§12.1); the others ignore it, because it
is a default, not a request.

### 12.5 Discovery

- `mtl_option_list(d, desc_size, cap, &n)` and `mtl_option_find(name, &d, size)` (AS) need no instance. They let GStreamer properties, FFmpeg AVOptions and bindings expose every knob without code per knob.
- `struct mtl_option_desc`: `key`, `object_kind`, `type` (`MTL_OPTION_INT`, `BOOL`, `ENUM`, `NS`,
  `STR`), `when` (`MTL_OPTION_AT_CREATE`, `MTL_OPTION_STOPPED`, `MTL_OPTION_RUNNING`), `scope_kind`,
  `essence_mask`, `dir_mask`, `unit_mask` (the session essences, directions and units it applies
  to: bit `1u << e` of `enum mtl_essence`, the `enum mtl_dir` values, bit `1u << u` of
  `enum mtl_unit_kind`), `flags` (`MTL_OPTION_PROVISIONAL`, `MTL_OPTION_DEPRECATED`), `min`, `max`, `def` (INT64_MIN for a
  derived default), `enum_names` (static `"name=value"` pairs: every value of an ENUM key,
  `"drop=1,defer=3"`, and the named special values of an INT or NS key, `"auto=-1"`; NULL if none),
  and `name`.
- `mtl_option_parse(name, value, &opt)` (AS, MS2a; D-154) is the one text form of an option, for
  GStreamer structure fields, `AVDictionary` entries and command lines.
  - `name` may end in "/N", the index from 0 of the port, leg or scheduler of a scoped key.
  - Values by type: INT decimal or 0x hex; BOOL 1/0/true/false/yes/no/on/off; ENUM a name or a
    number; NS an integer with ns, us, ms or s; INT and NS also a name of `enum_names`; STR as is.
  - It returns the key's object kind, so a framework routes instance keys to the instance and
    session keys to the session. A framework that exposes one element per essence lists only the
    keys whose masks include it.
  - Errors name the key in `mtl_last_error().field` (`OPTION_UNKNOWN`, `OPTION_RANGE`,
    `INVALID_ARGUMENT`).
  - Frameworks expose one container property ([migration.md](migration.md) §12.10).
- `mtl_option_find` returns `-MTL_EINVAL` (`OPTION_UNKNOWN`) when the running library does not know the name, and `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`) for a key it knows but does not implement yet: the feature test (R1); and `-MTL_ENOTSUP` (`OPTION_WITHDRAWN`) for a provisional key it withdrew.
- A generic exposure shows the tier: GStreamer appends "[provisional]" to the property blurb and FFmpeg to the AVOption help; a `MTL_OPTION_DEPRECATED` key is shown as deprecated (`G_PARAM_DEPRECATED`, `AV_OPT_FLAG_DEPRECATED`).
- Enumerated values never use 0 for "derived"; absence is that.

### 12.6 Key groups

The table below is generated from `lib/src/unified/mtl_options.def` (from task H1b): every key
with its name, type, default, mark, tier and milestone; until then it is the map by group.

**Stability.** A key is *stable* unless its header line ends with `P` (`MTL_OPTION_PROVISIONAL`
in `mtl_option_desc.flags`). A stable key freezes at `MTL_1.0`: its name, number and meaning never
change, and a later release may only make it an accepted no-op, flagged `MTL_OPTION_DEPRECATED`.
That no-op is not a silent ignore in the sense of R3: the flag is readable through
`mtl_option_find()` and `mtl_option_list()`, and generic exposures show it (§12.5). A provisional
key keeps its number and name for good (neither is ever reused), but its range, default and
meaning may change at any release, and it may be withdrawn: a withdrawn key is `-MTL_ENOTSUP`
(`OPTION_WITHDRAWN`) wherever it is passed, never ignored, and its `MTL_OPT_*` constant stays in
the header. Promotion clears `P` without a rename; no name prefix marks a tier. Provisional are the
engine, DPDK and legacy-compatibility knobs: the tier column of the table (until H1b, the `P`
marks of the header); `check.sh` prints the counts. A key that leaves `MTL_LATER`, or
is added later, starts provisional. The tiers are reviewed at MS6, before the freeze.

**Stream parameters are typed fields.** A parameter that the SDP carries and that is fixed for a
session's configuration is a field, not a key: `tsmode` and `max_udp_payload`
(`mtl_session_config`), `troffset_us` (`mtl_video_config`, `mtl_rtp_config`), `timing_model`
(`mtl_anc_config`). A tuning knob, or a value that changes at an update boundary (`rx.rtp_offset`,
R), stays a key; so does a deployment default (`instance.max_udp_payload`). The numbers 106, 112,
214, 304 and 700 are retired.

<!-- BEGIN TABLE options: lib/src/unified/mtl_options.def; gen_api_doc.py writes this region once that file exists (README §1) -->

| Range | Group | Examples |
|---|---|---|
| 100–199 | TX timing ([timing.md](timing.md)) | `tx.late_policy`, `tx.underrun_policy`, `tx.snap_mode`, `tx.snap_tolerance_ns`, `tx.horizon_ns`, `tx.index_offset` R, `tx.link_offset_budget_ns`, `tx.rows_late`, `tx.rtp_trim_ns` P, `session.epoch_tick`; Phase 7: `tx.late_tolerance_ns`, `tx.precede` |
| 200–299 | RX timing and delivery | `rx.incomplete`, `rx.rtp_offset`, `rx.link_offset_ns`, `rx.flush_offset_ns`, `rx.skew_budget_ns` (10 ms), `rx.signal_timeout_ns`, `rx.burst` (128), `rx.threads`, `rx.timing_parser`, `rx.convert_per_packet`, `rx.rows_step` (64); Phase 7, under `MTL_LATER`: `rx.mediaclk`, `rx.join_lead_ns` |
| 300–399 | queues and placement | `session.tx_queue`, `session.numa`, `session.src_port_mode`, `session.migrate` P, `session.tx_hang_detect_ns` P |
| 400–499 | capability requests | `caps.pacing` (a class), `caps.pacing_req` (PREFER or REQUIRE), `caps.dma`, `caps.hw_timestamps`, `caps.tx_copy` |
| 500–599 | video, cvideo (the `video.*` keys apply to cvideo too) | `video.disable_bulk`, `video.static_pad_p`, `video.start_vrx`, `video.pad_interval`, `video.convert_device` (on cvideo the codec device), `cvideo.threads`, `cvideo.quality`, `cvideo.pack`, `cvideo.max_bitrate_bps`; Phase 7: `video.vtotal`, `video.htotal` |
| 600–699 | audio | `audio.absorb_samples`, `audio.launch_offset_ns`, `audio.build_pacing`, `audio.fifo_ms` (10), `audio.rl_accuracy_ns`, `audio.rl_offset_ns` |
| 700–799 | ANC, fastmeta | `anc.target_delay_ns`, `anc.total_lines`, `fastmeta.target_delay_ns`; 704 and 705 retired |
| 800–899 | packet units | `packet.header_slot_bytes` (64), `packet.rx_min_packets` (1), `packet.rx_max_wait_ns` (one ptime, or 1 ms) |
| 1000–1009 | NACK retransmission (MTL's own; legacy "rtcp") | `rtx.enable`, `rtx.buffer_pkts`, `rtx.nack_interval_us`, `rtx.seq_bitmap_bytes`, `rtx.seq_skip_window` |
| 1010–1039 | sender reports, encryption, profile | `rtcp.*`, `crypto.*`, `session.profile`: Phase 7, declared under `MTL_LATER`; the TX sender-report keys (`rtcp.sr`, `rtcp.cname`, `rtcp.dst_port`) leave it in MS5 |
| 2000–2049 | instance: scheduling, threads, pods | `instance.sched_max` (≤ 18), `instance.sched_quota_mbs`, `instance.sched_sleep_us`, `instance.tasklets_per_sched`, `instance.sched_tx_audio_max`, `instance.sched_rx_audio_max`, `instance.rx_separate_video_lcore`, `instance.tasklet_time_measure` R, `instance.sys_lcore`, `instance.main_lcore` |
| 2000–2049, continued | | `instance.iova`, `instance.cni`, `instance.stall_ns` (1 s), `instance.cpu_arbitration`, `instance.cpu_shared`, `instance.allow_noiommu`, `instance.runtime_dir`, `instance.hotplug` P, `instance.telemetry` P, `instance.max_udp_payload` (1452: the default of a TX session's `sc.max_udp_payload`, §13.2); the command acknowledgement timeout is fixed (D-48), not a key |
| 2100–2199 | ports, all per port | `port.max_queues`, `port.tx_desc`, `port.rx_desc`, `port.rss`, `port.shared_rx_queue`, `port.no_igmp`, `port.af_xdp_copy`, `port.arp_timeout_s`, `port.dhcp`, `port.xsk_map`, `port.igmp_report_ms`; Phase 7, under `MTL_LATER`: `port.igmp_version` |
| 2200–2299 | time and logging | `time.ptp_domain` (127, range 0–127), `time.ptp_pi`, `time.ptp_unicast`, `time.phc2sys` (never in a pod), `time.phc_trust`, `time.ptp_announce_timeout` (3 intervals, range 2–10), `log.stat_dump_s` R, `instance.dma`; Phase 7: `time.fallback`, `time.freerun_slew_ppm` |

<!-- END TABLE options -->

Notes on keys whose rule the name does not give:

- `rx.threads`: the automatic value is 2 above 40 Gb/s. Today's engine rejects 2 with two legs or with rows units, so for them an explicit 2 fails at create (`-MTL_ENOTSUP`) and the automatic choice stays 1; use `caps.dma` instead.
- `instance.sys_lcore` places the system tasklets (CNI, ARP, PTP) on a shared scheduler or on their own (`MTL_SYS_SHARED`, `MTL_SYS_DEDICATED`). `instance.main_lcore` is the CPU of EAL's main lcore and of every thread that is not a scheduler, and is never given a scheduler. The two never coincide.
- `rx.convert_per_packet` is the one key that puts conversion on a tasklet (§9.7).
- `sc.tsmode` (§3.2, a field since OI-70) is what the session claims for the SDP's `TSMODE` (`info.tsmode`); 0 is no claim,
  `info.tsmode` 0, and an SDP without `TSMODE`, which receivers read as NEW (ST 2110-10 §8.7). The
  library cannot see whether an input was SAMP, so it never derives the claim: the application sets
  SAMP only when the RTP is the sampling or intended instant (a camera, a playback or SDI
  encapsulation that follows ST 2110-10 §7.6.2–§7.6.4 and §7.7.2–§7.7.5, a time-preserving processor
  whose input was SAMP, with an inclusive TSDELAY), PRES for a time-preserving processor of an input
  not marked SAMP, and NEW for one that re-stamps or snaps (§7.9, Annex C).
- `tx.rtp_trim_ns` on video, cvideo and ANC sessions keeps |value| < TFRAME, else `-MTL_EINVAL` (`OPTION_RANGE`): a playback RTP stays within ±TFRAME of the most recent N × TFRAME (ST 2110-10 §7.6.3); whole frames are `tx.index_offset`.
- `instance.max_udp_payload` (C, TX only) is the value a TX session's `max_udp_payload` takes when that field is 0 (an RX session's 0 means the Standard limit, §13.2), as legacy `pkt_udp_suggest_max_size` was instance-wide; it has the field's range (§13.2). The typed fields that replaced keys: §3.2 (`tsmode`, `max_udp_payload`), §3.3 (`troffset_us`, `timing_model`).

### 12.7 Capability requests

A capability request says what a session needs from the hardware. The rule is libfabric's: absent lets the library choose, a request is honoured or fails, and the grant is always reported. **No silent downgrade** (D-19).

| Request | When it cannot be met |
|---|---|
| absent | the library chooses; the choice is in `mtl_session_get_info` and the `info.*` keys |
| `MTL_REQ_PREFER` | success; the granted value differs, is visible in `mtl_session_get_info` and `info.*`, and is counted |
| `MTL_REQ_REQUIRE` | query and create fail (start, for what is known only then) with `-MTL_ENOTSUP` and a reason; `mtl_last_error().field` names the key |
| `MTL_REQ_OFF` | never use it: always met |

- `caps.dma` and `caps.hw_timestamps` take `enum mtl_req`. `caps.hw_timestamps` also needs the instance's `MTL_INSTANCE_HW_TIMESTAMP`.
- Pacing: `caps.pacing` names a class (`enum mtl_pacing`; absent = any) and `caps.pacing_req`
  says how hard: `MTL_REQ_PREFER` (absent) or `MTL_REQ_REQUIRE`, which fails with
  `PACING_UNAVAILABLE`. `caps.pacing_req` without `caps.pacing` on the session or as an instance
  default (§12.4) is `-MTL_EINVAL`. `MTL_PACING_HW` asks for any hardware class.
- `caps.tx_copy` forces the copy path; with `MTL_SESSION_REQUIRE_DIRECT` it is `-MTL_EINVAL`.
- The direct data path has one knob only, `MTL_SESSION_REQUIRE_DIRECT` (`DIRECT_IMPOSSIBLE`, §9.7).
- A later runtime downgrade is an event on every affected session (`MTL_EVENT_PACING_CHANGED`, `MTL_EVENT_LEG_STATE`) plus a status flag (`MTL_STATUS_PACING_DOWNGRADED`). The silent downgrades of today's engine are listed in [timing.md](timing.md) and [engine.md](engine.md).

### 12.8 TX queue placement

`session.tx_queue` (`MTL_TXQ_DEDICATED`, `MTL_TXQ_SHARED`) absent means, by essence:

| Essence | Default | Today |
|---|---|---|
| video, cvideo | a dedicated queue: rate-limited if the port grants `MTL_PACING_HW_RATE`, else software paced. **Never silently shared**: none left is `-MTL_ENOSPC`, `CAPACITY_TX_QUEUES` | `mt_queue.c:165` |
| audio | dedicated when its pacing resolves to the rate limiter, else shared | `st_tx_audio_session.c:2121-2124` |
| ANC, fastmeta | shared | `st_tx_ancillary_session.c:1693-1694`, `st_tx_fastmetadata_session.c:1438-1439` |

`session.tx_queue` set on the instance is the default (§12.4); the session's own wins. A shared TX queue forces software pacing for the whole port, which `info.tx_queue_kind` and `mtl_session_info.pacing_class` report.

## 13. Packet units

### 13.1 The model

- With `unit = MTL_UNIT_PACKETS` a session moves application-built RTP packets instead of frames, on any essence or on the generic RTP essence `MTL_RTP` (ST 2022-6, custom payloads). It replaces today's RTP-level sessions; no identifier says "passthrough".
- The verbs and the unit struct are the frame ones; only the unit's shape differs. **A chunk is the unit** for leases and results: one lease, one submit, one result per chunk.
- For timing, the unit is still the frame or field (video, cvideo, ANC, fastmeta, generic RTP) or the packet (audio). A unit spans one or more chunks, ended by `MTL_SUBMIT_UNIT_END`. Admission, late policy and RTP derivation are per unit.
- One unit kind per session, fixed at create: the slot shape, the pacing state and RTP ownership differ, and two producers of one RTP stream would collide on sequence numbers.
- TX chunk states: FREE → application (acquire) → QUEUED (submit) → IN_FLIGHT (picked up) → done
  when the last NIC buffer referencing it is freed; exactly one outcome per submitted chunk, results
  in submission order, each naming the unit the chunk belongs to (`media_index`). An RX chunk is
  formed at dequeue, so packet sessions skip RECEIVING and READY: FREE → application at dequeue →
  FREE at release, or held while a TX unit holds it.
- **No DPDK buffer is ever exposed, and no application code runs on a tasklet.**

### 13.2 Configuration

`sc.packet` (`struct mtl_packet_config`):

| Field | Zero means |
|---|---|
| `packets_per_chunk` | slots per lease, by essence: 32 for video, cvideo and generic RTP; 1 ms of packets for audio; 8 for ANC and fastmeta |
| `slot_bytes` | `sc.max_udp_payload`: 1452 B, the 1460 B Standard UDP Size Limit minus the UDP header (ST 2110-10 §6.3, `MTL_UDP_STD_LIMIT_BYTES`) |
| `packets_per_unit` | derived for video (the library packetiser's count, in `mtl_session_info.pkts_per_unit`), unlimited for ANC; required for cvideo and for generic RTP with UNIT pacing |
| `pacing` | `MTL_PKT_PACE_UNIT` |
| `unit_time` | `MTL_PKT_TIME_SUBMIT` |
| `rx_ring_packets` | 512: RX NIC buffers the session may hold (`-MTL_ENOSPC`, `RX_RING_BUDGET`, beyond the pool headroom) |
| `set_fields` | verbatim (§13.4) |
| `packet_flags` | `MTL_PKT_*` |

Sizes or counts that do not fit the port or the pacing class are `-MTL_EINVAL`, `PKT_CONFIG`.

**`sc.max_udp_payload`** (a field of `mtl_session_config`) bounds the RTP packet (UDP payload) of
every session, frame units included. **TX:** 0 = the instance's `instance.max_udp_payload`, else
1452 B, so the UDP datagram stays within the Standard UDP Size Limit of 1460 B (ST 2110-10 §6.3)
that every receiver accepts. Above 1452 only video and cvideo may go, up to min(port MTU − 28,
8952): that is the Extended UDP Size Limit of 8960 B (ST 2110-10 §6.4), and only then is `MAXUDP` =
this + 8 signalled in the SDP (§8.6; `info.max_udp_bytes`). `MTL_RTP` may also go up to MTU − 28
(an ST 2022-6 packet is up to 1456 B, §13.6). Any other essence above 1452 is `-MTL_EINVAL` naming
the field. **RX:** the sender's `MAXUDP` − 8; 0 = its SDP carries no `MAXUDP`, so receivers assume
the Standard limit, 1452 (§8.6); it sizes the receive buffers. **`VRX_FULL`** (both directions, the
timing parser included) takes `MAXUDP` = 1500 up to 1452, and this + 8 above it (ST 2110-21
§7.1.2–§7.1.4; [timing.md](timing.md) §5.1).

- Create checks: `slot_bytes` ≤ `sc.max_udp_payload` ≤ port MTU − 28 (`MTL_IPV4_HDR_BYTES`, `MTL_UDP_HDR_BYTES`); the packets per unit and the rate fit the granted pacing class (`PKT_CONFIG`); `rx_ring_packets` fits the RX queue pool headroom (`RX_RING_BUDGET`).
- The meta area of a chunk is `packets_per_chunk × sizeof(record)` (`struct mtl_pkt_tx` 16 B, `struct mtl_pkt_rx` 40 B), reported as `meta_capacity`.
- A UNIT-paced session asks a rate limiter for `packets_per_unit × slot_bytes × unit rate` (as today's `st_tx_video_session.c:84-91`); `rtp.bitrate_bps` 0 is the same product.
- An update in STOPPED may change `sc.packet`, never `unit`.

### 13.3 TX

- `mtl_tx_acquire` lends a chunk of `rows` packet slots of `row_bytes` capacity; the meta area is
  the packet table, one `struct mtl_pkt_tx` per slot, no meta header. Each entry's `data` (written by acquire, read-only to the application) is its slot, so the slots may be separate buffers (the data rooms of NIC buffers, never exposed as such) or one region; plane 0 addresses them only when they are contiguous (`addr` NULL otherwise).
- Write the RTP header and payload into slot i (`mtl_pkt_slot(&u, i)`, which reads `data`), set its `len` (`mtl_pkt_tx_table(&u)[i].len`; 0 skips the slot), set `u.used` to the packets used, and submit. The table is validated and copied at submit.
- With `MTL_PKT_SPLIT`, plane 0 holds header slots (`hdr_len`) and plane 1 payload slots, so payloads may live in another region.
- The library writes Ethernet, IP and UDP always. It duplicates every packet on both ST 2022-7 legs: the RTP bytes are identical on both legs by construction.
- A chunk that would take a unit past `packets_per_unit` is rejected (`PKT_COUNT`). A unit that ends short completes with `MTL_TXR_PKT_SHORT`, and the pacing grid is kept.

### 13.4 RTP header ownership

`packet.set_fields` lists the RTP fields the library writes; every other byte from the RTP header on is the application's. 0 (default) = verbatim.

| Bit | The library writes |
|---|---|
| `MTL_PKT_SET_TIMESTAMP` | `floor(M × rate)` from the unit's media time, one per unit; audio per packet from the sample index. `-MTL_EINVAL` (`PKT_CONFIG`) on an `SMPTE2022-6` session, whose packets each carry their own timestamp (ST 2022-6 §6.3): the application writes them |
| `MTL_PKT_SET_SEQ` | the 16-bit sequence number, and the extended sequence of RFC 4175 (video) or RFC 8331 (ANC), the payload's first two bytes, continuous across units |
| `MTL_PKT_SET_SSRC_PT` | SSRC and payload type from `sc.ssrc` and `sc.payload_type` |
| `MTL_PKT_SET_MARKER` | the essence's marker rule: M on the last packet of a unit (video: of a frame or field, ST 2110-20; cvideo; ST 2022-6: the last packet of a frame; ANC: also the empty packet of §5.1, ST 2110-40 §5.5), and M = 0 on every audio packet (AM824: ST 2110-31 §5.3; ST 2110-30 sets no rule for PCM, which follows the same) and every fastmeta packet (ST 2110-41 §5.2) |

ANC packet units build their payloads with the RFC 8331 helpers (§9.9); besides the extended
sequence the library writes nothing inside the payload.

- A stamped field is written before leg duplication, so it is equal on both legs.
- `MTL_PKT_TX_VALIDATE` checks V = 2, PT = the session's payload type, `len` ≤ `slot_bytes`, one timestamp per
  unit (`SMPTE2022-6`: timestamps that increase within the unit), the marker by the
  `MTL_PKT_SET_MARKER` rule, and with `MTL_PKT_TIME_SUBMIT` the application's
  timestamp against the derived one within ±1 unit; for video also the F bit, for audio the expected
  length from the packet time and format, for fastmeta the data item type against
  `fastmeta.data_item_type`. It counts violations (`pkt.validate_violations{reason}`), flags the
  chunk's result with reason `PKT_INVALID`, and never fixes a packet.
- `MTL_SUBMIT_RTP_TS` is for library-built packets; on a packet unit it is `-MTL_EINVAL` (stamping covers it).
- The header layouts (`struct mtl_rtp_hdr`, `mtl_rfc4175_hdr`, `mtl_rfc4175_srd`, `mtl_rfc9134_hdr`, `mtl_rfc8331_hdr`, `mtl_st41_hdr`) and endian-safe accessors (`mtl_rtp_get_ts`, `mtl_rtp_get_seq`, `mtl_rtp_set`) are in `mtl_packet.h`.

### 13.5 Pacing and unit time

| `pacing` | Packets leave | Compliance |
|---|---|---|
| `MTL_PKT_PACE_UNIT` | by the essence's wire model over each unit (video: ST 2110-21 over `packets_per_unit`; audio: the packet time; ANC: the ST 2110-40 window; generic RTP: `rtp.profile`) | compliant when the application sends the declared count |
| `MTL_PKT_PACE_LAUNCH` | a chunk at `unit.launch_tai_ns`, then at the session rate | the application's responsibility |
| `MTL_PKT_PACE_ASAP` | as fast as the queue accepts | non-compliant, reported |

| `unit_time` | The unit's media time |
|---|---|
| `MTL_PKT_TIME_SUBMIT` | from the first chunk's submission, by media mode, with the late policy and horizon of frames; later chunks of the unit carry no media fields |
| `MTL_PKT_TIME_FROM_RTP` | from the RTP timestamp of the unit's first packet (the inverse of the RTP rule, unambiguous within half a wrap); the launch index is the first at or after M + `min_tx_delay_ns`. `SMPTE2022-6`: the unit-grid instant nearest it (§13.6) |

- A chunk submitted after its due time leaves at once, bounded by the essence's burst limit, and is counted (`pkt.late_pkts`).
- If a unit's first chunk is admitted late, DROP latches the unit: its remaining chunks complete `DROPPED`/`TOO_LATE` until `MTL_SUBMIT_UNIT_END`.
- Whole-unit underrun defaults to SKIP for every essence in packet mode. `MTL_UNDERRUN_EMPTY_ANC` and `MTL_UNDERRUN_KEEPALIVE` need a library-built packet inside the application's stream, so they are accepted only with `MTL_PKT_SET_TIMESTAMP | MTL_PKT_SET_SEQ | MTL_PKT_SET_SSRC_PT`.
- Pacing classes: `MTL_PKT_PACE_UNIT` runs on `MTL_PACING_HW_RATE`, `MTL_PACING_HW_LAUNCH` or `MTL_PACING_SW`; `MTL_PKT_PACE_LAUNCH` on `MTL_PACING_HW_LAUNCH` or `MTL_PACING_SW` (with `MTL_SUBMIT_EXACT` reported non-compliant); a chunk without a launch time follows the previous one at the session rate (Rivermax's "0 = follow"); `MTL_PKT_PACE_ASAP` on any.
- A late chunk in the middle of an admitted unit leaves at once within the burst limit; its result keeps the unit's status and reports `max_packet_lateness_ns` in the full record, and the next unit still starts at its own launch index. DROP and DEFER apply per unit as for frames.
- `MTL_PKT_TIME_FROM_RTP` needs a timestamp the library can read (contiguous slots, or the SPLIT header plane). Half a wrap is 2^31 ticks: about 6.6 h at 90 kHz, 12.4 h at 48 kHz, 80 s at 27 MHz. It applies the processor recipe (M + `min_tx_delay_ns`) per unit, so a forwarder keeps the input's timestamps and a fixed RTP-to-launch offset.
- Today's `USER_PACING` and `EXACT_USER_PACING`, ignored on RTP sessions ([engine.md](engine.md) §7.4 #2), map to INDEX or TAI media mode with UNIT pacing, or to LAUNCH pacing.

### 13.6 Essence rules

| Essence | Rule in packet mode |
|---|---|
| video | field parity comes from the unit's media index, not the application's F bit (VALIDATE checks the F bit): today's RTP applications that never set the F bit send interlace at half rate (`doc/design.md:501`, `st_tx_video_session.c:1399-1404`) |
| cvideo | the RFC 9134 header fields are the application's; CBR needs exactly `packets_per_unit` packets per unit (`MTL_SUBMIT_UNIT_END` with fewer is `-MTL_EINVAL`); `MTL_CVIDEO_VBR_MAX` allows fewer; a rate-limiter class is downgraded to software as today, and reported |
| audio | the pacing unit is one packet; a chunk is consecutive packets, its media time the first sample of the first packet, each later packet first sample + the packet's samples; a chunk off the packet grid is a DISCONTINUITY (D-41); `MTL_SUBMIT_UNIT_END` is ignored |
| ANC | packets go out in the ST 2110-40 window, after the pacing sync (fixes [engine.md](engine.md) §7.4 #4); bytes are in wire order in both directions, the library swaps nothing (fixes #5) |
| fastmeta | data item type, K bit and length are the application's; the RX filters apply |
| generic RTP | `rtp.clock_rate` required; `MTL_RTP_LINEAR` (type NL) or `MTL_RTP_GAPPED` (type N, RACTIVE 1080/1125), below; `packets_per_unit` required with UNIT pacing, no pacing unit with LAUNCH or ASAP; `rtp.encoding` reported for SDP; RX duplicates removed as in §13.7; RTP only, no raw UDP |

**Generic RTP schedules.** `MTL_RTP_LINEAR` is the ST 2110-21 type NL read schedule over each
unit: TRS = unit period / `packets_per_unit`, TROFFSET = `rtp.troffset_us` or, 0, the ST 2022-8
§6 TRODEFAULT (the NL `VRX_FULL` × TRS). It is reported for `TP=2110TPNL` as `info.troffset_ns`,
`info.vrx_full` and `info.cmax`. `MTL_RTP_GAPPED` is the type N gapped schedule.

**ST 2022-6** (`rtp.encoding` `"SMPTE2022-6"`, timed by ST 2022-8) adds these rules:

- `rtp.clock_rate` 27000000, and `MTL_RTP_LINEAR`: ST 2022-8 §6 allows type NL or W, never
  the gapped schedule, so `MTL_RTP_GAPPED` is `-MTL_EINVAL`.
- The unit is one SDI frame, also for interlaced and PsF formats (ST 2022-8 §5.3 note 3):
  `rtp.unit.fps` is the SDI frame rate and `rtp.unit.scan` `MTL_PROGRESSIVE`.
- `packets_per_unit` is the ST 2022-6 §6.5 count for the SDI format: 4497 at 1080i29.97 and at
  1080p59.94 3G level A.
- A packet is 1396–1456 B of RTP: the 12 B RTP header, the 8 B payload header, 0–60 B of payload
  header extension and 1376 B of media (ST 2022-6 §6.2–§6.5). That is above the 1452 B of
  ST 2110-10, hence the MTU-derived limit of `MTL_RTP` (§13.2, PE7).
- Every packet carries its own timestamp (ST 2022-6 §6.3), so the application writes the
  timestamps: `MTL_PKT_SET_TIMESTAMP` is `-MTL_EINVAL`, `MTL_PKT_TX_VALIDATE` checks that they
  increase within the unit. `MTL_PKT_TIME_FROM_RTP` takes the unit-grid instant nearest the first
  packet's timestamp: for an epoch-aligned source that is the frame's Synchronizing Timestamp, which
  the first packet's own RTP precedes by 0.6–8.6 µs (ST 2022-8 §5.3).

**Timed text** (ST 2110-43, `rtp.encoding` `"ttml+xml"`, 90 kHz): with `rtp.unit` all zero the
session has no unit rate. It runs in TAI media mode only and never snaps a media time; UNIT pacing
is `-MTL_EINVAL`. With `MTL_PKT_PACE_LAUNCH` a document may leave before its media time, because it
becomes active at its RTP timestamp (ST 2110-43 §5.1), and LAUNCH pacing is not flagged
non-compliant on an essence that has no wire timing model.

### 13.7 RX

- `mtl_rx_dequeue` lends a chunk of 1..N received packets, formed at dequeue: up to `packets_per_chunk`, at least `packet.rx_min_packets`, or what arrived within `packet.rx_max_wait_ns`.
- The meta area is the table, one `struct mtl_pkt_rx` per packet (`mtl_pkt_rx_table(&u)`): `data` (the RTP header, or Ethernet with `MTL_PKT_RX_INCLUDE_L2`), `len`, `payload`/`payload_len` (SPLIT), `leg`, `seq` (extended for video), `gap` (missing sequence numbers before it, saturating), `flags`, `arrival_tai_ns`. `u.used` is the count.
- Packet flags: `MTL_PKTE_GAP_BEFORE`, `MTL_PKTE_UNIT_START`, `MTL_PKTE_MARKER`, `MTL_PKTE_REDUNDANT`, `MTL_PKTE_ARRIVAL_VALID`, `MTL_PKTE_HW_ARRIVAL`, `MTL_PKTE_HDR_EXT` (CSRCs or an RFC 8285 extension follow; `data` stays at the RTP header).
- **By default the packets are copied into the chunk in the caller** (dequeue is DPC), so NIC buffers are held only briefly. `MTL_PKT_RX_LEND` points the table into NIC buffers instead (zero copy, dequeue DP), bounded by `rx_ring_packets`; `mtl_rx_release` frees them.
- Duplicates of the two legs are removed for every essence by the key (RTP timestamp, sequence
  number; extended for video). The window holds `rx.skew_budget_ns` × the packet rate entries, and
  the timestamp keeps the key unique where the 16-bit sequence wraps inside it: ST 2022-6 at
  1080p59.94 sends about 270 000 packets/s and wraps every 0.24 s, inside a class C skew of 150 ms
  (ST 2022-7 Annex A). `MTL_PKT_RX_NO_DEDUP` delivers both copies (analysers), the second flagged
  REDUNDANT.
- A packet dropped because the session's RX ring is full (`pkt.rx_ring_full`) is not marked received, so its copy from the other leg is still delivered (G-PKT-5, [requirements.md](requirements.md) §4.5; today's order is the opposite, [engine.md](engine.md) §7.4 #8).
- Under `MTL_PKT_RX_LEND`, once `rx_ring_packets` packets are lent the next chunks are copied and counted (`pkt.lend_to_copy`).
- `MTL_PKT_RX_UNIT_ALIGNED`: a chunk never spans two units; it ends at a marker or a timestamp change, for application frame assemblers.
- `MTL_PKTE_HW_ARRIVAL` needs `caps.hw_timestamps` = `MTL_REQ_REQUIRE` (and the instance's `MTL_INSTANCE_HW_TIMESTAMP`). `MTL_PKT_RX_NO_DEDUP` plus per-leg arrival lets analysers measure the path differential.
- The chunk's unit record: packets in the chunk, per-leg received counts, packets recovered from the second leg only, missing sequence ranges, first and last arrival, and the unit's `media_index` when the first packet's timestamp maps onto the epoch. Frames are the application's to assemble. `MTL_STATUS_RX_SIGNAL` uses packet arrival.
- Packets are delivered in arrival order; there is no reordering.
- RTCP NACK is not offered in packet mode.

### 13.8 Results and the rest

- One TX result per chunk, with the unit's media index and time and the first packet's RTP on the wire. Reasons as for frames, plus `PKT_COUNT` and `PKT_INVALID`.
- Packet sessions share start arrays with frame sessions, only with `MTL_PKT_TIME_SUBMIT` and `MTL_PKT_PACE_UNIT`: app-built ANC with `MTL_PKT_SET_TIMESTAMP` beside a library video session gets the video's RTP and window.
- An update changes the Ethernet, IP and UDP headers at a unit boundary, never the RTP bytes.
- Holds work as for frames: zero-copy RTP-level forwarding with TX chunk slots over an RX chunk pool and `unit.hold` = the RX lease. With `MTL_PKT_RX_LEND` the TX side chains the lent NIC buffers behind new Ethernet, IP and UDP header buffers (zero copies end-to-end); held lent packets count against the RX `rx_ring_packets`.
- **Recipe, analyse and forward verbatim.** RX: a video packet session on two legs with
  `MTL_PKT_RX_UNIT_ALIGNED`. TX: `MTL_RTP`, `set_fields` 0, `unit_time` = `MTL_PKT_TIME_FROM_RTP`,
  `MTL_PKT_PACE_UNIT`, `min_tx_delay_ns` 2 ms, `rtp.clock_rate` 90000, the
  input's unit rate. Per RX chunk: note `MTL_PKTE_GAP_BEFORE` (`seq`, `gap`) and `arrival_tai_ns`
  per leg, copy each packet into a TX slot with its `len`, set `MTL_SUBMIT_UNIT_END` when the last
  packet has `MTL_PKTE_MARKER`, submit, release the RX chunk. A TX miss drops the chunk, and the TX
  stats show it.

## 14. Legs and ST 2022-7

### 14.1 Which legs exist

- A session has up to `MTL_MAX_LEGS` (2) legs. A leg exists when its `udp_port` is not 0. `flows[1]` that exists is the ST 2022-7 leg.
- A **reserved leg** (its bit in `legs_disabled` set, its flow all zero) also exists; it is Phase 7, and that bit on an all-zero flow is `-MTL_ENOTSUP` until then (§14.2).
- **Two legs with the same source and destination** (the same port, `ip` and `udp_port`, and TX `udp_src_port` or RX `source_filter`) are `-MTL_EINVAL`, naming `flows[1]`: an SDP cannot describe them (ST 2110-10 §8.5).
- **The set of existing legs is fixed at create**; in STOPPED, an update with `MTL_UPDATE_FLOWS` or `MTL_UPDATE_LEGS` may change it.
- Create reserves the queue, flow rule and scheduler quota of every leg whatever its link state. A configured leg is never pruned (G-91); the legacy `ALLOW_DOWN_PORTS` pruning stays legacy-only.
- `flows[i].port` 0 is the leg's own instance port, so a literal two-leg flow never puts both legs
  on one NIC by accident. The field holds `MTL_INDEX(port)`: set it with
  `mtl_flow_on_port(&sc.flows[i], port)`. Both legs on one instance port (`flows[1].port = 1`,
  which is port 0, or two explicit equal ports) are accepted, since a one-NIC test and a one-port
  null instance need them, and reported: `MTL_INFO_LEGS_SHARE_PORT` in `mtl_session_info.flags`
  (from `mtl_session_query` as well) and one WARNING log line at create naming `flows[1].port`. ST
  2022-7 then covers loss on the network paths, not a NIC or link failure.

### 14.2 Admin, oper and flow state

- `legs_disabled` sets the admin state per leg (`MTL_UPDATE_LEGS` changes it; a boundary command while started, so a leg never switches mid-unit).
- TX: a disabled leg builds and sends nothing, and keeps its reservation.
- RX: a disabled leg's packets are ignored; the rule stays installed and the membership is left. Re-enabling joins again.
- A **reserved leg** is a disabled leg that stays all zero until an update gives it an address together with `MTL_UPDATE_LEGS` (Phase 7, later; `-MTL_ENOTSUP` until then).
- **Every existing leg disabled = muted** (Phase 7, later; `-MTL_ENOTSUP` until then, and `MTL_STATUS_MUTED` is declared under `MTL_LATER`): the session stays RUNNING, TX units retire at their indices (counted in `tx.units_muted`, not `tx.units_dropped`), no sender reports, RX leaves its groups. `MTL_STATUS_MUTED` is a transport state.
- `status.leg[]` (`struct mtl_leg_status`): `admin` (1 = enabled), `oper` (1 = link up and flow resolved or joined), `flow_state` (`enum mtl_flow_state`). TX: `MTL_FLOW_WAITING_NEIGHBOUR` or `MTL_FLOW_RESOLVED`. RX: `MTL_FLOW_JOINING`, `MTL_FLOW_JOINED`, `MTL_FLOW_JOIN_FAILED`.

The flow state of a leg moves as in the two pictures below. A TX leg waits for its neighbour from
the start, or from an update of FLOWS, until the neighbour is resolved; a unit on a leg still
waiting is not sent there (reason `WAITING_NEIGHBOUR`), and an update does not wait for it (§4.7).

```mermaid
stateDiagram-v2
    direction LR
    [*] --> MTL_FLOW_WAITING_NEIGHBOUR: start, or<br/>update of FLOWS
    MTL_FLOW_WAITING_NEIGHBOUR --> MTL_FLOW_RESOLVED: neighbour<br/>resolved
```

An RX leg is joining from the moment its IGMP join is sent, until the join succeeds or fails.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> MTL_FLOW_JOINING: IGMP join sent
    MTL_FLOW_JOINING --> MTL_FLOW_JOINED: joined
    MTL_FLOW_JOINING --> MTL_FLOW_JOIN_FAILED: failed
```
- A leg carries traffic only when it is admin enabled, its link is up and its flow is resolved or joined. `MTL_EVENT_LEG_STATE` and `MTL_EVENT_FLOW_STATE` report changes.
- Oper state comes from a link monitor (MS5, a prerequisite of `MTL_EVENT_LEG_STATE`;
  `caps.link_source`): the LSC interrupt where the PMD supports it, else the admin thread polls
  `rte_eth_link_get_nowait()` every 100 ms; netlink on the kernel-socket and AF_XDP backends. A down
  leg is skipped (§14.3) and its queue is reset through the stalled-queue path
  ([engine.md](engine.md) §4, §5), so the pool never starves. Today the link is probed only in
  `mtl_start` ([legacy-internals.md](legacy-internals.md)).
- `mtl_session_info.leg[]` (`struct mtl_leg_info`) gives the granted values per leg: port, SSRC, source and destination ports, payload type, DSCP, TTL, source and resolved destination MAC.

### 14.3 TX

- One TX session builds every leg's packets from one header set: the payload and the RTP header, SSRC and payload type included (`sc.ssrc`, `sc.payload_type`), are identical on both legs, and an update switches all legs on the same unit.
- A leg whose link is down is skipped: its packets are counted in `leg.pkts_skipped`, and its descriptors are reclaimed through the stalled-queue path. The units complete with that leg marked not sent (`leg_reason` in the full record), so the pool never starves on a dead link. The leg resumes at the next unit boundary when the link returns, without application action.
- A unit is `ON_TIME` when sent on at least one leg; it is `DROPPED` with `LINK_DOWN` or `WAITING_NEIGHBOUR` only when no enabled leg sent it. With every leg disabled the session is muted (§14.2, Phase 7): its units count in `tx.units_muted`, not as drops (§6.3).
- `leg.observed_skew_ns` reports the launch difference between the legs.

### 14.4 RX

- Packets of both legs feed one unit. Duplicates are removed (frames: in reassembly; packet units: by RTP timestamp and sequence number, §13.7).
- `rx.skew_budget_ns` (10 ms) is the tolerated path differential; `rx.flush_offset_ns` defaults to it with two legs and to 1 ms with one. A unit's due time is first-packet arrival on the earliest leg + unit period + the flush offset.
- `leg.observed_skew_ns{window}` reports the path differential seen: this leg's first-packet
  arrival minus the earliest leg's, the maximum over the window. It informs the choice of
  `rx.skew_budget_ns`; nothing in the library acts on it.
- A unit completed by the other leg carries `MTL_UNITF_USED_REDUNDANCY`; `mtl_rx_get_detail()` gives `pkts_received[]` per leg and `pkts_recovered`.
- Per-leg counters: `leg.pkts`, `leg.pkts_lost`, `leg.pkts_reordered`, `leg.pkts_duplicate_same_leg`, `leg.pkts_late`, `leg.receiving`.
- A non-zero `sc.ssrc` or `sc.payload_type` on RX is checked on every leg; packets that differ are rejected and counted (`rx.pkts_rejected{cause=ssrc}`, `{cause=pt}`).
- **Skew.** The out-of-order units the receiver tracks are n = ceil(`rx.skew_budget_ns` / TFRAME) + 1
  (3 at 119.88p with the 10 ms class A default). n units tolerate a path differential below
  (n − RACTIVE) × TFRAME, not (n − 1) × TFRAME, because the lagging leg's unit is evicted when the
  leading leg starts unit N + n. Where the engine's cap is smaller (2 today: 17.4 ms at 59.94p, but
  8.7 ms at 119.88p, below class A), `info.tolerated_skew_ns` reports (n − RACTIVE) × TFRAME for the
  n it has and the session warns (MS1) (ST 2022-7 §7 Table 1; [standards.md](standards.md) §12).
- **Output latency.** The due time follows each unit's earliest-leg arrival, while ST 2022-7 §7 fixes
  PT at start-up: when the leading leg fails, delivery moves later by up to the path differential.
  A fixed presentation time needs `rx.link_offset_ns` *(inferred)*.
- **Relock** (MS2). The receiver relocks onto a stream only when the stale value lags the newest by more than `rx.skew_budget_ns` worth of media-clock ticks, or when every enabled leg is stale ([timing.md](timing.md) §11.5).
- Losing a leg: link loss is an event, not ERROR (§4.10); device removal with a surviving leg leaves the session degraded, with none it enters ERROR (§2.6). Use one SR-IOV resource per network, so each leg's VF comes from its own PF ([deployment.md](deployment.md)).

## 15. Formats, plugins, capture and IPMX calls

### 15.1 Standalone conversion

`mtl_convert(&d)` (DPC, `mtl_format.h`, MS4) runs the converters of the sessions outside a session (file tools, test pattern generators): one call for every pair; the pair, the CPU features and an optional DMA engine are chosen inside.

- `struct mtl_convert_desc` (`struct_size` first, R3): `width` × `height` pixels (audio: `width` = samples per channel, `height` 1), `src` and `dst`, `flags`, `mt`, `channel_map`.
- `struct mtl_convert_image`: `format` in the number space of `kind` (`MTL_FORMAT_TRANSPORT`, `MTL_FORMAT_APP` or `MTL_FORMAT_AUDIO`), `plane_count`, `channels` (audio: interleaved channels; video 0), `plane[]` (audio: one plane).
- `mt` null: CPU only, no instance needed. An instance: the call may use its DMA engines.
- Flags: `MTL_CONVERT_NO_SIMD` (the scalar reference path, for tests), `MTL_CONVERT_FIELD_SPLIT` (a
  frame in, two fields out: `dst` planes 0–1 and 2–3), `MTL_CONVERT_FIELD_MERGE` (two fields in, one
  woven frame out), `MTL_CONVERT_HALF_SCALE` (downsample by 2 in both directions),
  `MTL_CONVERT_CHECK` (only whether MTL converts the pair, in this call or in a session: 0 or
  `-MTL_ENOTSUP`; the planes are not read).
- Audio: AM824 ↔ AES3 (`MTL_AES3`, `enum mtl_audio_format_ext`), or a channel remap within one format. `channel_map` has `dst.channels` entries: `dst` channel i = `src` channel `channel_map[i]`, or `MTL_AUDIO_SILENT`; NULL = the same order. The remap is a later capability: a non-NULL `channel_map` is `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`) until then.
- Returns 0; `-MTL_ENOTSUP` for a pair MTL does not convert; `-MTL_EINVAL` for an invalid descriptor (R1, R3).
- Ownership: the planes stay the caller's. The call reads `src` and writes `dst` on the caller's thread, bounded by the frame size, and keeps no reference after it returns.

### 15.2 Format descriptions

- `mtl_format_describe(format, kind, width, height, &out, size)` (AS) fills `struct mtl_format_desc`
  (output; `size` versions it): the names `name`, `sdp` (codecs: the encoding name), `gst`,
  `av_pix_fmt` (static strings, NULL where none exists), `fourcc`, `plane_count`, the pixel group
  (`pg_bytes`, `pg_pixels`; audio: bytes per sample and 1), and the layout of one frame of `width` ×
  `height` (`row_bytes[]`, 0 when `width` is 0; `rows[]`; `plane_bytes[]`). 0 × 0 gives the names
  and the pixel group only. `-MTL_EINVAL` for an unknown format.
- `kind` (`enum mtl_format_kind`) names the number space, because format values overlap between kinds: TRANSPORT (`enum mtl_video_format`, `enum mtl_video_format_ext`), APP (`enum mtl_app_format`), CODEC (`enum mtl_codec`), AUDIO (`enum mtl_audio_format`, `enum mtl_audio_format_ext`).
- `mtl_format_parse(name, &format, &kind)` (AS) is the reverse: a name, an SDP string, a codec name or an FFmpeg or GStreamer name to a format and its kind; a name that matches nothing is `-MTL_EINVAL` (R1).
- A session's wire bandwidth is `mtl_session_info.wire_bps`, filled by `mtl_session_query` (no instance resources needed) and `mtl_session_get_info` (§3.5).
- `mtl_format_describe` and `mtl_format_parse` need no instance; the strings they return are static, valid for the life of the process. `mtl_audio_bytes(&a, samples, &bytes)` is the inline for audio sizes.

### 15.3 Plugins

`mtl_plugin_open(mt, path, dev, &p)` (CP, `mtl_plugin.h`) adds a codec or converter device to the instance.

- Exactly one of `path` and `dev`, else `-MTL_EINVAL`. `path` loads a plugin shared object into the instance; `dev` registers an in-process device (tests, applications that embed a codec; `MTL_CODEC_DEVICE_TEST`).
- A plugin exports one symbol, `MTL_PLUGIN_ENTRY_SYMBOL` (`"mtl_plugin_entry_v2"`), and never links libmtl: the host passes it a function table. Its entry returns < 0 when it cannot serve the host's ABI (`MTL_PLUGIN_ABI_VERSION` 2), and the open then fails.
- Formats are explicit pairs (`struct mtl_plugin_format_pair`) in the plugin number space: a transport format below 0x10000, `MTL_PLUGIN_APP(fmt)` or `MTL_PLUGIN_CODESTREAM(codec)`, never a bare application value.
- Ownership: the device struct and what it points to stay valid until `mtl_plugin_unload` returns (the one exception to R3's deep copy). The host table (`struct mtl_plugin_host`) is valid from `create_session` until `free_session` returns; a frame is lent to the plugin between `get_work` and `put_work`.
- Threads: plugin threads wait for work in `host->get_work()`; no plugin code runs on an MTL tasklet (R6).
- `mtl_plugin_unload(p)` (inline, `mtl_close` on the plugin) is `-MTL_EBUSY` while a session uses the plugin, and then does not consume `p`.

### 15.4 Packet capture

`mtl_session_capture(s, &p)` (CP, `mtl_observe.h`) copies a session's packets to a pcapng file from a library worker, never on a tasklet; `MTL_EVENT_CAPTURE_DONE` reports the end.

- `struct mtl_capture_params` (`struct_size` first, R3): `max_pkts` (0 = until stopped), `path` (the pcapng file, copied at the call), `legs` (a bit per leg; 0 = all).
- `p` NULL stops a running capture.
- Codes: those of R1–R4 (`-MTL_EINVAL` for an invalid struct, `-MTL_EBADF`).
- Payload keys never reach a capture (§15.5).

### 15.5 RTCP, SDP and encryption

Everything of `mtl_ipmx.h` is Phase 7 and declared only under `MTL_LATER`, with the option keys
(`rtcp.*`, `crypto.*`, `session.profile`), their value enums and the inline `mtl_rtcp_mib_next`. A
Phase 7 name leaves `MTL_LATER` in the milestone that implements it: TX sender reports driven by the
`rtcp.*` options, with the library-built Info Block, in MS5. The behaviour of sender reports, SDP
and encryption is in the header comment and [nmos-ipmx.md](nmos-ipmx.md).

- `mtl_rtcp_set_info(s, &info)` (CP): the Info Block fields of `struct mtl_rtcp_info` (deep-copied,
  R3) apply from the next report. When the bytes change the block version increments
  (`tx.rtcp_info_version`) and `MTL_EVENT_RTCP_INFO` is posted; a unit's `MTL_META_RTCP_MIB` record
  only appends to that unit's report and never changes the version. `-MTL_ENOSPC` if the report,
  Info Block and SDES do not fit one datagram; `-MTL_EINVAL` beyond the wire field sizes.
- `mtl_rtcp_read(s, &rpt, rpt_size, buf, cap, timeout)` (WT): the oldest unread report; a full ring
  drops the oldest (`rx.rtcp_sr_dropped`). 1, or `-MTL_EAGAIN`.
  `buf` (may be NULL) receives up to `cap` bytes of the Info Block (`info_bytes`); `mtl_rtcp_mib_next`
  iterates its Media Info Blocks from `mib_offset`.
- `mtl_sdp_render(s, &meta, buf, cap)` (CP): the SDP of a created session as it is now; built only
  on public calls, in libmtl_sdp (`mtl_sdp.h`), it allocates nothing. The length written, without
  the NUL; `-MTL_ENOSPC` if `cap` is short; `-MTL_EBUSY` while a value it needs is not known yet (an
  ANC raster before start).
- `mtl_sdp_parse(sdp, len, &sc, &meta)` (CP): fills `sc` (flows, payload types, source filters, the
  essence member; direction as given) and `meta` (`meta.options`: `rx.rtp_offset`, `rx.mediaclk`,
  `crypto.*`, for the caller to pass in `sc.options`). Returns the leg count; legs beyond those
  parsed are zeroed and their `legs_disabled` bits set. Unknown attributes are ignored; a malformed
  required one is `-MTL_EINVAL` with its name in `meta.error`; a string that does not fit its field is `-MTL_ENOSPC`
  naming the field.
- `mtl_crypto_set_key(s, key_version, key, key_bytes, when)` (CP): installs a 16- or 32-byte key,
  copied into locked, non-dumpable memory, zeroised on replace and close, never readable back and
  never in logs, stats or captures. `key` NULL zeroises every key, and TX units are then
  `MTL_TX_DROPPED`, `NO_KEY`. TX: from the first unit at or after `when` (NULL = the next unit). RX:
  units with media time at or after `when` (§4.7), kept beside the current key and chosen per packet
  by key version. A re-installed key continues its counter, so no IV and counter pair repeats under
  one key.

## 16. Where the rest is

What this file does not state, and where it is.

| Topic | Where |
|---|---|
| the wait protocol, waker options W0–W2 and the notifier contingency, command classes, the slot table layout, completing contexts | [core.md](core.md) §4–§6 |
| producer-by-producer memory recipes (GStreamer, FFmpeg, MXL, capture SDKs), framework pool sizing | [migration.md](migration.md) §12.4–§12.7; early source release is §9.11 |
| event producer classes and coalescing construction | [core.md](core.md) §6.5 |
| stats read construction (per-writer blocks, scans, seqlocks), USDT probes | [engine.md](engine.md) §1.8 |
| backends and what each grants, plugin and sample parity, OBS (capability requests are §12.7) | [engine.md](engine.md) §1.7; [migration.md](migration.md) §11.3–§11.5, §11.7; [coverage.md](coverage.md) |
| packet mode cost model, today's RTP-level defects, legacy mapping | [engine.md](engine.md) §6 (PE1–PE9) and §7.4; [coverage.md](coverage.md) §2.16; [requirements.md](requirements.md) §3.3, §4.5, §8 |
| the defaults | each key's comment in `mtl_options.h`; §2.1, §3.2, §3.3, §12.6, §12.8; [timing.md](timing.md) |
| the one-line rationale of each design decision (D-xx) | [decisions.md](decisions.md) |
| the plugin ABI beyond `mtl_plugin_open` (the device, session and frame structs) | the header comment of `mtl_plugin.h` |
| sender report schedule, Info Block contents, SDP lines, the encryption scheme (Phase 7) | the header comment of `mtl_ipmx.h` and `mtl_sdp.h`; [nmos-ipmx.md](nmos-ipmx.md) |
| pod privileges, MtlManager in Kubernetes, AF_XDP from a node daemon, time in a pod | [deployment.md](deployment.md) §4.7–§4.15 |
