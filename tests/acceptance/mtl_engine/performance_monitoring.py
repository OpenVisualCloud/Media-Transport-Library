# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Performance monitoring utilities for MTL tests — FPS, frames, throughput, CPU."""

import logging
import re
import time
import uuid
from fractions import Fraction
from typing import NamedTuple

from mtl_engine.media_files import pformat_to_exact_fps

logger = logging.getLogger(__name__)

# Default FPS monitoring configuration
FPS_TOLERANCE_PCT = 0.99  # 99% of requested FPS required for pass
# MTL dumps stats every 10 s and the first/last dumps are discarded as partial,
# so 4 samples needs a run of roughly 70 s or more.
FPS_MIN_STEADY_SAMPLES = 4  # Refuse to judge a run on fewer stat dumps than this
MAX_DROP_PCT = 0.10  # Trimmed mean: drop worst 10% of FPS samples per session

# Shared timestamp regex (compiled once)
_TS_RE = re.compile(r"MTL:\s+(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}),?")
# Banner stat_dump() prints before the stats of one dump; see lib/src/mt_stat.c.
_STAT_DUMP_RE = re.compile(r"M T\s+D E V\s+S T A T E")

_TX_FPS_RE = r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)"
_RX_FPS_RE = r"RX_VIDEO_SESSION\(\d+,(\d+):app_rx_st20p_(\d+)\):\s+fps\s+([\d.]+)"


class _Dump(NamedTuple):
    """Identity of one stat dump: its ordinal in the log, and the time it named."""

    idx: int
    ts: str


def _walk_dumps(log_lines):
    """Yield ``(dump, line)`` for every line of *log_lines* from the first dump on.

    A line belongs to the dump whose banner ``stat_dump()`` printed last before
    it, not to the dump its timestamp names, for two reasons:

    * MTL stamps each line as it prints it, so one dump can carry two seconds:
      the ``DEV`` line on the banner's and the session census on the next.
    * Freeing a session prints its stats once more, outside any dump.  An app
      that frees every session within a second prints what reads, by stamp, as
      one more dump naming every session.

    Those free-time lines are harmless where this puts them.  A session prints
    them and is unlinked under one hold of its lock, so no dump begun after them
    can name it; they therefore land in the last dump naming every session or
    later, and :func:`_steady_window` excludes those.
    """
    dump, seen = None, 0
    for line in log_lines:
        if _STAT_DUMP_RE.search(line):
            ts_m = _TS_RE.search(line)
            dump = _Dump(seen, ts_m.group(1) if ts_m else "")
            seen += 1
        if dump is not None:
            yield dump, line


def paced_fps(fps_token):
    """Rate the transport is paced at for a ``p<token>`` format token → float.

    The token is not the rate: `p59` is ST_FPS_P59_94 = 60000/1001.  Scoring
    sessions against 59 would grant 2.5 % of slack on top of
    :data:`FPS_TOLERANCE_PCT` and pass streams that are dropping frames.
    """
    return float(Fraction(pformat_to_exact_fps(fps_token)))


# ── FPS Monitoring ──────────────────────────────────────────────────────


def _steady_window(dumps, num_sessions):
    """Pick the stat dumps that cover steady-state traffic → (list of dumps, reason).

    MTL emits one stat line per session per dump, so a dump is a census of
    the sessions alive at that moment and the FPS it reports is the average
    over the period since the previous dump.  Two kinds of dump therefore
    cannot be compared against the target rate:

    * **Partial-period dumps.**  Session create and session free happen
      *inside* a dump period, so the first dump naming every session covers
      the last session's ramp-up and the last one covers the first free.  A
      1 s slice of a 10 s period reads as 6 fps at full rate.
    * **Pre-traffic dumps.**  RX sessions exist and report 0 fps from
      creation until the sender starts, which on the RX side is a separate
      process on another host.

    So: bound the window by the first and last dump naming every session,
    take what lies strictly between them, and start after the first dump in
    which every session is live.  Everything from there on is retained
    *including* zeros — a mid-run collapse is the failure this metric exists
    to catch, not an artifact.

    Dumps inside the bounds that name only some sessions are kept: no
    session can be created or freed there, so a short census is a stat line
    lost in transport, and the sessions it does name still reported a whole
    period.
    """
    order = list(dumps)
    full = [i for i, d in enumerate(order) if len(dumps[d]) == num_sessions]
    if len(full) < 2 or full[-1] - full[0] < 2:
        return [], f"no run of dumps naming all {num_sessions} sessions"
    window = order[full[0] + 1 : full[-1]]
    live = next(
        (
            i
            for i, d in enumerate(window)
            if len(dumps[d]) == num_sessions and all(v > 0 for v in dumps[d].values())
        ),
        None,
    )
    if live is None:
        return [], "no dump had every session live"
    return window[live + 1 :], ""


def _fps_dumps(log_lines, session_pattern):
    """Parse per-session FPS stat lines → {dump: {session_id: fps}}, in log order.

    Keyed on the dump the line was printed inside rather than the timestamp it
    carries — see :func:`_walk_dumps` for why those are not the same thing.
    """
    fps_re = re.compile(session_pattern)
    dumps = {}
    for dump, line in _walk_dumps(log_lines):
        m = fps_re.search(line)
        if m:
            dumps.setdefault(dump, {})[int(m.group(2))] = float(m.group(3))
    return dumps


def companion_steady_window(log_lines, num_sessions, direction):
    """Steady-state stat dumps of a companion log → list of :class:`_Dump`.

    The measured host gets its window free with the FPS verdict; the companion
    has no verdict of its own, so it needs this — and it needs one bound the
    measured host does not.  The companion app is killed, not stopped, so it
    frees no session and every dump to the end of its log still names all of
    them.  :func:`_steady_window`'s census bound therefore finds nothing to trim
    and the window runs on through the idle dumps after the far end stopped
    sending, which cost a TX-measured 36-session run 22 % of its companion rate.

    So drop the trailing dumps in which no session is live, plus the one before
    them: that dump contains the moment the far end stopped, so its period is
    partial — the same reason :func:`_steady_window` drops the dump containing
    the first session free.  Only a *trailing* run goes, so an all-zero blip
    mid-window still reaches the rate, and a log that ends mid-traffic is left
    whole.

    The trim is biased slightly high by construction: it cannot distinguish the
    idle tail of a healthy run from a run that genuinely collapsed at the end and
    stayed down, and removes both.  A collapse the far end recovers from is kept
    (it is not trailing), and a total collapse still fails on the measured side's
    own FPS verdict, so the bias costs a rate line rather than a verdict.
    """
    direction = direction.upper()
    if direction not in ("TX", "RX"):
        raise ValueError(f"direction must be TX or RX, got {direction!r}")

    dumps = _fps_dumps(log_lines, _TX_FPS_RE if direction == "TX" else _RX_FPS_RE)
    window, reason = _steady_window(dumps, num_sessions)
    if reason:
        logger.warning(
            f"companion {direction} steady window not found ({reason}); "
            f"not reporting its rate"
        )
        return []

    live_end = len(window)
    while live_end and not any(dumps[window[live_end - 1]].values()):
        live_end -= 1
    if live_end != len(window):
        window = window[: max(live_end - 1, 0)]

    # Same floor the measured side applies, for the same reason: too few samples
    # is the state in which one partial dump decides the number.  Say so rather
    # than publish a port rate averaged over one dump.
    if len(window) < FPS_MIN_STEADY_SAMPLES:
        logger.warning(
            f"companion {direction} steady window is only {len(window)} dump(s), "
            f"need {FPS_MIN_STEADY_SAMPLES}; not reporting its rate"
        )
        return []
    return window


def _monitor_fps_generic(
    log_lines,
    expected_fps,
    num_sessions,
    session_pattern,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    max_drop_pct=0.0,
):
    """Parse log lines, judge the steady window, return (all_ok, count, details).

    The window comes from :func:`_steady_window`; a run whose window holds
    fewer than ``FPS_MIN_STEADY_SAMPLES`` dumps is reported as a failure
    rather than judged on what little is left, since too few samples is
    exactly the state in which one partial dump decides the verdict.

    When *max_drop_pct* > 0, a **trimmed mean** is used: the worst
    ``max_drop_pct`` fraction of per-session FPS samples are discarded
    before computing the average.  This makes the metric resilient to
    short transient NIC/system events that briefly drop all sessions
    to zero without indicating a real capacity problem.
    """
    min_required = expected_fps * fps_tolerance_pct
    dumps = _fps_dumps(log_lines, session_pattern)

    window, reason = _steady_window(dumps, num_sessions)
    if not reason and len(window) < FPS_MIN_STEADY_SAMPLES:
        reason = (
            f"steady window is only {len(window)} dump(s), "
            f"need {FPS_MIN_STEADY_SAMPLES}; raise test_time"
        )
        window = []

    session_fps = {}
    for ts in window:
        for sid, fps_val in dumps[ts].items():
            session_fps.setdefault(sid, []).append(fps_val)

    def _trimmed_mean(hist):
        """Compute mean of *hist*, dropping the worst max_drop_pct fraction."""
        if not hist:
            return 0.0
        if max_drop_pct <= 0 or len(hist) <= 2:
            return sum(hist) / len(hist)
        n_drop = max(1, int(len(hist) * max_drop_pct))
        trimmed = sorted(hist)[n_drop:]  # drop lowest n_drop values
        return sum(trimmed) / len(trimmed) if trimmed else 0.0

    ok_sessions = {
        sid
        for sid, hist in session_fps.items()
        if hist and _trimmed_mean(hist) >= min_required
    }
    details = {
        "successful_count": len(ok_sessions),
        "successful_sessions": sorted(ok_sessions),
        "session_fps_history": session_fps,
        "min_required_fps": min_required,
        "window": window,
        "window_reject_reason": reason,
    }
    return len(ok_sessions) == num_sessions, len(ok_sessions), details


def monitor_tx_fps(
    log_lines,
    expected_fps,
    num_sessions,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    max_drop_pct=0.0,
):
    """Monitor TX FPS from RxTxApp logs."""
    return _monitor_fps_generic(
        log_lines,
        expected_fps,
        num_sessions,
        _TX_FPS_RE,
        fps_tolerance_pct,
        max_drop_pct,
    )


def monitor_rx_fps(
    log_lines,
    expected_fps,
    num_sessions,
    fps_tolerance_pct=FPS_TOLERANCE_PCT,
    max_drop_pct=0.0,
):
    """Monitor RX FPS from RxTxApp logs."""
    return _monitor_fps_generic(
        log_lines,
        expected_fps,
        num_sessions,
        _RX_FPS_RE,
        fps_tolerance_pct,
        max_drop_pct,
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

# Bytes each packet costs the link but no octet counter reports: FCS 4,
# preamble 7, start-of-frame delimiter 1, inter-frame gap 12.
ETH_L1_OVERHEAD_BYTES = 24
# Period the packet counts on a DEV stat line cover; the counters reset each
# dump.  Mirrors MT_STAT_INTERVAL_S_DEFAULT in lib/src/mt_stat.c.
MTL_STAT_INTERVAL_S = 10


def _wire_overhead_mbps(packets):
    """Mb/s of L1 framing overhead for *packets* seen in one stat period."""
    return packets * ETH_L1_OVERHEAD_BYTES * 8 / MTL_STAT_INTERVAL_S / 1e6


def _collect_in_window(log_lines, data_pattern, window):
    """Yield ``(dump, match)`` for data lines printed inside a *window* dump.

    A stat line reports the mean over the period since the previous dump, so a
    dump is usable only if that whole period carried steady traffic.  *window* is
    the set of dumps :func:`_steady_window` already established as such for the
    FPS verdict; anything outside it covers session ramp-up or teardown.
    Teardown matters most on TX: ``st20p_tx_free()`` takes about a second per
    session, so a 36-session app keeps dumping for 36 s after traffic stops --
    one partial period and then zeros, which cost the 36-session winner 30 % of
    its published rate.

    The dump is yielded because a caller cannot tell from the number of matches
    whether every dump was covered: one dump emits one line per port.
    """
    compiled = re.compile(data_pattern)
    wanted = {dump.idx for dump in window}
    for dump, line in _walk_dumps(log_lines):
        if dump.idx not in wanted:
            continue
        m = compiled.search(line)
        if m:
            yield dump, m


def monitor_tx_throughput(log_lines, window):
    """Monitor TX per-session throughput (Mb/s) over the steady window."""
    hist = {}
    for _, m in _collect_in_window(
        log_lines,
        r"TX_VIDEO_SESSION\(\d+,(\d+)\):\s+throughput\s+([\d.]+)\s+Mb/s",
        window,
    ):
        hist.setdefault(int(m.group(1)), []).append(float(m.group(2)))
    return {"session_throughput_history": hist}


def monitor_rx_throughput(log_lines, window):
    """Monitor RX per-session throughput (Mb/s) over the steady window."""
    hist = {}
    for _, m in _collect_in_window(
        log_lines,
        r"RX_VIDEO_SESSION\(\d+,(\d+)\):\s+throughput\s+([\d.]+)\s+Mb/s",
        window,
    ):
        hist.setdefault(int(m.group(1)), []).append(float(m.group(2)))
    return {"session_throughput_history": hist}


def monitor_dev_rate(log_lines, window):
    """Monitor DEV average TX/RX rate over the steady window.

    Returns the L2 rate MTL reports plus the *wire* (L1) rate derived from the
    packet counts on the same lines -- see :data:`ETH_L1_OVERHEAD_BYTES`.  Only
    the wire rate is comparable against a port's nominal speed.
    """
    tx_hist, rx_hist, tx_wire, rx_wire = [], [], [], []
    covered = set()
    for dump, m in _collect_in_window(
        log_lines,
        r"DEV\(\d+\):\s+Avr rate,\s+tx:\s+([\d.]+)\s+Mb/s,\s+rx:\s+([\d.]+)\s+Mb/s"
        r",\s+pkts,\s+tx:\s+(\d+),\s+rx:\s+(\d+)",
        window,
    ):
        l2_tx, l2_rx = float(m.group(1)), float(m.group(2))
        tx_hist.append(l2_tx)
        rx_hist.append(l2_rx)
        tx_wire.append(l2_tx + _wire_overhead_mbps(int(m.group(3))))
        rx_wire.append(l2_rx + _wire_overhead_mbps(int(m.group(4))))
        covered.add(dump.idx)
    # A redundant session dumps one DEV line per port, so the sample count is a
    # multiple of the window; what must hold is that no dump went unmatched.
    if len(covered) != len(window):
        logger.warning(
            f"matched DEV lines for {len(covered)} of {len(window)} steady-window "
            f"dump(s); the port rate covers only those"
        )
    return {
        "tx_rate_history": tx_hist,
        "rx_rate_history": rx_hist,
        "tx_wire_rate_history": tx_wire,
        "rx_wire_rate_history": rx_wire,
    }


# ── Result Display ──────────────────────────────────────────────────────


def display_session_results(
    direction,
    dma_label,
    num_sessions,
    fps,
    fps_details,
    tx_frame_counts,
    rx_frame_counts,
    throughput_details=None,
    dev_rate=None,
    companion_throughput_details=None,
    companion_dev_rate=None,
):
    """Display FPS, frame count, and throughput results for all sessions."""
    ok_ids = fps_details.get("successful_sessions", [])
    # No fallback: the monitor always sets this, and recomputing it here from
    # *fps* is how the bar came to be 58.41 for a 59.94 fps target.
    min_req = fps_details["min_required_fps"]
    window = fps_details.get("window") or []

    logger.info("=" * 80)
    logger.info(
        f"{direction} Results{dma_label}: {len(ok_ids)}/{num_sessions} sessions "
        f"at {fps:.2f} fps (min: {min_req:.2f})"
    )
    if window:
        logger.info(
            f"  Steady window: {len(window)} stat dumps, "
            f"{window[0].ts} … {window[-1].ts}"
        )
    else:
        logger.info(
            "  Steady window: none — "
            f"{fps_details.get('window_reject_reason', 'no FPS data')}"
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
            if not vals:
                continue
            wire = dev_rate.get(f"{name.lower()}_wire_rate_history", [])
            wire_str = f" wire {sum(wire)/len(wire):.2f} Mb/s" if wire else ""
            logger.info(
                f"    DEV {name}: {sum(vals)/len(vals):.2f} Mb/s{wire_str} "
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
            # Parse mpstat on the remote host to avoid transferring megabytes
            # of raw output over a slow SSH/proxy connection.  Only send back
            # the compact summary (one line: "cores_used max_sim samples c1,c2,...").
            awk_script = (
                r"""awk '"""
                r"""$0 ~ /^[[:space:]]*$/ { if (n>0) { samples++; if (n>mx) mx=n; n=0 } next }"""
                r"""{ off=0; if ($2=="AM"||$2=="PM") off=1 }"""
                r"""$(2+off)=="CPU" { next }"""
                r"""$(2+off)+0==$(2+off) && $(2+off)!="all" { """
                r"""  if ($(3+off)+0 >= 100.0) { all[$(2+off)]=1; n++ } """
                r"""}"""
                r"""END { """
                r"""  if (n>0) { samples++; if (n>mx) mx=n }"""
                r"""  c=0; ids=""; for (k in all) { c++; ids=ids (ids?",":" ") k }"""
                r"""  printf "%d %d %d%s\n", c, mx, samples, ids """
                r"""}' """
            )
            r = self.host.connection.execute_command(
                f"{awk_script} {self._log_path} 2>/dev/null || echo '0 0 0 '",
                shell=True,
            )
            parts = (r.stdout or "0 0 0 ").strip().split(None, 3)
            cores_used = int(parts[0]) if len(parts) > 0 else 0
            max_sim = int(parts[1]) if len(parts) > 1 else 0
            samples = int(parts[2]) if len(parts) > 2 else 0
            core_ids = (
                sorted(int(c) for c in parts[3].split(",") if c.strip())
                if len(parts) > 3 and parts[3].strip()
                else []
            )
            result = {
                "cores_used": cores_used,
                "max_cores_simultaneous": max_sim,
                "samples": samples,
                "core_ids": core_ids,
            }
        except Exception:
            result = empty

        try:
            self.host.connection.execute_command(f"rm -f {self._log_path}", shell=True)
        except Exception:
            pass
        return result


def log_cpu_core_results(info):
    """Log CPU core usage in parseable format for the HTML report."""
    logger.info(
        f"[CPU_CORES] cores_used={info['cores_used']} "
        f"max_simultaneous={info['max_cores_simultaneous']} "
        f"samples={info['samples']} "
        f"core_ids={','.join(str(c) for c in info['core_ids'])}"
    )
