/**
 * ipc_analysis — Instructions Per Cycle analysis via perf stat.
 *
 * IPC is the single most important CPU efficiency metric.  As Brendan Gregg's
 * "CPU Utilization is Wrong" explains: high CPU% with low IPC means the CPU
 * is mostly stalled waiting for memory, not actually computing.
 *
 * IPC < 1.0 → memory-stalled (cycles wasted on cache/memory latency)
 * IPC ≥ 1.0 → instruction-bound (CPU is retiring instructions efficiently)
 *
 * Also reports cache miss rate and branch miss rate for deeper classification.
 *
 * Source: `perf stat` hardware counters.
 * Requires: root/CAP_PERFMON, perf, hardware PMU support.
 */
import type { ToolResponse, IpcAnalysisData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function ipcAnalysis(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
  pid?: number;
}): Promise<ToolResponse<IpcAnalysisData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 2;
  const cores = params.cores ?? "0-55";
  const pid = params.pid;
  const meta = await buildMeta("fallback", duration * 1000);

  const events = "instructions,cycles,cache-misses,cache-references,branch-misses,branch-instructions";
  const target = pid !== undefined ? `-p ${pid}` : `-a -C ${cores}`;
  const cmd = `perf stat -e ${events} ${target} -- sleep ${duration} 2>&1`;

  const output = await sshExecSafe(host, cmd, (duration + 15) * 1000);
  if (!output) {
    return errorResponse(meta, "PERF_STAT_FAILED", "perf stat returned no output", "Ensure perf is installed and you have CAP_PERFMON or root.");
  }

  try {
    const get = (name: string): number => {
      // Match: "    123,456      instructions" or "    123,456      cache-misses"
      const m = output.match(new RegExp(`([\\d,]+)\\s+${name}`));
      return m ? parseInt(m[1].replace(/,/g, ""), 10) : 0;
    };

    const instructions = get("instructions");
    const cycles = get("cycles");
    const cacheMisses = get("cache-misses");
    const cacheRefs = get("cache-references");
    const branchMisses = get("branch-misses");
    const branchInstructions = get("branch-instructions");

    if (cycles === 0) {
      return errorResponse(meta, "PERF_NO_DATA", "No cycle counts captured — PMU may not be available", "Check that hardware counters are accessible (not in a VM without PMU passthrough).");
    }

    const ipc = instructions / cycles;
    const cacheMissPct = cacheRefs > 0 ? (cacheMisses / cacheRefs) * 100 : 0;
    const branchMissPct = branchInstructions > 0 ? (branchMisses / branchInstructions) * 100 : 0;

    let classification: IpcAnalysisData["classification"];
    let interpretation: string;

    if (ipc < 1.0) {
      classification = "memory-stalled";
      interpretation = `IPC ${round(ipc)} < 1.0 — CPU is spending most cycles stalled on memory. ` +
        `This means high CPU utilization is misleading: the CPU is waiting, not computing. ` +
        `Investigate: cache misses (${round(cacheMissPct)}%), NUMA placement, working set size.`;
    } else if (ipc < 2.0) {
      classification = "balanced";
      interpretation = `IPC ${round(ipc)} — moderate efficiency. CPU is doing useful work but ` +
        `there's room for improvement. Check cache miss rate (${round(cacheMissPct)}%) and NUMA alignment.`;
    } else {
      classification = "instruction-bound";
      interpretation = `IPC ${round(ipc)} ≥ 2.0 — CPU is retiring instructions efficiently. ` +
        `High CPU utilization here means real computation. Optimization focus should be on algorithmic improvements.`;
    }

    const warnings: string[] = [];
    if (ipc < 0.5) {
      warnings.push(`Severely memory-stalled: IPC ${round(ipc)} < 0.5 — most CPU time is wasted on memory latency`);
    }
    if (cacheMissPct > 50) {
      warnings.push(`Very high cache miss rate: ${round(cacheMissPct)}% — working set likely exceeds cache`);
    }
    if (branchMissPct > 5) {
      warnings.push(`Elevated branch misprediction: ${round(branchMissPct)}% — consider branch hints or data-oriented design`);
    }

    return okResponse<IpcAnalysisData>({
      cores: pid !== undefined ? `pid:${pid}` : cores,
      duration_sec: duration,
      instructions,
      cycles,
      ipc: round(ipc),
      cache_references: cacheRefs,
      cache_misses: cacheMisses,
      cache_miss_pct: round(cacheMissPct),
      branch_instructions: branchInstructions,
      branch_misses: branchMisses,
      branch_miss_pct: round(branchMissPct),
      classification,
      interpretation,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "IPC_PARSE_ERROR", `Failed to parse perf stat output: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function round(n: number): number { return Math.round(n * 100) / 100; }
