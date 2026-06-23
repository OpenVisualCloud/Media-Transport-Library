/**
 * funclatency — Function latency histogram via BCC funclatency.
 *
 * Traces a kernel or user-space function and produces a histogram of its
 * execution latency.  Extremely useful for pinpointing slow kernel paths:
 *
 *   - `do_nanosleep` — how long are sleeps really taking?
 *   - `tcp_sendmsg` — per-call send latency
 *   - `mlx5e_xmit` — driver-level transmit latency
 *   - `vfs_read` / `vfs_write` — per-call I/O time
 *
 * Source: BCC `funclatency` (attaches kprobe/kretprobe or uprobe/uretprobe).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, FunclatencyData, FunclatencyHistogram, RunqLatencyBucket } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function funclatency(params: {
  host?: string;
  function_pattern: string;
  duration_sec?: number;
  unit?: "nsecs" | "usecs" | "msecs";
  pid?: number;
}): Promise<ToolResponse<FunclatencyData>> {
  const host = params.host ?? "localhost";
  const functionPattern = params.function_pattern;
  const duration = params.duration_sec ?? 5;
  const unit = params.unit ?? "usecs";
  const pid = params.pid;

  const meta = await buildMeta("fallback", duration * 1000);

  // Validate function pattern (prevent command injection)
  if (!/^[a-zA-Z0-9_.*:]+$/.test(functionPattern)) {
    return errorResponse(
      meta,
      "FUNCLATENCY_INVALID_PATTERN",
      `Invalid function pattern: ${functionPattern}`,
      "Use kernel function names (e.g., 'do_nanosleep', 'tcp_sendmsg') or library:function for user-space.",
    );
  }

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("funclatency");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "FUNCLATENCY_MISSING",
      "funclatency (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command
  const flags: string[] = [];
  if (unit === "nsecs") flags.push("-n");      // nanoseconds
  else if (unit === "msecs") flags.push("-m");  // milliseconds
  else flags.push("-u");                         // microseconds (default)
  if (pid !== undefined) flags.push(`-p ${pid}`);
  flags.push(`-d ${duration}`);                  // duration
  // function pattern is the positional argument
  const cmd = `${binary} ${flags.join(" ")} '${functionPattern}' 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "FUNCLATENCY_NO_OUTPUT",
      `funclatency produced no output for pattern '${functionPattern}'`,
      "Ensure function exists (check /proc/kallsyms for kernel functions). Requires root/CAP_BPF.",
    );
  }

  try {
    const rawHistograms = parseBccHistograms(output);
    const histograms: FunclatencyHistogram[] = rawHistograms.map((h) => ({
      label: h.label,
      unit: unit,
      total_count: h.total_count,
      avg_value: h.avg_value,
      p50: h.p50,
      p99: h.p99,
      buckets: h.buckets.map((b): RunqLatencyBucket => ({ lo: b.lo, hi: b.hi, count: b.count })),
    }));

    const warnings: string[] = [];
    for (const h of histograms) {
      if (h.p99 > 1000 && unit === "usecs") {
        warnings.push(
          `${h.label}: p99 latency ${h.p99} µs for '${functionPattern}' — ` +
          `tail latency exceeds 1 ms`,
        );
      }
    }

    return okResponse<FunclatencyData>({
      function_pattern: functionPattern,
      duration_sec: duration,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "FUNCLATENCY_PARSE_ERROR",
      `Failed to parse funclatency output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
