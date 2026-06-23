/**
 * offwaketime — Combined off-CPU + waker stack analysis.
 *
 * Shows *why* threads were blocked AND *who* woke them up, in a single
 * folded-stack output.  The blocked stack (sleeper) and waker stack are
 * separated by "--" in the folded output.
 *
 * More informative than offcpu_time alone, as you can trace causality:
 * "Thread A blocked in futex_wait, woken by Thread B calling futex_wake
 * from its work-processing path."
 *
 * Source: BCC `offwaketime` (uses sched_switch + wakeup tracepoints).
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, OffWakeTimeData, OffWakeEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseFoldedStacks } from "../utils/bcc-parser.js";

export async function offWakeTime(params: {
  host?: string;
  duration_sec?: number;
  pid?: number;
  min_block_usec?: number;
  top_n?: number;
}): Promise<ToolResponse<OffWakeTimeData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const pid = params.pid;
  const minBlock = params.min_block_usec ?? 1000;
  const topN = params.top_n ?? 20;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("offwaketime");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "OFFWAKETIME_MISSING",
      "offwaketime (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = ["-f"]; // folded output
  if (pid !== undefined) flags.push(`-p ${pid}`);
  if (minBlock > 0) flags.push(`--min-block-time ${minBlock}`);
  const cmd = `${binary} ${flags.join(" ")} ${duration} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "OFFWAKETIME_NO_OUTPUT",
      "offwaketime produced no output (no events above threshold?)",
      "Ensure root/CAP_BPF+CAP_PERFMON. Try lowering min_block_usec.",
    );
  }

  try {
    const allStacks = parseFoldedStacks(output);
    const totalEntries = allStacks.length;

    // Split each stack at "--" separator to get blocked vs waker halves
    const entries: OffWakeEntry[] = allStacks.slice(0, topN).map((s) => {
      const sepIdx = s.frames.indexOf("--");
      if (sepIdx >= 0) {
        return {
          blocked_frames: s.frames.slice(0, sepIdx),
          waker_frames: s.frames.slice(sepIdx + 1),
          total_usec: s.value,
        };
      }
      // No separator — treat entire stack as blocked
      return {
        blocked_frames: s.frames,
        waker_frames: [],
        total_usec: s.value,
      };
    });

    const warnings: string[] = [];
    for (const e of entries) {
      if (e.total_usec > duration * 1_000_000 * 0.5) {
        const top = e.blocked_frames[e.blocked_frames.length - 1] || "unknown";
        warnings.push(
          `Stack blocked at "${top}" spent ${(e.total_usec / 1_000_000).toFixed(1)}s off-CPU (>50% of trace window)`,
        );
      }
    }

    return okResponse<OffWakeTimeData>({
      duration_sec: duration,
      pid_filter: pid,
      min_block_usec: minBlock,
      top_entries: entries,
      total_entries: totalEntries,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "OFFWAKETIME_PARSE_ERROR",
      `Failed to parse offwaketime output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
