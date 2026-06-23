/**
 * softirq_latency — Per-softirq-type latency distribution via BCC softirqs.
 *
 * Shows how long each softirq type (net_rx, timer, block, tasklet, etc.)
 * takes to execute, as a histogram.  Long softirq handlers steal CPU time
 * from user-space tasks and cause jitter.
 *
 * More detailed than the existing softirq_snapshot tool (which only shows
 * counts from /proc/softirqs).  This traces actual execution times.
 *
 * Source: BCC `softirqs` with -d (distribution/histogram mode).
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, SoftirqSlowerData, SoftirqHistogram, RunqLatencyBucket } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function softirqLatency(params: {
  host?: string;
  duration_sec?: number;
}): Promise<ToolResponse<SoftirqSlowerData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("softirqs");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "SOFTIRQS_MISSING",
      "softirqs (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // -d = distribution (histogram per softirq type)
  const cmd = `${binary} -d ${duration} 1 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "SOFTIRQS_NO_OUTPUT",
      "softirqs produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Check kernel BPF tracepoint support.",
    );
  }

  try {
    const rawHistograms = parseBccHistograms(output);
    const histograms: SoftirqHistogram[] = rawHistograms.map((h) => {
      // Labels from BCC softirqs look like "softirq = net_rx"
      const softirqName = h.label.replace(/^softirq\s*=\s*/, "").trim();
      const multiplier = h.unit === "msecs" ? 1000 : 1;
      return {
        softirq: softirqName,
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
      if (h.p99_usec > 500 && (h.softirq === "net_rx" || h.softirq === "net_tx")) {
        warnings.push(
          `${h.softirq}: p99 latency ${h.p99_usec} µs — network softirq taking >500 µs, may cause jitter`,
        );
      }
      if (h.p99_usec > 10000) {
        warnings.push(
          `${h.softirq}: p99 latency ${h.p99_usec} µs — extremely slow softirq handler (>10 ms)`,
        );
      }
    }

    return okResponse<SoftirqSlowerData>({
      duration_sec: duration,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "SOFTIRQS_PARSE_ERROR",
      `Failed to parse softirqs output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
