# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""mtl_engine/perf_compare.py without hosts: python -m pytest --noconftest <this file>."""

import os
import re
import subprocess  # nosec B404 # runs bash on proc()'s own script and a fixed child, via argv
import sys
import time
from collections import Counter
from dataclasses import replace
from html.parser import HTMLParser
from types import SimpleNamespace

import pytest
from mtl_engine import perf_compare as pc

FPS = 60000 / 1001
S = 10**9
HOST = SimpleNamespace(name="sut")


def _sh(monkeypatch, reply):
    """Route pc.sh to reply(script) and keep every script it got."""
    scripts = []

    def sh(host, script, check=True):
        scripts.append(script)
        return reply(script)

    monkeypatch.setattr(pc, "sh", sh)
    return scripts


def _bash(host, script, check=True):  # pc.sh without sudo: stderr into the output
    res = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    assert not check or res.returncode == 0, res.stdout + res.stderr
    return res.stdout + res.stderr


def test_cpu_lists_round_trip():
    assert pc.parse_cpus("0-3,8,10-11\n") == [0, 1, 2, 3, 8, 10, 11]
    assert pc.format_cpus([11, 10, 8, 3, 2, 1, 0]) == "0-3,8,10-11"


def _layout(monkeypatch, node_cpus, isolated, cores):
    """cpu_layout on sysfs reads where core c has the SMT sibling c + cores."""
    siblings = [f"{c},{c + cores}" for c in range(cores)] * 2
    nodes = "0 /sys/devices/system/cpu/cpu0/node0"  # the VF's and MAIN_LCORE's
    _sh(monkeypatch, lambda script: "\n".join([nodes, node_cpus, isolated, *siblings]))
    return pc.cpu_layout(HOST, "0000:15:00.0", 4)


def test_cpu_layout_takes_isolated_first_threads_after_the_os_cores(monkeypatch):
    slots = _layout(monkeypatch, "0-31", "4-20", 16)
    assert [s.cpus for s in slots] == [[4, 5, 6, 7], [8, 9, 10, 11]]
    assert all(s.isolated and s.nodes == [0] for s in slots)
    assert [s.ports for s in slots] == [[0], [1]]


def test_cpu_layout_keeps_both_slots_off_isolated_cores_when_too_few(monkeypatch):
    slots = _layout(monkeypatch, "0-39", "4-9", 20)
    assert [s.cpus for s in slots] == [[2, 3, 10, 11], [12, 13, 14, 15]]
    assert not any(s.isolated for s in slots)


def test_cpu_layout_refuses_a_node_without_enough_cores(monkeypatch):
    with pytest.raises(RuntimeError, match="0 isolated free cores and 4 others, not 8"):
        _layout(monkeypatch, "0-11", "", 6)


def test_cpu_layout_takes_the_isolated_cores_of_nic_1_and_the_main_lcore_node(
    monkeypatch,
):
    siblings = [f"{c},{c + 128}" for c in range(64, 96)]
    sysfs = ["2 /sys/devices/system/cpu/cpu0/node1", "64-95", "4-20,68-84", *siblings]
    scripts = _sh(monkeypatch, lambda script: "\n".join(sysfs))
    slots = pc.cpu_layout(HOST, "0000:c9:00.0", 8)
    assert [(s.nodes, s.cpus, s.isolated, s.main_node) for s in slots] == [
        ([2], list(range(68, 76)), True, 1),
        ([2], list(range(76, 84)), True, 1),
    ]
    assert "echo $n /sys/devices/system/cpu/cpu0/node*; " in scripts[0]


@pytest.mark.skipif(
    os.geteuid() != 0, reason="proc() reads /proc/timer_list, root only"
)
def test_proc_reads_a_process_whose_threads_come_and_go(monkeypatch):
    monkeypatch.setattr(pc, "sh", _bash)
    churn = (
        "import threading, time\n"
        "while True:\n"
        "    threading.Thread(target=time.sleep, args=(0.001,)).start()\n"
        "    time.sleep(0.0005)\n"
    )
    proc = subprocess.Popen([sys.executable, "-c", churn])
    try:
        inst = pc.Instance(None, "tx0", "tx", "A", None, "/t", "/log")
        inst.pid = proc.pid
        for _ in range(30):
            ts, threads = inst.proc()
            assert ts > 0 and proc.pid in threads
    finally:
        proc.kill()
        proc.wait()


@pytest.mark.skipif(len(os.sched_getaffinity(0)) < 4, reason="needs 4 CPUs")
def test_pin_app_threads_gives_each_free_cpu_its_share_of_the_threads(monkeypatch):
    monkeypatch.setattr(pc, "sh", _bash)
    cpus = sorted(os.sched_getaffinity(0))[:4]
    # a scheduler thread on cpus[1], a control thread EAL pinned to cpus[3], and six
    # that may run on more than one CPU: one of them as numa_bind() narrows a mask
    app = (
        "import os, threading, time\n"
        f"os.sched_setaffinity(0, {set(cpus)})\n"
        "def pinned(mask):\n"
        "    os.sched_setaffinity(0, mask)\n"
        "    print(threading.get_native_id(), flush=True)\n"
        "    time.sleep(60)\n"
        f"for mask in ({{{cpus[1]}}}, {{{cpus[3]}}}, {{{cpus[0]}, {cpus[2]}}}):\n"
        "    threading.Thread(target=pinned, args=(mask,), daemon=True).start()\n"
        "for _ in range(4):\n"
        "    threading.Thread(target=time.sleep, args=(60,), daemon=True).start()\n"
        "time.sleep(60)\n"
    )
    proc = subprocess.Popen(
        [sys.executable, "-c", app], stdout=subprocess.PIPE, text=True
    )
    try:
        sch, ctrl, _ = (int(proc.stdout.readline()) for _ in range(3))
        deadline = time.monotonic() + 10
        while len(os.listdir(f"/proc/{proc.pid}/task")) < 8:
            assert time.monotonic() < deadline, "the threads did not start"
            time.sleep(0.01)
        inst = pc.Instance(
            None, "tx0", "tx", "A", pc.Slot(0, [0], cpus, True), "/t", "/l"
        )
        inst.pid = proc.pid
        monkeypatch.setattr(inst, "records", lambda tail=0: _dump(S, tid=sch))
        assert inst.pin_app_threads() == ""
        tasks = os.listdir(f"/proc/{proc.pid}/task")
        allowed = {int(tid): os.sched_getaffinity(int(tid)) for tid in tasks}
        assert allowed.pop(sch) == {cpus[1]} and allowed[ctrl] == {cpus[3]}
        assert all(len(mask) == 1 for mask in allowed.values()), allowed
        shares = sorted(Counter(min(mask) for mask in allowed.values()).items())
        assert shares == [(cpus[0], 2), (cpus[2], 2), (cpus[3], 3)]
        assert inst.pin_app_threads() == ""  # done: no thread left in the slot's mask
        assert {int(t): os.sched_getaffinity(int(t)) for t in allowed} == allowed
    finally:
        proc.kill()
        proc.wait()


@pytest.mark.parametrize(
    "tid, why",
    [(0, "the schedulers have not started"), (999999999, "no scheduler thread found")],
)
def test_pin_app_threads_waits_for_the_schedulers_and_skips_one_that_left(
    monkeypatch, tid, why
):
    inst = pc.Instance(
        None, "tx0", "tx", "A", pc.Slot(0, [0], [4, 5, 6], True), "/t", "/l"
    )
    inst.pid = os.getpid()
    monkeypatch.setattr(inst, "records", lambda tail=0: _dump(S, tid=tid))
    scripts = []
    monkeypatch.setattr(
        pc, "sh", lambda h, s, check=True: scripts.append(s) or _bash(h, s, check)
    )
    assert inst.pin_app_threads() == why
    assert not any("taskset" in script for script in scripts)


def test_pin_app_threads_spreads_the_threads_over_the_free_cpus_of_both_nodes(
    monkeypatch,
):
    slot = pc.Slot(0, [0, 2], [4, 5, 68, 69], True, [0, 2])
    inst = pc.Instance(None, "tx0", "tx", "A", slot, "/t", "/l")
    inst.pid = 9
    monkeypatch.setattr(inst, "records", lambda tail=0: _dump(S, tid=101))
    scripts = _sh(
        monkeypatch, lambda script: "5\n" if "for t in 101;" in script else ""
    )
    assert inst.pin_app_threads() == ""
    assert scripts[-1].startswith("cpus=(4 68 69); ")


@pytest.mark.parametrize(
    "probe, masks",
    [
        (
            "fff\n1\nc00\n    L3:0=fff;1=fff\ninfo\nmon_groups\n",
            ("f00;1=f00", "f;1=f", "f0;1=f0"),
        ),
        ("fff\n1\n003\n    L3:0=fff\ninfo\n", ("f", "f0", "f00")),  # DDIO's low ways
    ],
)
def test_rdt_setup_gives_each_slot_a_third_of_the_ways_away_from_ddio(
    monkeypatch, probe, masks
):
    scripts = _sh(monkeypatch, lambda script: probe if "cbm_mask" in script else "")
    paths = pc.rdt_setup(HOST)
    assert paths == [f"{pc.RESCTRL}/{pc.RDT_PREFIX}{n}" for n in (0, 1)]
    for path, mask in zip([pc.RESCTRL, *paths], masks):
        assert f"echo 'L3:0={mask}' > {path}/schemata" in scripts[-1]


@pytest.mark.parametrize(
    "probe",
    [
        "",  # no cat_l3
        "no-L3\n",  # L3 not allocatable, e.g. CDP
        "fff\n1\nc00\n    L3:0=fff\ninfo\nother_tool\n",  # resctrl in use
        "fff\n5\nc00\n    L3:0=fff\ninfo\n",  # 4 ways each, but 5 at least
    ],
)
def test_rdt_setup_runs_without_cat_rather_than_take_resctrl(monkeypatch, probe):
    scripts = _sh(monkeypatch, lambda script: probe if "cbm_mask" in script else "")
    assert pc.rdt_setup(HOST) == []
    assert not any("mkdir" in s for s in scripts)


def _nics(nics):
    """The slots of *nics* from two cpu_layout()s: NIC 0's on node 0, isolated, NIC
    1's on node 2, not, as on a host that isolates node 0's cores alone."""
    return pc.nic_slots(
        nics,
        *(
            [pc.Slot(i, [n], [n * 32 + 4 + i], n == 0, [i]) for i in (0, 1)]
            for n in (0, 2)
        ),
    )


def test_the_nics_modes_map_their_slots_to_ports_and_nodes():
    span = _nics("span")
    assert [(s.idx, s.ports, s.nodes, s.cpus, s.isolated, s.nic) for s in span] == [
        (0, [0, 2], [0, 2], [4, 68], False, None),  # isolated only if both nodes are
        (1, [1, 3], [0, 2], [5, 69], False, None),
    ]
    split = _nics("split")  # a socket each, the rows of one table
    assert [(s.idx, s.ports, s.nodes, s.cpus, s.isolated, s.nic) for s in split] == [
        (0, [0], [0], [4], True, None),
        (1, [2], [2], [68], False, None),
    ]
    redundant = _nics("redundant")  # both ports of a NIC, the second the 2022-7 leg
    assert [(s.idx, s.ports, s.nodes, s.cpus, s.nic) for s in redundant] == [
        (0, [0, 1], [0], [4], None),
        (1, [2, 3], [2], [68], None),
    ]
    pairs = _nics("pairs")
    assert [(s.idx, s.ports, s.nodes, s.isolated, s.nic) for s in pairs] == [
        (0, [0], [0], True, 0),
        (1, [1], [0], True, 0),
        (2, [2], [2], False, 1),
        (3, [3], [2], False, 1),
    ]
    # each round A and B on each port of each NIC, slots swapped every round
    assert pc.round_order(0, 2) == ("A", "B") and pc.round_order(1, 2) == ("B", "A")
    assert pc.round_order(0, 4) == ("A", "B", "B", "A")
    assert pc.round_order(1, 4) == ("B", "A", "A", "B")


def test_the_ports_of_a_run_on_both_nics_are_two_of_each_of_two_cards():
    assert pc.two_cards(
        ["0000:15:00.0", "0000:15:00.1", "0000:c9:00.0", "0000:c9:00.1"]
    )
    for pfs in (
        ["0000:15:00.0", "0000:15:00.1"],  # one card
        ["0000:15:00.0", "0000:15:00.1", "0000:15:00.2", "0000:15:00.3"],  # of 4 ports
        ["0000:15:00.0", "0000:c9:00.0", "0000:15:00.1", "0000:c9:00.1"],  # mixed
        ["0000:15:00.0", "0000:80:00.0", "0000:c9:00.0", "0000:c9:00.1"],  # 3 cards
    ):
        assert not pc.two_cards(pfs), pfs


def test_nic_links_reads_the_speed_and_firmware_of_each_port(monkeypatch):
    reply = "100000 2.10 0x80018d29 1.4002.0\n 1.00 0x80016fed 1.3833.0\n"
    scripts = _sh(monkeypatch, lambda script: reply)
    links = pc.nic_links(HOST, ["0000:15:00.0", "0000:c9:00.0", "0000:c9:00.1"])
    assert links == [
        (100000, "2.10 0x80018d29 1.4002.0"),
        (0, "1.00 0x80016fed 1.3833.0"),
        (0, ""),
    ]
    assert scripts[0].startswith("for pf in 0000:15:00.0 0000:c9:00.0 0000:c9:00.1; do")
    # a port must carry its sessions, and a pooled run two alike NICs
    links = [(100000, "2.10"), (100000, "2.10"), (40000, "1.00"), (40000, "1.00")]
    pfs = ["p0", "p1", "p2", "p3"]
    assert pc.slow_ports(pfs, links, 39e9) == []
    assert pc.slow_ports(pfs, links, 41e9) == [
        "p2 links at 40 Gb/s",
        "p3 links at 40 Gb/s",
    ]
    assert pc.slow_ports(["p4"], [(0, "")], 1e9) == ["p4 has no link"]
    pairs = _nics("pairs")
    assert pc.unlike_nics(links, pairs) == (
        "Mb/s 100000 on NIC 0, 40000 on NIC 1; firmware 2.10 on NIC 0, 1.00 on NIC 1; "
        "isolated cores True on NIC 0, False on NIC 1"
    )
    alike = [replace(s, isolated=True) for s in pairs]
    assert pc.unlike_nics([(100000, "2.10")] * 4, alike) == ""


def test_a_port_carries_the_streams_its_link_speed_allows():
    hd, uhd, fuhd = (
        pc.stream_bps(w, h, FPS) for w, h in ((1920, 1080), (3840, 2160), (7680, 4320))
    )
    # MTL's packets of 1320 bytes of pixels take 1406 on the wire
    assert hd == pytest.approx(1920 * 1080 * 20 * FPS * 1125 / 1080 * 1406 / 1320)
    assert 36 * hd < 100e9 < 37 * hd and 14 * hd < 40e9 < 15 * hd
    assert 9 * uhd < 100e9 < 10 * uhd and 2 * fuhd < 100e9 < 3 * fuhd


def test_add_port_gives_the_second_port_as_many_sessions():
    for role, peer in (("tx", "dip"), ("rx", "ip")):
        config = {
            "interfaces": [{"name": "vf0", "ip": "own", "rx_queues_cnt": 64}],
            f"{role}_sessions": [
                {peer: ["peer"], "interface": [0], "st20p": [{"replicas": 3}]}
            ],
        }
        pc.add_port(config, role, "vf2", "tx2", "rx2")
        own, other = ("tx2", "rx2") if role == "tx" else ("rx2", "tx2")
        assert config["interfaces"][1] == {
            "name": "vf2",
            "ip": own,
            "rx_queues_cnt": 64,
        }
        assert config[f"{role}_sessions"][1] == {
            peer: [other],
            "interface": [1],
            "st20p": [{"replicas": 3}],
        }
        assert config[f"{role}_sessions"][0][peer] == ["peer"]  # a copy, not the same


def test_rdt_setup_only_monitors_slots_that_share_no_l3(monkeypatch):
    probe = "fff\n1\nc00\n    L3:0=fff;1=fff\ninfo\nmon_groups\n"
    scripts = _sh(monkeypatch, lambda script: probe if "cbm_mask" in script else "")
    paths = pc.rdt_setup(HOST, 2, partition=False)
    assert paths == [f"{pc.RESCTRL}/{pc.RDT_PREFIX}{n}" for n in (0, 1)]
    for path in paths:
        assert f"mkdir {path} && echo 'L3:0=fff;1=fff' > {path}/schemata" in scripts[-1]
    assert (
        f"> {pc.RESCTRL}/schemata" not in scripts[-1] and pc.RDT_SAVE not in scripts[-1]
    )


def test_rdt_setup_gives_the_pair_of_each_nic_the_ways_of_the_first(monkeypatch):
    probe = "fff\n1\nc00\n    L3:0=fff;1=fff\ninfo\nmon_groups\n"
    scripts = _sh(monkeypatch, lambda script: probe if "cbm_mask" in script else "")
    paths = pc.rdt_setup(HOST, 4)
    assert paths == [f"{pc.RESCTRL}/{pc.RDT_PREFIX}{n}" for n in range(4)]
    for path, mask in zip(paths, ("f;1=f", "f0;1=f0") * 2):
        assert f"mkdir {path} && echo 'L3:0={mask}' > {path}/schemata" in scripts[-1]


def test_dsa_layout_gives_each_slot_of_a_node_a_dsa_of_its_own(monkeypatch):
    listing = "Y\n0000:05:01.0 0\n0000:00:01.0 0\n0000:80:01.0 2\n"
    _sh(monkeypatch, lambda script: listing)
    bound = []
    monkeypatch.setattr(pc, "bind_dma_to_vfio", lambda h, bdf: bound.append(bdf) or 1)
    slots = [pc.Slot(0, [0], [4], True), pc.Slot(1, [0], [8], True)]
    assert pc.dsa_layout(HOST, slots) == ["0000:00:01.0", "0000:05:01.0"] == bound
    node2 = [pc.Slot(0, [2], [66], False), pc.Slot(1, [2], [74], False)]
    with pytest.raises(RuntimeError, match="node 2 has 1 DSA, its slots need 2"):
        pc.dsa_layout(HOST, node2)
    monkeypatch.setattr(pc, "bind_dma_to_vfio", lambda h, bdf: False)
    with pytest.raises(RuntimeError, match="DSA 0000:00:01.0 is not on vfio-pci"):
        pc.dsa_layout(HOST, slots)
    # a slot on two nodes takes a DSA of each
    listing += "0000:85:01.0 2\n"
    _sh(monkeypatch, lambda script: listing)
    monkeypatch.setattr(pc, "bind_dma_to_vfio", lambda h, bdf: True)
    span = [pc.Slot(p, [0, 2], [4 + p, 68 + p], True, [p, p + 2]) for p in (0, 1)]
    assert pc.dsa_layout(HOST, span) == [
        "0000:00:01.0,0000:80:01.0",
        "0000:05:01.0,0000:85:01.0",
    ]
    # lifting the denylist would reload vfio-pci, which unbinds every VF
    _sh(monkeypatch, lambda script: listing.replace("Y", "N", 1))
    with pytest.raises(RuntimeError, match="vfio-pci still refuses DSA"):
        pc.dsa_layout(HOST, slots)


# A dump's sys:tasklet_stat increments of each tasklet: calls, ns, work calls, work ns.
# rvs_pkt_rx works when it receives, the TX builder never and the transmitter nearly
# always.
TASKLETS = {
    "RX": [("rvs_pkt_rx", 1000, 400000, 100, 200000)],
    "TX": [
        ("tx_video_sessions_mgr", 1000, 100000, 0, 0),
        ("video_transmitter", 1000, 300000, 950, 290000),
    ],
}


def _dump(
    ts, sessions=2, fps=FPS, tid=101, tx=False, lost=0, pacing=(), dma=(), **window
):
    """The collector records of one stats dump of one instance; *window* marks it
    traced=, timed= (percentiles=False for a build without them) or session_timed=.

    *pacing* holds the (TX pacing counter, value) pairs of each session, *dma* the
    packets that DMA copied of each session that has a DMA device. The call
    times grow by 0.1 us a dump from 110 s, the session times from 150 s, session
    1's 0.1 us above session 0's.
    """
    d = "TX" if tx else "RX"
    t = int(ts)
    lines = [
        f"{t} L {pc.DUMP_START}",
        f"{t} L SCH(1:sch_1): tasklets 3, lcore 5(t_pid: {tid}), avg loop 500 ns",
        f"{t} L DEV(0): Avr rate, tx: 10.0 Mb/s, rx: 41000.0 Mb/s, pkts, tx: 100, rx: 50000",
    ]
    for s in range(sessions):
        lines.append(f"{t} L {d}_VIDEO_SESSION(1,{s}:app_{s}): fps {fps} frames 600")
        lines.append(
            f"{t} L {d}_VIDEO_SESSION(1,{s}): throughput 2600 Mb/s, cpu busy 3.5"
        )
    for s in range(sessions):
        lines += [f"{t} L TX_VIDEO_SESSION(1,{s}): {n} {v}" for n, v in pacing]
    for s, pkts in enumerate(dma):
        lines.append(
            f"{t} L RX_VIDEO_SESSION(1,{s}): pkts {pkts} by dma copy, dma busy 0.000000, "
            "flush timeouts 0"
        )
    if lost:
        lines += [
            f"{t} L {d}_VIDEO_SESSION(1,0): incomplete frames {lost}, pkts (x)",
            f"{t} L DEV(0): Status: rx_hw_dropped_packets {lost} rx_err_packets 0",
        ]
    if window.get("timed"):
        n = int((ts - 110 * S) / (10 * S))
        pct = f", p90 {2 + n / 10:.2f}us p95 {3 + n / 10:.2f}us p99 {4 + n / 10:.2f}us"
        pct = pct if window.get("percentiles", True) else ""
        for k, (name, *_) in enumerate(TASKLETS[d]):
            lines.append(
                f"{t} L SCH(1,{k}): tasklet {name}, avg {1 + n / 10:.2f}us max "
                f"{10 + n:.2f}us min {0.05 * (n + 1):.2f}us{pct}"
            )
    if window.get("session_timed"):
        n = int((ts - 150 * S) / (10 * S))
        for s in range(sessions):
            lines.append(
                f"{t} L {d}_VIDEO_SESSION(1,{s}): tasklet time avg "
                f"{0.5 + (n + s) / 10:.2f}us max {5 + n + s:.2f}us min "
                f"{0.02 * (n + s + 1):.2f}us"
            )
    if window.get("traced"):
        n = int((ts - 70 * S) / (10 * S)) + 1  # cumulative: grows by 1 per dump
        for name, cnt, ns, wcnt, wns in TASKLETS[d]:
            lines.append(f"{t} T 1 {name} {cnt * n} {ns * n} {wcnt * n} {wns * n}")
    lines.append(f"{t} L {pc.DUMP_END}")
    return lines


LAST = 190  # the dump that windows() drops, as the application was stopping
PROC0 = (10 * S, {101: (0, 0), 102: (0, 0)})
PROC1 = (60 * S, {101: (500, 10), 102: (50, 5), 103: (99, 0)})
RDT = (0, 0), (5 * 10**9, 7e6)


def _records(traced=True, timed_to=140, pacing=lambda ts: (), **kw):
    """A run's records: a dump every 10 s from 20 s, untraced to 60 s, traced 70-100 s,
    timed 110-*timed_to* s, session timed from 150 s."""
    records = []
    for ts in range(20, LAST + 1, 10):
        window = dict(traced=traced and 70 <= ts <= 100, timed=110 <= ts <= timed_to)
        records += _dump(
            ts * S, **window, session_timed=ts >= 150, pacing=pacing(ts), **kw
        )
    return records


def _tx_records(late=(), late_at=30):
    """A TX run: 3 warm-up re-plans a session in each dump, *late* at *late_at* s."""

    def pacing(ts):
        return [(pc.TX_REPLAN, 3), *(late if ts == late_at else ())]

    return _records(tx=True, pacing=pacing)


def _windows(records=None, t0=PROC0[0], t1=PROC1[0], **kw):
    return pc.windows(pc.parse_dumps(records or _records(**kw)), t0, t1)


def _metrics(dumps, role="RX", rdt=(None, None), pmu=None):
    return pc.metrics(role, dumps, PROC0, PROC1, 100, *rdt, pmu)


def test_parse_dumps_reads_one_dump_and_ignores_what_lies_outside():
    records = [
        "1 T 1 rvs_pkt_rx 1 1 1 1",  # before any dump
        "2 L SCH(1:sch_1): tasklets 3, lcore 5(t_pid: 9), avg loop 1 ns",
        *_dump(10 * S),
        "11 L RX_VIDEO_SESSION(1,7:app_7): fps 1.0 frames 1",  # after the dump
        f"12 L {pc.DUMP_START}",  # a dump the run cut short
    ]
    records[-3:-3] = [
        f"{10 * S} L DEV(1): Avr rate, tx: 1.0 Mb/s, rx: 1.0 Mb/s, pkts, tx: 1, rx: 2",
        f"{10 * S} L RX_VIDEO_SESSION(1,0): incomplete frames 3, pkts (idx error: 0)",
        f"{10 * S} L DEV(0): Status: rx_hw_dropped_packets 7 rx_err_packets 0",
        f"{10 * S} T 1 rvs_pkt_rx 2 20 1 10",
        f"{10 * S} T 1 rvs_pkt_rx 3 30 2 20",
        f"{10 * S} L RX_VIDEO_SESSION(1,0): pkts 7 by dma copy, dma busy 0.25, flush",
        f"{10 * S} L RX_VIDEO_SESSION(2,1): tasklet time avg 0.27us max 9.10us min 0.05us",
        f"{10 * S} L {pc.DUMP_END}",
    ]
    (d,) = pc.parse_dumps(records)
    assert d.ts == 10 * S and d.sch == {1: (500, 101)}
    assert d.fps == {("RX", "1", "0"): FPS, ("RX", "1", "1"): FPS}
    assert d.busy == {("RX", "1", "0"): 3.5, ("RX", "1", "1"): 3.5}
    assert d.dev == [11.0, 41001.0, 50002]  # both ports
    assert d.counts == {"incomplete frames": 3, "hw dropped pkts": 7}
    assert d.tasklets == {(1, "rvs_pkt_rx"): [5, 50, 3, 30]}
    assert d.session_times == {("RX", 2, 1): {"avg": 0.27, "max": 9.1, "min": 0.05}}
    assert not d.tasklet_times
    assert d.dma == {("1", "0"): 7}


def test_is_steady_needs_every_session_at_rate():
    (d,) = pc.parse_dumps(_dump(S, sessions=2))
    assert pc.is_steady(d, "RX", 2, FPS)
    assert not pc.is_steady(d, "RX", 3, FPS)
    assert not pc.is_steady(d, "TX", 2, FPS)
    (slow,) = pc.parse_dumps(_dump(S, fps=FPS * 0.98))
    assert not pc.is_steady(slow, "RX", 2, FPS)


def test_windows_drop_a_last_traced_dump_short_of_schedulers():
    dumps = pc.parse_dumps(_records())
    for d in dumps:  # two schedulers, but the trace stopped as the 100 s dump printed
        if d.tasklets and d.ts < 100 * S:
            d.tasklets[(2, "rvs_pkt_rx")] = list(d.tasklets[(1, "rvs_pkt_rx")])
    _, traced, *_ = pc.windows(dumps, PROC0[0], PROC1[0])
    assert [d.ts // S for d in traced] == [70, 80, 90]
    _, traced, *_ = _windows()  # all alike: all kept
    assert [d.ts // S for d in traced] == [70, 80, 90, 100]


def test_windows_split_the_run_at_the_proc_reads_and_drop_the_last_dump():
    untraced, traced, timed, session_timed = _windows()
    assert [d.ts // S for d in untraced] == [20, 30, 40, 50, 60]
    assert [d.ts // S for d in traced] == [70, 80, 90, 100]
    assert [d.ts // S for d in timed] == [120, 130, 140]  # 110 started untimed
    assert [d.ts // S for d in session_timed] == [160, 170, 180]  # and 150
    # the dump that ends the timed window holds the first session times: in its window
    *_, timed, session_timed = _windows(timed_to=150)
    assert [d.ts // S for d in timed] == [120, 130, 140, 150]
    assert [d.ts // S for d in session_timed] == [160, 170, 180]
    untraced, *_ = _windows(t0=25 * S, t1=50 * S)
    assert [d.ts // S for d in untraced] == [40, 50]


def test_metrics_of_an_instance():
    dumps = _windows()
    untraced = dumps[0]
    untraced[1].fps[("RX", "1", "1")] = 50.0  # one slow dump does not make the median
    del untraced[2].fps[("RX", "1", "0")]  # one lost line counts as 0 fps there
    m = _metrics(dumps, rdt=RDT)
    assert m["sch_1 avg loop (ns)"] == 500
    assert m["scheduler threads (cores)"] == pytest.approx(0.1)  # tid 101 only
    assert m["other threads (cores)"] == pytest.approx(0.01)  # 103 not in both reads
    assert m["sch preemptions (/s)"] == pytest.approx(0.2)  # of the schedulers only
    assert m["memory bandwidth (MB/s)"] == pytest.approx(100)
    assert m["LLC occupancy (MB)"] == pytest.approx(7)
    assert m["fps (worst session)"] == pytest.approx(FPS)
    assert m["RX rate (Mb/s)"] == 41000.0
    # traced: 70 s is the base, 80-100 s add 3 × the per-dump increments
    assert m["sch_1 avg loop, traced (ns)"] == 500
    assert m["rvs_pkt_rx avg (ns/call)"] == pytest.approx(400)
    assert m["rvs_pkt_rx work (ns/call)"] == pytest.approx(2000)
    assert m["rvs_pkt_rx work (% of a core)"] == pytest.approx(100 * 600000 / (30 * S))
    assert m["RX work (ns/pkt)"] == pytest.approx(600000 / (3 * 50000))
    for d in untraced[:3]:  # a session gone from most dumps fails the median
        d.fps.pop(("RX", "1", "0"), None)
    m = _metrics(dumps)  # and no RDT
    assert m["fps (worst session)"] == 0.0 and "memory bandwidth (MB/s)" not in m


def test_metrics_of_a_tx_instance_count_its_late_frames_in_the_window():
    late = [("build timeout frames", 1), ("epoch drop", 4)]
    dumps = pc.parse_dumps(_tx_records(late))
    assert dumps[1].counts == {
        pc.TX_REPLAN: 6,
        "build timeout frames": 2,
        "epoch drop": 8,
    }
    m = _metrics(pc.windows(dumps, PROC0[0], PROC1[0]), "TX")
    rows = [*pc.TX_LATE, f"{pc.TX_REPLAN} (/s)"]
    assert [m[k] for k in rows] == pytest.approx([2, 8, 0, 6 * 5 / 50])
    m = _metrics(_windows(_tx_records(late, 80)), "TX")  # in the traced window
    assert [m[k] for k in pc.TX_LATE] == [0, 0, 0]
    assert not [k for k in rows if k in _metrics(_windows())]


def test_metrics_sum_up_the_call_times_of_the_timed_window():
    new = _metrics(_windows(tx=True), "TX")
    stats = ("avg", "p90", "p95", "p99", "min", "max")
    # 120-140 s: the median of avg and the percentiles, the least min, the largest max
    for name in ("tx_video_sessions_mgr", "video_transmitter"):
        got = [new[f"sch_1 {name} {s}, timed (us)"] for s in stats]
        assert got == pytest.approx([1.2, 2.2, 3.2, 4.2, 0.1, 13])
    # a build older than the percentiles: the rest of its line, None for them
    old = _metrics(_windows(tx=True, percentiles=False), "TX")
    row = "sch_1 video_transmitter"
    assert [old[f"{row} {s}, timed (us)"] for s in stats] == [1.2, *[None] * 3, 0.1, 13]
    # beside a newer build or one as old, they read n/a among their tasklet's rows
    for a, b in ((old, new), (old, old)):
        rows = pc.compare([{"A": {"tx": a}, "B": {"tx": b}}], "tx")
        names = [r.metric for r in rows if r.metric.startswith(row)]
        assert names == [f"{row} {s}, timed (us)" for s in stats]
        (p99,) = [pc.cells(r) for r in rows if r.metric == f"{row} p99, timed (us)"]
        assert p99[1:3] == ["n/a", "4.20" if b is new else "n/a"]


def test_metrics_take_the_session_times_of_each_scheduler():
    untraced, traced, timed, session_timed = _windows()
    without = _metrics((untraced, traced, timed, ()))
    assert not [k for k in without if "session timed" in k]
    m = _metrics((untraced, traced, (), session_timed))
    # 160-180 s, both sessions: the median of their avg, the least min, the largest max
    row = "sch_1 RX video session {}, session timed (us)"
    stats = ("avg", "min", "max")
    assert [k for k in m if "session timed" in k] == [row.format(s) for s in stats]
    assert [m[row.format(s)] for s in stats] == pytest.approx([0.75, 0.04, 9])
    # each scheduler on its own, and none of the other role's sessions
    session_timed[0].session_times[("RX", 3, 0)] = {"avg": 2.0, "max": 3.0, "min": 1.0}
    session_timed[0].session_times[("TX", 1, 0)] = {"avg": 99, "max": 99, "min": 0.01}
    m = _metrics((untraced, traced, (), session_timed))
    sch_3 = [m[f"sch_3 RX video session {s}, session timed (us)"] for s in stats]
    assert sch_3 == [2.0, 1.0, 3.0]
    assert [m[row.format(s)] for s in stats] == pytest.approx([0.75, 0.04, 9])


# perf stat -x, of the 50 s between PROC0 and PROC1, every row a different value
PMU = {
    "task-clock": 2.56e5,
    "cycles": 6.4e11,
    "instructions": 9.6e11,
    "cache-references": 6e9,
    "cache-misses": 1.5e9,
}
PMU_ROWS = [
    "IPC",
    "core clock (GHz)",
    "LLC hits (M/s)",
    "LLC misses (M/s)",
    "LLC miss ratio (%)",
]


def test_metrics_take_the_pmu_rows_over_the_proc_window():
    windows = _windows()
    m = _metrics(windows, pmu=PMU)
    assert [m[k] for k in PMU_ROWS] == pytest.approx([1.5, 2.5, 90.0, 30.0, 25.0])
    for event in PMU:  # an event perf could not count gives no rows, not wrong ones
        for pmu in ({**PMU, event: 0}, {k: v for k, v in PMU.items() if k != event}):
            assert not [k for k in PMU_ROWS if k in _metrics(windows, pmu=pmu)]


def test_parse_perf_reads_what_perf_stat_counted():
    # perf 6.8 on the perf pair: a header with -o, an error, the counts with -x,
    text = (
        "# started on Tue Oct 20 07:51:32 2026\n"
        "\n"
        "Error: no permission, an error line, of words\n"
        "7835153,ns,duration_time,7835153,100.00,1.077,G/sec\n"
        "7.28,msec,task-clock,7278136,100.00,0.929,CPUs utilized\n"
        "<not counted>,,cycles,0,100.00,,\n"
        "<not supported>,,cache-misses,0,100.00,,\n"
    )
    assert pc.parse_perf(text) == {"duration_time": 7835153, "task-clock": 7.28}


def test_perf_tool_takes_the_first_perf_that_counts_every_event(monkeypatch, tmp_path):
    def sh(host, script, check=True):  # the script, with the host's /usr in tmp_path
        return _bash(host, script.replace("/usr/", f"{tmp_path}/"), check=False)

    def perf(path, counts):  # answers only the probe, with *counts* on stderr, rc 0
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(
                "#!/bin/sh\n"
                f"[ \"$*\" = 'stat -x, -e {pc.PMU_EVENTS} -- true' ] || exit 9\n"
                f"printf '{counts}' >&2\n"
            )
        os.chmod(path, 0o755)

    counted = "".join(f"7,,{e},1,100.00,,\\n" for e in pc.PMU_EVENTS.split(","))
    uncounted = counted.replace("7,,cache-misses", "<not counted>,,cache-misses")
    monkeypatch.setattr(pc, "sh", sh)
    # Ubuntu's wrapper, without the tools of the running kernel
    perf(f"{tmp_path}/bin/perf", "WARNING: perf not found for kernel 6.8.0-137\\n")
    assert pc.perf_tool(None) is None
    tools = f"{tmp_path}/lib/linux-tools/6.8.0-{{}}-generic/perf"
    perf(tools.format(99), counted)
    perf(tools.format(136), counted)
    assert pc.perf_tool(None) == tools.format(136)
    perf(tools.format(136), uncounted)
    assert pc.perf_tool(None) == tools.format(99)
    perf(f"{tmp_path}/bin/perf", counted)
    assert pc.perf_tool(None) == f"{tmp_path}/bin/perf"


def _rounds(*pairs, side="rx", metric="x"):
    return [{"A": {side: {metric: a}}, "B": {side: {metric: b}}} for a, b in pairs]


def _row(*pairs, metric="x"):
    (row,) = pc.compare(_rounds(*pairs, metric=metric), "rx")
    return row


def _valid(rows: dict, n: int = 2) -> list[dict]:
    """*n* valid rounds of the RX *rows*, {metric: (A, B)}, each 1% above the last; None
    for a build without the row."""
    return [
        {
            inst: {"rx": {k: v[i] and v[i] * 1.01**r for k, v in rows.items()}}
            for i, inst in enumerate("AB")
        }
        for r in range(n)
    ]


def test_compare_takes_the_mean_of_rounds_and_the_larger_spread_as_noise():
    row = _row((100, 110), (102, 112))
    assert row.diff == pytest.approx(100 * 10 / 101)
    assert row.noise == pytest.approx(100 * 2 / 101)
    assert pc.cells(row) == ["x", "101", "111", "+9.9%", "±2.0%"]


def test_a_difference_that_is_no_change_reads_as_none():
    assert pc.cells(_row((100, 102), (110, 104)))[3] == "~ (-1.9%)"  # under 2%
    assert pc.cells(_row((100, 106), (104, 110)))[3] == "~ (+5.9%)"  # under 2 × noise
    assert pc.cells(_row((100, 106), (103, 109)))[3] == "+5.9%"
    assert pc.cells(_row((0, 5), (0, 0)))[3] == "~ (from 0)"  # within ±200%
    assert pc.cells(_row((0, 5), (0, 7)))[3] == "from 0"


def test_a_metric_missing_on_one_side_has_no_difference():
    rounds = [{"A": {"rx": {"x": 1.0, "y": 2.0}}, "B": {"rx": {"x": 1.0}}}]
    rows = {r.metric: r for r in pc.compare(rounds, "rx")}
    assert pc.cells(rows["y"]) == ["y", "2", "n/a", "n/a", "n/a"]
    assert pc.compare(rounds, "tx") == []


def test_a_change_is_2_percent_and_twice_the_noise_of_the_candidate():
    def outcome(*pairs, metric="x"):
        return pc.outcome(_row(*pairs, metric=metric))

    assert outcome((100, 110), (102, 112)) == "regression"  # +9.9% against ±2.0%
    assert outcome((110, 100), (112, 102)) == "improvement"
    assert outcome((100, 106), (104, 110)) == "unchanged"  # +5.9%, under 2 × ±3.9%
    assert outcome((100, 106), (103, 109)) == "regression"  # +5.9%, just 2 × ±3.0%
    assert outcome((100, 101.9), (100, 101.9)) == "unchanged"  # no noise, under 2%
    assert outcome((100, 102), (100, 102)) == "regression"
    assert outcome((100, 110)) == "n/a"  # one round, no noise estimate
    assert outcome((100, 101)) == "unchanged"  # one round, but under 2%
    assert outcome((None, 1), (None, 1)) == "n/a"
    # 0 on either side, each the mirror of the other
    assert outcome((0, 5), (0, 7)) == "regression"
    assert outcome((5, 0), (7, 0)) == "improvement"
    assert outcome((0, 5), (0, 0)) == "unchanged"  # +100% within 2 × ±200%
    assert outcome((5, 0), (0, 0)) == "unchanged"
    assert outcome((0, 0), (0, 0)) == "unchanged"
    # the frame rate better higher, also from 0; no direction for IPC or the clock
    fps = "fps (worst session)"
    assert outcome((59.94, 50), (59.94, 50), metric=fps) == "regression"
    assert outcome((0, 50), (0, 50), metric=fps) == "improvement"
    for metric in ("IPC", "core clock (GHz)", "TX rate (Mb/s)", "LLC miss ratio (%)"):
        assert outcome((1.0, 1.2), (1.0, 1.2), metric=metric) == "differs"
        assert outcome((1.0, 1.01), (1.0, 1.01), metric=metric) == "unchanged"
    for stat in ("avg", "min", "max"):
        assert outcome((1, 2), (1, 2), metric=f"sch_1 t {stat}, timed (us)") == (
            "regression"
        )
    # no less noise than one step of what MTL prints: 10 ns of a call, 1 ns of a loop
    timed = "sch_1 t min, timed (us)"
    for pair in ((0.14, 0.13), (0.13, 0.14), (0.29, 0.30), (0.30, 0.29)):
        assert outcome(pair, pair, metric=timed) == "unchanged"
    assert outcome((0.16, 0.19), (0.16, 0.19), metric=timed) == "regression"
    # two steps, a float above them as 0.31 - 0.29 is, are not twice the noise
    assert outcome((0.29, 0.31), (0.29, 0.31), metric=timed) == "unchanged"
    assert outcome((9, 10), (9, 10), metric="sch_0 avg loop (ns)") == "unchanged"
    session = "sch_1 RX video session min, session timed (us)"
    assert outcome((0.13, 0.15), (0.13, 0.15), metric=session) == "unchanged"
    # and no more: means of 100 and 102.5 ns, twice a step and more, differ
    loop = "sch_1 avg loop (ns)"
    assert outcome((100, 102), (100, 103), metric=loop) == "regression"


def test_a_change_is_as_sure_as_it_is_far_from_its_noise():
    # 4 times its noise; each round's own difference, the slot swap in it
    r = _row((100, 108), (102, 110))
    assert pc.confidence(r) == "medium" and r.rounds == pytest.approx((8, 800 / 102))
    assert pc.confidence(_row((100, 110), (102, 112))) == "high"  # 5 times
    assert pc.confidence(_row((100, 105), (102, 107))) == "low"  # 2.5 times
    assert pc.confidence(_row((100, 100), (100, 101))) == ""  # no change
    assert pc.confidence(_row((100, 102), (100, 102))) == "high"  # no noise at all
    # each round's own difference, whatever their signs
    r = _row((100, 1000), (100, 99))
    assert pc.outcome(r) == "regression" and pc.confidence(r) == "low"
    assert (
        pc._per_round(r) == "+900% / -1.0%" and pc._per_round(_row((1, 2))) == "+100%"
    )
    assert pc._per_round(_row((None, 2), (None, 2))) == "n/a"
    # the noise: twice the standard error of the mean, the difference of two rounds,
    # which more rounds of the same spread narrow
    assert pc._noise([100, 103]) == 100 * 3 / 101.5
    assert pc._noise([100, 103, 100, 103]) == pytest.approx(100 * 3**0.5 / 101.5)
    assert pc._noise([100]) is None and pc._noise([0, 0]) is None
    # how a row reads; one that differs with no better direction turns grey
    rows = {
        "x": (100, 110),
        "y": (100, 100.2),
        "w": (None, 1),
        "LLC hits (M/s)": (1, 2),
    }
    marks = {r.metric: pc._mark(r) for r in pc.compare(_valid(rows), "rx")}
    assert marks == {
        "x": ("r", "❌ regression (high)"),
        "y": ("", "unchanged"),
        "w": ("", "n/a"),
        "LLC hits (M/s)": ("d", "➖ differs (high)"),
    }
    (ipc,) = pc.compare(_valid({"IPC": (1, 2)}), "rx")
    assert '<tr class="d"><td>IPC</td>' in pc._cmp_table([("IPC", ipc)], NAMES)
    assert "<td>a&lt;b</td>" in pc._cmp_table([("a<b", ipc)], NAMES)
    directions = [pc._direction(m) for m in ("fps (worst session)", "IPC", "x")]
    assert directions == [" ↑", "", " ↓"]


def test_values_show_the_digits_that_tell_them_apart():
    values = (0.0195, 0.02, 0.3, 0.0000167, 2.181, 59.9, 123.4, 7.0, 0.0, None)
    shown = "0.0195 0.0200 0.300 0.0000167 2.18 59.90 123 7 0 n/a"
    assert " ".join(map(pc._fmt, values)) == shown
    # two values that print alike get the decimals that tell them apart
    for a, b, shown in (
        (302.0, 302.4, ["302.0", "302.4"]),
        (2.181, 2.184, ["2.181", "2.184"]),
        (302.0, 302.0, ["302", "302"]),
        (1000.0, 1000.4, ["1,000.0", "1,000.4"]),  # thousands apart
    ):
        assert pc.cells(pc.Row("x", a, b, 100 * (b - a) / a, 0.0))[1:3] == shown
    assert pc._fmt(61902.4) == "61,902" and pc._fmt(-1500.0) == "-1,500"
    # but not two means that differ only in float rounding
    for pairs, shown in (
        (((144.33, 144.32), (223.25, 223.26)), ["184", "184"]),
        (((0.10, 0.12), (0.14, 0.12)), ["0.120", "0.120"]),
    ):
        row = _row(*pairs)
        assert row.a != row.b and pc.cells(row)[1:3] == shown


def test_the_builds_go_by_their_names_and_by_their_roles_when_alike():
    assert pc.build_names(NAMES) == NAMES
    alike = pc.build_names({"A": "main 1", "B": "main 1"})
    assert alike == {"A": "main 1 (baseline)", "B": "main 1 (candidate)"}


def test_the_key_rows_are_the_costs_of_a_role():
    for m in (
        "sch_12 avg loop (ns)",
        "video cpu busy (%)",
        "scheduler threads (cores)",
        "memory bandwidth (MB/s)",
        "LLC misses (M/s)",
        "RX work (ns/pkt)",
    ):
        assert pc.KEY_ROWS.fullmatch(m), m
    for m in ("sch_1 avg loop, traced (ns)", "other threads (cores)", "LLC hits (M/s)"):
        assert not pc.KEY_ROWS.fullmatch(m), m
    stats = ("avg", "p90", "p95", "p99", "min", "max")
    row = "sch_2 rvs_pkt_rx {}, timed (us)"
    timed = [s for s in stats if pc.TASKLET_ROWS.fullmatch(row.format(s))]
    assert timed == ["avg", "p99"]


def test_every_row_of_a_run_reads_in_a_group_of_its_own():
    rows = {
        **_metrics(_windows(), "RX", RDT, PMU),
        **_metrics(_windows(tx=True), "TX", RDT, PMU),
    }
    want = {*pc.TX_LATE, f"{pc.TX_REPLAN} (/s)", "TX rate (Mb/s)", "RX work (ns/pkt)"}
    assert want <= rows.keys(), want - rows.keys()
    groups = pc._groups(pc.compare([{"A": {"rx": rows}, "B": {"rx": rows}}], "rx"))
    assert [title for title, _ in groups] == [
        "Delivery",
        "Scheduler loops",
        "CPU",
        "Memory and cache",
        "Tasklets, 1 loop in 64 timed",
        pc.TIMED,
        pc.SESSIONS,
    ]


def test_the_rows_of_a_role_read_in_groups():
    names = [
        "sch_1 t max, timed (us)",
        "sch_1 avg loop (ns)",
        "IPC",
        "fps (worst session)",
        "sch_2 s avg, timed (us)",
        "x",
        "sch_1 s avg, timed (us)",
        "memory bandwidth (MB/s)",
        "t avg (ns/call)",
        "sch_1 RX video session min, session timed (us)",
        "hw dropped pkts",
    ]
    groups = pc._groups(pc.compare(_valid({m: (1, 1) for m in names}), "rx"))
    assert [(title, [r.metric for r in items]) for title, items in groups] == [
        ("Delivery", ["fps (worst session)", "hw dropped pkts"]),
        ("Scheduler loops", ["sch_1 avg loop (ns)"]),
        ("CPU", ["IPC"]),
        ("Memory and cache", ["memory bandwidth (MB/s)"]),
        ("Tasklets, 1 loop in 64 timed", ["t avg (ns/call)"]),
        (pc.TIMED, [names[0], names[4], names[6]]),
        (pc.SESSIONS, ["sch_1 RX video session min, session timed (us)"]),
        ("Other", ["x"]),
    ]


NAMES = {"A": "main", "B": "branch"}


def _summary(rows: dict, n: int = 2) -> str:
    return pc._summary(_valid(rows, n), {"rx": "s"}, NAMES)


def _verdict(rows: dict, n: int = 2) -> tuple:
    """The class and the first line of the verdict on *rows* of one role."""
    page = _summary(rows, n)
    return re.search(r'<div class="(verdict ?\w?)"><b>([^<]*)</b>', page).groups()


def test_the_summary_leads_with_the_verdict_and_the_largest_changes():
    loop, bw, same = "sch_1 avg loop (ns)", "memory bandwidth (MB/s)", (100, 100.2)
    assert _verdict({loop: (100, 90), bw: same}) == (
        "verdict i",
        "✅ branch improves 1 of the 2 key metrics and regresses none.",
    )
    assert _verdict({loop: (100, 110), bw: same}) == (
        "verdict r",
        "❌ branch regresses 1 of the 2 key metrics and improves none.",
    )
    assert _verdict({loop: (100, 90), bw: (100, 120)}) == (
        "verdict r",
        "❌ branch regresses 1 of the 2 key metrics and improves 1.",
    )
    assert _verdict({loop: same, bw: same}) == (
        "verdict",
        "branch changes none of the 2 key metrics.",
    )
    assert _verdict({loop: (100, 90)}, n=1) == (
        "verdict",
        "One valid round gives no noise estimate, so no change is scored.",
    )
    # shorter loops on more schedulers, as of a lower --sch_session_quota
    threads = "scheduler threads (cores)"
    assert _verdict({loop: (100, 80), threads: (3, 5)})[1] == (
        "❌ branch regresses 1 of the 2 key metrics and improves 1."
    )
    # the largest of each kind, the losses, the rule; the regressions first, by size
    rows = {loop: (100, 80), bw: (100, 95), "LLC misses (M/s)": (100, 130)}
    rows.update({threads: (3, 3.3), "video cpu busy (%)": (10, 10)})
    rows["fps (worst session)"] = (59.9, 59.9)  # no key row: none of the verdict
    page = _summary(rows)
    worst = "Worst: RX · LLC misses (M/s) +30.0% (high)."
    assert f"{worst} Best: RX · sch_1 avg loop (ns) -20.0% (high).<br>" in page
    fps = "No loss or late frames; the worst session at RX 60.20 → 60.20 fps."
    assert fps in page
    rule = "2 valid rounds, the slots swapped each round. A change is at least 2% "
    rule += "and 2 times the noise, twice the standard error of a build&#x27;s mean"
    assert rule in page and "Under 4 rounds the noise is a rough estimate" in page
    labels = ["LLC misses (M/s)", threads, loop, bw]
    order = [page.index(f"<td>RX · {m} ↓</td>") for m in labels]
    assert order == sorted(order)
    fold = '<details class="fold"><summary>1 unchanged</summary>'
    assert fold in page and page.index(fold) < page.index("<td>RX · video cpu busy")
    assert "regresses 2 of the 5 key metrics and improves 2." in page
    page = _summary(rows, 4)
    assert "4 valid rounds" in page and "Under 4 rounds" not in page
    assert "<br>No loss or late frames.<br>" in _summary({bw: (100, 100)})
    assert _verdict({"x": (1, 2)}) == ("verdict", "The builds share no key metric.")
    assert "Key metrics" not in _summary({"x": (1, 2)})
    # a loss row once a build counted any
    lost = {loop: (100, 90), "hw dropped pkts": (0, 6), "incomplete frames": (0, 0)}
    lost["epoch drop"] = (0, 2)
    assert _verdict(lost) == (
        "verdict r",
        "❌ branch regresses 2 of the 3 key metrics and improves 1.",
    )
    page = _summary(lost)
    loss = (
        "⚠️ Loss or late frames: RX hw dropped pkts 0 → 6.03; RX epoch drop 0 → 2.01."
    )
    assert loss in page and '<tr class="r"><td>RX · hw dropped pkts ↓</td>' in page
    # a loss from 0 has no size: none of the worst, and its change says from 0
    best = "Best: RX · sch_1 avg loop (ns) -10.0% (high)."
    assert "Worst" not in page and best in page
    assert "<td><b>0</b></td><td>6.03</td><td>from 0</td>" in page
    assert "incomplete frames" not in page
    # a tasklet's p99 once both builds have it, outside the verdict
    p99 = "sch_1 t p99, timed (us)"
    tasklets = "<h3>Tasklet call times, avg and p99, outside the verdict</h3>"
    page = _summary({loop: (100, 90), p99: (None, 5)})
    assert "improves 1 of the 1 key metrics" in page and tasklets not in page
    page = _summary({loop: (100, 90), p99: (5, 6)})
    assert "improves 1 of the 1 key metrics" in page and tasklets in page
    assert '<tr class="r"><td>RX · t on sch_1, p99 (us) ↓</td>' in page
    # the key rows only, after their role; every row in the count
    rows = {loop: (100, 90), "IPC": (1.0, 2.0), "x": (100, 110), "y": (1, None)}
    rows["RX work (ns/pkt)"] = (None, 5)  # a key row that A lacks
    page = _summary(rows)
    assert '<tr class="i"><td>RX · sch_1 avg loop (ns) ↓</td>' in page
    assert "RX · IPC" not in page and "RX · x" not in page and "RX · RX" not in page
    tally = (
        "❌ regression: 1 · ✅ improvement: 1 · ➖ differs: 1 · unchanged: 0 · n/a: 2"
    )
    assert f"All 5 rows: {tally}</div>" in page
    # which build lacks the tasklet rows that the summary leaves out
    assert "No sampled tasklet counters from main: they" in page


def test_a_bar_reads_the_difference_from_the_middle_to_25_percent():
    def bar(diff):
        return pc._bar(pc.Row("x", 1, 1, diff, 0.0))

    assert bar(None) == ""
    assert bar(10) == '<span class="bar"><i style="left:50.0%;width:20.0%"></i></span>'
    assert bar(-10) == '<span class="bar"><i style="left:30.0%;width:20.0%"></i></span>'
    assert bar(25) == '<span class="bar"><i style="left:50.0%;width:50.0%"></i></span>'
    assert bar(-300) == '<span class="bar"><i style="left:0.0%;width:50.0%"></i></span>'


def test_the_call_times_read_as_one_table_of_calls_and_schedulers():
    rows = {
        "sch_2 t avg, timed (us)": (1.0, 2.0),
        "sch_1 t avg, timed (us)": (2.0, 1.0),
        "sch_1 t p99, timed (us)": (None, None),  # no build has it: no column
        "sch_1 u avg, timed (us)": (1.0, 1.0),
        "sch_1 u max, timed (us)": (5.0, 5.0),  # t has no max: n/a
    }
    same = {x: {"rx": {k: v[i] for k, v in rows.items()}} for i, x in enumerate("AB")}
    rows = pc.compare([same, same], "rx")
    page = pc._matrix(rows)
    head = "<th>Call</th>\n<th>Scheduler</th>\n<th>avg (us) ↓</th>\n<th>max (us) ↓</th>"
    assert head in page
    # by call, then by scheduler; a line above the first row of the next call; the
    # better value bold, a cell of no change muted
    t1 = '<tr><td>t</td><td>sch_1</td><td class="i" title="✅ improvement (high); '
    t1 += 'rounds -50% / -50%">2 → <b>1</b><small>✅ -50.0% ±1.0%</small></td>'
    t1 += '<td class="u">n/a</td></tr>'
    t2 = '<tr><td>t</td><td>sch_2</td><td class="r" title="❌ regression (high); '
    t2 += 'rounds +100% / +100%"><b>1</b> → 2<small>❌ +100.0% ±1.0%</small></td>'
    t2 += '<td class="u">n/a</td></tr>'
    same_cell = '<td class="u" title="unchanged; rounds +0.0% / +0.0%">'
    u1 = f'<tr class="next"><td>u</td><td>sch_1</td>{same_cell}1 → 1'
    u1 += f"<small>~ (+0.0%) ±1.0%</small></td>{same_cell}5 → 5"
    u1 += "<small>~ (+0.0%) ±0.2%</small></td></tr>"
    assert page.index(t1) < page.index(t2) < page.index(u1)
    # the note on how a cell reads, once before the first of them
    groups = [("x", rows[:1]), ("y", [pc.Row("z", 1, 1, 0.0, 0.0)]), ("t", rows[1:])]
    page = pc._tables(groups, {"A": "a<1", "B": "b<2"})
    note = '<div class="note">In each cell the baseline → the candidate, the better'
    assert page.count(note) == 1
    assert page.index(note) < page.index('<table class="mx tm">')
    assert page.count('<table class="mx tm">') == 2
    assert "Baseline<br>a&lt;1" in page and "<1" not in page


SLOTS = [
    dict(host="s<t", role="rx", slot=0, cpus="4-7", isolated=True, cat=True, perf=False)
]
LABELS, ARGS = {"A": "main", "B": "a<b"}, {"A": "", "B": "--x"}


def _round(metrics, order=("A", "B"), problems=(), launches=()):
    return dict(
        order=order, metrics=metrics, problems=[*problems], notes=[], launches=launches
    )


class _Tags(HTMLParser):
    """The div, details, table and tr elements left open at the end of *page*; each
    must close the last one open."""

    def __init__(self, page):
        super().__init__()
        self.open = []
        self.feed(page)

    def handle_starttag(self, tag, attrs):
        if tag in ("div", "details", "table", "tr"):
            self.open.append(tag)

    def handle_endtag(self, tag):
        if tag in ("div", "details", "table", "tr"):
            assert self.open.pop() == tag


def test_the_job_summary_holds_the_verdict_and_folds_the_rest():
    rows = {"sch_1 avg loop (ns)": (100, 120), "sch_1 t avg, timed (us)": (1.0, 1.0)}
    rounds = [_round(m, o) for o, m in zip((("A", "B"), ("B", "A")), _valid(rows))]
    invalid = _rounds((1, 99999), metric="zz")[0]  # a row of its own in the tables
    rounds.append(_round(invalid, problems=["rx0 (B): late"]))
    page = pc.summary_html(LABELS, ARGS, SLOTS, rounds)
    # a fragment of no style: the job summary keeps none
    assert page.startswith("<h2>MTL perf-compare: a&lt;b against main</h2>")
    assert "<style" not in page and "<html" not in page
    order = (
        "<td>Extra RxTxApp arguments</td><td>-</td><td>--x</td>",
        "<tr><td>2</td><td>a&lt;b</td><td>main</td><td>valid</td></tr>",
        "<b>❌ a&lt;b regresses 1 of the 1 key metrics and improves none.</b>",
        '<tr class="r"><td>RX · sch_1 avg loop (ns) ↓</td><td><b>',
        "<details><summary><b>RX (s&lt;t): every row</b></summary>",
        "<h3>Tasklet call times, every call timed</h3>",
    )
    assert sorted(order, key=page.index) == list(order)
    assert "Test Information" not in page and "Instance Details" not in page
    assert "99,999" not in page and "<td>rx0 (B): late</td>" in page  # left out, named
    # a legend of what shows without style: no grey, no bars
    assert "The better value is bold." in page
    assert "in grey" not in page and "The bar of a change" not in page
    assert _Tags(page).open == []


def test_report_html_opens_the_setup_folds_the_instances_and_escapes_every_text():
    a_rx0 = dict(label="A", role="rx", slot=0, script="cd /m && exec RxTxApp '<y>'")
    a_rx0["config"] = {"k": "<v>"}
    a_rx1 = {**a_rx0, "slot": 1, "script": "a1", "config": {"slot": 1}}
    timed = _rounds((2.0, 3.0), metric="sch_1 t max, timed (us)")[0]
    timed["A"]["rx"]["sch_1 t avg, timed (us)"] = 1.0  # and no percentiles
    session = "sch_2 RX video session avg, session timed (us)"
    timed["A"]["rx"][session], timed["B"]["rx"][session] = 0.25, 0.5
    timed["B"]["rx"].update(
        {"sch_1 t avg, timed (us)": 1.0, "sch_1 t p90, timed (us)": 1.5}
    )
    x = _rounds((100, 110))[0]
    first = {k: {"rx": {**v["rx"], **timed[k]["rx"]}} for k, v in x.items()}
    swapped = [{**a_rx0, "label": "B"}, a_rx1]
    rounds = [
        _round(first, launches=[a_rx0]),
        _round(_rounds((1, 99999))[0], ("B", "A"), ["rx0 (B): <script>"], swapped),
        _round(x, launches=[a_rx0]),
    ]
    # the host's MTL and DPDK, neither build's
    sut_sw = {"kernel": "6.8<", "mtl_version": "host-mtl", "dpdk_driver": "host-dpdk"}
    sut_platform = {"sw_configuration": sut_sw, "hw_configuration": {"cpu": "Xeon"}}
    platforms = [("s<t (RX)", sut_platform), ("c (TX)", {})]
    details = [("Video", "1080p59 <GPM>")]
    page = pc.report_html(LABELS, ARGS, details, SLOTS, rounds, platforms)
    assert "<script>" not in page and "rx0 (B): &lt;script&gt;" in page
    assert "MTL perf-compare: a&lt;b against main" in page
    sut = page.index("SW Configuration — s&lt;t (RX)")
    assert sut < page.index("SW Configuration — c (TX)")
    assert "<td>Kernel</td><td>6.8&lt;</td>" in page  # the SUT's, not the companion's
    assert "<td>Kernel</td><td>N/A</td>" in page
    for row in ("MTL Version", "DPDK Version"):
        assert page.count(f"<td>{row}</td><td>per build, see Builds</td>") == 2
    assert "host-mtl" not in page and "host-dpdk" not in page
    assert "<td>CPU</td><td>Xeon</td>" in page and "<td>CPU</td><td>N/A</td>" in page
    placement = "<td>s&lt;t</td><td>RX</td><td>0</td><td>4-7</td><td>yes</td>"
    assert f"{placement}<td>yes</td><td>no</td>" in page
    # every instance as it ran, once, with the rounds it ran in
    a0 = "<summary>main RX in slot 0 on s&lt;t, rounds 1, 3</summary>"
    a1 = "<summary>main RX in slot 1 on s&lt;t, round 2</summary>"
    b0 = "<summary>a&lt;b RX in slot 0 on s&lt;t, round 2</summary>"
    assert page.index(a0) < page.index(a1) < page.index(b0)
    script = "<code>cd /m &amp;&amp; exec RxTxApp &#x27;&lt;y&gt;&#x27;</code>"
    config = "<pre><code>{\n  &quot;k&quot;: &quot;&lt;v&gt;&quot;\n}</code></pre>"
    assert page.count(script) == 2 and page.count(config) == 2
    # the builds and the results first, then the setup open and the instances folded
    for title in ("Test Information", "Platform &amp; Topology"):
        assert f"<details open><summary><b>{title}</b></summary>" in page
    assert "<details><summary><b>Instance Details</b></summary>" in page
    assert page.count("<details open>") == 2 and page.count("<details>") == 1 + 3
    order = (
        "<h2>Builds</h2>",
        "<td>Build</td><td>main</td><td>a&lt;b</td>",
        "<h2>Rounds</h2>",
        "<h2>Summary</h2>",
        "<h2>RX (s&lt;t)</h2>",
        "<b>Test Information</b>",
        "<td>Video</td><td>1080p59 &lt;GPM&gt;</td>",
        "<b>Platform &amp; Topology</b>",
        "<b>Instance Details</b>",
    )
    assert sorted(order, key=page.index) == list(order)

    # a row or a cell of a change in the color of its outcome
    def css(text):  # one space between selectors, none around the punctuation
        return re.sub(r"\s*([{};,:])\s*", r"\1", re.sub(r"\s+", " ", text))

    style = css(page[: page.index("</style>")])
    for rule in (
        "--i:#229954;--i-tint:#cbecd9;--r:#e74c3c;--r-tint:#f9d4d0;--d-tint:#efefef",
        ".sw.i::before{background:var(--i)}",
        ".sw.r::before{background:var(--r)}",
        "table.scored tr.i td,table.tm td.i{background:var(--i-tint)}",
        "table.scored tr.r td,table.tm td.r{background:var(--r-tint)}",
        "table.scored tr.d td,table.tm td.d{background:var(--d-tint)}",
        "table.scored tr:nth-child(even),table.tm tr:nth-child(even){background:none}",
        "table.scored td:last-child,table.tm td.u{color:var(--text-muted)}",
        "table.scored,table.tm{table-layout:fixed}",
        "tr.i .bar i{background:var(--i)}",
        "tr.r .bar i{background:var(--r)}",
        "table.cmp,table.tm{overflow:visible}",
        "table.cmp th,table.tm th{background:#f3f5f8;color:var(--brand);"
        "text-transform:none;border-bottom:2px solid var(--border);position:sticky;"
        "top:0;z-index:1}",
        ".verdict.r{border-left-color:var(--r);background:var(--r-tint)}",
        "details.fold{border:0;box-shadow:none;background:none;margin:4px 0 0;"
        "overflow:visible}",
        ".sec h3{color:var(--brand);background:#f1f2f4;border-left-color:var(--brand)}",
    ):
        assert css(rule) in style, rule
    assert '<table class="mx cmp scored"><tr>' in page
    assert '<table class="mx cmp"><tr>\n<th></th>\n<th>Baseline</th>' in page
    head = "<th>Metric</th>\n<th>Baseline<br>main</th>\n<th>Candidate<br>a&lt;b</th>\n"
    head += "<th>Change</th>\n<th>Noise</th>\n<th>Rounds</th>\n<th>Outcome</th>"
    assert head in page
    bar = '<span class="bar"><i style="left:50.0%;width:20.0%"></i></span>'
    x = f"<td>x ↓</td><td><b>100</b></td><td>110</td><td>{bar}+10.0%</td><td>±0.0%</td>"
    assert (
        f'<tr class="r">{x}<td>+10% / +10%</td><td>❌ regression (high)</td></tr>'
        in page
    )
    # the call times in one table, a column for each stat a build has; one round of
    # them gives no noise
    assert "<h3>Tasklet call times, every call timed</h3>" in page
    head = "<th>Scheduler</th>\n<th>avg (us) ↓</th>\n<th>p90 (us) ↓</th>\n"
    assert f"{head}<th>max (us) ↓</th>" in page and "p95 (us)" not in page
    t = "<tr><td>t</td><td>sch_1</td>"
    t += '<td class="u" title="unchanged; rounds +0.0%">1 → 1<small>~ (+0.0%)</small></td>'
    t += '<td class="u" title="n/a; rounds n/a">n/a → 1.50<small>n/a</small></td>'
    t += '<td class="u" title="n/a; rounds +50%">2 → 3<small>+50.0%</small></td></tr>'
    sessions = "<h3>Video session call times, every session timed</h3>"
    s = '<tr><td>RX video session</td><td>sch_2</td><td class="u" title="n/a; rounds '
    s += '+100%">0.250 → 0.500<small>+100.0%</small></td></tr>'
    assert page.index(t) < page.index(sessions) < page.index(s)
    assert page.count("In each cell") == 1  # under the role, before its first matrix
    assert "differs, in grey:" in page and "The bar of a change ends at -25%" in page
    assert "<tr><td>1</td><td>main</td><td>a&lt;b</td>" in page  # the rounds
    assert "No p90, p95 or p99 from main: no release" in page
    traced = "No sampled tasklet counters from main and a&lt;b: they"
    assert page.count(traced) == 2  # in the summary and under its role
    assert "99,999" not in page and "99999" not in page  # the invalid round
    assert page.endswith("</html>")
    assert _Tags(page).open == []
    rounds = [{**rounds[0], "launches": []}]  # a round that failed before it started
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms)
    assert "Instance Details" not in page
    # no note for both builds with percentiles, nor for a tasklet that one build runs
    a_rx, b_rx = (rounds[0]["metrics"][x]["rx"] for x in "AB")
    a_rx["sch_1 t p90, timed (us)"] = 1.2
    b_rx.update({"sch_2 u avg, timed (us)": 1.0, "sch_2 u p90, timed (us)": 2.0})
    b_rx["t avg (ns/call)"] = a_rx["t avg (ns/call)"] = 300  # and sampled counters
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms)
    assert "No p90" not in page and "No sampled tasklet counters" not in page
    b_rx["sch_1 t p90, timed (us)"] = None  # B without them
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms)
    assert "No p90, p95 or p99 from a&lt;b: no release" in page
    # no valid round compares nothing, so it lacks nothing
    rounds = [{**rounds[0], "problems": ["x"]}]
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms)
    assert "No valid round: the builds are not compared." in page
    assert "No sampled" not in page and "No p90" not in page
    # an A/A run: the same name, so the roles tell the builds apart
    rounds = [{**rounds[0], "problems": []}]
    page = pc.report_html(
        {"A": "main", "B": "main"}, ARGS, [], SLOTS, rounds, platforms
    )
    aa = "<th>Baseline<br>main (baseline)</th>\n<th>Candidate<br>main (candidate)</th>"
    assert aa in page


def test_the_report_names_the_builds_whose_launches_had_dma_and_their_dsa():
    slots = [{**SLOTS[0], "dsa": "0000:00:01.0"}, {**SLOTS[0], "role": "tx"}]
    launch = dict(role="rx", slot=0, script="exec RxTxApp --x", config={})
    launches = [{**launch, "label": "A"}, {**launch, "label": "B"}]
    launches[1]["script"] += " --dma_dev 0000:00:01.0"  # what B ran with
    rounds = [_round(_rounds((1, 1))[0], launches=launches)]
    platforms = [("s (RX)", {}), ("c (TX)", {})]
    page = pc.report_html(LABELS, ARGS, [], slots, rounds, platforms)
    assert "<td>RX copies with DMA</td><td>no</td><td>yes</td>" in page
    assert "<th>perf</th>\n<th>DSA</th>" in page
    assert "<td>no</td><td>0000:00:01.0</td></tr>" in page
    tx = (
        "<td>TX</td><td>0</td><td>4-7</td><td>yes</td><td>yes</td><td>no</td><td>-</td>"
    )
    assert tx in page
    summary = pc.summary_html(LABELS, ARGS, slots, rounds)
    assert "<td>RX copies with DMA</td><td>no</td><td>yes</td>" in summary
    rounds = [_round(_rounds((1, 1))[0], launches=launches[:1])]
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms)
    assert "RX copies with DMA" not in page and "DSA" not in page
    assert "Single core" not in page


def test_the_report_says_when_each_instance_ran_on_one_scheduler():
    rounds = [_round(_rounds((1, 1))[0])]
    platforms = [("s (RX)", {}), ("c (TX)", {})]
    for roles, shown in ((("rx",), "RX"), (("rx", "tx"), "RX and TX")):
        row = f"<td>Single core</td><td>{shown}</td><td>{shown}</td>"
        page = pc.report_html(
            LABELS, ARGS, [], SLOTS, rounds, platforms, "single", roles
        )
        assert row in page
        assert row in pc.summary_html(LABELS, ARGS, SLOTS, rounds, "single", roles)


def test_the_report_keeps_the_pair_of_each_nic_apart_or_pools_them():
    slots = [{**SLOTS[0], "slot": n, "nic": n // 2, "pfs": f"pf{n}"} for n in range(4)]
    loop = "sch_1 avg loop (ns)"
    metrics = [_rounds((100, 110 + nic), metric=loop)[0] for nic in (0, 1)]
    both = {
        x: {f"rx NIC {nic}": m[x]["rx"] for nic, m in enumerate(metrics)} for x in "AB"
    }
    rounds = [_round(both, pc.round_order(n, 4)) for n in range(2)]
    platforms = [("s (RX)", {}), ("c (TX)", {})]
    page = pc.report_html(LABELS, ARGS, [], slots, rounds, platforms, nics="pairs")
    for nic in (0, 1):
        assert f"<h2>RX NIC {nic} (s&lt;t)</h2>" in page
    assert "<td>RX NIC 1 · sch_1 avg loop (ns) ↓</td>" in page  # the verdict's
    assert "<th>Slot 3</th>\n<th>Result</th>" in page
    assert (
        "<td>main</td><td>a&lt;b</td><td>a&lt;b</td><td>main</td><td>valid</td>" in page
    )
    assert "<th>perf</th>\n<th>PFs</th>" in page and "<td>pf3</td>" in page
    assert "2 valid rounds, the slots swapped each round." in page
    page = pc.report_html(LABELS, ARGS, [], slots, rounds, platforms, nics="pooled")
    assert "<h2>RX (s&lt;t)</h2>" in page and "NIC 1 (s" not in page
    assert "2 valid rounds on each of 2 NICs, the slots swapped each round." in page
    assert "Under 4" not in page  # 4 samples
    one = pc.report_html(LABELS, ARGS, [], slots, rounds[:1], platforms, nics="pooled")
    assert "1 valid round on each of 2 NICs" in one and "Under 4 samples the" in one
    assert "<td>+10% / +11% / +10% / +11%</td>" in page  # a sample per NIC and round


class _Fake:
    """run_round on fake hosts: every host call answered from these attributes."""

    def __init__(self, monkeypatch, tmp_path):
        self.tmp_path = tmp_path
        self.runs = {}  # the records of an instance, _records() without
        self.traced = True
        self.perf = None
        self.fallback = set()  # the logs that fell back from hugepages
        self.dead = set()  # the units that inactive() reports
        self.busy = False
        self.pin = ""  # what pin_app_threads() returns
        self.dma = ""  # the labels whose RX instances are given --dma_dev
        self.single_core = ()  # the roles on one scheduler a NUMA node
        self.queues = None
        self.slots = [pc.Slot(i, [0], [4 + 4 * i], True, [i]) for i in (0, 1)]
        self.wire = ([0, 0], [0, 0])  # what each slot's two reads of the wire count
        self.pmu = {}  # the perf stat counts of an instance, PMU without
        self.paced = {}  # the pacing way lines of an instance's log
        self.scripts = _sh(monkeypatch, self.sh)
        for name, value in {
            "stop_units": lambda host: None,
            "wire_balance": lambda *a: next(self.wires),
            "runner_busy": lambda host: self.busy,
            "_wait_steady": lambda *a: [],
            "inactive": lambda host, names: [n for n in names if n in self.dead],
            "rdt_counters": lambda host, group: None,
        }.items():
            monkeypatch.setattr(pc, name, value)
        monkeypatch.setattr(
            pc.time, "sleep", lambda s: self.scripts.append(f"sleep {s}")
        )
        fake = self

        def proc(inst):
            fake.scripts.append("proc")
            fake.reads[inst.name] = fake.reads.get(inst.name, -1) + 1
            return (PROC0, PROC1)[fake.reads[inst.name]]

        def records(inst, tail=0):
            default = _records(tx=inst.role == "tx", traced=fake.traced)
            return fake.runs.get(inst.name, default)

        def trace(inst):
            inst.traced = fake.traced
            inst.window = "trace" if fake.traced else ""

        def start(inst, mtl_path, cmd):
            inst.pid, inst.script = 1, f"script of {cmd}"

        for name, value in {
            "start": start,
            "collect": lambda inst, *a: None,
            "trace": trace,
            "pin_app_threads": lambda inst: fake.pin,
            "proc": proc,
            "records": records,
            "save": lambda inst, out_dir: None,
        }.items():
            monkeypatch.setattr(pc.Instance, name, value)

    def sh(self, script):
        if script == "getconf CLK_TCK":
            return "100"
        if script.startswith("grep -qsF"):
            log = script.split()[-4]
            return "found\n" if log.rsplit("/", 1)[-1] in self.fallback else ""
        if script.startswith("grep -ho 'pacing way: [a-z_]*' "):
            return self.paced.get(script.rsplit("/", 1)[-1], "")
        if script.startswith("systemctl show -p MainPID"):
            return "2\n"
        if script.startswith("cat ") and any(
            f"file:{script[4:]} " in c for c in self.scripts
        ):
            # what a -pmu unit wrote to its log
            name = script.rsplit("/", 1)[-1].removesuffix(".pmu")
            pmu = self.pmu.get(name, PMU)
            return "".join(f"{v},,{k},1,100.00,,\n" for k, v in pmu.items())
        return ""

    def round(self):
        host = SimpleNamespace(name="h")
        vfs = [f"vf{p}" for s in self.slots for p in s.ports]
        sides = [
            pc.Side(host, r, "/mtl", vfs, self.slots, None, self.perf)
            for r in ("rx", "tx")
        ]
        self.launches, self.reads, self.wires = [], {}, iter(self.wire)
        return pc.run_round(
            sides,
            {"A": "/a", "B": "/b"},
            pc.round_order(0, len(self.slots)),
            lambda side, slot, label: (
                f"app {label}"
                + " --dma_dev d" * (side.role == "rx" and label in self.dma),
                {"slot": slot},
            ),
            str(self.tmp_path),
            2,
            FPS,
            0,
            0,
            self.launches,
            {"A": "main", "B": "dev"},
            self.single_core,
            self.queues,
        )


@pytest.fixture
def fake(monkeypatch, tmp_path):
    return _Fake(monkeypatch, tmp_path)


def test_run_round_measures_a_valid_round(fake):
    results, problems, notes = fake.round()
    assert problems == [] and notes == []
    # the timed and the sessions window after the traced one, each without the probes
    # before it, the last ended a dump period before the applications stop, so each
    # prints the dump that closes it
    calls = fake.scripts
    end = calls.index("systemctl stop mtl-perf-tx1-sessions")
    apps = next(
        k for k, c in enumerate(calls) if c.startswith("systemctl stop mtl-perf-rx0 ")
    )
    assert calls[end + 1 : apps] == [f"sleep {pc.DUMP_PERIOD_S}"]
    stops = [
        calls.index(f"systemctl stop mtl-perf-rx0-{kind}")
        for kind in ("trace", "timed", "sessions")
    ]
    assert calls.index("proc") < stops[0] < stops[1] < stops[2] < end
    window = f"sleep {2 * pc.DUMP_PERIOD_S}"  # each as long as the traced, of 0 s
    assert calls[stops[0] : stops[2]].count(window) == 2
    assert results["A"]["rx"]["sch_1 rvs_pkt_rx max, timed (us)"] == 13
    assert results["B"]["tx"]["sch_1 TX video session max, session timed (us)"] == 9
    assert results["A"]["rx"]["RX work (ns/pkt)"] > 0
    # TX: the average of every call; MTL marks no TX work apart from its polling
    tx = results["B"]["tx"]
    assert tx["tx_video_sessions_mgr avg (ns/call)"] == pytest.approx(100)
    assert tx["video_transmitter avg (ns/call)"] == pytest.approx(300)
    assert not [k for k in tx if "work" in k]
    assert not [s for s in calls if "-pmu" in s]  # a host without perf
    assert fake.launches == [
        {
            "label": label,
            "role": role,
            "slot": slot,
            "script": f"script of app {label}",
            "config": {"slot": slot},
        }
        for role in ("rx", "tx")
        for slot, label in enumerate("AB")
    ]


def test_start_keeps_the_script_its_unit_runs(monkeypatch):
    written = {}
    monkeypatch.setattr(
        pc, "write_file", lambda host, path, text: written.update({path: text})
    )
    props = []
    monkeypatch.setattr(
        pc, "_start_unit", lambda h, n, c, p, *a, **k: props.extend(p) or 7
    )
    slot = pc.Slot(0, [0], [4, 5, 6, 7], True)
    rdt = "/sys/fs/resctrl/mtl_perf_0"
    inst = pc.Instance(None, "rx0", "rx", "A", slot, "/t/a", "/run/A_rx0", rdt)
    inst.start("/mtl", "RxTxApp --x")
    assert written == {"/run/A_rx0.sh": inst.script + "\n"} and inst.pid == 7
    assert inst.script.startswith("mount --bind /t/a /mtl/.local_install && ")
    join = f"&& echo $$ > {rdt}/tasks && cd /mtl && exec env LD_LIBRARY_PATH="
    assert join in inst.script and inst.script.endswith(" RxTxApp --x")
    # MTL makes CPU 0 the main lcore of every instance
    assert props == ["AllowedCPUs=0,4-7", "AllowedMemoryNodes=0", "Nice=-20"]
    props.clear()
    inst.slot = pc.Slot(0, [0, 2], [4, 5, 68, 69], True)  # span: both nodes
    inst.start("/mtl", "RxTxApp --x")
    assert props == ["AllowedCPUs=0,4-5,68-69", "AllowedMemoryNodes=0,2", "Nice=-20"]
    # a slot off the main lcore's node: DPDK puts the dmadev data on that node
    for main_node, mems in ((0, "0,2"), (1, "1-2")):
        props.clear()
        inst.slot = pc.Slot(2, [2], [68, 69], True, [2], 1, main_node=main_node)
        inst.start("/mtl", "RxTxApp --x")
        assert props == [
            "AllowedCPUs=0,68-69",
            f"AllowedMemoryNodes={mems}",
            "Nice=-20",
        ]


def test_run_round_counts_every_instance_over_its_untraced_window(fake):
    fake.perf = "/p/perf"
    fake.pmu = {"B_tx1": {}}
    results, problems, notes = fake.round()
    assert problems == []
    assert results["A"]["rx"]["LLC misses (M/s)"] == pytest.approx(30)
    assert results["B"]["rx"]["IPC"] == pytest.approx(1.5)
    assert "IPC" not in results["B"]["tx"]
    assert notes == ["mtl-perf-tx1 (dev): perf stat did not count every event"]
    calls = fake.scripts
    procs = [i for i, c in enumerate(calls) if c == "proc"]
    for unit, log in (("rx0", "A_rx0"), ("rx1", "B_rx1"), ("tx0", "A_tx0")):
        start = next(
            i for i, s in enumerate(calls) if f"--unit=mtl-perf-{unit}-pmu" in s
        )
        assert calls[start].endswith(
            f" -p AllowedCPUs={pc.COLLECTOR_CPU} -p KillSignal=SIGINT -p "
            f"TimeoutStopSec=60 -p StandardOutput=file:{pc.RUN_DIR}/{log}.pmu -- "
            f"/p/perf stat -x, -e {pc.PMU_EVENTS} -p 1"
        )
        stop = calls.index(f"systemctl stop mtl-perf-{unit}-pmu")
        assert start < procs[0] and stop > procs[-1]  # around the untraced window
        assert calls[stop + 1] == f"cat {pc.RUN_DIR}/{log}.pmu"


def test_save_copies_the_log_and_the_record_of_every_collector(monkeypatch, tmp_path):
    monkeypatch.setattr(pc, "stream", lambda host, script: script)
    pc.Instance(None, "rx0", "rx", "A", None, "/t", "/run/A_rx0").save(str(tmp_path))
    names = ["A_rx0", "A_rx0.log", "A_rx0.pmu", "A_rx0.trace"]
    assert sorted(os.listdir(tmp_path)) == names
    with open(tmp_path / "A_rx0.pmu") as f:
        assert f.read() == "[ ! -f /run/A_rx0.pmu ] || cat /run/A_rx0.pmu"


def test_run_round_rejects_a_session_that_went_silent(fake):
    fake.runs["mtl-perf-rx1"] = [r for r in _records() if "_VIDEO_SESSION(1,1" not in r]
    assert fake.round()[1] == ["mtl-perf-rx1 (dev): 1 of 2 sessions"]


def test_run_round_rejects_a_session_below_the_frame_rate(fake):
    fake.runs["mtl-perf-rx0"] = _records(fps=FPS * 0.95)
    assert fake.round()[1] == [
        f"mtl-perf-rx0 (main): a session fell to {FPS * 0.95:.2f} fps"
    ]


def test_run_round_runs_a_pair_on_each_nic_and_keeps_their_rows_apart(fake):
    fake.slots = _nics("pairs")
    fake.wire = ([0, 0, 0, 0], [0, 0, 5, 0])
    fake.paced = {f"{x}_tx{n}": "pacing way: ratelimit\n" for n, x in enumerate("ABB")}
    fake.paced["A_tx3"] = "pacing way: tsc\n"
    results, problems, notes = fake.round()
    assert problems == []
    ran = [(x["label"], x["role"], x["slot"]) for x in fake.launches]
    assert ran == [(x, r, n) for r in ("rx", "tx") for n, x in enumerate("ABBA")]
    keys = ["rx NIC 0", "rx NIC 1", "tx NIC 0", "tx NIC 1"]
    assert sorted(results["A"]) == sorted(results["B"]) == keys
    assert "slot 2: 5 packets lost between the hosts" in notes
    paced = [n for n in notes if "pace" in n]
    assert paced == ["the builds pace TX differently on NIC 1: main tsc; dev ratelimit"]


def test_run_round_counts_the_losses_of_both_ports_of_a_slot_of_two_nics(fake):
    fake.slots = _nics("span")
    fake.wire = ([0, 0, 0, 0], [0, 0, 5, 0])  # port 2: NIC 1's of slot 0
    results, problems, notes = fake.round()
    assert problems == [] and sorted(results["A"]) == ["rx", "tx"]
    assert "slot 0: 5 packets lost between the hosts" in notes


def test_run_round_rejects_an_rx_session_of_a_dma_build_that_dma_did_not_copy(fake):
    fake.dma = "B"  # B runs in slot 1
    fake.runs["mtl-perf-rx1"] = _records(dma=(5, 5))
    assert fake.round()[1] == []
    for dma in ((5,), (5, 0)):  # a session that MTL gave no DMA, or one it copied 0
        fake.runs["mtl-perf-rx1"] = _records(dma=dma)
        assert fake.round()[1] == ["mtl-perf-rx1 (dev): 1 of 2 sessions copied by DMA"]


def test_run_round_rejects_a_second_scheduler_of_a_single_core_instance(fake):
    def more(records, idx, since=0):  # scheduler *idx* too, in each dump from *since* s
        out = []
        for r in records:
            out.append(r)
            if "SCH(1:" in r and int(r.split(" ", 1)[0]) >= since * S:
                out.append(r.replace("SCH(1:sch_1)", f"SCH({idx}:sch_{idx})"))
        return out

    records = more(_records(), 2, since=40)
    fake.runs["mtl-perf-rx1"] = records
    assert fake.round()[1] == []  # multi core: as many as the quota needs
    fake.single_core = ("tx",)
    assert fake.round()[1] == []
    fake.single_core = ("rx",)
    one = "single core runs one a NUMA node"
    assert fake.round()[1] == [f"mtl-perf-rx1 (dev): 2 schedulers, {one}"]
    # span: a scheduler on each NUMA node of the slot
    fake.slots = [replace(s, nodes=[0, 2]) for s in fake.slots]
    assert fake.round()[1] == []
    fake.runs["mtl-perf-rx1"] = more(records, 3)
    assert fake.round()[1] == [f"mtl-perf-rx1 (dev): 3 schedulers, {one}"]


def test_run_round_rejects_tasklet_stat_records_without_the_work_tasklets(fake):
    fake.runs["mtl-perf-rx0"] = [r.replace(" rvs_pkt_rx ", " x ") for r in _records()]
    assert fake.round()[1] == [
        "mtl-perf-rx0 (main): no rvs_pkt_rx work in sys:tasklet_stat"
    ]


@pytest.mark.parametrize(
    "drop, counts",
    [(" T 1 ", (0, 3, 3)), (" L SCH(1,", (4, 0, 3)), (": tasklet time ", (4, 3, 0))],
)
def test_run_round_rejects_a_build_short_of_the_dumps_of_a_window(fake, drop, counts):
    fake.runs["mtl-perf-tx1"] = [r for r in _records(tx=True) if drop not in r]
    want = "5 untraced, {} traced, {} timed and {} session timed dumps".format(*counts)
    assert fake.round()[1] == [f"mtl-perf-tx1 (dev): {want}"]


def test_run_round_rejects_a_tx_source_outside_hugepages_only(fake):
    fake.fallback = {"B_tx1", "A_rx0"}
    assert fake.round()[1] == ["mtl-perf-tx1 (dev): its source is not in hugepages"]


def test_run_round_measures_an_untraced_build_without_its_traced_rows(fake):
    fake.traced = False
    results, problems, _ = fake.round()
    assert problems == [] and results["A"]["rx"]["sch_1 avg loop (ns)"] == 500
    assert not [k for k in results["A"]["rx"] if "traced" in k or "/call" in k]
    assert "RX work (ns/pkt)" not in results["A"]["rx"]


def test_run_round_rejects_an_instance_that_exits_during_the_measurement(fake):
    fake.dead = {"mtl-perf-tx1-log"}
    results, problems, _ = fake.round()
    assert not [s for s in fake.scripts if "-trace" in s]  # nothing timed after it
    assert problems == ["exited during the measurement: mtl-perf-tx1-log"]
    assert results == {"A": {}, "B": {}}
    assert len(fake.launches) == 4  # what ran, for the report of a failed round


def test_run_round_waits_as_long_as_the_tx_queues_take(fake, monkeypatch):
    waits = []
    monkeypatch.setattr(pc, "_wait_steady", lambda *a: waits.append(a[1:]) or [])
    fake.queues = 4  # redundant: two queues a session
    fake.round()
    assert waits == [(2, FPS, 4)]


def test_run_round_keeps_what_ran_when_it_raises(fake, monkeypatch):
    def wait_steady(*a):
        raise RuntimeError("rx0: lost")

    monkeypatch.setattr(pc, "_wait_steady", wait_steady)
    with pytest.raises(RuntimeError, match="rx0: lost"):
        fake.round()
    assert [x["role"] for x in fake.launches] == ["rx", "rx", "tx", "tx"]


def test_run_round_notes_losses_and_what_ran_beside_it(fake):
    fake.runs["mtl-perf-rx0"] = _records(lost=3)
    fake.wire = ([10, 20], [15, 20])
    fake.busy = True
    fake.pin = "no scheduler thread"
    _, problems, notes = fake.round()
    assert problems == []
    assert notes == [
        *(
            f"mtl-perf-{n}: app threads not moved, no scheduler thread"
            for n in ("rx0 (main)", "rx1 (dev)", "tx0 (main)", "tx1 (dev)")
        ),
        "slot 0: 5 packets lost between the hosts",
        "a CI job ran on h during the round",
        "mtl-perf-rx0 (main): 15 incomplete frames",
        "mtl-perf-rx0 (main): 15 hw dropped pkts",
    ]


def test_run_round_notes_late_tx_frames_and_a_pacing_difference(fake):
    fake.runs["mtl-perf-tx1"] = _tx_records([("transmitter mismatch troffset", 1)])
    differ = {"A_tx0": "pacing way: ratelimit\n" * 2, "B_tx1": "pacing way: tsc\n"}
    fake.paced = differ
    results, problems, notes = fake.round()
    assert problems == []
    assert results["B"]["tx"]["transmitter mismatch troffset"] == 2
    assert notes == [
        "the builds pace TX differently: main ratelimit; dev tsc",
        "mtl-perf-tx1 (dev): 2 transmitter mismatch troffset",
    ]
    fake.runs = {}
    same = "pacing way: ratelimit\n"
    fake.paced = {"A_tx0": same, "B_tx1": same}
    assert fake.round()[2] == []
    fake.paced = {"A_tx0": same}  # B logged below info: nothing to compare
    assert fake.round()[2] == []
    # an invalid round says it too, as a build that falls back may not hold its rate
    fake.paced = differ
    fake.dead = {"mtl-perf-tx1"}
    _, problems, notes = fake.round()
    assert problems and notes == [
        "the builds pace TX differently: main ratelimit; dev tsc"
    ]


def test_units_of_an_instance_follow_its_window():
    inst = pc.Instance(None, "rx0", "rx", "A", None, "/t", "/run/A_rx0")
    assert inst.units == ["rx0", "rx0-log"]
    inst.window = "trace"
    assert inst.units == ["rx0", "rx0-log", "rx0-trace"]
    inst.window = "sessions"
    assert inst.units == ["rx0", "rx0-log", "rx0-sessions"]


def test_trace_attaches_the_sampled_probes_where_the_build_has_them(monkeypatch):
    for listed, window in (
        ("usdt:/l:sys:log_msg\nusdt:/l:sys:tasklet_time_sample\n", "trace"),
        ("usdt:/l:sys:log_msg\nusdt:/l:sys:tasklet_time_measure\n", ""),
    ):
        calls = _sh(monkeypatch, lambda script: listed)
        monkeypatch.setattr(
            pc.Instance, "collect", lambda inst, kind, script: calls.append(kind)
        )
        inst = pc.Instance(None, "rx0", "rx", "A", None, "/t", "/run/A_rx0")
        inst.trace()
        assert (inst.traced, inst.window) == (bool(window), window)
        assert calls[1:] == ["trace"] * bool(window)
    _sh(monkeypatch, lambda script: "")  # the listing failed
    with pytest.raises(RuntimeError, match="rx0: bpftrace lists no sys probes"):
        inst.trace()


def test_time_attaches_each_window_in_place_of_the_one_before(monkeypatch):
    calls = _sh(monkeypatch, lambda script: "")
    monkeypatch.setattr(
        pc.Instance, "collect", lambda inst, kind, script: calls.append((kind, script))
    )
    inst = pc.Instance(None, "rx0", "rx", "A", None, "/t", "/run/A_rx0")
    inst.time("timed")  # of an untraced build: no window before it
    inst.time("sessions")
    assert calls == [
        ("timed", "usdt::sys:tasklet_time_measure {}"),
        "systemctl stop rx0-timed",
        ("sessions", "usdt::sys:sessions_time_measure {}"),
    ]
    assert inst.window == "sessions"


def _steady_insts(monkeypatch, records, dead=(), pinned=None, pin=""):
    """Two RX instances for _wait_steady, a fake clock that each poll advances; each
    pin_app_threads() call goes into *pinned* with the time and returns *pin*."""
    pinned = [] if pinned is None else pinned
    clock = [0.0]
    monkeypatch.setattr(pc.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(pc.time, "sleep", lambda s: clock.__setitem__(0, clock[0] + s))
    monkeypatch.setattr(
        pc, "inactive", lambda host, names: [n for n in names if n in dead]
    )
    return [
        SimpleNamespace(
            host=None,
            name=f"rx{i}",
            role="rx",
            units=[f"rx{i}"],
            records=lambda tail=0, i=i: records[i],
            pin_app_threads=lambda i=i: pinned.append((f"rx{i}", clock[0])) or pin,
        )
        for i in (0, 1)
    ]


def test_wait_steady_waits_for_the_last_dump_of_every_instance(monkeypatch):
    steady, short = _dump(S), _dump(S, sessions=1)
    insts = _steady_insts(monkeypatch, [steady, short + steady])
    assert pc._wait_steady(insts, 2, FPS) == []
    insts = _steady_insts(monkeypatch, [steady, steady + short])
    timeout = pc.steady_timeout(2)
    assert pc._wait_steady(insts, 2, FPS) == [f"not steady after {timeout} s: rx1"]
    timeout = pc.steady_timeout(4)
    assert pc._wait_steady(insts, 2, FPS, 4) == [f"not steady after {timeout} s: rx1"]


def test_wait_steady_moves_the_app_threads_once_a_dump_shows_all_sessions(
    monkeypatch,
):
    pinned = []
    records = [_dump(S), _dump(S, sessions=1)]  # rx1 has one of its two sessions
    insts = _steady_insts(monkeypatch, records, pinned=pinned)
    sleep = pc.time.sleep

    def second_session_of_rx1(s):  # it has both a poll later, at speed
        records[1] = _dump(S)
        sleep(s)

    monkeypatch.setattr(pc.time, "sleep", second_session_of_rx1)
    assert pc._wait_steady(insts, 2, FPS) == []
    # each once all its sessions exist
    assert pinned == [("rx0", 0), ("rx1", pc.POLL_S)]
    # a move that failed is tried at every poll
    pinned.clear()
    slow = [_dump(S), _dump(S, fps=FPS / 2)]
    insts = _steady_insts(monkeypatch, slow, pinned=pinned, pin="not started")
    pc._wait_steady(insts, 2, FPS)
    rx1 = [t for name, t in pinned if name == "rx1"]
    assert rx1[:3] == [0, pc.POLL_S, 2 * pc.POLL_S]


def test_wait_steady_stops_at_an_instance_that_exited(monkeypatch):
    insts = _steady_insts(monkeypatch, [_dump(S), _dump(S)], dead={"rx0"})
    assert pc._wait_steady(insts, 2, FPS) == ["exited before steady state: rx0"]
