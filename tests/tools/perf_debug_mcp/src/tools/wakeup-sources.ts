/**
 * wakeup_sources — Trace what wakes up blocked threads via BCC wakeuptime.
 *
 * Records kernel stacks at the point where threads are woken up from sleep,
 * attributed by the waker.  Complementary to offcpu_time: while offcputime
 * shows *where* threads sleep, wakeuptime shows *what* wakes them up and
 * how much of the wakeup latency is attributable to specific wakers.
 *
 * Use case: Understanding why real-time threads are being delayed — if the
 * waker is a network IRQ softirq, it means the thread is blocked on I/O
 * completion rather than a lock.
 *
 * Source: BCC `wakeuptime` (traces sched_switch + sched_wakeup with stacks).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, WakeupSourcesData, WakeupStackEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseFoldedStacks } from "../utils/bcc-parser.js";

export async function wakeupSources(params: {
  host?: string;
  duration_sec?: number;
  pid?: number;
  top_n?: number;
}): Promise<ToolResponse<WakeupSourcesData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const pid = params.pid;
  const topN = params.top_n ?? 20;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("wakeuptime");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "WAKEUPTIME_MISSING",
      "wakeuptime (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command — folded output (-f) for easy parsing
  const flags: string[] = ["-f"];
  if (pid !== undefined) flags.push(`-p ${pid}`);
  // positional: duration
  const cmd = `${binary} ${flags.join(" ")} ${duration} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "WAKEUPTIME_NO_OUTPUT",
      "wakeuptime produced no output (no wakeup events?)",
      "Ensure root/CAP_BPF+CAP_PERFMON. Try a longer duration or remove pid filter.",
    );
  }

  try {
    const allStacks = parseFoldedStacks(output);
    const totalCaptured = allStacks.length;
    const topStacks: WakeupStackEntry[] = allStacks.slice(0, topN).map((s) => ({
      frames: s.frames,
      total_usec: s.value,
    }));

    const warnings: string[] = [];
    // Warn if dominant waker is an interrupt-related path
    for (const s of topStacks.slice(0, 3)) {
      const frameStr = s.frames.join(";");
      if (frameStr.includes("irq") || frameStr.includes("softirq")) {
        const topFrame = s.frames[s.frames.length - 1] || "IRQ path";
        warnings.push(
          `Top waker stack involves IRQ/softirq path (${topFrame}) — ` +
          `wakeup latency tied to interrupt processing`,
        );
      }
    }

    return okResponse<WakeupSourcesData>({
      duration_sec: duration,
      pid_filter: pid,
      top_stacks: topStacks,
      total_stacks_captured: totalCaptured,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "WAKEUPTIME_PARSE_ERROR",
      `Failed to parse wakeuptime output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
