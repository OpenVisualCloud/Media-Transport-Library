/**
 * off_cpu_analysis — Per-task scheduling latency analysis.
 *
 * Answers: "Which tasks are experiencing the worst scheduling latency?"
 *
 * Uses a two-snapshot delta of /proc/<pid>/task/<tid>/schedstat to compute
 * per-thread run-queue wait time, runtime, and context switches over a
 * measured interval.  This works on ALL Linux kernels without requiring
 * `perf sched` (many distro perf builds omit the sched subcommand), BCC,
 * or any special capabilities.
 *
 * /proc/<pid>/task/<tid>/schedstat provides three fields:
 *   field 1: time spent on CPU (ns)
 *   field 2: time spent waiting on a run queue (ns) — this IS scheduling delay
 *   field 3: number of timeslices run on this CPU
 *
 * By snapshotting before and after a sleep, we get exact deltas per-thread.
 *
 * Complementary to offcpu_time (BCC) which shows per-stack off-CPU time.
 *
 * Source: /proc/[pid]/task/[tid]/schedstat, /proc/[pid]/task/[tid]/sched.
 * Requires: readable /proc (no special privileges needed).
 */
import type { ToolResponse, OffCpuAnalysisData, OffCpuTaskSummary } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/**
 * Shell script that:
 *  1. Snapshots schedstat + comm + sched for all tasks in /proc
 *  2. Sleeps for the requested duration
 *  3. Re-snapshots
 *  4. Outputs "before" and "after" blocks separated by a marker
 *
 * Each line: TID RUNTIME_NS WAITTIME_NS SLICES COMM [MAX_WAIT_NS]
 *
 * For max scheduling delay we parse the sched file's
 * se.statistics.wait_max field (nanoseconds) if available.
 */
function buildSnapshotScript(duration: number): string {
  // The script collects all thread scheduling data in two passes.
  // We capture max_wait from /proc/<tid>/sched between snapshots using deltas.
  return `
snapshot() {
  for f in /proc/[0-9]*/task/[0-9]*/schedstat; do
    [ -f "$f" ] || continue
    tid_dir=\${f%/schedstat}
    tid=\${tid_dir##*/}
    comm=$(cat "\${tid_dir}/comm" 2>/dev/null || echo "?")
    read runtime waittime slices < "$f" 2>/dev/null || continue
    # Try to get se.statistics.wait_max from sched (in ns)
    wmax=""
    if [ -f "\${tid_dir}/sched" ]; then
      wmax=$(grep 'wait_max' "\${tid_dir}/sched" 2>/dev/null | awk '{print $3}' | head -1)
    fi
    echo "$tid $runtime $waittime $slices $comm \${wmax:-0}"
  done
}
echo "===BEFORE==="
snapshot
echo "===SLEEP==="
sleep ${duration}
echo "===AFTER==="
snapshot
echo "===DONE==="
`;
}

interface SchedSample {
  tid: number;
  runtime_ns: number;
  waittime_ns: number;
  slices: number;
  comm: string;
  wait_max_ns: number;
}

function parseSnapshot(block: string): Map<number, SchedSample> {
  const map = new Map<number, SchedSample>();
  for (const line of block.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    // TID RUNTIME_NS WAITTIME_NS SLICES COMM WAIT_MAX_NS
    const parts = trimmed.split(/\s+/);
    if (parts.length < 5) continue;
    const tid = parseInt(parts[0], 10);
    if (isNaN(tid)) continue;
    map.set(tid, {
      tid,
      runtime_ns: parseInt(parts[1], 10) || 0,
      waittime_ns: parseInt(parts[2], 10) || 0,
      slices: parseInt(parts[3], 10) || 0,
      comm: parts[4],
      wait_max_ns: parseFloat(parts[5]) || 0,
    });
  }
  return map;
}

export async function offCpuAnalysis(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
  top_n?: number;
}): Promise<ToolResponse<OffCpuAnalysisData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const cores = params.cores ?? "0-55";
  const topN = params.top_n ?? 30;
  const meta = await buildMeta("fallback", duration * 1000);

  const script = buildSnapshotScript(duration);
  const timeoutMs = (duration + 30) * 1000;
  const output = await sshExecSafe(host, script, timeoutMs);

  if (!output || !output.includes("===DONE===")) {
    return errorResponse(
      meta,
      "SCHEDSTAT_FAILED",
      "Failed to collect /proc/*/schedstat snapshots",
      "Ensure /proc/<pid>/task/<tid>/schedstat is readable. Check: cat /proc/self/schedstat",
    );
  }

  try {
    // Split into before/after blocks
    const beforeIdx = output.indexOf("===BEFORE===");
    const sleepIdx = output.indexOf("===SLEEP===");
    const afterIdx = output.indexOf("===AFTER===");
    const doneIdx = output.indexOf("===DONE===");

    if (beforeIdx < 0 || afterIdx < 0) {
      return errorResponse(meta, "SCHEDSTAT_PARSE", "Could not find snapshot markers in output");
    }

    const beforeBlock = output.slice(beforeIdx + 12, sleepIdx >= 0 ? sleepIdx : afterIdx);
    const afterBlock = output.slice(afterIdx + 11, doneIdx);

    const before = parseSnapshot(beforeBlock);
    const after = parseSnapshot(afterBlock);

    // Compute deltas for threads that exist in both snapshots
    const tasks: OffCpuTaskSummary[] = [];

    for (const [tid, aft] of after) {
      const bef = before.get(tid);
      if (!bef) continue; // thread appeared during measurement, skip

      const dRuntime = aft.runtime_ns - bef.runtime_ns;
      const dWait = aft.waittime_ns - bef.waittime_ns;
      const dSlices = aft.slices - bef.slices;

      // Skip threads with no activity during the window
      if (dSlices <= 0 && dRuntime <= 0 && dWait <= 0) continue;

      const runtimeMs = dRuntime / 1_000_000;
      const totalWaitMs = dWait / 1_000_000;
      const switches = dSlices;
      const avgDelayMs = switches > 0 ? totalWaitMs / switches : 0;

      // For max_delay: use the delta of wait_max from /proc/<tid>/sched.
      // If the max increased during our window, the delta is the new worst case.
      // If it didn't increase, we have no per-window max — use avg as lower bound.
      const dWaitMax = aft.wait_max_ns - bef.wait_max_ns;
      const maxDelayMs = dWaitMax > 0
        ? dWaitMax / 1_000_000
        : avgDelayMs;

      tasks.push({
        comm: aft.comm,
        pid: tid,  // we report TID since schedstat is per-thread
        runtime_ms: round(runtimeMs),
        switches,
        avg_delay_ms: round(avgDelayMs),
        max_delay_ms: round(maxDelayMs),
      });
    }

    if (tasks.length === 0) {
      return errorResponse(
        meta,
        "SCHEDSTAT_NO_TASKS",
        "No scheduling activity captured during the measurement window",
        "Verify the target host has active schedulable tasks.",
      );
    }

    // Sort by avg_delay_ms descending (worst average scheduling latency first)
    tasks.sort((a, b) => b.avg_delay_ms - a.avg_delay_ms);
    const topTasks = tasks.slice(0, topN);
    const totalTasks = tasks.length;

    const warnings: string[] = [];
    for (const t of topTasks.slice(0, 5)) {
      if (t.max_delay_ms > 10) {
        warnings.push(`${t.comm}:${t.pid} max scheduling delay ${t.max_delay_ms}ms — may cause latency spikes`);
      }
    }

    const avgMaxDelay = topTasks.reduce((s, t) => s + t.max_delay_ms, 0) / topTasks.length;
    if (avgMaxDelay > 5) {
      warnings.push(`Average max delay across top ${topTasks.length} tasks is ${round(avgMaxDelay)}ms — system may be overcommitted`);
    }

    return okResponse<OffCpuAnalysisData>({
      cores,
      duration_sec: duration,
      tasks: topTasks,
      total_tasks: totalTasks,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "SCHEDSTAT_PARSE_ERROR",
      `Failed to parse schedstat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

function round(n: number): number { return Math.round(n * 1000) / 1000; }
