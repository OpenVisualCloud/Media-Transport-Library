# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Two MTL builds or configurations side by side, each an isolated TX and RX instance.

The design, the isolation and the metrics are described in doc/perf_compare.md.
"""

from __future__ import annotations

import json
import logging
import math
import os
import re

import pytest
from conftest import get_host_mtl_path, is_host_sut
from mtl_engine import ip_pools
from mtl_engine import perf_compare as pc
from mtl_engine.const import RXTXAPP_PATH
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)

SLOT_CORES = 8
WARMUP_S = 30
RUN_DIR = "/tmp/mtl-perf-compare"
FPS = {50: 50.0, 59: 60000 / 1001}
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
        "fail_pct": get("--perf_compare_fail_pct"),
    }
    if not all(ARGS.fullmatch(a) for a in opts["args"].values()):
        pytest.fail("--perf_compare_args_a/b take letters, digits, spaces and _./:=,+-")
    if opts["sessions"] < 1 or opts["rounds"] < 1:
        pytest.fail("--perf_compare_sessions and --perf_compare_rounds start at 1")
    if opts["fail_pct"] is not None and not math.isfinite(opts["fail_pct"]):
        pytest.fail("--perf_compare_fail_pct takes a finite number")
    if opts["fail_pct"] is not None and opts["rounds"] % 2:
        pytest.fail("--perf_compare_fail_pct needs an even number of rounds: the swap")
    return opts


def _app_cmd(opts, media_config, media_path, fps, test_time):
    """Return app_cmd(side, slot, label) for pc.run_round: the RxTxApp of one instance."""
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
            **extra,
        )
        if side.role == "tx":
            app.config["interfaces"][0]["ip"] = tx_ip
            app.config["tx_sessions"][0]["dip"][0] = rx_ip
        else:
            app.config["interfaces"][0]["ip"] = rx_ip
            app.config["rx_sessions"][0]["ip"][0] = tx_ip
        app.prepare_execution(build=side.mtl_path, host=side.host)
        return f"{app.command} {opts['args'][label]}".strip()

    return app_cmd


def _side(role, host) -> pc.Side:
    vfs = [(getattr(host, attr, None) or [None])[0] for attr in ("vfs", "vfs_r")]
    if None in vfs:
        pytest.skip(f"{host.name} needs a VF on each of two PFs")
    return pc.Side(
        host,
        role,
        get_host_mtl_path(host),
        vfs,
        pc.cpu_layout(host, vfs[0], SLOT_CORES),
        [],
    )


def _report(opts, video, sides, rounds) -> tuple[str, dict]:
    labels, args = opts["labels"], opts["args"]
    names = {s.role: pc.sh(s.host, "hostname").strip() for s in sides}
    valid = [r["metrics"] for r in rounds if not r["problems"]]
    timed = {
        k: "; ".join(sorted({m for r in valid for m in r[k].get("trace", {}).values()}))
        for k in ("A", "B")
    }
    lines = [
        f"# perf-compare: A `{labels['A']}` vs B `{labels['B']}`",
        "",
        f"{opts['sessions']} st20p {video} sessions per instance, RxTxApp, "
        f"{len(rounds)} rounds with the slots swapped every other round.",
        "",
        "| | A | B |",
        "| --- | --- | --- |",
        f"| Build | `{labels['A']}` | `{labels['B']}` |",
        f"| Extra args | `{args['A'] or '-'}` | `{args['B'] or '-'}` |",
        f"| Traced window timing | {timed['A'] or '-'} | {timed['B'] or '-'} |",
        "",
        "| Host | Role | Slot | CPUs | isolcpus | CAT |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for s in sides:
        for slot in s.slots:
            lines.append(
                f"| {names[s.role]} | {s.role.upper()} | {slot.idx} | "
                f"{pc.format_cpus(slot.cpus)} | {'yes' if slot.isolated else 'no'} | "
                f"{'yes' if s.rdt else 'no'} |"
            )
    lines += ["", "| Round | Slot 0 | Slot 1 | Result |", "| --- | --- | --- | --- |"]
    for n, r in enumerate(rounds, 1):
        result = "; ".join(r["problems"] + r["notes"]) or "valid"
        lines.append(f"| {n} | {r['order'][0]} | {r['order'][1]} | {result} |")
    tables = {s.role: pc.compare(valid, s.role) for s in sides}
    for s in sides:
        lines += [
            "",
            f"## {s.role.upper()} ({names[s.role]})",
            "",
            *pc.render_rows(tables[s.role]),
        ]
    data = {
        "labels": labels,
        "args": args,
        "video": video,
        "sessions": opts["sessions"],
        "rounds": rounds,
        "rows": {role: [r.__dict__ for r in rows] for role, rows in tables.items()},
    }
    return "\n".join(lines) + "\n", data


@pytest.mark.dual
@pytest.mark.perf_compare
@pytest.mark.skipif(
    "not config.getoption('--perf_compare_slots')",
    reason="perf-compare runs only with --perf_compare_slots (doc/perf_compare.md)",
)
@pytest.mark.parametrize(
    "media_file",
    [
        yuv_files_422rfc10["ParkJoy_1080p_24frames"],
        yuv_files_422rfc10["ParkJoy_4K_24frames"],
    ],
    indirect=["media_file"],
    ids=["1080p", "2160p"],
)
@pytest.mark.parametrize("fps", [50, 59], ids=["50fps", "59fps"])
def test_perf_compare(hosts, test_time, nic_port_list, media_file, fps, request):
    """Run build A and build B at once, each a TX and an RX instance, and compare them."""
    opts = _options(request)
    rx_host = next((h for h in hosts.values() if is_host_sut(h)), None)
    tx_host = next((h for h in hosts.values() if h is not rx_host), None)
    if not rx_host or not tx_host:
        pytest.skip("perf-compare needs a SUT and a second host")
    media_config, media_path = media_file
    video = f"{media_config['height']}p{fps}"
    out_dir = os.path.join(os.environ.get("MTL_LOG_FOLDER", "logs"), "latest")
    traced_s = test_time // 2
    app_time = (
        pc.steady_timeout(opts["sessions"]) + WARMUP_S + test_time + traced_s + 120
    )
    app_cmd = _app_cmd(opts, media_config, media_path, fps, app_time)
    sides, rounds = [], []
    try:
        for role, host in (("rx", rx_host), ("tx", tx_host)):
            sides.append(_side(role, host))
            sides[-1].rdt = pc.rdt_setup(host)
        for n in range(opts["rounds"]):
            order = ("A", "B") if n % 2 == 0 else ("B", "A")
            results, problems, notes = pc.run_round(
                sides,
                opts["trees"],
                order,
                app_cmd,
                RUN_DIR,
                os.path.join(out_dir, "perf_compare", f"round{n + 1}"),
                opts["sessions"],
                FPS[fps],
                WARMUP_S,
                test_time,
                traced_s,
            )
            logger.info(f"Round {n + 1} {order}: {problems or 'valid'} {notes}")
            rounds.append(
                {
                    "order": order,
                    "metrics": results,
                    "problems": problems,
                    "notes": notes,
                }
            )
    finally:
        for s in sides:
            pc.stop_units(s.host)
            pc.rdt_teardown(s.host)

    report, data = _report(opts, video, sides, rounds)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "perf_compare.md"), "w") as f:
        f.write(report)
    with open(os.path.join(out_dir, "perf_compare.json"), "w") as f:
        json.dump(data, f, indent=2)
    logger.info("\n" + report)

    problems = [p for r in rounds for p in r["problems"]]
    if problems:
        pytest.fail("; ".join(problems))
    if opts["fail_pct"] is not None:
        worse = [
            f"{s.role.upper()} {r.metric} {r.diff:+.1f}%"
            for s in sides
            for r in pc.compare([r["metrics"] for r in rounds], s.role)
            if r.regression is not None and r.regression > opts["fail_pct"]
        ]
        if worse:
            pytest.fail(
                f"B is worse than A by more than {opts['fail_pct']}%: {', '.join(worse)}"
            )
