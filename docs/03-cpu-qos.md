# 3. CPU QoS configuration — static CPU Manager, Topology Manager and RDT

> **Troubleshooting first:** If the script stalls or fails, see
> [If it stops at stage 1/4](#if-it-stops-at-stage-14) below. If this network also
> needs an outbound proxy, set `LAB_HTTP_PROXY` and `LAB_HTTPS_PROXY` in
> `config/lab.env` before running this chapter; scripts build `NO_PROXY`
> automatically.

This chapter enables CPU and NUMA placement behavior that the `pinned` scenario
depends on, and keeps Intel RDT measurement/control meaningful.

## What each mechanism does

### Kubernetes CPU Manager

CPU Manager is a kubelet feature that decides which CPUs each container may run
on. With the default `none` policy, CPU requests are enforced as CFS quota/weight
in a shared pool. With `static`, a container in a **Guaranteed** Pod receives an
exclusive cpuset when its CPU request is an integer (`1`, `5`, ...), with
request=limit.

In this recipe, decoder (`1`) and encoder (`5`) containers in `pinned` are those
integer Guaranteed containers, so they get exclusive CPUs.

`cpuManagerPolicyOptions` tighten this further:

- `full-pcpus-only: "true"` allocates whole physical cores, not individual SMT
  siblings.
- `strict-cpu-reservation: "true"` keeps Burstable/BestEffort work off
  `reservedSystemCPUs` instead of letting them share those CPUs.

### Kubernetes Topology Manager

Topology Manager coordinates NUMA-aware admission across CPU and memory providers.
NUMA alignment here means: for each container, the CPUs it gets and the memory it
will use must be satisfiable from one NUMA node.

- `topologyManagerPolicy: single-numa-node` admits a container only when one NUMA
  node can satisfy the request.
- `topologyManagerScope: container` makes that decision per container (decoder and
  encoder independently), not once for the whole Pod.

That is why the pinned workload can keep decoder and encoder allocations NUMA-local
without manual `taskset` pinning.

### Intel Resource Director Technology (RDT)

Intel RDT controls shared-resource QoS (LLC cache ways and memory bandwidth) and
exposes usage counters through Linux `resctrl`.

This recipe uses RDT in later chapters to:

- **monitor** LLC occupancy and memory bandwidth per workload/noise cgroup, and
- **control** noisy-neighbor impact with CAT/MBA policies.

RDT is separate from CPU Manager and Topology Manager: CPU Manager/Topology Manager
choose CPU and NUMA placement; RDT controls shared cache/bandwidth behavior of the
already-placed workloads.

## How they interact in this recipe

| Scenario | Depends on CPU Manager static | Depends on Topology Manager single-numa-node | Depends on Intel RDT |
|---|---|---|---|
| `baseline` | no | no | optional (monitoring only) |
| `numa-pool` | no (uses Burstable + runner `taskset`) | no | optional (monitoring/control in noisy-neighbor work) |
| `pinned` | **yes** (exclusive integer-CPU Guaranteed containers) | **yes** (NUMA-local admission) | optional for chapter 9 density, required for chapter 12 isolation control |

`numa-pool` intentionally remains a shared-pool, taskset-guided comparison case.
`pinned` is the Kubernetes-native static CPU Manager path and does **not** use
taskset for container CPU ownership.

## Why this chapter edits `/var/lib/kubelet/config.yaml`

`/var/lib/kubelet/config.yaml` is the kubelet's **node-local** configuration file
on each worker. The kubelet process reads it at startup and applies those settings
locally; the Kubernetes API server does not directly store this file.

`scripts/configure-cpu-qos.sh` edits that file on the target worker because these
CPU/Topology Manager settings are kubelet configuration, not Pod spec fields. It
writes a backup (`config.yaml.pre-cpu-qos`) first so the previous node-local state
can be restored if needed. A kubelet restart is required because the kubelet loads
this file at process start.

Verification reads `/api/v1/nodes/<node>/proxy/configz` to confirm the **live**
kubelet runtime config, not just the file on disk. That catches cases where the file
was edited but the running kubelet has not loaded the change.

## What `scripts/configure-cpu-qos.sh` does (before you run it)

Before it asks you to run commands, the script workflow is:

1. Load lab inventory/configuration from `config/lab.env` and `config/nodes.env`.
2. Resolve the target worker, read `Thread(s) per core` with remote `lscpu`, and
   fail if it disagrees with `LAB_THREADS_PER_CORE` in `config/lab.env` — an
   undetected mismatch either makes `full-pcpus-only` reject every pinned Pod
   with `SMTAlignmentError` or silently hands out half the cores the scenario
   claims.
3. Check Kubernetes API reachability (`kubectl cluster-info`) before any disruption.
4. Confirm the target node exists and is `Ready`.
5. Cordon and drain the worker (disruptive step).
6. Back up and update `/var/lib/kubelet/config.yaml` with static CPU Manager and
   Topology Manager settings.
7. Remove stale `/var/lib/kubelet/cpu_manager_state` and
   `/var/lib/kubelet/memory_manager_state`.
8. Restart kubelet.
9. Wait for `Ready`, uncordon the node, and verify live kubelet config through the
   API server.

Applying this script is disruptive and must not run during measurements.
`scripts/configure-cpu-qos.sh --verify` is read-only and does not cordon, drain, or
restart kubelet.

## Required: Configure DUT Congifuration 

For density benchmarking and QoS testing the CPU topology must be documented within `config/lab.env`.
This is only required for benchmarking purposes only

```bash
LAB_CONTROLLER=control-plane
LAB_WORKERS=worker-1            # comma-separated if you have several
LAB_DEFAULT_NODE=worker-1       # the worker runs get measured on
LAB_SOCKET_COUNT=2         # sockets on the worker (2 is required, see below)
LAB_CORES_PER_SOCKET=64    # physical cores per socket
LAB_SOCKET0_PARITY=even    # see "CPU numbering" below
LAB_RESERVED_CPUS=0-3      # kept away from the workload
```

Read the socket and core counts off the worker with `lscpu`:

```bash
ssh <user>@<worker> "lscpu | grep -E 'Socket|Core|Thread|NUMA node\('"
```

Expect `Thread(s) per core: 1` and `NUMA node(s)` equal to `Socket(s)`. If not,
Hyper-Threading or Sub-NUMA Clustering is still on — go back to
[01-bios-bkc.md](01-bios-bkc.md).

**CPU numbering.** Check how your BIOS enumerates CPUs, because the CPU-pool
planner depends on it:

```bash
ssh <user>@<worker> "lscpu -p=CPU,NODE | grep -v '^#' | head -4"
```

The expected output is CPU 0 and 2 on NUMA node 0, CPU 1 and 3 on node 1: the
IDs alternate between sockets, so socket 0 owns the even IDs and
`LAB_SOCKET0_PARITY=even` is correct. If the alternation runs the other way
(CPU 0 on node 1), set `LAB_SOCKET0_PARITY=odd`.

If instead the IDs come in contiguous blocks — the whole first half on node 0 —
the planner does not model your machine: it assumes alternating IDs and
2 sockets, and it will place `numa-pool` sessions on the wrong socket *silently*,
because the `taskset` call itself still succeeds. Confirm the layout with
`numactl --hardware`, and treat the `numa-pool` scenario as unsupported until
`cpu_pools()` in [python/mxlperf/render.py](../python/mxlperf/render.py) is
extended. `baseline` and `pinned` do not depend on the parity model.

**Reserved CPUs.** `LAB_RESERVED_CPUS` is the set kept for the kubelet,
containerd, the metrics exporters and the RDT helper. Reserve at least two CPUs
per socket: with alternating IDs, `0-3` does exactly that. Set it once here —
[03-cpu-qos.md](03-cpu-qos.md) writes the same value into the kubelet's
`reservedSystemCPUs`, and the PCM exporter and the profiling server are pinned to
it as well.

## Apply it

```bash
scripts/configure-cpu-qos.sh              # LAB_DEFAULT_NODE
scripts/configure-cpu-qos.sh <node>       # a specific worker
scripts/configure-cpu-qos.sh --verify     # read back only, change nothing
```

Applying restarts the kubelet, so the node is cordoned and drained first — never
run it during a measurement. The worker's `sudo` password is typed into the
worker's own prompt; the script never stores or forwards it. The previous kubelet
config is kept as `/var/lib/kubelet/config.yaml.pre-cpu-qos`.

Four stages: cordon + drain → write the config → wait for `Ready` and uncordon →
verify through the API server.

## Hyper-Threading

Both settings work. What must not happen is a disagreement between the hardware
and `config/lab.env`, so the script reads `Thread(s) per core` off the worker and
refuses to continue unless it equals `LAB_THREADS_PER_CORE`.

The reason is that `DEC_CORES` and `ENC_CORES` count **physical cores**, while a
CPU request counts **logical CPUs**. `full-pcpus-only` admits a Guaranteed
container only when its request is a whole multiple of the threads per core, so
the request the kubelet sees is `cores × LAB_THREADS_PER_CORE`:

| `LAB_THREADS_PER_CORE` | `ENC_CORES=5` becomes | Encoder owns |
| --- | --- | --- |
| 1 (HT off) | `cpu: "5"` | 5 physical cores |
| 2 (HT on) | `cpu: "10"` | 5 physical cores, both threads of each |

The physical footprint of a scenario is the same either way, but the documented
numbers were measured with Hyper-Threading off — `Disabled` is the reference state
in [01-bios-bkc.md](01-bios-bkc.md), so treat an HT-on run as its own baseline
rather than a comparison against them. Get the value wrong in the other
direction — `LAB_THREADS_PER_CORE=1` on an SMT host — and the odd request of `5`
is rejected for every Pod with `SMTAlignmentError`. Do not weaken the CPU Manager
policy to work around that, because it changes the benchmark.

With SMT on, `LAB_RESERVED_CPUS` must also cover **whole** cores. The sibling of
CPU *N* is CPU *N + sockets × cores_per_socket*, and `lscpu -p=CPU,CORE` lists the
pairs. Reserving one thread of a core strands the other: `strict-cpu-reservation`
keeps the workload off the reserved thread, and `full-pcpus-only` will not hand
out a core whose sibling is busy — so the core is lost entirely rather than
half-used. Both this script and `scripts/preflight.sh` refuse such a list.

## If it stops at stage 1/4

```
== 1/4 draining worker-1 ==
Unable to connect to the server: Forbidden
```

This is not the node, the kubelet or a permission problem: it is an HTTP proxy in
the shell you typed the command in. `kubectl` honours `http_proxy`, your kubeconfig
points at the control plane on port 6443, and a proxy asked to `CONNECT` there
answers 403. Every script here excludes the cluster's own addresses from the proxy
for its own process, so on a current checkout this does not happen — if you see it,
`git pull`. The full explanation, and how to fix your own shell so that a `kubectl`
you type by hand works too, is in
[02-kubernetes-install-troubleshooting.md § Symptom: API server answers `Forbidden`](02-kubernetes-install-troubleshooting.md#symptom-api-server-answers-forbidden).

Before it drains anything the script now checks the two things that make draining
impossible, and names the chapter to go back to:

```
FATAL: cannot reach the API server. kubectl says:
FATAL: node worker-1 is not in the cluster. It has these nodes:
FATAL: node worker-1 is in the cluster but not Ready (Ready=False), so it
       cannot be drained.
```

All three mean chapter 2 is not finished. `kubectl get nodes` showing every node
`Ready` is the condition for starting this one.

## What it writes

Into `/var/lib/kubelet/config.yaml` on the worker:

```yaml
cpuManagerPolicy: static
cpuManagerPolicyOptions:
  full-pcpus-only: "true"
  strict-cpu-reservation: "true"
cpuManagerReconcilePeriod: 10s
reservedSystemCPUs: 0-3
topologyManagerPolicy: single-numa-node
topologyManagerScope: container
cgroupDriver: systemd
cgroupsPerQOS: true
```

| Setting | What it does | Why it matters here |
|---|---|---|
| `cpuManagerPolicy: static` | A container in a Guaranteed Pod whose CPU request is a whole number gets those CPUs exclusively; nothing else is scheduled on them. | This is what "pinned" means. Without it, CPU allocation is no different from Burstable. |
| `full-pcpus-only: "true"` | Allocations are whole physical cores, never single SMT siblings. | Ensures that allocated CPUs are not split across hyperthreads — see [Hyper-Threading](#hyper-threading) for what that means with SMT on. |
| `strict-cpu-reservation: "true"` | Burstable and BestEffort Pods are kept *off* `reservedSystemCPUs` instead of merely sharing them. | Stops system daemons and lab Pods from leaking onto each other. |
| `reservedSystemCPUs: 0-3` | Reserves CPUs on each socket for the kubelet, containerd, and system services. | Reserved CPUs must not contend with workload cores. |
| `topologyManagerPolicy: single-numa-node` | A container is admitted only if its CPUs *and* its memory can come from one NUMA node; otherwise it is rejected. | This is what removes cross-socket traffic overhead. |
| `topologyManagerScope: container` | Alignment is decided per container, not per Pod. | Allows containers of different sizes to be independently NUMA-aligned. |
| `cgroupDriver: systemd` + `cgroupsPerQOS` | Matches containerd's `SystemdCgroup = true`. | With mismatched drivers cpuset writes are unreliable and exclusivity silently fails. |
| `cpuManagerReconcilePeriod: 10s` | How often the kubelet re-applies cpusets. | Bounds how long a stale cpuset can survive after a container restart mid-run. |

The planner mirrors `full-pcpus-only` in `LAB_FULL_PCPUS_ONLY` (`config/lab.env`);
leave it `true` so `mxlperf` sizes CPU requests the way the kubelet admits them.

The script also deletes `/var/lib/kubelet/cpu_manager_state` and
`memory_manager_state` before restarting the kubelet. A policy change is ignored
while the old state file exists — this is the single most common reason "I set
static and nothing happened".

## Verify

```bash
scripts/configure-cpu-qos.sh --verify <node>
```

It reads the *live* configuration through the API server
(`/api/v1/nodes/<node>/proxy/configz`), not the file on disk, so it cannot be
fooled by an edit that the kubelet never loaded.

## The three cases this enables

| Scenario | Kubernetes QoS class | CPU request | Placement |
|---|---|---|---|
| `baseline` | Burstable (`500m`) | fractional | shared pool, scheduler and CFS decide everything |
| `numa-pool` | Burstable (`500m`) | fractional | shared pool, with socket-level CPU affinity for session placement |
| `pinned` | **Guaranteed** (`1` decoder, `5` encoder) | whole numbers | exclusive cores, NUMA-aligned by the Topology Manager |

Only `pinned` needs this package. `baseline` and `numa-pool` run on any cluster —
which is precisely why they are the comparison. CPU placement in `pinned` is
performed by the Kubernetes CPU Manager and Topology Manager; pods receive
exclusively allocated whole cores via the standard Guaranteed QoS mechanism.

Next: [04-perfspect-baseline.md](04-perfspect-baseline.md).
