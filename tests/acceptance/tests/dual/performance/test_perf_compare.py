# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Two MTL builds or configurations side by side, each an isolated TX and RX instance.

The design, the isolation and the metrics are described in doc/perf_compare.md.
"""

from __future__ import annotations

import json
import logging
import os
import re
import shlex

import pytest
from common.host_setup import ensure_pf_up
from common.nicctl import Nicctl
from conftest import get_host_mtl_path, is_host_sut
from mtl_engine import ip_pools
from mtl_engine import perf_compare as pc
from mtl_engine.const import LOG_FOLDER, RXTXAPP_PATH
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)

FPS = {25: 25.0, 29: 30000 / 1001, 50: 50.0, 59: 60000 / 1001}
# --perf_compare_single_core: the roles whose instances run on one scheduler on each
# NUMA node of their slot, with the largest quota RxTxApp takes, perf-pytest's too
SINGLE_CORE = {"none": (), "RX": ("rx",), "TX": ("tx",), "both": ("rx", "tx")}
SINGLE_CORE_QUOTA = 99
# The most video sessions of one direction an MTL scheduler takes.
SCH_MAX_SESSIONS = 60
# The extra arguments go into a root shell script and into the report as they are.
ARGS = re.compile(r"[A-Za-z0-9_ ./:=,+-]*")
# RxTxApp takes a long option with one dash too, and by a prefix no other one has:
# the shortest such prefix of each option the case sets itself.
SHORTEST = {
    "tasklet_time": "tasklet_ti",
    "dma_dev": "dm",
    "lcores": "lc",
    "sch_session_quota": "sch_s",
    "sch_data_quota": "sch_d",
    "dedicated_sys_lcore": "de",
    "rx_separate_lcore": "rx_se",
}
# the options that would give a single core instance more schedulers
SCHEDULING = (
    "sch_session_quota",
    "sch_data_quota",
    "dedicated_sys_lcore",
    "rx_separate_lcore",
)
# --perf_compare_dma: the builds whose RX instances copy with a DSA
DMA = {"none": "", "A": "A", "B": "B", "both": "AB"}
# --perf_compare_nics, doc/perf_compare.md
NICS = {
    "single": "NIC 0: a slot on each of its ports",
    "span": "both NICs: each slot has a port of each, as many sessions on each",
    "split": "both NICs: a slot on port 0 of each, so each build has a socket of its own",
    "pairs": "both NICs: a pair of slots on each, its rows apart",
    "pooled": "both NICs: a pair of slots on each, each NIC of a round a sample",
}


def _given(options: list[str], name: str) -> bool:
    """Whether *options* hold RxTxApp's --*name*, by any prefix it takes."""
    return any(
        (m := re.fullmatch(r"--?(\w+)", t))
        and m[1].startswith(SHORTEST[name])
        and name.startswith(m[1])
        for t in options
    )


def _options(request) -> dict:
    get = request.config.getoption
    trees = get("--perf_compare_slots").split(",")
    labels = get("--perf_compare_labels").split(",")
    if len(trees) != 2 or len(labels) != 2:
        pytest.fail(
            "--perf_compare_slots and --perf_compare_labels take two values each"
        )
    opts = {
        "trees": dict(zip("AB", trees)),
        "labels": dict(zip("AB", labels)),
        "args": {"A": get("--perf_compare_args_a"), "B": get("--perf_compare_args_b")},
        "sessions": get("--perf_compare_sessions"),
        "rounds": get("--perf_compare_rounds"),
        "cores": get("--perf_compare_cores"),
        "single_core": SINGLE_CORE.get(get("--perf_compare_single_core")),
        "dma": DMA.get(get("--perf_compare_dma")),
        "nics": get("--perf_compare_nics"),
    }
    if not all(ARGS.fullmatch(a) for a in opts["args"].values()):
        pytest.fail("--perf_compare_args_a/b take letters, digits, spaces and _./:=,+-")
    options = [t.split("=")[0] for a in opts["args"].values() for t in a.split()]
    if _given(options, "tasklet_time"):
        pytest.fail(
            "--tasklet_time would time every window; the timed window is the case's"
        )
    if _given(options, "dma_dev"):
        pytest.fail(
            "--dma_dev would give one DSA to both slots and hosts; --perf_compare_dma "
            "gives each RX instance its own"
        )
    if _given(options, "lcores"):
        pytest.fail(
            "--lcores would replace the slot's CPUs; --perf_compare_cores sets how "
            "many each slot has"
        )
    if opts["dma"] is None:
        pytest.fail(f"--perf_compare_dma is one of {', '.join(DMA)}")
    if opts["single_core"] is None:
        pytest.fail(f"--perf_compare_single_core is one of {', '.join(SINGLE_CORE)}")
    if opts["nics"] not in NICS:
        pytest.fail(f"--perf_compare_nics is one of {', '.join(NICS)}")
    if opts["sessions"] < 1 or opts["rounds"] < 1:
        pytest.fail("--perf_compare_sessions and --perf_compare_rounds start at 1")
    if opts["single_core"] and opts["sessions"] > SCH_MAX_SESSIONS:
        pytest.fail(
            f"--perf_compare_single_core takes up to {SCH_MAX_SESSIONS} sessions a "
            "port, as many as an MTL scheduler"
        )
    given = [n for n in SCHEDULING if _given(options, n)]
    if opts["single_core"] and given:
        pytest.fail(f"--{given[0]} would give a single core instance more schedulers")
    floor = 2 if opts["single_core"] == SINGLE_CORE["both"] else 3
    if opts["cores"] < floor:
        pytest.fail(
            f"--perf_compare_cores starts at {floor}: one for the application threads "
            "and one for a scheduler, and one for MTL's system tasklets "
            "(dedicated_sys_lcore) unless both roles are single core"
        )
    return opts


def _subnet(ip: str, by: int) -> str:
    a, b, c, d = ip.split(".")
    if not 0 < int(c) + by < 256:
        raise ValueError(f"no subnet {by} above {ip}")
    return f"{a}.{b}.{int(c) + by}.{d}"


def _app_cmd(opts, media_config, media_path, fps, test_time):
    """Return app_cmd(side, slot, label) for pc.run_round: the RxTxApp of one instance.

    It returns the command and the RxTxApp configuration it runs with.
    """
    # Each port has a subnet of its own: port 1 the redundant pools', and NIC 1's
    # ports those 100 above NIC 0's.
    pools = [(ip_pools.tx[0], ip_pools.rx[0]), (ip_pools.tx_r[0], ip_pools.rx_r[0])]
    if opts["nics"] != "single":
        pools += [(_subnet(tx, 100), _subnet(rx, 100)) for tx, rx in pools]

    def app_cmd(side, slot, label):
        first, *more = side.slots[slot].ports
        tx_ip, rx_ip = pools[first]
        app = RxTxApp(
            RXTXAPP_PATH, config_file_path=f"tests/perf_compare_{side.role}{slot}.json"
        )
        extra = (
            {"input_file": media_path} if side.role == "tx" else {"rx_queues_cnt": 64}
        )
        if side.dma and label in opts["dma"]:  # the RX side's DSA of the slot
            extra["dma_dev"] = side.dma[slot]
        if side.role in opts["single_core"]:  # the system tasklets on it too
            extra["sch_session_quota"] = SINGLE_CORE_QUOTA
        else:
            extra["dedicated_sys_lcore"] = True
        app.create_command(
            session_type="st20p",
            direction=side.role,
            test_mode="unicast",
            transport_format=media_config["format"],
            pixel_format=media_config["file_format"],
            width=media_config["width"],
            height=media_config["height"],
            framerate=f"p{fps}",
            port=5004,
            payload_type=96,
            pacing="narrow",
            packing="GPM",
            replicas=opts["sessions"],
            nic_port=side.vfs[first],
            lcores=pc.format_cpus(side.slots[slot].cpus[1:]),  # see pc.Slot
            disable_migrate=True,
            test_time=test_time,
            source_ip=tx_ip,
            destination_ip=rx_ip,
            **extra,
        )
        for port in more:  # span: the slot's port on NIC 1
            pc.add_port(app.config, side.role, side.vfs[port], *pools[port])
        app.prepare_execution(build=side.mtl_path, host=side.host)
        return f"{app.command} {opts['args'][label]}".strip(), app.config

    return app_cmd


def _ports(host, nics: str) -> tuple[list[str], list[str]]:
    """The PFs and the VFs of *host* in port order: those of NIC 0, then those of NIC 1
    when *nics* uses both, whose VFs are made here as nic_port_list makes NIC 0's."""
    vfs = [(getattr(host, a, None) or [None])[0] for a in ("vfs", "vfs_r")]
    if None in vfs:
        pytest.fail(f"{host.name} needs a VF on each of two PFs")
    ports = host.network_interfaces[: 2 if nics == "single" else 4]
    pfs = [port.pci_address.lspci for port in ports]
    if nics == "single":
        return pfs, vfs
    if not pc.two_cards(pfs):
        pytest.fail(
            f"{host.name}: --perf_compare_nics {nics} takes two ports of each of two "
            f"NICs, the topology has {', '.join(pfs)}"
        )
    nicctl = Nicctl(get_host_mtl_path(host), host)
    for pf in pfs[2:]:  # create_vfs() keeps the VFs a PF has
        vfs.append(nicctl.create_vfs(pf, num_of_vfs=1)[0])
        ensure_pf_up(host, pf)
    return pfs, vfs


def _slots(host, vfs: list[str], nics: str, cores: int) -> list[pc.Slot]:
    """cpu_layout()'s slots of NIC 0, or of both NICs as *nics* uses them."""
    nic0 = pc.cpu_layout(host, vfs[0], cores)
    if nics == "single":
        return nic0
    nic1 = pc.cpu_layout(host, vfs[2], cores)
    if nic0[0].nodes == nic1[0].nodes:
        pytest.fail(f"{host.name}: both NICs are on NUMA node {nic0[0].nodes[0]}")
    return pc.nic_slots(nics, nic0, nic1)


def _platform(out_dir, host) -> dict:
    """The platform_config.json the session collected for *host*, {} without one."""
    try:
        with open(os.path.join(out_dir, f"{host.name}_platform_config.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _write_report(out_dir, opts, details, hostnames, sides, pfs, rounds) -> None:
    slots = [
        {
            "host": hostnames[s.role],
            "role": s.role,
            "slot": slot.idx,
            "cpus": pc.format_cpus(slot.cpus),
            "isolated": slot.isolated,
            "cat": bool(s.rdt) and opts["nics"] != "split",  # split: RDT only monitors
            "perf": bool(s.perf),
            **({"dsa": s.dma[slot.idx]} if s.dma else {}),
            **({"nic": slot.nic} if slot.nic is not None else {}),
            **(
                {"pfs": ", ".join(pfs[s.role][p] for p in slot.ports)}
                if opts["nics"] != "single"
                else {}
            ),
        }
        for s in sides
        for slot in s.slots
    ]
    platforms = [
        (f"{hostnames[s.role]} ({s.role.upper()})", _platform(out_dir, s.host))
        for s in sides
    ]
    nics, single = opts["nics"], opts["single_core"]
    report = pc.report_html(
        opts["labels"], opts["args"], details, slots, rounds, platforms, nics, single
    )
    summary = pc.summary_html(opts["labels"], opts["args"], slots, rounds, nics, single)
    os.makedirs(out_dir, exist_ok=True)
    for name, page in (("perf_compare", report), ("perf_compare_summary", summary)):
        with open(os.path.join(out_dir, f"{name}.html"), "w") as f:
            f.write(page)


@pytest.mark.dual
@pytest.mark.perf_compare
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["ParkJoy_1080p_24frames"],
        yuv_files_422rfc10["ParkJoy_4K_24frames"],
        yuv_files_422rfc10["Penguin_8K_24frames"],
    ],
    indirect=["media_file"],
    ids=["1080p", "2160p", "4320p"],
)
@pytest.mark.parametrize(
    "fps", [25, 29, 50, 59], ids=["25fps", "29fps", "50fps", "59fps"]
)
def test_perf_compare(hosts, test_time, nic_port_list, media_file, fps, request):
    """Run build A and build B at once, each a TX and an RX instance, and compare them."""
    opts = _options(request)
    rx_host = next((h for h in hosts.values() if is_host_sut(h)), None)
    tx_host = next((h for h in hosts.values() if h is not rx_host), None)
    if not rx_host or not tx_host:
        pytest.fail("perf-compare needs a SUT and a second host")
    media_config, media_path = media_file
    video = f"{media_config['height']}p{fps}"
    out_dir = os.path.join(os.environ.get("MTL_LOG_FOLDER", LOG_FOLDER), "latest")
    # span: an instance has the sessions of each of its two ports
    sessions = opts["sessions"] * (2 if opts["nics"] == "span" else 1)
    rate = pc.stream_bps(media_config["width"], media_config["height"], FPS[fps])
    traced_s = test_time // 2
    # the traced, the timed and the sessions window each take a dump period more at
    # both ends, and the last a period for the dump that closes it
    windows_s = test_time + 3 * (traced_s + 2 * pc.DUMP_PERIOD_S) + pc.DUMP_PERIOD_S
    app_time = pc.steady_timeout(sessions) + pc.WARMUP_S + windows_s + 100
    app_cmd = _app_cmd(opts, media_config, media_path, fps, app_time)
    details = [
        ("pytest", shlex.join(["pytest", *request.config.invocation_params.args])),
        (
            "Workload",
            f"{opts['sessions']} st20p sessions per build"
            f"{'' if opts['nics'] == 'single' else ' and port'}, RxTxApp, unicast",
        ),
        (
            "Video",
            f"{video}: {media_config['width']}x{media_config['height']} "
            f"{media_config['format']}, GPM, narrow pacing",
        ),
        *([("NICs", NICS[opts["nics"]])] if opts["nics"] != "single" else []),
        ("Rounds", f"{opts['rounds']}; the builds swap slots every round"),
        ("Warm-up", f"{pc.WARMUP_S} s after every session is steady"),
        ("Untraced window", f"{test_time} s: the primary rows"),
        (
            "Traced window",
            f"{traced_s} s: the tasklet and work rows, 1 loop in 64 timed",
        ),
        ("Timed window", f"{traced_s} s: the tasklet call times, every call timed"),
        (
            "Sessions window",
            f"{traced_s} s: the video session call times, every session timed",
        ),
    ]
    sides, rounds, pfs = [], [], {}
    try:
        for role, host in (("rx", rx_host), ("tx", tx_host)):
            pfs[role], vfs = _ports(host, opts["nics"])
            links = pc.nic_links(host, pfs[role])
            slots = _slots(host, vfs, opts["nics"], opts["cores"])
            used = sorted({p for slot in slots for p in slot.ports})  # split: 2 of 4
            used_pfs = [pfs[role][p] for p in used]
            used_links = [links[p] for p in used]
            if slow := pc.slow_ports(used_pfs, used_links, opts["sessions"] * rate):
                pytest.fail(
                    f"{host.name}: {opts['sessions']} x {video} on a port take "
                    f"{opts['sessions'] * rate / 1e9:.1f} Gb/s, and {', '.join(slow)} "
                    "(--perf_compare_sessions counts per port, with one NIC or two)"
                )
            if opts["nics"] == "pooled":
                if unlike := pc.unlike_nics(links, slots):
                    pytest.fail(f"{host.name}: pooled takes alike NICs, but {unlike}")
            if opts["nics"] == "split" and slots[0].isolated != slots[1].isolated:
                pytest.fail(
                    f"{host.name}: split takes the cores of both NICs' nodes isolated "
                    "alike, as each round puts a build in each slot"
                )
            sides.append(pc.Side(host, role, get_host_mtl_path(host), vfs, slots, []))
            sides[-1].rdt = pc.rdt_setup(host, len(slots), opts["nics"] != "split")
            sides[-1].perf = pc.perf_tool(host)
            if role == "rx" and opts["dma"]:  # MTL has no TX DMA
                sides[-1].dma = pc.dsa_layout(host, slots)
        hostnames = {s.role: pc.sh(s.host, "hostname").strip() for s in sides}
        for n in range(opts["rounds"]):
            order = pc.round_order(n, len(sides[0].slots))
            launches, failed = [], False
            try:
                results, problems, notes = pc.run_round(
                    sides,
                    opts["trees"],
                    order,
                    app_cmd,
                    os.path.join(out_dir, "perf_compare", f"round{n + 1}"),
                    sessions,
                    FPS[fps],
                    test_time,
                    traced_s,
                    launches,
                    pc.build_names(opts["labels"]),
                    opts["single_core"],
                )
            except Exception as e:  # keep the rounds so far for the report
                logger.exception(f"Round {n + 1} {order} failed")
                # the first line: the rest is the log's, not the table's
                problems = [f"failed: {(str(e) or repr(e)).splitlines()[0]}"]
                results, notes = {"A": {}, "B": {}}, []
                failed = True
            else:
                logger.info(f"Round {n + 1} {order}: {problems or 'valid'} {notes}")
            rounds.append(
                {
                    "order": order,
                    "metrics": results,
                    "problems": problems,
                    "notes": notes,
                    "launches": launches,
                }
            )
            # after each round: a job that times out keeps what it measured
            _write_report(out_dir, opts, details, hostnames, sides, pfs, rounds)
            if failed:
                break
    finally:
        for s in sides:
            pc.stop_units(s.host)
            pc.rdt_teardown(s.host)

    problems = [p for r in rounds for p in r["problems"]]
    if problems:
        pytest.fail("; ".join(problems))
