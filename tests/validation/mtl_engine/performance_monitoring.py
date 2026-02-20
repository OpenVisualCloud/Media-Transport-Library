# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""Performance monitoring utilities for MTL tests — FPS, frames, throughput, CPU."""

import logging
import re
import time
import uuid
from datetime import datetime

logger = logging.getLogger(__name__)

# Default FPS monitoring configuration
FPS_WARMUP_SECONDS = 45  # Skip first N seconds (PTP sync + session ramp-up)
FPS_COOLDOWN_SECONDS = 15  # Discard last N seconds to ignore teardown artifacts
FPS_TOLERANCE_PCT = 0.99  # 99% of requested FPS required for pass

# Shared timestamp regex (compiled once)
_TS_RE = re.compile(r"MTL:\s+(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}),?")
_TS_FMT = "%Y-%m-%d %H:%M:%S"


# ── FPS Monitoring ──────────────────────────────────────────────────────


def _monitor_fps_generic(
    log_lines,
    expected_fps,
    num_sessions,
    session_pattern,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    warmup_seconds=FPS_WARMUP_SECONDS,
    cooldown_seconds=FPS_COOLDOWN_SECONDS,
):
    """Parse log lines, filter by warmup/cooldown, return (all_ok, count, details)."""
    fps_re = re.compile(session_pattern)
    start_ts = last_ts = None
    min_required = expected_fps * fps_tolerance_pct
    raw_samples = []  # (session_id, fps, elapsed_s)

    for line in log_lines:
        ts_m = _TS_RE.search(line)
        if ts_m and start_ts is None:
            start_ts = datetime.strptime(ts_m.group(1), _TS_FMT)
        m = fps_re.search(line)
        if m and ts_m and start_ts:
            cur = datetime.strptime(ts_m.group(1), _TS_FMT)
            last_ts = cur
            raw_samples.append(
                (int(m.group(2)), float(m.group(3)), (cur - start_ts).total_seconds())
            )

    end_elapsed = (
        (last_ts - start_ts).total_seconds() if last_ts and start_ts else float("inf")
    )
    cutoff = end_elapsed - cooldown_seconds

    session_fps = {}
    for sid, fps_val, elapsed in raw_samples:
        if elapsed < warmup_seconds:
            continue
        if cooldown_seconds > 0 and elapsed > cutoff:
            continue
        session_fps.setdefault(sid, []).append(fps_val)

    ok_sessions = {
        sid
        for sid, hist in session_fps.items()
        if hist and sum(hist) / len(hist) >= min_required
    }
    details = {
        "successful_count": len(ok_sessions),
        "successful_sessions": sorted(ok_sessions),
        "session_fps_history": session_fps,
        "min_required_fps": min_required,
    }
    return len(ok_sessions) == num_sessions, len(ok_sessions), details


_TX_FPS_RE = r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)"
_RX_FPS_RE = r"RX_VIDEO_SESSION\(\d+,(\d+):app_rx_st20p_(\d+)\):\s+fps\s+([\d.]+)"


def monitor_tx_fps(
    log_lines,
    expected_fps,
    num_sessions,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    warmup_seconds=FPS_WARMUP_SECONDS,
    cooldown_seconds=FPS_COOLDOWN_SECONDS,
):
    """Monitor TX FPS from RxTxApp logs."""
    return _monitor_fps_generic(
        log_lines,
        expected_fps,
        num_sessions,
        _TX_FPS_RE,
        fps_tolerance_pct,
        warmup_seconds,
        cooldown_seconds,
    )


def monitor_rx_fps(
    log_lines,
    expected_fps,
    num_sessions,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    warmup_seconds=FPS_WARMUP_SECONDS,
    cooldown_seconds=FPS_COOLDOWN_SECONDS,
):
    """Monitor RX FPS from RxTxApp logs."""
    return _monitor_fps_generic(
        log_lines,
        expected_fps,
        num_sessions,
        _RX_FPS_RE,
        fps_tolerance_pct,
        warmup_seconds,
        cooldown_seconds,
    )


# ── Frame Count Extraction ──────────────────────────────────────────────


def _extract_max_frames(log_lines, *patterns):
    """Extract max frame count per session from log lines matching any pattern."""
    compiled = [re.compile(p) for p in patterns]
    result = {}
    for line in log_lines:
        for pat in compiled:
            m = pat.search(line)
            if m:
                sid, frames = int(m.group(1)), int(m.group(2))
                if sid not in result or frames > result[sid]:
                    result[sid] = frames
                break
    return result


def monitor_tx_frames(log_lines, num_sessions=0):
    """Extract TX frame counts per session → {session_id: max_frames}."""
    return _extract_max_frames(
        log_lines,
        r"TX_VIDEO_SESSION\(\d+,\d+:app_tx_st20p_(\d+)\):\s+fps\s+[\d.]+\s+frames\s+(\d+)",
    )


def monitor_rx_frames_simple(log_lines, num_sessions=0):
    """Extract RX frame counts per session → {session_id: max_frames}."""
    return _extract_max_frames(
        log_lines,
        r"RX_VIDEO_SESSION\(\d+,\d+:app_rx_st20p_(\d+)\):\s+fps\s+[\d.]+\s+frames\s+(\d+)",
        r"app_rx_st20p_result\((\d+)\),\s+OK,\s+fps\s+[\d.]+,\s+(\d+)\s+frame received",
    )


# ── Throughput & Device Rate ────────────────────────────────────────────


def _collect_after_warmup(log_lines, data_pattern, warmup_seconds):
    """Yield regex matches for data lines past the warmup period."""
    compiled = re.compile(data_pattern)
    start_ts = None
    for line in log_lines:
        ts_m = _TS_RE.search(line)
        if ts_m and start_ts is None:
            start_ts = datetime.strptime(ts_m.group(1), _TS_FMT)
        m = compiled.search(line)
        if m:
            if ts_m and start_ts:
                elapsed = (
                    datetime.strptime(ts_m.group(1), _TS_FMT) - start_ts
                ).total_seconds()
                if elapsed < warmup_seconds:
                    continue
            elif start_ts is None:
                continue
            yield m


def monitor_tx_throughput(log_lines, num_sessions=0, warmup_seconds=FPS_WARMUP_SECONDS):
    """Monitor TX per-session throughput (Mb/s)."""
    hist = {}
    for m in _collect_after_warmup(
        log_lines,
        r"TX_VIDEO_SESSION\(\d+,(\d+)\):\s+throughput\s+([\d.]+)\s+Mb/s",
        warmup_seconds,
    ):
        hist.setdefault(int(m.group(1)), []).append(float(m.group(2)))
    return {"session_throughput_history": hist}


def monitor_rx_throughput(log_lines, num_sessions=0, warmup_seconds=FPS_WARMUP_SECONDS):
    """Monitor RX per-session throughput (Mb/s)."""
    hist = {}
    for m in _collect_after_warmup(
        log_lines,
        r"RX_VIDEO_SESSION\(\d+,(\d+)\):\s+throughput\s+([\d.]+)\s+Mb/s",
        warmup_seconds,
    ):
        hist.setdefault(int(m.group(1)), []).append(float(m.group(2)))
    return {"session_throughput_history": hist}


def monitor_dev_rate(log_lines, warmup_seconds=FPS_WARMUP_SECONDS):
    """Monitor DEV average TX/RX rate (Mb/s)."""
    tx_hist, rx_hist = [], []
    for m in _collect_after_warmup(
        log_lines,
        r"DEV\(\d+\):\s+Avr rate,\s+tx:\s+([\d.]+)\s+Mb/s,\s+rx:\s+([\d.]+)\s+Mb/s",
        warmup_seconds,
    ):
        tx_hist.append(float(m.group(1)))
        rx_hist.append(float(m.group(2)))
    return {"tx_rate_history": tx_hist, "rx_rate_history": rx_hist}


# ── Result Display ──────────────────────────────────────────────────────


def display_session_results(
    direction,
    dsa_label,
    num_sessions,
    fps,
    fps_details,
    tx_frame_counts,
    rx_frame_counts,
    fps_warmup_seconds=FPS_WARMUP_SECONDS,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    throughput_details=None,
    dev_rate=None,
    companion_throughput_details=None,
    companion_dev_rate=None,
):
    """Display FPS, frame count, and throughput results for all sessions."""
    ok_ids = fps_details.get("successful_sessions", [])
    min_req = fps_details.get("min_required_fps", fps * fps_tolerance_pct)

    logger.info("=" * 80)
    logger.info(
        f"{direction} Results{dsa_label}: {len(ok_ids)}/{num_sessions} sessions "
        f"at {fps} fps (min: {min_req:.1f}, warmup: {fps_warmup_seconds}s)"
    )
    logger.info("=" * 80)

    for sid in range(num_sessions):
        hist = fps_details.get("session_fps_history", {}).get(sid, [])
        tx_f = tx_frame_counts.get(sid, 0)
        rx_f = rx_frame_counts.get(sid, 0)
        pct = f"{rx_f / tx_f * 100:.1f}%" if tx_f > 0 else "N/A"
        if hist:
            avg = sum(hist) / len(hist)
            status = "✓" if sid in ok_ids else "✗"
            logger.info(
                f"  S{sid}: FPS avg={avg:.1f} min={min(hist):.1f} "
                f"max={max(hist):.1f} {status} | TX={tx_f} RX={rx_f} ({pct})"
            )
        else:
            logger.info(f"  S{sid}: No FPS data ✗ | TX={tx_f} RX={rx_f} ({pct})")

    # Throughput (informational only)
    _log_throughput("Measured", direction, throughput_details, dev_rate, num_sessions)
    comp_dir = "RX" if direction == "TX" else "TX"
    _log_throughput(
        "Companion",
        comp_dir,
        companion_throughput_details,
        companion_dev_rate,
        num_sessions,
    )
    logger.info("=" * 80)


def _log_throughput(label, direction, tp_details, dev_rate, num_sessions):
    """Log throughput summary for one side (measured or companion)."""
    if not tp_details and not dev_rate:
        return
    logger.info(f"  --- {label} ({direction}) throughput [informational] ---")
    if dev_rate:
        for key, name in [("tx_rate_history", "TX"), ("rx_rate_history", "RX")]:
            vals = dev_rate.get(key, [])
            if vals:
                logger.info(
                    f"    DEV {name}: {sum(vals)/len(vals):.2f} Mb/s "
                    f"({len(vals)} samples)"
                )
    if tp_details:
        for sid in range(num_sessions):
            vals = tp_details.get("session_throughput_history", {}).get(sid, [])
            if vals:
                logger.info(
                    f"    S{sid} ({direction}): avg={sum(vals)/len(vals):.2f} "
                    f"min={min(vals):.2f} max={max(vals):.2f} Mb/s"
                )


def get_companion_log_summary(host, log_path, max_lines=30):
    """Retrieve and display tail of companion log from remote host."""
    try:
        result = host.connection.execute_command(
            f"tail -n {max_lines} {log_path} 2>/dev/null || echo 'Log file not found'",
            shell=True,
        )
        if result.stdout and "Log file not found" not in result.stdout:
            for line in result.stdout.splitlines():
                if line.strip():
                    logger.debug(f"  companion: {line}")
    except Exception as e:
        logger.warning(f"Could not retrieve companion log from {log_path}: {e}")


# ── CPU Core Usage Monitor ──────────────────────────────────────────────


class CpuCoreMonitor:
    """Background CPU core usage monitor using mpstat.

    Tracks cores that reach 100% %usr during the test.

    Usage::

        monitor = CpuCoreMonitor(host, interval=2)
        monitor.start(duration=120)
        # … run test …
        info = monitor.stop()  # {"cores_used": N, "max_cores_simultaneous": M, ...}
    """

    def __init__(self, host, interval=2):
        self.host = host
        self.interval = interval
        self._process = None
        self._log_path = ""

    def start(self, duration=120):
        """Start background mpstat sampling."""
        count = max(1, duration // self.interval)
        self._log_path = f"/tmp/cpu_monitor_{uuid.uuid4().hex[:8]}.log"
        try:
            self._process = self.host.connection.start_process(
                f"mpstat -P ALL {self.interval} {count} > {self._log_path} 2>&1",
                shell=True,
            )
        except Exception as e:
            logger.warning(f"CpuCoreMonitor: start failed on {self.host.name}: {e}")
            self._process = None

    def stop(self):
        """Stop monitoring, parse results, return core usage dict."""
        empty = {
            "cores_used": 0,
            "max_cores_simultaneous": 0,
            "samples": 0,
            "core_ids": [],
        }
        if not self._process:
            return empty

        try:
            if hasattr(self._process, "running") and self._process.running:
                time.sleep(1)
                try:
                    self._process.stop()
                except Exception:
                    pass
                try:
                    self._process.kill(wait=5)
                except Exception:
                    pass
        except Exception:
            pass

        try:
            r = self.host.connection.execute_command(
                f"cat {self._log_path} 2>/dev/null || echo ''", shell=True
            )
            result = self._parse_mpstat(r.stdout or "")
        except Exception:
            result = empty

        try:
            self.host.connection.execute_command(f"rm -f {self._log_path}", shell=True)
        except Exception:
            pass
        return result

    @staticmethod
    def _parse_mpstat(output):
        """Parse mpstat -P ALL output → cores that hit 100% %usr."""
        all_busy = set()
        max_sim = 0
        samples = 0
        cur_busy = set()
        in_sample = False

        for line in output.splitlines():
            s = line.strip()
            if not s:
                if in_sample and cur_busy:
                    samples += 1
                    all_busy.update(cur_busy)
                    max_sim = max(max_sim, len(cur_busy))
                    cur_busy = set()
                in_sample = False
                continue

            parts = s.split()
            if len(parts) < 5:
                continue

            # Detect AM/PM format shift
            off = 1 if len(parts) > 2 and parts[1] in ("AM", "PM") else 0
            cpu_col = parts[1 + off] if len(parts) > 1 + off else ""

            if cpu_col == "CPU":
                in_sample = True
                continue
            if not cpu_col.isdigit() or cpu_col == "all":
                continue

            try:
                if float(parts[2 + off]) >= 100.0:
                    cur_busy.add(int(cpu_col))
                    in_sample = True
            except (ValueError, IndexError):
                continue

        if cur_busy:
            samples += 1
            all_busy.update(cur_busy)
            max_sim = max(max_sim, len(cur_busy))

        return {
            "cores_used": len(all_busy),
            "max_cores_simultaneous": max_sim,
            "samples": samples,
            "core_ids": sorted(all_busy),
        }


def log_cpu_core_results(info):
    """Log CPU core usage in parseable format for the HTML report."""
    logger.info(
        f"[CPU_CORES] cores_used={info['cores_used']} "
        f"max_simultaneous={info['max_cores_simultaneous']} "
        f"samples={info['samples']} "
        f"core_ids={','.join(str(c) for c in info['core_ids'])}"
    )
