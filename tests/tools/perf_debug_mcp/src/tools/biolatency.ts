/**
 * biolatency — Block I/O latency distribution as a histogram.
 *
 * Shows how long block I/O requests take (from issue to completion),
 * revealing storage performance issues that can cause CPU scheduling
 * delays via I/O wait.
 *
 * Uses standard BCC histogram format (same as runqlat).
 *
 * Source: BCC `biolatency`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, BiolatencyData, BiolatencyHistogram, RunqLatencyBucket } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function biolatency(params: {
  host?: string;
  duration_sec?: number;
  per_disk?: boolean;
}): Promise<ToolResponse<BiolatencyData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const perDisk = params.per_disk ?? false;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("biolatency");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "BIOLATENCY_MISSING",
      "biolatency (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = [];
  if (perDisk) flags.push("-D");
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "BIOLATENCY_NO_OUTPUT",
      "biolatency produced no output (no block I/O during trace?)",
      "Ensure root/CAP_BPF+CAP_PERFMON. No I/O may have occurred during the trace window.",
    );
  }

  try {
    const rawHistograms = parseBccHistograms(output);
    const histograms: BiolatencyHistogram[] = rawHistograms.map((h) => {
      const multiplier = h.unit === "msecs" ? 1000 : 1;
      return {
        label: h.label,
        unit: h.unit,
        total_count: h.total_count,
        avg_usec: Math.round(h.avg_value * multiplier * 100) / 100,
        p50_usec: Math.round(h.p50 * multiplier * 100) / 100,
        p99_usec: Math.round(h.p99 * multiplier * 100) / 100,
        buckets: h.buckets.map((b): RunqLatencyBucket => ({ lo: b.lo, hi: b.hi, count: b.count })),
      };
    });

    const warnings: string[] = [];
    for (const h of histograms) {
      if (h.p99_usec > 10000) {
        warnings.push(`${h.label}: p99 I/O latency ${h.p99_usec} µs (>10 ms) — slow storage`);
      }
      if (h.p99_usec > 100000) {
        warnings.push(`${h.label}: p99 I/O latency ${h.p99_usec} µs (>100 ms) — storage severely degraded`);
      }
    }

    return okResponse<BiolatencyData>({
      duration_sec: duration,
      per_disk: perDisk,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "BIOLATENCY_PARSE_ERROR",
      `Failed to parse biolatency output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
