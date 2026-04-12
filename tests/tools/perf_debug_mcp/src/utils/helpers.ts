/**
 * General utility helpers.
 */

import type { SchedPolicy } from "../types.js";

/**
 * Convert numeric scheduling policy to string enum.
 */
export function policyToString(policy: number): SchedPolicy {
  switch (policy) {
    case 0: return "SCHED_OTHER";
    case 1: return "SCHED_FIFO";
    case 2: return "SCHED_RR";
    case 3: return "SCHED_BATCH";
    case 4: return "SCHED_ISO";
    case 5: return "SCHED_IDLE";
    case 6: return "SCHED_DEADLINE";
    default: return "UNKNOWN";
  }
}

/**
 * Check if a task is a kernel thread.
 * Kernel threads have ppid=2 (kthreadd) or pid=0, or flags & PF_KTHREAD.
 * We also heuristically check VmSize==0 and comm patterns.
 */
export function isKernelThread(ppid: number, flags: number, vsize: number): boolean {
  // PF_KTHREAD = 0x00200000
  if (flags & 0x00200000) return true;
  // kernel threads have ppid 0 (swapper) or 2 (kthreadd)
  if (ppid === 0 || ppid === 2) return true;
  // no virtual memory -> kernel thread
  if (vsize === 0 && ppid <= 2) return true;
  return false;
}

/**
 * Truncate an array to top_n with deterministic ordering.
 */
export function topN<T>(arr: T[], n: number): T[] {
  return arr.slice(0, n);
}

/**
 * Clamp a number to a range.
 */
export function clamp(val: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, val));
}

/**
 * Round to 2 decimal places.
 */
export function round2(val: number): number {
  return Math.round(val * 100) / 100;
}

/**
 * Parse a core/CPU filter string into a Set of numeric IDs.
 * Accepts:
 *   - Single number: "4"
 *   - Comma-separated: "4,5,6,7,8,9,10,11,12,13"
 *   - Ranges: "4-13"
 *   - Mixed: "4-13,20,30-35"
 * Returns null if the input is null/undefined/empty (meaning "no filter").
 */
export function parseCoreFilter(filter?: string | null): Set<number> | null {
  if (filter === null || filter === undefined || filter.trim() === "") return null;

  const result = new Set<number>();
  const parts = filter.split(",");

  for (const part of parts) {
    const trimmed = part.trim();
    if (!trimmed) continue;

    const rangeParts = trimmed.split("-");
    if (rangeParts.length === 2) {
      const start = parseInt(rangeParts[0], 10);
      const end = parseInt(rangeParts[1], 10);
      if (!isNaN(start) && !isNaN(end) && start <= end) {
        for (let i = start; i <= end; i++) {
          result.add(i);
        }
      }
    } else {
      const num = parseInt(trimmed, 10);
      if (!isNaN(num)) {
        result.add(num);
      }
    }
  }

  return result.size > 0 ? result : null;
}

/**
 * Compute aggregate statistics (mean/stddev/min/max) from an array of numbers.
 * Returns null for empty arrays.
 */
export function computeStats(values: number[]): { mean: number; stddev: number; min: number; max: number } | null {
  if (values.length === 0) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const mean = values.reduce((s, v) => s + v, 0) / values.length;
  const variance = values.reduce((s, v) => s + (v - mean) ** 2, 0) / values.length;
  const stddev = Math.sqrt(variance);
  return {
    mean: Math.round(mean * 100) / 100,
    stddev: Math.round(stddev * 100) / 100,
    min: Math.round(min * 100) / 100,
    max: Math.round(max * 100) / 100,
  };
}

/**
 * Map numeric state character from /proc/<pid>/stat to readable string.
 */
export function stateToString(state: string): string {
  switch (state) {
    case "R": return "running";
    case "S": return "sleeping";
    case "D": return "disk_sleep";
    case "Z": return "zombie";
    case "T": return "stopped";
    case "t": return "tracing_stop";
    case "X": return "dead";
    case "x": return "dead";
    case "K": return "wakekill";
    case "W": return "waking";
    case "P": return "parked";
    case "I": return "idle";
    default: return state;
  }
}
