# CPU isolation wrapper

`isolate.sh [--] <command...>` runs one command at nice −20 inside a cgroup v2
cpuset partition that exists only while the command runs. The NoCtx runner
(`tests/integration_tests/noctx/run.sh`) uses it for its strict timing cases.
Background on static and dynamic CPU isolation: [doc/isolation.md](../../../doc/isolation.md).

## Environment

| Variable | Set by | Values |
|---|---|---|
| `MTL_ISOLATE` | caller | `off` (default) runs the command as is; `try`; `require` |
| `MTL_ISOLATE_PORTS` | caller | Comma-separated PCI BDFs, the MTL primary port (port P) first |
| `MTL_CPU_ISOLATION` | wrapper, for the command | `exclusive` or `none` |

If the partition is unavailable, `try` prints one `WARNING` with the reason and
runs the command unconfined; `require` prints `isolate.sh: ERROR ... (<why>)`
and exits 1 without running it. Otherwise the command's exit status is the
wrapper's.

## Requirements

- Root.
- cgroup v2 with the `cpuset` controller in `/sys/fs/cgroup/cgroup.subtree_control`.
- The `isolated` value of `cpuset.cpus.partition`, Linux 6.1 or later.
  `cpuset.cpus.exclusive` (Linux 6.7) is written when present. Validated on 6.8.
- No root-level sibling cgroup with an explicit overlapping `cpuset.cpus`,
  such as k3s's `kubepods.slice` (`AllowedCPUs=0-255`): the kernel then
  refuses the partition. `isolate.sh` never changes other cgroups to make room.
- At least 6 physical cores on each port's NUMA node.

## What it does

- **Partition.** Creates `/sys/fs/cgroup/mtl-isolate-<pid>` with
  `cpuset.cpus` = the chosen CPUs (and `cpuset.cpus.exclusive`),
  `cpuset.cpus.partition` = `isolated`, and reads the state back. Those CPUs
  leave every other cgroup and load balancing, so no foreign task (containers,
  pods) runs there. The cgroup gets `cgroup.pressure=0`, which stops its PSI
  averaging.
- **CPUs.** For each distinct NUMA node of the ports: the 4 highest-numbered
  physical cores plus their SMT siblings, never the node's first 2 cores. The
  partition is the union, and `cpuset.mems` is the ports' nodes. MTL binds each
  scheduler to its port's node, so every port node needs CPUs. Setup fails if a
  port's node cannot be resolved or has too few cores.
- **Port P.** EAL's main lcore is the partition's first CPU, and `mtl_init()`
  binds the main thread to port P's node (`numa_bind`). Setup fails unless that
  CPU is on port P's node, i.e. port P's node must hold the partition's
  lowest-numbered CPU. Do not pass `--lcores`: EAL then pins its main lcore to CPU 0,
  outside the cgroup.
- **Start CPU.** The command starts on the partition's first CPU, EAL's main
  lcore, which runs no MTL scheduler.
- **Housekeeping, exclusive mode only.** IRQ affinities
  (`/proc/irq/*/smp_affinity_list`) are moved off the partition where the
  kernel allows it, `/proc/irq/default_smp_affinity` excludes it (VF MSI-X
  vectors are only allocated when the ports open), and `vm.stat_interval` is
  raised to 120 s. The originals are written to
  `/run/mtl-isolate-<pid>.restore` first and restored on exit.
- **Host impact.** While the command runs, the partition's CPUs are taken from
  all other workloads, movable IRQs run only on the other CPUs, and system-wide
  VM counters fold per-CPU deltas only every 120 s.

## Cleanup and sweep

On exit the wrapper terminates what is left in its cgroup, removes it and
restores the saved settings. If the wrapper is SIGKILLed, a watcher in its own
session does this, so a kill of the wrapper's session or process group does not
reach it, and it ignores SIGPIPE in case its stderr pipe is already gone. If
both die, the next wrapper or `isolate.sh --sweep` (root) does; it exits 1 if
a partition is left. `/run` is tmpfs, so a reboot also resets the settings. Wrappers and sweeps serialize on
`flock /run/mtl-isolate.lock` (a wrapper waits 10 s, then gives up), and remove
the stale `mtl-isolate-*` cgroups of dead wrappers.

## Limits

- An isolated partition does no load balancing, so a thread runs where it
  starts: every unpinned thread shares the partition's first CPU.
- The app's main thread is placed only by the wrapper's start CPU, because EAL
  init runs in its own pthread.
- The 4th scheduler on a node lands on the SMT sibling of the main CPU.
- Not covered without boot parameters: the timer tick (`nohz_full=`), RCU
  callbacks and expedited grace periods (`rcu_nocbs=`), per-CPU work queued on
  the CPU that triggered it, and managed or per-CPU IRQs.
- RL video launches packet 0 from the TX tasklet (`WAIT_TARGET`), so outside a
  partition a late tasklet sends it late (tens of µs at high frame rates).
