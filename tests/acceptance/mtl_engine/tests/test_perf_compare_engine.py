# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""mtl_engine/perf_compare.py without hosts: python -m pytest --noconftest <this file>."""

import html
import io
import json
import os
import re
import socket
import struct
import subprocess  # nosec B404 # runs bash on proc()'s own script and a fixed child, via argv
import sys
import time
from collections import Counter
from html.parser import HTMLParser
from types import SimpleNamespace

import pytest
import requests
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


def _layout(monkeypatch, node_cpus, isolated, cores, count=2):
    """cpu_layout on sysfs reads where core c has the SMT sibling c + cores."""
    siblings = [f"{c},{c + cores}" for c in range(cores)] * 2
    nodes = "0 /sys/devices/system/cpu/cpu0/node0"  # the VF's and MAIN_LCORE's
    _sh(monkeypatch, lambda script: "\n".join([nodes, node_cpus, isolated, *siblings]))
    return pc.cpu_layout(HOST, "0000:15:00.0", count)


def test_cpu_layout_takes_isolated_first_threads_after_the_os_cores(monkeypatch):
    slots = _layout(monkeypatch, "0-31", "4-20", 16)
    assert [s.cpus for s in slots] == [[4, 5, 6, 7, 8, 9], [10, 11, 12, 13, 14, 15]]
    assert all(s.isolated and s.node == 0 for s in slots)
    assert [s.ports for s in slots] == [[0], [1]]


def test_cpu_layout_takes_isolated_cores_for_8_a_slot_or_as_many_as_the_others(
    monkeypatch,
):
    # 18 cores after the OS's: 9 isolated are as many as the others, 8 fewer
    slots = _layout(monkeypatch, "0-39", "4-12", 20)
    assert [s.cpus for s in slots] == [[4, 5, 6, 7], [8, 9, 10, 11]]
    assert all(s.isolated for s in slots)
    slots = _layout(monkeypatch, "0-39", "4-11", 20)
    assert [s.cpus for s in slots] == [[2, 3, 12, 13, 14], [15, 16, 17, 18, 19]]
    assert not any(s.isolated for s in slots)
    # 38 cores after the OS's: 16 isolated give 8 a slot, though 22 are not isolated
    slots = _layout(monkeypatch, "0-79", "4-19", 40)
    assert [s.cpus for s in slots] == [list(range(4, 12)), list(range(12, 20))]
    assert [s.cpus for s in _layout(monkeypatch, "0-79", "4-11", 40, 1)] == [
        list(range(4, 12))
    ]
    (slot,) = _layout(monkeypatch, "0-79", "4-10", 40, 1)  # 7: the 31 others
    assert slot.cpus == [2, 3, *range(11, 40)] and not slot.isolated


def test_cpu_layout_takes_3_cores_a_slot_at_least(monkeypatch):
    with pytest.raises(RuntimeError, match="0 isolated free cores and 4 others, not 6"):
        _layout(monkeypatch, "0-11", "", 6)
    assert [s.cpus for s in _layout(monkeypatch, "0-9", "", 5, 1)] == [[2, 3, 4]]
    with pytest.raises(RuntimeError, match="0 isolated free cores and 2 others, not 3"):
        _layout(monkeypatch, "0-7", "", 4, 1)


def test_cpu_layout_takes_the_isolated_cores_of_nic_1_and_the_main_lcore_node(
    monkeypatch,
):
    siblings = [f"{c},{c + 128}" for c in range(64, 96)]
    sysfs = ["2 /sys/devices/system/cpu/cpu0/node1", "64-95", "4-20,68-84", *siblings]
    scripts = _sh(monkeypatch, lambda script: "\n".join(sysfs))
    slots = pc.cpu_layout(HOST, "0000:c9:00.0")
    assert [(s.node, s.cpus, s.isolated, s.main_node) for s in slots] == [
        (2, list(range(68, 76)), True, 1),
        (2, list(range(76, 84)), True, 1),
    ]
    assert "echo $n /sys/devices/system/cpu/cpu0/node*; " in scripts[0]


def test_cpu_layout_gives_one_slot_all_the_isolated_cores_of_the_node(monkeypatch):
    siblings = [f"{c},{c + 128}" for c in range(32)]
    sysfs = ["0 /sys/devices/system/cpu/cpu0/node0", "0-31,128-159", "4-20,68-84"]
    _sh(monkeypatch, lambda script: "\n".join(sysfs + siblings))
    slots = pc.cpu_layout(HOST, "0000:15:00.0", 1)  # split, redundant and whole
    assert [(s.idx, s.cpus, s.isolated, s.ports) for s in slots] == [
        (0, list(range(4, 21)), True, [0])
    ]


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


@pytest.mark.skipif(
    os.geteuid() != 0, reason="proc() reads /proc/timer_list, root only"
)
@pytest.mark.skipif(len(os.sched_getaffinity(0)) < 2, reason="needs 2 CPUs")
def test_proc_reads_the_cpu_ticks_the_name_and_the_cpu_of_each_thread(monkeypatch):
    monkeypatch.setattr(pc, "sh", _bash)
    cpus = sorted(os.sched_getaffinity(0))
    pins = {"tx_st20p_0": cpus[-1], "a_b)_c": cpus[0]}  # two CPUs, as proc() names
    spin = (
        "import ctypes, os, threading, time\n"
        "def spin(name, cpu):\n"
        "    ctypes.CDLL(None).prctl(15, name.encode(), 0, 0, 0)  # PR_SET_NAME\n"
        "    os.sched_setaffinity(0, {cpu})\n"
        "    end = time.monotonic() + 30\n"
        "    while time.monotonic() < end:\n"
        "        os.stat('/')  # some system time too\n"
        f"for name, cpu in (('tx_st20p_0', {cpus[-1]}), ('a b) c', {cpus[0]})):\n"
        "    threading.Thread(target=spin, args=(name, cpu), daemon=True).start()\n"
        "time.sleep(30)\n"
    )
    proc = subprocess.Popen([sys.executable, "-c", spin])
    try:
        inst = pc.Instance(None, "tx0", "tx", "A", None, "/t", "/log")
        inst.pid = proc.pid
        deadline = time.monotonic() + 10
        while len(os.listdir(f"/proc/{proc.pid}/task")) < 3:
            assert time.monotonic() < deadline, "the threads did not start"
            time.sleep(0.01)
        time.sleep(0.5)
        task = f"/proc/{proc.pid}/task"

        def comm(tid):
            with open(f"{task}/{tid}/comm") as f:
                return f.read()

        (tid,) = (t for t in os.listdir(task) if "tx_st" in comm(t))
        with open(f"{task}/{tid}/stat") as f:  # fields 14 and 15 from field 3 on
            utime, stime = map(int, f.read().rsplit(")", 1)[1].split()[11:13])
        with open(f"{task}/{tid}/schedstat") as f:
            run = int(f.read().split()[0])
        threads = {t[2]: t for t in inst.proc()[1].values()}
        assert pins.keys() <= threads.keys()  # a space reads as _
        ticks, _, _, _, run_ns = threads["tx_st20p_0"]
        assert stime > 0 and ticks >= utime + stime  # both count
        assert run > 0 and run_ns >= run  # its time on a CPU since, in ns
        assert {name: threads[name][3] for name in pins} == pins
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


def _nics(nics, cores=(1, 1)):
    """The slots of *nics* from cpu_layout()'s one slot of each NIC's node, of *cores*
    CPUs: NIC 0's on node 0, isolated, NIC 1's on node 2, not, so each keeps its own."""
    nic0, nic1 = (
        pc.Slot(0, n, list(range(n * 32 + 4, n * 32 + 4 + k)), n == 0, [0])
        for n, k in zip((0, 2), cores)
    )
    return pc.nic_slots(nics, nic0, nic1)


def test_the_nics_modes_map_their_slots_to_ports_and_nodes():
    split = _nics("split")  # a socket each
    assert [(s.idx, s.ports, s.node, s.cpus, s.isolated) for s in split] == [
        (0, [0], 0, [4], True),
        (1, [2], 2, [68], False),
    ]
    # both ports of a NIC: redundant's second the 2022-7 leg, whole's more sessions
    for nics in ("redundant", "whole"):
        assert [(s.idx, s.ports, s.node, s.cpus) for s in _nics(nics)] == [
            (0, [0, 1], 0, [4]),
            (1, [2, 3], 2, [68]),
        ]
    # each build as many CPUs as the smaller node has, as it runs in both slots
    for cores in ((17, 15), (15, 17)):
        assert [s.cpus for s in _nics("split", cores)] == [
            list(range(4, 19)),
            list(range(68, 83)),
        ]
    # the slots swapped every round
    assert pc.round_order(0) == ("A", "B") and pc.round_order(1) == ("B", "A")
    assert pc.round_order(2) == ("A", "B")


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
    # a port must carry its sessions
    links = [(100000, "2.10"), (100000, "2.10"), (40000, "1.00"), (40000, "1.00")]
    pfs = ["p0", "p1", "p2", "p3"]
    assert pc.slow_ports(pfs, links, 39e9) == []
    assert pc.slow_ports(pfs, links, 41e9) == [
        "p2 links at 40 Gb/s",
        "p3 links at 40 Gb/s",
    ]
    assert pc.slow_ports(["p4"], [(0, "")], 1e9) == ["p4 has no link"]


def test_a_port_carries_the_streams_its_link_speed_allows():
    hd, uhd, fuhd = (
        pc.stream_bps(w, h, FPS) for w, h in ((1920, 1080), (3840, 2160), (7680, 4320))
    )
    # MTL's packets of 1320 bytes of pixels take 1406 on the wire
    assert hd == pytest.approx(1920 * 1080 * 20 * FPS * 1125 / 1080 * 1406 / 1320)
    assert 36 * hd < 100e9 < 37 * hd and 14 * hd < 40e9 < 15 * hd
    assert 9 * uhd < 100e9 < 10 * uhd and 2 * fuhd < 100e9 < 3 * fuhd


def test_add_port_gives_the_second_port_its_own_sessions():
    for role, peer in (("tx", "dip"), ("rx", "ip")):
        config = {
            "interfaces": [{"name": "vf0", "ip": "own", "rx_queues_cnt": 64}],
            f"{role}_sessions": [
                {peer: ["peer"], "interface": [0], "st20p": [{"replicas": 3}]}
            ],
        }
        pc.add_port(config, role, "vf2", "tx2", "rx2", 1)
        own, other = ("tx2", "rx2") if role == "tx" else ("rx2", "tx2")
        assert config["interfaces"][1] == {
            "name": "vf2",
            "ip": own,
            "rx_queues_cnt": 64,
        }
        assert config[f"{role}_sessions"][1] == {
            peer: [other],
            "interface": [1],
            "st20p": [{"replicas": 1}],
        }
        assert config[f"{role}_sessions"][0] == {  # a copy, not the same
            peer: ["peer"],
            "interface": [0],
            "st20p": [{"replicas": 3}],
        }


def test_port_sessions_fill_the_first_port_before_the_second():
    hd, uhd = (pc.stream_bps(w, h, FPS) for w, h in ((1920, 1080), (3840, 2160)))
    assert pc.port_sessions(37, 2, hd) == [36, 1]
    assert pc.port_sessions(72, 2, hd) == [36, 36]
    assert pc.port_sessions(20, 2, hd) == [20, 0]
    assert pc.port_sessions(10, 2, uhd) == [9, 1]
    assert pc.port_sessions(73, 2, hd) == [36, 37]  # the last takes what is left
    assert pc.port_sessions(37, 1, hd) == [37]


def test_rdt_setup_only_monitors_slots_that_share_no_l3(monkeypatch):
    probe = "fff\n1\nc00\n    L3:0=fff;1=fff\ninfo\nmon_groups\n"
    scripts = _sh(monkeypatch, lambda script: probe if "cbm_mask" in script else "")
    paths = pc.rdt_setup(HOST, partition=False)
    assert paths == [f"{pc.RESCTRL}/{pc.RDT_PREFIX}{n}" for n in (0, 1)]
    for path in paths:
        assert f"mkdir {path} && echo 'L3:0=fff;1=fff' > {path}/schemata" in scripts[-1]
    assert (
        f"> {pc.RESCTRL}/schemata" not in scripts[-1] and pc.RDT_SAVE not in scripts[-1]
    )


def test_dsa_layout_gives_each_slot_of_a_node_a_dsa_of_its_own(monkeypatch):
    listing = "Y\n0000:05:01.0 0\n0000:00:01.0 0\n0000:80:01.0 2\n"
    _sh(monkeypatch, lambda script: listing)
    bound = []
    monkeypatch.setattr(pc, "bind_dma_to_vfio", lambda h, bdf: bound.append(bdf) or 1)
    slots = [pc.Slot(0, 0, [4], True), pc.Slot(1, 0, [8], True)]
    assert pc.dsa_layout(HOST, slots) == ["0000:00:01.0", "0000:05:01.0"] == bound
    # a slot of each node, split, takes the first DSA of its node
    apart = [pc.Slot(0, 0, [4], True), pc.Slot(1, 2, [68], True)]
    assert pc.dsa_layout(HOST, apart) == ["0000:00:01.0", "0000:80:01.0"]
    node2 = [pc.Slot(0, 2, [66], False), pc.Slot(1, 2, [74], False)]
    with pytest.raises(RuntimeError, match="node 2 has 1 DSA, its slots need 2"):
        pc.dsa_layout(HOST, node2)
    monkeypatch.setattr(pc, "bind_dma_to_vfio", lambda h, bdf: False)
    with pytest.raises(RuntimeError, match="DSA 0000:00:01.0 is not on vfio-pci"):
        pc.dsa_layout(HOST, slots)
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
# 101 the scheduler on CPU 5, 102 a frame thread on CPU 6, 104 MTL's stats thread,
# which moves from CPU 7 to 102's, 105 the EAL thread of an idle lcore on CPU 4, 106
# the main thread on CPU 9, which runs less than a tick; 103 starts late
PROC0 = (
    10 * S,
    {
        101: (0, 0, "mtl_sch_1", 5, 0),
        102: (0, 0, "rx_st20p_0", 6, 0),
        104: (0, 0, "mtl_stat", 7, 0),
        105: (0, 0, "dpdk-worker5", 4, 700),
        106: (0, 0, "RxTxApp_main", 9, 700),
    },
)
PROC1 = (
    60 * S,
    {
        101: (500, 10, "mtl_sch_1", 5, 5 * S),
        102: (50, 5, "rx_st20p_0", 6, S // 2),
        103: (99, 0, "rx_st20p_1", 8, S),
        104: (25, 0, "mtl_stat", 6, S // 4),
        105: (0, 0, "dpdk-worker5", 4, 700),
        106: (0, 0, "RxTxApp_main", 9, 900),
    },
)
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
    assert d.dev == [11.0, 41001.0, 50002, 101]  # both ports
    assert d.ports == {0: (10.0, 41000.0), 1: (1.0, 1.0)}
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
    # the CPUs that the threads of a stage ran on: not 103's, which is not in both
    # reads, nor idle 105's, but 106's, which ran under a tick, and 104's at the second
    # read, which all threads count once, as 102 shares it; the cores of the stage over
    # its CPUs and over the frames of both sessions, each at its median
    stages = (
        ("scheduler", 1, 0.1),
        ("application", 1, 0.01),
        ("other", 2, 0.005),
        ("all", 3, 0.115),
    )
    frames = 2 * FPS
    for stage, cpus, cores in stages:
        assert m[f"{stage} threads (CPUs)"] == cpus, stage
        utilization = m[f"{stage} threads utilization (%)"]
        assert utilization == pytest.approx(100 * cores / cpus), stage
        per_frame = m[f"{stage} threads per frame (ms)"]
        assert per_frame == pytest.approx(cores * 1e3 / frames), stage
    assert m["sch preemptions (/s)"] == pytest.approx(0.2)  # of the schedulers only
    assert m["memory bandwidth (MB/s)"] == pytest.approx(100)
    assert m["LLC occupancy (MB)"] == pytest.approx(7)
    assert m["fps (worst session)"] == pytest.approx(FPS)
    assert m["fps (mean of sessions)"] == pytest.approx(FPS)
    assert m["RX rate (Gb/s)"] == 41.0 and "RX rate, port 0 (Gb/s)" not in m
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
    assert m["fps (mean of sessions)"] == pytest.approx(FPS / 2)
    untraced[1].sch[2] = (600, 102)  # a second scheduler in any dump counts
    m = _metrics(dumps)
    assert m["scheduler threads (CPUs)"] == 2
    # no frame thread is left: no CPUs, and no utilization of them
    assert m["application threads (CPUs)"] == 0
    assert m["application threads utilization (%)"] is None


def test_metrics_give_each_port_of_an_instance_on_two_its_rate_then_the_sum():
    records = []
    for rec in _records():
        records.append(rec)
        if " DEV(0): Avr rate" in rec:  # port 1 receives 1 Gb/s less
            records.append(
                rec.replace("DEV(0)", "DEV(1)").replace("rx: 41000", "rx: 40000")
            )
    m = _metrics(_windows(records))
    rates = [k for k in m if " rate" in k]
    assert rates == [
        "RX rate, port 0 (Gb/s)",
        "RX rate, port 1 (Gb/s)",
        "RX rate (Gb/s)",
    ]
    assert [m[k] for k in rates] == [41.0, 40.0, 81.0]
    tx = []
    for rec in _records(tx=True):  # a TX instance sends 10 Mb/s on each
        tx += [rec, rec.replace("DEV(0)", "DEV(1)")] if " DEV(0): Avr" in rec else [rec]
    m = _metrics(_windows(tx), "TX")
    rates = [f"TX rate{p} (Gb/s)" for p in (", port 0", ", port 1", "")]
    assert [m[k] for k in rates] == pytest.approx([0.01, 0.01, 0.02])


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
    assert not m.keys() & set(pc.RX_LOSS)  # nothing a TX instance receives
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
    rates = ("TX rate (Gb/s)", "RX rate, port 1 (Gb/s)")
    # nor for the utilization, or the CPUs that the case spreads all threads but the
    # schedulers over; the CPUs of the schedulers and the CPU per frame are costs
    cpus = [f"{s} threads (CPUs)" for s in ("application", "other", "all")]
    cpus += ["scheduler threads utilization (%)", "all threads utilization (%)"]
    for metric in ("IPC", "core clock (GHz)", *rates, *cpus, "LLC miss ratio (%)"):
        assert outcome((1.0, 1.2), (1.0, 1.2), metric=metric) == "differs", metric
        assert outcome((1.0, 1.01), (1.0, 1.01), metric=metric) == "unchanged"
    for metric in ("scheduler threads (CPUs)", "other threads per frame (ms)"):
        assert outcome((3, 4), (3, 4), metric=metric) == "regression", metric
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
    # two values that print alike get a decimal more, which may tell them apart
    for a, b, shown in (
        (302.0, 302.4, ["302.0", "302.4"]),
        (2.181, 2.184, ["2.181", "2.184"]),
        (302.0, 302.0, ["302", "302"]),
        (1000.0, 1000.4, ["1,000.0", "1,000.4"]),  # thousands apart
        (3.0001, 2.99995, ["3.000", "3.000"]),  # but no digit below that
        (0.00413301, 0.00413304, ["0.004133", "0.004133"]),
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
        "scheduler threads per frame (ms)",
        "memory bandwidth (MB/s)",
        "LLC misses (M/s)",
        "RX work (ns/pkt)",
    ):
        assert pc.KEY_ROWS.fullmatch(m), m
    # the threads but the schedulers scored outside the verdict: the noise of the
    # application threads at pixel_format none is unmeasured; nor the scheduler CPUs
    not_key = [f"{s} threads per frame (ms)" for s in ("application", "other", "all")]
    not_key += ["scheduler threads (CPUs)", "LLC hits (M/s)"]
    for m in ("sch_1 avg loop, traced (ns)", "RX rate (Gb/s)", *not_key):
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
    want = {*pc.TX_LATE, f"{pc.TX_REPLAN} (/s)", "TX rate (Gb/s)", "RX work (ns/pkt)"}
    assert want <= rows.keys(), want - rows.keys()
    groups = pc._groups(pc.compare([{"A": {"rx": rows}, "B": {"rx": rows}}], "rx"))
    assert [title for title, _ in groups] == [
        "Delivery",
        pc.CPU_STAGES,
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
    threads = "scheduler threads per frame (ms)"
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
    # every key row in one table, none folded: the unchanged one after the changes
    labels = ["LLC misses (M/s)", threads, loop, bw, "video cpu busy (%)"]
    order = [page.index(f"<td>RX · {m} ↓</td>") for m in labels]
    assert order == sorted(order)
    keys = page[page.index("<h3>Key metrics</h3>") :]
    assert keys[: keys.index("</table>")].count("<tr") == 1 + len(labels)
    assert "<details" not in page
    assert "regresses 2 of the 5 key metrics and improves 2." in page
    page = _summary(rows, 4)
    assert "4 valid rounds" in page and "Under 4 rounds" not in page
    page = _summary(rows, 1)
    assert "1 valid round, the slots" in page and "Under 4" not in page
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
    page = _summary({loop: (100, 90), p99: (5, 6), "sch_1 t avg, timed (us)": (5, 5)})
    assert "improves 1 of the 1 key metrics" in page and tasklets in page
    assert '<tr class="r"><td>RX · t on sch_1, p99 (us) ↓</td>' in page
    assert "<tr><td>RX · t on sch_1, avg (us) ↓</td>" in page  # unchanged, shown
    assert "<details" not in page
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


def test_the_verdict_of_an_overloaded_role_scores_the_frames_and_their_cost():
    # the candidate delivers 24% more frames: its costs of a second grow with them,
    # those of a frame fall
    worst, mean = "fps (worst session)", pc.MEAN_FPS
    threads, busy = "scheduler threads per frame (ms)", "video cpu busy (%)"
    rows = {worst: (33, 41), mean: (34, 42), threads: (4, 3.5), busy: (50, 62)}
    rows["sch_1 avg loop (ns)"] = (1000, 1240)
    assert _verdict(rows) == (
        "verdict i",
        "✅ branch improves 3 of the 3 key metrics and regresses none.",
    )
    page = _summary(rows)
    assert "RX: the builds delivered different frame rates, as under overload" in page
    keys = page[page.index("<h3>Key metrics</h3>") :]
    assert busy not in keys[: keys.index("</table>")]
    assert _verdict({worst: (41, 33), mean: (42, 34)})[1].startswith(
        "❌ branch regresses 2 of the 2"
    )
    # a dip of one session under the rate delivers the same frames: the costs of a
    # second compare, and the busier build regresses
    rows.update({worst: (59.94, 55), mean: (59.94, 59.9), threads: (4, 4.4)})
    assert _verdict(rows)[0] == "verdict r"
    assert "as under overload" not in _summary(rows)
    assert "regresses 3 of the 3 key metrics" in _summary(rows)
    # each role on its own: TX at equal frames keeps its costs of a second
    tx = {mean: (59.94, 59.94), busy: (50, 55)}
    valid = [
        {
            x: {"rx": _valid(rows)[r][x]["rx"], "tx": _valid(tx)[r][x]["rx"]}
            for x in "AB"
        }
        for r in (0, 1)
    ]
    valid[0]["A"]["rx"][mean] = valid[1]["A"]["rx"][mean] = 34  # RX overloaded
    page = pc._summary(valid, {"rx": "s", "tx": "c"}, NAMES)
    assert "RX: the builds delivered" in page and "TX: the builds" not in page
    keys = page[page.index("<h3>Key metrics</h3>") :]
    keys = keys[: keys.index("</table>")]
    assert "TX · video cpu busy (%) ↓" in keys and "RX · video cpu busy" not in keys


def test_the_verdict_of_an_overloaded_role_scores_its_late_frames():
    # more frames, but three times as many sent late: not "regresses none"
    worst, mean = "fps (worst session)", pc.MEAN_FPS
    rows = {
        worst: (33, 41),
        mean: (34, 42),
        "transmitter mismatch troffset": (1e3, 3e3),
    }
    assert _verdict(rows) == (
        "verdict r",
        "❌ branch regresses 1 of the 3 key metrics and improves 2.",
    )


def test_the_verdict_counts_the_frames_of_an_rx_that_its_tx_overloads_once():
    worst, mean = "fps (worst session)", pc.MEAN_FPS
    both = [
        {x: {role: dict(v[x]["rx"]) for role in ("rx", "tx")} for x in "AB"}
        for v in _valid({worst: (33, 41), mean: (34, 42)})
    ]
    page = pc._summary(both, {"rx": "s", "tx": "c"}, NAMES)
    assert "improves 2 of the 2 key metrics" in page  # TX's, not RX's again
    # an RX short of what its TX sends, as when it skips frames, counts on its own,
    # a worst session of its own too
    for v in both:
        v["B"]["rx"] = {worst: 36, mean: 37}
    page = pc._summary(both, {"rx": "s", "tx": "c"}, NAMES)
    assert "improves 4 of the 4 key metrics" in page
    for v in both:
        v["B"]["rx"] = {worst: 30, mean: v["B"]["tx"][mean]}
    page = pc._summary(both, {"rx": "s", "tx": "c"}, NAMES)
    keys = page[page.index("Key metrics") :]
    assert "RX · fps (worst session) ↑" in keys and "RX · fps (mean" not in keys
    # an RX frame rate change with its TX's unchanged, each within 1%, is scored, as
    # the TX's is not: +1.7% TX, +2.4% RX
    for v in both:
        v["A"]["tx"], v["B"]["tx"] = {worst: 46.0, mean: 46.0}, {
            worst: 46.8,
            mean: 46.8,
        }
        v["A"]["rx"], v["B"]["rx"] = {worst: 45.6, mean: 45.6}, {
            worst: 46.7,
            mean: 46.7,
        }
    page = pc._summary(both, {"rx": "s", "tx": "c"}, NAMES)
    assert "improves 2 of the 2 key metrics" in page
    # an RX limit of its own, with its TX at the rate
    for v in both:
        v["A"]["tx"] = v["B"]["tx"] = {worst: 59.94, mean: 59.94}
    page = pc._summary(both, {"rx": "s", "tx": "c"}, NAMES)
    assert "improves 2 of the 2 key metrics" in page and "RX: the builds" in page


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


def test_the_job_summary_holds_the_verdict_and_every_row():
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
        "<h2>RX (s&lt;t)</h2>",
        "<h3>Tasklet call times, every call timed</h3>",
    )
    assert sorted(order, key=page.index) == list(order)
    assert "<details" not in page  # each role's tables open, as in the report
    assert "Test Information" not in page and "Instance Details" not in page
    assert "99,999" not in page and "<td>rx0 (B): late</td>" in page  # left out, named
    # a legend of what shows without style: no grey, no bars
    assert "The better value is bold." in page
    assert "in grey" not in page and "The bar of a change" not in page
    assert _Tags(page).open == []


def test_the_report_shows_every_row_and_the_cpus_and_utilization_of_each_stage():
    rows = {
        "fps (worst session)": (59.94, 59.94),
        "sch_1 avg loop (ns)": (100, 100),
        "sch_2 avg loop (ns)": (100, 120),
        "scheduler threads (CPUs)": (1, 2),  # one more: a regression
        "scheduler threads utilization (%)": (100, 100),
        "application threads (CPUs)": (3, 2),  # what the schedulers leave: no direction
        "application threads utilization (%)": (40, 60),
        "application threads per frame (ms)": (0.2, 0.24),
    }
    same = {x: {"rx": {k: v[i] for k, v in rows.items()}} for i, x in enumerate("AB")}
    rounds = [_round(same, ("A", "B")), _round(same, ("B", "A"))]
    page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, [("s (RX)", {}), ("c", {})])
    section = page[page.index("<h2>RX (s&lt;t)</h2>") :]
    section = section[: section.index('<div class="sec">')]
    # a group's unchanged row in the table of its changes, none folded
    loops = section[section.index("<h3>Scheduler loops</h3>") :]
    loops = loops[: loops.index("</table>")]
    assert '<tr class="r"><td>sch_2 avg loop (ns) ↓</td>' in loops
    assert "<tr><td>sch_1 avg loop (ns) ↓</td>" in loops
    assert "<details" not in section
    # a row per stage, in order, of its CPUs, their utilization and its CPU per frame
    stages = section[section.index('<table class="mx tm stg">') :]
    stages = stages[: stages.index("</table>")]
    head = "<th>Threads</th>\n<th>CPUs</th>\n<th>utilization (%)</th>\n"
    assert head + "<th>CPU per frame (ms) ↓</th>" in stages
    trs = stages.split("<tr><td>")[1:]
    assert [t[: t.index("</td>")] for t in trs] == [pc.STAGE_TEXT[s] for s in pc.STAGES]
    by = dict(zip(pc.STAGES, trs))
    assert re.findall(r'<td class="(\w+)"', by["scheduler"]) == ["r", "u", "u"]
    assert "<b>1</b> → 2<small>❌ +100.0% ±0.0%</small>" in by["scheduler"]
    assert ">100 → 100<small>~ (+0.0%) ±0.0%</small>" in by["scheduler"]
    assert re.findall(r'<td class="(\w+)"', by["application"]) == ["d", "d", "r"]
    assert ">3 → 2<small>➖ -33.3% ±0.0%</small>" in by["application"]
    assert ">40 → 60<small>➖ +50.0% ±0.0%</small>" in by["application"]
    assert by["other"].count('<td class="u">n/a</td>') == 3
    summary = pc.summary_html(LABELS, ARGS, SLOTS, rounds)
    assert _Tags(page).open == [] and _Tags(summary).open == []


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
    assert "<th>Round</th>\n<th>Slot 0</th>\n<th>Slot 1</th>\n<th>Result</th>" in page
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


def test_the_builds_list_every_input_of_the_run_in_the_report_and_the_summary():
    rounds = [_round(_rounds((1, 1))[0])]
    platforms = [("s (RX)", {}), ("c (TX)", {})]
    inputs = [("video", "1080p59"), ("sessions", 37), ("nics", "split: a<b")]
    rows = (
        "<td>Extra RxTxApp arguments</td>",
        "<th>Input</th>\n<th>Selected</th>",
        "<td>video</td><td>1080p59</td>",
        "<td>sessions</td><td>37</td>",
        "<td>nics</td><td>split: a&lt;b</td>",
        "<h2>Rounds</h2>",
    )
    for page in (
        pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms, inputs=inputs),
        pc.summary_html(LABELS, ARGS, SLOTS, rounds, inputs=inputs),
    ):
        assert sorted(rows, key=page.index) == list(rows)  # in the builds, in order
    assert "<th>Input</th>" not in pc.summary_html(LABELS, ARGS, SLOTS, rounds)


def test_the_report_says_when_each_instance_ran_on_one_scheduler():
    rounds = [_round(_rounds((1, 1))[0])]
    platforms = [("s (RX)", {}), ("c (TX)", {})]
    for roles, shown in ((("rx",), "RX"), (("rx", "tx"), "RX and TX")):
        row = f"<td>Single core</td><td>{shown}</td><td>{shown}</td>"
        page = pc.report_html(LABELS, ARGS, [], SLOTS, rounds, platforms, roles)
        assert row in page
        assert row in pc.summary_html(LABELS, ARGS, SLOTS, rounds, roles)


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
        self.single_core = ()  # the roles on one scheduler
        self.queues = None
        self.slots = [pc.Slot(i, 0, [4 + 4 * i], True, [i]) for i in (0, 1)]
        self.pmu = {}  # the perf stat counts of an instance, PMU without
        self.paced = {}  # the pacing way lines of an instance's log
        self.session = "st20p"  # the session type of every instance
        self.coded = {}  # the sessions on the codec plugin of an st22p instance's log
        self.scripts = _sh(monkeypatch, self.sh)
        for name, value in {
            "stop_units": lambda host: None,
            "runner_busy": lambda host: self.busy,
            "_wait_steady": lambda *a: ([], []),
            "inactive": lambda host, names: [n for n in names if n in self.dead],
            "rdt_counters": lambda host, group: None,
            "stream": lambda host, script: f"what {script} read",
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
        if "/physfn/net)" in script:  # keep_learned(): the PF and MAC of each VF
            vfs = re.findall(r"devices/(\w+)/physfn", script)
            return " ".join(f"pf-{vf} 02:00:00:00:00:0{vf[-1]}" for vf in vfs)
        if script.startswith("grep -qsF"):
            log = script.split()[-4]
            return "found\n" if log.rsplit("/", 1)[-1] in self.fallback else ""
        if script.startswith("grep -ho 'pacing way: [a-z_]*' "):
            return self.paced.get(script.rsplit("/", 1)[-1], "")
        if script.startswith("grep -cs 'get one session at [0-9]* on dev "):
            return f"{self.coded.get(script.rsplit('/', 1)[-1], 2)}\n"
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
        vfs = [f"vf{p}" for p in range(1 + max(p for s in self.slots for p in s.ports))]
        sides = [
            pc.Side(host, r, "/mtl", vfs, self.slots, None, self.perf)
            for r in ("rx", "tx")
        ]
        self.launches, self.reads = [], {}
        return pc.run_round(
            sides,
            {"A": "/a", "B": "/b"},
            pc.round_order(0),
            lambda side, slot, label: (
                f"app {label}"
                + " --dma_dev d" * (side.role == "rx" and label in self.dma),
                {
                    "slot": slot,
                    "interfaces": [
                        {"ip": f"10.0.{int(side.role == 'tx')}.{p}"}
                        for p in side.slots[slot].ports
                    ],
                    f"{side.role}_sessions": [{self.session: [{"replicas": 2}]}],
                },
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
            "config": {
                "slot": slot,
                "interfaces": [{"ip": f"10.0.{int(role == 'tx')}.{slot}"}],
                f"{role}_sessions": [{"st20p": [{"replicas": 2}]}],
            },
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
    slot = pc.Slot(0, 0, [4, 5, 6, 7], True)
    rdt = "/sys/fs/resctrl/mtl_perf_0"
    inst = pc.Instance(None, "rx0", "rx", "A", slot, "/t/a", "/run/A_rx0", rdt)
    inst.start("/mtl", "RxTxApp --x")
    assert written == {"/run/A_rx0.sh": inst.script + "\n"} and inst.pid == 7
    assert inst.script.startswith("mount --bind /t/a /mtl/.local_install && ")
    join = f"&& echo $$ > {rdt}/tasks && cd /mtl && exec env LD_LIBRARY_PATH="
    assert join in inst.script and inst.script.endswith(" RxTxApp --x")
    # MTL makes CPU 0 the main lcore of every instance
    assert props == ["AllowedCPUs=0,4-7", "AllowedMemoryNodes=0", "Nice=-20"]
    # a slot off the main lcore's node: DPDK puts the dmadev data on that node
    for main_node, mems in ((0, "0,2"), (1, "1-2")):
        props.clear()
        inst.slot = pc.Slot(2, 2, [68, 69], True, [2], main_node=main_node)
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


def test_run_round_measures_an_overloaded_round(fake, monkeypatch):
    fake.runs["mtl-perf-rx0"] = _records(fps=FPS * 0.95)
    results, problems, notes = fake.round()
    assert problems == []
    assert notes == [f"mtl-perf-rx0 (main): a session fell to {FPS * 0.95:.2f} fps"]
    assert results["A"]["rx"]["fps (worst session)"] == pytest.approx(FPS * 0.95)
    # the note of a wait that every instance ended under the rate, before the round's
    under = (
        "under the rate after 126 s: mtl-perf-rx0 (56.9 fps at worst, 2 of 2 sessions)"
    )
    monkeypatch.setattr(pc, "_wait_steady", lambda *a: ([], [under]))
    assert fake.round()[1:] == ([], [under, notes[0]])


def test_run_round_rejects_a_session_that_collapsed_or_has_no_flow(fake):
    # under a tenth of the rate: before the DMA gate, as a session that receives
    # nothing copies nothing
    fake.dma = "A"
    fake.runs["mtl-perf-rx0"] = _records(fps=FPS * 0.09)
    collapsed = f"a session at {FPS * 0.09:.2f} fps, collapsed or no flow"
    assert fake.round()[1] == [f"mtl-perf-rx0 (main): {collapsed}"]
    fake.runs["mtl-perf-rx0"] = _records(fps=FPS * 0.11, dma=(5, 5))
    assert fake.round()[1] == []
    # one session of the instance without a flow, the other at the rate
    one = [
        r.replace(f"(1,1:app_1): fps {FPS}", "(1,1:app_1): fps 0") for r in _records()
    ]
    fake.dma, fake.runs["mtl-perf-rx0"] = "", one
    no_flow = "mtl-perf-rx0 (main): a session at 0.00 fps, collapsed or no flow"
    assert fake.round()[1] == [no_flow]


def test_run_round_keeps_the_switch_learning_each_rx_port(fake):
    fake.slots = _nics("whole")  # two ports a slot
    fake.round()
    (unit,) = [c for c in fake.scripts if "--unit=mtl-perf-arp" in c]
    # each RX port asked for from the companion's PF of that port, by its own IP
    frames = re.findall(r"(pf-vf\d) ([0-9a-f]+)", unit)
    assert [pf for pf, _ in frames] == ["pf-vf0", "pf-vf1", "pf-vf2", "pf-vf3"]
    probes = [pc.arp_probe(f"02:00:00:00:00:0{p}", f"10.0.0.{p}") for p in range(4)]
    assert [bytes.fromhex(f) for _, f in frames] == probes
    assert f"-p AllowedCPUs={pc.COLLECTOR_CPU}" in unit
    assert f"python3 -I {pc.RUN_DIR}/arp.py pf-vf0 " in unit
    # its log kept, as the next round empties RUN_DIR
    log = (fake.tmp_path / "arp.log").read_text()
    assert log == f"what [ ! -f {pc.RUN_DIR}/arp.log ] || cat {pc.RUN_DIR}/arp.log read"


def test_keep_learned_refuses_a_vf_whose_pf_it_cannot_find(monkeypatch):
    _sh(monkeypatch, lambda script: "pf0 02:00:00:00:00:00\n")  # the second VF none
    with pytest.raises(RuntimeError, match="no PF and MAC of each VF"):
        pc.keep_learned(HOST, [("vf0", "10.0.0.1"), ("vf1", "10.0.0.2")])


def test_an_arp_probe_asks_for_an_ip_from_a_mac_with_no_sender_ip():
    probe = pc.arp_probe("02:aa:bb:cc:dd:ee", "192.168.7.1")
    mac = bytes.fromhex("02aabbccddee")
    assert len(probe) == 42 and probe[:14] == b"\xff" * 6 + mac + b"\x08\x06"
    # Ethernet/IPv4, request, sender MAC and IP 0, target MAC 0 and IP
    assert probe[14:22] == bytes.fromhex("0001080006040001")
    assert probe[22:42] == mac + bytes(4) + bytes(6) + bytes([192, 168, 7, 1])


def test_the_arp_script_sends_each_frame_out_of_its_interface_every_period(
    monkeypatch, capsys
):
    sent = []

    class Sock:
        def __init__(self, family, kind):
            assert (family, kind) == ("packet", "raw")

        def bind(self, addr):
            self.iface = addr[0]

        def send(self, frame):
            if self.iface == "down" and not sent:
                raise OSError("Network is down")
            sent.append((self.iface, frame))

    class Done(Exception):
        pass

    def sleep(s):
        assert s == pc.ARP_PERIOD_S
        if len(sent) > 2:
            raise Done

    fake_socket = SimpleNamespace(AF_PACKET="packet", SOCK_RAW="raw", socket=Sock)
    monkeypatch.setitem(sys.modules, "socket", fake_socket)
    monkeypatch.setitem(sys.modules, "time", SimpleNamespace(sleep=sleep))
    monkeypatch.setattr(sys, "argv", ["arp.py", "pf0", "0a0b", "pf1", "0c"])
    with pytest.raises(Done):
        exec(pc.ARP_SCRIPT, {})
    assert sent == [("pf0", b"\x0a\x0b"), ("pf1", b"\x0c")] * 2
    assert capsys.readouterr().out.splitlines() == ["pf0 0a0b", "pf1 0c"]
    # a link down refuses one frame, said with its interface, and the script goes on
    sent.clear()
    monkeypatch.setattr(sys, "argv", ["arp.py", "down", "0a", "pf1", "0c"])
    with pytest.raises(Done):
        exec(pc.ARP_SCRIPT, {})
    assert sent == [("pf1", b"\x0c"), ("down", b"\x0a"), ("pf1", b"\x0c")]
    out = capsys.readouterr().out.splitlines()
    assert out == ["down 0a", "pf1 0c", "down Network is down"]
    # SIGINT, as systemd stops it, ends it quietly

    def interrupt(s):
        raise KeyboardInterrupt

    monkeypatch.setitem(sys.modules, "time", SimpleNamespace(sleep=interrupt))
    exec(pc.ARP_SCRIPT, {})


def _sent(records, rx=None, tx=None, since=0, until=LAST):
    """*records* with the DEV(0) line of every dump from *since* to *until* s receiving
    *rx* and sending *tx* packets."""
    out = []
    for r in records:
        if (
            " DEV(0): Avr rate" in r
            and since * S <= int(r.split(" ", 1)[0]) <= until * S
        ):
            got, sent = re.search(r"pkts, tx: (\d+), rx: (\d+)", r).groups()[::-1]
            new_tx, new_rx = (sent if tx is None else tx), (got if rx is None else rx)
            r = r.replace(
                f"pkts, tx: {sent}, rx: {got}", f"pkts, tx: {new_tx}, rx: {new_rx}"
            )
        out.append(r)
    return out


def test_run_round_rejects_a_window_whose_packets_the_network_lost(fake):
    # a TX receives next to nothing, and is not held to it
    tx = {f"mtl-perf-tx{s}": _sent(_records(tx=True), 0, 50000) for s in (0, 1)}
    fake.runs = dict(tx)
    assert fake.round()[1] == []  # every packet arrived
    # from 110 s, the timed and the sessions windows, both RX get a third: the wire
    flooded = _sent(_records(), rx=16000, since=110)
    fake.runs.update({"mtl-perf-rx0": flooded, "mtl-perf-rx1": flooded})
    lost = "got 32% in the timed, 32% in the sessions window of what its TX sent, and"
    lost += " counted no drop of the rest: lost between the hosts or in its NIC"
    assert fake.round()[1] == [
        f"mtl-perf-rx0 (main): {lost}",
        f"mtl-perf-rx1 (dev): {lost}",
    ]
    # however few frames that leaves, it is not a collapse
    zero = [r.replace(f"fps {FPS}", "fps 0") for r in flooded]
    fake.runs.update({"mtl-perf-rx0": zero, "mtl-perf-rx1": zero})
    assert fake.round()[1] == [
        f"mtl-perf-rx0 (main): {lost}",
        f"mtl-perf-rx1 (dev): {lost}",
    ]
    # each RX against the TX of its own slot
    halved = {"mtl-perf-tx1": _sent(_records(tx=True), 0, 25000)}
    fake.runs = {**tx, **halved, "mtl-perf-rx1": _sent(_records(), rx=25000)}
    assert fake.round()[1] == []
    fake.runs = {**tx, "mtl-perf-rx1": flooded}
    assert fake.round()[1] == [f"mtl-perf-rx1 (dev): {lost}"]
    # a flood in one dump at the edge of the untraced window, which /proc bounds on both
    # hosts alike, counts
    edge = _sent(_records(), rx=16000, since=60, until=60)
    fake.runs = {**tx, "mtl-perf-rx0": edge, "mtl-perf-rx1": edge}
    assert [re.search(r"in the (\w+) window", p)[1] for p in fake.round()[1]] == [
        "untraced"
    ] * 2
    # a step in the last dump of a later window only, the sessions window's 180 s: one
    # dump of each end is left out, as the hosts start it seconds apart; one more counts
    for since, short in ((180, []), (170, ["sessions"] * 2)):
        edge = _sent(_records(), rx=16000, since=since)
        fake.runs = {**tx, "mtl-perf-rx0": edge, "mtl-perf-rx1": edge}
        problems = fake.round()[1]
        assert [re.search(r"in the (\w+) window", p)[1] for p in problems] == short
    # what its NIC dropped is the receiver's own, which the loss rows score
    dropped = [
        r.replace("rx_hw_dropped_packets 1", "rx_hw_dropped_packets 34000")
        for r in _sent(_records(lost=1), rx=16000, since=110)
    ]
    fake.runs = {**tx, "mtl-perf-rx0": dropped}
    assert fake.round()[1] == []


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
    one = "mtl-perf-rx1 (dev): 2 schedulers, single core runs one"
    for roles in (("rx",), ("rx", "tx")):
        fake.single_core = roles
        assert fake.round()[1] == [one]


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
    monkeypatch.setattr(pc, "_wait_steady", lambda *a: waits.append(a[1:]) or ([], []))
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
    fake.busy = True
    fake.pin = "no scheduler thread"
    _, problems, notes = fake.round()
    assert problems == []
    assert notes == [
        *(
            f"mtl-perf-{n}: app threads not moved, no scheduler thread"
            for n in ("rx0 (main)", "rx1 (dev)", "tx0 (main)", "tx1 (dev)")
        ),
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


def _steady_insts(monkeypatch, records, dead=(), pinned=None, pin="", st22p=False):
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
            st22p=st22p,
            units=[f"rx{i}"],
            records=lambda tail=0, i=i: records[i],
            pin_app_threads=lambda i=i: pinned.append((f"rx{i}", clock[0])) or pin,
        )
        for i in (0, 1)
    ]


def test_wait_steady_waits_for_the_last_dump_of_every_instance(monkeypatch):
    steady, short = _dump(S), _dump(S, sessions=1)
    insts = _steady_insts(monkeypatch, [steady, short + steady])
    assert pc._wait_steady(insts, 2, FPS) == ([], [])
    insts = _steady_insts(monkeypatch, [steady, steady + short])
    timeout = pc.steady_timeout(2)
    behind = "rx1 (59.9 fps at worst, 1 of 2 sessions)"  # how far it was
    not_steady = ([f"not steady after {timeout} s: {behind}"], [])
    assert pc._wait_steady(insts, 2, FPS) == not_steady
    timeout = pc.steady_timeout(4)
    assert pc._wait_steady(insts, 2, FPS, 4) == (
        [f"not steady after {timeout} s: {behind}"],
        [],
    )
    insts = _steady_insts(monkeypatch, [_dump(S, fps=40.0), []])
    assert pc._wait_steady(insts, 2, FPS)[0][0].endswith(
        ": rx0 (40.0 fps at worst, 2 of 2 sessions), rx1 (no session yet)"
    )
    # every session there, some under the rate: overloaded, which the round measures
    insts = _steady_insts(monkeypatch, [_dump(S, fps=40.0), steady])
    timeout = pc.steady_timeout(2)
    assert pc._wait_steady(insts, 2, FPS) == (
        [],
        [f"under the rate after {timeout} s: rx0 (40.0 fps at worst, 2 of 2 sessions)"],
    )


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
    assert pc._wait_steady(insts, 2, FPS) == ([], [])
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
    assert pc._wait_steady(insts, 2, FPS) == (["exited before steady state: rx0"], [])


def _tx_config(*entries):
    """A TX RxTxApp configuration of st20p session entries, (dips, sessions) each."""
    return {
        "tx_sessions": [
            {
                "dip": list(dips),
                "interface": [0],
                "st20p": [{"replicas": n, "start_port": 5004}],
            }
            for dips, n in entries
        ]
    }


def test_multicast_groups_put_a_few_sessions_to_a_group_of_each_leg():
    # whole: an entry a port, 10 sessions on the first and 3 on the second
    tx = _tx_config((["10.0.1.1"], 10), (["10.0.2.1"], 3))
    rx = {
        "rx_sessions": [
            {"ip": [ip], "st20p": [{"replicas": n, "start_port": 5004}]}
            for ip, n in (("10.0.0.1", 10), ("10.0.0.2", 3))
        ]
    }
    pc.multicast_groups(tx, "tx", 4)
    pc.multicast_groups(rx, "rx", 4)
    groups = [(e["dip"], e["st20p"][0]["replicas"]) for e in tx["tx_sessions"]]
    assert groups == [
        (["239.168.0.1"], 4),
        (["239.168.0.2"], 4),
        (["239.168.0.3"], 2),
        (["239.168.2.1"], 3),
    ]
    # the RX receives the groups its TX sends, and the rest of an entry stays
    assert [e["ip"] for e in rx["rx_sessions"]] == [g for g, _ in groups]
    assert all(e["interface"] == [0] for e in tx["tx_sessions"])
    assert pc.stream_groups(tx)[2] == (
        "239.168.0.3",
        ["239.168.0.3:5004", "239.168.0.3:5006"],
    )
    assert sum(len(streams) for _, streams in pc.stream_groups(tx)) == 13
    # redundant: each leg of an entry has groups of its own, captured apart
    red = _tx_config((["10.0.1.1", "10.0.3.1"], 5))
    pc.multicast_groups(red, "tx", 4)
    assert [e["dip"] for e in red["tx_sessions"]] == [
        ["239.168.0.1", "239.168.1.1"],
        ["239.168.0.2", "239.168.1.2"],
    ]
    assert [g for g, _ in pc.stream_groups(red)] == [
        "239.168.0.1",
        "239.168.1.1",
        "239.168.0.2",
        "239.168.1.2",
    ]


def _stream(port, compliance="narrow", failed=(), media="video"):
    """A stream of an EBU LIST report, as LIST 2.2 writes it."""
    return {
        "media_type": media,
        "network_information": {
            "destination_address": "239.168.0.1",
            "destination_port": port,
        },
        "global_video_analysis": {
            "compliance": compliance,
            "cinst": {"histogram": [[0, 90.0], [1, 9.0], [2, 1.0]]},
            "vrx": {"histogram": [[1, 5.0], [5, 95.0]]},
        },
        "media_specific": {"min_tro_ns": 621033, "max_tro_ns": 629242},
        "analyses": {
            check: {"result": "not_compliant" if check in failed else "compliant"}
            for check in ("rtp_sequence", "2110_21_vrx")
        },
    }


def test_list_verdicts_take_each_video_stream_by_its_timing_and_every_check():
    report = {
        "streams": [
            _stream(5004),
            _stream(5006, failed=("rtp_sequence",)),
            _stream(5008, "wide"),
            _stream(5010, "narrow_linear"),
            _stream(5012, media="audio"),
        ]
    }
    verdicts = pc.list_verdicts(report)
    assert verdicts["239.168.0.1:5004"] == {
        "class": "narrow",
        "failed": [],
        "cinst": 2,
        "vrx": [1, 5],
        "tro_us": [621.0, 629.2],
    }
    assert verdicts["239.168.0.1:5006"]["failed"] == ["rtp_sequence"]
    # narrow by its timing is not enough: a stream fails with any check of LIST
    ok = [pc.compliant(verdicts[f"239.168.0.1:{p}"]) for p in (5004, 5006, 5008, 5010)]
    assert ok == [True, False, False, True] and len(verdicts) == 4
    assert not pc.compliant({"why": "not captured whole"})
    # what another LIST version leaves out is no verdict, and no error
    (bare,) = pc.list_verdicts({"streams": [{"media_type": "video"}]}).values()
    assert not pc.compliant(bare) and bare["cinst"] is None
    assert pc.list_verdicts({"streams": None}) == {}


def _pcap(path, packets):
    """Write a ns pcap of UDP/RTP *packets*, (address, port, sequence, marker) each."""
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B23C4D, 2, 4, 0, 0, 128, 1))
        for n, (address, port, seq, marker) in enumerate(packets):
            ip = bytes([0x45, 0, 0, 0, 0, 0, 0, 0, 64, 17, 0, 0, 10, 0, 0, 1])
            udp = struct.pack(">HHHH", 5000, port, 0, 0)
            rtp = bytes([0x80, 96 | marker << 7]) + struct.pack(">H", seq) + bytes(8)
            frame = bytes(12) + b"\x08\x00" + ip + socket.inet_aton(address) + udp + rtp
            f.write(struct.pack("<IIII", 1, n, len(frame), len(frame)) + frame)


def _sequences(path):
    with open(path, "rb") as f:
        f.read(24)
        out = []
        while len(h := f.read(16)) == 16:
            frame = f.read(struct.unpack("<IIII", h)[2])
            out.append(struct.unpack(">HxxxxxxH", frame[36:46]))  # port, sequence
        return out


def _capture_script(monkeypatch, tmp_path, packets, filters="", tcpdump=0):
    """Run CAPTURE_SCRIPT on fakes of tc, tcpdump and the socket: tcpdump captures
    *packets*, and the PF has *filters* of another. Returns (its JSON, its calls)."""
    calls = []
    group = "239.168.0.1"

    def run(cmd, capture_output, text):
        calls.append(" ".join(cmd))
        if cmd[0] == "timeout":
            _pcap(cmd[cmd.index("-w") + 1], packets)
            error = "tcpdump: ice1: down" if tcpdump else ""
            return SimpleNamespace(returncode=tcpdump, stderr=error, stdout="")
        shown = filters if cmd[:3] == ("tc", "filter", "show") else ""
        return SimpleNamespace(returncode=0, stderr="", stdout=shown)

    class Sock:
        def __init__(self, family, kind):
            assert (family, kind) == (socket.AF_INET, socket.SOCK_DGRAM)

        def setsockopt(self, level, option, value):
            calls.append(("join", option, value))

        def close(self):
            calls.append("leave")

    real = {k: getattr(socket, k) for k in ("AF_INET", "SOCK_DGRAM", "IPPROTO_IP")}
    fake_socket = SimpleNamespace(
        **real,
        IP_ADD_MEMBERSHIP=socket.IP_ADD_MEMBERSHIP,
        inet_aton=socket.inet_aton,
        inet_ntoa=socket.inet_ntoa,
        if_nametoindex=lambda name: {"pf1": 7}[name],
        socket=Sock,
    )
    monkeypatch.setitem(sys.modules, "socket", fake_socket)
    monkeypatch.setitem(sys.modules, "subprocess", SimpleNamespace(run=run))
    monkeypatch.setitem(sys.modules, "time", SimpleNamespace(sleep=lambda s: None))
    raw, out = tmp_path / "g.raw", tmp_path / "g.pcap"
    argv = ["capture.py", "pf1", group, str(len(packets)), str(raw), str(out)]
    monkeypatch.setattr(sys, "argv", argv)
    printed = io.StringIO()
    monkeypatch.setattr(sys, "stdout", printed)
    exec(pc.CAPTURE_SCRIPT, {"__name__": "__main__"})
    monkeypatch.undo()
    return json.loads(printed.getvalue()), calls


def test_the_capture_script_keeps_each_streams_whole_frames_of_a_group(
    monkeypatch, tmp_path
):
    g = "239.168.0.1"
    packets = [
        (g, 5004, 1, 0),
        (g, 5004, 2, 1),  # the end of the frame the capture began in
        (g, 5006, 7, 1),
        (g, 5004, 3, 0),
        (g, 5006, 9, 0),  # 8 lost in the first frame, as libpcap loses them
        (g, 5004, 4, 1),
        (g, 5006, 10, 1),
        (g, 5010, 1, 1),
        (g, 5004, 5, 0),
        (g, 5006, 11, 0),
        (g, 5010, 2, 1),
        (g, 5004, 6, 1),
        (g, 5006, 12, 1),
        (g, 5010, 3, 0),
        (g, 5010, 5, 0),  # 4 lost in a frame kept
        (g, 5010, 6, 1),
        (g, 5004, 7, 0),  # the frame the capture ended in
        (g, 5008, 1, 1),
        (g, 5008, 2, 1),  # no whole frame after its first
        (g, 5012, 1, 1),
        (g, 5012, 2, 1),
        (g, 5012, 4, 0),  # 3 lost right after the second marker
        (g, 5012, 5, 1),
    ]
    result, calls = _capture_script(monkeypatch, tmp_path, packets)
    gaps = {f"{g}:5004": 0, f"{g}:5006": 0, f"{g}:5010": 1, f"{g}:5012": 1}
    assert result == {"rc": 0, "error": "", "gaps": gaps}
    kept = [(5004, 5), (5006, 11), (5004, 6), (5006, 12)]
    kept += [(5010, 3), (5010, 5), (5010, 6), (5012, 4), (5012, 5)]
    assert _sequences(tmp_path / "g.pcap") == kept
    drop = "dev pf1 ingress pref 2110"
    join = socket.inet_aton(g) + bytes(4) + struct.pack("@i", 7)
    assert calls == [
        "tc qdisc add dev pf1 clsact",
        f"tc filter del {drop}",
        f"tc filter add {drop} protocol ip flower skip_hw dst_ip {g} action drop",
        ("join", socket.IP_ADD_MEMBERSHIP, join),
        # not promiscuous, which stalls the TX of the PF's VFs; NIC times in ns
        "timeout 30 tcpdump -p -q -i pf1 -j adapter_unsynced "
        "--time-stamp-precision=nano -s 128 -B 1000000 -c 23 "
        f"-w {tmp_path / 'g.raw'} udp and dst host {g}",
        "leave",
        f"tc filter del {drop}",
        "tc filter show dev pf1 ingress",
        "tc filter show dev pf1 egress",
        "tc qdisc del dev pf1 clsact",
    ]
    # a qdisc with filters of another stays; a capture that failed keeps nothing
    result, calls = _capture_script(
        monkeypatch, tmp_path, packets, filters="filter protocol all", tcpdump=1
    )
    assert result == {"rc": 1, "error": "tcpdump: ice1: down", "gaps": {}}
    assert calls[-1] == "tc filter show dev pf1 ingress"


def test_a_capture_is_judged_only_whole():
    streams = ["g:5004", "g:5006"]
    # another group's gaps are not this one's
    whole = {"rc": 0, "error": "", "gaps": {"g:5004": 0, "g:5006": 0, "g:5008": 4}}
    assert pc._captured(whole, streams) == ""
    for change, why in (
        ({"rc": 124, "error": "timed out"}, "tcpdump exited 124: timed out"),
        ({"gaps": {"g:5004": 0}}, "no whole frame of g:5006"),
        ({"gaps": {"g:5004": 2, "g:5006": 1}}, "3 gaps in the RTP sequence"),
    ):
        assert pc._captured({**whole, **change}, streams) == why


EBU = {"ebu_ip": "10.1.1.1", "user": "u", "password": "secret", "proxy": False}


def test_list_report_sends_the_password_on_stdin_and_deletes_the_pcap_there(
    monkeypatch,
):
    commands, deleted, reports = [], [], []
    upload = SimpleNamespace(stdout="uploaded\n>>>UUID: id1\n")

    def execute_command(cmd, **kw):
        commands.append((cmd, kw["input_data"]))
        return upload

    host = SimpleNamespace(connection=SimpleNamespace(execute_command=execute_command))

    class Client:
        def __init__(self, ip, user, password, pcap_id):
            assert (ip, user, password, pcap_id) == ("10.1.1.1", "u", "secret", "id1")

        def download_report(self):
            return reports.pop(0)

        def reanalyze(self, previous):
            assert previous["streams"] == []
            return reports.pop(0)

        def delete_pcap(self):
            deleted.append(1)

    monkeypatch.setattr(pc, "PcapComplianceClient", Client)

    def report():
        return pc.list_report(host, "/mtl", EBU, "/r/g.pcap")

    reports[:] = [{"streams": [1]}]
    assert report() == ({"streams": [1]}, "")
    assert commands == [
        (
            "sudo -n timeout 300 python3 /mtl/tests/acceptance/compliance/"
            "upload_pcap.py --ip 10.1.1.1 --user u --password-stdin --pcap /r/g.pcap",
            "secret\n",
        )
    ]
    # a dropped ingest, analysed twice more, as pcap_compliance.py does
    reports[:] = [{"streams": []}, {"streams": []}, {"streams": [2]}]
    assert report() == ({"streams": [2]}, "")
    reports[:] = [{"streams": []}] * 4
    assert report() == ({"streams": []}, "") and len(reports) == 1
    reports[:] = [False]
    assert report() == (None, "LIST did not analyse it in time")

    def unreachable(self):
        raise requests.ConnectionError("down")

    monkeypatch.setattr(Client, "download_report", unreachable)
    with pytest.raises(requests.ConnectionError):  # the capture's to report
        report()
    assert len(deleted) == 5  # every pcap that reached LIST
    upload.stdout = "Error: 401 Unauthorized"
    assert report() == (None, "the upload failed: Error: 401 Unauthorized")
    assert len(deleted) == 5


class _Comply:
    """compliance_round on fake hosts, 6 sessions of build B, 4 to a group."""

    def __init__(self, monkeypatch, tmp_path):
        self.tmp_path = tmp_path
        self.calls = []
        # phc2sys's samples, (offset ns, servo state); *during* join them at a capture
        self.samples = [(90000, 0), (900, 2), (100, 2), (-50, 2), (30, 2)]
        self.during = [(10, 2), (-20, 2)]
        self.tai = ["37", "37"]
        self.dead = set()
        self.steady = ([], [])
        self.waited = []
        g1, g2 = "239.168.0.1", "239.168.0.2"
        self.caps = {
            g: {
                "rc": 0,
                "error": "",
                "gaps": {f"{g}:{5004 + 2 * k}": 0 for k in range(n)},
            }
            for g, n in ((g1, 4), (g2, 2))
        }
        self.reports = {
            f"{pc.RUN_DIR}/{g1}.pcap": (
                {"streams": [_stream(5004), _stream(5006, "wide"), _stream(5008)]},
                "",
            ),
            f"{pc.RUN_DIR}/{g2}.pcap": ({"streams": []}, ""),
        }
        _sh(monkeypatch, self.sh)
        fake = self

        def wait_steady(insts, *a):
            fake.waited.append([i.name for i in insts])
            return fake.steady

        def list_report(host, mtl, ebu, pcap):
            if isinstance(fake.reports[pcap], Exception):
                raise fake.reports[pcap]
            return fake.reports[pcap]

        for name, value in {
            "stop_units": lambda host: fake.calls.append(f"stop_units {host.name}"),
            "runner_busy": lambda host: False,
            "_wait_steady": wait_steady,
            "inactive": lambda host, names: [n for n in names if n in fake.dead],
            "stream": lambda host, script: "the phc2sys log\n",
            "list_report": list_report,
        }.items():
            monkeypatch.setattr(pc, name, value)
        clock = iter(range(0, 10**6, 10))
        monkeypatch.setattr(pc.time, "monotonic", lambda: next(clock))
        monkeypatch.setattr(pc.time, "sleep", lambda s: fake.calls.append(f"sleep {s}"))
        for name, value in {
            "start": lambda inst, mtl, cmd: fake.calls.append(
                f"start {inst.name} {cmd}"
            ),
            "collect": lambda inst, kind, script: None,
            "pin_app_threads": lambda inst: fake.calls.append(f"pin {inst.name}") or "",
            "save": lambda inst, out_dir: fake.calls.append(f"save {inst.name}"),
        }.items():
            monkeypatch.setattr(pc.Instance, name, value)

    def sh(self, script):
        self.calls.append(script)
        if "/physfn/net" in script:
            return "pf1\n"
        if "CLOCK_TAI" in script:
            return self.tai.pop(0)
        if script.startswith("cat ") and script.endswith("/phc2sys.log"):
            line = "phc2sys[1.0]: pf1 sys offset {:9} s{} freq 1 delay 0\n"
            return "".join(line.format(*sample) for sample in self.samples)
        if script.startswith("systemctl show -p MainPID"):
            return "2\n"
        if "/capture.py " in script:
            self.samples += self.during
            self.during = []
            group = script.split()[4]
            if group not in self.caps:
                raise RuntimeError(f"companion: rc 1 from {script!r}: Traceback")
            return "a warning\n" + json.dumps(self.caps[group])
        return ""

    def round(self):
        hosts = SimpleNamespace(name="sut"), SimpleNamespace(name="companion")
        slots = [pc.Slot(i, 0, [4 + 4 * i], True, [i]) for i in (0, 1)]
        sides = [
            pc.Side(host, role, "/mtl", ["vf0", "vf1"], slots, [])
            for host, role in zip(hosts, ("rx", "tx"))
        ]

        def app_cmd(side, slot, label):
            config = _tx_config((["10.0.0.1"], 6))
            pc.multicast_groups(config, "tx", 4)
            return f"app {side.role}{slot} {label}", config

        trees = {"A": "/a", "B": "/b"}
        out = str(self.tmp_path)
        return pc.compliance_round(sides, trees, "B", app_cmd, out, 6, FPS, 1000, EBU)


@pytest.fixture
def comply(monkeypatch, tmp_path):
    return _Comply(monkeypatch, tmp_path)


def test_compliance_round_has_list_judge_every_stream_of_a_build_alone(comply):
    verdicts, problems, notes = comply.round()
    assert problems == [] and notes == []
    g1, g2 = "239.168.0.1", "239.168.0.2"
    assert sorted(verdicts) == [f"{g1}:{p}" for p in (5004, 5006, 5008, 5010)] + [
        f"{g2}:{p}" for p in (5004, 5006)
    ]
    ok = [pc.compliant(verdicts[f"{g1}:{p}"]) for p in (5004, 5006, 5008)]
    assert ok == [True, False, True]
    # what LIST did not report, it did not judge
    none = {"why": "LIST found no video stream there"}
    assert verdicts[f"{g1}:5010"] == verdicts[f"{g2}:5004"] == none
    calls = comply.calls
    # the build alone in slot 0, RX first; the capture on the companion's PF of slot 1
    starts = [c for c in calls if c.startswith("start ")]
    assert starts == ["start mtl-perf-rx0 app rx0 B", "start mtl-perf-tx0 app tx0 B"]
    assert "ls /sys/bus/pci/devices/vf1/physfn/net" in calls
    # only the TX, which LIST judges, has to be steady, and its app threads off the
    # scheduler CPUs, as a TX starved of them sends late
    assert comply.waited == [["mtl-perf-tx0"]] and "pin mtl-perf-tx0" in calls
    (phc,) = [c for c in calls if "--unit=mtl-perf-phc2sys" in c]
    assert "phc2sys -s CLOCK_REALTIME -c pf1 -O 37 -S 0.001 -m" in phc
    assert calls.index(phc) < calls.index(starts[0])  # locked by the time it is steady
    captures = [c for c in calls if "/capture.py " in c]
    run = f"python3 -I {pc.RUN_DIR}/capture.py pf1"
    assert captures == [
        f"{run} {g1} 4000 {pc.RUN_DIR}/{g1}.pcap.raw {pc.RUN_DIR}/{g1}.pcap",
        f"{run} {g2} 2000 {pc.RUN_DIR}/{g2}.pcap.raw {pc.RUN_DIR}/{g2}.pcap",
    ]
    assert calls.index(f"sleep {pc.WARMUP_S}") < calls.index(captures[0])
    assert f"rm -f {pc.RUN_DIR}/{g2}.pcap.raw {pc.RUN_DIR}/{g2}.pcap" in calls
    # what each capture kept and LIST's report of it, and the clock's log
    with open(comply.tmp_path / f"{g1}.json") as f:
        kept = json.load(f)
    assert kept["capture"] == comply.caps[g1] and kept["list"]["streams"]
    assert (comply.tmp_path / "phc2sys.log").read_text() == "the phc2sys log\n"
    # and each instance's command and multicast configuration
    with open(comply.tmp_path / "mtl-perf-tx0.json") as f:
        launch = json.load(f)
    assert launch["command"] == "app tx0 B"
    assert launch["config"]["tx_sessions"][0]["dip"] == [g1]
    # every unit stopped, phc2sys with the companion's, before the logs are copied
    assert calls[-6:] == [
        "systemctl stop mtl-perf-tx0",
        "stop_units companion",
        "systemctl stop mtl-perf-rx0",
        "stop_units sut",
        "save mtl-perf-rx0",
        "save mtl-perf-tx0",
    ]


def test_compliance_round_judges_no_stream_of_a_group_it_did_not_capture_whole(
    comply,
):
    g1, g2 = "239.168.0.1", "239.168.0.2"
    comply.caps[g2]["gaps"][f"{g2}:5006"] = 12
    del comply.reports[f"{pc.RUN_DIR}/{g2}.pcap"]  # LIST never sees it
    verdicts, problems, _ = comply.round()
    assert problems == [] and verdicts[f"{g2}:5006"] == {
        "why": "12 gaps in the RTP sequence"
    }
    # a group that fails on the host or at LIST is its own streams', not the pass's
    del comply.caps[g1]
    comply.caps[g2]["gaps"][f"{g2}:5006"] = 0
    comply.reports[f"{pc.RUN_DIR}/{g2}.pcap"] = requests.ConnectionError("LIST down")
    comply.calls.clear()
    comply.tai = ["37", "37"]
    verdicts, problems, _ = comply.round()
    assert problems == []
    assert verdicts[f"{g1}:5004"]["why"].startswith("failed: companion: rc 1 from ")
    assert verdicts[f"{g2}:5004"] == {"why": "failed: LIST down"}
    assert f"rm -f {pc.RUN_DIR}/{g1}.pcap.raw {pc.RUN_DIR}/{g1}.pcap" in comply.calls


@pytest.mark.parametrize(
    "change, problem, captures",
    [
        (
            {"samples": [(9000, 2), (100, 2), (9000, 2), (100, 2)]},
            "phc2sys held pf1 within 2000 ns of CLOCK_TAI in no three samples in a "
            "row in 30 s",
            0,
        ),
        # during the captures: beyond the bound, or past the step threshold, where the
        # servo leaves s2 and comes back
        (
            {"during": [(10, 2), (2500, 2)]},
            "phc2sys held pf1 not locked within 2000 ns of CLOCK_TAI during the "
            "captures, 2500 ns at worst",
            2,
        ),
        (
            {"during": [(1500000, 0), (40, 1), (10, 2)]},
            "phc2sys held pf1 not locked within 2000 ns of CLOCK_TAI during the "
            "captures, 1500000 ns at worst",
            2,
        ),
        (
            {"during": [(10, 2), (40, 1), (10, 2)]},
            "phc2sys held pf1 not locked within 2000 ns of CLOCK_TAI during the "
            "captures, 40 ns at worst",
            2,
        ),
        ({"tai": ["37", "0"]}, "the TAI-UTC offset of companion changed", 2),
        # found after the capture it died in, the last one too
        ({"dead": {"mtl-perf-tx0"}}, "exited during the captures: mtl-perf-tx0", 1),
        (
            {"dead": {"mtl-perf-phc2sys"}},
            "exited during the captures: mtl-perf-phc2sys",
            1,
        ),
        (
            {"dead": {"mtl-perf-phc2sys"}, "samples": [(90000, 0)]},
            "exited before steady state: mtl-perf-phc2sys",
            0,
        ),
        (
            {"steady": (["not steady after 1 s: tx0"], [])},
            "not steady after 1 s: tx0",
            0,
        ),
    ],
)
def test_compliance_round_says_why_a_pass_is_not_valid(
    comply, change, problem, captures
):
    for k, v in change.items():
        setattr(comply, k, v)
    verdicts, problems, _ = comply.round()
    assert problems == [problem]
    assert sum("/capture.py " in c for c in comply.calls) == captures
    assert bool(verdicts) == bool(captures)


PLATFORMS = [("s (RX)", {}), ("c (TX)", {})]


def test_the_report_shows_what_list_found_of_each_build():
    good = pc.list_verdicts({"streams": [_stream(5004)]})
    wide = pc.list_verdicts({"streams": [_stream(5006, "wide", ("2110_21_vrx",))]})
    passes = [
        {
            "label": "A",
            "verdicts": {
                **good,
                "239.168.0.10:5004": {"why": "a later group"},
                "239.168.0.1:5008": {"why": "2 gaps in the RTP sequence"},
            },
            "problems": [],
            "notes": [],
        },
        {"label": "B", "verdicts": wide, "problems": [], "notes": ["a note"]},
    ]
    rounds = [_round(_rounds((1, 1))[0])]
    names = pc.build_names(LABELS)
    a, b = names["A"], html.escape(names["B"])
    for page in (
        pc.report_html(LABELS, ARGS, [], SLOTS, rounds, PLATFORMS, compliance=passes),
        pc.summary_html(LABELS, ARGS, SLOTS, rounds, compliance=passes),
    ):
        rows = (
            "<h2>Compliance</h2>",
            f"<td>{a}</td><td>3</td><td>1</td><td>0</td><td>2</td><td>valid</td>",
            f"<td>{b}</td><td>1</td><td>0</td><td>1</td><td>0</td><td>a note</td>",
            f"<td>{a}</td><td>239.168.0.1:5008</td><td>not judged</td>"
            "<td></td><td></td><td></td><td></td><td>2 gaps in the RTP sequence</td>",
            f"<td>{a}</td><td>239.168.0.10:5004</td>",  # after .1, as numbers
            f"<td>{b}</td><td>239.168.0.1:5006</td><td>wide</td>"
            "<td>2110_21_vrx</td><td>2</td><td>1 to 5</td><td>621.0 to 629.2</td>",
        )
        assert sorted(rows, key=page.index) == list(rows)
    assert "Compliance" not in pc.summary_html(LABELS, ARGS, SLOTS, rounds)


# --perf_compare_codec: st22p sessions, a codec plugin's


def _st22p_rx_dump(ts, succ, sessions=2, drops=()):
    """A dump of an st22p RX instance: its session lines read 0 fps, as st22 RX counts
    no frames, and its pipeline gives the frames of each since its last line, *succ*;
    *drops* holds (counter, value) lines of session 0's pipeline."""
    t = int(ts)
    lines = [f"{t} L {pc.DUMP_START}"]
    for s in range(sessions):
        name = f"app_rx_st22p_{s}"
        lines += [
            f"{t} L RX_VIDEO_SESSION(1,{s}:{name}): fps 0.000000 frames 0 pkts 364440",
            f"{t} L RX_ST22P({name}), p(1:ready) d(2:decoding) c(0:free)",
            f"{t} L RX_ST22P({name}), frame get try {succ + 3} succ {succ}, put {succ}",
            # the RX pipeline's decoder line, under a TX prefix (st22_pipeline_rx.c)
            f"{t} L TX_ST22P({name}), decoder get try {succ} succ {succ}, put {succ}",
        ]
    lines += [f"{t} L RX_ST22P(app_rx_st22p_0), {n} {v}" for n, v in drops]
    return lines + [f"{t} L {pc.DUMP_END}"]


def test_parse_dumps_rates_an_st22p_rx_session_by_the_frames_its_pipeline_gave():
    drops = [("decode fail", 2), ("busy drop frame", 3)]
    records = [
        *_st22p_rx_dump(10 * S, 599),
        *_st22p_rx_dump(20 * S, 599),
        f"{30 * S} L {pc.DUMP_START}",  # a dump that skipped its items (mt_stat.c)
        f"{30 * S} L {pc.DUMP_END}",
        *_st22p_rx_dump(40 * S, 1198, drops=drops),
    ]
    first, second, skipped, fourth = pc.parse_dumps(records)
    key = ("RX", "app_rx_st22p_0", pc.ST22P)
    # no rate before a session's second line, and none from its session line's 0
    assert first.fps == {} and skipped.fps == {}
    assert second.fps == {
        key: pytest.approx(59.9),
        ("RX", "app_rx_st22p_1", pc.ST22P): pytest.approx(59.9),
    }
    assert fourth.fps[key] == pytest.approx(59.9)  # its frames since 20 s
    assert fourth.counts == {"decode fail": 2, "busy drop frame": 3}
    assert pc.is_steady(second, "RX", 2, FPS) and not pc.is_steady(first, "RX", 2, 0)
    # a TX st22 session counts the frames it sends, its pipeline's are no RX rate
    tx = [
        line
        for t in (1, 2)
        for line in (
            f"{t * 10 * S} L {pc.DUMP_START}",
            f"{t * 10 * S} L TX_VIDEO_SESSION(1,0:app_tx_st22p_0): fps 59.94 frames 599",
            f"{t * 10 * S} L TX_ST22P(app_tx_st22p_0), encode fail {t}",
            f"{t * 10 * S} L TX_ST22P(app_tx_st22p_0), frame get try 600 succ 599, put 599",
            f"{t * 10 * S} L {pc.DUMP_END}",
        )
    ]
    for t, d in enumerate(pc.parse_dumps(tx), 1):
        assert d.fps == {("TX", "1", "0"): 59.94} and d.counts == {"encode fail": t}


def test_the_threads_of_an_st22p_session_are_application_threads():
    for name in ("tx_st20p_0", "rx_st20p_12", "tx_st22p_3", "rx_st22p_7"):
        assert pc.APP_THREAD.fullmatch(name), name
    # the codec's threads have no name of their own: other
    assert not pc.APP_THREAD.fullmatch("RxTxApp")


def test_metrics_of_an_st22p_instance_count_its_codec_drops():
    dumps = _windows()
    dumps[0][1].counts.update({"decode fail": 2, "encode fail": 5})
    assert (
        not {"decode fail", "busy drop frame", "encode fail"} & _metrics(dumps).keys()
    )
    m = pc.metrics("RX", dumps, PROC0, PROC1, 100, None, None, None, True)
    assert (m["decode fail"], m["busy drop frame"]) == (2, 0)
    assert "encode fail" not in m
    tx = _windows(tx=True)
    tx[0][1].counts.update({"encode fail": 5})
    m = pc.metrics("TX", tx, PROC0, PROC1, 100, None, None, None, True)
    assert m["encode fail"] == 5 and "decode fail" not in m
    assert {"decode fail", "busy drop frame", "encode fail"} <= set(pc.LOSS_ROWS)


def test_wait_steady_moves_an_st22p_rx_s_app_threads_once_every_session_got_frames(
    monkeypatch,
):
    # its decoder starts its threads at its first frame
    idle = _st22p_rx_dump(S, 0) + _st22p_rx_dump(2 * S, 0)
    flowing = idle + _st22p_rx_dump(3 * S, 599)
    for st22p, moved in ((True, pc.POLL_S), (False, 0)):
        pinned, records = [], [idle, idle]
        insts = _steady_insts(monkeypatch, records, pinned=pinned, st22p=st22p)
        sleep = pc.time.sleep

        def frames(s, records=records, sleep=sleep):  # a poll later
            records[:] = [flowing, flowing]
            sleep(s)

        monkeypatch.setattr(pc.time, "sleep", frames)
        assert pc._wait_steady(insts, 2, FPS) == ([], [])
        assert pinned == [("rx0", moved), ("rx1", moved)], st22p


def test_start_gives_an_st22p_instance_no_realtime_policy(monkeypatch):
    monkeypatch.setattr(pc, "write_file", lambda *a: None)
    props = []
    monkeypatch.setattr(
        pc, "_start_unit", lambda h, n, c, p, *a, **k: props.extend(p) or 7
    )
    inst = pc.Instance(None, "rx0", "rx", "A", pc.Slot(0, 0, [4, 5], True), "/t", "/r")
    inst.st22p = True
    inst.start("/mtl", "env KAHAWAI_CFG_PATH=/c RxTxApp --x")
    assert props[-1] == "RestrictRealtime=yes"


def test_run_round_takes_an_st22p_instance_whose_sessions_the_plugin_coded(fake):
    fake.session = "st22p"
    results, problems, notes = fake.round()
    assert problems == [] and notes == []
    assert results["A"]["rx"]["decode fail"] == 0
    assert results["B"]["tx"]["encode fail"] == 0
    greps = [c for c in fake.scripts if c.startswith("grep -cs 'get one session")]
    assert len(greps) == 4
    # what the command counts in a log as st_plugin.c writes it: the SVT plugin's
    # encoder and decoder sessions, not the sample plugin's
    log = fake.tmp_path / "log"
    log.write_text(
        "MTL: st22_get_encoder_session(0), get one session at 0 on dev "
        "st22_svt_jpeg_xs_plugin_encoder, max codestream size 777600\n"
        "MTL: st22_get_decoder_session(1), get one session at 1 on dev "
        "st22_svt_jpeg_xs_plugin_decoder\n"
        "MTL: st22_get_encoder_session(2), get one session at 0 on dev "
        "st22_encoder_sample, max codestream size 777600\n"
    )
    assert _bash(None, f"{greps[0].rsplit(' ', 1)[0]} {log}", check=False) == "2\n"
    # one session on another plugin, the sample's, or a log without the lines
    fake.coded = {"A_rx0": 1, "B_tx1": 0}
    results, problems, _ = fake.round()
    assert problems == [
        f"mtl-perf-rx0 (main): 1 of 2 sessions on {pc.CODEC_DEV}*",
        f"mtl-perf-tx1 (dev): 0 of 2 sessions on {pc.CODEC_DEV}*",
    ]
    assert "rx" not in results["A"] and "tx" not in results["B"]
    # st20p: no plugin to look for
    fake.session, fake.scripts[:] = "st20p", []
    assert fake.round()[1] == []
    assert not [c for c in fake.scripts if "get one session" in c]
