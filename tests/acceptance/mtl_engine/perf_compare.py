# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Two isolated MTL instances side by side: placement, USDT collection, comparison.

The design, the isolation and the metrics are described in doc/perf_compare.md.
"""

from __future__ import annotations

import base64
import copy
import html
import json
import logging
import math
import os
import re
import shlex
import statistics
import time
from collections import Counter
from dataclasses import dataclass, field, replace
from datetime import datetime

from common.generate_report import html_head, platform_html
from mtl_engine.const import LD_LIB_PATH_REL
from mtl_engine.dma import bind_dma_to_vfio
from mtl_engine.performance_monitoring import (
    FPS_TOLERANCE_PCT,
    HUGEPAGE_FALLBACK_MARKER,
    format_cpus,
    parse_cpus,
    wire_balance,
)

logger = logging.getLogger(__name__)

UNIT_PREFIX = "mtl-perf-"
RUN_DIR = "/tmp/mtl-perf-compare"
WARMUP_S = 30
RESCTRL = "/sys/fs/resctrl"
RDT_PREFIX = "mtl_perf_"
RDT_SAVE = "/run/mtl-perf-compare.schemata"
# The collectors run on an OS core, outside both slots (cpu_layout leaves cores 0 and 1).
COLLECTOR_CPU = 1
# MTL gives EAL "-l 0,<lcores>", so the other OS core is every instance's main lcore.
MAIN_LCORE = 0

# An explicit newline: BPFTRACE_STRLEN cuts a long message before its own.
LOG_SCRIPT = 'usdt::sys:log_msg { printf("%llu L %s\\n", nsecs, str(arg1)); }'
SAMPLE_SCRIPT = "usdt::sys:tasklet_time_sample {}"
# What perf stat counts per process: its CPU time, cycles and instructions (IPC and
# clock), and the last level cache references and misses.
PMU_EVENTS = "task-clock,cycles,instructions,cache-references,cache-misses"
STAT_SCRIPT = (
    'usdt::sys:tasklet_stat { printf("%llu T %d %s %llu %llu %llu %llu\\n", '
    "nsecs, arg0, str(arg1), arg2, arg3, arg4, arg5); }"
)
# The probe of each window after the traced one, which every release has: while it is
# attached MTL times every tasklet call, or every video session inside the call of its
# tasklet, and the dump prints the times since the last one. Each has a window of its
# own, as timing the sessions would add to the times of the calls.
TIME_SCRIPTS = {
    "timed": "usdt::sys:tasklet_time_measure {}",
    "sessions": "usdt::sys:sessions_time_measure {}",
}

# The one tasklet whose calls that do not return MTL_TASKLET_ALL_DONE are its work:
# rvs_pkt_rx, when it received packets. In a paced run the TX builder builds in calls
# that return ALL_DONE, as its ring to the transmitter is full and the bulk waits in
# its inflight, and the transmitter returns pending while a packet waits for the
# pacer, so their work cannot be told from their polling.
WORK_TASKLET = "rvs_pkt_rx"
DUMP_PERIOD_S = 10  # MT_STAT_INTERVAL_S_DEFAULT
POLL_S = 5
# Enough lines of a collector log to hold the last complete dump of 64 sessions.
TAIL_LINES = 3000
# Prints the Cpus_allowed_list of the /proc status file it is given.
CPUS_ALLOWED = "awk '/^Cpus_allowed_list/ {print $2}'"

DUMP_START = "M T    D E V   S T A T E"
DUMP_END = "E N D    S T A T E"
RECORD = re.compile(r"^\d+ [LT] ")
RE_SCH = re.compile(
    r"SCH\((\d+):[^)]*\): tasklets \d+, lcore \d+\(t_pid: (\d+)\), avg loop (\d+) ns"
)
RE_FPS = re.compile(r"(RX|TX)_VIDEO_SESSION\((\d+),(\d+):[^)]*\): fps ([\d.]+)")
RE_BUSY = re.compile(
    r"(RX|TX)_VIDEO_SESSION\((\d+),(\d+)\): throughput .*cpu busy ([\d.]+)"
)
RE_DEV = re.compile(
    r"DEV\(\d+\): Avr rate, tx: ([\d.]+) Mb/s, rx: ([\d.]+) Mb/s, pkts, tx: \d+, rx: (\d+)"
)
RE_INCOMPLETE = re.compile(r"RX_VIDEO_SESSION\(\d+,\d+\): incomplete frames (\d+)")
RE_HW_DROP = re.compile(r"DEV\(\d+\): Status: rx_hw_dropped_packets (\d+)")
# Printed only for a session that holds a DMA device.
RE_DMA = re.compile(r"RX_VIDEO_SESSION\((\d+),(\d+)\): pkts (\d+) by dma copy")
# The percentiles are newer than the rest of the line.
RE_TASKLET_TIME = re.compile(
    r"SCH\((\d+),\d+\): tasklet ([^,]+), avg (?P<avg>[\d.]+)us max (?P<max>[\d.]+)us "
    r"min (?P<min>[\d.]+)us(?:, p90 (?P<p90>[\d.]+)us p95 (?P<p95>[\d.]+)us "
    r"p99 (?P<p99>[\d.]+)us)?"
)
RE_SESSION_TIME = re.compile(
    r"(RX|TX)_VIDEO_SESSION\((\d+),(\d+)\): tasklet time avg (?P<avg>[\d.]+)us "
    r"max (?P<max>[\d.]+)us min (?P<min>[\d.]+)us"
)
# TX video counters of a frame sent late, which a dump prints when they moved: 0 in
# a healthy run. The rate limiter's warm-up re-plans are a rate, not 0.
TX_LATE = ("build timeout frames", "epoch drop", "transmitter mismatch troffset")
TX_REPLAN = "transmitter recalculate warmup"
RE_TX_PACING = re.compile(
    rf"TX_VIDEO_SESSION\(\d+,\d+\): ({'|'.join((*TX_LATE, TX_REPLAN))}) (\d+)"
)
# A count of perf stat -x, "<count>,<unit>,<event>,..."; not an error or <not counted>.
RE_PERF = re.compile(r"^([\d.]+),[^,]*,([^,]+)", re.M)

# The rows with no better direction, which the report does not score: the clock; IPC,
# which a polling loop raises with idle polls as much as with speed; the cache
# footprint, hits and miss ratio; the port rates, which carry pads and other traffic;
# and the re-plans, which releases count differently. The frame rate is better
# higher, and every other row is a cost, better lower, as the time of a call, which is
# also a wait for the tasklets after it on its scheduler.
UNSCORED = (
    "core clock (GHz)",
    "IPC",
    "LLC occupancy (MB)",
    "LLC hits (M/s)",
    "LLC miss ratio (%)",
    "RX rate (Mb/s)",
    "TX rate (Mb/s)",
    f"{TX_REPLAN} (/s)",
)
HIGHER = ("fps (worst session)",)
# A difference is a change from MIN_CHANGE_PCT and NOISE_FACTOR times the noise, else
# none: the factor keeps a rough noise estimate, as of few rounds, from making a change.
# A change of high or medium confidence is that many times its noise.
MIN_CHANGE_PCT = 2.0
NOISE_FACTOR = 2.0
CONFIDENCE = (("high", 5.0), ("medium", 3.0))
# The class and the mark of an outcome in the report.
OUTCOMES = {
    "regression": ("r", "❌"),
    "improvement": ("i", "✅"),
    "differs": ("d", "➖"),
    "unchanged": ("", ""),
    "n/a": ("", ""),
}
CHANGES = ("regression", "improvement", "differs")
BAR_PCT = 25  # where a difference bar ends; most changes are a few percent
# The steps MTL prints a time in, and so the least noise of its rows: a difference
# within one step tells nothing.
PRINT_STEPS = (
    (" avg loop (ns)", 1),
    (" avg loop, traced (ns)", 1),
    (", timed (us)", 0.01),
    (", session timed (us)", 0.01),
)
# The cost rows of a role that the summary's verdict scores, and the loss and late
# frame rows, 0 when healthy.
KEY_ROWS = re.compile(
    r"sch_\d+ avg loop \(ns\)|video cpu busy \(%\)|scheduler threads \(cores\)"
    r"|memory bandwidth \(MB/s\)|LLC misses \(M/s\)|RX work \(ns/pkt\)"
)
RX_LOSS = ("incomplete frames", "hw dropped pkts")
LOSS_ROWS = (*RX_LOSS, *TX_LATE)
# The tasklet rows that the summary scores outside its verdict: the timed window runs
# slower than production, and a tasklet on several schedulers would count several times.
TASKLET_ROWS = re.compile(r"sch_\d+ .+ (avg|p99), timed \(us\)")
# The groups of a role's rows in report order, by name or pattern; the call times of
# the last two each read as one table, see _matrix().
SAMPLED = "Tasklets, 1 loop in 64 timed"
TIMED = "Tasklet call times, every call timed"
SESSIONS = "Video session call times, every session timed"
CALL_STATS = ("avg", "p90", "p95", "p99", "min", "max")
GROUPS = (
    (
        "Delivery",
        (
            "fps (worst session)",
            "RX rate (Mb/s)",
            "TX rate (Mb/s)",
            *LOSS_ROWS,
            f"{TX_REPLAN} (/s)",
        ),
    ),
    ("Scheduler loops", re.compile(r"sch_\d+ avg loop \(ns\)")),
    (
        "CPU",
        (
            "video cpu busy (%)",
            "scheduler threads (cores)",
            "other threads (cores)",
            "sch preemptions (/s)",
            "IPC",
            "core clock (GHz)",
        ),
    ),
    ("Memory and cache", re.compile(r"memory bandwidth \(MB/s\)|LLC .+")),
    (
        SAMPLED,
        re.compile(
            r"sch_\d+ avg loop, traced \(ns\)|.+ \((ns/call|% of a core|ns/pkt)\)"
        ),
    ),
    (TIMED, re.compile(r"sch_\d+ .+ \w+, timed \(us\)")),
    (SESSIONS, re.compile(r"sch_\d+ .+ \w+, session timed \(us\)")),
)
RE_TIMED_ROW = re.compile(r"sch_(\d+) (.+) (\w+), (?:session )?timed \(us\)")
# The improvement green and the regression red, light tints of the page's status
# colors, 24% of their hue, and the bars of a darker green for 3:1 on white. A
# color-blind reader may not tell the two apart, so a change also has its mark and its
# word, and the better value is bold. A row or a cell of a change takes the tint of
# its outcome, or grey where it differs with no better direction, and its text stays
# in the page's ink. A table of class cmp has the baseline in column 2 and the
# candidate in column 3, one of class scored also the change, the noise, the rounds
# and the outcome, at the same widths in every group; class tm is a table of call
# times, see _matrix(). The heads of both stay in view while their rows scroll past.
STYLE = """
:root{--i:#229954;--i-tint:#cbecd9;--r:#e74c3c;--r-tint:#f9d4d0;--d-tint:#efefef}
.sw::before{content:"";display:inline-block;width:9px;height:9px;border-radius:50%;
  margin:0 4px 0 2px}
.sw.i::before{background:var(--i)}
.sw.r::before{background:var(--r)}
table.cmp,table.tm{overflow:visible}
table.cmp th,table.tm th{background:#f3f5f8;color:var(--brand);text-transform:none;
  border-bottom:2px solid var(--border);position:sticky;top:0;z-index:1}
table.cmp td,table.tm td{padding:6px 12px}
table.scored tr:nth-child(even),table.tm tr:nth-child(even){background:none}
table.scored tr.i td,table.tm td.i{background:var(--i-tint)}
table.scored tr.r td,table.tm td.r{background:var(--r-tint)}
table.scored tr.d td,table.tm td.d{background:var(--d-tint)}
tr.i td:first-child,table.tm td.i{box-shadow:inset 3px 0 var(--i)}
tr.r td:first-child,table.tm td.r{box-shadow:inset 3px 0 var(--r)}
tr.d td:first-child,table.tm td.d{box-shadow:inset 3px 0 var(--text-muted)}
table.scored td:last-child,table.tm td.u{color:var(--text-muted)}
table.scored tr.i td:last-child,table.scored tr.r td:last-child,
table.scored tr.d td:last-child{color:var(--text);font-weight:700}
table.scored,table.tm{table-layout:fixed}
table.scored th:nth-child(1){width:22%}
table.scored th:nth-child(2),table.scored th:nth-child(3){width:12%}
table.scored th:nth-child(4){width:17%}
table.scored th:nth-child(5){width:7%}
table.scored th:nth-child(6){width:12%}
table.scored td,table.tm td{font-variant-numeric:tabular-nums}
table.scored td:nth-child(2),table.scored td:nth-child(3),table.scored td:nth-child(5),
table.scored td:nth-child(6),table.scored th:nth-child(2),table.scored th:nth-child(3),
table.scored th:nth-child(5),table.scored th:nth-child(6),
table.tm td:nth-child(n+3),table.tm th:nth-child(n+3){text-align:right}
table.tm th:nth-child(1){width:22%}
table.tm th:nth-child(2){width:9%}
table.tm small{display:block;font-size:.85em;color:var(--text-muted)}
table.tm tr.next td{border-top:2px solid var(--border)}
.bar{display:inline-block;position:relative;width:60px;height:6px;margin-right:8px;
  border-radius:3px;background:#e8ecf1;vertical-align:middle}
.bar::before{content:"";position:absolute;left:50%;top:-2px;width:1px;height:10px;
  background:#a9b3c1}
.bar i{position:absolute;top:0;height:100%;border-radius:3px;background:#b8c0cc}
tr.i .bar i{background:var(--i)}
tr.r .bar i{background:var(--r)}
tr.d .bar i{background:var(--text-muted)}
.sec h3{color:var(--brand);background:#f1f2f4;border-left-color:var(--brand)}
table.scored td{overflow-wrap:anywhere}
details.fold{border:0;box-shadow:none;background:none;margin:4px 0 0;overflow:visible}
details.fold>summary{padding:4px 2px;color:var(--text-muted);font-size:.84em;
  background:none;border:0}
details.fold[open]>summary{border-bottom:0}
.verdict{padding:12px 16px;margin-bottom:14px;border-radius:6px;line-height:1.8;
  background:#f8f9fb;border-left:4px solid var(--border)}
.verdict.i{border-left-color:var(--i);background:var(--i-tint)}
.verdict.r{border-left-color:var(--r);background:var(--r-tint)}
.note{font-size:.82em;color:var(--text-muted);margin:8px 0}
"""


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


@dataclass
class Slot:
    """The CPUs and the ports of one slot on one host.

    The first CPU is its instance's application and EAL control threads', the
    others its --lcores: the schedulers take some, and the application threads
    also the ones they leave. A slot on two NUMA nodes has the CPUs of its first
    node first, as many as of the second.
    """

    idx: int
    nodes: list[int]
    cpus: list[int]
    isolated: bool
    ports: list[int] = field(default_factory=list)  # its VFs, indices into Side.vfs
    nic: int | None = None  # its NIC when each NIC has a pair of slots
    main_node: int = 0  # the NUMA node of MAIN_LCORE


def cpu_layout(host, pf: str, cores: int) -> list[Slot]:
    """Split the physical cores of *pf*'s NUMA node into two equal slots.

    One hardware thread per core, so no slot owns an SMT sibling of the other's
    cores; the node's first two cores stay with the OS. Isolated cores
    (isolcpus) are used when the node has enough of them, else only cores that
    are not isolated, so both slots are alike.
    """
    lines = sh(
        host,
        f"n=$(cat /sys/bus/pci/devices/{pf}/numa_node); [ $n -ge 0 ] || n=0; "
        f"echo $n /sys/devices/system/cpu/cpu{MAIN_LCORE}/node*; "
        "cat /sys/devices/system/node/node$n/cpulist /sys/devices/system/cpu/isolated "
        "/sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list",
    ).split("\n")
    nic_node, main_node = lines[0].split()
    node, node_cpus, isolated = (
        int(nic_node),
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
            f"{host.name}: NUMA node {node} has {len(iso)} isolated free cores and "
            f"{len(pool) - len(iso)} others, not {2 * cores} of either"
        )
    return [
        Slot(
            i,
            [node],
            picked[i * cores : (i + 1) * cores],
            set(picked) <= isolated,
            [i],
            main_node=int(main_node.rsplit("node", 1)[1]),
        )
        for i in (0, 1)
    ]


def add_port(config: dict, role: str, vf: str, tx_ip: str, rx_ip: str) -> None:
    """Add *vf* to the RxTxApp configuration of one port, with as many sessions."""
    iface = {
        **config["interfaces"][0],
        "name": vf,
        "ip": tx_ip if role == "tx" else rx_ip,
    }
    config["interfaces"].append(iface)
    group = copy.deepcopy(config[f"{role}_sessions"][0])
    group["dip" if role == "tx" else "ip"] = [rx_ip if role == "tx" else tx_ip]
    group["interface"] = [len(config["interfaces"]) - 1]
    config[f"{role}_sessions"].append(group)


def nic_slots(nics: str, nic0: list[Slot], nic1: list[Slot]) -> list[Slot]:
    """The slots of a host from cpu_layout()'s of each NIC, as *nics* uses them: for
    span, slot p has port p of each NIC; for pairs and pooled, NIC 0's slots 0 and 1
    are followed by NIC 1's, slots 2 and 3."""
    if nics == "span":
        return [
            replace(
                a,
                nodes=a.nodes + b.nodes,
                cpus=a.cpus + b.cpus,
                isolated=a.isolated and b.isolated,
                ports=[p, p + 2],
            )
            for p, (a, b) in enumerate(zip(nic0, nic1))
        ]
    return [
        replace(s, idx=2 * nic + s.idx, ports=[2 * nic + s.idx], nic=nic)
        for nic, layout in enumerate((nic0, nic1))
        for s in layout
    ]


def round_order(n: int, slots: int) -> tuple:
    """The label of each slot in round *n*: the builds swap slots every round, and NIC
    1's pair runs the other way round, so every round has each build on both NICs."""
    order = ("A", "B") if n % 2 == 0 else ("B", "A")
    return order + order[::-1] if slots == 4 else order


def stream_bps(width: int, height: int, fps: float) -> float:
    """The wire rate of a 4:2:2 10-bit stream: 20 bits a pixel, paced over 1125 of 1080
    lines, in MTL's packets of 1320 bytes of pixels that take 1406 on the wire."""
    return width * height * 20 * fps * 1125 / 1080 * 1406 / 1320


def two_cards(pfs: list[str]) -> bool:
    """Whether *pfs* are the two ports of one card, then the two of another."""
    cards = [pf.rsplit(".", 1)[0] for pf in pfs]
    return (
        len(cards) == 4
        and cards[0] == cards[1]
        and cards[2] == cards[3]
        and cards[0] != cards[2]
    )


def nic_links(host, pfs: list[str]) -> list[tuple[int, str]]:
    """The speed in Mb/s, 0 without a link, and the firmware of each PF of *pfs*,
    once it has a carrier or 20 s on."""
    out = sh(
        host,
        f"for pf in {' '.join(pfs)}; do i=$(ls /sys/bus/pci/devices/$pf/net); "
        'for n in $(seq 20); do [ "$(cat /sys/class/net/$i/carrier)" = 1 ] && break; '
        'sleep 1; done; echo "$(cat /sys/class/net/$i/speed) '
        "$(ethtool -i $i | sed -n 's/^firmware-version: //p')\"; done 2>/dev/null",
        check=False,
    ).splitlines()
    links = []
    for line in out + [""] * (len(pfs) - len(out)):
        speed, _, firmware = line.partition(" ")
        links.append((int(speed) if speed.isdigit() else 0, firmware))
    return links


def slow_ports(pfs: list[str], links: list[tuple[int, str]], bps: float) -> list[str]:
    """The ports of *pfs*, with their speed, whose links carry less than *bps*."""
    return [
        f"{pf} links at {speed / 1000:g} Gb/s" if speed else f"{pf} has no link"
        for pf, (speed, _) in zip(pfs, links)
        if bps > speed * 10**6
    ]


def unlike_nics(links: list[tuple[int, str]], slots: list[Slot]) -> str:
    """How NIC 0 and NIC 1 of a pair on each NIC differ, by port 0 and slot 0 of
    each: speed, firmware or isolated cores; "" when they are alike."""
    return "; ".join(
        f"{what} {a} on NIC 0, {b} on NIC 1"
        for what, a, b in (
            ("Mb/s", links[0][0], links[2][0]),
            ("firmware", links[0][1], links[2][1]),
            ("isolated cores", slots[0].isolated, slots[2].isolated),
        )
        if a != b
    )


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
    return int(sh(host, f"systemctl show -p MainPID --value {name}"))


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


def rdt_setup(host, count: int = 2) -> list[str]:
    """Give each of *count* slots an equal, exclusive share of the L3 ways.

    Returns a group path per slot, or [] when the host has no L3 allocation or
    another user owns resctrl. Slot i has the ways of slot i % 2 in every L3,
    so a pair of slots on each of two sockets keeps the two pairs apart.
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
    lines = ["L3:" + ";".join(f"{d}={m:x}" for d in domains) for m in masks]
    slot_lines = [lines[1 + i % 2] for i in range(count)]
    paths = [f"{RESCTRL}/{RDT_PREFIX}{i}" for i in range(count)]
    sh(
        host,
        f"grep '^ *L3:' {RESCTRL}/schemata > {RDT_SAVE} && "
        f"echo '{lines[0]}' > {RESCTRL}/schemata && "
        + " && ".join(
            f"mkdir {p} && echo '{line}' > {p}/schemata"
            for p, line in zip(paths, slot_lines)
        ),
    )
    logger.info(f"{host.name}: CAT {share} of {ways} L3 ways per slot")
    return paths


def dsa_layout(host, slots: list[Slot]) -> list[str]:
    """Bind a DSA on each slot's NUMA node to vfio-pci and return their PCI addresses.

    MTL gives a session only a DMA device on its port's node, and vfio gives one to a
    single process, so each slot of a node takes a DSA of its own: the i-th slot on a
    node its i-th DSA by address. host_dma_devices has lifted vfio-pci's denylist on
    DSA for the session; lifting it here would reload vfio-pci and unbind the VFs.
    """
    out = sh(
        host,
        "cat /sys/module/vfio_pci/parameters/disable_denylist; "
        "for d in /sys/bus/pci/devices/*; do "
        '[ "$(cat $d/vendor):$(cat $d/device)" = 0x8086:0x0b25 ] && '
        "echo ${d##*/} $(cat $d/numa_node); done",
        check=False,
    ).split("\n")
    if out[0].strip() != "Y":
        raise RuntimeError(f"{host.name}: vfio-pci still refuses DSA (its denylist)")
    by_node: dict = {}
    for bdf, node in sorted(line.split() for line in out[1:] if line.strip()):
        by_node.setdefault(int(node), []).append(bdf)
    picked = []
    for slot in slots:
        mine = []
        for node in slot.nodes:
            nth = [s.idx for s in slots if node in s.nodes].index(slot.idx)
            dsa = by_node.get(node, [])
            if nth >= len(dsa):
                raise RuntimeError(
                    f"{host.name}: NUMA node {node} has {len(dsa)} DSA, its slots need "
                    f"{nth + 1}"
                )
            if not bind_dma_to_vfio(host, dsa[nth]):
                raise RuntimeError(f"{host.name}: DSA {dsa[nth]} is not on vfio-pci")
            mine.append(dsa[nth])
        picked.append(",".join(mine))
    return picked


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
    """The applications of one direction: one host, and the VFs and slots of its
    instances."""

    host: object
    role: str  # "rx" or "tx"
    mtl_path: str
    vfs: list[str]
    slots: list[Slot]
    rdt: list[str]
    perf: str | None = None  # a perf that counts PMU_EVENTS, see perf_tool()
    dma: list[str] = field(default_factory=list)  # the DSA of each slot, dsa_layout()


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
    perf: str | None = None
    pid: int = 0
    traced: bool = False  # the build has the traced window's probes
    window: str = ""  # the kind of the probes attached beside the log's, if any
    script: str = ""  # the command its unit ran, see start()
    dma: bool = False  # its command gives it a DMA device

    @property
    def key(self) -> str:
        """Its results' key: its role, and the NIC of its slot if each NIC has a pair."""
        return (
            self.role if self.slot.nic is None else f"{self.role} NIC {self.slot.nic}"
        )

    @property
    def units(self) -> list[str]:
        window = [f"{self.name}-{self.window}"] if self.window else []
        return [self.name, f"{self.name}-log", *window]

    def start(self, mtl_path: str, app_cmd: str) -> None:
        """Start *app_cmd* in its slot's cpuset, with its own tree at <mtl>/.local_install.

        MTL gives EAL MAIN_LCORE as the main lcore, so the cpuset has it and its
        node's memory too: DPDK allocates on the main lcore's node for a thread that
        is no lcore and asks for no node, and its dmadev data only there.
        """
        libs = ":".join(f"{mtl_path}/{p}" for p in LD_LIB_PATH_REL.split(":"))
        join_rdt = f"echo $$ > {self.rdt}/tasks && " if self.rdt else ""
        self.script = (
            f"mount --bind {self.tree} {mtl_path}/.local_install && "
            "mkdir -p /var/run/imtl && mount -t tmpfs tmpfs /var/run/imtl && "
            f"{join_rdt}cd {mtl_path} && exec env LD_LIBRARY_PATH={libs} {app_cmd}"
        )
        write_file(self.host, f"{self.log}.sh", self.script + "\n")
        self.pid = _start_unit(
            self.host,
            self.name,
            f"unshare --mount --ipc --propagation private sh {self.log}.sh",
            [
                f"AllowedCPUs={format_cpus({MAIN_LCORE, *self.slot.cpus})}",
                "AllowedMemoryNodes="
                f"{format_cpus({self.slot.main_node, *self.slot.nodes})}",
                "Nice=-20",
            ],
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
        """Attach the sampled timing and sys:tasklet_stat, if the build has them.

        A build has both or neither, as before them; one without them has no
        traced window.
        """
        out = sh(self.host, f"bpftrace -l 'usdt:*:sys:*' -p {self.pid}", check=False)
        probes = set(re.findall(r"sys:(\w+)", out))
        if "log_msg" not in probes:  # attached since the start, so the listing failed
            raise RuntimeError(f"{self.name}: bpftrace lists no sys probes: {out}")
        self.traced = "tasklet_time_sample" in probes
        if self.traced:
            self.collect("trace", f"{SAMPLE_SCRIPT} {STAT_SCRIPT}")
            self.window = "trace"

    def time(self, kind: str) -> None:
        """Attach TIME_SCRIPTS[*kind*] in place of the probes of the window before.

        Each window has its probes alone: full timing would add to the sampled
        counters, and timing the sessions to the call times.
        """
        if self.window:
            sh(self.host, f"systemctl stop {self.name}-{self.window}", check=False)
        self.collect(kind, TIME_SCRIPTS[kind])
        self.window = kind

    def pmu_start(self) -> None:
        """Count PMU_EVENTS of the process with perf stat, until pmu_stop()."""
        if self.perf:
            _start_unit(
                self.host,
                f"{self.name}-pmu",
                f"{self.perf} stat -x, -e {PMU_EVENTS} -p {self.pid}",
                [f"AllowedCPUs={COLLECTOR_CPU}"],
                f"{self.log}.pmu",
            )

    def pmu_stop(self) -> dict[str, float]:
        """Stop the perf stat of pmu_start(): on SIGINT it writes its counts to its log."""
        if not self.perf:
            return {}
        sh(self.host, f"systemctl stop {self.name}-pmu", check=False)
        return parse_perf(sh(self.host, f"cat {self.log}.pmu", check=False))

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
        """Move the threads that may run on more than one CPU off the scheduler CPUs.

        EAL pins each thread it starts to one CPU; CFS would move one that the
        main thread started, as the application's, onto a CPU that a scheduler
        polls. Each gets one of the free CPUs in turn: the kernel
        balances no task across isolated CPUs, so threads that shared a mask
        would all stay on the CPU they started on. Returns why it could not, or "".
        """
        dumps = parse_dumps(self.records(tail=TAIL_LINES))
        tids = [tid for _, tid in dumps[-1].sch.values()] if dumps else []
        if 0 in tids:  # a dump from before mtl_start(): t_pid 0
            return "the schedulers have not started"
        out = sh(
            self.host,
            f"for t in {' '.join(map(str, tids))}; do "
            f"{CPUS_ALLOWED} /proc/{self.pid}/task/$t/status 2>/dev/null; done",
            check=False,
        )
        busy = parse_cpus(",".join(out.split()))
        if not busy:
            return "no scheduler thread found"
        free = [c for c in self.slot.cpus if c not in busy]
        if not free:
            return "the schedulers use every CPU of the slot"
        sh(
            self.host,
            f"cpus=({' '.join(map(str, free))}); n=0; "
            f"for t in /proc/{self.pid}/task/*; do "
            f"case $({CPUS_ALLOWED} $t/status) in *[,-]*) ;; *) continue ;; esac; "
            "taskset -pc ${cpus[n++ % ${#cpus[@]}]} ${t##*/} >/dev/null; "
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
            'echo "${t##*/} ${s##*) } ${n##*[[:space:]]}" || :; done 2>/dev/null',
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
        for suffix in ("", ".log", ".trace", ".pmu"):
            path = f"{self.log}{suffix}"
            with open(os.path.join(out_dir, os.path.basename(path)), "w") as f:
                f.write(stream(self.host, f"[ ! -f {path} ] || cat {path}"))


def perf_tool(host) -> str | None:
    """A perf on *host* that counts every event of PMU_EVENTS, or None.

    Ubuntu's /usr/bin/perf refuses to run without the linux-tools of the running
    kernel; the perf of another installed version counts these events. perf exits
    0 on an event it cannot count, so its counts decide.
    """
    found = sh(
        host,
        "ls -d /usr/bin/perf 2>/dev/null; "
        "ls -d /usr/lib/linux-tools/*/perf 2>/dev/null | sort -rV",
        check=False,
    )
    for perf in found.split():
        out = sh(host, f"{perf} stat -x, -e {PMU_EVENTS} -- true", check=False)
        if parse_perf(out).keys() >= set(PMU_EVENTS.split(",")):
            return perf
    return None


def parse_perf(text: str) -> dict[str, float]:
    """{event: count} of perf stat -x, output, without what perf could not count."""
    return {event: float(count) for count, event in RE_PERF.findall(text)}


def runner_busy(host) -> bool:
    """True while a GitHub Actions job runs on *host*; the perf hosts also serve other repos."""
    return sh(host, "pgrep -x Runner.Worker", check=False).strip() != ""


@dataclass
class Dump:
    """One MTL stats dump as the log_msg collector saw it."""

    ts: int
    sch: dict = field(default_factory=dict)  # idx: (avg loop ns, t_pid)
    tasklets: dict = field(default_factory=dict)  # (sch, name): [cnt, sum, wcnt, wsum]
    fps: dict = field(default_factory=dict)  # (dir, m, s): fps
    busy: dict = field(default_factory=dict)  # (dir, m, s): cpu busy
    dev: list = field(default_factory=list)  # [tx Mb/s, rx Mb/s, rx pkts]
    counts: Counter = field(default_factory=Counter)  # LOSS_ROWS or TX_REPLAN: sum
    tasklet_times: dict = field(default_factory=dict)  # (sch, name): {stat: us}
    session_times: dict = field(default_factory=dict)  # (dir, m, s): {stat: us}
    dma: dict = field(default_factory=dict)  # (m, s): RX pkts copied by DMA


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
        elif m := RE_FPS.search(msg):
            cur.fps[(m[1], m[2], m[3])] = float(m[4])
        elif m := RE_BUSY.search(msg):
            cur.busy[(m[1], m[2], m[3])] = float(m[4])
        elif m := RE_DEV.search(msg):
            vals = [float(m[1]), float(m[2]), int(m[3])]
            cur.dev = [a + b for a, b in zip(cur.dev, vals)] if cur.dev else vals
        elif m := RE_INCOMPLETE.search(msg):
            cur.counts["incomplete frames"] += int(m[1])
        elif m := RE_HW_DROP.search(msg):
            cur.counts["hw dropped pkts"] += int(m[1])
        elif m := RE_DMA.search(msg):
            cur.dma[(m[1], m[2])] = int(m[3])
        elif m := RE_TX_PACING.search(msg):
            cur.counts[m[1]] += int(m[2])
        elif m := RE_TASKLET_TIME.search(msg):
            times = {k: float(v) for k, v in m.groupdict().items() if v is not None}
            cur.tasklet_times[(int(m[1]), m[2])] = times
        elif m := RE_SESSION_TIME.search(msg):
            times = {k: float(v) for k, v in m.groupdict().items()}
            cur.session_times[(m[1], int(m[2]), int(m[3]))] = times
    return dumps


def is_steady(dump: Dump, direction: str, sessions: int, fps: float) -> bool:
    rates = [v for k, v in dump.fps.items() if k[0] == direction]
    return len(rates) == sessions and min(rates) >= FPS_TOLERANCE_PCT * fps


def windows(dumps: list[Dump], t0: int, t1: int) -> tuple[list, list, list, list]:
    """(untraced, traced, timed, sessions) dumps of one run.

    The untraced dumps are those whose whole period lies between the /proc reads
    at *t0* and *t1*, the same span for every instance of the round. traced[0]
    is the dump the tracing started in: only the base of the cumulative
    counters, as its own period was partly untraced; the last is dropped when
    the tracing stopped within it, short of some schedulers. The timed dumps
    hold MTL's call times and the sessions dumps its session times, each but for
    the first, whose period was partly untimed. The last dump of the run is
    dropped, as the application was stopping.
    """
    dumps = dumps[:-1]
    first = t0 + DUMP_PERIOD_S * 10**9
    untraced = [d for d in dumps if first <= d.ts <= t1]
    traced = [d for d in dumps if d.tasklets]
    if len(traced) > 1 and len(traced[-1].tasklets) < len(traced[-2].tasklets):
        traced = traced[:-1]
    timed = [d for d in dumps if d.tasklet_times][1:]
    sessions = [d for d in dumps if d.session_times][1:]
    return untraced, traced, timed, sessions


def _median(values):
    values = list(values)
    return statistics.median(values) if values else None


def _times(rows: str, times: list[dict], stats=CALL_STATS, window="timed") -> dict:
    """The *rows* of a window of call times: per stat, the median of the avg and the
    percentiles of *times*, the least min and the largest max, in us. A build older
    than the percentiles has None for them rather than no key: compare() orders the
    rows by their first appearance, so they keep their place and read n/a."""
    out = {}
    for stat in stats:
        values = [t[stat] for t in times if stat in t]
        total = {"min": min, "max": max}.get(stat, _median)
        out[f"{rows} {stat}, {window} (us)"] = total(values) if values else None
    return out


def metrics(direction, dumps, proc0, proc1, clk_tck, rdt0, rdt1, pmu=None) -> dict:
    """The metrics of one instance in one round, in report order; *dumps* as windows()
    returns them."""
    untraced, traced, timed, session_timed = dumps
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
    m["sch preemptions (/s)"] = (
        sum(proc1[1][t][1] - proc0[1][t][1] for t in both & sch_tids) / dt
    )
    if rdt0 and rdt1:
        m["memory bandwidth (MB/s)"] = (rdt1[0] - rdt0[0]) / 1e6 / dt
        m["LLC occupancy (MB)"] = rdt1[1] / 1e6
    if pmu and all(pmu.get(e) for e in PMU_EVENTS.split(",")):
        refs, misses = pmu["cache-references"], pmu["cache-misses"]
        m["IPC"] = pmu["instructions"] / pmu["cycles"]
        m["core clock (GHz)"] = pmu["cycles"] / pmu["task-clock"] / 1e6  # ms
        # Over the /proc window: perf's duration_time sums the threads of a -p
        # process. perf counts about 1 s more, the reads, for A and B alike.
        m["LLC hits (M/s)"] = (refs - misses) / 1e6 / dt
        m["LLC misses (M/s)"] = misses / 1e6 / dt
        m["LLC miss ratio (%)"] = 100 * misses / refs
    sessions = {k for d in untraced for k in d.fps if k[0] == direction}
    # The median of each session, so one dump that lost packets does not void
    # the round; the loss counters below report it.
    m["fps (worst session)"] = min(
        _median(d.fps.get(k, 0.0) for d in untraced) for k in sessions
    )
    rate = 1 if direction == "RX" else 0
    m[f"{direction} rate (Mb/s)"] = _median(d.dev[rate] for d in untraced if d.dev)
    counts = sum((d.counts for d in untraced), Counter())
    for name in RX_LOSS:
        m[name] = counts[name]
    if direction == "TX":
        for name in TX_LATE:
            m[name] = counts[name]
        m[f"{TX_REPLAN} (/s)"] = counts[TX_REPLAN] / (len(untraced) * DUMP_PERIOD_S)

    base, traced = traced[:1], traced[1:]
    # The loop time MTL always measures, now with the timing cost in it.
    for idx in sorted({i for d in traced for i in d.sch}):
        m[f"sch_{idx} avg loop, traced (ns)"] = _median(
            d.sch[idx][0] for d in traced if idx in d.sch
        )
    if traced:
        per_name: dict = {}
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
            if wcnt and name == WORK_TASKLET:
                m[f"{name} work (ns/call)"] = wsum / wcnt
                m[f"{name} work (% of a core)"] = 100 * wsum / dt_ns
        pkts = sum(d.dev[2] for d in traced if d.dev)
        work = per_name.get(WORK_TASKLET, [0, 0, 0, 0])[3]
        if pkts and work:
            m["RX work (ns/pkt)"] = work / pkts
    for key in sorted({k for d in timed for k in d.tasklet_times}):
        times = [d.tasklet_times[key] for d in timed if key in d.tasklet_times]
        m.update(_times(f"sch_{key[0]} {key[1]}", times))
    # The video sessions of a scheduler are those of its manager, whose index is its.
    by_sch: dict = {}
    for d in session_timed:
        for (role, idx, _), times in d.session_times.items():
            if role == direction:
                by_sch.setdefault(idx, []).append(times)
    for idx, times in sorted(by_sch.items()):
        rows = f"sch_{idx} {direction} video session"
        m.update(_times(rows, times, ("avg", "min", "max"), "session timed"))
    return m


@dataclass
class Row:
    metric: str
    a: float | None
    b: float | None
    diff: float | None
    noise: float | None
    rounds: tuple = ()  # (B - A) / A in % of each round that has both


def _noise(values) -> float | None:
    """The noise of the rounds *values* of a build, in % of their mean: twice the
    standard error of the mean, which for two rounds is their difference, and which
    more rounds narrow rather than widen as a range would."""
    values = [v for v in values if v is not None]
    if len(values) < 2 or not statistics.mean(values):
        return None
    twice_sem = math.sqrt(4 * statistics.variance(values) / len(values))
    return 100 * twice_sem / abs(statistics.mean(values))


def _mean(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def compare(rounds: list[dict], side: str) -> list[Row]:
    """Rows of one side; rounds[r] is {"A": {side: metrics}, "B": {side: metrics}}."""
    names: list[str] = []
    for r in rounds:
        for inst in ("A", "B"):
            names += [n for n in r[inst].get(side, {}) if n not in names]
    rows = []
    for name in names:
        a_vals = [r["A"].get(side, {}).get(name) for r in rounds]
        b_vals = [r["B"].get(side, {}).get(name) for r in rounds]
        a, b = _mean(a_vals), _mean(b_vals)
        diff = 100 * (b - a) / a if a and b is not None else None
        noises = [n for n in (_noise(a_vals), _noise(b_vals)) if n is not None]
        noise = max(noises) if noises else None
        step = next((s for end, s in PRINT_STEPS if name.endswith(end)), 0)
        # no less than a step of the smaller one, past the float rounding of b - a
        if noise is not None and step and a and b:
            noise = max(noise, 100 * step * (1 + 1e-9) / min(abs(a), abs(b)))
        paired = tuple(
            100 * (y - x) / x for x, y in zip(a_vals, b_vals) if x and y is not None
        )
        rows.append(Row(name, a, b, diff, noise, paired))
    return rows


def _places(v: float) -> int:
    """The decimals of *v* in the report: none from 100 or for an integer, two from 1,
    three significant digits below."""
    if abs(v) >= 100 or v == int(v):
        return 0
    return 2 if abs(v) >= 1 else 2 - math.floor(math.log10(abs(v)))


def _fmt(v) -> str:
    return "n/a" if v is None else f"{v:,.{_places(v)}f}"


def _no_change(diff: float, noise: float | None) -> bool:
    """Whether *diff* is no change: under MIN_CHANGE_PCT or NOISE_FACTOR times *noise*."""
    return abs(diff) < MIN_CHANGE_PCT or (
        noise is not None and abs(diff) < NOISE_FACTOR * noise
    )


def cells(r: Row) -> list[str]:
    """The report's columns of *r*; a difference that is no change reads "~", one from
    0 says so."""
    if r.diff is None:
        diff = "n/a"
        if r.a == 0 and r.b:
            diff = "~ (from 0)" if _no_change(_change(r), r.noise) else "from 0"
    elif _no_change(r.diff, r.noise):
        diff = f"~ ({r.diff:+.1f}%)"
    else:
        diff = f"{r.diff:+.1f}%"
    noise = "n/a" if r.noise is None else f"±{r.noise:.1f}%"
    a, b = _fmt(r.a), _fmt(r.b)
    # decimals until two values that differ beyond float rounding print apart
    if a == b and None not in (r.a, r.b) and not math.isclose(r.a, r.b):
        places = max(_places(r.a), _places(r.b))
        while a == b and places < 12:
            places += 1
            a, b = f"{r.a:,.{places}f}", f"{r.b:,.{places}f}"
    return [r.metric, a, b, diff, noise]


def _change(r: Row) -> float:
    """The difference of *r* as scored: A at 0 has no (B - A) / A, so B above it is the
    mirror of B at 0, -100%."""
    return r.diff if r.a else 100.0


def outcome(r: Row) -> str:
    """How the candidate did in *r*: a regression or an improvement, lower for a cost
    and higher for a row of HIGHER; "differs" for a change of a row with no better
    direction; "unchanged" for no change; "n/a" without a value, or without a noise
    estimate, as of one valid round, for values that differ by MIN_CHANGE_PCT or more.
    """
    if None in (r.a, r.b):
        return "n/a"
    if r.noise is None:
        small = r.diff is not None and abs(r.diff) < MIN_CHANGE_PCT
        return "unchanged" if r.a == r.b or small else "n/a"
    if _no_change(_change(r), r.noise):
        return "unchanged"
    if r.metric in UNSCORED:
        return "differs"
    return "improvement" if (_change(r) > 0) == (r.metric in HIGHER) else "regression"


def confidence(r: Row) -> str:
    """How sure a change of *r* is: high or medium from that many times its noise, low
    below; "" for no change."""
    if outcome(r) not in CHANGES:
        return ""
    times = abs(_change(r)) / r.noise if r.noise else math.inf
    return next((name for name, least in CONFIDENCE if times >= least), "low")


def build_names(labels: dict) -> dict:
    """The builds as the report names them; equal labels, as of an A/A run, with
    their roles."""
    if labels["A"] != labels["B"]:
        return dict(labels)
    return {"A": f"{labels['A']} (baseline)", "B": f"{labels['B']} (candidate)"}


def _group(metric: str) -> tuple[int, str]:
    """The order and the title of the group of *metric*."""
    for i, (title, rows) in enumerate(GROUPS):
        if metric in rows if isinstance(rows, tuple) else rows.fullmatch(metric):
            return i, title
    return len(GROUPS), "Other"


def _groups(rows: list[Row]) -> list[tuple[str, list[Row]]]:
    """*rows* under the titles of their groups, in group order."""
    groups: dict = {}
    for r in rows:
        groups.setdefault(_group(r.metric), []).append(r)
    return [(title, items) for (_, title), items in sorted(groups.items())]


def _table(head, rows: list[str], cls: str = "mx") -> str:
    """A table of the header cells *head* and the <tr> lines *rows*, all HTML."""
    ths = "".join(f"\n<th>{h}</th>" for h in head)
    return "\n".join([f'<table class="{cls}"><tr>{ths}', "</tr>", *rows, "</table>"])


def _text_table(head: list, rows: list[list], cls: str = "mx") -> str:
    tds = ("".join(f"<td>{html.escape(str(c))}</td>" for c in row) for row in rows)
    return _table(map(html.escape, head), [f"<tr>{t}</tr>" for t in tds], cls)


def _mark(r: Row) -> tuple[str, str]:
    """The class of *r* in a table, that of its outcome, and its Outcome cell: a change
    with its mark and confidence."""
    out = outcome(r)
    cls, mark = OUTCOMES[out]
    return cls, f"{mark} {out} ({confidence(r)})" if mark else out


def _values(r: Row) -> tuple[str, ...]:
    """The class, outcome, A, B, change and noise of *r*, the better value bold."""
    cls, why = _mark(r)
    _, a, b, diff, noise = map(html.escape, cells(r))
    # the baseline is better in a regression, the candidate in an improvement
    a, b = (f"<b>{v}</b>" if cls == x else v for x, v in (("r", a), ("i", b)))
    return cls, why, a, b, diff, noise


def _bar(r: Row) -> str:
    """The candidate's difference as a bar from the middle of a track that ends at
    -BAR_PCT and +BAR_PCT."""
    if r.diff is None:
        return ""
    width = min(abs(r.diff), BAR_PCT) / BAR_PCT * 50
    left = 50 if r.diff > 0 else 50 - width
    return (
        f'<span class="bar"><i style="left:{left:.1f}%;width:{width:.1f}%"></i></span>'
    )


def _per_round(r: Row) -> str:
    """The candidate's difference in each round, in percent, whole from 10."""
    text = (f"{d:+.0f}%" if abs(d) >= 10 else f"{d:+.1f}%" for d in r.rounds)
    return " / ".join(text) or "n/a"


def _direction(metric: str) -> str:
    """The arrow of the better direction of *metric*, if it has one."""
    return "" if metric in UNSCORED else " ↑" if metric in HIGHER else " ↓"


def _cmp_table(items: list[tuple[str, Row]], names: dict) -> str:
    """A table of both builds, a row of each of *items*, (label, row), in the class of
    its outcome."""
    head = [
        "Metric",
        f"Baseline<br>{html.escape(names['A'])}",
        f"Candidate<br>{html.escape(names['B'])}",
        "Change",
        "Noise",
        "Rounds",
        "Outcome",
    ]
    rows = []
    for label, r in items:
        cls, why, a, b, diff, noise = _values(r)
        cols = (
            f"<td>{html.escape(label + _direction(r.metric))}</td><td>{a}</td>"
            f"<td>{b}</td><td>{_bar(r)}{diff}</td><td>{noise}</td>"
            f"<td>{_per_round(r)}</td><td>{html.escape(why)}</td>"
        )
        rows.append(f'<tr class="{cls}">{cols}</tr>' if cls else f"<tr>{cols}</tr>")
    return _table(head, rows, "mx cmp scored")


def _changes(items: list[tuple[str, Row]], names: dict) -> str:
    """The *items* that changed in a table, the others folded under how many they are."""
    changed = [(label, r) for label, r in items if outcome(r) in CHANGES]
    rest = [(label, r) for label, r in items if outcome(r) not in CHANGES]
    parts = [_cmp_table(changed, names)] if changed else []
    if rest:
        count = Counter(outcome(r) for _, r in rest)
        what = " and ".join(f"{count[o]} {o}" for o in ("unchanged", "n/a") if count[o])
        parts.append(
            f'<details class="fold"><summary>{what}</summary>'
            f"{_cmp_table(rest, names)}</details>"
        )
    return "\n".join(parts)


def _by_size(items: list[tuple[str, Row]]) -> list[tuple[str, Row]]:
    """*items*, (label, row), regressions first, then improvements and differences, each
    by its size; the others after them in their order."""
    rank = {o: i for i, o in enumerate(CHANGES)}

    def key(item):
        out = outcome(item[1])
        return (rank[out], -abs(_change(item[1]))) if out in rank else (len(rank), 0)

    return sorted(items, key=key)


def _matrix(rows: list[Row]) -> str:
    """The call times of *rows* as one table: a row per call and scheduler and a column
    per stat that a build has. A cell holds both builds' values, the better one in bold,
    and below them the change and the noise, in the class of its outcome, muted for no
    change; its title gives its outcome and rounds."""
    grid: dict = {}
    for r in rows:
        m = RE_TIMED_ROW.fullmatch(r.metric)
        grid.setdefault((m[2], int(m[1])), {})[m[3]] = r
    shown = {
        s for g in grid.values() for s, r in g.items() if (r.a, r.b) != (None, None)
    }
    stats = [s for s in CALL_STATS if s in shown]
    trs, last = [], None
    for (call, sch), g in sorted(grid.items()):
        tds = []
        for stat in stats:
            if stat not in g:
                tds.append('<td class="u">n/a</td>')
                continue
            r = g[stat]
            cls, why, a, b, diff, noise = _values(r)
            mark = OUTCOMES[outcome(r)][1]
            below = " ".join(x for x in (mark, diff, noise) if x and x != "n/a") or diff
            title = html.escape(f"{why}; rounds {_per_round(r)}")
            tds.append(
                f'<td class="{cls or "u"}" title="{title}">{a} → {b}'
                f"<small>{below}</small></td>"
            )
        first = ' class="next"' if last is not None and call != last else ""
        last = call
        trs.append(
            f"<tr{first}><td>{html.escape(call)}</td><td>sch_{sch}</td>{''.join(tds)}</tr>"
        )
    return _table(["Call", "Scheduler", *(f"{s} (us) ↓" for s in stats)], trs, "mx tm")


def _tables(groups: list[tuple[str, list[Row]]], names: dict) -> str:
    """Each of *groups* under its title: one of call times as a single _matrix(), the
    first of those after a note on how its cells read, another as its changes."""
    parts, noted = [], False
    for title, rows in groups:
        parts.append(f"<h3>{html.escape(title)}</h3>")
        if not all(RE_TIMED_ROW.fullmatch(r.metric) for r in rows):
            parts.append(_changes([(r.metric, r) for r in rows], names))
            continue
        if not noted:
            parts.append(
                '<div class="note">In each cell the baseline → the candidate, the better '
                "one in bold, and below them the change and the noise; a cell's title "
                "gives its outcome and its rounds.</div>"
            )
            noted = True
        parts.append(_matrix(rows))
    return "\n".join(parts)


def _details(title: str, parts: list[str], opened: bool = False) -> str:
    """A section that a click folds or unfolds: the setup, or the instances as they ran."""
    body = "\n".join(parts)
    return (
        f'<div class="sec"><details{" open" * opened}><summary><b>{title}</b></summary>'
        f'<div class="db">{body}</div></details></div>'
    )


def _note(text: str) -> str:
    return f'<div class="note">{html.escape(text)}</div>'


def _is_key_row(r: Row) -> bool:
    """Whether the summary's verdict scores *r*: a key row that both builds have, or a
    loss row once a build counted any."""
    if r.metric in LOSS_ROWS:
        return bool(r.a or r.b)
    return bool(KEY_ROWS.fullmatch(r.metric)) and None not in (r.a, r.b)


def _call(metric: str) -> str:
    """*metric* as a row of mixed tables reads, a call time by its call and scheduler."""
    m = RE_TIMED_ROW.fullmatch(metric)
    return f"{m[2]} on sch_{m[1]}, {m[3]} (us)" if m else metric


def _verdict(
    valid: list[dict], rows: dict, keys: list, names: dict, samples: int = 1
) -> str:
    """The verdict box: what the candidate does to the key rows *keys*, (label, row),
    its largest regression and improvement, the losses, and the rule of a change."""
    count = Counter(outcome(r) for _, r in keys)
    reg, imp, n = count["regression"], count["improvement"], len(keys)
    lines, cls = [], ""
    if len(valid) < 2:
        lines.append("One valid round gives no noise estimate, so no change is scored.")
    elif not n:
        lines.append("The builds share no key metric.")
    else:
        cls = "r" if reg else "i" if imp else ""
        if reg:
            lines.append(f"❌ {names['B']} regresses {reg} of the {n} key metrics")
            lines[-1] += f" and improves {imp or 'none'}."
        elif imp:
            lines.append(f"✅ {names['B']} improves {imp} of the {n} key metrics and")
            lines[-1] += " regresses none."
        else:
            lines.append(f"{names['B']} changes none of the {n} key metrics.")
        # a loss from 0 has no size, and the loss line below has it
        ranked = [x for x in _by_size(keys) if x[1].a]
        extremes = []
        for word, out in (("Worst", "regression"), ("Best", "improvement")):
            label, r = next((x for x in ranked if outcome(x[1]) == out), ("", None))
            if r:
                extremes.append(f"{word}: {label} {r.diff:+.1f}% ({confidence(r)}).")
        if extremes:
            lines.append(" ".join(extremes))
    every = [(role.upper(), r) for role, role_rows in rows.items() for r in role_rows]
    lost = [
        f"{role} {r.metric} {_fmt(r.a)} → {_fmt(r.b)}"
        for role, r in every
        if r.metric in LOSS_ROWS and (r.a or r.b)
    ]
    if lost:
        lines.append(f"⚠️ Loss or late frames: {'; '.join(lost)}.")
    else:
        fps = "; ".join(
            f"{role} {_fmt(r.a)} → {_fmt(r.b)}"
            for role, r in every
            if r.metric == "fps (worst session)"
        )
        at = f"; the worst session at {fps} fps" if fps else ""
        lines.append(f"No loss or late frames{at}.")
    rounds = len(valid) // samples
    on = f" on each of {samples} NICs" if samples > 1 else ""
    rule = (
        f"{rounds} valid round{'s' * (rounds > 1)}{on}, the slots swapped each round. "
        f"A change is at least {MIN_CHANGE_PCT:g}% and {NOISE_FACTOR:g} times the "
        "noise, twice the standard error of a build's mean over its rounds."
    )
    if 1 < len(valid) < 4:  # of a pooled run, each NIC of a round is a sample
        rule += (
            f" Under 4 {'samples' if samples > 1 else 'rounds'} the noise is a rough "
        )
        rule += "estimate: a change of low "
        rule += "confidence is a hint."
    lines.append(rule)
    head, *rest = map(html.escape, lines)
    attr = f"verdict {cls}" if cls else "verdict"
    return f'<div class="{attr}"><b>{head}</b><br>{"<br>".join(rest)}</div>'


def _summary(
    valid: list[dict], hosts: dict, names: dict, styled: bool = True, samples: int = 1
) -> str:
    """The verdict on the key rows of the roles, those rows, the regressions first, the
    tasklet call times outside the verdict, and which build lacks tasklet rows; its
    legend tells of the colors and the bars only when *styled*. *valid* has *samples*
    of each round."""
    p = ['<div class="sec"><h2>Summary</h2>']
    if not valid:
        p.append(
            '<div class="verdict">No valid round: the builds are not compared.</div>'
        )
        return "\n".join(p + ["</div>"])
    rows = {role: compare(valid, role) for role in hosts}

    def items(keep):
        return [
            (f"{role.upper()} · {_call(r.metric)}", r)
            for role in hosts
            for r in sorted(rows[role], key=lambda r: _group(r.metric)[0])
            if keep(r)
        ]

    keys = items(_is_key_row)
    tasklets = items(
        lambda r: TASKLET_ROWS.fullmatch(r.metric) and None not in (r.a, r.b)
    )
    p.append(_verdict(valid, rows, keys, names, samples))
    every = Counter(outcome(r) for role in hosts for r in rows[role])
    tally = " · ".join(f"{OUTCOMES[o][1]} {o}: {every[o]}".strip() for o in OUTCOMES)
    p.append(_note(f"All {every.total()} rows: {tally}"))
    if keys:
        p.append("<h3>Key metrics</h3>")
        p.append(_changes(_by_size(keys), names))
    if tasklets:
        p.append("<h3>Tasklet call times, avg and p99, outside the verdict</h3>")
        p.append(_changes(_by_size(tasklets), names))
    notes = dict.fromkeys(
        n for role in hosts for n in _tasklet_notes(rows[role], names)
    )
    p += map(_note, notes)
    high, medium = (least for _, least in CONFIDENCE)
    grey = ", in grey" if styled else ""
    bar = f" The bar of a change ends at -{BAR_PCT}% and +{BAR_PCT}%." if styled else ""
    p.append(
        '<div class="note">A change of the candidate is a <span class="sw r">'
        'regression</span> ❌ or an <span class="sw i">improvement</span> ✅: lower '
        "for a cost, ↓ after the metric, higher for the frame rate, ↑. ➖ marks a row "
        f"with no better direction that differs{grey}: the clock, IPC, the cache "
        "footprint, hits and miss ratio, the port rates and the re-plans. The better "
        f"value is bold. A change is of high confidence from {high:g} times its noise, "
        f"medium from {medium:g} and low below; Rounds gives the candidate's difference "
        "in each valid round. The noise of a loop or call time is at least one step of "
        f"what MTL prints.{bar} The summary leaves out a row that a build lacks.</div>"
    )
    return "\n".join(p + ["</div>"])


def _tasklet_notes(rows: list[Row], names: dict) -> list[str]:
    """Which build lacks the sampled tasklet counters or the percentiles of the
    timed calls: no release up to v26.09 has either."""
    if not rows:
        return []
    sampled = [r for r in rows if _group(r.metric)[1] == SAMPLED]
    timed = {r.metric: r for r in rows if RE_TIMED_ROW.fullmatch(r.metric)}

    def without(has) -> str:
        sides = (("a", names["A"]), ("b", names["B"]))
        return " and ".join(name for side, name in sides if not has(side))

    def percentiles(side: str) -> bool:
        return not any(
            getattr(r, side) is None
            and getattr(timed[m.replace(" p90,", " avg,")], side) is not None
            for m, r in timed.items()
            if m.endswith(" p90, timed (us)")
        )

    notes = []
    if who := without(lambda side: any(getattr(r, side) is not None for r in sampled)):
        notes.append(
            f"No sampled tasklet counters from {who}: they come from sys:tasklet_stat, "
            "which no release up to v26.09 has, nor main before it was added."
        )
    if who := without(percentiles):
        notes.append(
            f"No p90, p95 or p99 from {who}: no release up to v26.09 prints them, nor "
            "main before they were added."
        )
    return notes


def _platform_text(plat: dict) -> dict:
    """*plat* as HTML text, without the host's own MTL and DPDK: each build has its own."""
    text = {
        name: {k: html.escape(str(v)) for k, v in plat.get(name, {}).items()}
        for name in ("sw_configuration", "hw_configuration")
    }
    text["sw_configuration"].update(
        dict.fromkeys(("mtl_version", "dpdk_driver"), "per build, see Builds")
    )
    return text


def _pool(metrics: dict, nic: int) -> dict:
    """The rows of the pair on *nic* of a round of a pair on each NIC, by role."""
    end = f" NIC {nic}"
    return {
        x: {k.removesuffix(end): m for k, m in metrics[x].items() if k.endswith(end)}
        for x in "AB"
    }


def _results(labels, args, slots, rounds, styled, nics, single_core) -> list[str]:
    """The builds, the rounds, the summary and the tables of each role; the tables
    folded unless *styled*. *nics* is how the builds use the NICs: single, span,
    pairs or pooled; *single_core* whether each instance had one scheduler."""
    valid = [r["metrics"] for r in rounds if not r["problems"]]
    if nics == "pooled":  # each NIC's pair of each round is a sample
        valid = [_pool(v, nic) for v in valid for nic in (0, 1)]
    hosts = {
        f"{s['role']} NIC {s['nic']}" if nics == "pairs" else s["role"]: s["host"]
        for s in slots
    }
    names = build_names(labels)
    builds = [
        ["Build", labels["A"], labels["B"]],
        ["Extra RxTxApp arguments", args["A"] or "-", args["B"] or "-"],
    ]
    # the builds whose instances ran with a DMA device, as their launches say
    dma = {
        x["label"]
        for r in rounds
        for x in r["launches"]
        if "--dma_dev" in x["script"].split()
    }
    if dma:
        builds.append(
            ["RX copies with DMA", *("yes" if x in dma else "no" for x in "AB")]
        )
    if single_core:
        builds.append(["Single core", "yes", "yes"])
    runs = [
        [
            n,
            *(names[x] for x in r["order"]),
            "; ".join(r["problems"] + r["notes"]) or "valid",
        ]
        for n, r in enumerate(rounds, 1)
    ]
    width = len(rounds[0]["order"]) if rounds else 2
    slot_heads = [f"Slot {i}" for i in range(width)]
    p = [
        '<div class="sec"><h2>Builds</h2>',
        _text_table(["", "Baseline", "Candidate"], builds, "mx cmp"),
        "</div>",
        '<div class="sec"><h2>Rounds</h2>',
        _text_table(["Round", *slot_heads, "Result"], runs),
        "</div>",
        _summary(valid, hosts, names, styled, 2 if nics == "pooled" else 1),
    ]
    for role, host in hosts.items():
        title = f"{role.upper()} ({html.escape(host)})"
        rows = compare(valid, role)
        tables = _tables(_groups(rows), names)
        if not styled:
            p.append(_details(f"{title}: every row", [tables]))
            continue
        notes = map(_note, _tasklet_notes(rows, names))
        p += [f'<div class="sec"><h2>{title}</h2>', tables, *notes, "</div>"]
    return p


def summary_html(labels, args, slots, rounds, nics="single", single_core=False) -> str:
    """The report as the job summary shows it, which keeps no style: the builds, the
    rounds and the summary, and the tables of each role folded; the workflow links the
    full report, with the setup and the instances, after it."""
    title = html.escape(f"MTL perf-compare: {labels['B']} against {labels['A']}")
    page = [
        f"<h2>{title}</h2>",
        *_results(labels, args, slots, rounds, False, nics, single_core),
    ]
    return "\n".join(page)


def report_html(
    labels, args, details, slots, rounds, platforms, nics="single", single_core=False
) -> str:
    """The report of a case: its builds, its rounds, how B differs from A, then its
    setup and its instances as they ran.

    *details* are the rows of its test information, *slots* one dict per slot of
    each host (host, role, slot, cpus, isolated, cat, perf; dsa of an RX slot when
    a build uses DMA; nic and pfs when the builds use both NICs), *rounds* the rounds
    as the case keeps them, with the launches of run_round(), *platforms* the
    (label, platform_config.json) of the SUT and the companion, *nics* how the
    builds use the NICs: single, span, pairs or pooled, and *single_core* whether
    each instance had one scheduler.
    """
    title = f"MTL perf-compare: {labels['B']} against {labels['A']}"
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    p = [html_head(html.escape(title), now, STYLE)]
    p += _results(labels, args, slots, rounds, True, nics, single_core)
    info = _text_table(["Parameter", "Value"], details, "kv")
    p.append(_details("Test Information", [info], opened=True))
    yes = {True: "yes", False: "no"}
    placement = [
        [s["host"], s["role"].upper(), s["slot"], s["cpus"]]
        + [yes[s[k]] for k in ("isolated", "cat", "perf")]
        for s in slots
    ]
    head = ["Host", "Role", "Slot", "CPUs", "isolcpus", "CAT", "perf"]
    for key, column in (("pfs", "PFs"), ("dsa", "DSA")):
        if any(key in s for s in slots):
            head.append(column)
            for row, s in zip(placement, slots):
                row.append(s.get(key, "-"))
    # the SUT's label and platform, then the companion's
    texts = [(html.escape(label), _platform_text(plat)) for label, plat in platforms]
    platform = [
        platform_html(*texts[0], *texts[1]),
        '<div class="section-title">Placement</div>',
        _text_table(head, placement),
    ]
    p.append(_details("Platform &amp; Topology", platform, opened=True))
    # A card per instance as it ran; the slot swap runs each build in both slots.
    names = build_names(labels)
    hosts = {s["role"]: s["host"] for s in slots}
    runs: dict = {}
    for n, r in enumerate(rounds, 1):
        for x in r["launches"]:
            runs.setdefault((x["label"], x["role"], x["slot"]), (x, []))[1].append(n)
    cards = []
    for (label, role, slot), (x, ns) in sorted(runs.items()):
        where = f"slot {slot} on {hosts[role]}, round{'s' * (len(ns) > 1)}"
        config = json.dumps(x["config"], indent=2)
        cards.append(
            f"<details><summary>{html.escape(names[label])} {role.upper()} in "
            f"{html.escape(where)} {', '.join(map(str, ns))}</summary>"
            f'<div class="db"><div class="cm"><b>Command:</b> '
            f'<code>{html.escape(x["script"])}</code></div>'
            f'<div class="cfg"><b>Test Configuration (JSON):</b>'
            f"<pre><code>{html.escape(config)}</code></pre></div></div></details>"
        )
    if cards:
        p.append(_details("Instance Details", cards))
    p.append("</div></body></html>")
    return "\n".join(p)


def _exited(insts) -> list[str]:
    return [u for i in insts for u in inactive(i.host, i.units)]


def _wait_steady(insts, sessions, fps) -> list[str]:
    """Wait until the last dump of every instance shows all its sessions at speed.

    The application threads of an instance leave its scheduler CPUs once a dump shows
    all its sessions, so that they exist: on isolated CPUs they could not reach speed
    on the one CPU they started on.
    """
    timeout = steady_timeout(sessions)
    deadline = time.monotonic() + timeout
    moved: set[str] = set()

    def steady(i, last, rate):
        return any(is_steady(d, i.role.upper(), sessions, rate) for d in last[i.name])

    while True:
        if dead := _exited(insts):
            return [f"exited before steady state: {', '.join(dead)}"]
        last = {i.name: parse_dumps(i.records(tail=TAIL_LINES))[-1:] for i in insts}
        for i in insts:  # every session, at any rate
            if steady(i, last, 0) and i.name not in moved and not i.pin_app_threads():
                moved.add(i.name)
        waiting = [i.name for i in insts if not steady(i, last, fps)]
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
    out_dir,
    sessions,
    fps,
    untraced_s,
    traced_s,
    launches,
    names,
    single_core=False,
):
    """Run one round of every instance at once.

    *sides* is [rx side, tx side], *order[slot]* names the instance ("A" or "B")
    in that slot, *trees* maps it to its build tree and *app_cmd(side, slot,
    label)* returns its application command and the configuration to report with
    it. The logs and records of the round are copied to *out_dir*, and *launches*
    gets what each instance ran, its label, role, slot, script and configuration,
    also when the round raises. The problems and notes name the builds by *names*;
    with *single_core*, an instance with more than one scheduler is one.
    Returns ({label: {Instance.key: metrics}}, problems, notes).
    """
    rx, tx = sides
    problems, notes, insts = [], [], []

    def who(i):
        return f"{i.name} ({names[i.label]})"

    clk = {id(s.host): int(sh(s.host, "getconf CLK_TCK")) for s in sides}
    for s in sides:
        stop_units(s.host)
        # Its scripts run as root: no one else may write there, or own it first.
        sh(s.host, f"rm -rf {RUN_DIR} && mkdir -m 700 {RUN_DIR}")
    wire0 = wire_balance(tx.host, tx.vfs, rx.host, rx.vfs)
    busy = runner_busy(tx.host)
    try:
        # RX first: a unicast TX session waits for the ARP reply of the RX port.
        for s in sides:
            for slot in range(len(s.slots)):
                label = order[slot]
                inst = Instance(
                    s.host,
                    f"{UNIT_PREFIX}{s.role}{slot}",
                    s.role,
                    label,
                    s.slots[slot],
                    trees[label],
                    f"{RUN_DIR}/{label}_{s.role}{slot}",
                    s.rdt[slot] if s.rdt else None,
                    s.perf,
                )
                insts.append(inst)
                cmd, config = app_cmd(s, slot, label)
                inst.start(s.mtl_path, cmd)
                inst.dma = "--dma_dev" in cmd.split()
                launches.append(
                    {
                        "label": label,
                        "role": s.role,
                        "slot": slot,
                        "script": inst.script,
                        "config": config,
                    }
                )
                inst.collect("log", LOG_SCRIPT)
        problems += _wait_steady(insts, sessions, fps)
        if not problems:
            for i in insts:
                if why := i.pin_app_threads():
                    notes.append(f"{who(i)}: app threads not moved, {why}")
            time.sleep(WARMUP_S)
            for i in insts:
                i.pmu_start()
            snap0 = {i.name: (i.proc(), rdt_counters(i.host, i.rdt)) for i in insts}
            time.sleep(untraced_s)
            snap1 = {i.name: (i.proc(), rdt_counters(i.host, i.rdt)) for i in insts}
            pmu = {i.name: i.pmu_stop() for i in insts}
            for i in insts:
                i.trace()
            # One dump period for the dump the tracing starts in, one for the last.
            time.sleep(traced_s + 2 * DUMP_PERIOD_S)
            dead = _exited(insts)
            for kind in TIME_SCRIPTS:
                if dead:
                    break
                for i in insts:
                    i.time(kind)
                time.sleep(traced_s + 2 * DUMP_PERIOD_S)  # as long as the traced
                dead = _exited(insts)
            if dead:
                problems.append(f"exited during the measurement: {', '.join(dead)}")
            else:
                for i in insts:  # so the window ends here, not as the applications stop
                    sh(i.host, f"systemctl stop {i.name}-{i.window}", check=False)
                # and each prints the dump that closes it before it stops
                time.sleep(DUMP_PERIOD_S)
    finally:
        for s in reversed(sides):
            units = " ".join(i.name for i in insts if i.host is s.host)
            sh(s.host, f"systemctl stop {units}", check=False)
            stop_units(s.host)
        for i in insts:
            try:
                i.save(out_dir)
            except Exception as e:  # keep the error that got here, if any
                logger.error(f"{i.name}: saving its logs failed: {e}")

    wire1 = wire_balance(tx.host, tx.vfs, rx.host, rx.vfs)
    if wire0 and wire1:
        for slot in rx.slots:
            lost = sum(wire1[p] - wire0[p] for p in slot.ports)
            if lost > 0:
                notes.append(f"slot {slot.idx}: {lost} packets lost between the hosts")
    if busy or runner_busy(tx.host):
        notes.append(f"a CI job ran on {tx.host.name} during the round")
    # Without the rate limiter a build re-plans nothing: say so, or its -100% reads as
    # a gain. MTL logs the way at info level; a log without it compares nothing.
    paced: dict = {}  # NIC of a pair: {label: the pacing ways of its TX sessions}
    for i in insts:
        if i.role == "tx":
            log = sh(i.host, f"grep -ho 'pacing way: [a-z_]*' {i.log}", check=False)
            ways = " and ".join(sorted(set(re.findall(r"way: (\w+)", log))))
            paced.setdefault(i.slot.nic, {})[i.label] = ways
    for nic, by_label in paced.items():
        if all(by_label.values()) and len(set(by_label.values())) > 1:
            ways = "; ".join(f"{names[x]} {way}" for x, way in sorted(by_label.items()))
            on = "" if nic is None else f" on NIC {nic}"
            notes.append(f"the builds pace TX differently{on}: {ways}")

    results: dict = {label: {} for label in order}
    if problems:
        return results, problems, notes
    for i in insts:
        direction = i.role.upper()
        (p0, r0), (p1, r1) = snap0[i.name], snap1[i.name]
        w = windows(parse_dumps(i.records()), p0[0], p1[0])
        untraced, traced, timed, session_timed = w
        if (
            len(untraced) < 2
            or (i.traced and len(traced) < 2)
            or len(timed) < 2
            or len(session_timed) < 2
        ):
            problems.append(
                f"{who(i)}: {len(untraced)} untraced, {len(traced)} traced, "
                f"{len(timed)} timed and {len(session_timed)} session timed dumps"
            )
            continue
        named = len({k for d in untraced for k in d.fps if k[0] == direction})
        if named != sessions:
            problems.append(f"{who(i)}: {named} of {sessions} sessions")
            continue
        schedulers = len({s for d in untraced for s in d.sch})
        if single_core and schedulers != 1:
            problems.append(f"{who(i)}: {schedulers} schedulers, single core runs one")
            continue
        # MTL falls back to the CPU copy, silently, for a session it gives no DMA
        copied = len({k for d in untraced for k, n in d.dma.items() if n})
        if i.dma and copied != sessions:
            problems.append(f"{who(i)}: {copied} of {sessions} sessions copied by DMA")
            continue
        found = f"grep -qsF '{HUGEPAGE_FALLBACK_MARKER}' {i.log} && echo found"
        if i.role == "tx" and sh(i.host, found, check=False):
            problems.append(f"{who(i)}: its source is not in hugepages")
            continue
        m = metrics(direction, w, p0, p1, clk[id(i.host)], r0, r1, pmu[i.name])
        if i.perf and "IPC" not in m:
            notes.append(f"{who(i)}: perf stat did not count every event")
        if i.traced and i.role == "rx" and "RX work (ns/pkt)" not in m:
            problems.append(f"{who(i)}: no {WORK_TASKLET} work in sys:tasklet_stat")
        worst = m["fps (worst session)"]
        if worst < FPS_TOLERANCE_PCT * fps:
            problems.append(f"{who(i)}: a session fell to {worst:.2f} fps")
        for loss in LOSS_ROWS:
            if m.get(loss):
                notes.append(f"{who(i)}: {m[loss]} {loss}")
        results[i.label][i.key] = m
    return results, problems, notes
