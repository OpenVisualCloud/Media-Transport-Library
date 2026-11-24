# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""VF dual-host performance tests — session capacity sweep via binary search."""

from __future__ import annotations

import json
import logging
import math
import re
import shlex
import time
import traceback
from fractions import Fraction

import pytest
from common.nicctl import ensure_vfio_bound, reset_vfio_bindings
from conftest import get_host_mtl_path, is_host_sut
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine import ip_pools
from mtl_engine.const import RXTXAPP_PATH
from mtl_engine.dma import setup_host_dma_all
from mtl_engine.execute import kill_stale_processes, read_remote_log, run
from mtl_engine.integrity import calculate_yuv_frame_size
from mtl_engine.media_files import pformat_to_exact_fps, yuv_files_422rfc10
from mtl_engine.performance_monitoring import (
    companion_window,
    data_cores,
    display_session_results,
    format_cpus,
    log_data_cores,
    monitor_dev_rate,
    monitor_rx_fps,
    monitor_rx_frames_simple,
    monitor_rx_throughput,
    monitor_tx_fps,
    monitor_tx_frames,
    monitor_tx_throughput,
    parse_cpus,
    sent_at_rate,
    tx_vrx,
)
from mtl_engine.rxtxapp import RxTxApp, config_file

logger = logging.getLogger(__name__)

MAX_DROP_PCT = 0.10  # Trimmed mean: drop worst 10% of FPS samples per session

# An RX failure the network explains alone (see _run_iteration) says nothing
# about the SUT: on a shared switch, other runners' traffic fills the SUT's port
# (doc/ci_runner_setup.md).  It is re-run, and a case whose every attempt the
# network decided fails as such instead of reporting a count.
NETWORK_LOSS_ATTEMPTS = 3
NETWORK_LOSS_WAIT = 60  # Seconds between attempts
NETWORK_LOSS_MARKER = "lost between the hosts"

# What MTL logs when the SUT itself dropped: the NIC (DEV status, printed in
# any stat period with a non-zero error counter), the frame buffer pool, and the
# pipeline.  Not its OFO-window drops: with DMA, runs that pass log them too.
SUT_DROP_MARKERS = (
    "Status: rx_hw_dropped_packets",
    "framebuff pool empty",
    "busy drop frame",
)
# MTL's count of the packets its incomplete frames missed, when not zero
# ("missed" up to v26.01).
MISSED_PKTS = re.compile(r"(?:unrecovered|missed): [1-9]")

# Exit codes that mean the app died rather than finished.
CRASH_EXIT_CODES = (134, 137, 139, 244, 251)  # SIGABRT, SIGKILL, SIGSEGV, DPDK, DPDK

# ── Scheduler session quotas ──
# Controls how many sessions the library places on each scheduler (lcore).
# Higher quota = fewer cores (denser packing), but risks overloading a core.
# The unit is one 1080p59 stream's bandwidth (args.c ST_ARG_SCH_SESSION_QUOTA),
# and a *redundant* session charges one unit per port -- st_rx_video_session.c
# does `quota_mbs *= ops->num_port` outside the user-quota guard -- so a
# redundant session costs 2.  The library silently ignores a quota >= 100.
#
# Single core: must exceed the whole run's charge, or the library opens a second
# scheduler and the result is no longer a single-core measurement.  At 60 a
# redundant sweep split in two above 30 sessions; 99 covers 49 of them, and
# _single_core_violation() fails the iteration if a split happens anyway.
SCH_SESSION_QUOTA_SINGLE_CORE = 99
# The highest quota RxTxApp takes (args.c).
SCH_SESSION_QUOTA_MAX = 99
# Above this, an RX session without DMA polls a packet lcore besides its scheduler
# (ST_RX_VIDEO_PKT_LCORE_MBPS, lib/src/st2110/st_header.h).
RX_PKT_LCORE_MBS = 40_000

# Phase 1 binary search (multi-core) must find the session ceiling of the *NIC*,
# so it packs thinly enough that no core is the limit -- 12 units, the library's
# own ST_QUOTA_TX1080P_PER_SCH.  Leaving this to the per-mode quota below let
# packing density cap the sweep: at quota 18 the +DMA sweep put 36 sessions on
# 2 schedulers, failed, and settled at 33, while no-DMA spread the same 36 over
# 3 schedulers at quota 16 and passed.  Phase 2 then searches the fewest cores
# that still carry that count.  CLI --sch_quota overrides both phases.
SCH_SESSION_QUOTA_PHASE1_MC = 12

# A multi-core fixed run (--num_sessions) packs at these per-mode quotas.
# Multi-core no-DMA: RX ~21/core max, TX ~28/core max (TX has no memcpy).
SCH_SESSION_QUOTA_MULTI_CORE = 16  # RX no-DMA (~76% of SC max)
SCH_SESSION_QUOTA_MULTI_CORE_TX = 24  # TX no-DMA

# Redundant mode creates 2 internal sessions per logical session.
SCH_SESSION_QUOTA_MULTI_CORE_TX_REDUNDANT = 36  # TX redundant
SCH_SESSION_QUOTA_MULTI_CORE_RX_REDUNDANT = 21  # RX redundant

# DMA offloads memcpy but scheduler overhead remains significant.
# With quota=32 and 36 sessions, one scheduler hit 121% CPU busy and
# dropped frames mid-run.  quota=18 gives 2 balanced schedulers at ~68%
# CPU each (36 sessions), leaving enough headroom for transients.
SCH_SESSION_QUOTA_DMA = 18  # RX DMA
SCH_SESSION_QUOTA_DMA_RX_REDUNDANT = 18  # RX DMA redundant


MAX_SESSIONS = {
    ("tx", False): 64,
    ("tx", True): 64,
    ("rx", False): 64,
    ("rx", True): 64,
}

# RxTxApp gives every TX session its own hugepage copy of the source file, and
# falls back to a plain mmap() of it -- a different memory path -- for whichever
# sessions no longer fit.  It says so once, via warn(), and runs anyway, so a
# sweep on a full-length asset silently measures two populations at once.  The
# perf cases therefore use 24-frame prefixes of the assets the rest of the suite
# uses, named "<asset>_24frames.yuv".
#
# 24 is one number for all three resolutions, not a coincidence: at the
# bandwidth ceiling the session count is inversely proportional to frame size,
# so worst-case demand is frames * line_rate / (8 * lowest_fps_in_the_sweep) =
# frames * 500 MB at 100 Gbps, independent of resolution.  Against the ~15 GiB a
# runner has left for sources after MTL's own arena, that leaves every
# resolution's memory ceiling above its capacity ceiling -- 132 sessions against
# the 64 MAX_SESSIONS allows at 1080p, 33 against 24 at 2160p, 8 against 6 at
# 4320p -- which is what makes a *passing* iteration wholly hugepage-backed.
# Probes above the ceiling can still exhaust the pool, so the fallback is also
# checked for directly, in _has_hugepage_source_fallback().  Revisit if the line
# rate rises above 100 Gbps, 25 fps stops being the cheapest case, or hugepages
# shrink.
PERF_SOURCE_FRAMES = 24

# What RxTxApp prints, once, when a session's source no longer fits in
# hugepages and it reads through mmap() instead (tx_st20p_app.c).
HUGEPAGE_FALLBACK_MARKER = "source malloc on hugepage fail"

CRASH_RECOVERY_WAIT = 30  # Seconds to wait after VF reset for link recovery


def _get_tx_rx_hosts(hosts: dict, direction: str):
    """Return (tx_host, rx_host) with SUT running the measured side."""
    host_list = list(hosts.values())
    if len(host_list) < 2:
        pytest.skip("Dual-host test requires at least 2 hosts")

    sut = next((h for h in host_list if is_host_sut(h)), None)
    client = next((h for h in host_list if not is_host_sut(h)), None)
    if not (sut and client):
        sut, client = host_list[0], host_list[1]

    if direction == "tx":
        tx_host, rx_host = sut, client
    else:
        tx_host, rx_host = client, sut

    logger.info(
        f"Host assignment: SUT={sut.name} → measured {direction.upper()} | "
        f"TX={tx_host.name}, RX={rx_host.name}"
    )
    return tx_host, rx_host


def _check_perf_source_size(tx_host, media_file_path: str, media_config: dict) -> None:
    """Fail unless the staged source is PERF_SOURCE_FRAMES frames long.

    Staging a full-length asset by mistake does not fail the run -- it puts most
    sessions on RxTxApp's mmap fallback and publishes a number anyway, so the
    size is worth checking rather than assuming.  The name is checked too: it is
    what ties PERF_SOURCE_FRAMES to the assets media_files.py asks for, so
    raising the constant without provisioning new prefixes cannot pass quietly.
    """
    suffix = f"_{PERF_SOURCE_FRAMES}frames.yuv"
    if not media_file_path.endswith(suffix):
        pytest.fail(
            f"{media_file_path} is not a {PERF_SOURCE_FRAMES}-frame prefix; the "
            f"perf cases need an asset named *{suffix}"
        )

    frame_bytes = calculate_yuv_frame_size(
        media_config["width"], media_config["height"], media_config["file_format"]
    )
    result = tx_host.connection.execute_command(
        f"stat -c %s {media_file_path}", expected_return_codes=None
    )
    staged = (result.stdout or "").strip()
    if result.return_code != 0 or not staged.isdigit():
        pytest.fail(
            f"cannot size {media_file_path} on {tx_host.name}: stat exited "
            f"{result.return_code} saying {staged!r}"
        )

    staged = int(staged)
    if staged != PERF_SOURCE_FRAMES * frame_bytes:
        pytest.fail(
            f"{media_file_path} holds {staged / frame_bytes:.1f} frames "
            f"({staged} B); the sweep needs {PERF_SOURCE_FRAMES} "
            f"({PERF_SOURCE_FRAMES * frame_bytes} B)"
        )


def _has_hugepage_source_fallback(tx_lines) -> bool:
    """True if any TX session read its source through the mmap() fallback.

    PERF_SOURCE_FRAMES keeps the file small enough that this should not happen,
    but the pool is shared with MTL's own arena, which grows with the session
    count, and nothing in CI provisions the pool for this leg.  The app's own
    report is the one signal that covers every cause.
    """
    return any(HUGEPAGE_FALLBACK_MARKER in line for line in tx_lines)


def _wire_balance(tx_host, tx_vfs, rx_host, rx_vfs) -> list[int] | None:
    """Return, per leg, the packets the TX port sent minus those the RX port got.

    Both are the MAC's own unicast counters on the PF behind each VF, and a
    clean path delivers every packet, so the change across an iteration is
    what the network lost on that leg.  None if a counter cannot be read (a NIC
    without ice's .nic statistics, or the host not answering).
    """
    counts = []
    for host, vfs, counter in (
        (tx_host, tx_vfs, "tx_unicast.nic"),
        (rx_host, rx_vfs, "rx_unicast.nic"),
    ):
        try:
            result = host.connection.execute_command(
                "; ".join(
                    f"ethtool -S $(ls /sys/bus/pci/devices/{vf}/physfn/net) | "
                    f"awk '$1 == \"{counter}:\" {{print $2}}'"
                    for vf in vfs
                ),
                shell=True,
                timeout=20,
                expected_return_codes=None,
            )
            values = (result.stdout or "").split()
        except Exception as e:
            logger.warning(f"{host.name}: cannot read {counter}: {e}")
            return None
        if len(values) != len(vfs) or not all(v.isdigit() for v in values):
            logger.warning(f"{host.name}: no {counter} for {vfs}: {values}")
            return None
        counts.append([int(v) for v in values])

    sent, received = counts
    return [s - r for s, r in zip(sent, received)]


def _apply_side_kwargs(
    kwargs,
    side,
    media_path,
    tx_vf,
    rx_vf,
    tx_vf_r,
    rx_vf_r,
    redundant,
    redundant_kwargs,
):
    """Set TX or RX-specific kwargs (NIC port, file, redundancy)."""
    if side == "tx":
        kwargs["input_file"] = media_path
        vf, vf_r = tx_vf, tx_vf_r
    else:
        vf, vf_r = rx_vf, rx_vf_r
    if redundant and vf_r:
        kwargs.update(nic_port=vf, nic_port_r=vf_r, **redundant_kwargs)
    else:
        kwargs["nic_port"] = vf


def _cpu_layout(host, port: str) -> tuple[str | None, str | None, str]:
    """Return the CPUs of *port*'s NUMA node on *host* for (schedulers, application),
    and why the application's threads cannot be pinned, or "".

    The schedulers get the node's isolated CPUs, one per core, e.g. "4-20" of
    isolated "4-20,68-84" for a port on node 0, so that no two share a core.  The
    application's own threads, RxTxApp's frame threads and MTL's helper threads,
    get the node's other CPUs except the second threads of the scheduler cores:
    left to every CPU, the kernel also ran them on those, and on other nodes.
    They are not isolated, so the kernel balances the threads over them.  Either
    is None if there is none.  A host without isolated CPUs needs no pinning.
    """
    try:
        result = host.connection.execute_command(
            f"n=$(cat /sys/bus/pci/devices/{port}/numa_node); [ $n -ge 0 ] || n=0; "
            "cat /sys/devices/system/cpu/isolated /sys/devices/system/node/node$n/cpulist "
            "/sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list",
            shell=True,
        )
        isolated, node, *siblings = (result.stdout or "").split("\n")
        cores = [parse_cpus(line) for line in siblings if line.strip()]
        isolated, node = set(parse_cpus(isolated)), set(parse_cpus(node))
        schedulers = isolated & node & {min(core) for core in cores}
        shared = {cpu for core in cores if min(core) in schedulers for cpu in core}
        lcores = format_cpus(schedulers)
        # Without isolated schedulers EAL takes its lcores from the same mask.
        app_cpus = format_cpus(node - isolated - shared) if schedulers else ""
        why = ""
        if schedulers and not app_cpus:
            why = f"no CPU of {port}'s node is left for the application's threads"
            logger.warning(f"Host {host.name}: {why}, so they are not pinned")
        logger.info(
            f"Host {host.name}: CPUs of {port}'s node: schedulers {lcores or None}, "
            f"application {app_cpus or None}"
        )
        return lcores or None, app_cpus or None, why
    except Exception as e:
        logger.warning(f"Host {host.name}: could not read the CPU layout: {e}")
        return None, None, f"the CPU layout could not be read: {e}"


# Moves each thread of the app $2 of config file $1 that may run on more than one
# CPU onto CPUs $3, every 2 s, until the app exits or $4 s pass.  A taskset in front
# of the command would not hold: mtl_init() binds the main thread to its NUMA node
# (numa_bind()), and the threads it starts after inherit that.  EAL's threads, each
# on one CPU, stay.  It prints each thread it moves.  After each look it prints how
# many threads were on $3, how many on one CPU each and how many it could not move,
# then the busy and the total time of CPUs $3 so far (/proc/stat), which counts
# every program and interrupt on them.
PIN_SCRIPT = r"""
cfg=$1 exe=$2 cpus=$3 end=$((SECONDS + $4)) start=$((SECONDS + 60)) p='' want=' '
IFS=, read -ra ranges <<<"$cpus"
for r in "${ranges[@]}"; do
  for ((c = ${r%-*}; c <= ${r#*-}; c++)); do want+="$c "; done
done
while [ -z "$p" ] && ((SECONDS < start)); do
  for p in $(pgrep -f -- "$exe") ""; do
    [[ $p && $(readlink "/proc/$p/exe") == */"$exe" ]] &&
      grep -qzFx -- "$cfg" "/proc/$p/cmdline" 2>/dev/null && break
  done
  [ -n "$p" ] || sleep 0.2
done
while [ -n "$p" ] && kill -0 "$p" 2>/dev/null && ((SECONDS < end)); do
  on=0 one=0 off=0
  for t in /proc/"$p"/task/*; do
    m=
    while read -r k v; do [ "$k" != Cpus_allowed_list: ] || m=$v; done 2>/dev/null <"$t/status"
    case $m in
    "") ;;
    "$cpus") on=$((on + 1)) ;;
    *[,-]*)
      if taskset -pc "$cpus" "${t##*/}" >/dev/null 2>&1; then
        name='?' && read -r name 2>/dev/null <"$t/comm"
        echo "moved ${t##*/} $name from $m"
        on=$((on + 1))
      else
        off=$((off + 1))
      fi ;;
    *) one=$((one + 1)) ;;
    esac
  done
  busy=0 total=0
  while read -r name u n s i w q sq st _; do
    [[ $name == cpu[0-9]* && $want == *" ${name#cpu} "* ]] || continue
    busy=$((busy + u + n + s + q + sq + st)) total=$((total + u + n + s + i + w + q + sq + st))
  done </proc/stat
  echo "threads $on $one $off $busy $total"
  sleep 2
done
"""
# A share of the app CPUs past which they may hold the app back.
APP_CPU_BUSY = 0.8


def _pin_app_threads(host, app, cpus: str | None, seconds: int, role: str) -> str:
    """Keep the threads of RxTxApp *app* on *cpus*, for at most *seconds*, with
    PIN_SCRIPT, which runs on *cpus* too; return the log it writes, or ""."""
    if not cpus:
        return ""
    log = f"/tmp/mtl_perf_pin_{role}.log"
    args = (config_file(app.command), app.get_executable_name(), cpus, str(seconds))
    script = (
        f"rm -f {log}; taskset -c {cpus} bash -c {shlex.quote(PIN_SCRIPT)} pin "
        f"{' '.join(map(shlex.quote, args))} > {log} 2>&1 &"
    )
    host.connection.execute_command(
        f"sudo -n bash -c {shlex.quote(script)}", shell=True, expected_return_codes=None
    )
    return log


def _app_threads(host, log: str, cpus: str) -> tuple[bool, float]:
    """Log what the PIN_SCRIPT of *log* did on *host*.

    Returns whether it moved every thread that it had to, and the largest share of
    *cpus* that was busy between two looks.  A log it cannot read counts as not
    pinned.
    """
    try:
        out = host.connection.execute_command(
            f"sudo -n cat {log}", shell=True, expected_return_codes=None
        ).stdout
    except Exception as e:
        logger.warning(f"[APP_CPUS] {host.name}: could not read {log}: {e}")
        return False, 0.0
    lines = (out or "").splitlines()
    looks = [
        tuple(map(int, f[1:]))
        for f in map(str.split, lines)
        if f[:1] == ["threads"] and len(f) == 6 and all(v.isdigit() for v in f[1:])
    ]
    if not looks:
        logger.warning(f"[APP_CPUS] {host.name}: RxTxApp's threads not pinned: {out!r}")
        return False, 0.0
    on, one, _, _, _ = max(looks, key=lambda look: sum(look[:3]))
    off = max(look[2] for look in looks)  # a move that failed in any look
    moved = len({x.split()[1] for x in lines if x.startswith("moved ")})
    busy = max(
        (
            (b[3] - a[3]) / (b[4] - a[4])
            for a, b in zip(looks, looks[1:])
            if b[4] > a[4] and b[3] >= a[3]
        ),
        default=0.0,
    )
    logger.info(
        f"[APP_CPUS] {host.name}: {moved} threads of RxTxApp moved to CPUs {cpus}; "
        f"at the look with the most threads, {on} on those, {one} on one CPU each; "
        f"up to {off} not moved; those CPUs were up to {busy:.0%} busy"
    )
    return on > 0 and not off, busy


def _select_mc_quota(
    is_tx: bool,
    use_dma: bool,
    redundant: bool,
) -> int:
    """Return the multi-core scheduler quota of a fixed run in the given mode."""
    if use_dma and redundant:
        return SCH_SESSION_QUOTA_DMA_RX_REDUNDANT
    if use_dma:
        return SCH_SESSION_QUOTA_DMA
    if redundant and is_tx:
        return SCH_SESSION_QUOTA_MULTI_CORE_TX_REDUNDANT
    if redundant and not is_tx:
        return SCH_SESSION_QUOTA_MULTI_CORE_RX_REDUNDANT
    if is_tx:
        return SCH_SESSION_QUOTA_MULTI_CORE_TX
    return SCH_SESSION_QUOTA_MULTI_CORE


def _quota_mbs(width: int, height: int, fps: int, ports: int = 1) -> int:
    """What one 4:2:2 10-bit session charges a scheduler, in Mb/s, as MTL works it
    out: st20_get_bandwidth_bps() in whole Mb/s, times its ports.  Integer
    division where MTL's is, so that a quota of _quota_for_sessions() admits what
    MTL admits: 1080p59 charges 2589, a quota unit, and 2160p59 10357, not 4 units.
    """
    rate = Fraction(pformat_to_exact_fps(fps))
    traffic = width * height * 8 * 5 // 2 * rate.numerator // rate.denominator
    return int(traffic / (1080 / 1125)) // 10**6 * ports


def _quota_for_sessions(per_sch: int, media_config: dict, fps: int, ports: int) -> int:
    """The scheduler quota that admits *per_sch* sessions of *media_config* each."""
    charge = _quota_mbs(media_config["width"], media_config["height"], fps, ports)
    return -(-per_sch * charge // _quota_mbs(1920, 1080, 59))


def _is_crash_code(return_code: int) -> bool:
    """Return True if *return_code* means the app died rather than exited.

    A negative code is a signal reported by the process runner; the rest are
    the ones RxTxApp and DPDK are seen to abort with.
    """
    return return_code < 0 or return_code in CRASH_EXIT_CODES


def _is_crash(detail: str) -> bool:
    """Return True if the detail indicates a crash requiring VF FLR."""
    crash_markers = tuple(f"exit code {code}" for code in CRASH_EXIT_CODES) + (
        "exit code -",
        "companion",
        "exited early",
    )
    return any(marker in detail for marker in crash_markers)


def _single_core_violation(single_core: bool, cores_used: int) -> str | None:
    """Return a reason if a single-core iteration did not stay on one core.

    The sessions then had more CPU than the mode claims, so the count is not a
    single-core capacity result.  *cores_used* is 0 when unmeasured, which is a
    monitoring gap and not a violation.
    """
    if single_core and cores_used > 1:
        return f"single-core run spread over {cores_used} cores"

    return None


def _fewest_cores_result(
    iteration_results: list[dict], num_sessions: int
) -> dict | None:
    """Return the passing iteration at *num_sessions* that used the fewest cores.

    Multi-core sweeps run the winning session count more than once -- thinly
    packed in phase 1, then denser in phase 2 -- so report the cheapest placement
    that actually passed, not the last one attempted.  None if none did.
    """
    candidates = [
        it
        for it in iteration_results
        if it["num_sessions"] == num_sessions and it["passed"] and it["cores_used"]
    ]
    if not candidates:
        return None

    return min(candidates, key=lambda it: it["cores_used"])


def _log_iteration_table(
    iteration_results: list[dict],
    last_config: dict | None,
    *,
    show_quota: bool = False,
) -> None:
    """Log the per-iteration result table and (optionally) the RxTxApp config.

    Args:
        iteration_results: List of dicts with keys num_sessions, passed,
            cores_used, detail, and optionally quota.
        last_config: Latest RxTxApp JSON config dict, or None.
        show_quota: If True, append the quota value to each result line.
    """
    for it in iteration_results:
        ok = "✓" if it["passed"] else "✗"
        c = f"  cores={it['cores_used']}" if it.get("cores_used") else ""
        q = f"  q={it['quota']}" if show_quota and it.get("quota") is not None else ""
        logger.info(f"  {it['num_sessions']:>3} sessions  {ok}{c}{q}  {it['detail']}")
    if last_config:
        logger.info("RXTXAPP_CONFIG_BEGIN")
        for cfg_line in json.dumps(last_config, indent=2).splitlines():
            logger.info(cfg_line)
        logger.info("RXTXAPP_CONFIG_END")


def _run_iteration(
    direction: str,
    num_sessions: int,
    fps: int,
    media_config: dict,
    media_file_path: str,
    tx_host,
    rx_host,
    build_tx: str,
    build_rx: str,
    tx_vf: str,
    rx_vf: str,
    tx_vf_r: str | None,
    rx_vf_r: str | None,
    redundant: bool,
    single_core: bool,
    use_dma: bool,
    dma_device: str | None,
    dma_label: str,
    test_time: int,
    sch_session_quota: int | None = None,
    sut_app_args: str | None = None,
) -> tuple[bool, int, str, dict | None, dict]:
    """Run one iteration: start companion, run measured app, validate FPS.

    Args:
        sch_session_quota: If set, overrides the default SUT scheduler
            session quota for this iteration (used by core-minimization
            phase to test different quotas).
        sut_app_args: Extra RxTxApp arguments for the measured app.

    Returns:
        (passed, successful_count, detail_string, config_dict, cores), *cores*
        being data_cores() of the run, or {} if it did not run.
    """
    is_tx = direction == "tx"
    measured_host = tx_host if is_tx else rx_host
    companion_host = rx_host if is_tx else tx_host
    build_measured = build_tx if is_tx else build_rx
    build_companion = build_rx if is_tx else build_tx

    # ── Session parameters ──
    base_kwargs = {
        "session_type": "st20p",
        "test_mode": "unicast",
        "transport_format": media_config["format"],
        "pixel_format": media_config["file_format"],
        "width": media_config["width"],
        "height": media_config["height"],
        "framerate": f"p{fps}",
        "port": 5004,
        "payload_type": 96,
        "pacing": "narrow",
        "packing": "GPM",
        "replicas": num_sessions,
        "rx_queues_cnt": 64,
        "source_ip": ip_pools.tx[0],
        "destination_ip": ip_pools.rx[0],
    }

    # ── SUT scheduler settings (applied only to measured app) ──
    sut_extra_kwargs: dict = {}
    # Disable runtime session migration for all modes.  The admin thread
    # monitors scheduler CPU-busy% and migrates sessions to new schedulers,
    # which spins up extra cores beyond what initial placement requires.
    # With explicit quotas the initial round-robin distribution is already
    # optimal, so migration only wastes cores.
    sut_extra_kwargs["disable_migrate"] = True

    if single_core:
        sut_extra_kwargs["sch_session_quota"] = SCH_SESSION_QUOTA_SINGLE_CORE
    else:
        # Multi-core: dedicate a lcore to CNI/PTP system tasks so sch0
        # data sessions are not starved by admin-thread tasklets.
        sut_extra_kwargs["dedicated_sys_lcore"] = True
        # Override the library's conservative defaults with per-mode quotas
        # derived from single-core capacity tests.
        if sch_session_quota is not None:
            sut_extra_kwargs["sch_session_quota"] = sch_session_quota
        else:
            sut_extra_kwargs["sch_session_quota"] = _select_mc_quota(
                is_tx, use_dma, redundant
            )

    # Pin DPDK lcores to isolated CPUs (shields from OS timer ticks/RCU).
    lcores, app_cpus, unpinned = _cpu_layout(measured_host, tx_vf if is_tx else rx_vf)
    if lcores:
        sut_extra_kwargs["lcores"] = lcores

    # ── Companion scheduler settings ──
    # The companion just needs to keep up — it is not the measured side.
    companion_dir = "rx" if is_tx else "tx"
    companion_extra_kwargs: dict = {}
    companion_extra_kwargs["sch_session_quota"] = SCH_SESSION_QUOTA_MULTI_CORE
    companion_extra_kwargs["dedicated_sys_lcore"] = True
    # Same reason as the measured side: a migrating companion changes its own
    # core count mid-run, so the far end of the link is not the same load in
    # every iteration of the sweep.
    companion_extra_kwargs["disable_migrate"] = True

    companion_lcores, companion_app_cpus, _ = _cpu_layout(
        companion_host, rx_vf if is_tx else tx_vf
    )
    if companion_lcores:
        companion_extra_kwargs["lcores"] = companion_lcores

    redundant_kwargs = {}
    if redundant:
        redundant_kwargs = {
            "redundant": True,
            "source_ip": ip_pools.tx[0],
            "destination_ip": ip_pools.rx[0],
            "source_ip_r": ip_pools.tx_r[0],
            "destination_ip_r": ip_pools.rx_r[0],
        }

    # ── Companion app ──
    companion_app = RxTxApp(RXTXAPP_PATH)
    # E830 HW rate-limiter takes ~2 s per TX queue; with many sessions
    # the measured TX app runs much longer than test_time alone.
    rl_overhead = num_sessions * 3 if is_tx else 0
    companion_kwargs = {
        **base_kwargs,
        **companion_extra_kwargs,
        "direction": companion_dir,
        "test_time": test_time + 10 + rl_overhead,
    }
    # TX companion doesn't need many RX queues — free VF queue capacity.
    if companion_dir == "tx" and "rx_queues_cnt" in companion_kwargs:
        del companion_kwargs["rx_queues_cnt"]

    if is_tx:
        # Companion is RX
        _apply_side_kwargs(
            companion_kwargs,
            "rx",
            media_file_path,
            tx_vf,
            rx_vf,
            tx_vf_r,
            rx_vf_r,
            redundant,
            redundant_kwargs,
        )
    else:
        # Companion is TX
        _apply_side_kwargs(
            companion_kwargs,
            "tx",
            media_file_path,
            tx_vf,
            rx_vf,
            tx_vf_r,
            rx_vf_r,
            redundant,
            redundant_kwargs,
        )

    companion_app.create_command(**companion_kwargs)
    companion_app.prepare_execution(build=build_companion, host=companion_host)

    # For RX tests, set companion TX's destination MAC to the RX VF's MAC
    # so the companion TX does not need ARP resolution.
    if not is_tx:
        try:
            mac_result = rx_host.connection.execute_command(
                f"cat /sys/bus/pci/devices/{rx_vf}/net/*/address "
                f"2>/dev/null || echo ''",
                shell=True,
            )
            rx_mac = (mac_result.stdout or "").strip()
            if rx_mac:
                companion_app.command += f" --p_tx_dst_mac {rx_mac}"
                logger.info(f"Using RX VF MAC: {rx_mac} for companion TX")
        except Exception as e:
            logger.warning(f"Could not get RX VF MAC, relying on ARP: {e}")

    r_tag = "_redundant" if redundant else ""
    c_tag = "_sc" if single_core else "_mc"
    d_tag = "_dma" if use_dma else ""
    companion_log = (
        f"{build_companion}/tests/acceptance/logs/performance/"
        f"{direction}{r_tag}{c_tag}_fps{fps}_{num_sessions}s{d_tag}"
        f"_{companion_dir}_companion.log"
    )

    # A TX verdict is taken at the sender, so only RX depends on delivery.
    tx_vfs = [tx_vf] + ([tx_vf_r] if redundant else [])
    rx_vfs = [rx_vf] + ([rx_vf_r] if redundant else [])
    balance = None if is_tx else _wire_balance(tx_host, tx_vfs, rx_host, rx_vfs)

    companion_pin = _pin_app_threads(
        companion_host,
        companion_app,
        companion_app_cpus,
        companion_kwargs["test_time"] + 60,
        companion_dir,
    )
    companion_process = companion_host.connection.start_process(
        f"mkdir -p $(dirname {companion_log}) && "
        f"{companion_app.command} > {companion_log} 2>&1",
        cwd=build_companion,
        shell=True,
    )
    # Track on the app object so companion_app.stop_process() can manage it
    companion_app._process = companion_process
    companion_app._host = companion_host
    time.sleep(10)

    # Verify companion is still running before starting measured app.
    # The SSH process wrapper may not reliably expose a 'running' attribute,
    # so we also check the companion log for early-exit error signatures.
    companion_alive = True
    if hasattr(companion_process, "running") and not companion_process.running:
        companion_alive = False

    if companion_alive:
        # Double-check: look for fatal errors anywhere in the companion log.
        # MTL init can produce hundreds of lines before the actual error
        # (e.g. missing media file), so scanning only the first N lines
        # is not sufficient.
        log_snippet = read_remote_log(companion_host, companion_log)
        for line in log_snippet:
            if "open fail" in line or "open_source fail" in line:
                companion_alive = False
                break

    if not companion_alive:
        log_snippet = read_remote_log(companion_host, companion_log)
        tail = "\n".join(log_snippet[-20:]) if log_snippet else "(empty)"
        companion_app.stop_process()
        return False, 0, f"companion {companion_dir} exited early:\n{tail}", None, {}

    try:
        # ── Build measured app ──
        measured_app = RxTxApp(RXTXAPP_PATH)
        measured_kwargs = {
            **base_kwargs,
            **sut_extra_kwargs,  # SC or MC scheduler settings for SUT only
            "direction": direction,
            "test_time": test_time,
        }

        if is_tx:
            _apply_side_kwargs(
                measured_kwargs,
                "tx",
                media_file_path,
                tx_vf,
                rx_vf,
                tx_vf_r,
                rx_vf_r,
                redundant,
                redundant_kwargs,
            )
        else:
            _apply_side_kwargs(
                measured_kwargs,
                "rx",
                media_file_path,
                tx_vf,
                rx_vf,
                tx_vf_r,
                rx_vf_r,
                redundant,
                redundant_kwargs,
            )

        # DMA offloads memory copies on the RX side only.
        if use_dma and dma_device and not is_tx:
            measured_kwargs["dma_dev"] = dma_device

        measured_app.create_command(**measured_kwargs)
        measured_app.prepare_execution(build=build_measured, host=measured_host)
        if sut_app_args:
            measured_app.command += f" {sut_app_args}"

        # ── Run measured app ──
        measured_pin = _pin_app_threads(
            measured_host,
            measured_app,
            app_cpus,
            test_time + 60 + rl_overhead,
            direction,
        )
        try:
            result = run(
                measured_app.command,
                cwd=build_measured,
                timeout=test_time + 60 + rl_overhead,
                testcmd=True,
                host=measured_host,
            )
        except Exception as e:
            logger.error(f"Measured process failed: {e}")
            kill_stale_processes(tx_host, rx_host)
            return False, 0, f"process timeout: {e}", None, {}
        # Not pinned when the pinner had no CPUs to pin to, or failed.
        pinned, app_load = not unpinned, 0.0
        if measured_pin:
            pinned, app_load = _app_threads(measured_host, measured_pin, app_cpus)
        if companion_pin:
            _app_threads(companion_host, companion_pin, companion_app_cpus)

        # ── Log the measured app's output ──
        # Unfiltered on purpose: the port drop counters, DEV rate lines and
        # queue/mempool diagnostics are what attribute a capacity ceiling to a
        # cause, and a keyword filter dropped exactly those while keeping the
        # fps dumps, leaving a ceiling visible but unexplainable.
        if result.stdout_text:
            for line in result.stdout_text.splitlines():
                logger.info(f"{direction.upper()}: {line}")

        # ── Analyze FPS ──
        stdout_lines = result.stdout_text.splitlines() if result.stdout_text else []

        # Not every non-zero exit invalidates the run: the app also exits
        # non-zero on a late teardown error, after a whole good measurement.
        # Keep the data and classify the code below -- a crash is vetoed there.
        # Discarding it here cost three probes of run 35233378303 their data.
        if result.return_code != 0:
            logger.warning(f"Measured app exited with code {result.return_code}")

        losses = None
        if balance is not None:
            # Stopped first: reads taken while packets are in flight disagree.
            companion_app.stop_process()
            after = _wire_balance(tx_host, tx_vfs, rx_host, rx_vfs)
            if after is not None:
                losses = [a - b for a, b in zip(after, balance)]

        time.sleep(5)
        companion_lines = read_remote_log(companion_host, companion_log)
        # The companion's log lives on the other host, which no artifact
        # collects.  Deciding whether a receive ceiling is the receiver or the
        # sender needs the sender's own rate line, so mirror it into this log.
        for line in companion_lines:
            logger.info(f"companion {companion_dir.upper()}: {line}")

        # The rate the sessions are paced at: p59 is 60000/1001, not 59.
        paced_fps = float(Fraction(pformat_to_exact_fps(fps)))
        sent = {}
        if is_tx:
            success, count, fps_details = monitor_tx_fps(
                stdout_lines, paced_fps, num_sessions, max_drop_pct=MAX_DROP_PCT
            )
            vrx_window = fps_details["window"]
        else:
            sender = monitor_tx_fps(
                companion_lines, paced_fps, num_sessions, max_drop_pct=MAX_DROP_PCT
            )[2]
            vrx_window = sender["window"]
            sent = sent_at_rate(sender)
            success, count, fps_details = monitor_rx_fps(
                stdout_lines,
                paced_fps,
                num_sessions,
                max_drop_pct=MAX_DROP_PCT,
                sent=sent,
            )

        # Frame counts & throughput (TX/RX line assignment)
        tx_lines = stdout_lines if is_tx else companion_lines
        rx_lines = companion_lines if is_tx else stdout_lines
        tx_frames = monitor_tx_frames(tx_lines, num_sessions)
        rx_frames = monitor_rx_frames_simple(rx_lines, num_sessions)
        m_tp_fn = monitor_tx_throughput if is_tx else monitor_rx_throughput
        c_tp_fn = monitor_rx_throughput if is_tx else monitor_tx_throughput
        # The rates come from steady traffic only: for the measured app, the same
        # dumps as its FPS verdict; for the companion, its own steady dumps.
        m_window = fps_details["window"]
        c_window = companion_window(companion_lines, companion_dir)
        m_throughput = m_tp_fn(stdout_lines, m_window)
        c_throughput = c_tp_fn(companion_lines, c_window)

        m_dev_rate = monitor_dev_rate(stdout_lines, m_window)
        c_dev_rate = monitor_dev_rate(companion_lines, c_window)

        # ── Display results ──
        logger.info("=" * 70)
        logger.info(f"COMMAND: {measured_app.command}")
        if hasattr(measured_app, "config") and measured_app.config:
            logger.info("MEASURED APP CONFIG:")
            for cfg_line in json.dumps(measured_app.config, indent=2).splitlines():
                logger.info(cfg_line)
            logger.info("=" * 70)
        logger.info("=" * 70)
        cores = data_cores(
            stdout_lines, fps_details["window"], None if single_core else 0
        )
        log_data_cores(cores, fps_details["window"])
        if success and not cores:
            logger.warning("No scheduler line in the dumps judged: cores not counted")

        display_session_results(
            direction.upper(),
            dma_label,
            num_sessions,
            paced_fps,
            fps_details,
            tx_frames,
            rx_frames,
            throughput_details=m_throughput,
            dev_rate=m_dev_rate,
            companion_throughput_details=c_throughput,
            companion_dev_rate=c_dev_rate,
        )

        detail = f"{count}/{num_sessions} sessions at {paced_fps:.2f} fps"
        if not pinned:
            detail += ", app threads not pinned"
        elif app_load > APP_CPU_BUSY:
            detail += f", app CPUs {app_load:.0%} busy"
        if fps_details["sender_ramp_sessions"]:
            detail += (
                f", {len(fps_details['sender_ramp_sessions'])} kept up with a "
                "ramping sender"
            )
        # MTL flags a frame when one of its packets may have left the sender later
        # than ST 2110-21 allows (the VRX model, section 6.6.2).  A run with a
        # flagged frame is not a valid result:
        # - a TX run fails;
        # - an RX run is vetoed: its sender, not the receiver, broke the model, so
        #   the run says nothing about the receiver.
        vetoed = False
        frames, late, flagged, worst = tx_vrx(tx_lines, vrx_window)
        if frames or late:
            detail += (
                f", {'' if is_tx else 'sender '}VRX may break: {frames} frames"
                f"{' over 2 ports' if redundant else ''} in {flagged} sessions, up to "
                f"{worst:.0f} us past the target, {late} started late"
            )
            success, vetoed = False, not is_tx
        if _has_hugepage_source_fallback(tx_lines):
            # Part of this iteration's TX read its source through mmap() instead
            # of hugepages, so the number would average two memory paths.  Veto
            # rather than fail the case: the pool is shared with MTL's arena, so
            # a probe above the ceiling can exhaust it while the counts the sweep
            # will settle on stay clean.
            detail += ", hugepage source fallback"
            success, vetoed = False, True
        if result.return_code != 0:
            # Keep the code in the detail so _is_crash still schedules a VF FLR
            # before the next iteration.
            detail += f", exit code {result.return_code}"
            if _is_crash_code(result.return_code):
                # The dumps stop wherever the process did, so the steady window
                # closes at the crash and can still read as full rate.  A run
                # that died is not a capacity result at any session count, and
                # passing it here would feed the sweep false headroom.
                success, vetoed = False, True
        # The network alone caused the failure when all of these are true:
        # - every leg lost packets, and the SUT's frames missed some (ST 2022-7
        #   recovers a packet that only one leg lost);
        # - the companion ramped every session up to the bar (a full link keeps it
        #   below: that is the sweep's ceiling, not a network fault);
        # - the SUT logged no drop at its NIC or in its frame buffers;
        # - nothing above vetoed the run.
        if (
            losses
            and not success
            and not vetoed
            and all(lost > 0 for lost in losses)
            and any(MISSED_PKTS.search(line) for line in stdout_lines)
            and len(sent) == num_sessions
            and not any(m in line for line in stdout_lines for m in SUT_DROP_MARKERS)
        ):
            detail += f", {sum(losses)} pkts {NETWORK_LOSS_MARKER}"
        app_config = measured_app.config if hasattr(measured_app, "config") else None
        return success, count, detail, app_config, cores

    finally:
        companion_app.stop_process()


def _run_session_sweep(
    direction: str,
    redundant: bool,
    single_core: bool,
    fps: int,
    media_file: tuple,
    use_dma: bool,
    hosts: dict,
    mtl_path: str,
    test_time: int,
    test_config: dict = None,
    num_sessions: int | None = None,
    sch_quota: int | None = None,
    sut_app_args: str | None = None,
) -> None:
    """Auto-sweep session count using binary search to find max passing.

    If *num_sessions* is set (via ``--num_sessions`` CLI option), run a
    single iteration with exactly that many sessions and report pass/fail
    instead of performing a binary search.

    If *sch_quota* is set (via ``--sch_quota`` CLI option), override the
    scheduler session quota for every multi-core iteration.  Higher quota =
    fewer cores (sessions packed into fewer schedulers).  Single-core runs
    ignore it: they always use SCH_SESSION_QUOTA_SINGLE_CORE, which has to
    stay high enough that the run cannot spill onto a second scheduler.
    """
    media_config, media_file_path = media_file
    resolution = f"{media_config['height']}p"

    # ── Host assignment ──
    tx_host, rx_host = _get_tx_rx_hosts(hosts, direction)

    for label, host in [("TX", tx_host), ("RX", rx_host)]:
        if not hasattr(host, "vfs") or len(host.vfs) < 1:
            pytest.skip(f"{label} host ({host.name}) needs at least 1 VF")
    if redundant:
        for label, host in [("TX", tx_host), ("RX", rx_host)]:
            if not hasattr(host, "vfs_r") or len(host.vfs_r) < 1:
                pytest.skip(f"Redundant requires VFs on {label} port 1 ({host.name})")

    is_tx = direction == "tx"

    # DMA only offloads RX memcpy; TX+DMA is identical to TX no-DMA.
    if use_dma and is_tx:
        pytest.skip("DMA only benefits RX; TX+DMA is identical to TX no-DMA")
    if (
        single_core
        and not is_tx
        and not use_dma
        and _quota_mbs(media_config["width"], media_config["height"], fps)
        > RX_PKT_LCORE_MBS
    ):
        pytest.skip(
            "An RX session over 40 Gb/s without DMA polls a second lcore, or MTL "
            "refuses it when redundant, so there is no single-core result"
        )

    tx_vf, rx_vf = tx_host.vfs[0], rx_host.vfs[0]
    tx_vf_r = tx_host.vfs_r[0] if redundant else None
    rx_vf_r = rx_host.vfs_r[0] if redundant else None

    # ── DMA setup (RX SUT only — TX tests are skipped above) ──
    dma_device = None
    if use_dma:
        dma_device = setup_host_dma_all(
            rx_host,
            rx_vf,
            role="RX",
        )
        if dma_device is None:
            pytest.skip(f"DMA not available on {rx_host.name}")
    dma_label = f" with DMA ({dma_device})" if use_dma else ""

    # Below every skip: a host without VFs or a DSA channel, or a mode that does
    # not apply, is an environment fact, while a wrong source is a
    # misconfiguration and has to be reported as one.
    _check_perf_source_size(tx_host, media_file_path, media_config)

    # ── Paths ──
    build_tx = get_host_mtl_path(tx_host, default=mtl_path)
    build_rx = get_host_mtl_path(rx_host, default=mtl_path)

    max_sess = MAX_SESSIONS.get((direction, redundant), 48)

    # ── Shared kwargs for every _run_iteration call ──
    iter_kwargs = dict(
        direction=direction,
        fps=fps,
        media_config=media_config,
        media_file_path=media_file_path,
        tx_host=tx_host,
        rx_host=rx_host,
        build_tx=build_tx,
        build_rx=build_rx,
        tx_vf=tx_vf,
        rx_vf=rx_vf,
        tx_vf_r=tx_vf_r,
        rx_vf_r=rx_vf_r,
        redundant=redundant,
        single_core=single_core,
        use_dma=use_dma,
        dma_device=dma_device,
        dma_label=dma_label,
        test_time=test_time,
        sut_app_args=sut_app_args,
    )

    # ── Sweep state ──
    iteration_results: list[dict] = []
    max_passing = 0
    last_config = None  # latest RxTxApp JSON config from iterations

    mode_tag = "REDUNDANT " if redundant else ""
    core_tag = "SC" if single_core else "MC"
    dma_tag = " +DMA" if use_dma else ""
    fixed_mode = num_sessions is not None
    if fixed_mode:
        sweep_desc = f"FIXED RUN ({num_sessions} sessions)"
    else:
        sweep_desc = "SESSION SWEEP (binary search)"
    logger.info(
        f"\n{'═' * 70}\n"
        f"  {sweep_desc}: {mode_tag}{direction.upper()} "
        f"{core_tag}{dma_tag} | {fps}fps | {resolution} | "
        f"{'sessions=' + str(num_sessions) if fixed_mode else 'range=[1, ' + str(max_sess) + ']'} "
        f"test_time={test_time}s\n"
        f"{'═' * 70}"
    )

    # ── Pre-sweep: kill leftover processes, FLR all VFs, clean hugepages ──
    kill_stale_processes(tx_host, rx_host)
    time.sleep(2)

    # Build VF list for pre-sweep FLR reset (DMA VFs always on RX side).
    presweep_tx_vfs = [tx_vf] + ([tx_vf_r] if tx_vf_r else [])
    presweep_rx_vfs = [rx_vf] + ([rx_vf_r] if rx_vf_r else [])
    if dma_device:
        presweep_rx_vfs.extend(dma_device.split(","))

    logger.info("Pre-sweep cleanup: FLR + rebind all VFs, clean hugepages")
    reset_vfio_bindings(tx_host, tx_host.name, presweep_tx_vfs)
    reset_vfio_bindings(rx_host, rx_host.name, presweep_rx_vfs)
    time.sleep(5)

    def _is_infra_failure(detail: str) -> bool:
        """Return True if the failure is infrastructure-related (not capacity).

        Infrastructure failures affect ALL session counts, so continuing
        the binary search would be pointless.
        """
        infra_markers = ("open fail", "process timeout")
        return any(m in detail for m in infra_markers)

    def _measured_nothing(detail: str) -> bool:
        """Return True if a run says nothing about the cores it had.

        That is a run that never ran (infrastructure, an exception), one that died,
        one whose source fell back to mmap(), and an RX run whose sender broke VRX.
        """
        return (
            _is_infra_failure(detail)
            or detail.startswith("exception:")
            or _is_crash(detail)
            or "hugepage source fallback" in detail
            or "sender VRX may break" in detail
        )

    def _run_one(
        n: int,
        phase: str,
        quota_override: int | None = None,
    ) -> bool:
        """Run one sweep iteration.  Returns True if passed."""
        nonlocal max_passing

        quota_info = f"  quota={quota_override}" if quota_override is not None else ""
        logger.info(f"\n{'━' * 70}")
        logger.info(
            f"  [{phase}] {n} session(s){quota_info}  " f"(max_passing={max_passing})"
        )
        logger.info(f"{'━' * 70}")

        # ── Inter-iteration cleanup ──
        prev = iteration_results[-1] if iteration_results else None
        if prev is not None:
            # Always kill leftover processes first
            kill_stale_processes(tx_host, rx_host)
            time.sleep(2)

            # Build VF lists (shared by both paths)
            tx_vfs = [tx_vf] + ([tx_vf_r] if tx_vf_r else [])
            rx_vfs = [rx_vf] + ([rx_vf_r] if rx_vf_r else [])
            if dma_device:
                # DMA only on RX SUT (TX+DMA tests are skipped)
                rx_vfs.extend(dma_device.split(","))

            if _is_crash(prev["detail"]):
                # Full unbind/FLR/rebind after a DPDK crash + hugepage cleanup
                logger.info("  VFIO cleanup (previous iteration crashed)…")
                reset_vfio_bindings(tx_host, tx_host.name, tx_vfs)
                reset_vfio_bindings(rx_host, rx_host.name, rx_vfs)
                logger.info(f"  Waiting {CRASH_RECOVERY_WAIT}s for NIC link recovery…")
                time.sleep(CRASH_RECOVERY_WAIT)
            else:
                # Light check: companion SIGKILL may leave VFs unbound
                rebound = ensure_vfio_bound(
                    tx_host,
                    tx_host.name,
                    tx_vfs,
                )
                rebound |= ensure_vfio_bound(
                    rx_host,
                    rx_host.name,
                    rx_vfs,
                )
                if rebound:
                    logger.info(
                        "  VFs rebound after previous run, "
                        "waiting 10s for link recovery…"
                    )
                    time.sleep(10)
                else:
                    time.sleep(3)
        else:
            # First iteration — small grace period
            time.sleep(2)

        # ── Execute ──
        passed, count, detail = False, 0, "unknown error"
        iter_config, cores = None, {}
        try:
            passed, count, detail, iter_config, cores = _run_iteration(
                num_sessions=n,
                sch_session_quota=quota_override,
                **iter_kwargs,
            )
        except Exception as e:
            logger.error(f"  Iteration {n} raised: {type(e).__name__}: {e}")
            logger.error(traceback.format_exc())
            detail = f"exception: {type(e).__name__}: {e}"
        finally:
            kill_stale_processes(tx_host, rx_host)

        # Keep the latest RxTxApp config for the sweep summary
        if iter_config is not None:
            nonlocal last_config
            last_config = iter_config

        # The lcores the run used, and of those its schedulers: phase 2 sets how
        # many schedulers a run gets, and packet lcores come on top.
        cores_used = len(cores)
        schedulers = sum(isinstance(sch, int) for sch in cores)
        violation = _single_core_violation(single_core, cores_used)
        if passed and violation:
            passed = False
            detail = f"{detail}; {violation}"

        iteration_results.append(
            {
                "num_sessions": n,
                "passed": passed,
                "successful_count": count,
                "detail": detail,
                "cores_used": cores_used,
                "schedulers": schedulers,
                "quota": quota_override,
            }
        )

        cores_str = f"  cores={cores_used}" if cores_used else ""
        if passed:
            max_passing = max(max_passing, n)
            logger.info(f"  ✓ {n} sessions PASSED{cores_str}  ({detail})")
        else:
            logger.info(f"  ✗ {n} sessions FAILED{cores_str}  ({detail})")

        return passed

    def _last_detail() -> str:
        """Return the detail string from the most recent iteration."""
        return iteration_results[-1]["detail"] if iteration_results else ""

    def _run_with_network_retry(
        n: int,
        phase: str,
        quota_override: int | None = None,
    ) -> bool | None:
        """Run *n* sessions, re-running a failure the network alone explains.

        Returns whether they passed, or None if the network decided every attempt.
        """
        for attempt in range(1, NETWORK_LOSS_ATTEMPTS + 1):
            if _run_one(n, phase, quota_override):
                return True
            if NETWORK_LOSS_MARKER not in _last_detail():
                return False
            if attempt < NETWORK_LOSS_ATTEMPTS:
                logger.warning(
                    f"  {n} sessions lost packets between the hosts, not at the "
                    f"SUT's NIC or frame buffers; attempt {attempt + 1}/"
                    f"{NETWORK_LOSS_ATTEMPTS} in {NETWORK_LOSS_WAIT}s"
                )
                time.sleep(NETWORK_LOSS_WAIT)

        return None

    network_failure = (
        f"every one of {NETWORK_LOSS_ATTEMPTS} attempts lost packets between "
        f"the hosts, not at the SUT's NIC or frame buffers"
    )

    if fixed_mode:
        passed = _run_with_network_retry(
            num_sessions, phase="FIXED", quota_override=sch_quota
        )

        logger.info(
            f"\n{'═' * 70}\n"
            f"  FIXED RUN RESULT: {mode_tag}{direction.upper()} "
            f"{'SC' if single_core else 'MC'}{dma_label}\n"
            f"  FPS: {fps}  |  Resolution: {resolution}  |  "
            f"Sessions: {num_sessions}  |  "
            f"{'PASSED' if passed else 'FAILED'}\n"
            f"{'═' * 70}"
        )
        _log_iteration_table(iteration_results, last_config)
        logger.info(f"{'═' * 70}\n")

        if not passed:
            pytest.fail(
                f"Fixed run FAILED: {num_sessions} sessions for "
                f"{mode_tag}{direction.upper()} "
                f"{'SC' if single_core else 'MC'}{dma_label} "
                f"@ {fps}fps / {resolution}"
                + (f": {network_failure}" if passed is None else "")
            )

        logger.log(
            TEST_PASS,
            f"Fixed run: {num_sessions} sessions for "
            f"{mode_tag}{direction.upper()} "
            f"{'SC' if single_core else 'MC'}{dma_label} "
            f"@ {fps}fps / {resolution}",
        )
    else:
        # ── Phase 1: binary search max sessions ──
        # CLI --sch_quota overrides the per-mode default.
        phase1_quota: int | None = sch_quota
        if phase1_quota is None and not single_core:
            phase1_quota = SCH_SESSION_QUOTA_PHASE1_MC

        undecided = None  # The count whose every attempt the network decided
        infra = False
        lo, hi = 1, max_sess
        while lo <= hi:
            mid = (lo + hi) // 2
            passed = _run_with_network_retry(
                mid, phase="BSEARCH", quota_override=phase1_quota
            )
            if passed is None:
                undecided = mid
                logger.error(f"  ⚠ {mid} sessions: {network_failure} — aborting sweep")
                break
            if passed:
                lo = mid + 1
            else:
                if _is_infra_failure(_last_detail()):
                    logger.error("  ⚠ Infrastructure failure detected — aborting sweep")
                    infra = True
                    break
                hi = mid - 1

        # ── Phase 2 (MC only): the fewest cores that carry max_passing ──
        # Phase 1 spread the sessions so that no core was the limit; pack the same
        # sessions onto fewer schedulers, and keep the fewest that still pass.
        if (
            max_passing > 0
            and not single_core
            and undecided is None
            and not infra
            and sch_quota is None
        ):
            spread = _fewest_cores_result(iteration_results, max_passing)
            lo, hi = 1, (spread["schedulers"] if spread else 1) - 1
            while lo <= hi:
                cores = (lo + hi) // 2
                quota = _quota_for_sessions(
                    math.ceil(max_passing / cores),
                    media_config,
                    fps,
                    2 if redundant else 1,
                )
                if quota > SCH_SESSION_QUOTA_MAX:
                    lo = cores + 1
                    continue
                logger.info(
                    f"\n  ── Phase 2: {max_passing} sessions on at most {cores} "
                    f"scheduler{'s' if cores > 1 else ''} (quota={quota}) ──"
                )
                passed = _run_with_network_retry(
                    max_passing, phase="CORES", quota_override=quota
                )
                if passed is False and _measured_nothing(_last_detail()):
                    logger.warning(
                        f"  ⚠ {max_passing} sessions at quota {quota} not measured "
                        "— once more"
                    )
                    passed = _run_with_network_retry(
                        max_passing, phase="CORES", quota_override=quota
                    )
                if passed is None or (
                    passed is False and _measured_nothing(_last_detail())
                ):
                    why = (_last_detail().splitlines() or [""])[0]
                    logger.warning(
                        f"  ⚠ {max_passing} sessions at quota {quota} not measured "
                        f"({why}) — the cores are those of the runs that passed"
                    )
                    break
                if passed:
                    hi = min(cores, iteration_results[-1]["schedulers"] or cores) - 1
                else:
                    lo = cores + 1

        # ── Compute best (minimum) cores from sweep ──
        best_cores = 0
        best_quota_val = phase1_quota
        best = _fewest_cores_result(iteration_results, max_passing)
        if best:
            best_cores = best["cores_used"]
            best_quota_val = best["quota"]

        # ── Summary ──
        cores_line = ""
        if best_cores:
            cores_line = f"  CORES: {best_cores} (quota={best_quota_val})\n"

        logger.info(
            f"\n{'═' * 70}\n"
            f"  SWEEP SUMMARY: {mode_tag}{direction.upper()} "
            f"{'SC' if single_core else 'MC'}{dma_label}\n"
            f"  FPS: {fps}  |  Resolution: {resolution}  |  "
            f"MAX PASSING: {max_passing}\n"
            f"{cores_line}"
            f"{'═' * 70}"
        )
        _log_iteration_table(iteration_results, last_config, show_quota=True)
        logger.info(f"{'═' * 70}\n")

        if undecided is not None:
            pytest.fail(
                f"Sweep FAILED at {undecided} sessions for "
                f"{mode_tag}{direction.upper()} "
                f"{'SC' if single_core else 'MC'}{dma_label} "
                f"@ {fps}fps / {resolution}: {network_failure}"
            )

        if max_passing == 0:
            pytest.fail(
                f"Sweep FAILED: no sessions passed for "
                f"{mode_tag}{direction.upper()} "
                f"{'SC' if single_core else 'MC'}{dma_label} "
                f"@ {fps}fps / {resolution}"
            )

        logger.log(
            TEST_PASS,
            f"Sweep: max {max_passing} sessions"
            f"{f' on {best_cores} cores' if best_cores else ''} for "
            f"{mode_tag}{direction.upper()} "
            f"{'SC' if single_core else 'MC'}{dma_label} "
            f"@ {fps}fps / {resolution}",
        )


# ── Common parametrize decorators (shared across all 4 test functions) ──

_PERF_MARKS = [
    pytest.mark.performance,
    pytest.mark.parametrize("use_dma", [False, True], ids=["no_dma", "dma"]),
    pytest.mark.parametrize(
        "media_file",
        [
            yuv_files_422rfc10["ParkJoy_1080p_24frames"],
            yuv_files_422rfc10["ParkJoy_4K_24frames"],
            yuv_files_422rfc10["Penguin_8K_24frames"],
        ],
        indirect=["media_file"],
        ids=["1080p", "2160p", "4320p"],
    ),
    pytest.mark.parametrize(
        "fps",
        [25, 29, 50, 59],
        ids=["25fps", "29fps", "50fps", "59fps"],
    ),
    pytest.mark.parametrize(
        "single_core",
        [True, False],
        ids=["single_core", "multi_core"],
    ),
]


def _apply_perf_marks(func):
    """Apply all shared performance parametrize marks to a test function."""
    for mark in reversed(_PERF_MARKS):
        func = mark(func)
    return func


@_apply_perf_marks
def test_tx(
    hosts,
    mtl_path,
    test_time,
    nic_port_list,
    single_core,
    fps,
    media_file,
    use_dma,
    test_config,
    prepare_ramdisk,
    num_sessions,
    sch_quota,
    sut_app_args,
) -> None:
    """TX performance: auto-sweep sessions from start upward until failure."""
    _run_session_sweep(
        direction="tx",
        redundant=False,
        single_core=single_core,
        fps=fps,
        media_file=media_file,
        use_dma=use_dma,
        hosts=hosts,
        mtl_path=mtl_path,
        test_time=test_time,
        test_config=test_config,
        num_sessions=num_sessions,
        sch_quota=sch_quota,
        sut_app_args=sut_app_args,
    )


@_apply_perf_marks
def test_rx(
    hosts,
    mtl_path,
    test_time,
    nic_port_list,
    single_core,
    fps,
    media_file,
    use_dma,
    test_config,
    prepare_ramdisk,
    num_sessions,
    sch_quota,
    sut_app_args,
) -> None:
    """RX performance: auto-sweep sessions from start upward until failure."""
    _run_session_sweep(
        direction="rx",
        redundant=False,
        single_core=single_core,
        fps=fps,
        media_file=media_file,
        use_dma=use_dma,
        hosts=hosts,
        mtl_path=mtl_path,
        test_time=test_time,
        test_config=test_config,
        num_sessions=num_sessions,
        sch_quota=sch_quota,
        sut_app_args=sut_app_args,
    )


@_apply_perf_marks
def test_tx_redundant(
    hosts,
    mtl_path,
    test_time,
    nic_port_list,
    single_core,
    fps,
    media_file,
    use_dma,
    test_config,
    prepare_ramdisk,
    num_sessions,
    sch_quota,
    sut_app_args,
) -> None:
    """TX Redundant (ST2022-7): auto-sweep sessions until failure."""
    _run_session_sweep(
        direction="tx",
        redundant=True,
        single_core=single_core,
        fps=fps,
        media_file=media_file,
        use_dma=use_dma,
        hosts=hosts,
        mtl_path=mtl_path,
        test_time=test_time,
        test_config=test_config,
        num_sessions=num_sessions,
        sch_quota=sch_quota,
        sut_app_args=sut_app_args,
    )


@_apply_perf_marks
def test_rx_redundant(
    hosts,
    mtl_path,
    test_time,
    nic_port_list,
    single_core,
    fps,
    media_file,
    use_dma,
    test_config,
    prepare_ramdisk,
    num_sessions,
    sch_quota,
    sut_app_args,
) -> None:
    """RX Redundant (ST2022-7): auto-sweep sessions until failure."""
    _run_session_sweep(
        direction="rx",
        redundant=True,
        single_core=single_core,
        fps=fps,
        media_file=media_file,
        use_dma=use_dma,
        hosts=hosts,
        mtl_path=mtl_path,
        test_time=test_time,
        test_config=test_config,
        num_sessions=num_sessions,
        sch_quota=sch_quota,
        sut_app_args=sut_app_args,
    )
