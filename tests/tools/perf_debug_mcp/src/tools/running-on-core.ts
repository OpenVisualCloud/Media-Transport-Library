/**
 * running_on_core(cpu, window_ms, mode, top_n)
 * Return tasks observed running on a CPU within a window.
 */
import type { ToolResponse, RunningOnCoreData, RunningTask, ModeUsed } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { buildTaskTable } from "../collectors/proc-pid-stat.js";
import { getSchedulerInfo } from "../collectors/scheduler.js";
import { getEbpfBridge } from "../collectors/ebpf-bridge.js";
import { stateToString, round2 } from "../utils/helpers.js";
import { sleep } from "../utils/proc-reader.js";
import { isLocalHost } from "../remote/host-utils.js";

export async function runningOnCore(params: {
  cpu: number;
  window_ms?: number;
  mode?: ModeUsed;
  top_n?: number;
  host?: string;
}): Promise<ToolResponse<RunningOnCoreData>> {
  const { cpu } = params;
  const windowMs = params.window_ms ?? 250;
  const requestedMode = params.mode ?? "auto";
  const topN = params.top_n ?? 50;
  const host = params.host;

  let modeUsed: ModeUsed = "fallback";
  const meta = await buildMeta(modeUsed, windowMs);

  try {
    // Try eBPF mode (eBPF only works locally)
    const bridge = getEbpfBridge();
    if (isLocalHost(host) && (requestedMode === "ebpf" || requestedMode === "auto") && bridge.isEnabled) {
      const snapshot = await bridge.getSchedSnapshot(windowMs);
      if (snapshot) {
        modeUsed = "ebpf";
        meta.mode_used = modeUsed;

        const totalWindowNs = windowMs * 1_000_000;
        const tasks: RunningTask[] = snapshot.sched
          .filter((e) => e.cpu === cpu)
          .map((e) => ({
            pid: e.pid,
            tid: e.tid,
            comm: e.comm,
            runtime_ns: e.runtime_ns,
            runtime_pct: round2((e.runtime_ns / totalWindowNs) * 100),
            switches: e.switches,
          }))
          .sort((a, b) => (b.runtime_ns ?? 0) - (a.runtime_ns ?? 0))
          .slice(0, topN);

        return okResponse({ cpu, tasks }, meta);
      }
    }

    if (requestedMode === "ebpf") {
      return errorResponse(meta, "EBPF_UNAVAILABLE", "eBPF mode requested but not available");
    }

    // Fallback: sample /proc over the window
    // Take two snapshots and look at "processor" field + CPU time deltas
    const HZ = 100; // Linux USER_HZ is typically 100

    const before = await buildTaskTable({ host });
    const beforeMap = new Map<number, { utime: number; stime: number; processor: number }>();
    for (const t of before) {
      beforeMap.set(t.pid, { utime: t.utime, stime: t.stime, processor: t.processor });
    }

    await sleep(windowMs);

    const after = await buildTaskTable({ host });

    // Find tasks that were on this CPU in the after snapshot
    // and compute their CPU time delta
    const tasks: RunningTask[] = [];

    for (const t of after) {
      // Task was last seen on this CPU
      if (t.processor !== cpu) continue;

      const schedInfo = getSchedulerInfo(t);
      const prev = beforeMap.get(t.pid);

      let runtimeDeltaTicks = 0;
      if (prev) {
        const utimeDelta = Math.max(0, t.utime - prev.utime);
        const stimeDelta = Math.max(0, t.stime - prev.stime);
        runtimeDeltaTicks = utimeDelta + stimeDelta;
      }

      // Convert ticks to nanoseconds
      const runtimeNs = Math.round((runtimeDeltaTicks / HZ) * 1e9);
      const windowNs = windowMs * 1_000_000;

      tasks.push({
        pid: t.pid,
        tid: t.pid, // In fallback, we use pid as tid from /proc/<tid>/stat
        comm: t.comm,
        runtime_ns: runtimeNs > 0 ? runtimeNs : undefined,
        runtime_pct: runtimeNs > 0 ? round2((runtimeNs / windowNs) * 100) : undefined,
        last_seen_ms_ago: 0, // just observed
        policy: schedInfo.policy,
        rt_priority: t.rt_priority > 0 ? t.rt_priority : undefined,
        state: stateToString(t.state),
      });
    }

    // Sort by runtime desc, then pid, tid as tie-breakers
    tasks.sort((a, b) => {
      const aRuntime = a.runtime_ns ?? 0;
      const bRuntime = b.runtime_ns ?? 0;
      if (aRuntime !== bRuntime) return bRuntime - aRuntime;
      if (a.pid !== b.pid) return a.pid - b.pid;
      return a.tid - b.tid;
    });

    modeUsed = "fallback";
    meta.mode_used = modeUsed;
    return okResponse({ cpu, tasks: tasks.slice(0, topN) }, meta);
  } catch (err) {
    return errorResponse(meta, "RUNNING_ON_CORE_ERROR", `Failed: ${err}`);
  }
}
