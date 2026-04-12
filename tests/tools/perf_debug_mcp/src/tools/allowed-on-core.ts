/**
 * allowed_on_core(cpu, scope, include_kernel_threads, limit)
 * Enumerate tasks whose affinity includes the specified CPU.
 */
import type { ToolResponse, AllowedOnCoreData, TaskInfo } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { buildTaskTable, listPids, listTids } from "../collectors/proc-pid-stat.js";
import { readTaskAffinity, isAllowedOnCpu } from "../collectors/affinity.js";
import { getSchedulerInfo } from "../collectors/scheduler.js";
import { isKernelThread, stateToString } from "../utils/helpers.js";
import { cpuListToHexMask } from "../utils/proc-reader.js";

export async function allowedOnCore(params: {
  cpu: number;
  scope?: "threads" | "processes";
  include_kernel_threads?: boolean;
  limit?: number;
  host?: string;
}): Promise<ToolResponse<AllowedOnCoreData>> {
  const { cpu } = params;
  const scope = params.scope ?? "threads";
  const includeKernelThreads = params.include_kernel_threads ?? true;
  const limit = params.limit ?? 5000;
  const host = params.host;

  const meta = await buildMeta("fallback");

  try {
    const tasks: TaskInfo[] = [];

    // Build full task table
    const taskTable = await buildTaskTable({ limit: limit * 2, host });

    // For "processes" scope, we only look at the main thread (tid == pid)
    const candidates = scope === "processes"
      ? taskTable.filter((t) => t.pid === taskTable.find((tt) => tt.pid === t.pid)?.pid)
      : taskTable;

    // Deduplicate by tid
    const seen = new Set<number>();

    for (const stat of candidates) {
      const tid = scope === "processes" ? stat.pid : stat.pid; // tid from stat.pid for task
      // In /proc/<pid>/task/<tid>/stat, the pid field is actually the tid
      const actualTid = stat.pid;
      const actualPid = stat.ppid > 0 ? stat.pid : stat.pid; // We need tgid

      if (seen.has(actualTid)) continue;

      // Check if kernel thread
      const isKthread = isKernelThread(stat.ppid, stat.flags, stat.vsize);
      if (!includeKernelThreads && isKthread) continue;

      // Check affinity
      const affinity = await readTaskAffinity(actualTid, actualTid, host);
      if (!affinity) continue;
      if (!isAllowedOnCpu(affinity, cpu)) continue;

      seen.add(actualTid);

      const schedInfo = getSchedulerInfo(stat);

      const taskInfo: TaskInfo = {
        pid: stat.pid, // For threads, this is the TID from /proc
        tid: actualTid,
        comm: stat.comm,
        affinity_cpus: affinity.cpus_allowed,
        affinity_hexmask: cpuListToHexMask(affinity.cpus_allowed),
        policy: schedInfo.policy,
        rt_priority: schedInfo.rt_priority,
        nice: stat.nice,
        is_kernel_thread: isKthread,
        state: stateToString(stat.state),
      };
      if (affinity.cpuset_cpus) {
        taskInfo.cpuset_cpus = affinity.cpuset_cpus;
      }

      tasks.push(taskInfo);

      if (tasks.length >= limit) break;
    }

    // Sort by policy priority (RT first), then pid, tid
    tasks.sort((a, b) => {
      // RT tasks first
      const aRt = a.policy === "SCHED_FIFO" || a.policy === "SCHED_RR" ? 1 : 0;
      const bRt = b.policy === "SCHED_FIFO" || b.policy === "SCHED_RR" ? 1 : 0;
      if (aRt !== bRt) return bRt - aRt;
      if (a.pid !== b.pid) return a.pid - b.pid;
      return a.tid - b.tid;
    });

    return okResponse({ cpu, task_count: tasks.length, tasks }, meta);
  } catch (err) {
    return errorResponse(meta, "ALLOWED_ON_CORE_ERROR", `Failed: ${err}`);
  }
}
