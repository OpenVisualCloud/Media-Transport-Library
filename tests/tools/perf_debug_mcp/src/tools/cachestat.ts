/**
 * cachestat — Page cache hit/miss ratio monitoring.
 *
 * Shows page cache effectiveness per interval: hits, misses, dirty pages,
 * hit ratio, and buffer/cached memory sizes.  A low hit ratio means the
 * working set doesn't fit in RAM and I/O is going to disk (or being
 * read from storage), which can indirectly cause CPU scheduling delays.
 *
 * Output columns: HITS  MISSES  DIRTIES  HITRATIO  BUFFERS_MB  CACHED_MB
 *
 * Source: BCC `cachestat`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, CachestatData, CachestatSample } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function cachestat(params: {
  host?: string;
  duration_sec?: number;
  interval_sec?: number;
}): Promise<ToolResponse<CachestatData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const interval = params.interval_sec ?? 1;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("cachestat");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "CACHESTAT_MISSING",
      "cachestat (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const count = Math.max(1, Math.round(duration / interval));
  const cmd = `${binary} ${interval} ${count} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "CACHESTAT_NO_OUTPUT",
      "cachestat produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Check kernel BPF tracepoint support.",
    );
  }

  try {
    const lines = output.split("\n");
    const samples: CachestatSample[] = [];

    for (const line of lines) {
      const trimmed = line.trim();
      // Data lines: "  340        0        6  100.00%         1090      15318"
      const match = trimmed.match(
        /^\s*(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)%\s+(\d+)\s+(\d+)/
      );
      if (match) {
        samples.push({
          hits: parseInt(match[1], 10),
          misses: parseInt(match[2], 10),
          dirties: parseInt(match[3], 10),
          hit_ratio_pct: parseFloat(match[4]),
          buffers_mb: parseInt(match[5], 10),
          cached_mb: parseInt(match[6], 10),
        });
      }
    }

    const avgHitRatio = samples.length > 0
      ? Math.round((samples.reduce((s, x) => s + x.hit_ratio_pct, 0) / samples.length) * 100) / 100
      : 0;

    const warnings: string[] = [];
    if (avgHitRatio < 90) {
      warnings.push(`Average cache hit ratio ${avgHitRatio}% — significant cache misses, check working set size`);
    }
    if (avgHitRatio < 50) {
      warnings.push(`Average cache hit ratio ${avgHitRatio}% — severe: most reads going to disk`);
    }

    return okResponse<CachestatData>({
      duration_sec: duration,
      interval_sec: interval,
      samples,
      avg_hit_ratio_pct: avgHitRatio,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "CACHESTAT_PARSE_ERROR",
      `Failed to parse cachestat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
