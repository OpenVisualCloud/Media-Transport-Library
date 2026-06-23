/**
 * llc_stat — Last-Level Cache (LLC) hit/miss statistics per process via BCC llcstat.
 *
 * Uses hardware performance counters (perf_events) to sample LLC references
 * and misses per CPU/process.  High cache miss rates indicate working-set
 * doesn't fit in cache, excessive cross-NUMA access, or cache pollution from
 * co-tenants — all critical for real-time DPDK/media pipelines.
 *
 * Source: BCC `llcstat` (uses perf_event_open for LLC-loads and LLC-load-misses).
 * Requires: bpfcc-tools package, root/CAP_PERFMON.
 */
import type { ToolResponse, LlcStatData, LlcStatEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function llcStat(params: {
  host?: string;
  duration_sec?: number;
  sample_period?: number;
}): Promise<ToolResponse<LlcStatData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const samplePeriod = params.sample_period ?? 100;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("llcstat");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "LLCSTAT_MISSING",
      "llcstat (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command
  // -c: sample_period (default 100)
  const cmd = `${binary} -c ${samplePeriod} ${duration} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "LLCSTAT_NO_OUTPUT",
      "llcstat produced no output",
      "Ensure root/CAP_PERFMON. Verify hardware performance counters are available (not in a VM without PMU passthrough).",
    );
  }

  try {
    const entries = parseLlcStatOutput(output);

    // Compute summary
    const totalRefs = entries.reduce((s, e) => s + e.references, 0);
    const totalMisses = entries.reduce((s, e) => s + e.misses, 0);
    const overallHitPct = totalRefs > 0
      ? Math.round(((totalRefs - totalMisses) / totalRefs) * 10000) / 100
      : 100;

    const warnings: string[] = [];
    // Warn for processes with very high miss rates
    for (const e of entries) {
      if (e.hit_pct < 70 && e.references > 1000) {
        warnings.push(
          `PID ${e.pid} (${e.comm}): LLC hit rate only ${e.hit_pct}% — ` +
          `working set likely exceeds cache or cross-NUMA access`,
        );
      }
    }

    if (overallHitPct < 80) {
      warnings.push(
        `Overall LLC hit rate ${overallHitPct}% — significant cache pressure system-wide`,
      );
    }

    return okResponse<LlcStatData>({
      duration_sec: duration,
      sample_period: samplePeriod,
      entries,
      summary: {
        total_references: totalRefs,
        total_misses: totalMisses,
        overall_hit_pct: overallHitPct,
      },
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "LLCSTAT_PARSE_ERROR",
      `Failed to parse llcstat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse llcstat output.
 *
 * Format:
 * ```
 * PID      NAME             CPU     REFERENCE          MISS    HIT%
 * 12345    mtl_sch_0        5       1234567            123     99.99%
 * 67890    python3          2       456789             45678   90.01%
 * ```
 */
function parseLlcStatOutput(output: string): LlcStatEntry[] {
  const lines = output.split("\n").filter((l) => l.trim().length > 0);
  const entries: LlcStatEntry[] = [];

  // Find header line
  let dataStart = -1;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (trimmed.startsWith("PID") && trimmed.includes("REFERENCE")) {
      dataStart = i + 1;
      break;
    }
  }
  if (dataStart < 0) return [];

  for (let i = dataStart; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (!trimmed || trimmed.startsWith("Total") || trimmed.startsWith("Detaching")) continue;

    // Split on whitespace
    const parts = trimmed.split(/\s+/);
    if (parts.length < 5) continue;

    const pid = parseInt(parts[0], 10);
    const comm = parts[1];
    const cpu = parseInt(parts[2], 10);
    const references = parseInt(parts[3], 10);
    const misses = parseInt(parts[4], 10);
    let hitPct = 100;
    if (parts[5]) {
      hitPct = parseFloat(parts[5].replace("%", ""));
    } else if (references > 0) {
      hitPct = Math.round(((references - misses) / references) * 10000) / 100;
    }

    if (isNaN(pid) || isNaN(references)) continue;

    entries.push({
      pid,
      comm: comm || "unknown",
      cpu: isNaN(cpu) ? -1 : cpu,
      references,
      misses: isNaN(misses) ? 0 : misses,
      hit_pct: isNaN(hitPct) ? 100 : hitPct,
    });
  }

  // Sort by misses descending (worst cache behavior first)
  entries.sort((a, b) => b.misses - a.misses);
  return entries;
}
