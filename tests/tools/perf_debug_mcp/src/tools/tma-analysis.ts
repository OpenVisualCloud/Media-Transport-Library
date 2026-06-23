/**
 * tma_analysis — Intel Top-Down Microarchitecture Analysis via perf stat.
 *
 * TMA categorizes all CPU pipeline slots into four top-level buckets:
 *   - Retiring: useful work (good)
 *   - Bad Speculation: wasted work from mispredictions
 *   - Frontend Bound: instruction fetch/decode starvation
 *   - Backend Bound: execution unit or memory stalls
 *
 * Levels 2 and 3 drill deeper (e.g., Backend → Memory Bound → L3 Bound).
 * This is the gold standard for CPU bottleneck classification on Intel CPUs.
 *
 * Source: `perf stat --topdown` (Linux 5.x+, Intel PMU).
 * Requires: root/CAP_PERFMON, Intel CPU, perf with --topdown support.
 */
import type { ToolResponse, TmaAnalysisData, TmaLevel1, TmaLevel2 } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function tmaAnalysis(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
  level?: number;
  pid?: number;
}): Promise<ToolResponse<TmaAnalysisData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 2;
  const cores = params.cores ?? "0-55";
  const level = Math.min(Math.max(params.level ?? 2, 1), 3);
  const pid = params.pid;
  const meta = await buildMeta("fallback", duration * 1000);

  // Collect all 3 levels if level >= 2 (we need L1 always, plus deeper levels)
  // We collect the max requested level; lower levels are derived from it.
  const target = pid !== undefined ? `-p ${pid}` : `-a -C ${cores}`;
  const cmd = `perf stat --topdown --td-level ${level} ${target} -- sleep ${duration} 2>&1`;

  const output = await sshExecSafe(host, cmd, (duration + 15) * 1000);
  if (!output) {
    return errorResponse(meta, "TMA_FAILED", "perf stat --topdown returned no output", "Ensure perf >= 5.x and Intel CPU with TMA PMU support. Run as root.");
  }

  // Check for errors
  if (output.includes("not supported") || output.includes("Invalid argument")) {
    return errorResponse(meta, "TMA_NOT_SUPPORTED", "TMA --topdown not supported on this system", "Requires Intel CPU and perf with --topdown support (Linux 5.x+).");
  }

  try {
    const metrics = parseTmaOutput(output);

    // Build Level 1 from L1 metrics or by summing L2 subcategories
    const level1: TmaLevel1 = {
      retiring_pct: metrics["tma_retiring"] ??
        ((metrics["tma_light_operations"] ?? 0) + (metrics["tma_heavy_operations"] ?? 0)),
      bad_speculation_pct: metrics["tma_bad_speculation"] ??
        ((metrics["tma_branch_mispredicts"] ?? 0) + (metrics["tma_machine_clears"] ?? 0)),
      frontend_bound_pct: metrics["tma_frontend_bound"] ??
        ((metrics["tma_fetch_latency"] ?? 0) + (metrics["tma_fetch_bandwidth"] ?? 0)),
      backend_bound_pct: metrics["tma_backend_bound"] ??
        ((metrics["tma_memory_bound"] ?? 0) + (metrics["tma_core_bound"] ?? 0)),
    };

    let level2: TmaLevel2 | undefined;
    if (level >= 2) {
      level2 = {
        fetch_latency_pct: metrics["tma_fetch_latency"] ?? 0,
        fetch_bandwidth_pct: metrics["tma_fetch_bandwidth"] ?? 0,
        branch_mispredicts_pct: metrics["tma_branch_mispredicts"] ?? 0,
        machine_clears_pct: metrics["tma_machine_clears"] ?? 0,
        light_operations_pct: metrics["tma_light_operations"] ?? 0,
        heavy_operations_pct: metrics["tma_heavy_operations"] ?? 0,
        memory_bound_pct: metrics["tma_memory_bound"] ?? 0,
        core_bound_pct: metrics["tma_core_bound"] ?? 0,
      };
    }

    let level3: Record<string, number> | undefined;
    if (level >= 3) {
      // L3 metrics are everything not in L1/L2
      const l1l2Keys = new Set([
        "tma_retiring", "tma_bad_speculation", "tma_frontend_bound", "tma_backend_bound",
        "tma_fetch_latency", "tma_fetch_bandwidth", "tma_branch_mispredicts", "tma_machine_clears",
        "tma_light_operations", "tma_heavy_operations", "tma_memory_bound", "tma_core_bound",
      ]);
      level3 = {};
      for (const [k, v] of Object.entries(metrics)) {
        if (!l1l2Keys.has(k)) level3[k] = v;
      }
    }

    // Determine dominant bottleneck
    const l1Entries: [string, number][] = [
      ["Backend Bound", level1.backend_bound_pct],
      ["Frontend Bound", level1.frontend_bound_pct],
      ["Bad Speculation", level1.bad_speculation_pct],
      ["Retiring", level1.retiring_pct],
    ];
    l1Entries.sort((a, b) => b[1] - a[1]);
    const dominant = l1Entries[0][0];

    let interpretation: string;
    if (dominant === "Backend Bound") {
      const memBound = level2?.memory_bound_pct ?? 0;
      const coreBound = level2?.core_bound_pct ?? 0;
      if (memBound > coreBound) {
        interpretation = `Backend Bound (${round(level1.backend_bound_pct)}%), dominated by Memory Bound (${round(memBound)}%). ` +
          `CPU pipeline stalls on data access. Optimize: NUMA placement, cache-friendly access patterns, prefetching.`;
      } else {
        interpretation = `Backend Bound (${round(level1.backend_bound_pct)}%), dominated by Core Bound (${round(coreBound)}%). ` +
          `Execution unit contention or long-latency ops (divide, sqrt). Optimize: vectorization, reduce dependency chains.`;
      }
    } else if (dominant === "Frontend Bound") {
      interpretation = `Frontend Bound (${round(level1.frontend_bound_pct)}%). ` +
        `Instruction fetch/decode is the bottleneck. Optimize: code layout, reduce instruction cache misses, use PGO.`;
    } else if (dominant === "Bad Speculation") {
      interpretation = `Bad Speculation (${round(level1.bad_speculation_pct)}%). ` +
        `Branch misprediction or machine clears wasting pipeline slots. Optimize: branchless algorithms, reduce indirect calls.`;
    } else {
      interpretation = `Retiring (${round(level1.retiring_pct)}%). ` +
        `CPU is primarily doing useful work. This is the ideal state — optimize algorithms to reduce total instructions.`;
    }

    const warnings: string[] = [];
    if (level1.backend_bound_pct > 50) {
      warnings.push(`Backend Bound ${round(level1.backend_bound_pct)}% — majority of pipeline slots wasted on stalls`);
    }
    if (level1.bad_speculation_pct > 20) {
      warnings.push(`Bad Speculation ${round(level1.bad_speculation_pct)}% — significant wasted work from mispredictions`);
    }
    if (level2 && level2.memory_bound_pct > 40) {
      warnings.push(`Memory Bound ${round(level2.memory_bound_pct)}% — data access latency is the primary bottleneck`);
    }

    return okResponse<TmaAnalysisData>({
      cores: pid !== undefined ? `pid:${pid}` : cores,
      duration_sec: duration,
      level,
      level1,
      level2,
      level3,
      dominant_bottleneck: dominant,
      interpretation,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "TMA_PARSE_ERROR", `Failed to parse TMA output: ${err instanceof Error ? err.message : String(err)}`);
  }
}

/**
 * Parse perf stat --topdown output.
 *
 * Format varies by level. Metrics appear as:
 *   %  tma_backend_bound      %  tma_retiring  ...
 *                        67.9                 13.1  ...
 *
 * Headers and values are on separate lines. Headers have "% tma_xxx" pattern.
 */
function parseTmaOutput(output: string): Record<string, number> {
  const result: Record<string, number> = {};
  const lines = output.split("\n");

  // Find the header line with %  tma_xxx patterns
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const headerMatches = [...line.matchAll(/%\s+(tma_\w+)/g)];
    if (headerMatches.length === 0) continue;

    // Next line has the values
    const valLine = lines[i + 1];
    if (!valLine) continue;
    const values = valLine.trim().split(/\s+/).map(parseFloat).filter(v => !isNaN(v));

    for (let j = 0; j < headerMatches.length && j < values.length; j++) {
      result[headerMatches[j][1]] = values[j];
    }
  }

  return result;
}

function round(n: number): number { return Math.round(n * 100) / 100; }
