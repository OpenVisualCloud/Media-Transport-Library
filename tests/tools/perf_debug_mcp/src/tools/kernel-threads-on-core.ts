/**
 * kernel_threads_on_core(cpu, window_ms, top_n)
 * Specialized view for kernel threads that commonly interfere.
 */
import type { ToolResponse, KernelThreadsOnCoreData, RunningTask } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { buildTaskTable } from "../collectors/proc-pid-stat.js";
import { getSchedulerInfo } from "../collectors/scheduler.js";
import { isKernelThread, stateToString, round2 } from "../utils/helpers.js";
import { sleep } from "../utils/proc-reader.js";

export async function kernelThreadsOnCore(params: {
  cpu: number;
  window_ms?: number;
  top_n?: number;
  host?: string;
}): Promise<ToolResponse<KernelThreadsOnCoreData>> {
  const { cpu } = params;
  const windowMs = params.window_ms ?? 1000;
  const topN = params.top_n ?? 30;
  const host = params.host;

  const meta = await buildMeta("fallback", windowMs);

  try {
    const HZ = 100;
    const before = await buildTaskTable({ host });
    const beforeMap = new Map<number, { utime: number; stime: number }>();
    for (const t of before) {
      beforeMap.set(t.pid, { utime: t.utime, stime: t.stime });
    }

    await sleep(windowMs);
    const after = await buildTaskTable({ host });

    const windowNs = windowMs * 1_000_000;
    const tasks: RunningTask[] = [];

    for (const t of after) {
      // Must be last seen on this CPU
      if (t.processor !== cpu) continue;
      // Must be a kernel thread
      if (!isKernelThread(t.ppid, t.flags, t.vsize)) continue;

      const prev = beforeMap.get(t.pid);
      let runtimeNs = 0;
      if (prev) {
        const delta = Math.max(0, (t.utime - prev.utime) + (t.stime - prev.stime));
        runtimeNs = Math.round((delta / HZ) * 1e9);
      }

      const schedInfo = getSchedulerInfo(t);
      tasks.push({
        pid: t.pid,
        tid: t.pid,
        comm: t.comm,
        runtime_ns: runtimeNs > 0 ? runtimeNs : undefined,
        runtime_pct: runtimeNs > 0 ? round2((runtimeNs / windowNs) * 100) : undefined,
        last_seen_ms_ago: 0,
        policy: schedInfo.policy,
        rt_priority: t.rt_priority > 0 ? t.rt_priority : undefined,
        state: stateToString(t.state),
      });
    }

    // Sort by runtime desc, then comm name awareness for common kthreads
    tasks.sort((a, b) => {
      const aRt = a.runtime_ns ?? 0;
      const bRt = b.runtime_ns ?? 0;
      if (aRt !== bRt) return bRt - aRt;
      return a.pid - b.pid;
    });

    return okResponse({ cpu, tasks: tasks.slice(0, topN) }, meta);
  } catch (err) {
    return errorResponse(meta, "KERNEL_THREADS_ERROR", `Failed: ${err}`);
  }
}
