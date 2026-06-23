/**
 * critical_sections — Detect long critical sections (preempt/IRQ disabled)
 * via BCC criticalstat.
 *
 * Traces kernel sections where preemption or interrupts are disabled and
 * reports any that exceed a configurable threshold.  Long critical sections
 * are a hidden source of latency spikes — while preemption is disabled,
 * no other task can run on that core, including real-time DPDK threads.
 *
 * Source: BCC `criticalstat` (traces preempt_disable/enable, local_irq_disable/enable).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 * Note: criticalstat requires CONFIG_DEBUG_PREEMPT or CONFIG_PREEMPT_TRACER
 *       to be enabled in the kernel — not available on all systems.
 */
import type { ToolResponse, CriticalSectionsData, CriticalSectionEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function criticalSections(params: {
  host?: string;
  duration_sec?: number;
  threshold_usec?: number;
}): Promise<ToolResponse<CriticalSectionsData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const thresholdUsec = params.threshold_usec ?? 100;

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("criticalstat");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "CRITICALSTAT_MISSING",
      "criticalstat (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools. " +
      "Note: criticalstat requires CONFIG_DEBUG_PREEMPT or CONFIG_PREEMPT_TRACER in the kernel.",
    );
  }
  const binary = bin.trim();

  // Build command
  // -d: duration in seconds,  -p: threshold in µs
  const cmd = `timeout ${duration + 5} ${binary} -d ${duration} -p ${thresholdUsec} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);

  // criticalstat may produce no output if no violations occurred — that's OK
  if (!output || !output.trim()) {
    return okResponse<CriticalSectionsData>({
      duration_sec: duration,
      threshold_usec: thresholdUsec,
      entries: [],
      total_violations: 0,
      warnings: [],
    }, meta);
  }

  try {
    const entries = parseCriticalStatOutput(output);

    const warnings: string[] = [];
    if (entries.length > 10) {
      warnings.push(
        `${entries.length} critical section violations (>${thresholdUsec} µs) detected — ` +
        `frequent long sections with preemption/IRQs disabled`,
      );
    }

    // Check for very long violations
    for (const e of entries.slice(0, 5)) {
      if (e.latency_usec > 1000) {
        warnings.push(
          `Critical section of ${e.latency_usec} µs at ${e.caller} — ` +
          `preemption/IRQs disabled for >1 ms`,
        );
      }
    }

    return okResponse<CriticalSectionsData>({
      duration_sec: duration,
      threshold_usec: thresholdUsec,
      entries,
      total_violations: entries.length,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "CRITICALSTAT_PARSE_ERROR",
      `Failed to parse criticalstat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse criticalstat output.
 *
 * Typical output:
 * ```
 * Setting up eBPF...
 * Tracing critical sections...
 *
 *  TIMESTAMP(us)  DURATION(us)  CALLER
 *  1234567890     150           __do_softirq+0x12
 *                                <- preempt_schedule_notrace+0x34
 *                                <- __do_softirq+0x56
 * ```
 */
function parseCriticalStatOutput(output: string): CriticalSectionEntry[] {
  const lines = output.split("\n");
  const entries: CriticalSectionEntry[] = [];
  let currentEntry: CriticalSectionEntry | null = null;

  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    if (trimmed.startsWith("Setting up") || trimmed.startsWith("Tracing") ||
        trimmed.startsWith("TIMESTAMP") || trimmed.startsWith("Detaching")) continue;

    // Main entry line: "timestamp  duration  caller"
    const mainMatch = trimmed.match(/^(\d+)\s+(\d+)\s+(.+)/);
    if (mainMatch) {
      if (currentEntry) entries.push(currentEntry);
      currentEntry = {
        latency_usec: parseInt(mainMatch[2], 10),
        caller: mainMatch[3].trim(),
        stack: [],
      };
      continue;
    }

    // Stack back-trace line: "<- function+offset"
    const stackMatch = trimmed.match(/^<-\s*(.+)/);
    if (stackMatch && currentEntry) {
      if (!currentEntry.stack) currentEntry.stack = [];
      currentEntry.stack.push(stackMatch[1].trim());
      continue;
    }
  }

  if (currentEntry) entries.push(currentEntry);

  // Sort by latency descending
  entries.sort((a, b) => b.latency_usec - a.latency_usec);
  return entries;
}
