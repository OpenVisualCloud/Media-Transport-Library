# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""VF dual-host performance tests — session capacity sweep via binary search."""

from __future__ import annotations

import json
import logging
import time
import traceback

import pytest
from common.nicctl import ensure_vfio_bound, reset_vfio_bindings
from conftest import get_host_mtl_path, is_host_sut
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine import ip_pools
from mtl_engine.const import RXTXAPP_PATH
from mtl_engine.dma import setup_host_dma_all
from mtl_engine.execute import kill_stale_processes, read_remote_log, run
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.performance_monitoring import (
    CpuCoreMonitor,
    display_session_results,
    get_companion_log_summary,
    log_cpu_core_results,
    monitor_dev_rate,
    monitor_rx_fps,
    monitor_rx_frames_simple,
    monitor_rx_throughput,
    monitor_tx_fps,
    monitor_tx_frames,
    monitor_tx_throughput,
)
from mtl_engine.rxtxapp import RxTxApp

logger = logging.getLogger(__name__)

WARMUP_SECONDS = 60  # Warmup passed to FPS monitor
COOLDOWN_SECONDS = 10  # Cooldown passed to FPS monitor
MAX_DROP_PCT = 0.10  # Trimmed mean: drop worst 10% of FPS samples per session
MAX_FIXED_RETRIES = 1  # Retry fixed-mode runs once on failure (transient HW events)

# ── Scheduler session quotas ──
# Controls how many sessions the library places on each scheduler (lcore).
# Higher quota = fewer cores (denser packing), but risks overloading a core.
# Values are derived from single-core capacity tests with a safety margin.
SCH_SESSION_QUOTA_SINGLE_CORE = 60

# Phase 1 binary search (multi-core): None = fall through to per-mode quota.
# CLI --sch_quota overrides both phases.
SCH_SESSION_QUOTA_PHASE1_MC = None

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


def _get_isolcpus(host) -> str | None:
    """Return the isolcpus= range from the remote host's kernel cmdline.

    Parses /proc/cmdline on *host* and returns the value of the isolcpus=
    boot parameter (e.g. "4-20,34-50,132-148,162-178"), or None if the
    host was not booted with isolcpus.
    """
    try:
        result = host.connection.execute_command("cat /proc/cmdline", shell=True)
        cmdline = (result.stdout or "").strip()
        for token in cmdline.split():
            if token.startswith("isolcpus="):
                value = token.split("=", 1)[1]
                logger.info(f"Host {host.name}: isolcpus={value}")
                return value
    except Exception as e:
        logger.warning(f"Host {host.name}: could not read isolcpus: {e}")
    return None


def _select_mc_quota(
    is_tx: bool,
    use_dma: bool,
    redundant: bool,
) -> int:
    """Return the multi-core scheduler quota for the given mode.

    Centralises the per-mode quota selection used both in ``_run_iteration``
    (for the SUT scheduler) and in the verification phase of
    ``_run_session_sweep``.
    """
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
) -> tuple[bool, int, str, dict | None, int]:
    """Run one iteration: start companion, run measured app, validate FPS.

    Args:
        sch_session_quota: If set, overrides the default SUT scheduler
            session quota for this iteration (used by core-minimization
            phase to test different quotas).

    Returns:
        (passed, successful_count, detail_string, config_dict, cores_used).
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
    isolcpus = _get_isolcpus(measured_host)
    if isolcpus:
        sut_extra_kwargs["lcores"] = isolcpus

    # ── Companion scheduler settings ──
    # The companion just needs to keep up — it is not the measured side.
    companion_dir = "rx" if is_tx else "tx"
    companion_extra_kwargs: dict = {}
    companion_extra_kwargs["sch_session_quota"] = SCH_SESSION_QUOTA_MULTI_CORE
    companion_extra_kwargs["dedicated_sys_lcore"] = True

    companion_isolcpus = _get_isolcpus(companion_host)
    if companion_isolcpus:
        companion_extra_kwargs["lcores"] = companion_isolcpus

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
        f"{build_companion}/tests/validation/logs/performance/"
        f"{direction}{r_tag}{c_tag}_fps{fps}_{num_sessions}s{d_tag}"
        f"_{companion_dir}_companion.log"
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
        return False, 0, f"companion {companion_dir} exited early:\n{tail}", None, 0

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

        # ── Run measured app with CPU monitoring ──
        cpu_monitor = CpuCoreMonitor(measured_host, interval=2)
        cpu_monitor.start(duration=test_time + rl_overhead)

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
            cpu_monitor.stop()
            return False, 0, f"process timeout: {e}", None, 0

        cores_info = cpu_monitor.stop()

        # ── Log relevant output lines ──
        if result.stdout_text:
            keywords = ("fps", "session", "error", "fail", "warn", "frame", "dma")
            for line in result.stdout_text.splitlines():
                if any(kw in line.lower() for kw in keywords):
                    logger.info(f"{direction.upper()}: {line}")
        get_companion_log_summary(companion_host, companion_log, max_lines=50)

        if result.return_code != 0:
            return False, 0, f"exit code {result.return_code}", None, 0

        # ── Analyze FPS ──
        stdout_lines = result.stdout_text.splitlines() if result.stdout_text else []

        time.sleep(5)
        companion_lines = read_remote_log(companion_host, companion_log)

        monitor_fps_fn = monitor_tx_fps if is_tx else monitor_rx_fps
        success, count, fps_details = monitor_fps_fn(
            stdout_lines,
            fps,
            num_sessions,
            warmup_seconds=WARMUP_SECONDS,
            cooldown_seconds=COOLDOWN_SECONDS,
            max_drop_pct=MAX_DROP_PCT,
        )

        # Frame counts & throughput (TX/RX line assignment)
        tx_lines = stdout_lines if is_tx else companion_lines
        rx_lines = companion_lines if is_tx else stdout_lines
        tx_frames = monitor_tx_frames(tx_lines, num_sessions)
        rx_frames = monitor_rx_frames_simple(rx_lines, num_sessions)
        m_tp_fn = monitor_tx_throughput if is_tx else monitor_rx_throughput
        c_tp_fn = monitor_rx_throughput if is_tx else monitor_tx_throughput
        m_throughput = m_tp_fn(
            stdout_lines, num_sessions, warmup_seconds=WARMUP_SECONDS
        )
        c_throughput = c_tp_fn(
            companion_lines, num_sessions, warmup_seconds=WARMUP_SECONDS
        )

        m_dev_rate = monitor_dev_rate(stdout_lines, warmup_seconds=WARMUP_SECONDS)
        c_dev_rate = monitor_dev_rate(companion_lines, warmup_seconds=WARMUP_SECONDS)

        # ── Display results ──
        logger.info("=" * 70)
        logger.info(f"COMMAND: {measured_app.command}")
        if hasattr(measured_app, "config") and measured_app.config:
            logger.info("MEASURED APP CONFIG:")
            for cfg_line in json.dumps(measured_app.config, indent=2).splitlines():
                logger.info(cfg_line)
            logger.info("=" * 70)
        logger.info("=" * 70)
        log_cpu_core_results(cores_info)

        display_session_results(
            direction.upper(),
            dma_label,
            num_sessions,
            fps,
            fps_details,
            tx_frames,
            rx_frames,
            fps_warmup_seconds=WARMUP_SECONDS,
            throughput_details=m_throughput,
            dev_rate=m_dev_rate,
            companion_throughput_details=c_throughput,
            companion_dev_rate=c_dev_rate,
        )

        detail = f"{count}/{num_sessions} sessions at {fps} fps"
        app_config = measured_app.config if hasattr(measured_app, "config") else None
        cores_used = cores_info.get("cores_used", 0) if cores_info else 0
        return success, count, detail, app_config, cores_used

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
) -> None:
    """Auto-sweep session count using binary search to find max passing.

    If *num_sessions* is set (via ``--num_sessions`` CLI option), run a
    single iteration with exactly that many sessions and report pass/fail
    instead of performing a binary search.

    If *sch_quota* is set (via ``--sch_quota`` CLI option), override the
    scheduler session quota for all iterations.  Higher quota = fewer
    cores (sessions packed into fewer schedulers).  Use 60 for minimal
    cores.
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
        f"test_time={test_time}s "
        f"warmup={WARMUP_SECONDS}s cooldown={COOLDOWN_SECONDS}s\n"
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

    def _is_crash(detail: str) -> bool:
        """Return True if the detail indicates a crash requiring VF FLR."""
        crash_codes = (
            "exit code -",
            "exit code 134",  # SIGABRT
            "exit code 137",  # SIGKILL
            "exit code 139",  # SIGSEGV
            "exit code 244",  # DPDK
            "exit code 251",  # DPDK
            "companion",
            "exited early",
        )
        return any(code in detail for code in crash_codes)

    def _is_infra_failure(detail: str) -> bool:
        """Return True if the failure is infrastructure-related (not capacity).

        Infrastructure failures affect ALL session counts, so continuing
        the binary search would be pointless.
        """
        infra_markers = ("open fail", "process timeout")
        return any(m in detail for m in infra_markers)

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
        iter_config, cores_used = None, 0
        try:
            passed, count, detail, iter_config, cores_used = _run_iteration(
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

        iteration_results.append(
            {
                "num_sessions": n,
                "passed": passed,
                "successful_count": count,
                "detail": detail,
                "cores_used": cores_used,
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

    if fixed_mode:
        # ── Fixed session run with retry ──
        # Transient NIC/system events (link flaps, PF admin resets) can
        # cause ~20 s outages that tank per-session averages.  Retrying
        # once is the most reliable way to distinguish real capacity
        # failures from one-off hardware glitches.
        passed = False
        for attempt in range(1 + MAX_FIXED_RETRIES):
            passed = _run_one(num_sessions, phase="FIXED", quota_override=sch_quota)
            if passed:
                break
            if attempt < MAX_FIXED_RETRIES:
                logger.warning(
                    f"  Attempt {attempt + 1} failed — possible transient event. "
                    f"Retrying after VF reset ({MAX_FIXED_RETRIES - attempt} "
                    f"retries left)…"
                )
                kill_stale_processes(tx_host, rx_host)
                time.sleep(2)
                reset_vfio_bindings(tx_host, tx_host.name, presweep_tx_vfs)
                reset_vfio_bindings(rx_host, rx_host.name, presweep_rx_vfs)
                time.sleep(10)

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

        lo, hi = 1, max_sess
        while lo <= hi:
            mid = (lo + hi) // 2
            if _run_one(mid, phase="BSEARCH", quota_override=phase1_quota):
                lo = mid + 1
            else:
                if _is_infra_failure(_last_detail()):
                    logger.error("  ⚠ Infrastructure failure detected — aborting sweep")
                    break
                hi = mid - 1

        # ── Phase 2 (MC only): verification re-run at default quota ──
        phase2_cores: int | None = None
        default_quota: int | None = None
        if max_passing > 0 and not single_core:
            default_quota = _select_mc_quota(is_tx, use_dma, redundant)
            logger.info(
                f"\n  ── Phase 2: re-verify {max_passing} sessions "
                f"at quota={default_quota} ──"
            )
            if _run_one(
                max_passing,
                phase="VERIFY",
                quota_override=default_quota,
            ):
                verify_it = iteration_results[-1]
                phase2_cores = verify_it.get("cores_used", 0)
                logger.info(
                    f"  ✓ Verification passed at quota={default_quota} "
                    f"({phase2_cores} cores)"
                )
            else:
                logger.info(
                    f"  ✗ Verification FAILED at quota={default_quota} — "
                    f"{max_passing} sessions not stable"
                )

        # ── Compute best (minimum) cores from sweep ──
        best_cores = 0
        best_quota_val = phase1_quota
        for it in reversed(iteration_results):
            if it["num_sessions"] == max_passing and it["passed"]:
                best_cores = it.get("cores_used", 0)
                best_quota_val = it.get("quota")
                break

        # ── Summary ──
        cores_line = ""
        if best_cores:
            cores_line = f"  CORES: {best_cores} (quota={best_quota_val})"
            if phase2_cores is not None and phase2_cores != best_cores:
                cores_line += (
                    f"  |  default quota={default_quota}: " f"{phase2_cores} cores"
                )
            cores_line += "\n"

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
            yuv_files_422rfc10["ParkJoy_1080p"],
            yuv_files_422rfc10["ParkJoy_4K"],
            yuv_files_422rfc10["Penguin_8K"],
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
    )
