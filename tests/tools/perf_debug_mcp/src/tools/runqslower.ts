/**
 * runqslower — Trace individual scheduling delays above a threshold.
 *
 * While runqlat shows a histogram of all scheduling latencies, runqslower
 * shows *each* event above a threshold with task name, PID, and delay.
 * Perfect for finding specific latency spikes that get hidden in histograms.
 *
 * Output columns: TASK PID [PREV_TASK PREV_PID] DELTA_us
 *
 * Source: BCC `runqslower`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, RunqSlowerData, RunqSlowerEvent } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function runqSlower(params: {
  host?: string;
  duration_sec?: number;
  threshold_usec?: number;
  pid?: number;
  show_previous?: boolean;
}): Promise<ToolResponse<RunqSlowerData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const threshold = params.threshold_usec ?? 10000;
  const pid = params.pid;
  const showPrev = params.show_previous ?? true;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("runqslower");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "RUNQSLOWER_MISSING",
      "runqslower (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = [];
  if (showPrev) flags.push("-P");
  if (pid !== undefined) flags.push(`-p ${pid}`);
  // positional: min_us
  const cmd = `timeout ${duration} ${binary} ${flags.join(" ")} ${threshold} 2>/dev/null || true`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);

  // No output is valid — means no events exceeded threshold
  const events: RunqSlowerEvent[] = [];

  if (output && output.trim()) {
    const lines = output.split("\n");

    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith("Tracing") || trimmed.startsWith("TASK")) continue;

      if (showPrev) {
        // Format: TASK       PID   PREV_TASK      PREV_PID   DELTA_us
        const match = trimmed.match(/^(\S+)\s+(\d+)\s+(\S+)\s+(\d+)\s+(\d+)/);
        if (match) {
          events.push({
            task: match[1],
            pid: parseInt(match[2], 10),
            prev_task: match[3],
            prev_pid: parseInt(match[4], 10),
            delta_usec: parseInt(match[5], 10),
          });
        }
      } else {
        // Format: TASK       PID   DELTA_us
        const match = trimmed.match(/^(\S+)\s+(\d+)\s+(\d+)/);
        if (match) {
          events.push({
            task: match[1],
            pid: parseInt(match[2], 10),
            delta_usec: parseInt(match[3], 10),
          });
        }
      }
    }
  }

  // Sort by delay descending
  events.sort((a, b) => b.delta_usec - a.delta_usec);

  const warnings: string[] = [];
  if (events.length === 0) {
    warnings.push(`No run-queue delays exceeded ${threshold} µs in ${duration}s — system is not CPU-starved`);
  }
  const extreme = events.filter((e) => e.delta_usec > 100_000);
  if (extreme.length > 0) {
    warnings.push(
      `${extreme.length} events with >100 ms scheduling delay — severe CPU starvation`,
    );
  }

  return okResponse<RunqSlowerData>({
    duration_sec: duration,
    threshold_usec: threshold,
    events: events.slice(0, 200), // cap to prevent huge responses
    total_events: events.length,
    warnings,
  }, meta);
}
