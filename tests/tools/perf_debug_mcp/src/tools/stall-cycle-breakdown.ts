/**
 * stall_cycle_breakdown — Break down CPU cycles into useful work vs stalls.
 *
 * Shows the truth behind "CPU utilization is wrong": what fraction of cycles
 * are actually retiring instructions vs stalled on L1/L2/L3 cache misses and
 * memory access.  Uses PMU cycle_activity events available on Intel processors.
 *
 * Key insight: if stalls_total is 70% of cycles, then 70% of reported CPU
 * utilization is the CPU waiting, not computing.
 *
 * Source: `perf stat` with cycle_activity.* PMU events.
 * Requires: root/CAP_PERFMON, Intel CPU with cycle_activity events.
 */
import type { ToolResponse, StallCycleBreakdownData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function stallCycleBreakdown(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
  pid?: number;
}): Promise<ToolResponse<StallCycleBreakdownData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 2;
  const cores = params.cores ?? "0-55";
  const pid = params.pid;
  const meta = await buildMeta("fallback", duration * 1000);

  const events = [
    "cycles", "instructions",
    "cycle_activity.stalls_total",
    "cycle_activity.stalls_l1d_miss",
    "cycle_activity.stalls_l2_miss",
    "cycle_activity.stalls_l3_miss",
    "cycle_activity.cycles_mem_any",
  ].join(",");

  const target = pid !== undefined ? `-p ${pid}` : `-a -C ${cores}`;
  const cmd = `perf stat -e ${events} ${target} -- sleep ${duration} 2>&1`;

  const output = await sshExecSafe(host, cmd, (duration + 15) * 1000);
  if (!output) {
    return errorResponse(meta, "PERF_STAT_FAILED", "perf stat returned no output", "Ensure perf is installed and CPU supports cycle_activity events.");
  }

  try {
    const get = (name: string): number => {
      const m = output.match(new RegExp(`([\\d,]+)\\s+${name.replace(/\./g, "\\.")}`));
      return m ? parseInt(m[1].replace(/,/g, ""), 10) : 0;
    };

    const totalCycles = get("cycles");
    const instructions = get("instructions");
    const stallsTotal = get("cycle_activity.stalls_total");
    const stallsL1d = get("cycle_activity.stalls_l1d_miss");
    const stallsL2 = get("cycle_activity.stalls_l2_miss");
    const stallsL3 = get("cycle_activity.stalls_l3_miss");
    const cyclesMemAny = get("cycle_activity.cycles_mem_any");

    if (totalCycles === 0) {
      return errorResponse(meta, "STALL_NO_DATA", "No cycle counts captured", "Check PMU availability and permissions.");
    }

    const ipc = instructions / totalCycles;
    const stallsotalPct = (stallsTotal / totalCycles) * 100;
    const busyCycles = totalCycles - stallsTotal;
    const busyPct = (busyCycles / totalCycles) * 100;
    const memStallPct = (cyclesMemAny / totalCycles) * 100;

    let interpretation: string;
    if (stallsotalPct > 70) {
      interpretation = `Severely stalled: ${round(stallsotalPct)}% of cycles are stalls (only ${round(busyPct)}% doing useful work). ` +
        `Memory accounts for ${round(memStallPct)}% of cycles. This CPU is mostly waiting, not computing.`;
    } else if (stallsotalPct > 40) {
      interpretation = `Moderately stalled: ${round(stallsotalPct)}% of cycles are stalls. ` +
        `${round(busyPct)}% of cycles retire instructions. Memory stall contribution: ${round(memStallPct)}%.`;
    } else {
      interpretation = `Efficient: only ${round(stallsotalPct)}% stalls, ${round(busyPct)}% of cycles doing useful work. ` +
        `CPU is compute-bound, not memory-bound.`;
    }

    const warnings: string[] = [];
    if (stallsotalPct > 50) {
      warnings.push(`${round(stallsotalPct)}% stall cycles — reported CPU utilization is misleading`);
    }
    if (stallsL3 > totalCycles * 0.1) {
      warnings.push(`L3 miss stalls are ${round((stallsL3 / totalCycles) * 100)}% of cycles — data not fitting in LLC`);
    }
    if (memStallPct > 60) {
      warnings.push(`Memory-bound: ${round(memStallPct)}% cycles involve memory access — check NUMA placement and cache partitioning`);
    }

    return okResponse<StallCycleBreakdownData>({
      cores: pid !== undefined ? `pid:${pid}` : cores,
      duration_sec: duration,
      total_cycles: totalCycles,
      instructions,
      ipc: round(ipc),
      stalls_total: stallsTotal,
      stalls_total_pct: round(stallsotalPct),
      stalls_l1d_miss: stallsL1d,
      stalls_l2_miss: stallsL2,
      stalls_l3_miss: stallsL3,
      cycles_mem_any: cyclesMemAny,
      busy_cycles: busyCycles,
      busy_pct: round(busyPct),
      memory_stall_pct: round(memStallPct),
      interpretation,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "STALL_PARSE_ERROR", `Failed to parse stall data: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function round(n: number): number { return Math.round(n * 100) / 100; }
