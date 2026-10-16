# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Two isolated MTL instances side by side: placement, USDT collection, comparison.

The design, the isolation and the metrics are described in doc/perf_compare.md.
"""

from __future__ import annotations

import base64
import logging
import os
import re
import shlex
import statistics
import time
from dataclasses import dataclass, field

from mtl_engine.const import LD_LIB_PATH_REL

logger = logging.getLogger(__name__)

UNIT_PREFIX = "mtl-perf-"
RESCTRL = "/sys/fs/resctrl"
RDT_PREFIX = "mtl_perf_"
RDT_SAVE = "/run/mtl-perf-compare.schemata"
# The collectors run on an OS core, outside both slots (cpu_layout leaves cores 0 and 1).
COLLECTOR_CPU = 1

# An explicit newline: BPFTRACE_STRLEN cuts a long message before its own.
LOG_SCRIPT = 'usdt::sys:log_msg { printf("%llu L %s\\n", nsecs, str(arg1)); }'
TRACE_SCRIPT = "usdt::sys:tasklet_time_measure {}"
SAMPLE_SCRIPT = "usdt::sys:tasklet_time_sample {}"
STAT_SCRIPT = (
    'usdt::sys:tasklet_stat { printf("%llu T %d %s %llu %llu %llu %llu\\n", '
    "nsecs, arg0, str(arg1), arg2, arg3, arg4, arg5); }"
)

# Tasklets whose work is the data path of a direction.
WORK_TASKLETS = {
    "RX": ("rvs_pkt_rx",),
    "TX": ("tx_video_sessions_mgr", "video_transmitter"),
}
STEADY_FPS = 0.99
DIRECTION = {"rx": "RX", "tx": "TX"}
DUMP_PERIOD_S = 10  # MT_STAT_INTERVAL_S_DEFAULT
POLL_S = 5
# Enough lines of a collector log to hold the last complete dump of 64 sessions.
TAIL_LINES = 3000

DUMP_START = "M T    D E V   S T A T E"
DUMP_END = "E N D    S T A T E"
RECORD = re.compile(r"^\d+ [LT] ")
RE_SCH = re.compile(
    r"SCH\((\d+):[^)]*\): tasklets \d+, lcore \d+\(t_pid: (\d+)\), avg loop (\d+) ns"
)
RE_TASKLET = re.compile(r"SCH\((\d+),\d+\): tasklet ([^,]+), avg ([\d.]+)us")
RE_FPS = re.compile(r"(RX|TX)_VIDEO_SESSION\((\d+),(\d+):[^)]*\): fps ([\d.]+)")
RE_BUSY = re.compile(
    r"(RX|TX)_VIDEO_SESSION\((\d+),(\d+)\): throughput .*cpu busy ([\d.]+)"
)
RE_DEV = re.compile(
    r"DEV\(\d+\): Avr rate, tx: ([\d.]+) Mb/s, rx: ([\d.]+) Mb/s, pkts, tx: (\d+), rx: (\d+)"
)
RE_INCOMPLETE = re.compile(r"RX_VIDEO_SESSION\(\d+,\d+\): incomplete frames (\d+)")
RE_HW_DROP = re.compile(r"DEV\(\d+\): Status: rx_hw_dropped_packets (\d+)")


def sh(host, script: str, check: bool = True) -> str:
    """Run *script* as root on *host* and return its output."""
    res = host.connection.execute_command(
        f"sudo -n bash -c {shlex.quote(script)}",
        shell=True,
        stderr_to_stdout=True,
        expected_return_codes=None,
    )
    if check and res.return_code != 0:
        raise RuntimeError(
            f"{host.name}: rc {res.return_code} from {script!r}: {res.stdout}"
        )
    return res.stdout or ""


def stream(host, script: str) -> str:
    """Output of *script*, read while it runs.

    mfd_connect returns the output of execute_command only at the end, and the
    command stalls once that output fills the SSH channel window.
    """
    proc = host.connection.start_process(
        f"sudo -n bash -c {shlex.quote(script)}", shell=True
    )
    return "".join(proc.get_stdout_iter())


def write_file(host, path: str, text: str) -> None:
    """Write *text* to *path* on *host*, untouched by any quoting layer."""
    sh(host, f"echo {base64.b64encode(text.encode()).decode()} | base64 -d > {path}")


def steady_timeout(sessions: int) -> int:
    """Seconds an instance may take to carry all its sessions."""
    return 120 + 3 * sessions  # the E830 rate limiter takes ~2 s per TX queue


def parse_cpus(text: str) -> list[int]:
    cpus = []
    for part in filter(None, text.strip().split(",")):
        lo, _, hi = part.partition("-")
        cpus.extend(range(int(lo), int(hi or lo) + 1))
    return cpus


def format_cpus(cpus) -> str:
    ranges: list[list[int]] = []
    for c in sorted(cpus):
        if ranges and c == ranges[-1][1] + 1:
            ranges[-1][1] = c
        else:
            ranges.append([c, c])
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in ranges)


@dataclass
class Slot:
    """The CPUs of one slot on one host.

    EAL's main lcore is cpus[0]; the schedulers take others, and the
    application and control threads the ones they leave.
    """

    idx: int
    node: int
    cpus: list[int]
    isolated: bool


def cpu_layout(host, pf: str, cores: int) -> list[Slot]:
    """Split the physical cores of *pf*'s NUMA node into two equal slots.

    One hardware thread per core, so no slot owns an SMT sibling of the other's
    cores; the node's first two cores stay with the OS. Isolated cores
    (isolcpus) are used when the node has enough of them, else only cores that
    are not isolated, so both slots are alike.
    """
    lines = sh(
        host,
        f"n=$(cat /sys/bus/pci/devices/{pf}/numa_node); [ $n -ge 0 ] || n=0; echo $n; "
        "cat /sys/devices/system/node/node$n/cpulist /sys/devices/system/cpu/isolated "
        "/sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list",
    ).split("\n")
    node, node_cpus, isolated = (
        int(lines[0]),
        parse_cpus(lines[1]),
        set(parse_cpus(lines[2])),
    )
    first_threads = {min(parse_cpus(line)) for line in lines[3:] if line.strip()}
    pool = sorted(c for c in node_cpus if c in first_threads)[2:]
    iso = [c for c in pool if c in isolated]
    picked = iso if len(iso) >= 2 * cores else [c for c in pool if c not in isolated]
    picked = picked[: 2 * cores]
    if len(picked) < 2 * cores:
        raise RuntimeError(
            f"{host.name}: NUMA node {node} has fewer than {2 * cores} free cores"
        )
    return [
        Slot(i, node, picked[i * cores : (i + 1) * cores], set(picked) <= isolated)
        for i in (0, 1)
    ]


def stop_units(host) -> None:
    sh(
        host,
        f"systemctl stop '{UNIT_PREFIX}*' 2>/dev/null; "
        f"systemctl reset-failed '{UNIT_PREFIX}*' 2>/dev/null; true",
    )


def _start_unit(host, name: str, cmd: str, props: list[str], log: str, env=()) -> int:
    """Start *cmd* as the transient service *name* and return its main PID.

    systemd expands $ in ExecStart=, so *cmd* carries no script text: the
    callers put that in a file.
    """
    props = [
        *props,
        "KillSignal=SIGINT",
        "TimeoutStopSec=60",
        f"StandardOutput=file:{log}",
    ]
    sh(
        host,
        f"systemctl reset-failed {name} 2>/dev/null; systemd-run --unit={name} --quiet "
        + " ".join(f"-p {p}" for p in props)
        + "".join(f" -E {e}" for e in env)
        + f" -- {cmd}",
    )
    return int(sh(host, f"systemctl show -p MainPID --value {name}").strip())


def inactive(host, names: list[str]) -> list[str]:
    """The units of *names* that do not run."""
    states = sh(host, f"systemctl is-active {' '.join(names)}", check=False).split()
    return [n for n, state in zip(names, states) if state != "active"]


def rdt_teardown(host) -> None:
    """Remove the slot groups and give the default group its saved L3 ways back."""
    sh(
        host,
        f"for g in {RESCTRL}/{RDT_PREFIX}*; do [ ! -d $g ] || rmdir $g; done; "
        f"[ ! -f {RDT_SAVE} ] || {{ cat {RDT_SAVE} > {RESCTRL}/schemata && rm {RDT_SAVE}; }}",
    )


def rdt_setup(host) -> list[str]:
    """Give each slot an equal, exclusive share of the L3 ways.

    Returns the two group paths, or [] when the host has no L3 allocation or
    another user owns resctrl.
    """
    rdt_teardown(host)
    out = sh(
        host,
        f"grep -qw cat_l3 /proc/cpuinfo || exit 0; "
        f"[ -d {RESCTRL}/info ] || mount -t resctrl resctrl {RESCTRL} || exit 0; "
        f"[ -d {RESCTRL}/info/L3 ] || {{ echo no-L3; exit 0; }}; "
        f"cat {RESCTRL}/info/L3/cbm_mask {RESCTRL}/info/L3/min_cbm_bits "
        f"{RESCTRL}/info/L3/shareable_bits; grep '^ *L3:' {RESCTRL}/schemata; "
        f"find {RESCTRL} -mindepth 1 -maxdepth 1 -type d -printf '%f\\n'",
        check=False,
    ).split()
    if out == ["no-L3"]:
        logger.warning(f"{host.name}: resctrl has no L3 allocation (CDP?); no CAT")
    if len(out) < 4:
        return []
    others = set(out[4:]) - {"info", "mon_data", "mon_groups"}
    if others:
        logger.warning(f"{host.name}: resctrl groups {sorted(others)} exist; no CAT")
        return []
    full, min_bits, shared = int(out[0], 16), int(out[1]), int(out[2], 16)
    ways = bin(full).count("1")
    share = ways // 3
    # A CBM must be contiguous, so the slots take one end of it and the rest of the
    # host the other; the end without the ways the NIC writes into (DDIO, the
    # shareable bits), so neither slot shares them.
    block = (1 << 2 * share) - 1
    ends = [off for off in (0, ways - 2 * share) if not (block << off) & shared]
    if share < min_bits or not ends:
        logger.warning(f"{host.name}: cannot split {ways} L3 ways equally; no CAT")
        return []
    slot0 = ((1 << share) - 1) << ends[0]
    masks = [full & ~(slot0 | slot0 << share), slot0, slot0 << share]
    domains = [d.split("=")[0] for d in out[3].split(":", 1)[1].split(";")]

    def line(mask):
        return "L3:" + ";".join(f"{d}={mask:x}" for d in domains)

    paths = [f"{RESCTRL}/{RDT_PREFIX}{i}" for i in (0, 1)]
    sh(
        host,
        f"grep '^ *L3:' {RESCTRL}/schemata > {RDT_SAVE} && "
        f"echo '{line(masks[0])}' > {RESCTRL}/schemata && "
        + " && ".join(
            f"mkdir {p} && echo '{line(m)}' > {p}/schemata"
            for p, m in zip(paths, masks[1:])
        ),
    )
    logger.info(f"{host.name}: CAT {share} of {ways} L3 ways per slot")
    return paths


def rdt_counters(host, group: str | None) -> tuple[int, int] | None:
    """(mbm_total_bytes, llc_occupancy) of *group* summed over its L3 domains."""
    if not group:
        return None
    out = sh(
        host,
        f"cat {group}/mon_data/mon_L3_*/mbm_total_bytes; echo -; "
        f"cat {group}/mon_data/mon_L3_*/llc_occupancy",
        check=False,
    )
    total, _, occupancy = out.partition("-")
    try:
        return sum(map(int, total.split())), sum(map(int, occupancy.split()))
    except ValueError:
        return None


@dataclass
class Side:
    """The applications of one direction: one host, one VF and one slot per instance."""

    host: object
    role: str  # "rx" or "tx"
    mtl_path: str
    vfs: list[str]
    slots: list[Slot]
    rdt: list[str]


@dataclass
class Instance:
    """One application of one instance: a TX or an RX process on one host."""

    host: object
    name: str
    role: str
    label: str
    slot: Slot
    tree: str
    log: str
    rdt: str | None = None
    pid: int = 0
    trace_mode: str | None = None  # how the traced window timed it; None: not yet

    @property
    def units(self) -> list[str]:
        traced = [f"{self.name}-trace"] if self.trace_mode else []
        return [self.name, f"{self.name}-log", *traced]

    def start(self, mtl_path: str, app_cmd: str) -> None:
        """Start *app_cmd* in its slot's cpuset, with its own tree at <mtl>/.local_install.

        The process starts on cpus[0], and its mask is then widened to the slot:
        EAL takes its lcores from that mask, so the schedulers get the other
        CPUs and every unpinned thread starts where the main thread is.
        """
        libs = ":".join(f"{mtl_path}/{p}" for p in LD_LIB_PATH_REL.split(":"))
        cpus = format_cpus(self.slot.cpus)
        join_rdt = f"echo $$ > {self.rdt}/tasks && " if self.rdt else ""
        write_file(
            self.host,
            f"{self.log}.sh",
            f"mount --bind {self.tree} {mtl_path}/.local_install && "
            "mkdir -p /var/run/imtl && mount -t tmpfs tmpfs /var/run/imtl && "
            f"{join_rdt}cd {mtl_path} && exec taskset -c {self.slot.cpus[0]} "
            f"taskset -c {cpus} env LD_LIBRARY_PATH={libs} {app_cmd}\n",
        )
        self.pid = _start_unit(
            self.host,
            self.name,
            f"unshare --mount --ipc --propagation private sh {self.log}.sh",
            [f"AllowedCPUs={cpus}", f"AllowedMemoryNodes={self.slot.node}", "Nice=-20"],
            self.log,
        )

    def collect(self, kind: str, script: str) -> None:
        write_file(self.host, f"{self.log}.{kind}.bt", script + "\n")
        _start_unit(
            self.host,
            f"{self.name}-{kind}",
            f"bpftrace -p {self.pid} {self.log}.{kind}.bt",
            [f"AllowedCPUs={COLLECTOR_CPU}"],
            f"{self.log}.{kind}",
            env=["BPFTRACE_STRLEN=200"],
        )

    def trace(self) -> None:
        """Attach the traced window's probes, the cheapest timing the build has."""
        out = sh(self.host, f"bpftrace -l 'usdt:*:sys:*' -p {self.pid}", check=False)
        probes = set(re.findall(r"sys:(\w+)", out))
        if "log_msg" not in probes:  # attached since the start, so the listing failed
            raise RuntimeError(f"{self.name}: bpftrace lists no sys probes: {out}")
        if "tasklet_time_sample" in probes:
            self.trace_mode, script = "sampled", f"{SAMPLE_SCRIPT} {STAT_SCRIPT}"
        elif "tasklet_stat" in probes:
            self.trace_mode, script = "full", f"{TRACE_SCRIPT} {STAT_SCRIPT}"
        else:
            self.trace_mode, script = "full, no counters", TRACE_SCRIPT
        self.collect("trace", script)

    def records(self, tail: int = 0) -> list[str]:
        """The collector lines of all kinds in time order, or the last *tail* of each."""
        cut = f" | tail -n {tail}" if tail else ""
        out = stream(
            self.host,
            f"for f in {self.log}.log {self.log}.trace; do "
            f"[ ! -f $f ] || cat $f{cut}; done",
        )
        lines = [line for line in out.split("\n") if RECORD.match(line)]
        return sorted(lines, key=lambda line: int(line.split(" ", 1)[0]))

    def pin_app_threads(self) -> str:
        """Move the threads that may run anywhere in the slot off the scheduler CPUs.

        CFS would otherwise move an application thread onto a CPU that a
        scheduler polls. Returns why it could not, or "".
        """
        dumps = parse_dumps(self.records(tail=TAIL_LINES))
        tids = " ".join(str(tid) for _, tid in dumps[-1].sch.values()) if dumps else ""
        busy = parse_cpus(
            ",".join(
                sh(
                    self.host,
                    f"for t in {tids}; do awk '/^Cpus_allowed_list/ {{print $2}}' "
                    f"/proc/{self.pid}/task/$t/status; done",
                    check=False,
                ).split()
            )
        )
        free = format_cpus(c for c in self.slot.cpus if c not in busy)
        if not busy:
            return "no scheduler thread found"
        if not free:
            return "the schedulers use every CPU of the slot"
        sh(
            self.host,
            f"for t in /proc/{self.pid}/task/*; do "
            f"[ \"$(awk '/^Cpus_allowed_list/ {{print $2}}' $t/status)\" != "
            f"{format_cpus(self.slot.cpus)} ] || taskset -pc {free} ${{t##*/}} >/dev/null; "
            "done",
            check=False,
        )
        return ""

    def proc(self) -> tuple[int, dict[int, tuple[int, int]]]:
        """(CLOCK_MONOTONIC ns, {tid: (cpu ticks, involuntary switches)}).

        CLOCK_MONOTONIC is the clock of the bpftrace timestamps.
        """
        out = sh(
            self.host,
            f"grep -m1 'now at' /proc/timer_list; for t in /proc/{self.pid}/task/*; do "
            "s=$(cat $t/stat) && n=$(grep ^nonvoluntary_ctxt_switches $t/status) && "
            'echo "${t##*/} ${s##*) } ${n##*[[:space:]]}"; done',
        ).split("\n")
        threads = {}
        for line in filter(None, out[1:]):
            f = line.split()
            # f[1] is field 3 of /proc/<pid>/stat, so utime and stime are f[12] and f[13].
            threads[int(f[0])] = (int(f[12]) + int(f[13]), int(f[-1]))
        return int(out[0].split()[2]), threads

    def save(self, out_dir: str) -> None:
        """Copy the application log and the collector records to *out_dir*."""
        os.makedirs(out_dir, exist_ok=True)
        for suffix in ("", ".log", ".trace"):
            path = f"{self.log}{suffix}"
            with open(os.path.join(out_dir, os.path.basename(path)), "w") as f:
                f.write(stream(self.host, f"[ ! -f {path} ] || cat {path}"))


def pf_unicast(host, vf: str) -> tuple[int, int] | None:
    """(tx, rx) unicast frames of the PF behind *vf*, as its MAC counted them."""
    out = sh(
        host,
        f"ethtool -S $(ls /sys/bus/pci/devices/{vf}/physfn/net) | "
        "grep -E '^ *(tx|rx)_unicast.nic:'",
        check=False,
    )
    counters = dict(
        line.strip().split(": ") for line in out.split("\n") if ": " in line
    )
    try:
        return int(counters["tx_unicast.nic"]), int(counters["rx_unicast.nic"])
    except (KeyError, ValueError):
        return None


def runner_busy(host) -> bool:
    """True while a GitHub Actions job runs on *host*; the perf hosts also serve other repos."""
    return sh(host, "pgrep -x Runner.Worker", check=False).strip() != ""


@dataclass
class Dump:
    """One MTL stats dump as the log_msg collector saw it."""

    ts: int
    traced: bool = False
    sch: dict = field(default_factory=dict)  # idx: (avg loop ns, t_pid)
    tasklet_avg: dict = field(default_factory=dict)  # name: [avg ns per call]
    tasklets: dict = field(default_factory=dict)  # (sch, name): [cnt, sum, wcnt, wsum]
    fps: dict = field(default_factory=dict)  # (dir, m, s): fps
    busy: dict = field(default_factory=dict)  # (dir, m, s): cpu busy
    dev: list = field(default_factory=list)  # [tx Mb/s, rx Mb/s, tx pkts, rx pkts]
    incomplete: int = 0
    hw_dropped: int = 0


def parse_dumps(records: list[str]) -> list[Dump]:
    dumps: list[Dump] = []
    cur = None
    for rec in records:
        ts, kind, msg = rec.split(" ", 2)
        if kind == "T":
            if cur:
                sch, name, *vals = msg.split()
                acc = cur.tasklets.setdefault((int(sch), name), [0, 0, 0, 0])
                for i, v in enumerate(vals):
                    acc[i] += int(v)
                cur.traced = True
            continue
        if DUMP_START in msg:
            cur = Dump(int(ts))
            continue
        if not cur:
            continue
        if DUMP_END in msg:
            dumps.append(cur)
            cur = None
        elif m := RE_SCH.search(msg):
            cur.sch[int(m[1])] = (int(m[3]), int(m[2]))
        elif m := RE_TASKLET.search(msg):
            cur.tasklet_avg.setdefault(m[2], []).append(float(m[3]) * 1000)
            cur.traced = True
        elif m := RE_FPS.search(msg):
            cur.fps[(m[1], m[2], m[3])] = float(m[4])
        elif m := RE_BUSY.search(msg):
            cur.busy[(m[1], m[2], m[3])] = float(m[4])
        elif m := RE_DEV.search(msg):
            vals = [float(m[1]), float(m[2]), int(m[3]), int(m[4])]
            cur.dev = [a + b for a, b in zip(cur.dev, vals)] if cur.dev else vals
        elif m := RE_INCOMPLETE.search(msg):
            cur.incomplete += int(m[1])
        elif m := RE_HW_DROP.search(msg):
            cur.hw_dropped += int(m[1])
    return dumps


def is_steady(dump: Dump, direction: str, sessions: int, fps: float) -> bool:
    rates = [v for k, v in dump.fps.items() if k[0] == direction]
    return len(rates) == sessions and min(rates) >= STEADY_FPS * fps


def windows(dumps: list[Dump], t0: int, t1: int) -> tuple[list[Dump], list[Dump]]:
    """(untraced, traced) dumps of one run.

    The untraced dumps are those whose whole period lies between the /proc reads
    at *t0* and *t1*, the same span for every instance of the round. traced[0]
    is the dump the tracing started in: only the base of the cumulative
    counters, as its own period was partly untraced. The last dump of the run
    is dropped, as the application was stopping.
    """
    dumps = dumps[:-1]
    first = t0 + DUMP_PERIOD_S * 10**9
    untraced = [d for d in dumps if not d.traced and first <= d.ts <= t1]
    return untraced, [d for d in dumps if d.traced]


def _median(values):
    values = list(values)
    return statistics.median(values) if values else None


def metrics(direction, untraced, traced, proc0, proc1, clk_tck, rdt0, rdt1) -> dict:
    """The metrics of one instance in one round, in report order."""
    m: dict = {}
    for idx in sorted({i for d in untraced for i in d.sch}):
        m[f"sch_{idx} avg loop (ns)"] = _median(
            d.sch[idx][0] for d in untraced if idx in d.sch
        )
    m["video cpu busy (%)"] = _median(
        statistics.mean(v for k, v in d.busy.items() if k[0] == direction)
        for d in untraced
        if any(k[0] == direction for k in d.busy)
    )
    dt = (proc1[0] - proc0[0]) / 1e9
    sch_tids = {d.sch[i][1] for d in untraced for i in d.sch}
    both = proc0[1].keys() & proc1[1].keys()
    ticks = {t: proc1[1][t][0] - proc0[1][t][0] for t in both}
    cores = sum(ticks.values()) / clk_tck / dt
    sch_cores = sum(ticks[t] for t in both & sch_tids) / clk_tck / dt
    m["scheduler threads (cores)"] = sch_cores
    m["other threads (cores)"] = cores - sch_cores
    m["process (cores)"] = cores
    m["sch preemptions (/s)"] = (
        sum(proc1[1][t][1] - proc0[1][t][1] for t in both & sch_tids) / dt
    )
    if rdt0 and rdt1:
        m["memory bandwidth (MB/s)"] = (rdt1[0] - rdt0[0]) / 1e6 / dt
        m["LLC occupancy (MB)"] = rdt1[1] / 1e6
    sessions = {k for d in untraced for k in d.fps if k[0] == direction}
    # The median of each session, so one dump that lost packets does not void
    # the round; the loss counters below report it.
    m["fps (worst session)"] = min(
        _median(d.fps.get(k, 0.0) for d in untraced) for k in sessions
    )
    rate = 1 if direction == "RX" else 0
    m[f"{direction} rate (Mb/s)"] = _median(d.dev[rate] for d in untraced if d.dev)
    m["incomplete frames"] = sum(d.incomplete for d in untraced)
    m["hw dropped pkts"] = sum(d.hw_dropped for d in untraced)

    base, traced = traced[:1], traced[1:]
    # The loop time MTL always measures, now with the timing cost in it.
    for idx in sorted({i for d in traced for i in d.sch}):
        m[f"sch_{idx} avg loop, traced (ns)"] = _median(
            d.sch[idx][0] for d in traced if idx in d.sch
        )
    per_name: dict = {}
    if traced and base[0].tasklets and traced[-1].tasklets:
        first, last = base[0], traced[-1]
        dt_ns = last.ts - first.ts
        for key, vals in last.tasklets.items():
            delta = [b - a for a, b in zip(first.tasklets.get(key, [0, 0, 0, 0]), vals)]
            acc = per_name.setdefault(key[1], [0, 0, 0, 0])
            for i, v in enumerate(delta):
                acc[i] += v
        for name, (cnt, tsum, wcnt, wsum) in sorted(per_name.items()):
            if cnt:
                m[f"{name} avg (ns/call)"] = tsum / cnt
            if wcnt:
                m[f"{name} work (ns/call)"] = wsum / wcnt
                m[f"{name} work (% of a core)"] = 100 * wsum / dt_ns
        pkts = sum(d.dev[3 if direction == "RX" else 2] for d in traced if d.dev)
        work = sum(per_name.get(n, [0, 0, 0, 0])[3] for n in WORK_TASKLETS[direction])
        if pkts and work:
            m[f"{direction} work (ns/pkt)"] = work / pkts
    # A build without sys:tasklet_stat prints these while every loop is timed.
    for name in sorted({n for d in traced for n in d.tasklet_avg} - per_name.keys()):
        m[f"{name} avg (ns/call)"] = _median(
            statistics.mean(d.tasklet_avg[name])
            for d in traced
            if name in d.tasklet_avg
        )
    return m


# Metrics a regression check looks at; lower is better for all of them.
PRIMARY = re.compile(r"avg loop \(ns\)|process \(cores\)|work \(ns/pkt\)")
# Metrics of the traced window. They compare only when A and B were timed alike:
# sampled, every loop with the cumulative counters of mtl_sch_get_stats(), or every
# loop without them, which costs less again.
TRACED = re.compile(r"traced|\(ns/call\)|work \(")


@dataclass
class Row:
    metric: str
    a: float | None
    b: float | None
    diff: float | None
    noise: float | None
    alike: bool = True  # False: A and B were timed differently, so no diff

    @property
    def regression(self) -> float | None:
        """How much worse B is than A, in percent beyond the noise, or None."""
        if self.diff is None or not PRIMARY.search(self.metric):
            return None
        return self.diff - (self.noise or 0.0)


def _spread(values) -> float | None:
    values = [v for v in values if v is not None]
    if len(values) < 2 or not statistics.mean(values):
        return None
    return 100 * (max(values) - min(values)) / abs(statistics.mean(values))


def _mean(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def compare(rounds: list[dict], side: str) -> list[Row]:
    """Rows of one side; rounds[r] is {"A": {side: metrics}, "B": {side: metrics}}."""
    names: list[str] = []
    for r in rounds:
        for inst in ("A", "B"):
            names += [n for n in r[inst].get(side, {}) if n not in names]
    modes = [
        {r[inst].get("trace", {}).get(side) for r in rounds} for inst in ("A", "B")
    ]
    rows = []
    for name in names:
        a_vals = [r["A"].get(side, {}).get(name) for r in rounds]
        b_vals = [r["B"].get(side, {}).get(name) for r in rounds]
        a = _mean(a_vals)
        b = _mean(b_vals)
        alike = modes[0] == modes[1] or not TRACED.search(name)
        diff = 100 * (b - a) / a if a and b is not None and alike else None
        spreads = [s for s in (_spread(a_vals), _spread(b_vals)) if s is not None]
        rows.append(Row(name, a, b, diff, max(spreads) if spreads else None, alike))
    return rows


def _fmt(v) -> str:
    if v is None:
        return "n/a"
    return f"{v:.0f}" if abs(v) >= 100 or v == int(v) else f"{v:.2f}"


def render_rows(rows: list[Row]) -> list[str]:
    out = ["| Metric | A | B | Diff | Noise |", "| --- | ---: | ---: | ---: | ---: |"]
    for r in rows:
        if not r.alike:
            diff = "traced unlike"
        elif r.diff is None:
            diff = "n/a"
        elif abs(r.diff) < 0.05 or r.noise is not None and abs(r.diff) <= r.noise:
            diff = f"~ ({r.diff:+.1f}%)"
        else:
            diff = f"{r.diff:+.1f}%"
        noise = "n/a" if r.noise is None else f"±{r.noise:.1f}%"
        out.append(f"| {r.metric} | {_fmt(r.a)} | {_fmt(r.b)} | {diff} | {noise} |")
    return out


def _wait_steady(insts, sessions, fps) -> list[str]:
    """Wait until the last dump of every instance shows all its sessions at speed."""
    timeout = steady_timeout(sessions)
    deadline = time.monotonic() + timeout
    while True:
        dead = [u for i in insts for u in inactive(i.host, i.units)]
        if dead:
            return [f"exited before steady state: {', '.join(dead)}"]
        waiting = [
            i.name
            for i in insts
            if not any(
                is_steady(d, DIRECTION[i.role], sessions, fps)
                for d in parse_dumps(i.records(tail=TAIL_LINES))[-1:]
            )
        ]
        if not waiting:
            return []
        if time.monotonic() > deadline:
            return [f"not steady after {timeout} s: {', '.join(waiting)}"]
        time.sleep(POLL_S)


def run_round(
    sides,
    trees,
    order,
    app_cmd,
    run_dir,
    out_dir,
    sessions,
    fps,
    warmup_s,
    untraced_s,
    traced_s,
):
    """Run one round of both instances at once.

    *sides* is [rx side, tx side], *order[slot]* names the instance ("A" or "B")
    in that slot, *trees* maps it to its build tree and *app_cmd(side, slot,
    label)* returns its application command. The logs and records of the round
    are copied to *out_dir*. Returns ({label: {role: metrics, "trace": {role: mode}}},
    problems, notes).
    """
    rx, tx = sides
    problems, notes, insts = [], [], []
    clk = {id(s.host): int(sh(s.host, "getconf CLK_TCK")) for s in sides}
    for s in sides:
        stop_units(s.host)
        sh(s.host, f"rm -rf {run_dir}; mkdir -p {run_dir}")
    wire0 = [
        (pf_unicast(tx.host, tx.vfs[i]), pf_unicast(rx.host, rx.vfs[i])) for i in (0, 1)
    ]
    busy = runner_busy(tx.host)
    try:
        # RX first: a unicast TX session waits for the ARP reply of the RX port.
        for s in sides:
            for slot in (0, 1):
                label = order[slot]
                inst = Instance(
                    s.host,
                    f"{UNIT_PREFIX}{s.role}{slot}",
                    s.role,
                    label,
                    s.slots[slot],
                    trees[label],
                    f"{run_dir}/{label}_{s.role}{slot}",
                    s.rdt[slot] if s.rdt else None,
                )
                insts.append(inst)
                inst.start(s.mtl_path, app_cmd(s, slot, label))
                inst.collect("log", LOG_SCRIPT)
        problems += _wait_steady(insts, sessions, fps)
        if not problems:
            for i in insts:
                if why := i.pin_app_threads():
                    notes.append(f"{i.name} ({i.label}): app threads not moved, {why}")
            time.sleep(warmup_s)
            snap0 = {i.name: (i.proc(), rdt_counters(i.host, i.rdt)) for i in insts}
            time.sleep(untraced_s)
            snap1 = {i.name: (i.proc(), rdt_counters(i.host, i.rdt)) for i in insts}
            for i in insts:
                i.trace()
            # One dump period for the dump the tracing starts in, one for the last.
            time.sleep(traced_s + 2 * DUMP_PERIOD_S)
            dead = [u for i in insts for u in inactive(i.host, i.units)]
            if dead:
                problems.append(f"exited during the measurement: {', '.join(dead)}")
    finally:
        for s in reversed(sides):
            names = " ".join(i.name for i in insts if i.host is s.host)
            sh(s.host, f"systemctl stop {names}", check=False)
            stop_units(s.host)
        for i in insts:
            try:
                i.save(out_dir)
            except Exception as e:  # keep the error that got here, if any
                logger.error(f"{i.name}: saving its logs failed: {e}")

    for i in (0, 1):
        t0, r0 = wire0[i]
        t1, r1 = pf_unicast(tx.host, tx.vfs[i]), pf_unicast(rx.host, rx.vfs[i])
        if t0 and r0 and t1 and r1:
            lost = (t1[0] - t0[0]) - (r1[1] - r0[1])
            if lost > 0:
                notes.append(f"slot {i}: {lost} packets lost between the hosts")
    if busy or runner_busy(tx.host):
        notes.append(f"a CI job ran on {tx.host.name} during the round")

    results: dict = {label: {} for label in order}
    if problems:
        return results, problems, notes
    for i in insts:
        direction = DIRECTION[i.role]
        (p0, r0), (p1, r1) = snap0[i.name], snap1[i.name]
        untraced, traced = windows(parse_dumps(i.records()), p0[0], p1[0])
        if len(untraced) < 2 or len(traced) < 2:
            problems.append(
                f"{i.name} ({i.label}): {len(untraced)} untraced and "
                f"{len(traced)} traced dumps"
            )
            continue
        m = metrics(direction, untraced, traced, p0, p1, clk[id(i.host)], r0, r1)
        if (
            i.trace_mode != "full, no counters"
            and f"{direction} work (ns/pkt)" not in m
        ):
            problems.append(f"{i.name} ({i.label}): no sys:tasklet_stat records")
        worst = m["fps (worst session)"]
        if worst < STEADY_FPS * fps:
            problems.append(f"{i.name} ({i.label}): a session fell to {worst:.2f} fps")
        for loss in ("incomplete frames", "hw dropped pkts"):
            if m[loss]:
                notes.append(f"{i.name} ({i.label}): {m[loss]} {loss}")
        results[i.label][i.role] = m
        results[i.label].setdefault("trace", {})[i.role] = i.trace_mode
    return results, problems, notes
