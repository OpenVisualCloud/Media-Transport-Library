# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""VF dual-host performance tests — session capacity sweep via binary search."""

import json
import logging
import time
import traceback
from typing import List, Optional, Tuple

import pytest
from common.nicctl import ensure_vfio_bound, reset_vfio_bindings
from conftest import get_host_mtl_path, is_host_sut
from mfd_common_libs.log_levels import TEST_PASS
from mtl_engine import ip_pools
from mtl_engine.const import RXTXAPP_PATH
from mtl_engine.dma import setup_host_dma
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
COOLDOWN_SECONDS = 15  # Cooldown passed to FPS monitor
SCH_SESSION_QUOTA_SINGLE_CORE = 60

MAX_SESSIONS = {
    ("tx", False): 48,
    ("tx", True): 36,
    ("rx", False): 48,
    ("rx", True): 36,
}

START_SESSIONS = 20
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
    add_pacing=False,
):
    """Set TX or RX-specific kwargs (NIC port, file, redundancy)."""
    if side == "tx":
        kwargs["input_file"] = media_path
        if add_pacing:
            kwargs["pacing_way"] = "auto"
        vf, vf_r = tx_vf, tx_vf_r
    else:
        kwargs["output_file"] = "/dev/null"
        kwargs["measure_latency"] = True
        vf, vf_r = rx_vf, rx_vf_r
    if redundant and vf_r:
        kwargs.update(nic_port=vf, nic_port_r=vf_r, **redundant_kwargs)
    else:
        kwargs["nic_port"] = vf


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
) -> Tuple[bool, int, str, Optional[dict]]:
    """Run one iteration: start companion, run measured app, validate FPS.

    Returns (passed, successful_count, detail_string, config_dict).
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
    if single_core:
        base_kwargs["sch_session_quota"] = SCH_SESSION_QUOTA_SINGLE_CORE
        base_kwargs["disable_migrate"] = True

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
    companion_dir = "rx" if is_tx else "tx"
    companion_kwargs = {
        **base_kwargs,
        "direction": companion_dir,
        "test_time": test_time + 10,  # companion outlives the measured app
    }

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
            add_pacing=True,
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
        # Double-check: look for fatal errors in the first lines of the log
        log_snippet = read_remote_log(companion_host, companion_log)
        for line in log_snippet[:30]:
            if "open fail" in line or "open_source fail" in line:
                companion_alive = False
                break

    if not companion_alive:
        log_snippet = read_remote_log(companion_host, companion_log)
        tail = "\n".join(log_snippet[-20:]) if log_snippet else "(empty)"
        companion_app.stop_process()
        return False, 0, f"companion {companion_dir} exited early:\n{tail}", None

    try:
        # ── Build measured app ──
        measured_app = RxTxApp(RXTXAPP_PATH)
        measured_kwargs = {
            **base_kwargs,
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

        if use_dma and dma_device:
            measured_kwargs["dma_dev"] = dma_device

        measured_app.create_command(**measured_kwargs)
        measured_app.prepare_execution(build=build_measured, host=measured_host)

        # ── Run measured app with CPU monitoring ──
        cpu_monitor = CpuCoreMonitor(measured_host, interval=2)
        cpu_monitor.start(duration=test_time)

        try:
            result = run(
                measured_app.command,
                cwd=build_measured,
                timeout=test_time + 60,
                testcmd=True,
                host=measured_host,
            )
        except Exception as e:
            logger.error(f"Measured process failed: {e}")
            kill_stale_processes(tx_host, rx_host)
            cpu_monitor.stop()
            return False, 0, f"process timeout: {e}", None

        cores_info = cpu_monitor.stop()

        # ── Log relevant output lines ──
        if result.stdout_text:
            keywords = ("fps", "session", "error", "fail", "warn", "frame", "dma")
            for line in result.stdout_text.splitlines():
                if any(kw in line.lower() for kw in keywords):
                    logger.info(f"{direction.upper()}: {line}")
        get_companion_log_summary(companion_host, companion_log, max_lines=50)

        if result.return_code != 0:
            return False, 0, f"exit code {result.return_code}", None

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
        return success, count, detail, app_config

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
) -> None:
    """Auto-sweep session count using binary search to find max passing."""
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

    tx_vf, rx_vf = tx_host.vfs[0], rx_host.vfs[0]
    tx_vf_r = tx_host.vfs_r[0] if redundant else None
    rx_vf_r = rx_host.vfs_r[0] if redundant else None

    # ── DMA setup (measured host only, auto-discovered on same NUMA as NIC) ──
    dma_device = None
    if use_dma:
        measured_host = tx_host if direction == "tx" else rx_host
        dma_device = setup_host_dma(
            measured_host,
            tx_vf if direction == "tx" else rx_vf,
            role=direction.upper(),
        )
        if dma_device is None:
            pytest.skip(f"DMA not available on {measured_host.name}")
    dma_label = f" with DMA ({dma_device})" if use_dma else ""

    # ── Paths ──
    build_tx = get_host_mtl_path(tx_host, default=mtl_path)
    build_rx = get_host_mtl_path(rx_host, default=mtl_path)

    max_sess = MAX_SESSIONS.get((direction, redundant), 48)
    start_sess = min(START_SESSIONS, max_sess)

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
    iteration_results: List[dict] = []
    max_passing = 0
    last_config = None  # latest RxTxApp JSON config from iterations

    mode_tag = "REDUNDANT " if redundant else ""
    core_tag = "SC" if single_core else "MC"
    dma_tag = " +DMA" if use_dma else ""
    logger.info(
        f"\n{'═' * 70}\n"
        f"  SESSION SWEEP (binary search): {mode_tag}{direction.upper()} "
        f"{core_tag}{dma_tag} | {fps}fps | {resolution} | "
        f"start={start_sess} max={max_sess} "
        f"test_time={test_time}s "
        f"warmup={WARMUP_SECONDS}s cooldown={COOLDOWN_SECONDS}s\n"
        f"{'═' * 70}"
    )

    # ── Pre-sweep: kill leftover processes (VFs already bound by conftest) ──
    kill_stale_processes(tx_host, rx_host)
    time.sleep(3)

    def _is_crash(detail: str) -> bool:
        """Return True if the detail string indicates a DPDK/VFIO crash."""
        return any(
            code in detail
            for code in ("exit code -1", "exit code 244", "exit code 251")
        )

    def _is_infra_failure(detail: str) -> bool:
        """Return True if the failure is infrastructure-related (not capacity).

        Infrastructure failures (companion crash, media file missing, SSH
        errors) will affect ALL session counts, so continuing the sweep
        would be pointless.
        """
        infra_markers = (
            "companion",
            "exited early",
            "open fail",
            "process timeout",
        )
        return any(m in detail for m in infra_markers)

    def _run_one(n: int, phase: str) -> bool:
        """Run one sweep iteration.  Returns True if the iteration passed."""
        nonlocal max_passing

        logger.info(f"\n{'━' * 70}")
        logger.info(f"  [{phase}] {n} session(s)  " f"(max_passing={max_passing})")
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
                measured_vfs = tx_vfs if direction == "tx" else rx_vfs
                measured_vfs.append(dma_device)

            if _is_crash(prev["detail"]):
                # Full unbind/rebind after a DPDK crash
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
        passed, count, detail, iter_config = False, 0, "unknown error", None
        try:
            passed, count, detail, iter_config = _run_iteration(
                num_sessions=n, **iter_kwargs
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
            }
        )

        if passed:
            max_passing = max(max_passing, n)
            logger.info(f"  ✓ {n} sessions PASSED  ({detail})")
        else:
            logger.info(f"  ✗ {n} sessions FAILED  ({detail})")

        return passed

    def _last_detail() -> str:
        """Return the detail string from the most recent iteration."""
        return iteration_results[-1]["detail"] if iteration_results else ""

    # ── Binary search to find max passing session count ──
    # 1. Probe start_sess first. If it passes, binary-search UP in
    #    [start_sess+1, max_sess]. If it fails, binary-search DOWN in
    #    [1, start_sess-1].
    #
    # Invariant: lo-1 is known-pass (or 0), hi+1 is known-fail (or max+1).
    #
    # Early abort: if the last failure looks like an infrastructure problem
    # (companion crash, media file missing) rather than a capacity limit,
    # stop immediately — no session count will succeed.

    if _run_one(start_sess, phase="PROBE"):
        # start_sess passed — search upward for the ceiling
        lo, hi = start_sess + 1, max_sess
        while lo <= hi:
            mid = (lo + hi) // 2
            if _run_one(mid, phase="BSEARCH-UP"):
                lo = mid + 1
            else:
                if _is_infra_failure(_last_detail()):
                    logger.error("  ⚠ Infrastructure failure detected — aborting sweep")
                    break
                hi = mid - 1
    else:
        if _is_infra_failure(_last_detail()):
            logger.error("  ⚠ Infrastructure failure on first probe — aborting sweep")
        else:
            # start_sess failed — search downward for any passing count
            lo, hi = 1, start_sess - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                if _run_one(mid, phase="BSEARCH-DOWN"):
                    lo = mid + 1
                else:
                    if _is_infra_failure(_last_detail()):
                        logger.error(
                            "  ⚠ Infrastructure failure detected " "— aborting sweep"
                        )
                        break
                    hi = mid - 1

    # ── Summary ──
    logger.info(
        f"\n{'═' * 70}\n"
        f"  SWEEP SUMMARY: {mode_tag}{direction.upper()} "
        f"{'SC' if single_core else 'MC'}{dma_label}\n"
        f"  FPS: {fps}  |  Resolution: {resolution}  |  "
        f"MAX PASSING: {max_passing}\n"
        f"{'═' * 70}"
    )
    for it in iteration_results:
        ok = "✓" if it["passed"] else "✗"
        logger.info(f"  {it['num_sessions']:>3} sessions  {ok}  {it['detail']}")
    if last_config:
        logger.info("RXTXAPP_CONFIG_BEGIN")
        for cfg_line in json.dumps(last_config, indent=2).splitlines():
            logger.info(cfg_line)
        logger.info("RXTXAPP_CONFIG_END")
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
        f"Sweep: max {max_passing} sessions for "
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
    )
