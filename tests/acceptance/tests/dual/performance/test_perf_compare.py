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
from conftest import get_host_mtl_path, is_host_sut
from mtl_engine import ip_pools
from mtl_engine import perf_compare as pc
from mtl_engine.const import LOG_FOLDER, RXTXAPP_PATH
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)

SLOT_CORES = 8
FPS = {25: 25.0, 29: 30000 / 1001, 50: 50.0, 59: 60000 / 1001}
# The extra arguments go into a root shell script and into the report as they are.
ARGS = re.compile(r"[A-Za-z0-9_ ./:=,+-]*")


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
    }
    if not all(ARGS.fullmatch(a) for a in opts["args"].values()):
        pytest.fail("--perf_compare_args_a/b take letters, digits, spaces and _./:=,+-")
    # RxTxApp takes long options by a prefix and with one dash too
    tasklet_time = re.compile(r"--?tasklet_ti(me?)?")
    if any(tasklet_time.fullmatch(t) for a in opts["args"].values() for t in a.split()):
        pytest.fail(
            "--tasklet_time would time every window; the timed window is the case's"
        )
    if opts["sessions"] < 1 or opts["rounds"] < 1:
        pytest.fail("--perf_compare_sessions and --perf_compare_rounds start at 1")
    return opts


def _app_cmd(opts, media_config, media_path, fps, test_time):
    """Return app_cmd(side, slot, label) for pc.run_round: the RxTxApp of one instance.

    It returns the command and the RxTxApp configuration it runs with.
    """
    # Slot 1 takes the addresses of the redundant pools: another subnet on the other PF.
    pools = ((ip_pools.tx[0], ip_pools.rx[0]), (ip_pools.tx_r[0], ip_pools.rx_r[0]))

    def app_cmd(side, slot, label):
        tx_ip, rx_ip = pools[slot]
        app = RxTxApp(
            RXTXAPP_PATH, config_file_path=f"tests/perf_compare_{side.role}{slot}.json"
        )
        extra = (
            {"input_file": media_path} if side.role == "tx" else {"rx_queues_cnt": 64}
        )
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
            nic_port=side.vfs[slot],
            dedicated_sys_lcore=True,
            disable_migrate=True,
            test_time=test_time,
            source_ip=tx_ip,
            destination_ip=rx_ip,
            **extra,
        )
        app.prepare_execution(build=side.mtl_path, host=side.host)
        return f"{app.command} {opts['args'][label]}".strip(), app.config

    return app_cmd


def _platform(out_dir, host) -> dict:
    """The platform_config.json the session collected for *host*, {} without one."""
    try:
        with open(os.path.join(out_dir, f"{host.name}_platform_config.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _write_report(out_dir, opts, details, hostnames, sides, rounds) -> None:
    slots = [
        {
            "host": hostnames[s.role],
            "role": s.role,
            "slot": slot.idx,
            "cpus": pc.format_cpus(slot.cpus),
            "isolated": slot.isolated,
            "cat": bool(s.rdt),
            "perf": bool(s.perf),
        }
        for s in sides
        for slot in s.slots
    ]
    platforms = [
        (f"{hostnames[s.role]} ({s.role.upper()})", _platform(out_dir, s.host))
        for s in sides
    ]
    report = pc.report_html(
        opts["labels"], opts["args"], details, slots, rounds, platforms
    )
    summary = pc.summary_html(opts["labels"], opts["args"], slots, rounds)
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
    traced_s = test_time // 2
    # the traced, the timed and the sessions window each take a dump period more at
    # both ends, and the last a period for the dump that closes it
    windows_s = test_time + 3 * (traced_s + 2 * pc.DUMP_PERIOD_S) + pc.DUMP_PERIOD_S
    app_time = pc.steady_timeout(opts["sessions"]) + pc.WARMUP_S + windows_s + 100
    app_cmd = _app_cmd(opts, media_config, media_path, fps, app_time)
    details = [
        ("pytest", shlex.join(["pytest", *request.config.invocation_params.args])),
        ("Workload", f"{opts['sessions']} st20p sessions per build, RxTxApp, unicast"),
        (
            "Video",
            f"{video}: {media_config['width']}x{media_config['height']} "
            f"{media_config['format']}, GPM, narrow pacing",
        ),
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
    sides, rounds = [], []
    try:
        for role, host in (("rx", rx_host), ("tx", tx_host)):
            vfs = [(getattr(host, a, None) or [None])[0] for a in ("vfs", "vfs_r")]
            if None in vfs:
                pytest.fail(f"{host.name} needs a VF on each of two PFs")
            slots = pc.cpu_layout(host, vfs[0], SLOT_CORES)
            sides.append(pc.Side(host, role, get_host_mtl_path(host), vfs, slots, []))
            sides[-1].rdt = pc.rdt_setup(host)
            sides[-1].perf = pc.perf_tool(host)
        hostnames = {s.role: pc.sh(s.host, "hostname").strip() for s in sides}
        for n in range(opts["rounds"]):
            order = ("A", "B") if n % 2 == 0 else ("B", "A")
            launches, failed = [], False
            try:
                results, problems, notes = pc.run_round(
                    sides,
                    opts["trees"],
                    order,
                    app_cmd,
                    os.path.join(out_dir, "perf_compare", f"round{n + 1}"),
                    opts["sessions"],
                    FPS[fps],
                    test_time,
                    traced_s,
                    launches,
                    pc.build_names(opts["labels"]),
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
            _write_report(out_dir, opts, details, hostnames, sides, rounds)
            if failed:
                break
    finally:
        for s in sides:
            pc.stop_units(s.host)
            pc.rdt_teardown(s.host)

    problems = [p for r in rounds for p in r["problems"]]
    if problems:
        pytest.fail("; ".join(problems))
