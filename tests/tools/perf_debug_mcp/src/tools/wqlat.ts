/**
 * wqlat — Kernel workqueue latency measurement via bpftrace.
 *
 * Traces kernel workqueue items and measures the delay from when work is
 * queued to when it begins execution.  High workqueue latency indicates
 * kernel worker thread starvation (often caused by CPU oversubscription
 * on the cores handling kworker threads).
 *
 * Note: The BCC `wqlat` tool is not universally available. This implementation
 * uses bpftrace with the workqueue:workqueue_queue_work and
 * workqueue:workqueue_execute_start tracepoints as a fallback.
 *
 * Source: bpftrace custom script.
 * Requires: bpftrace, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, WqlatData, WqlatEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function wqlat(params: {
  host?: string;
  duration_sec?: number;
}): Promise<ToolResponse<WqlatData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;

  const meta = await buildMeta("fallback", duration * 1000);

  // Check for bpftrace
  const bpfCheck = await sshExecSafe(host, "command -v bpftrace 2>/dev/null", 5_000);
  if (!bpfCheck || !bpfCheck.trim()) {
    return errorResponse(
      meta,
      "BPFTRACE_MISSING",
      "bpftrace not found on target host",
      "Install bpftrace: apt-get install bpftrace",
    );
  }

  // bpftrace script: track workqueue queue→execute latency
  // workqueue:workqueue_queue_work has args: work, workqueue (pointer), function
  // workqueue:workqueue_execute_start has args: work, function
  const script = [
    "tracepoint:workqueue:workqueue_queue_work",
    "{ @start[args->work] = nsecs; }",
    "",
    "tracepoint:workqueue:workqueue_execute_start",
    "/@start[args->work]/",
    "{",
    "  $lat = nsecs - @start[args->work];",
    "  @latency[ksym(args->function)] = stats($lat);",
    "  delete(@start[args->work]);",
    "}",
    "",
    `interval:s:${duration} { exit(); }`,
    "",
    "END { clear(@start); }",
  ].join("\n");

  const cmd = `bpftrace -e '${script}' 2>/dev/null`;
  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);

  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "WQLAT_NO_OUTPUT",
      "wqlat bpftrace script produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Verify workqueue tracepoints exist.",
    );
  }

  try {
    const entries: WqlatEntry[] = [];
    const lines = output.split("\n");

    for (const line of lines) {
      // bpftrace stats output:
      // @latency[func_name]: count N, average M, total P
      const match = line.match(
        /@latency\[([^\]]+)\]:\s+count\s+(\d+),\s+average\s+(\d+),\s+total\s+(\d+)/,
      );
      if (match) {
        const count = parseInt(match[2], 10);
        const avgNs = parseInt(match[3], 10);
        const totalNs = parseInt(match[4], 10);

        entries.push({
          workqueue: match[1],
          count,
          avg_usec: Math.round(avgNs / 1000 * 100) / 100,
          max_usec: 0, // stats() doesn't expose max directly
          total_usec: Math.round(totalNs / 1000),
        });
      }
    }

    // Sort by total time descending
    entries.sort((a, b) => b.total_usec - a.total_usec);

    const warnings: string[] = [];
    for (const e of entries) {
      if (e.avg_usec > 1000) {
        warnings.push(
          `Workqueue ${e.workqueue}: avg latency ${e.avg_usec} µs — kworker threads may be starved`,
        );
      }
    }

    return okResponse<WqlatData>({
      duration_sec: duration,
      entries: entries.slice(0, 50),
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "WQLAT_PARSE_ERROR",
      `Failed to parse wqlat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
