/**
 * Scheduler policy reader.
 * Reads scheduling policy and RT priority from /proc/<pid>/task/<tid>/stat.
 */
import type { SchedPolicy } from "../types.js";
import { policyToString } from "../utils/helpers.js";
import type { ProcTaskStat } from "../types.js";

export interface SchedulerInfo {
  policy: SchedPolicy;
  rt_priority: number;
  nice: number;
}

/**
 * Extract scheduler info from a parsed task stat.
 */
export function getSchedulerInfo(stat: ProcTaskStat): SchedulerInfo {
  return {
    policy: policyToString(stat.policy),
    rt_priority: stat.rt_priority,
    nice: stat.nice,
  };
}
