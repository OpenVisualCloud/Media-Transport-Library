/**
 * offcpu_time — Off-CPU stack traces showing where threads sleep/block.
 *
 * Records kernel + user stacks when threads are descheduled (blocked on I/O,
 * locks, futex, sleep, etc.) and reports the total off-CPU time per unique
 * stack.  The top stacks reveal exactly what's blocking your threads — far
 * more actionable than "CPU is idle" from /proc/stat.
 *
 * Source: BCC `offcputime` (uses sched_switch tracepoint + stack traces).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, OffcpuTimeData, OffcpuStackEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseFoldedStacks } from "../utils/bcc-parser.js";

export async function offcpuTime(params: {
  host?: string;
  duration_sec?: number;
  pid?: number;
  min_block_usec?: number;
  top_n?: number;
}): Promise<ToolResponse<OffcpuTimeData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const pid = params.pid;
  const minBlock = params.min_block_usec ?? 1000; // 1ms default minimum
  const topN = params.top_n ?? 20;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("offcputime");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "OFFCPUTIME_MISSING",
      "offcputime (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command — folded output (-f) is the easiest to parse
  const flags: string[] = ["-f"]; // folded output
  if (pid !== undefined) flags.push(`-p ${pid}`);
  if (minBlock > 0) flags.push(`--min-block-time ${minBlock}`);
  // duration is positional
  const cmd = `${binary} ${flags.join(" ")} ${duration} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "OFFCPUTIME_NO_OUTPUT",
      "offcputime produced no output (no off-CPU events above threshold?)",
      "Ensure root/CAP_BPF+CAP_PERFMON. Try lowering min_block_usec. Verify target has schedulable threads.",
    );
  }

  try {
    const allStacks = parseFoldedStacks(output);
    const totalCaptured = allStacks.length;
    const topStacks: OffcpuStackEntry[] = allStacks.slice(0, topN).map((s) => ({
      frames: s.frames,
      total_usec: s.value,
    }));

    const warnings: string[] = [];
    // Warn for stacks with very high off-CPU time
    for (const s of topStacks) {
      if (s.total_usec > duration * 1_000_000 * 0.5) {
        const topFrame = s.frames[s.frames.length - 1] || "unknown";
        warnings.push(
          `Stack topped by "${topFrame}" spent ${(s.total_usec / 1_000_000).toFixed(1)}s off-CPU ` +
          `(>${Math.round(50)}% of trace window)`,
        );
      }
    }

    return okResponse<OffcpuTimeData>({
      duration_sec: duration,
      pid_filter: pid,
      min_block_usec: minBlock,
      top_stacks: topStacks,
      total_stacks_captured: totalCaptured,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "OFFCPUTIME_PARSE_ERROR",
      `Failed to parse offcputime output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
