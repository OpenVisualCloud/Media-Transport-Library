/**
 * cpudist — CPU on-time/off-time distribution per-process via BCC cpudist.
 *
 * Shows a histogram of how long threads run on a CPU before being descheduled
 * (on-CPU mode) or how long they are off-CPU (off-CPU mode).  Complementary
 * to runq_latency: while runqlat shows wait *before* running, cpudist shows
 * the *duration* of each CPU burst.
 *
 * Short on-CPU times suggest frequent preemption or voluntary yields.
 * Very long on-CPU times on isolated cores may be expected (DPDK poll mode).
 *
 * Source: BCC `cpudist` (uses sched_switch tracepoint).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, CpudistData, CpudistHistogram, RunqLatencyBucket } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccHistograms } from "../utils/bcc-parser.js";

export async function cpudist(params: {
  host?: string;
  duration_sec?: number;
  mode?: "on-cpu" | "off-cpu";
  per_process?: boolean;
  pid?: number;
}): Promise<ToolResponse<CpudistData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const mode = params.mode ?? "on-cpu";
  const perProcess = params.per_process ?? false;
  const pid = params.pid;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("cpudist");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "CPUDIST_MISSING",
      "cpudist (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command — capture stderr via 2>&1 so we can include it in diagnostics
  const flags: string[] = [];
  if (mode === "off-cpu") flags.push("-O"); // off-CPU mode
  if (perProcess) flags.push("-P");          // per-process histograms
  if (pid !== undefined) flags.push(`-p ${pid}`);
  // positional: interval count
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>&1`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "CPUDIST_NO_OUTPUT",
      "cpudist produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Check kernel BPF tracepoint support.",
    );
  }

  // Check for BCC error messages in the output
  const firstLine = output.trim().split("\n")[0];
  if (firstLine.toLowerCase().includes("error") || firstLine.toLowerCase().includes("cannot") || firstLine.toLowerCase().includes("failed")) {
    return errorResponse(
      meta,
      "CPUDIST_BCC_ERROR",
      `cpudist reported an error: ${firstLine}`,
      "Check BPF kernel support: ls /sys/kernel/btf/vmlinux && cat /proc/sys/kernel/bpf_jit_enable",
    );
  }

  try {
    const rawHistograms = parseBccHistograms(output);
    const histograms: CpudistHistogram[] = rawHistograms.map((h) => {
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
    // In on-CPU mode, very short bursts indicate excessive preemption
    if (mode === "on-cpu") {
      for (const h of histograms) {
        if (h.p50_usec < 10 && h.total_count > 100) {
          warnings.push(
            `${h.label}: median on-CPU time only ${h.p50_usec} µs — extremely frequent context switches`,
          );
        }
      }
    }

    return okResponse<CpudistData>({
      duration_sec: duration,
      mode,
      per_process: perProcess,
      histograms,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "CPUDIST_PARSE_ERROR",
      `Failed to parse cpudist output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
