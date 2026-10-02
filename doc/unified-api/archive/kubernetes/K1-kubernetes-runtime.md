# K1: How Kubernetes runs a DPDK media process, and what MTL's API needs to cope with it

| | |
|---|---|
| Status | Research input for the unified-API design (revision 4), 2026-10-01. Nothing here is implemented |
| Question | The owner asked: "design API in a way that the library can safely work in kubernetes pod". This document collects the facts; §10 turns them into requirements K-REQ-1…20 |
| Baseline | MTL `main` @ `545a266a`; design: [REVISION-4.md](../REVISION-4.md), [15 §8](../15-security-and-deployment.md#8-deployment), [mtl.h](../../sketch/include/mtl/experimental/mtl.h) |
| Method | Web fetches of Kubernetes docs (from the `kubernetes/website` sources), project READMEs and source files (sriov-cni, device plugin, operator, DPDK, Linux, linuxptp, libxdp), vendor guides, and MTL's own code |
| Legend | **[verified]**: I read the cited page or source line on 2026-10-01. **[inferred]**: from knowledge or deduction, not re-checked for this report; treat it as a hypothesis to test |

## 0. Top findings

1. **The shutdown budget is shared and can be zero.** `terminationGracePeriodSeconds` (default 30 s) covers the `preStop` hook *and* the SIGTERM handling; when it runs out, SIGKILL follows. Hard node-pressure eviction uses a **0 s** grace period. So MTL needs a deadline-bounded orderly stop, and correctness can never depend on that stop running ([§1.1](#11-termination)).
2. **SIGKILL is mostly clean, but some things survive it.** The kernel resets the VF when the vfio fd closes, and frees hugepages, XSK sockets and the MtlManager connection.
   What survives: IGMP state in the switch (about 260 s of stale flooding), MTL's SysV-shm lcore table (it lives in the pod's IPC namespace), MtlManager's UDP filter refcounts, and XDP programs attached by netlink ([§1.4](#14-what-survives-what), [§7](#7-multicast-igmp-and-af_xdp)).
3. **A container restart is not a fresh pod.** The IPC namespace, network namespace, emptyDir volumes and the allocated VF all persist. The PID namespace and the root filesystem are new, so MTL's `kill(pid, 0)` check on stale lcore entries can hit a reused PID ([§1.4](#14-what-survives-what)).
4. **The pod's CPUs are its affinity mask.** CPU Manager gives a Guaranteed pod exclusive CPUs through its cpuset. DPDK already derives lcores from `sched_getaffinity` when no `-l` is given. MtlManager and explicit `lcores` strings use host-global CPU IDs that the pod may not own ([§4](#4-cpu)).
5. **A VF in a pod is a constrained device.** sriov-cni sets the admin MAC, VLAN, spoofchk, trust and rate from the network attachment and restores them on DEL. An untrusted E810 VF cannot enable promiscuous or allmulti mode and is limited to 18 MAC filters, unicast and multicast together. An RX pod that joins many groups hits that limit ([§2](#2-sr-iov-in-kubernetes)).
6. **Time comes from the host.** A VF's PHC is readable through virtchnl but cannot be adjusted. `CLOCK_TAI` is not namespaced, and its offset is set by the host's `phc2sys` only when the grandmaster's UTC offset is traceable. The reference ST 2110 platform on OpenShift (NVIDIA Holoscan for Media) runs PTP as a node service ([§5](#5-time)).
7. **Today's MTL Kubernetes manifests run privileged.** Media Communications Mesh's MTL-based DaemonSets use `privileged: true`, root, a hostPath `/dev/vfio` and a hard-coded VF address. The minimum for a vfio VF is far smaller: the device nodes from the device plugin, `IPC_LOCK` and hugepages ([§6](#6-capabilities-and-security), [§8](#8-how-others-deploy)).

## What MTL needs from the node (for orientation)

From [15 §8](../15-security-and-deployment.md#8-deployment), [mtl.h](../../sketch/include/mtl/experimental/mtl.h) and the code:

- **Network ports.** One VF per port (`0000:af:01.0`, iavf PMD over vfio-pci), `native_af_xdp:<if>` or `kernel:<if>`. The DPDK EAL runs with `--in-memory --file-prefix MTL --match-allocations`, plus `--remap-lcore-ids` on DPDK 25.11 or later (`lib/src/dev/mt_dev.c:336-345`, `:442-445`) [verified].
- **Memory.** Hugepages, pinned and mapped into the IOMMU; `RLIMIT_MEMLOCK` or `CAP_IPC_LOCK`.
- **CPUs.** Scheduler lcores from the `lcores` string, from MtlManager, or from the SysV-shm lcore table (`ftok("/dev/null", 21)`, `mt_sch.c:550`) guarded by `/tmp/kahawai_lcore.lock` (`mt_platform.h:57`) [verified]. `MTL_INSTANCE_TASKLET_THREAD` runs the schedulers as unpinned pthreads ([04 §7.1](../04-threading-and-execution.md)).
- **Time.** `time_source`: AUTO, the built-in PTP client, the PHC disciplined by ptp4l/phc2sys, `CLOCK_TAI`, system TAI or user-supplied (`mtl.h:381-388`).
- **Multicast.** MTL builds IGMPv3 reports itself, repeats an unsolicited report every 10 s until it sees a query (`mt_mcast.h:14`, `mt_mcast.c:359`), and sends a leave on session teardown (`mt_mcast.c:735`) [verified].
- **MtlManager.** A root daemon on `/var/run/imtl/mtl_manager.sock`. It arbitrates lcores, loads the XDP program on host interfaces, adds flows and UDP filters, and hands out XSK map fds ([15 §7](../15-security-and-deployment.md#7-the-mtlmanager-socket)).

## 1. Pod and container lifecycle

### 1.1 Termination

On pod deletion with the default 30 s grace period, the sequence is as follows [verified, S1, S2]:

| Step | What happens | Consequence for MTL |
|---|---|---|
| 1 | The pod shows as Terminating, and the grace-period countdown **starts** | The clock runs before MTL hears anything |
| 2 | `preStop` runs if defined: exec in the container, or httpGet/sleep from the kubelet. "the hook must complete its execution before the TERM signal can be sent" | Time the hook spends is time the SIGTERM handler loses. The docs' example: grace 60 s, hook 55 s, stop 10 s means the container is killed |
| 3 | "The kubelet triggers the container runtime to send a TERM signal to process 1 inside each container" | Only PID 1 is signalled. A shell wrapper without `exec` swallows it (§1.6) |
| 4 | If `preStop` is still running when the grace period expires, there is "a small, one-off grace period extension of 2 seconds" | |
| 5 | At expiry: "The container runtime sends `SIGKILL` to any processes still running in any container in the Pod" | No cleanup code runs |

- **Stop signal.** The `ContainerStopSignals` feature lets a pod pick a stop signal other than TERM; it needs `spec.os.name` [verified, S1].
- **Hooks are not guaranteed.** Delivery is "at least once" and a failed hook "kills the Container". `preStop` also runs before a liveness- or startup-probe kill, but not when the process has already exited [verified, S2].
- **Probe kills.** A liveness kill follows the same flow. A probe-level `terminationGracePeriodSeconds` (KEP-2238) can shorten it [inferred: GA around v1.27].

### 1.2 Kills without a grace period

- **Hard eviction.** Node-pressure eviction with a hard threshold "uses a `0s` grace period (immediate shutdown)". With a soft threshold, the kubelet uses the lesser of the soft grace period and `eviction-max-pod-grace-period`. In both cases, "The kubelet does not respect your configured PodDisruptionBudget or the pod's terminationGracePeriodSeconds" [verified, S6].
- **OOM kill.** The kubelet sets `oom_score_adj` to −997 for Guaranteed pods. An OOM-killed container is restarted per `restartPolicy` and does not get SIGTERM [verified, S6; the "no SIGTERM" part is kernel behaviour, inferred].
- **Hugepages are outside the memory cgroup.** With `HugepageAwareEviction`, the kubelet subtracts hugepage capacity from `memory.available`, because hugepage RAM otherwise inflates the available figure and "can delay eviction and lead to OOM kills" [verified, S6].
- **Graceful node shutdown.** It is off unless `shutdownGracePeriod` is set; when set, pods get their share of it (regular pods first, then critical ones). Under non-graceful shutdown (the `out-of-service` taint), pods are force-deleted [verified, S7].
- **API eviction and drain.** API-initiated eviction (`kubectl drain`) respects PDBs and the grace period [inferred]. The SR-IOV Network Operator drains nodes, by pool and `maxUnavailable`, when it reconfigures VFs, so a VF policy change restarts every SR-IOV pod on the node [verified that drains are pool-scoped, S18; the restart consequence is inferred].

### 1.3 Probes and restarts

- **Probe types.** Startup: "Kubernetes does not execute liveness or readiness probes until the startup probe succeeds"; on failure the container is killed. Liveness: "determine when to restart a container", and it does not wait for readiness.
  Readiness: "determine when a container is ready to accept traffic" and runs for the whole lifetime. Probes can be exec, httpGet, tcpSocket or gRPC (stable since v1.27); `timeoutSeconds` defaults to 1 s [verified, S1, S8]. The other defaults are `periodSeconds` 10 and `failureThreshold` 3 [inferred, from the API reference].
- **Exec probes are expensive here.** Each one forks a process in the container's cgroup, so it competes with tasklets on an exclusive-CPU pod. httpGet and gRPC are served by the app's own thread [inferred].
- **Restart backoff.** It is exponential: "10s, 20s, 40s, …", capped at 300 s and reset after 10 minutes of clean running. Newer gates reduce it: `ReduceDefaultCrashLoopBackOffDecay` (1 s up to 60 s) and a per-node `maxContainerRestartPeriod`. `restartPolicyRules` match exit codes, and `RestartAllContainers` restarts the whole pod in place [verified, S1].
- **Readiness does not steer multicast.** Media goes to multicast groups, not Services, so readiness does not move it. It does gate Deployment rollouts and anything (NMOS controllers, operators) that watches pod conditions. A RollingUpdate brings the new sender up before the old one stops, which can put two senders on one group; senders need `strategy: Recreate` or distinct addresses [inferred].

### 1.4 What survives what

| Resource | Container restart (same pod) | Pod deleted, replacement pod | SIGKILL of the MTL process |
|---|---|---|---|
| Network namespace, pod IP | kept [verified, S10] | new | n/a |
| IPC namespace (SysV shm, POSIX shm) | **kept**: containers in a pod share IPC ("SystemV semaphores or POSIX shared memory", S10), and the sandbox holds it [inferred] | new | shm segments stay; `IPC_RMID` runs only in MTL's own uninit (`mt_sch.c:596-605`) [verified] |
| PID namespace | new (per container unless `shareProcessNamespace`); PIDs restart from low numbers [inferred] | new | n/a |
| Container root filesystem (`/tmp`) | new (fresh from the image) [inferred] | new | n/a |
| emptyDir, including `medium: HugePages` | **kept**: "safe across container crashes" [verified, S9] | deleted | hugetlbfs files would stay; MTL leaves none because `--in-memory` uses `memfd_create(MFD_HUGETLB)` (`eal_memalloc.c:224-247`) [verified] |
| Allocated SR-IOV VF | **kept** (the same PCI address and `/dev/vfio/N`) [inferred: device plugin allocations are per pod] | released, then reset by CNI DEL; may go to another pod | vfio fd closes, and vfio-pci resets the device on last close (`vfio_pci_core_disable` → `vfio_pci_dev_set_try_reset`, `vfio_pci_core.c:683-810`) and again on next open (`pci_try_reset_function`, `:612`) [verified] |
| VF state the VF requested (multicast MAC filters, promisc/allmulti, FDIR, queues) | cleared by the reset | cleared | cleared: `ice_reset_vf` clears promisc/allmulti and FDIR and rebuilds the VSI (`ice_vf_lib.c:889-982`) [verified] |
| VF admin state set by the PF (MAC, VLAN, Tx rate) | **kept**: "host admin configuration is persistent across reset" (`ice_vf_lib.c:491-521`) [verified] | restored to the pre-pod value by sriov-cni DEL (§2.2) | kept |
| MtlManager lcores, queues, flows of this client | released when the socket closes (`mtl_instance.hpp:66-88`) [verified] | same | same |
| MtlManager UDP filter refcounts (`udp4_dp_filter`) | **leaked**: they are kept per interface, not per client (`mtl_instance.hpp:272-286`, `mtl_interface.hpp:102-134`) [verified] | leaked | leaked |
| XDP program on a host interface | owned by MtlManager, which detaches it only when its interface object goes (`mtl_interface.hpp:453`) [verified] | same | same |
| IGMP membership in the switch | lapses after about 260 s without reports (§7.1) | same | same |

So MTL's SysV-shm lcore table (the path without a manager) outlives a crashed container in a pod. The next container sees stale entries. Its cleanup, `lcore_shm_check_and_clean`, frees an entry only when the hostname and user match and `kill(pid, 0)` fails (`mt_sch.c:685-699`) [verified].
In a pod the hostname is the same, and the new container's PIDs restart, so a reused PID keeps a dead lcore "active" [inferred]. The docs already warn that the PID check "becomes less useful" in containers ([doc/shm_lcore.md](../../../shm_lcore.md)) [verified].

### 1.5 Sidecars and init containers

- **Sidecars.** A sidecar is an init container with `restartPolicy: Always`; stable since v1.33 [verified, S3]. It starts in order before the main containers. On termination, "the kubelet postpones terminating sidecar containers until the main application container has fully stopped", then stops them "in the reverse order".
  If time runs out, "all remaining containers in the Pod will be terminated simultaneously with a short grace period" [verified, S1, S3].
- **Ordering helps a media sidecar.** An MTL "media gateway" sidecar serving the app containers outlives them, but only if the app containers stop well within the grace period [inferred].
- **Init containers.** They run to completion before the app. They suit host checks (hugepages, VF present, PTP locked) but cannot hold devices for the app: each container opens the vfio fd itself [inferred].

### 1.6 PID 1 and `shareProcessNamespace`

- **PID 1 ignores default signals.** The kernel does not deliver a signal whose disposition is the default to a namespace's init process, so an MTL app running as PID 1 with no SIGTERM handler ignores SIGTERM and dies only at SIGKILL [inferred, pid_namespaces(7)]. Docker's `init: true` (used in `docker/docker-compose.yml`) has no direct pod equivalent; use tini or dumb-init, or install a handler.
- **Shared process namespace.** With `shareProcessNamespace: true`, "The container process no longer has PID 1", the pause process is PID 1, and processes and `/proc/$pid/root` are visible across containers [verified, S5].
- **MTL itself.** The library installs no signal handlers today: grep finds no `signal(`/`sigaction` in `lib/src` [verified]. DPDK installs a **temporary** process-wide SIGBUS handler while it faults in hugepages (`eal_memalloc.c:93-120`, `:589-594`) [verified].

## 2. SR-IOV in Kubernetes

### 2.1 The SR-IOV network device plugin [verified, S16]

- **Resources.** VFs are advertised as `<prefix>/<resourceName>` (default prefix `intel.com`), selected by vendor, device, `drivers: ["vfio-pci"]` and `pfNames` (with VF ranges such as `netpf0#0,2-7`). A device matching several selectors goes to the first.
- **Environment.** The plugin injects `PCIDEVICE_<RESOURCE>=0000:03:02.1,…` and `PCIDEVICE_<RESOURCE>_INFO={"0000:3b:02.6":{"vfio":{"vfio-dev-mount":"/dev/vfio/169","vfio-mount":"/dev/vfio/vfio"}}}`.
- **Devices.** For vfio-pci, the device spec adds the group node and `/dev/vfio/vfio`. That is what lets a vfio pod run "in a non-privilege Pod with only **IPC_LOCK** capability added" (`docs/dpdk/README.md`).
- **No VF management.** "This plugin does not bind or unbind any driver … It also doesn't create virtual functions". The CNI resets the VF on pod deletion.
- **No-IOMMU VMs.** The plugin "Works within virtual deployments … that do not have virtualized-iommu support". The nodes appear as `/dev/vfio/noiommu-N`, mounted at the usual paths, and "the pod must be privileged and have both IPC_LOCK and CAP_SYS_RAWIO" (`docs/dpdk/README-virt.md`).
- **Discovery.** Multus publishes the attachment result, including the IPAM address and a `device-info` block with the PCI address, in the `k8s.v1.cni.cncf.io/network-status` annotation; an app can read it through a downward-API volume [inferred: NPWG device-info spec, not re-read].

### 2.2 sriov-cni, ADD and DEL [verified, S17]

- **DPDK mode.** The CNI enters DPDK mode when the VF has a DPDK driver (`n.DPDKMode = hasDpdkDriver`, `pkg/config/config.go:90`).
- **ADD.** It records the VF's original state (`FillOriginalVfInfo`), then **always** calls `ApplyVFConfig`: VLAN, QoS and proto, admin MAC, min/max Tx rate, spoofchk, trust, link state. Only the netns move (`SetupVF`) is skipped in DPDK mode (`cnicommands/cni.go:98-110`). It caches the NetConf under `/var/lib/cni/sriov`.
- **IPAM.** For DPDK VFs, IPAM "only allocate[s] IP address(es) … not apply the IP address(es) to container interface".
- **DEL.** It takes a per-device lock, calls `ResetVFConfig` (VLAN, spoofchk, trust and rates restored if they were set; the admin MAC restored when one was cached), skips `ReleaseVF` in DPDK mode, and deletes the allocated-PCI record (`cni.go:240-325`).
- **Not handled.** sriov-cni neither saves nor restores allmulti, promisc or multicast filters. Those are VF-requested state, which the vfio reset clears anyway (§1.4).
- **Operator fields.** The SR-IOV Network Operator exposes the same knobs per `SriovNetwork`: `vlan`, `vlanQoS`, `vlanProto`, `spoofChk`, `trust`, `linkState`, `maxTxRate`, `minTxRate` (`api/v1/sriovnetwork_types.go`) [verified, S18].

### 2.3 What can leak from the previous tenant of a VF

- **Nothing on the data path.** vfio reset plus the ice VFLR handling clears the VF's queues, filters, promisc and FDIR; queue-level TM and rate-limit configuration made through virtchnl goes with the VSI rebuild [inferred from `ice_vf_pre_vsi_rebuild`, not traced line by line].
- **PF-side admin state can leak.** Anything set with `ip link set <pf> vf N …` outside sriov-cni (for example a `max_tx_rate` a script applied) persists across resets and pods. sriov-cni restores only the fields it changed.
- **Hardware-pacing setup can leak.** MTL's hardware-rate-limit pacing depends on a patched ICE PF driver (CLAUDE.md, "SEGFAULT in `iavf_tm_node_add`"). PF-level setup done for it is outside Kubernetes' view [inferred].
- **The CNI lock prevents reuse races.** sriov-cni's device lock and allocated-PCI file stop a VF being configured for a new pod before DEL of the old one completes [verified, `cni.go:240-247`, `config.go:56-77`].

### 2.4 E810 VF limits that matter to MTL [verified, S20]

| Limit | Source | Effect on MTL |
|---|---|---|
| Promiscuous or allmulti mode requires a trusted VF ("Unprivileged VF %d is attempting to configure promiscuous mode") | `virt/virtchnl.c:508-509` | an RX design that falls back to allmulti fails on an untrusted VF |
| An untrusted VF may hold at most `ICE_MAX_MACADDR_PER_VF` = 18 MAC filters, unicast and multicast together | `virt/virtchnl.h:26`, `virt/virtchnl.c:965-970` | each IPv4 multicast group needs a multicast MAC filter, so about 16 groups per untrusted VF after its own unicast and broadcast [inferred count] |
| If the PF set the MAC administratively (sriov-cni `mac`, or the operator), an untrusted VF cannot add or delete unicast MACs | `virt/virtchnl.c:657-666` | MTL must use the VF's MAC, never program its own |
| spoofchk on (the default) drops frames whose source MAC is not the VF's | ice behaviour [inferred] | same |
| PHC: the VF has a read-only clock via `VIRTCHNL_OP_1588_PTP_GET_TIME`; the kernel registers only `gettimex64` | `iavf/iavf_ptp.c:184-332` | the VF can read, but not discipline, the device clock (§5) |

- **DCF.** The Device Config Function is a trusted VF that programs switch rules for its siblings. It is a node-level role, not something to give a tenant pod [inferred].
- **ADQ.** ADQ (queue groups bound to applications) applies to kernel netdevs and has no Kubernetes integration that I found [inferred].

## 3. Hugepages, memlock and IOMMU

### 3.1 Hugepages [verified, S11]

- **Resources.** `hugepages-2Mi` and `hugepages-1Gi` are container-level resources. "Huge page requests must equal the limits", "huge pages do not support overcommit", and a CPU or memory request must accompany them. A ResourceQuota can cap them per namespace.
- **Per-container accounting.** "Huge pages are isolated at a container scope, so each container has own limit on their cgroup sandbox". Two MTL processes in two containers of one pod have separate budgets.
- **Volumes.** Use `emptyDir: {medium: HugePages}` for one size and `HugePages-<size>` when the pod uses several. The volume "may not consume more huge page memory than the pod request".
- **Node changes.** Pages must be pre-allocated on the node; dynamically added pages need a kubelet restart to show up.
- **MTL needs only the resource.** With `--in-memory`, DPDK "bypasses the need to access hugepage mount point and files within it" [verified, S24 enable_func], so MTL needs the hugepage *resource* but not necessarily the emptyDir mount [inferred for MTL; DPDK still reads `/sys/kernel/mm/hugepages`].
- **Host counters can mislead.** `/sys/kernel/mm/hugepages/*/free_hugepages` in a container shows the **host** pool, not the container's limit [inferred]. Over the limit, a hugetlb fault raises SIGBUS. DPDK catches it during allocation ("SIGBUS: Cannot mmap more hugepages", `eal_memalloc.c:589-594`) [verified], so an allocation at open fails cleanly.
  Memory not touched at open could still fault later [inferred].

### 3.2 Memlock and `CAP_IPC_LOCK`

- **vfio accounting.** vfio type1 charges pinned pages to `RLIMIT_MEMLOCK` unless `capable(CAP_IPC_LOCK)` (`vfio_iommu_type1.c:1659`, `:1777`) [verified]. Separately, DMA mappings per container are capped by `dma_entry_limit`, 64K by default [verified, S24].
- **No rlimit API.** Kubernetes has no pod field for rlimits; the container inherits the runtime's default. A DPDK pod therefore adds `IPC_LOCK` rather than raising memlock [inferred: no `ulimits` in the Pod API; the compose file uses `ulimits.memlock: -1`, `docker/docker-compose.yml`].
- **User namespaces defeat it.** `capable()` checks the **initial** user namespace. In a pod with user namespaces (`hostUsers: false`), "capabilities … are limited to the pod user namespace", so `IPC_LOCK` there does not lift the vfio memlock limit [verified that capable() is used, S19, and the S15 statement;
  the combination is inferred]. User-namespace pods also cannot use host namespaces [verified, S15].

### 3.3 IOMMU and no-IOMMU

- **No-IOMMU is unsafe by design.** "Since no-IOMMU mode forgoes IOMMU protection, it is inherently unsafe" [verified, S24]. vfio refuses a no-IOMMU group without `CAP_SYS_RAWIO` (`vfio/group.c:428-429`) [verified].
- **It needs PA mode.** No-IOMMU forces IOVA = PA. Reading PFNs from `/proc/self/pagemap` needs `CAP_SYS_ADMIN` ("`DAC_READ_SEARCH` and `SYS_ADMIN` to read `/proc/self/pagemaps`") [verified, S24].
- **Result: a node-compromise risk.** A no-IOMMU pod is effectively privileged, and its NIC can DMA anywhere in host memory. That is a node-level compromise surface, acceptable only on single-tenant nodes [inferred]. Clouds without a virtual IOMMU are the common case ([doc/aws.md](../../../aws.md), [doc/vm.md](../../../vm.md)).

## 4. CPU

- **Static policy.** "Only containers that are both part of a `Guaranteed` pod and have integer CPU `requests` are assigned exclusive CPUs"; others share the pool. The static policy needs a non-zero CPU reservation, and the kubelet reconciles assignments into cgroupfs [verified, S12].
- **Policy options.** `full-pcpus-only` (GA, 1.33+), `distribute-cpus-across-numa` (beta), `strict-cpu-reservation` (GA, 1.35+), `prefer-align-cpus-by-uncorecache` (GA, 1.36+), `align-by-socket` and `distribute-cpus-across-cores` (alpha) [verified, S12]. `full-pcpus-only` allocates whole physical cores, SMT siblings included [inferred from the name and the ResourceManagers docs, not re-read].
- **Topology Manager.** Policies are `none`, `best-effort`, `restricted` and `single-numa-node`, with container or pod scope; it is stable since v1.27. A pod it rejects ends with an admission failure and "the Kubernetes scheduler will **not** attempt to reschedule the pod".
  The scheduler is not topology-aware, so Holoscan for Media requires a topology-aware secondary scheduler [verified, S13, S29].
- **How a process finds its CPUs.** The cpuset cgroup sets the task's allowed mask, which `sched_getaffinity(0)` returns. DPDK uses exactly that when no core list is given: `rte_thread_get_affinity_by_id(rte_thread_self(), &cpuset)` with source "affinity auto-detection" (`eal_common_options.c:2163-2168`).
  An `-l` list naming CPUs outside it fails with "please check specified cores are part of …" (`:2174-2181`) [verified, S23].
- **Pinning outside the cpuset.** `sched_setaffinity` or `pthread_setaffinity_np` with a mask outside the cpuset fails with EINVAL; a partly overlapping mask is cut to the intersection [inferred, sched_setaffinity(2)]. MTL's `mtl_bind_to_lcore` already uses the EAL's cpuset per lcore (`mt_main.c:735-741`) [verified].
- **MtlManager is cpuset-blind.** It grants lcore IDs from its own host-global view (`mt_sch.c:713-731` walks the EAL's enabled lcores and asks the manager) [verified]. It does not know which CPUs belong to which pod. Inside a pod the EAL lcore set is already limited to the pod's CPUs, so arbitration between pods is CPU Manager's job [inferred].
- **No load balancing on isolated CPUs.** OpenShift's PerformanceProfile splits CPUs into `isolated` and `reserved`. Pods that want full CPUs carry `cpu-load-balancing.crio.io: "disable"`, `cpu-quota.crio.io: "disable"`, `irq-load-balancing.crio.io: "disable"` and run with `runtimeClassName: performance-<profile>`;
  a real-time kernel is optional (`realTimeKernel.enabled`) [verified, S28]. With load balancing disabled (as with `isolcpus`), the kernel does not spread threads across the pod's CPUs: an unpinned thread stays where it was started.
  So MTL's unpinned `TASKLET_THREAD` schedulers can stack on one CPU [inferred; matches the `isolcpus` behaviour noted in [doc/isolation.md](../../../isolation.md)].
- **No spare CPU for housekeeping.** A Guaranteed pod with integer CPUs owns only its exclusive CPUs; its non-scheduler threads (waker, admin, stats, the app's control thread) run on those same CPUs. MTL's rule that workers "never run on a scheduler lcore" ([15 §4](../15-security-and-deployment.md#4-threads-the-waker-priorities-and-affinity)) then needs one CPU of the pod set aside [inferred].
- **Interrupts.** With `globallyDisableIrqLoadBalancing: true` or the per-pod annotation, IRQs stay off the pod's CPUs [verified, S28/S29]. Kernel-socket and AF_XDP backends depend on IRQ and softirq placement for the NIC queue, which a pod cannot set itself [inferred].
- **Holoscan for Media in practice.** Isolated `8-63`, reserved `0-7`, `nosmt`, `idle=poll`, `single-numa-node`, and Guaranteed pods with `cpu: 4`, `hugepages-2Mi: 4Gi` and an SR-IOV pool resource [verified, S29].

## 5. Time

- **PTP runs on the node.** OpenShift's PTP Operator runs linuxptp as a DaemonSet configured by `PtpConfig`. Holoscan for Media's ordinary-clock profile uses `ptp4l -2 -s` on the PF and `phc2sys -w -m -n <domain> -s <if>` with SCHED_FIFO 10 [verified, S29].
  Events reach applications through a REST API (v1 and v2) and the cloud-event-proxy [verified as headings only, S32]. Holoscan states: "The Holoscan for Media platform provides PTP as a service" [verified, S29].
- **`CLOCK_TAI` needs `phc2sys` to set the offset.** `phc2sys` sets the kernel TAI offset (`clock_adjtime(CLOCK_REALTIME, ADJ_TAI)`) only when the PTP UTC offset is traceable (`pmc_agent_utc_offset_traceable`, `phc2sys.c:1137-1141`, `clockadj.c:211-219`) [verified, S25].
  Without that, `CLOCK_TAI == CLOCK_REALTIME` and the offset reads 0, which is why `MTL_TIME_SOURCE_CLOCK_TAI` must reject a zero offset (`mtl.h:385`). Setting it needs `CAP_SYS_TIME`, which a workload pod should not have [inferred].
- **No per-pod clock.** Time namespaces offset only `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME` (`kernel/time/namespace.c:31-35`) [verified, S22]. `CLOCK_REALTIME` and `CLOCK_TAI` in a pod are the host's.
- **The VF reads the PF's clock.** An E810 VF has no clock of its own. The kernel iavf driver registers a read-only PHC through `VIRTCHNL_OP_1588_PTP_GET_TIME` [verified, S21]. DPDK's iavf PMD has `timesync_read_time` and a PHC sync alarm for RX timestamps (`iavf_ethdev.c:156-171`, `:272`, `:1111-1149`) [verified].
  So a pod with a DPDK VF can read the PHC that host ptp4l disciplines on the PF, without `/dev/ptp*` and without `CAP_SYS_TIME`. One PHC per E810 device serves all its ports and VFs [inferred].
- **Running ptp4l on a VF.** It cannot work as a servo, since the VF cannot adjust the clock; OpenShift documents PTP on PFs [inferred: VF PTP not offered by the operator].
- **The built-in PTP client in a pod.** MTL's own client on a VF needs PTP multicast (224.0.1.129 or 01:1B:19:00:00:00) to reach the VF; that costs MAC filters (§2.4) and works only if the switch forwards PTP to that VLAN. It disciplines a software offset, not the PHC.
  N pods mean N independent PTP clients with N slightly different time estimates, so cross-pod alignment ([06](../06-timing-pacing-and-sync.md)) is worse than with one host-disciplined PHC [inferred].
- **What a pod can actually use, in order of preference.**
  1. The NIC PHC read through the VF, disciplined by the node's ptp4l. This is TAI by PTP definition.
  2. `CLOCK_TAI`, kept by the node's `phc2sys` with a non-zero kernel offset. Its accuracy is limited by the `phc2sys` servo (sub-µs to a few µs) [inferred].
  3. MTL's built-in PTP client, only where no node PTP exists.
  4. `CLOCK_REALTIME` plus an assumed UTC offset, marked ESTIMATED.

## 6. Capabilities and security

| Need | Capability or setting | Backend | Notes |
|---|---|---|---|
| Pin DMA memory beyond memlock | `IPC_LOCK` | vfio, AF_XDP UMEM | ineffective under user namespaces (§3.2) |
| NUMA policy (`mbind`, `set_mempolicy`) | `SYS_NICE` under the runtime's default seccomp profile | all | the compose file adds it: "Lets DPDK set the NUMA memory policy" [verified, docker/README.md]; the seccomp gating is inferred from Docker's default profile |
| Optional `SCHED_FIFO` waker | `SYS_NICE` | all | failure is reported, not fatal ([15 §4](../15-security-and-deployment.md)) |
| vfio VF | the device-plugin device nodes, nothing else | DPDK PMD | no `privileged`, no hostPath `/dev/vfio` [verified, S16] |
| no-IOMMU VF | `SYS_RAWIO` (and `SYS_ADMIN` for pagemap PFNs) | DPDK PMD | effectively privileged (§3.3) |
| AF_XDP socket | `NET_RAW`: `if (!ns_capable(net->user_ns, CAP_NET_RAW))` | native AF_XDP | `xsk.c:2196` [verified] |
| Load or attach an XDP program, update maps | `NET_ADMIN` + `BPF` (or `SYS_ADMIN`), bpffs | AF_XDP | belongs to a node agent, not the pod (§7.2) |
| `ethtool` ntuple flows, queue setup on a kernel netdev | `NET_ADMIN` in the netdev's namespace | AF_XDP, kernel | done by MtlManager today |

- **Pod Security Standards.** Baseline forbids host namespaces, `privileged` and hostPath, and allows adding only `AUDIT_WRITE, CHOWN, DAC_OVERRIDE, FOWNER, FSETID, KILL, MKNOD, NET_BIND_SERVICE, SETFCAP, SETGID, SETPCAP, SETUID, SYS_CHROOT`.
  Restricted additionally requires `runAsNonRoot`, `allowPrivilegeEscalation: false`, seccomp `RuntimeDefault` or `Localhost`, dropping ALL capabilities (only `NET_BIND_SERVICE` may be added back), and the volume types configMap, csi, downwardAPI, emptyDir, ephemeral, PVC, projected and secret [verified, S14].
  **Any MTL pod with `IPC_LOCK` needs a namespace labelled `privileged`** or a policy exception, even though it is not a privileged container.
- **Non-root works.** It needs only vfio node permissions (the device plugin and runtime handle these) and memlock. `--in-memory` removes the hugetlbfs write requirement [verified, S24]. MTL's remaining writes are `/tmp/kahawai_lcore.lock` (opened `O_CREAT`, falls back to read-only, `mt_sch.c:506-511`) and the SysV shm segment [verified].
  Neither works on a read-only root filesystem without a `/tmp` emptyDir [inferred]. OpenShift documents "rootless DPDK pods", needing the `container_use_devices` SELinux boolean only for a TAP device [verified, S28].
- **hostPath for the manager socket.** "Using the `hostPath` volume type presents many security risks … If you can avoid using a `hostPath` volume, you should" [verified, S9]. The socket today is world-writable, and identity comes from the message body ([15 §7](../15-security-and-deployment.md)) [verified there].
- **`SO_PEERCRED` across namespaces.** The kernel translates the peer's PID into the **reader's** PID namespace: a manager without `hostPID` gets 0 for a process it cannot see, and with `hostPID` it gets the host PID, not the one the client writes in its register message.
  The UID is translated into the reader's user namespace: a user-namespaced pod's UID 1000 is seen as a host UID above 65535 [inferred, kernel `cred_to_ucred`;
  the S15 mapping rule is verified]. `SO_PEERPIDFD` (Linux 6.5+) gives a race-free handle [inferred]. Identity checks therefore have to rest on UID/GID (or a per-pod socket and directory) and never on the PID the client reports.

## 7. Multicast, IGMP and AF_XDP

### 7.1 IGMP from a pod

- **MTL is the IGMP host.** On a DPDK VF no kernel stack sees the traffic, so MTL itself must answer queries, send unsolicited reports and send leaves. It does all three (see "What MTL needs from the node", `mt_mcast.c`) [verified].
- **A crashed RX pod keeps its groups for minutes.** It sends no leave; the snooping switch keeps forwarding until the group membership interval expires: robustness 2 × query interval 125 s + query response interval 10 s = **260 s** with RFC 3376 defaults [inferred, RFC 3376 §8.4; switch defaults vary]. Without a querier, many switches keep or flood groups indefinitely [inferred].
- **The flood follows the VF.** The flood lands on the VF's port. If the same VF goes to another pod, that pod's VSI filters drop the traffic, but uplink bandwidth is still used.
- **2022-7.** Both legs leak independently.
- **Recreated pods.** A Deployment pod recreated elsewhere joins from a new port, so the old port keeps receiving for the same interval.

### 7.2 AF_XDP from a pod

- **The program outlives its loader.** A netlink-attached XDP program stays on the interface after the loading process exits.
  libxdp's multiprog dispatcher pins its component programs in bpffs (`/sys/fs/bpf/xdp/dispatch-IFINDEX-DID/…`) precisely because "The kernel will automatically detach component programs from the dispatcher once the last reference to them disappears" [verified, S26;
  the persistence of the netlink attachment is inferred]. Only bpf_link attachments that are not pinned die with the process [inferred].
- **Sockets clean up after themselves.** XSK sockets and their xskmap entries go when the process dies (`xsk_delete_from_maps`, `xsk.c:1574`) [verified].
- **The netdev must be in the socket's namespace.** An XSK binds to `dev_get_by_index(sock_net(sk), ifindex)` (`xsk.c:1645`) [verified].
  With a kernel-driver VF moved into the pod netns by sriov-cni, the pod's ifindex differs from the host's. MtlManager in the host netns receives the **pod's** ifindex (`if_nametoindex` in `mt_af_xdp.c:390`) and looks it up in its own namespace (`get_interface(ifindex)`, `mtl_instance.hpp:274-276`) [verified code;
  the mismatch is inferred]. The manager-assisted AF_XDP path therefore works only with `hostNetwork: true`, or after a netns-aware redesign.
- **Rights MTL takes today.** Without the manager, `xsk_socket__create` loads libxdp's default program, which needs BPF and NET_ADMIN rights ("please run with mtl manager or root user", `mt_af_xdp.c:434`) [verified]. With the manager, MTL passes `XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD` and gets the map fd over the socket (`:390-419`) [verified].
- **Prior art.** Intel's AF_XDP device plugin and CNI load the program on the host through the device plugin, and hand the XSK map to the pod over a per-pod UDS (30–300 s inactivity timeout, a UID setting so non-root can use it) or by pinned maps.
  It notes that "User 0 does not imply that the pod needs to be privileged", but pinned maps require a privileged DaemonSet for bidirectional mount propagation. **Intel no longer maintains it**; the fork is `redhat-et/afxdp-plugins-for-kubernetes` [verified, S27]. bpfman (CNCF) is the general Kubernetes answer to "who owns an eBPF program's lifetime" [inferred].

## 8. How others deploy

| Product | Model | Shutdown, crash and health | Source |
|---|---|---|---|
| MTL via Media Communications Mesh | `media-proxy` DaemonSet: `privileged: true`, `runAsUser: 0`, hostPath `/dev/vfio`, VF hard-coded in args (`-d 0000:31:01.5`), hugepage emptyDirs of both sizes, `/var/run/imtl` from a hostPath PV, hostPorts; no probes, no `preStop` | none declared | S30 [verified] |
| NVIDIA Holoscan for Media (Rivermax, ST 2110) | OpenShift with PerformanceProfile, SR-IOV Operator, PTP Operator (OC, `phc2sys -w`), NUMA Resources Operator and topology-aware scheduler; Helm charts; Guaranteed pods (`cpu: 4`, `hugepages-2Mi`, an SR-IOV pool) | PTP is "a service" of the platform; monitoring for SR-IOV, PTP and the NMOS registry | S29 [verified] |
| Rivermax on mlx5 | bifurcated driver: no vfio, so no device reset on exit; RDMA device via `isRdma` | the kernel keeps the netdev; flow steering is cleaned up with the verbs context | S16 notes that bifurcating drivers need no privilege [verified]; the rest is inferred |
| Telco CNFs (OpenShift telco core/RAN reference designs; Ericsson, Nokia, Cisco user planes) | Guaranteed QoS, exclusive CPUs, the CRI-O annotations of §4, SR-IOV vfio VFs, hugepages; root DPDK historically, now "rootless DPDK pods" | per-vendor; commonly an HTTP health endpoint served by a control thread, not by the dataplane | S28 [verified for the OpenShift side]; vendor practice inferred |
| VPP (Calico/VPP, FD.io) | VPP as a privileged hostNetwork DaemonSet that owns the uplink (DPDK, AF_XDP, AF_PACKET or RDMA) and serves pods over memif or tun | the agent restores interfaces after a VPP restart; probes hit the agent, not the VPP main loop | [inferred] |
| OVS-DPDK | a host service or privileged DaemonSet; pods attach through vhost-user sockets (userspace CNI) | a pod restart does not touch the NIC | [inferred] |
| AWS CDI, EVS, Grass Valley AMPP, Nevion, others | no public technical deployment detail found (the AWS CDI docs returned 403) | | [not found] |

Common patterns [inferred from the table]:

1. The NIC owner is either the pod itself (SR-IOV VF) or a privileged node agent (VPP, OVS, the AF_XDP device plugin, MtlManager). Never both.
2. PTP is a node service, and pods consume time rather than run PTP.
3. Liveness checks the control thread's view of the dataplane's progress counters, never packet flow. A liveness probe that fails when the network is quiet restarts healthy RX pods during an upstream outage.
4. Readiness waits for link, time lock and session start.

## 9. Gaps in today's MTL against these facts

| # | Gap | Where |
|---|---|---|
| G1 | No instance-wide stop that respects a deadline; `mtl_instance_close` takes no timeout, and per-session closes add up | `mtl.h:417` |
| G2 | The SysV-shm lcore table and `/tmp` lock assume a host, not a pod: they survive container restarts, fail the PID check, and need writable `/tmp` | `mt_sch.c:506-605`, `:685-699` |
| G3 | MtlManager identity comes from the message, the socket is world-writable and needs a hostPath, the manager is netns-blind for AF_XDP, and UDP filter refcounts leak on client death | §6, §7.2, [15 §7](../15-security-and-deployment.md) |
| G4 | `TASKLET_THREAD` schedulers are unpinned, which is wrong on CPUs with load balancing disabled | [04 §7.1](../04-threading-and-execution.md) |
| G5 | No health or progress signal an app can turn into a liveness probe | — |
| G6 | No way to name a port by its Kubernetes resource (`PCIDEVICE_*`) or to take the IP from the IPAM result | `MTL_PORTS` exists (`mtl.h:405-408`) |
| G7 | No up-front check of VF trust, MAC filter budget, memlock and capabilities; failures surface late or as generic errors | — |

## 10. What this means for MTL's API

Each requirement names the finding it rests on. "Must" means the design is incomplete without it.

### Lifecycle

- **K-REQ-1: Bounded orderly shutdown.** One call stops a whole instance within a caller-given deadline (for example `mtl_instance_close(mt, timeout_ns)`, or a shutdown budget option). It drains TX to a frame boundary, flushes the rest, retires every session, releases the manager, and closes the devices.
  It returns `-MTL_ETIMEDOUT` with what was skipped, never later than the deadline. The documented typical cost must fit well inside the default 30 s grace period after a `preStop` hook. *Why: §1.1; G1.*
- **K-REQ-2: Shutdown order that protects the network first.** Within the budget the order is: (1) stop TX at the next frame boundary, (2) send IGMP leaves on every leg, (3) release MtlManager resources, (4) device and memory teardown, which can be cut short without harm because the kernel cleans it up. Steps 1–2 must complete in about one frame time plus a few ms. *Why: §1.4, §7.1.*
- **K-REQ-3: Signal-safe trigger, no handlers.** The library installs no signal handlers and documents the SIGTERM recipe. A handler calls the AS-safe `mtl_instance_interrupt`, and the main thread calls the bounded close. It also documents the PID 1 rule (handle SIGTERM explicitly, or use tini) and the temporary DPDK SIGBUS handler at open. *Why: §1.6.*
- **K-REQ-4: Crash-only correctness.** SIGKILL at any instant leaves nothing that blocks the next start or needs manual cleanup. Pod-scoped and host-scoped leftovers (SysV shm, files in `/tmp` or hugetlbfs, manager state) must be either avoided or owned by a party that reclaims them when the connection closes.
  The design must list every residual effect (the IGMP timeout, XDP programs owned by the node agent) with its owner and how long it lasts. *Why: §1.2, §1.4; G2, G3.*
- **K-REQ-5: Restart in place.** `mtl_instance_open` must succeed in a restarted container of the same pod, with the same VF just reset by vfio, the same IPC namespace and the same emptyDirs, and without depending on PIDs from a previous incarnation. Lcore arbitration without a manager in a pod must use the affinity mask (K-REQ-8), not the shm table. *Why: §1.4; G2.*
- **K-REQ-6: Fail fast, with a reason.** Open checks everything it can before it touches a device: VF presence and driver, IOMMU mode, hugepages within the **cgroup** limit, memlock or `IPC_LOCK`, needed capabilities, CPU set size.
  It fails with one named reason and a one-line text the app can write to `terminationMessagePath`. A CrashLoopBackOff is then diagnosable from `kubectl describe`. *Why: §1.3 (backoff up to 300 s per retry), §3.1, §6; G7.*

### Health

- **K-REQ-7: Liveness, readiness and startup signals.** One cheap, lock-free status call, usable from any thread at any rate, returns three things:
  - *liveness*: every scheduler's loop counter advanced within N ms, the control thread is not wedged, and the device is not in a fatal reset;
  - *readiness*: links up, time source locked (or within its stated error), all started sessions in RUNNING;
  - *startup*: the phase reached (EAL, ports, time lock, sessions), so a startup probe can allow for slow PTP lock.

  Liveness must not depend on packets arriving or on PTP lock. The library serves no HTTP itself: the app maps the status to an httpGet or gRPC probe, which is cheaper than exec on exclusive CPUs. *Why: §1.3, §8; G5.*

### CPU

- **K-REQ-8: CPUs from the affinity mask.** With no `lcores`, the instance uses `sched_getaffinity` of the opening thread (DPDK's own default). An explicit `lcores` naming a CPU outside it fails with a named reason. Lcore IDs reported to the app are CPU IDs. MtlManager lcore arbitration is off by default inside a cpuset that CPU Manager already made exclusive. *Why: §4.*
- **K-REQ-9: Every thread pinned explicitly, in every mode.** Each scheduler is pinned to exactly one CPU, `TASKLET_THREAD` included. Non-scheduler threads go to a "housekeeping" CPU, which can be named or taken from the mask (for example the last CPU, or a shared one when the pod has a fractional request), because a pod with exclusive CPUs has no spare CPU outside its set.
  Placement is reported, along with a warning when SMT siblings of a scheduler CPU are in use by others (`full-pcpus-only` absent). *Why: §4 (no load balancing on isolated CPUs); G4.*

### Memory and devices

- **K-REQ-10: Budget against the cgroup, allocate at open.** Hugepage capacity is reported and checked against the container's hugetlb limit, not host `/sys` counters.
  All pools are faulted in at create, so an over-limit condition is an `-MTL_ENOMEM` at create, never a SIGBUS later. Memlock is checked per vfio mapping budget (`dma_entry_limit`, `RLIMIT_MEMLOCK` without `IPC_LOCK`, user-namespace caveat). *Why: §3.1, §3.2.*
- **K-REQ-11: Ports from Kubernetes.** The port spec accepts a device-plugin resource name or variable (for example `env:PCIDEVICE_INTEL_COM_E810_MEDIA_A`, taking the Nth address), plus a source IP and prefix from the IPAM result. MTL uses the VF's current admin MAC and never sets one. A helper parses Multus `network-status` (a file path from the downward API) into port specs.
  *Why: §2.1, §2.2; G6.*
- **K-REQ-12: VF capabilities are probed and reported.** At open, MTL reads what the VF allows: trust (promisc/allmulti), the MAC filter budget (18 untrusted on E810), spoofchk, Tx rate set by the PF, availability of hardware pacing/TM, and PHC readability. A session that needs more (for example the 17th multicast group on an untrusted VF) fails at create with a named reason.
  Today it would fail somewhere inside the join. *Why: §2.4.*
- **K-REQ-13: No assumptions about a VF's history.** MTL treats a VF as possibly reused and assumes nothing about it except what K-REQ-12 reads. On close it removes what it added: multicast filters, flow rules, TM. Admin state set through the PF is read only. *Why: §2.3.*
- **K-REQ-14: An explicit IOMMU policy.** No-IOMMU or PA mode is refused unless an instance option opts in by name. The IOVA mode is reported in port capabilities. Deployment docs mark no-IOMMU pods as privileged and node-trusting. *Why: §3.3; [15 §2](../15-security-and-deployment.md#2-imported-memory-and-the-iommu).*

### Time

- **K-REQ-15: Time sources that fit a pod.** `MTL_TIME_SOURCE_AUTO` in a pod means, in order: the VF-readable PHC when the node disciplines it, then `CLOCK_TAI` with a non-zero kernel offset, then nothing. The built-in PTP client runs only when named (D-85). The instance reports which source it used, who disciplines it (node or MTL), its lock state and its estimated error.
  No source needs `CAP_SYS_TIME` or `/dev/ptp*`, and MTL never adjusts a host clock. Whether the PHC is "disciplined" needs a signal the pod can see; a hook to read the PTP Operator's events, or a configured "trust the node" flag, is an open design point. *Why: §5.*

### Privileges, the manager and AF_XDP

- **K-REQ-16: A documented minimal privilege profile per backend.** It is checked at open. A vfio VF needs the plugin's device nodes, `IPC_LOCK` and optionally `SYS_NICE`;
  AF_XDP needs `NET_RAW` plus a socket or map from a node agent; the kernel backend needs nothing extra. All three must run as non-root, with a read-only root filesystem (writes only to a configurable runtime directory, none to `/tmp`, `/var/run/dpdk` or `/dev/hugepages`), under seccomp `RuntimeDefault` and without hostIPC, hostPID or hostNetwork for vfio.
  The docs give a ready pod spec and state that `IPC_LOCK` puts the pod outside the Baseline PSS. *Why: §6, §8 (status quo is privileged).*
- **K-REQ-17: MtlManager optional and pod-safe.** In a pod the default needs no manager: CPU Manager and the VF already isolate. When the manager is used (AF_XDP on shared host netdevs):
  - identity comes from `SO_PEERCRED` UID/GID, or from a per-pod socket and directory, never from PIDs;
  - every grant (lcores, queues, flows, UDP filters) is tied to the connection and released when it closes;
  - interfaces are named in a netns-independent way (host ifname or PCI address, not the client's ifindex);
  - the client survives a manager restart and re-registers.

  *Why: §6, §7.2; G3.*
- **K-REQ-18: AF_XDP program lifetime belongs to the node.** In a pod the library never attaches or detaches XDP programs. It accepts a pre-created XSK socket or map fd, received over SCM_RIGHTS, inherited, or from a pinned map path, from a node agent (MtlManager, the AF_XDP device plugin, bpfman). It reports clearly when none is available. *Why: §7.2.*

### Multi-container pods and the network

- **K-REQ-19: Multi-container and multi-process pods.** One instance per process. Two containers in one pod, each with its own VF, hugepage limit and CPUs, must not see each other through shared IPC, `/tmp` or lockfiles;
  this follows from K-REQ-5 and K-REQ-16. A sidecar MTL gateway serving app containers terminates last (KEP-753 ordering), so its bounded close (K-REQ-1) must also cover clients that never said goodbye. *Why: §1.4, §1.5, §3.1.*
- **K-REQ-20: Multicast that tolerates crashes.** IGMP reports keep answering queries and stay periodic without one (as today).
  The report interval and IGMP version are instance options. Leaves are sent on every leg on orderly close (K-REQ-2). The docs state the worst-case stale-flood window after SIGKILL (about the switch's group membership interval, 260 s by default) and tell operators to enable an IGMP querier and fast-leave on media VLANs. *Why: §7.1.*

## Sources

Kubernetes documentation, read from `github.com/kubernetes/website` (`main`, 2026-10-01) unless the URL says otherwise:

- S1 Pod lifecycle: <https://kubernetes.io/docs/concepts/workloads/pods/pod-lifecycle/> [verified]
- S2 Container lifecycle hooks: <https://kubernetes.io/docs/concepts/containers/container-lifecycle-hooks/> [verified]
- S3 Sidecar containers: <https://kubernetes.io/docs/concepts/workloads/pods/sidecar-containers/> [verified]; KEP-753: <https://github.com/kubernetes/enhancements/tree/master/keps/sig-node/753-sidecar-containers> [not re-read]
- S5 Share process namespace: <https://kubernetes.io/docs/tasks/configure-pod-container/share-process-namespace/> [verified]
- S6 Node-pressure eviction: <https://kubernetes.io/docs/concepts/scheduling-eviction/node-pressure-eviction/> [verified]
- S7 Node shutdowns: <https://kubernetes.io/docs/concepts/cluster-administration/node-shutdown/> [verified]
- S8 Probes: <https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/> [verified]
- S9 Volumes (emptyDir, hostPath): <https://kubernetes.io/docs/concepts/storage/volumes/> [verified]
- S10 Pods: <https://kubernetes.io/docs/concepts/workloads/pods/> [verified]
- S11 Hugepages: <https://kubernetes.io/docs/tasks/manage-hugepages/scheduling-hugepages/> [verified]
- S12 CPU management policies: <https://kubernetes.io/docs/tasks/administer-cluster/cpu-management-policies/> [verified]
- S13 Topology Manager: <https://kubernetes.io/docs/tasks/administer-cluster/topology-manager/> [verified]
- S14 Pod Security Standards: <https://kubernetes.io/docs/concepts/security/pod-security-standards/> [verified]
- S15 User namespaces: <https://kubernetes.io/docs/concepts/workloads/pods/user-namespaces/> [verified]

SR-IOV:

- S16 SR-IOV network device plugin: <https://github.com/k8snetworkplumbingwg/sriov-network-device-plugin> (README, `docs/dpdk/README.md`, `docs/dpdk/README-virt.md`) [verified]
- S17 sriov-cni: <https://github.com/k8snetworkplumbingwg/sriov-cni> (`pkg/cnicommands/cni.go`, `pkg/sriov/sriov.go`, `pkg/config/config.go`, `docs/configuration-reference.md`) [verified]
- S18 SR-IOV Network Operator: <https://github.com/k8snetworkplumbingwg/sriov-network-operator> (`api/v1/sriovnetwork_types.go`, `doc/advanced-features.md`) [verified]

Linux kernel (`torvalds/linux` master):

- S19 vfio: `drivers/vfio/pci/vfio_pci_core.c`, `drivers/vfio/vfio_iommu_type1.c`, `drivers/vfio/group.c` [verified]
- S20 ice: `drivers/net/ethernet/intel/ice/ice_vf_lib.c`, `virt/virtchnl.c`, `virt/virtchnl.h` [verified]
- S21 iavf PTP: `drivers/net/ethernet/intel/iavf/iavf_ptp.c` [verified]; DPDK `drivers/net/intel/iavf/iavf_ethdev.c` [verified]
- S22 `net/xdp/xsk.c`, `kernel/time/namespace.c` [verified]

DPDK, linuxptp and XDP:

- S23 DPDK source: `lib/eal/common/eal_common_options.c`, `lib/eal/linux/eal_memalloc.c` (<https://github.com/DPDK/dpdk>, main) [verified]
- S24 DPDK guides: <https://doc.dpdk.org/guides/linux_gsg/linux_drivers.html>, <https://doc.dpdk.org/guides/linux_gsg/linux_eal_parameters.html>, <https://doc.dpdk.org/guides/linux_gsg/enable_func.html> [verified]
- S25 linuxptp: `phc2sys.c`, `clockadj.c` (<https://github.com/richardcochran/linuxptp>) [verified]
- S26 libxdp README: <https://github.com/xdp-project/xdp-tools/blob/main/lib/libxdp/README.org> [verified]
- S27 AF_XDP plugins for Kubernetes: <https://github.com/intel/afxdp-plugins-for-kubernetes> [verified]

Vendor and platform guides:

- S28 OpenShift 4.16 Scalability and performance: <https://docs.redhat.com/en/documentation/openshift_container_platform/4.16/HTML-single/scalability_and_performance/index> [verified, text extracted]
- S29 NVIDIA Holoscan for Media: <https://docs.nvidia.com/holoscan-for-media/latest/user-guide/app-development/index.html>, `…/platform-setup/prod-manual/openshift-cluster-tuning.html`, `…/openshift-cluster-configuration.html` [verified]
- S30 Media Communications Mesh: <https://github.com/OpenVisualCloud/Media-Communications-Mesh/tree/main/deployment> (`DaemonSet/media-proxy*.yaml`, `pv.yaml`) [verified]
- S31 RFC 3376 (IGMPv3) timer defaults: <https://www.rfc-editor.org/rfc/rfc3376> [inferred, not re-read]
- S32 OpenShift PTP hardware chapter: <https://docs.redhat.com/en/documentation/openshift_container_platform/4.16/HTML/networking/using-ptp-hardware> [headings only]
