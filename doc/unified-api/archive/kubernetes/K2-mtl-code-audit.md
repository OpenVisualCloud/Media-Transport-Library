# K2 — What MTL leaves behind when a Kubernetes pod stops

Audit of the Media Transport Library at commit `545a266a` (DPDK pinned to 26.07,
`versions.env:1`) for running inside a Kubernetes pod. A pod can be stopped (SIGTERM, then
SIGKILL after `terminationGracePeriodSeconds`, default 30 s), OOM-killed (SIGKILL),
restarted in place (new container, same pod sandbox: same IPC, network and UTS
namespaces, new PID namespace and root filesystem), or deleted, with its VF handed to
another pod. This is the evidence base for the Kubernetes requirements on the new API
(`REVISION-4.md`, `sketch/include/mtl/experimental/*.h`).

Labels: **[verified]** means read in this tree, or in the DPDK 26.07 source on this host
where a path starts with `DPDK`. **[inferred]** means a consequence of verified code plus
Linux, DPDK or ice behaviour that was not tested here. Paths without a prefix are under
`lib/src/`.

## 0. Summary

- MTL installs no signal handler, no `atexit` handler and no watchdog. Every exit path other
  than a completed `mtl_uninit` is a SIGKILL path (`grep` of `lib/src` for `signal(`,
  `sigaction`, `atexit`: no hits) **[verified]**.
- Most process-scoped state is reclaimed by the kernel on SIGKILL: hugepages (because of
  `--in-memory`), vfio, XSK sockets, kernel sockets and their multicast memberships, flocks.
- What survives a SIGKILL, and so reaches the next process or pod, is: state in the NIC or
  in the PF driver (RL shapers, filters, PHC frequency), state in the network (IGMP
  memberships, neighbour caches), state in the node (CLOCK_REALTIME frequency from
  phc2sys, `tx_maxrate`, XDP programs and ethtool rules owned by MtlManager), and state in
  shared IPC (the SysV lcore table).
- The critical hazards are DMA into freed memory in vfio no-IOMMU mode (§7), the opt-in
  phc2sys service steering the node's clock (§2.1), and MtlManager run as a DaemonSet,
  which resolves ifindexes in its own network namespace (§4).
- DPDK itself registers one `atexit` handler, for its telemetry socket (§1.1).
- Shutdown has unbounded waits (`sch_stop`, `mt_handle_drain`). A stuck tasklet therefore
  turns SIGTERM into SIGKILL, and an unregister timeout leads to a use-after-free (§5).
- There is no heartbeat a liveness probe could read (§6).

## 1. Inventory: common to every backend

### 1.1 EAL and hugepage memory

| Item | Evidence |
|---|---|
| EAL argv always has `--file-prefix MT_DPDK --match-allocations --in-memory` | `dev/mt_dev.c:336-345`, `mt_mem.h:11` **[verified]** |
| No `--socket-mem`, `--huge-dir`, `--no-telemetry` or `--legacy-mem`; memory grows on demand | `dev/mt_dev.c:320-497` **[verified]** |
| `--in-memory` sets `no_shconf` and `unlink_before_mapping` | DPDK `lib/eal/common/eal_common_options.c:2236-2240` **[verified]** |
| Without a hugetlbfs mount, in-memory mode allocates hugepages anonymously | DPDK `lib/eal/linux/eal_hugepage_info.c:502-510` **[verified]** |
| `--iova-mode` is passed only when the user sets it; otherwise DPDK chooses | `dev/mt_dev.c:453-461` **[verified]** |
| EAL runs on a temporary pthread, joined at once, so the caller's thread is not pinned | `dev/mt_dev.c:302-307`, `:505-519` **[verified]** |
| EAL cannot be initialised twice in a process: a static flag, never reset | `dev/mt_dev.c:324`, `:500-503`, `:522`; `rte_eal_cleanup` at `:2172` **[verified]** |
| A runtime directory is still attempted (errors ignored in `no_shconf` mode) | DPDK `eal_common_options.c:2414-2420` **[verified]** |
| A telemetry socket `dpdk_telemetry.v2` is created there, with `atexit(unlink_sockets)` | DPDK `lib/telemetry/telemetry.c:610-652` **[verified]** |
| During page allocation DPDK temporarily replaces the SIGBUS handler | DPDK `lib/eal/linux/eal_memalloc.c:84-120` **[verified]** |

Lifecycle:

- **Clean `mtl_uninit`:** mempools are freed, ports closed, then `rte_eal_cleanup`
  (`mt_main.c:628-664`, `dev/mt_dev.c:2169-2176`) **[verified]**.
  The process cannot call `mtl_init` again, because `eal_initted` stays true **[verified]**.
- **SIGKILL or OOM:** no hugetlbfs file exists to leak; the anonymous or memfd pages go back
  to the node's pool when the mm and fds are released **[inferred]**. The telemetry socket
  file stays in the container's runtime directory. The next process detects that it is
  stale, unlinks it and rebinds (`telemetry.c:500-545`), and a live one makes it take a
  `:N` suffix **[verified]**.
- **Pod budgets:** DPDK sizes from the node-wide `/sys/kernel/mm/hugepages` counts, not from
  the pod's hugetlb cgroup limit. Growth past the limit faults; DPDK's SIGBUS trap turns that
  into an allocation failure, not a crash **[inferred]**.
- **Retry:** any `mtl_init` failure after EAL init (link down, DHCP timeout) can only be
  retried by restarting the process **[verified]**. A crash-loop restart is the only retry
  in a pod.

### 1.2 Lcore table: SysV shm and flock (used when MtlManager is absent)

| Item | Evidence |
|---|---|
| Lockfile `/tmp/kahawai_lcore.lock`, `open(O_CREAT, 0666)` then a read-only retry | `mt_platform.h:57`; `mt_sch.c:505-525` **[verified]** |
| `flock(LOCK_EX)` with no timeout | `mt_sch.c:517` **[verified]** |
| SysV key `ftok("/dev/null", 21)`, `shmget(0666 \| IPC_CREAT)` | `mt_sch.c:550-556` **[verified]** |
| The table is cleared when the attacher is the only one (`shm_nattch == 1`) | `mt_sch.c:577-580` **[verified]** |
| `IPC_RMID` only when `nattch == 0` after a clean detach | `mt_sch.c:587-614` **[verified]** |
| An entry is reclaimed only when its hostname and user match and `kill(pid, 0) != 0` | `mt_sch.c:685-699` **[verified]** |
| `mtl_lcore_shm_clean(MTL_LCORE_CLEAN_PID_AUTO_CHECK)` counts dead entries but never clears `active`, and takes no lock | `mt_sch.c:1308-1333`, `:1367-1391` **[verified]** |
| Without the manager, `mtl_init` fails if the lockfile cannot be opened | `mt_sch.c:977-980`, `:646-672` **[verified]** |
| The documented way to share the table across containers: `ipc: host` plus bind-mounting the lockfile and `/dev/null` | `docker/README.md:121-134` **[verified]** |

Lifecycle:

- **Clean exit:** entries are released, the segment is detached and removed by the last user.
- **SIGKILL:** the flock goes with the fd. The segment and this process's `active` entries
  stay until the IPC namespace dies (pod deletion) or someone removes them **[verified]**.
- **Same-pod restart, single MTL process:** the new process is the only attacher, so it
  clears the table (`:577-580`). This is the common case and it works **[inferred]**.
- **Several MTL processes in one pod, or `hostIPC: true`:** stale entries survive. Their PIDs
  belong to another PID namespace, and the PID check breaks in both directions (§3.3).
- With `readOnlyRootFilesystem: true` and no writable `/tmp`, `mtl_init` without the manager
  fails **[inferred]**.

### 1.3 Threads, affinity and memory policy

| Thread | Created at | Affinity |
|---|---|---|
| EAL worker per scheduler (`dpdk-workerN`) | `rte_eal_remote_launch`, `mt_sch.c:278-285` | pinned to one CPU by EAL **[verified]** |
| Scheduler pthread with `MTL_FLAG_TASKLET_THREAD` | `mt_sch.c:286-287` | inherited; not registered with EAL (SF-49) **[verified]** |
| TSC calibration (1 s, then exits) | `mt_main.c:109-121`, `:200` | inherited **[verified]** |
| Admin (6 s period, session migration) | `mt_admin.c:379-388` | inherited **[verified]** |
| Stat dump | `mt_stat.c:164` and the stat thread | inherited **[verified]** |
| CNI thread (control packets when not run as a tasklet) | `mt_cni.c:397-415` | inherited **[verified]** |
| Kernel socket TX/RX threads | `datapath/mt_dp_socket.c:288`, `:627` | inherited **[verified]** |
| DPDK interrupt, alarm and telemetry control threads | DPDK | DPDK control cpuset **[inferred]** |

- `mtl_init` calls `numa_bind()` on the caller's thread when there is more than one NUMA node
  and `MTL_FLAG_NOT_BIND_PROCESS_NUMA` is not set (`mt_main.c:448-461`) **[verified]**. That
  rewrites the caller's CPU affinity to the NIC node's CPUs, intersected with the cpuset by
  the kernel, so it can widen an affinity the application had narrowed. It also sets
  `MPOL_BIND` for the thread and every thread it creates later **[inferred]**. EAL init
  happens before this, at `:407`, so EAL lcores are unaffected **[verified]**.
- Scheduler threads switch to `MPOL_LOCAL` while they run and restore it afterwards
  (`mt_sch.c:128-153`) **[verified]**.
- The inherited-affinity threads may run on the CPUs EAL pinned a busy-polling lcore to,
  because the pod's cpuset includes them **[inferred]**.

### 1.4 Files, sockets, environment, `/proc`, `/sys`, commands

| Item | Evidence |
|---|---|
| `KAHAWAI_CFG_PATH`, or else `kahawai.json` in the CWD, is parsed at init; it can name plugins that are `dlopen`ed | `mt_config.c:52-61`, `:9-30`; `st2110/pipeline/st_plugin.c:879` **[verified]** |
| `getenv` is used nowhere else in `lib/src` | grep **[verified]** |
| `/proc/<pid>/comm` and `/proc/stat` are read; for kernel ports also `/proc/net/route` and `/sys/class/net/<if>/device/numa_node` | `mt_util.c:1034`, `:1064`; `mt_socket.c:87`, `:176` **[verified]** |
| `/sys/class/net/<if>/queues/tx-N/tx_maxrate` is written (native AF_XDP on ice) | `dev/mt_af_xdp.c:90-101`, `:277-292` **[verified]** |
| `/sys/class/net/<if>/device/driver` is read | `dev/mt_af_xdp.c:255` **[verified]** |
| `mt_run_cmd` (`popen`) exists, but nothing in `lib/` calls it; no `system()` | `mt_util.c:807-829` **[verified]** |
| Manager client socket: connect to `/var/run/imtl/mtl_manager.sock` (compiled in) | `mt_instance.c:192-202`; `manager/mtl_mproto.h:16` **[verified]** |
| `geteuid() == 0` sets `impl->privileged` | `mt_main.c:476-481` **[verified]** |

### 1.5 Capabilities and devices actually needed

| Need | For | Evidence |
|---|---|---|
| `/dev/vfio/*`, `RLIMIT_MEMLOCK`/`IPC_LOCK` | DPDK PMD (vfio type1 pins pages) | `docker/docker-compose.yml` (devices, memlock, `IPC_LOCK`, `SYS_NICE`) **[verified]** |
| `CAP_SYS_ADMIN` (pagemap) | IOVA PA mode | **[inferred]** (DPDK) |
| `CAP_SYS_TIME` | `MTL_FLAG_PHC2SYS_ENABLE` | `mt_ptp.c:198-203` **[verified]** |
| `CAP_NET_ADMIN` + `/dev/vhost-net` | `MTL_FLAG_VIRTIO_USER` | `dev/mt_dev.c:1555`, `:2454` **[verified]** |
| `CAP_NET_RAW`, `CAP_BPF`, host network | AF_XDP | `docker/docker-compose.xdp.yml` **[verified]**; `dev/mt_af_xdp.c:436` **[verified]** |
| A writable `/sys` | AF_XDP `tx_maxrate` (pods mount `/sys` read-only unless privileged) | **[inferred]** |
| Root or the manager | MtlManager itself: `--privileged --net=host`, `/sys/fs/bpf` | `manager/README.md:78-84` **[verified]** |

## 2. Inventory per backend

### 2.1 DPDK PMD over vfio (VF on iavf, or PF on ice)

| Resource | Created at | Clean uninit | SIGKILL/OOM, then the next process or pod |
|---|---|---|---|
| Ports, queues, descriptor rings | `dev/mt_dev.c:1156-1300` | stop `:775-793`, close `:795-813`, `iavf_dev_close` flushes flows and sends a VF reset (DPDK `iavf_ethdev.c:3215-3223`) **[verified]** | Not run. vfio-pci resets the function on release and on the next open **[inferred]** |
| Mempools (hugepages) | `dev/mt_dev.c:1336-1410`, `:2470-2495` | freed `:2178-2220` **[verified]** | freed with the process **[inferred]** |
| rte_flow rules (RX steering) | `mt_flow.c:62`, `:182`, `:205` | destroyed `mt_flow.c:285` **[verified]** | on a VF they live in the PF (virtchnl FDIR) until VF reset **[inferred]** |
| RL/TM hierarchy and shapers | `dev/mt_dev.c:566-772` | not torn down explicitly; port close **[verified]** | VF: kept in ice `vf->qs_bw` and the HW scheduler until VF reset (`patches/ice_drv/2.6.7/0001-*.patch`, "HW inconsistent until VF reset") **[verified]**; PF: in HW until PMD re-init **[inferred]** |
| Multicast MAC filters (`set_mc_addr_list` or `mac_addr_add`), including `01:00:5e:00:00:01` | `mt_mcast.c:410-451`, `:484-485` | removed `mt_mcast.c:537-538` **[verified]** | VF: PF switch filters until VF reset **[inferred]** |
| Promiscuous mode (opt-in, `MTL_FLAG_NIC_RX_PROMISCUOUS`) | `dev/mt_dev.c:1292-1296` | not disabled; port close **[verified]** | VF trust/promisc state is in the PF until reset **[inferred]** |
| IGMP membership (MTL sends v3 reports) | join `mt_mcast.c:561-665`; reports every 10 s `:342-361`, `mt_mcast.h:14` | session destroy sends leave `mt_mcast.c:735-743`; `mt_mcast_uinit` frees the list without leaves `:527-559` **[verified]** | No leave: the switch keeps forwarding to the VF's MAC (maybe now another pod's) until its timer expires, typically about 260 s **[inferred]** |
| ARP cache | in-process; `mt_arp.c` | gone | Peers keep IP→old MAC until their neighbour timeout. No gratuitous ARP at start (grep) **[verified]** |
| PHC adjustment (PTP client, PF only) | `rte_eth_timesync_adjust_time/freq` `mt_ptp.c:358-390`; timesync enabled only for `MT_PORT_PF` `dev/mt_dev.c:1992-2030` | not restored **[verified]** | frequency offset stays in the NIC and affects every user of that PHC (ptp4l, other VFs) until re-steered **[inferred]** |
| CLOCK_REALTIME steering (`MTL_FLAG_PHC2SYS_ENABLE`) | `clock_adjtime` with `ADJ_SETOFFSET`, `ADJ_FREQUENCY`, `ADJ_TICK`, `mt_ptp.c:163-240`, `:1308` | not restored **[verified]** | node-wide clock (not namespaced); last frequency and tick persist after death **[inferred]** |
| DMA devices (dmadev via `-a`) | `mt_dma.c:219-255` | `rte_dma_stop` `:264` **[verified]** | vfio reset **[inferred]**; same IOMMU caveat (§7) |
| virtio_user exception path (tap via `/dev/vhost-net`), with IP set and link up | `dev/mt_dev.c:1539-1594` | `mt_dev_if_pre_uinit` stop/close `:2557-2580` **[verified]** | non-persistent tap vanishes with its fd **[inferred]** |
| `MTL_PMD_DPDK_AF_XDP`/`AF_PACKET` vdevs | `dev/mt_dev.c:361-389` | PMD close | the DPDK af_xdp PMD may leave its XDP program attached after SIGKILL **[inferred]** |

`mt_tap.c` is Windows-only (`meson.build:25-28`) **[verified]**; on Linux the exception path
is virtio_user or the CNI tasklet/thread.

### 2.2 Native AF_XDP (`MTL_PMD_NATIVE_AF_XDP`)

- **Requires MtlManager.** `mt_dev_xdp_init` refuses to start without it
  (`dev/mt_af_xdp.c:727-733`) **[verified]**. The library never loads XDP itself: it creates
  sockets with `XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD` (`:417-419`) and gets the XSK map fd
  from the manager over `SCM_RIGHTS` (`:384-403`; `mt_instance.c:58-112`) **[verified]**.
- **Queues** are granted by the manager, one request per queue (`:759-765`), and returned
  in `xdp_free` (`:237`) **[verified]**.
- **UMEM** is the RX mempool's hugepage memory (`:294-333`). The kernel unpins it, and drops
  the XSK map entry, when the socket fd closes, so SIGKILL is clean here **[inferred]**.
- **`tx_maxrate`:** a probe write to `tx-1` at init (`:277-292`); the rate is set on TX queue
  get (`:867-874`) and cleared to 0 only on put (`:889-892`) **[verified]**. After SIGKILL the
  rate stays on that kernel queue for the next user, MTL or not **[inferred]**.
- **ethtool ntuple rules and UDP filters** go through the manager (`mt_socket.c:353-422`)
  **[verified]**; see §4 for cleanup.

### 2.3 Kernel socket (`MTL_PMD_KERNEL_SOCKET`)

- UDP sockets per TX/RX entry, `SO_BINDTODEVICE`, `SO_REUSEPORT`, then `SO_RCVBUFFORCE`
  falling back to `SO_RCVBUF` (`datapath/mt_dp_socket.c:212-231`, `:455-512`) **[verified]**.
- Multicast is joined per socket with `IP_ADD_MEMBERSHIP` or `IP_ADD_SOURCE_MEMBERSHIP`
  (`mt_socket.c:424-450`), so the kernel drops the membership, and sends the IGMP leave, when
  the fd closes, including on SIGKILL **[inferred]**. This is the only backend whose IGMP
  state is correct after SIGKILL.
- Threads are joined on clean free (`mt_dp_socket.c:391-411`) **[verified]**.
- **Hang:** in `mt_socket_get_mac`, a failing `sendto` does `continue` before the abort and
  timeout checks (`mt_socket.c:323-328`). With no route, for example, it busy-spins forever,
  and `mtl_abort` cannot stop it **[verified]**.

## 3. Lcores, cpusets and namespaces

### 3.1 How lcores are chosen

1. **No `lcores` parameter** (`include/mtl_api.h:630`): there is no `-l`, so EAL takes the
   calling thread's affinity (DPDK `eal_common_options.c:2155-2161`). The calling thread is
   MTL's EAL thread, which inherits from the `mtl_init` caller, so the pod's cgroup cpuset is
   respected **[verified]**.
2. **`lcores` given:** MTL builds `-l "<main_lcore>,<lcores>"` with `main_lcore` defaulting to
   0 (`dev/mt_dev.c:416-441`; `include/mtl_api.h:737`) **[verified]**. A worker CPU outside
   the cpuset aborts the process (`rte_panic("Cannot set affinity")`, DPDK
   `lib/eal/linux/eal.c:878-881`); a main CPU outside it fails init (`:838-843`)
   **[verified]**. When `lcores` is set but `main_lcore` is not, CPU 0 is put in the list,
   and CPU 0 is rarely in an exclusive pod cpuset, so init fails **[inferred]**. MTL never
   checks `sched_getaffinity` itself (grep) **[verified]**.
3. **`--remap-lcore-ids`** is always passed with DPDK 25.11 or later (`dev/mt_dev.c:443-446`),
   so lcore IDs are 0..N-1, not CPU IDs (DPDK `eal_common_options.c:932-980`; the comment at
   `mt_main.c:736`) **[verified]**.
4. **Scheduler start** walks `rte_get_next_lcore(.., skip_main=1, ..)` and asks the manager
   or the shm table for each ID (`mt_sch.c:701-791`) **[verified]**. The main lcore is never
   given to a scheduler, so one pod CPU only hosts the EAL init thread, which has exited, and
   control threads **[inferred]**.
5. The NUMA match falls back to other nodes only with the "across NUMA" flag (`:780-785`)
   **[verified]**.

### 3.2 Identity: lcore ID, not CPU

Both allocators are keyed by **lcore ID**: the manager's `std::bitset<128>`
(`manager/mtl_lcore.hpp:11-55`) and the shm table (`mt_sch.c:744-760`) **[verified]**. With
remapping, two pods with disjoint cpusets both have lcore 1:

- under one manager the second pod is refused lcore 1 and may run out (false exhaustion);
- two pods sharing CPUs under different IDs can double-book a CPU;
- the manager also rejects any ID of 128 or more (`:38`).

All of this is **[inferred]** from the verified keying. With the Kubernetes static CPU
manager (exclusive cpusets) cross-pod arbitration is unnecessary; with the shared pool it is
needed, and it would have to be by CPU.

### 3.3 `kill(pid, 0)`, pid, uid and hostname across namespaces

| Use | Where | Namespace behaviour |
|---|---|---|
| `getpid()` stored in shm entries | `mt_util.c:1016`; `mt_sch.c:760` | PID namespace-local **[verified]** |
| `kill(pid, 0) != 0` means dead | `mt_sch.c:692`, `:1325` | Another container's PID gives `ESRCH`, so a live sibling's lcores are stolen. A restarted container's new process often reuses the PID (PID 1), so stale entries are never freed. `EPERM` also counts as dead **[inferred]** |
| `gethostname` must match | `mt_util.c:1024`; `mt_sch.c:690` | With `hostIPC`, entries from another pod (different hostname) are never cleaned **[inferred]** |
| `getpwuid(getuid())->pw_name` must match | `mt_util.c:1029-1031` | It is "unknow" in images without the uid in `/etc/passwd`; uid in a user namespace is local **[verified]**/**[inferred]** |
| pid, uid and hostname sent to the manager | `mt_instance.c:212-217` | used only for logging; liveness is the socket close (`manager/mtl_manager.cpp:192-201`), which works across namespaces **[verified]** |
| ifindex sent to the manager | `mt_instance.c:228`; `mt_socket.c:397-421`; `dev/mt_af_xdp.c:390`, `:742` | network namespace-local, resolved by the manager in its own namespace (§4) **[verified]** |
| `ftok("/dev/null")` | `mt_sch.c:550` | key from the container's `/dev/null` inode, so it may differ per container unless `/dev/null` is bind-mounted (`docker/README.md:131-134`) **[inferred]** |
| `/tmp/kahawai_lcore.lock` | `mt_platform.h:57` | per container rootfs, so containers in one pod do not serialise on it **[inferred]** |

## 4. MtlManager

**Socket and protocol**

- Path `/var/run/imtl/mtl_manager.sock`, compiled in, no override (`manager/mtl_mproto.h:16`)
  **[verified]**.
- The manager unlinks any existing socket before `bind` (`mtl_manager.cpp:85`), so a second
  manager silently takes over the path from a running one **[verified]**.
- Mode `0777` (`:96`) **[verified]**.
- Trust is SF-47: identity comes from the message, and there is no `SO_PEERCRED`
  (`manager/mtl_instance.hpp:191-213`) **[verified]**.
- The library's `send`, `recv` and `recvmsg` have no timeout and no `MSG_NOSIGNAL`
  (`mt_instance.c:17`, `:24`, `:69`, `:91`) **[verified]**. A manager that is wedged
  therefore blocks `mtl_init` and every lcore, queue or flow request forever, and a manager
  that is gone raises SIGPIPE in the application **[inferred]**. PR #1770 is not in this tree.
- `mt_instance_init`'s failure is ignored (`mt_main.c:484`), so a missing manager silently
  selects the shm allocator **[verified]**.

**When a client dies:** the kernel closes its socket, and `recv == 0` erases the
`mtl_instance` (`mtl_manager.cpp:192-201`). Its destructor returns lcores, queues and ethtool
flows (`mtl_instance.hpp:66-89`) **[verified]**. It does not return UDP filter references,
which `handle_message_udp_dp_filter` never records per client (`:272-287`;
`mtl_interface.hpp:114-122`) **[verified]**. The port stays in the BPF filter map, and the
refcount stays skewed for later clients **[inferred]**.

**Interface lifetime:**

- The first client naming an ifindex creates an `mtl_interface`, which calls
  `clear_flow_rules()` (`mtl_interface.hpp:73-91`). That deletes **every** ntuple rule on
  the interface, including ones it did not create (`:168-233`) **[verified]**.
- It then attaches `mtl.xdp.o` and the libxdp XSK program (`:412-450`) **[verified]**.
- The interface is destroyed, with XDP detached and all rules cleared again, when the last
  client using it goes (`:93-100`; weak pointer at `:460`) **[verified]**.
- `xdp_mode` is overwritten to `NATIVE` after an SKB-mode fallback (`:420-430`), so detach
  uses the wrong mode and an SKB-attached program can stay **[verified]** (effect
  **[inferred]**).

**When the manager itself stops or restarts:**

- Only SIGINT is handled (`mtl_manager.cpp:57-61`). The image has no `STOPSIGNAL`
  (`manager/Dockerfile:61-64`) and the README stops it with `docker kill -s SIGINT`
  (`manager/README.md:92-95`) **[verified]**.
- Kubernetes sends SIGTERM, which kills it without running any destructor. XDP programs, pinned
  libxdp state and ethtool rules stay on the interfaces **[inferred]**.
- SF-48 (SIGPIPE) can also kill it when a pod dies mid-request **[inferred]**.
- A restarted manager has an empty lcore bitset and queue map. Clients never reconnect: there
  is no reconnect code and `instance_fd` stays the dead fd (`mt_main.h:1268-1270`)
  **[verified]**. It can grant lcores and queues that live clients hold, its `clear_flow_rules`
  wipes their steering, and it builds a new XSK map their sockets are not in, so their RX stops
  **[inferred]**.

**As a DaemonSet serving pods:**

- The socket must be shared through a hostPath. The manager needs host network,
  `/sys/fs/bpf` and privilege (`manager/README.md:78-84`) **[verified]**.
- Clients must share the manager's network namespace, because ifindexes are interpreted in
  the manager's namespace (`mtl_interface.hpp:171`, `:238`, `:272`, `:379`, `:420`)
  **[verified]**. The repository's own AF_XDP compose file uses `network_mode: host`
  (`docker/docker-compose.xdp.yml`) **[verified]**.
- A pod whose VF netdev was moved into its namespace by SR-IOV CNI sends an ifindex that names
  a different host interface, or none. The manager then attaches XDP to the wrong interface,
  or wipes its rules **[inferred]**.
- Lcore IDs are per-pod remapped IDs, not host CPUs (§3.2), and the manager knows nothing of
  pod cpusets **[inferred]**.
- `MAX_CLIENTS = 10` is only the listen backlog (`mtl_manager.cpp:22`, `:98`) **[verified]**.

**Conclusion:** today the manager is safe only as a per-node host daemon for host-network
processes. It is not a multi-tenant DaemonSet.

## 5. Shutdown and startup

### 5.1 `mtl_uninit` order

`_mt_stop` → `mt_main_free` → `mt_dev_if_uinit` → `mt_stat_uinit` → `mt_instance_uinit` →
`rte_eal_cleanup` (`mt_main.c:628-664`) **[verified]**:

- `_mt_stop` (`mt_dev_stop` → `mt_sch_stop_all`) skips application schedulers
  (`mt_sch.c:1224-1244`) **[verified]**.
- `mt_main_free` covers PTP, DHCP, config, plugins, admin, CNI, ARP, mcast, map, DMA,
  queues and `mt_dev_free` (`:210-234`) **[verified]**.
- `mt_sch_mrg_uinit` releases the shm lcores (`sch_uinit_lcores`) before it frees still-active
  schedulers (`mt_sch.c:1022-1030`). An application scheduler's lcore is marked free while it
  still polls **[verified]** (effect **[inferred]**).

### 5.2 Unbounded or long waits on the stop path

| Wait | Bound | Evidence |
|---|---|---|
| `sch_stop` waits for the loop to set `stopped` | **none** (10 ms sleeps forever) | `mt_sch.c:316-319` **[verified]** |
| `rte_eal_wait_lcore` | **none** | `mt_sch.c:321` **[verified]** |
| `mt_handle_drain` spins while the API refcount is above 0 | **none** | `mt_handle_guard.h:130` **[verified]** |
| `flock(LOCK_EX)` on the lcore file | **none** | `mt_sch.c:517` **[verified]** |
| Manager `recv` | **none** | `mt_instance.c:24` **[verified]** |
| `mtl_sch_unregister_tasklet` | 1 s, then `-EIO` with the tasklet still registered | `mt_sch.c:885-901` **[verified]** |
| st20p TX free: per frame up to 100 × 10 ms, or 50 ms if transmitting | about 1 s × frames × sessions | `st2110/pipeline/st20_pipeline_tx.c:722-753` **[verified]** |
| TX pad flush | 2 × burst × 1 ms per queue | `dev/mt_dev.c:1782-1799` **[verified]** |

Consequences:

- The callers of `mtl_sch_unregister_tasklet` ignore its return, null their pointer and free
  the session (for example `st2110/st_tx_video_session.c:3902-3914`). A tasklet that comes
  back after the 1 s timeout runs on freed memory **[verified]** (UAF **[inferred]**).
- A tasklet that never returns hangs `mtl_stop` and `mtl_uninit` forever, so SIGTERM becomes
  SIGKILL after the grace period, with every SIGKILL residue of §2 **[inferred]**.
- With 16 st20p TX sessions × 3 frames held by the application, the frame flush alone takes
  about 48 s, past the 30 s default grace period **[inferred]**.
- Sessions do not stop transmitting before teardown. There is no global "quiesce TX, then
  leave groups" step that could run within a few seconds **[verified]** by `mtl_uninit` order.

### 5.3 SIGTERM in practice

- The library has no handler.
- RxTxApp and the samples catch only SIGINT (`tests/tools/RxTxApp/src/rxtx_app.c:350-359`,
  `:462`; `app/sample/sample_util.c:478-509`) **[verified]**, so SIGTERM kills them by
  default action.
- A container's PID 1 ignores signals it has no handler for, so an MTL application run as
  PID 1 without an init ignores SIGTERM and always waits for SIGKILL. Compose sets
  `init: true` (`docker/docker-compose.yml`) **[verified]**; the PID 1 rule is
  **[inferred]** from pid_namespaces(7).
- `mtl_abort` is only an atomic store (`mt_main.c:747-757`), so it is async-signal-safe and
  interrupts the ARP, PTP-stable and kernel-ARP waits (except the `continue` bug, §2.3)
  **[verified]**.

### 5.4 Time to ready (for startup probes)

| Phase | Worst case | Evidence |
|---|---|---|
| EAL init, vfio probe, VF reset | seconds | **[inferred]** |
| PF timesync start | 100 × 10 ms = 1 s | `dev/mt_dev.c:852-893` **[verified]** |
| Link wait, strict mode | 300 × 100 ms = **30 s**, then failure | `dev/mt_dev.c:815-850`, `:2007-2016` **[verified]** |
| Link wait, relaxed (`ALLOW_DOWN`) | 3 s | `dev/mt_dev.h:37-38` **[verified]** |
| DHCP | 49 × 100 ms ≈ 5 s, then `-ETIME` | `mt_dhcp.c:549-564` **[verified]** |
| TSC calibration | 1 s, joined in `mtl_start` | `mt_main.c:109-121`, `:351` **[verified]** |
| Unicast ARP per TX session (or RX with RTCP) | `arp_timeout_s`, default **60 s** | `mt_main.c:573-576`; `mt_arp.c:171-203` **[verified]** |
| First RL TX video session: PTP stable plus pad training | up to **180 s** plus training | `st2110/st_tx_video_session.c:374-376` **[verified]** |

The header comment claims 3 × 300 × 100 ms = 90 s in strict mode (`dev/mt_dev.h:14-25`). The
code exits on the first failed poll round, so the real bound is 30 s **[verified]**.

## 6. Health: what a liveness probe could read

| Signal | Where | Usable? |
|---|---|---|
| `loop_cnt` | local variable in `sch_tasklet_func` (`mt_sch.c:162`, `:215`) | not visible **[verified]** |
| `avg_ns_per_loop` | updated every 2 s, only while the loop runs (`mt_sch.c:211-216`) | a stuck loop keeps its last good value; it is not a heartbeat **[verified]** |
| `stat_time` per scheduler and tasklet | only with `MTL_FLAG_TASKLET_TIME_MEASURE` or USDT (`mt_sch.c:120-124`, `:203-226`) | read **and reset** by the stat thread without a lock (`mt_sch.c:462-484`) **[verified]**, so a second reader races and sees torn min/max **[inferred]** |
| `stopped` and `started` atomics | `mt_sch.c:239`, `:295` | lifecycle only **[verified]** |
| `mtl_get_var_info` | `mt_main.c:1019-1035` | counts only **[verified]** |
| `mtl_get_port_stats` | `dev/mt_dev.c:2635-2652`, under `stats_lock` (`:181-215`) | thread-safe; packet counters that rise show the dataplane moves. On iavf each call is a PF mailbox round-trip **[verified]**/**[inferred]** |
| Per-session stats | log dump only | **[verified]** by the absence of a public getter for scheduler state |

No public state tells "scheduler N has not completed a loop for X ms". The missing piece is
a per-scheduler monotonic `loop_seq` and `last_loop_tsc`, written with a relaxed store by the
scheduler and read lock-free.

## 7. IOMMU, IOVA and orphaned DMA

- MTL never checks for vfio no-IOMMU mode: no hit for `noiommu`, `rte_vfio` or
  `enable_unsafe_noiommu_mode` in `lib/src` **[verified]**. DPDK supports it
  (`lib/eal/linux/eal_vfio.c:65-94`, `:377-389`) **[verified]**.
- IOVA is left to DPDK unless the user forces it (`dev/mt_dev.c:453-461`) **[verified]**.
- In PA mode MTL copies packets that cross pages (`st2110/st_tx_video_session.c:2923`) and
  refuses `mtl_dma_map`, `mtl_dma_unmap` and `mtl_udma_create` (`mt_main.c:840`, `:901`,
  `:1146`) **[verified]**.

| Mode | After SIGKILL |
|---|---|
| vfio type1 with IOMMU, IOVA=VA or IOVA=PA | Pages stay pinned by the vfio container until it is torn down. Device release clears bus mastering and resets the function. In-flight DMA lands in still-pinned pages or faults in the IOMMU; nothing reaches memory that was handed to someone else **[inferred]** |
| vfio no-IOMMU (IOVA=PA) | Nothing is pinned. On exit the mm, and hugepage fds by number, can be released before the vfio device fd. The VF's RX rings still hold physical addresses, so the NIC can DMA received packets into hugepages already back in the node pool and reallocated to another pod. If the function cannot be reset, this lasts until some reset **[inferred]** |
| AF_XDP | UMEM pinned by the kernel until the socket is unbound from the queue **[inferred]** |
| Kernel socket | No device DMA into user memory |

## 8. Hazards

Severity in a pod: **Critical** = corrupts or disturbs another tenant or the node; **High** =
the pod or its peers stay broken until someone acts, or shutdown hangs or crashes; **Medium**
= degraded or bounded in time; **Low** = cosmetic or needs an unusual setup.

| ID | Hazard | Severity | Current behaviour | What the API/engine must guarantee |
|---|---|---|---|---|
| H-K-1 | Orphaned DMA in vfio no-IOMMU mode | Critical | not detected; PA DMA into freed hugepages after SIGKILL (§7) | Detect no-IOMMU and refuse to open unless explicitly opted in; report the IOMMU mode in instance info |
| H-K-2 | phc2sys steers the node's CLOCK_REALTIME and leaves frequency and tick set on death | Critical | `mt_ptp.c:163-240` | Never touch a node clock from a pod by default; document it as a host-daemon role; on stop, restore the frequency to its value at start |
| H-K-3 | The PTP client adjusts a shared PF PHC and leaves the frequency offset | High | `mt_ptp.c:358-390`; PF only | Do not discipline a PHC unless granted ownership; restore on stop; VF and pod mode use time read-only |
| H-K-4 | DaemonSet manager resolves pod ifindexes in its own namespace | Critical (DaemonSet) | wrong interface gets XDP and has its rules wiped (§4) | Identify ports by PCI BDF or netns plus name, never by bare ifindex; refuse cross-namespace requests |
| H-K-5 | Manager restart loses all grants and wipes live clients' rules and XSK map | High | no reconnect; `clear_flow_rules` on first use | Clients re-register and re-announce what they hold; the manager persists state or rebuilds it from clients; never delete rules it did not create |
| H-K-6 | Manager dies on SIGTERM or SIGPIPE without cleanup | High | only SIGINT handled; no `MSG_NOSIGNAL` | Handle SIGTERM like SIGINT; ignore SIGPIPE; detach XDP and delete own rules on exit |
| H-K-7 | Manager socket world-writable, identity self-reported | High | SF-47 | `SO_PEERCRED`, socket `0660` with a group, ownership per client |
| H-K-8 | Library raises SIGPIPE, or blocks forever, on manager loss or a wedged manager | High | `mt_instance.c:17-31` | `MSG_NOSIGNAL`; per-request timeout; a manager-lost event; defined fallback or fail |
| H-K-9 | Lcore allocators keyed by remapped lcore ID, not CPU | High (shared manager), Low (static CPU manager) | `mtl_lcore.hpp`; `mt_sch.c:744-760` | Allocate CPUs by host CPU ID inside the process's cpuset, or skip arbitration when the cpuset is exclusive |
| H-K-10 | SysV lcore table is wrong across PID, IPC and UTS namespaces; the "clean" tool does nothing | Medium | `kill(pid, 0)`, hostname check, per-container lock; `mt_sch.c:1308-1333` | Liveness by fd or flock owned per lcore (released by the kernel), not by PID; or no cross-process table at all in pod mode |
| H-K-11 | `lcores` without `main_lcore` puts CPU 0 in `-l`; a CPU outside the cpuset aborts in EAL | Medium (deterministic crash-loop) | `dev/mt_dev.c:430-441`; DPDK `eal.c:838-843`, `:878-881` | Validate the requested CPUs against `sched_getaffinity` before EAL; default to the cpuset; never `rte_panic` on configuration |
| H-K-12 | `numa_bind` rewrites the caller's affinity and memory policy | Low | `mt_main.c:448-461` | Do not change the caller's threads; set policy only on MTL-owned threads |
| H-K-13 | Unbounded stop waits on a stuck tasklet, the API refcount, the manager or flock | High | §5.2 | Every stop wait bounded, with the total under a caller-given deadline; on overrun, report and leak rather than hang |
| H-K-14 | Tasklet unregister timeout followed by a free gives a use-after-free | High | `mt_sch.c:885-901` plus callers | On timeout, never free memory a registered tasklet can reach; quarantine it and mark the instance faulted |
| H-K-15 | Bounded but additive teardown (frame flush, pad flush) exceeds the grace period | Medium | about 1 s per held frame | A stop deadline parameter; a quiesce-all-TX step first; drop held frames after the deadline |
| H-K-16 | No SIGTERM path: the library and apps catch only SIGINT; PID 1 ignores SIGTERM | Medium | §5.3 | A documented signal-safe "request stop" (`mtl_abort` qualifies) and a sample handler for SIGTERM; recommend an init in the image |
| H-K-17 | IGMP memberships not left and no gratuitous ARP after SIGKILL or at start | Medium | `mt_mcast.c:527-559`; no GARP | Send leaves on stop before the queues close; send a GARP and an IGMP report at start; document the switch timeout window |
| H-K-18 | VF state in the PF (RL `qs_bw`, filters, FDIR, promisc) survives until VF reset | Medium | depends on vfio FLR (§2.1) | Reset the VF at open (`iavf` reset) and verify a clean state; report reset failure |
| H-K-19 | AF_XDP `tx_maxrate` left on the queue; UDP filter refcount leak in the manager | Medium | `dev/mt_af_xdp.c:867-892`; `mtl_instance.hpp:272-287` | Reset the queue rate on queue grant (manager-side); track filters per client and drop them on disconnect |
| H-K-20 | Startup can block up to 30 s (link), 60 s (ARP) or 180 s (PTP); `mtl_init` cannot be retried in-process | Medium | §5.4; `dev/mt_dev.c:500-503` | Non-blocking open with readiness events; startup states visible to a probe; init retryable without exiting |
| H-K-21 | No heartbeat for liveness; the stats reader resets and races | Medium | §6 | A per-scheduler `loop_seq` and `last_loop_tsc` readable lock-free; a stall event with a threshold |
| H-K-22 | Kernel-socket ARP spins forever when `sendto` fails | High | `mt_socket.c:323-328` | All waits honour abort and timeout |
| H-K-23 | `kahawai.json` in the CWD can `dlopen` plugins | Medium (security) | `mt_config.c:52-61` | No implicit config file; plugins only by explicit API or path option |
| H-K-24 | Two managers split-brain: the new one unlinks the live socket | Medium | `mtl_manager.cpp:85` | Single-instance lock (flock on a pid file) before unlink |
| H-K-25 | SKB-mode XDP never detached because the mode is overwritten | Medium | `mtl_interface.hpp:420-430` | Record the mode actually attached |
| H-K-26 | `/tmp` lock required without the manager; read-only rootfs breaks `mtl_init` | Low | `mt_sch.c:505-514` | No filesystem state in pod mode, or a configurable runtime directory |
| H-K-27 | DPDK telemetry socket on by default, shared `MT_DPDK` prefix | Low | `dev/mt_dev.c:336-345` | Pass `--no-telemetry` unless asked; prefix per instance |
| H-K-28 | Busy-polling lcores under a CFS quota or the shared CPU pool | High (deployment) | not detected **[inferred]** | Detect a CFS quota or non-exclusive CPUs at open and warn or refuse in lcore mode; report it in instance info |

## 9. Engine fixes needed regardless of the API

1. **Bound every stop-path wait.** `sch_stop` (`mt_sch.c:316-321`), `mt_handle_drain`
   (`mt_handle_guard.h:130`), the manager `recv` (`mt_instance.c:24`) and the lcore `flock`
   (`mt_sch.c:517`). On timeout, mark the instance faulted and skip freeing memory that a
   stuck thread can reach (fixes H-K-13 and H-K-14).
2. **Fix the unregister timeout.** It must not be followed by a session free, so propagate
   `-EIO` and quarantine (`mt_sch.c:885-901` and the callers listed in §5.2).
3. **Kernel ARP loop:** move the abort and timeout checks before the `continue`
   (`mt_socket.c:323-328`).
4. **Library manager I/O:** `MSG_NOSIGNAL`, `SO_RCVTIMEO`, handle a short read, and detect a
   lost manager (`mt_instance.c`).
5. **MtlManager:**
   - handle SIGTERM and ignore SIGPIPE;
   - add `MSG_NOSIGNAL` to `send` and `sendmsg`;
   - use `SO_PEERCRED` and socket mode `0660`;
   - take a single-instance lock before `unlink`;
   - delete only the rules it created, not all of them (`mtl_interface.hpp:75`, `:97`);
   - track UDP filters per client;
   - record the XDP mode actually attached;
   - reset `tx_maxrate` on queue grant.
6. **`mtl_lcore_shm_clean(PID_AUTO_CHECK)`** should clear the entries it reports, and do so
   under the file lock (`mt_sch.c:1308-1333`).
7. **Validate CPUs against `sched_getaffinity`** before building `-l`. Do not inject
   `main_lcore = 0` (`dev/mt_dev.c:430-441`).
8. **Key lcore arbitration by CPU ID** (`rte_lcore_cpuset`), not by remapped lcore ID
   (manager protocol and shm table).
9. **Detect vfio no-IOMMU** and require an explicit opt-in; log the IOMMU and IOVA mode at
   init.
10. **Leave IGMP groups explicitly in `mt_mcast_uinit`**, before the ports close
    (`mt_mcast.c:527-559`). Send a gratuitous ARP once the port is up.
11. **Restore PHC and system-clock frequency** on PTP and phc2sys uninit. Gate phc2sys
    behind an explicit host-role option.
12. **Add a per-scheduler heartbeat** (`loop_seq`, `last_loop_tsc`) and stop the stat thread
    resetting counters that other readers use (`mt_sch.c:462-484`).
13. **Pass `--no-telemetry`** by default; correct the 90 s comment in `dev/mt_dev.h:14-25`.
14. **Do not `numa_bind` the caller's thread** (`mt_main.c:448-461`); apply policy only to
    MTL-owned threads.
