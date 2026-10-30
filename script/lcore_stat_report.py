#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Sums up a lcore_stat_dump.sh run: per CPU, every task that ran besides the MTL scheduler
threads (count, total, p99, max), and the hard irq, softirq and irq vector time. Writes
DIR/timeline.csv. Times are in us, the resolution of the ftrace text output. With
--isolation, reports how isolated the CPUs are; it only reads.

usage: lcore_stat_report.py DIR
       lcore_stat_report.py --isolation CPUS"""

import collections
import csv
import os
import re
import sys

LINE = re.compile(
    r"^\s*(.+)-(\d+)\s+\[(\d+)\]\s+\S+\s+(\d+)\.(\d{6}):\s+(\w+):\s*(.*)$"
)
FIELD = re.compile(r"(\w+)=(\S+)")


def cgroup(pid):
    """the cgroup of a live task, a docker container by its short id"""
    try:
        with open("/proc/%d/cgroup" % pid) as f:
            path = f.read().strip().split(":")[-1]
    except OSError:
        return ""
    m = re.search(r"docker-([0-9a-f]{12})", path)
    return "docker:" + m.group(1) if m else path.rsplit("/", 1)[-1]


def who(comm):
    """kworker/65:1 -> kworker, Erlang's 66_scheduler -> N_scheduler"""
    return re.sub(r"^\d+_", "N_", comm.split("/")[0])


def cpu_list(text):
    """65-72,80 -> {65, ..., 72, 80}; also the kernel's all, N and 0-63:1/2 forms"""
    last = int(read("/sys/devices/system/cpu/possible").split("-")[-1])
    cpus = set()
    for part in text.replace("\n", "").split(","):
        part = part.strip().replace("N", str(last))
        if part == "all":
            part = "0-%d" % last
        if not part:
            continue
        rng, _, stride = part.partition(":")
        lo, _, hi = rng.partition("-")
        use, _, group = stride.partition("/")
        group = int(group or 1)
        for c in range(int(lo), int(hi or lo) + 1):
            if (c - int(lo)) % group < int(use or group):
                cpus.add(c)
    return cpus


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def sysfs_cpus(path):
    text = read(path)
    return cpu_list(text) if text and text != "(null)" else set()


def isolation(cpus):
    """isolcpus/nohz_full/rcu_nocbs, cpuset partitions, IRQ affinity, numa balancing, SMT"""
    print("isolation of cpus %s" % ",".join(map(str, sorted(cpus))))
    cmdline = dict(a.split("=", 1) for a in read("/proc/cmdline").split() if "=" in a)
    have = {
        # the scheduler domain isolation, whatever the isolcpus flags
        "isolcpus": sysfs_cpus("/sys/devices/system/cpu/isolated"),
        "nohz_full": sysfs_cpus("/sys/devices/system/cpu/nohz_full"),
        # nohz_full CPUs offload their RCU callbacks too
        "rcu_nocbs": cpu_list(cmdline.get("rcu_nocbs", ""))
        | sysfs_cpus("/sys/devices/system/cpu/nohz_full"),
    }
    for key, isolated in have.items():
        missing = sorted(cpus - isolated)
        state = "missing %s" % missing if missing else "all"
        print("  %-10s %-40s cmdline: %s" % (key, state, cmdline.get(key, "-")))

    print(
        "  cgroup v2 cpusets (non-root cgroups with an explicit cpuset.cpus over them):"
    )
    root = "/sys/fs/cgroup"
    covered = set()
    for d, _, _ in os.walk(root):
        explicit = read(os.path.join(d, "cpuset.cpus"))
        if d == root or not explicit or not cpus & cpu_list(explicit):
            continue
        part = read(os.path.join(d, "cpuset.cpus.partition"))
        if part.startswith(("isolated", "root")) and "invalid" not in part:
            covered |= cpus & cpu_list(read(os.path.join(d, "cpuset.cpus.effective")))
        print("    %-40s cpus %-12s %s" % (d[len(root) :], explicit, part))
    print("  valid partition (isolated/root) covers: %s" % (sorted(covered) or "none"))

    print("  IRQs whose effective affinity includes them (count on them since boot):")
    lines = open("/proc/interrupts").read().splitlines()
    col = {int(c[3:]): i for i, c in enumerate(lines[0].split()) if c.startswith("CPU")}
    fired, silent = [], 0
    for line in lines[1:]:
        f = line.split()
        irq = f[0].rstrip(":")
        aff = (
            read("/proc/irq/%s/effective_affinity_list" % irq) if irq.isdigit() else ""
        )
        if not aff or not cpus & cpu_list(aff):
            continue
        n = sum(int(f[1 + col[c]]) for c in cpus if c in col)
        if n:
            fired.append("%s %s -> %s: %d" % (irq, " ".join(f[len(col) + 1 :]), aff, n))
        else:
            silent += 1
    for line in fired or ["none fired"]:
        print("    " + line)
    if silent:
        print("    and %d more that have not fired" % silent)
    print("  default_smp_affinity: %s" % read("/proc/irq/default_smp_affinity"))
    print("  kernel.numa_balancing: %s" % read("/proc/sys/kernel/numa_balancing"))

    print("  SMT siblings outside the set:")
    sib = []
    for c in sorted(cpus):
        path = "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list" % c
        others = sysfs_cpus(path) - cpus
        if others:
            sib.append("%d:%s" % (c, ",".join(map(str, sorted(others)))))
    print("    " + (" ".join(sib) or "none"))


def p99(v):
    v = sorted(v)
    return v[min(len(v) - 1, int(0.99 * len(v)))]


def parse(meta, mtl):
    """per CPU log -> task runs (start, cpu, comm, pid, dur, preempted), irq times, tlb flushes"""
    runs, irqs, tlb = [], collections.defaultdict(list), collections.Counter()
    for cpu in map(int, meta["cpus"].strip('"').split()):
        on = None  # us the running task started
        open_irq = {}  # what -> (start us, name)
        events = unknown = 0
        for line in open(os.path.join(meta["log_dir"], "lcore_status_cpu%d.log" % cpu)):
            m = LINE.match(line)
            if not m:
                unknown += not line.startswith("#")
                continue
            events += 1
            us, ev, f = (
                int(m.group(4)) * 1000000 + int(m.group(5)),
                m.group(6),
                m.group(7),
            )
            if ev == "sched_switch":
                a = dict(FIELD.findall(f.replace(" ==> ", " ")))
                pid = int(a["prev_pid"])
                busy = pid in mtl or a["prev_comm"].startswith("mtl_sch")
                if on is not None and not busy:
                    preempted = a["prev_state"].startswith("R")
                    runs.append((on, cpu, a["prev_comm"], pid, us - on, preempted))
                on = us
            elif ev == "tlb_flush":
                r = re.search(r"reason:(.*?) \(", f)
                tlb[(cpu, r.group(1) if r else f)] += 1
            elif ev.endswith(("_entry", "_exit")):
                a = dict(FIELD.findall(f))
                base = ev.rsplit("_", 1)[0]
                if base == "irq_handler":
                    key = "irq" + a.get("irq", "")
                    name = "%s:%s" % (key, a.get("name", ""))
                elif base == "softirq":
                    key = "softirq" + a.get("vec", "")
                    name = "softirq:" + f.split("action=")[-1].rstrip("]")
                else:
                    key = name = base
                if ev.endswith("_entry"):
                    open_irq[key] = (us, name)
                elif key in open_irq:
                    t0, name = open_irq.pop(key)
                    irqs[(cpu, name)].append(us - t0)
                    runs.append((t0, cpu, name, -1, us - t0, False))
        if unknown and not events:
            print("cpu %d: no line in the expected trace format" % cpu, file=sys.stderr)
    return runs, irqs, tlb


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == "--isolation":
        isolation(cpu_list(sys.argv[2]))
        return
    d = sys.argv[1]
    with open(os.path.join(d, "meta.env")) as f:
        meta = dict(line.strip().split("=", 1) for line in f if "=" in line)
    off = meta["realtime_minus_clock_ns"]
    off = int(off) // 1000 if off else None
    with open(os.path.join(d, "mtl_threads.txt")) as f:
        mtl = {int(line.split()[0]) for line in f if line.strip()}
    runs, irqs, tlb = parse(meta, mtl)
    cg = {pid: cgroup(pid) for _, _, _, pid, _, _ in runs if pid > 0}

    with open(os.path.join(d, "timeline.csv"), "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(
            ["kind", "start_us", "start_realtime_us", "cpu", "comm", "pid", "cgroup"]
            + ["dur_us", "preempted"]
        )
        for t, cpu, comm, pid, dur, preempted in sorted(runs):
            kind = "irq" if pid < 0 else ("idle" if pid == 0 else "run")
            real = t + off if off is not None else ""
            out.writerow(
                [
                    kind,
                    t,
                    real,
                    cpu,
                    comm,
                    pid,
                    cg.get(pid, ""),
                    dur,
                    int(preempted and pid > 0),
                ]
            )

    print(
        "lcore_stat_dump: cpus %s, %s s, clock %s"
        % (meta["cpus"].strip('"'), meta["time_s"], meta["clock"])
    )
    print("\nTasks besides mtl_sch_* (idle: the CPU idle while the MTL thread slept)")
    print(
        "%4s  %-20s %-18s %7s %10s %8s %8s %8s"
        % ("cpu", "who", "cgroup", "count", "total_us", "p99_us", "max_us", "preempt")
    )
    g = collections.defaultdict(list)
    for _, cpu, comm, pid, dur, preempted in runs:
        if pid >= 0:
            name = "idle" if pid == 0 else who(comm)
            g[(cpu, name, cg.get(pid, ""))].append((dur, preempted))
    for k in sorted(g, key=lambda k: (k[0], -sum(x[0] for x in g[k]))):
        v = [x[0] for x in g[k]]
        pre = (
            "-"
            if k[1] == "idle"
            else "%d%%" % (100 * sum(x[1] for x in g[k]) // len(v))
        )
        print(
            "%4d  %-20s %-18s %7d %10d %8d %8d %8s"
            % (k[0], k[1][:20], k[2][:18], len(v), sum(v), p99(v), max(v), pre)
        )
    # a task's run time includes the irqs inside it, a softirq's the hard irqs inside it
    print("\nIRQ, softirq and irq vector time (also inside the task times above)")
    print(
        "%4s  %-28s %7s %10s %8s %8s"
        % ("cpu", "what", "count", "total_us", "p99_us", "max_us")
    )
    for k in sorted(irqs, key=lambda k: (k[0], -sum(irqs[k]))):
        v = irqs[k]
        print(
            "%4d  %-28s %7d %10d %8d %8d"
            % (k[0], k[1][:28], len(v), sum(v), p99(v), max(v))
        )
    if tlb:
        print("\ntlb_flush")
        for (c, r), n in sorted(tlb.items()):
            print("%4d  %-28s %7d" % (c, r, n))


if __name__ == "__main__":
    main()
