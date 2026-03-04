#!/usr/bin/env python3
"""
Latency watchdog — monitors poc pipeline for sustained latency jumps.

Tails the sender and receiver JSON stat lines, computes a rolling baseline,
and when a jump ≥ JUMP_THRESHOLD_US is sustained for ≥ SUSTAIN_SECONDS,
captures a comprehensive diagnostic snapshot.

Usage:
    python3 latency_monitor.py [--duration-hours 2] [--threshold-us 5000]

Output:
    /dev/shm/poc_logs/latency_monitor.log    — continuous latency log
    /dev/shm/poc_logs/latency_diag_<ts>/     — diagnostic snapshots
"""

import argparse
import datetime
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from collections import deque
from pathlib import Path

# ── Defaults ──
SENDER_LOG    = "/dev/shm/poc_logs/sender.log"
RECEIVER_LOG  = "/dev/shm/poc_logs/receiver.log"
MONITOR_LOG   = "/dev/shm/poc_logs/latency_monitor.log"
DIAG_BASE     = "/dev/shm/poc_logs"
POLL_INTERVAL = 2.0          # seconds between polls
BASELINE_WINDOW = 60         # samples for rolling baseline (~2 minutes)
JUMP_THRESHOLD_US = 5000     # µs jump from baseline to trigger alert
SUSTAIN_SECONDS = 10         # how long the jump must persist before diag
DIAG_COOLDOWN = 300          # seconds between diagnostic captures
MAX_DIAGS = 10               # max diagnostic snapshots to keep

# ── PIDs for the pipeline ──
SYNTH_TX_PATTERN = "synthetic_st20_tx"
SENDER_PATTERN   = "mtl_to_mxl_sender"
RECEIVER_PATTERN = "mxl_sink_receiver"


def ts():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log(msg, fh=None):
    line = f"[{ts()}] {msg}"
    print(line, flush=True)
    if fh:
        fh.write(line + "\n")
        fh.flush()


def parse_last_json(logfile, role):
    """Extract the last JSON stats line for a given role from a log file."""
    try:
        # Read last 20KB of file to find the last JSON line
        with open(logfile, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            read_size = min(size, 20480)
            f.seek(size - read_size)
            data = f.read().decode("utf-8", errors="replace")

        lines = data.strip().split("\n")
        for line in reversed(lines):
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                d = json.loads(line)
                if d.get("role") == role:
                    return d
            except json.JSONDecodeError:
                continue
    except (FileNotFoundError, OSError):
        pass
    return None


def get_pids():
    """Get PIDs of pipeline processes."""
    pids = {}
    try:
        out = subprocess.check_output(
            ["ps", "-eo", "pid,comm,args"], text=True, timeout=5
        )
        for line in out.strip().split("\n"):
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            pid_s, comm, args = parts
            if SYNTH_TX_PATTERN in args and "grep" not in args:
                pids["synth_tx"] = int(pid_s)
            elif SENDER_PATTERN in args and "grep" not in args:
                pids["sender"] = int(pid_s)
            elif RECEIVER_PATTERN in args and "grep" not in args:
                pids["receiver"] = int(pid_s)
    except Exception:
        pass
    return pids


def run_cmd(cmd, timeout=10):
    """Run a shell command, return stdout (capped to 8KB)."""
    try:
        out = subprocess.check_output(
            cmd, shell=True, text=True, timeout=timeout,
            stderr=subprocess.STDOUT
        )
        return out[:8192]
    except subprocess.TimeoutExpired:
        return f"[TIMEOUT after {timeout}s]"
    except Exception as e:
        return f"[ERROR: {e}]"


def capture_diagnostics(diag_dir, baseline, current, pids, fh):
    """Capture comprehensive diagnostic snapshot."""
    os.makedirs(diag_dir, exist_ok=True)
    log(f"DIAG: Capturing diagnostics to {diag_dir}", fh)

    # 1. Summary
    with open(f"{diag_dir}/summary.txt", "w") as f:
        f.write(f"Latency jump detected at {ts()}\n")
        f.write(f"Baseline RDMA avg: {baseline['rdma_avg']:.0f} µs\n")
        f.write(f"Current  RDMA avg: {current.get('rdma_avg', '?')} µs\n")
        f.write(f"Baseline E2E  avg: {baseline['e2e_avg']:.0f} µs\n")
        f.write(f"Current  E2E  avg: {current.get('e2e_avg', '?')} µs\n")
        f.write(f"Jump magnitude: {current.get('rdma_avg', 0) - baseline['rdma_avg']:.0f} µs (RDMA)\n")
        f.write(f"Pipeline PIDs: {pids}\n\n")

    # 2. CPU frequencies
    with open(f"{diag_dir}/cpu_freq.txt", "w") as f:
        f.write("=== CPU frequencies (MHz) ===\n")
        # Only sample pipeline-relevant cores
        cores_of_interest = [0, 1, 2, 3, 4, 56, 57, 112, 113]
        for c in cores_of_interest:
            freq_path = f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq"
            try:
                with open(freq_path) as ff:
                    freq = int(ff.read().strip()) // 1000
                    f.write(f"  CPU{c:3d}: {freq} MHz\n")
            except:
                pass
        f.write("\n=== CPU governor ===\n")
        f.write(run_cmd("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))
        f.write("\n=== intel_pstate ===\n")
        f.write(run_cmd("cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null"))
        f.write(run_cmd("cat /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null"))
        f.write(run_cmd("cat /sys/devices/system/cpu/intel_pstate/min_perf_pct 2>/dev/null"))

    # 3. C-state residency
    with open(f"{diag_dir}/cstates.txt", "w") as f:
        f.write("=== C-state residency (pipeline cores) ===\n")
        for c in [1, 2, 3, 4, 56, 57]:
            f.write(f"\n--- CPU {c} ---\n")
            for state_dir in sorted(Path(f"/sys/devices/system/cpu/cpu{c}/cpuidle").glob("state*")):
                try:
                    name = (state_dir / "name").read_text().strip()
                    usage = (state_dir / "usage").read_text().strip()
                    time_us = (state_dir / "time").read_text().strip()
                    disable = (state_dir / "disable").read_text().strip()
                    f.write(f"  {name}: usage={usage} time={time_us}µs disable={disable}\n")
                except:
                    pass

    # 4. turbostat snapshot (2 second sample)
    with open(f"{diag_dir}/turbostat.txt", "w") as f:
        f.write(run_cmd("turbostat --quiet --interval 2 --num_iterations 1 2>&1 | head -30", timeout=10))

    # 5. /proc/interrupts for NIC IRQs
    with open(f"{diag_dir}/interrupts.txt", "w") as f:
        f.write("=== NIC-related interrupts ===\n")
        f.write(run_cmd("grep -E 'ens1np0|enp171s0np0|irdma|ice' /proc/interrupts | head -30"))
        f.write("\n=== IRQ affinity for ice/irdma ===\n")
        f.write(run_cmd("grep -rl 'ice\\|irdma' /proc/irq/*/actions 2>/dev/null | head -5; "
                        "for irq in $(grep -E 'ice-' /proc/interrupts | awk '{print $1}' | tr -d ':' | head -10); do "
                        "echo \"IRQ $irq: affinity=$(cat /proc/irq/$irq/smp_affinity_list 2>/dev/null)\"; done"))

    # 6. softirqs
    with open(f"{diag_dir}/softirqs.txt", "w") as f:
        f.write(run_cmd("cat /proc/softirqs"))

    # 7. Network stats -- ethtool for both NICs
    with open(f"{diag_dir}/nic_stats.txt", "w") as f:
        for iface in ["ens1np0", "enp171s0np0"]:
            f.write(f"\n=== {iface} ethtool -S (selected) ===\n")
            f.write(run_cmd(f"ethtool -S {iface} 2>/dev/null | grep -E 'tx_bytes|rx_bytes|tx_errors|rx_errors|"
                            f"tx_dropped|rx_dropped|rx_crc|link_down|tx_timeout|rx_over'"))
            f.write(f"\n=== {iface} ethtool -c (coalescing) ===\n")
            f.write(run_cmd(f"ethtool -c {iface} 2>/dev/null"))
            f.write(f"\n=== {iface} ethtool -g (ring) ===\n")
            f.write(run_cmd(f"ethtool -g {iface} 2>/dev/null"))

    # 8. RDMA device stats
    with open(f"{diag_dir}/rdma_stats.txt", "w") as f:
        for dev in ["rocep39s0", "rocep171s0"]:
            f.write(f"\n=== rdma stat {dev} ===\n")
            f.write(run_cmd(f"rdma statistic show link {dev}/1 2>/dev/null"))
        f.write("\n=== rdma resource ===\n")
        f.write(run_cmd("rdma resource show 2>/dev/null"))

    # 9. Process stats (CPU, memory, threads, voluntary/involuntary ctx switches)
    with open(f"{diag_dir}/process_stats.txt", "w") as f:
        for name, pid in pids.items():
            f.write(f"\n=== {name} (PID {pid}) ===\n")
            f.write(run_cmd(f"cat /proc/{pid}/status 2>/dev/null | grep -E "
                            f"'Name|Pid|Threads|VmRSS|VmSize|Cpus_allowed_list|"
                            f"voluntary_ctxt|nonvoluntary_ctxt'"))
            f.write(f"\n--- /proc/{pid}/sched ---\n")
            f.write(run_cmd(f"cat /proc/{pid}/sched 2>/dev/null | head -30"))
            f.write(f"\n--- /proc/{pid}/stat (raw) ---\n")
            f.write(run_cmd(f"cat /proc/{pid}/stat 2>/dev/null"))
            f.write(f"\n--- top -Hp (threads) ---\n")
            f.write(run_cmd(f"top -b -n1 -Hp {pid} 2>/dev/null | head -20"))

    # 10. NUMA memory
    with open(f"{diag_dir}/numa.txt", "w") as f:
        f.write(run_cmd("numastat -m 2>/dev/null"))
        f.write("\n=== numastat for pipeline PIDs ===\n")
        for name, pid in pids.items():
            f.write(f"\n--- {name} (PID {pid}) ---\n")
            f.write(run_cmd(f"numastat -p {pid} 2>/dev/null"))

    # 11. Hugepages
    with open(f"{diag_dir}/hugepages.txt", "w") as f:
        f.write(run_cmd("cat /proc/meminfo | grep -i huge"))
        f.write("\n")
        f.write(run_cmd("cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"))
        f.write(run_cmd("cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages"))

    # 12. dmesg tail (last 50 lines, look for errors)
    with open(f"{diag_dir}/dmesg.txt", "w") as f:
        f.write(run_cmd("dmesg --time-format iso | tail -50"))
        f.write("\n=== ice/irdma errors ===\n")
        f.write(run_cmd("dmesg --time-format iso | grep -iE 'ice|irdma|error|warn|timeout|reset|fault' | tail -30"))

    # 13. Memory pressure / OOM / swap
    with open(f"{diag_dir}/memory.txt", "w") as f:
        f.write(run_cmd("free -h"))
        f.write("\n")
        f.write(run_cmd("cat /proc/pressure/memory 2>/dev/null"))
        f.write("\n")
        f.write(run_cmd("cat /proc/pressure/cpu 2>/dev/null"))
        f.write("\n")
        f.write(run_cmd("cat /proc/pressure/io 2>/dev/null"))

    # 14. PFC / ECN counters
    with open(f"{diag_dir}/pfc_ecn.txt", "w") as f:
        for iface in ["ens1np0", "enp171s0np0"]:
            f.write(f"\n=== {iface} PFC stats ===\n")
            f.write(run_cmd(f"ethtool -S {iface} 2>/dev/null | grep -iE 'pfc|pause|ecn|cnp'"))
            f.write(f"\n=== {iface} DCB ===\n")
            f.write(run_cmd(f"dcb pfc show dev {iface} 2>/dev/null"))
            f.write(run_cmd(f"dcb ets show dev {iface} 2>/dev/null"))

    # 15. Recent sender/receiver log lines
    with open(f"{diag_dir}/pipeline_logs.txt", "w") as f:
        f.write("=== Sender log (last 30 lines) ===\n")
        f.write(run_cmd(f"tail -30 {SENDER_LOG}"))
        f.write("\n=== Receiver log (last 30 lines) ===\n")
        f.write(run_cmd(f"tail -30 {RECEIVER_LOG}"))
        f.write("\n=== SynthTX log (last 20 lines) ===\n")
        f.write(run_cmd("tail -20 /dev/shm/poc_logs/synthtx.log"))

    # 16. Context switches per second (1-second vmstat sample)
    with open(f"{diag_dir}/vmstat.txt", "w") as f:
        f.write(run_cmd("vmstat 1 3", timeout=10))

    # 17. /proc/schedstat for relevant CPUs
    with open(f"{diag_dir}/schedstat.txt", "w") as f:
        f.write(run_cmd("head -10 /proc/schedstat 2>/dev/null"))

    # 18. Two successive latency samples 2s apart (to see if it's still elevated)
    with open(f"{diag_dir}/latency_samples.txt", "w") as f:
        for i in range(3):
            sender = parse_last_json(SENDER_LOG, "sender")
            receiver = parse_last_json(RECEIVER_LOG, "receiver")
            f.write(f"\n--- Sample {i} (t+{i*2}s) ---\n")
            if sender:
                f.write(f"  RDMA: avg={sender['lat_rdma_us']['avg']} "
                        f"min={sender['lat_rdma_us']['min']} "
                        f"max={sender['lat_rdma_us']['max']}\n")
                f.write(f"  Total: avg={sender['lat_total_us']['avg']} "
                        f"max={sender['lat_total_us']['max']}\n")
                f.write(f"  Queue: avg={sender['lat_queue_us']['avg']} "
                        f"max={sender['lat_queue_us']['max']}\n")
                f.write(f"  Bridge: avg={sender['lat_bridge_us']['avg']} "
                        f"max={sender['lat_bridge_us']['max']}\n")
                f.write(f"  Drops: {sender['rx_drops']}\n")
            if receiver:
                f.write(f"  E2E: avg={receiver['lat_e2e_us']['avg']} "
                        f"max={receiver['lat_e2e_us']['max']}\n")
                f.write(f"  Consume: avg={receiver['lat_consume_us']['avg']} "
                        f"max={receiver['lat_consume_us']['max']}\n")
            if i < 2:
                time.sleep(2)

    log(f"DIAG: Snapshot complete → {diag_dir}", fh)
    return diag_dir


def main():
    parser = argparse.ArgumentParser(description="Latency watchdog for poc pipeline")
    parser.add_argument("--duration-hours", type=float, default=2.0,
                        help="How long to monitor (hours, default: 2)")
    parser.add_argument("--threshold-us", type=int, default=JUMP_THRESHOLD_US,
                        help=f"Jump threshold in µs (default: {JUMP_THRESHOLD_US})")
    parser.add_argument("--sustain-sec", type=int, default=SUSTAIN_SECONDS,
                        help=f"Sustain duration before diag (default: {SUSTAIN_SECONDS})")
    parser.add_argument("--poll", type=float, default=POLL_INTERVAL,
                        help=f"Poll interval in seconds (default: {POLL_INTERVAL})")
    args = parser.parse_args()

    threshold = args.threshold_us
    sustain = args.sustain_sec
    duration = args.duration_hours * 3600
    poll = args.poll

    fh = open(MONITOR_LOG, "a")
    log(f"=== Latency monitor started (threshold={threshold}µs, sustain={sustain}s, "
        f"duration={args.duration_hours}h) ===", fh)

    # Rolling baselines
    rdma_history = deque(maxlen=BASELINE_WINDOW)
    e2e_history  = deque(maxlen=BASELINE_WINDOW)
    total_history = deque(maxlen=BASELINE_WINDOW)
    queue_history = deque(maxlen=BASELINE_WINDOW)

    # State
    jump_detected_at = None   # timestamp when jump first seen
    last_diag_time = 0
    diag_count = 0
    start_time = time.time()
    sample_count = 0
    last_sender_time_s = 0
    last_receiver_time_s = 0

    # Baseline lock: once we have enough samples, lock the initial baseline
    baseline_locked = False
    locked_baseline = {}

    running = True
    def sighandler(sig, frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGINT, sighandler)
    signal.signal(signal.SIGTERM, sighandler)

    log("Collecting initial baseline (~2 minutes)...", fh)

    while running and (time.time() - start_time) < duration:
        time.sleep(poll)

        sender = parse_last_json(SENDER_LOG, "sender")
        receiver = parse_last_json(RECEIVER_LOG, "receiver")

        if not sender or not receiver:
            continue

        # Skip if we already processed this stat line (dedup by time_s)
        s_time = sender.get("time_s", 0)
        r_time = receiver.get("time_s", 0)
        if s_time == last_sender_time_s and r_time == last_receiver_time_s:
            continue
        last_sender_time_s = s_time
        last_receiver_time_s = r_time

        rdma_avg  = sender["lat_rdma_us"]["avg"]
        rdma_max  = sender["lat_rdma_us"]["max"]
        total_avg = sender["lat_total_us"]["avg"]
        total_max = sender["lat_total_us"]["max"]
        queue_avg = sender["lat_queue_us"]["avg"]
        queue_max = sender["lat_queue_us"]["max"]
        bridge_avg = sender["lat_bridge_us"]["avg"]
        e2e_avg   = receiver["lat_e2e_us"]["avg"]
        e2e_max   = receiver["lat_e2e_us"]["max"]
        consume_avg = receiver["lat_consume_us"]["avg"]
        drops     = sender["rx_drops"]

        rdma_history.append(rdma_avg)
        e2e_history.append(e2e_avg)
        total_history.append(total_avg)
        queue_history.append(queue_avg)
        sample_count += 1

        # Compute rolling baseline
        if len(rdma_history) < 5:
            continue

        bl_rdma = sum(rdma_history) / len(rdma_history)
        bl_e2e  = sum(e2e_history) / len(e2e_history)
        bl_total = sum(total_history) / len(total_history)
        bl_queue = sum(queue_history) / len(queue_history)

        # Lock baseline after initial window fills
        if not baseline_locked and len(rdma_history) >= min(30, BASELINE_WINDOW):
            baseline_locked = True
            locked_baseline = {
                "rdma_avg": bl_rdma,
                "e2e_avg": bl_e2e,
                "total_avg": bl_total,
                "queue_avg": bl_queue,
            }
            log(f"BASELINE LOCKED: rdma={bl_rdma:.0f}µs  e2e={bl_e2e:.0f}µs  "
                f"total={bl_total:.0f}µs  queue={bl_queue:.0f}µs", fh)

        # Periodic status (every 30 samples ≈ 1 minute)
        if sample_count % 30 == 0:
            elapsed = (time.time() - start_time) / 60
            bl_ref = locked_baseline if baseline_locked else {"rdma_avg": bl_rdma, "e2e_avg": bl_e2e}
            delta_rdma = rdma_avg - bl_ref["rdma_avg"]
            delta_e2e = e2e_avg - bl_ref["e2e_avg"]
            log(f"STATUS [{elapsed:.0f}m]: rdma={rdma_avg}µs(Δ{delta_rdma:+.0f}) "
                f"e2e={e2e_avg}µs(Δ{delta_e2e:+.0f}) "
                f"total={total_avg} queue={queue_avg} bridge={bridge_avg} "
                f"consume={consume_avg} drops={drops} "
                f"rdma_max={rdma_max} e2e_max={e2e_max}", fh)

        if not baseline_locked:
            continue

        # ── Jump detection ──
        ref = locked_baseline
        delta_rdma = rdma_avg - ref["rdma_avg"]
        delta_e2e  = e2e_avg - ref["e2e_avg"]
        delta_total = total_avg - ref["total_avg"]
        delta_queue = queue_avg - ref["queue_avg"]

        # Check which component jumped
        jumped = False
        jump_component = []
        if delta_rdma >= threshold:
            jumped = True
            jump_component.append(f"RDMA+{delta_rdma:.0f}µs")
        if delta_e2e >= threshold:
            jumped = True
            jump_component.append(f"E2E+{delta_e2e:.0f}µs")
        if delta_total >= threshold:
            jumped = True
            jump_component.append(f"Total+{delta_total:.0f}µs")
        if delta_queue >= threshold:
            jumped = True
            jump_component.append(f"Queue+{delta_queue:.0f}µs")

        # Also detect sustained moderate jumps (e.g., RDMA going from 970 to 2000)
        if rdma_avg > ref["rdma_avg"] * 1.5 and delta_rdma > 500:
            jumped = True
            if not any("RDMA" in c for c in jump_component):
                jump_component.append(f"RDMA×{rdma_avg/ref['rdma_avg']:.1f}")

        if jumped:
            if jump_detected_at is None:
                jump_detected_at = time.time()
                log(f"⚠ JUMP DETECTED: {', '.join(jump_component)} "
                    f"(rdma={rdma_avg} e2e={e2e_avg} vs baseline rdma={ref['rdma_avg']:.0f} "
                    f"e2e={ref['e2e_avg']:.0f})", fh)

            sustained_sec = time.time() - jump_detected_at
            if sustained_sec >= sustain:
                if (time.time() - last_diag_time) >= DIAG_COOLDOWN and diag_count < MAX_DIAGS:
                    log(f"🔴 SUSTAINED JUMP ({sustained_sec:.0f}s): "
                        f"rdma={rdma_avg}µs e2e={e2e_avg}µs — capturing diagnostics...", fh)
                    pids = get_pids()
                    diag_ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                    diag_dir = f"{DIAG_BASE}/latency_diag_{diag_ts}"
                    current = {
                        "rdma_avg": rdma_avg, "rdma_max": rdma_max,
                        "e2e_avg": e2e_avg, "e2e_max": e2e_max,
                        "total_avg": total_avg, "queue_avg": queue_avg,
                        "bridge_avg": bridge_avg, "consume_avg": consume_avg,
                        "drops": drops, "jump_components": jump_component,
                    }
                    capture_diagnostics(diag_dir, ref, current, pids, fh)
                    last_diag_time = time.time()
                    diag_count += 1

                    # After diag, keep monitoring but update baseline to new level
                    # so we detect the NEXT jump from here
                    log(f"Baseline updated to new level: rdma={rdma_avg}µs e2e={e2e_avg}µs", fh)
                    locked_baseline = {
                        "rdma_avg": float(rdma_avg),
                        "e2e_avg": float(e2e_avg),
                        "total_avg": float(total_avg),
                        "queue_avg": float(queue_avg),
                    }
                    rdma_history.clear()
                    e2e_history.clear()
                    total_history.clear()
                    queue_history.clear()
                    jump_detected_at = None
                elif diag_count >= MAX_DIAGS:
                    log(f"Max diagnostics ({MAX_DIAGS}) reached, continuing to monitor only.", fh)
        else:
            if jump_detected_at is not None:
                dur = time.time() - jump_detected_at
                log(f"✓ Jump cleared after {dur:.0f}s (rdma={rdma_avg} e2e={e2e_avg})", fh)
                jump_detected_at = None

    elapsed = (time.time() - start_time) / 3600
    log(f"=== Monitor ended after {elapsed:.1f}h, {sample_count} samples, "
        f"{diag_count} diagnostics captured ===", fh)
    fh.close()


if __name__ == "__main__":
    main()
