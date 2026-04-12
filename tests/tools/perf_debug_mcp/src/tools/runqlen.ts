/**
 * runqlen — Run queue length distribution as a histogram.
 *
 * Shows how many tasks are queued on each CPU's run queue, sampled over time.
 * A queue length of 0 means the CPU was idle when sampled; 1 means one task
 * was running (no waiting); 2+ means tasks are waiting.  Sustained qlen > 1
 * indicates CPU saturation.
 *
 * Complementary to runq_latency: runqlat measures wait *time*, runqlen
 * measures queue *depth*.
 *
 * Source: BCC `runqlen`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, RunqLenData, RunqLenHistogram, RunqLatencyBucket } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function runqLen(params: {
  host?: string;
  duration_sec?: number;
  per_cpu?: boolean;
}): Promise<ToolResponse<RunqLenData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const perCpu = params.per_cpu ?? false;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("runqlen");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "RUNQLEN_MISSING",
      "runqlen (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = [];
  if (perCpu) flags.push("-C");
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "RUNQLEN_NO_OUTPUT",
      "runqlen produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Check kernel BPF tracepoint support.",
    );
  }

  try {
    // runqlen uses standard histogram format but unit label is "runqlen" not "usecs"
    // The parseBccHistograms expects usecs/msecs — we'll parse manually for queue lengths
    const histograms: RunqLenHistogram[] = [];
    const blocks = output.split(/\n(?=\s*(?:cpu\s*=|runqlen))/);
    let currentLabel = "all";

    for (const block of blocks) {
      const lines = block.split("\n");
      const buckets: RunqLatencyBucket[] = [];

      for (const line of lines) {
        // Label line like "cpu = 5"
        const labelMatch = line.match(/^\s*(cpu\s*=\s*\d+)/);
        if (labelMatch) {
          currentLabel = labelMatch[1];
          continue;
        }

        // Bucket line: "  0          : 51       |@@@@|"
        const bucketMatch = line.match(/^\s*(\d+)\s*:\s*(\d+)\s*(\|.*\|)?/);
        if (bucketMatch) {
          const val = parseInt(bucketMatch[1], 10);
          buckets.push({
            lo: val,
            hi: val + 1,
            count: parseInt(bucketMatch[2], 10),
          });
        }
      }

      if (buckets.length > 0) {
        const totalSamples = buckets.reduce((s, b) => s + b.count, 0);
        const avgLen = totalSamples > 0
          ? buckets.reduce((s, b) => s + b.count * b.lo, 0) / totalSamples
          : 0;
        const maxLen = buckets.reduce((m, b) => (b.count > 0 ? Math.max(m, b.lo) : m), 0);

        histograms.push({
          label: currentLabel,
          unit: "runqlen",
          total_samples: totalSamples,
          avg_len: Math.round(avgLen * 1000) / 1000,
          max_len: maxLen,
          buckets,
        });
        currentLabel = "all"; // reset for next block
      }
    }

    const warnings: string[] = [];
    for (const h of histograms) {
      if (h.avg_len > 1) {
        warnings.push(`${h.label}: avg run queue length ${h.avg_len} — CPU saturation detected`);
      }
      if (h.max_len >= 4) {
        warnings.push(`${h.label}: max queue depth ${h.max_len} — heavy CPU contention`);
      }
    }

    return okResponse<RunqLenData>({
      duration_sec: duration,
      per_cpu: perCpu,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "RUNQLEN_PARSE_ERROR",
      `Failed to parse runqlen output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
