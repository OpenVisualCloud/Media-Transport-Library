/**
 * runq_latency — Run-queue (scheduler) latency distribution via BCC runqlat.
 *
 * Measures the time tasks spend waiting in the CPU run queue before being
 * scheduled.  High run-queue latency indicates CPU contention — tasks are
 * ready to run but can't get a core.  This is the #1 indicator of CPU
 * oversubscription or misplaced workloads on isolated cores.
 *
 * Source: BCC `runqlat` (uses sched_switch/sched_wakeup tracepoints).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, RunqLatencyData, RunqLatencyHistogram } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function runqLatency(params: {
  host?: string;
  duration_sec?: number;
  per_cpu?: boolean;
  pid?: number;
}): Promise<ToolResponse<RunqLatencyData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const perCpu = params.per_cpu ?? false;
  const pid = params.pid;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("runqlat");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "RUNQLAT_MISSING",
      "runqlat (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command — capture stderr via 2>&1 so we can include it in diagnostics
  const flags: string[] = [];
  if (perCpu) flags.push("--cpus");
  if (pid !== undefined) flags.push(`-p ${pid}`);
  // duration is the positional argument
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>&1`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "RUNQLAT_NO_OUTPUT",
      "runqlat produced no output",
      "Ensure you have root or CAP_BPF+CAP_PERFMON. Check kernel supports BPF tracepoints.",
    );
  }

  // Check for BCC error messages in the output
  const firstLine = output.trim().split("\n")[0];
  if (firstLine.toLowerCase().includes("error") || firstLine.toLowerCase().includes("cannot") || firstLine.toLowerCase().includes("failed")) {
    return errorResponse(
      meta,
      "RUNQLAT_BCC_ERROR",
      `runqlat reported an error: ${firstLine}`,
      "Check BPF kernel support: ls /sys/kernel/btf/vmlinux && cat /proc/sys/kernel/bpf_jit_enable",
    );
  }

  try {
    const rawHistograms = parseBccHistograms(output);
    const histograms: RunqLatencyHistogram[] = rawHistograms.map((h) => {
      const multiplier = h.unit === "msecs" ? 1000 : 1;
      return {
        label: h.label,
        unit: h.unit,
        total_count: h.total_count,
        avg_usec: Math.round(h.avg_value * multiplier * 100) / 100,
        p50_usec: Math.round(h.p50 * multiplier * 100) / 100,
        p99_usec: Math.round(h.p99 * multiplier * 100) / 100,
        buckets: h.buckets.map((b) => ({ lo: b.lo, hi: b.hi, count: b.count })),
      };
    });

    const warnings: string[] = [];
    // Warn if any p99 latency > 100 µs (significant for real-time workloads)
    for (const h of histograms) {
      if (h.p99_usec > 100) {
        warnings.push(`${h.label}: p99 run-queue latency ${h.p99_usec} µs — tasks waited >100 µs for CPU`);
      }
      if (h.p99_usec > 1000) {
        warnings.push(`${h.label}: p99 run-queue latency ${h.p99_usec} µs — severe CPU contention (>1 ms)`);
      }
    }

    return okResponse<RunqLatencyData>({
      duration_sec: duration,
      per_cpu: perCpu,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "RUNQLAT_PARSE_ERROR",
      `Failed to parse runqlat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
