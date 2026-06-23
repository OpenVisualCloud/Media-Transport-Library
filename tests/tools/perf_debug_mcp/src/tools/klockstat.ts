/**
 * klockstat — Kernel mutex/spinlock contention statistics.
 *
 * Traces kernel lock acquisition and reports the callers with highest
 * spin (contention waiting) and hold times.  Identifies which kernel
 * code paths are causing the most lock contention.
 *
 * Output has two tables:
 *   1. Spin contention — time spent waiting to acquire locks
 *   2. Hold times — time locks are held (long holds cause contention)
 *
 * Source: BCC `klockstat`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, KlockstatData, KlockstatEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

function parseKlockstatTable(lines: string[]): KlockstatEntry[] {
  const entries: KlockstatEntry[] = [];
  for (const line of lines) {
    // Format: b'caller+0xNN'    avg    count   max    total
    const match = line.match(/b'([^']+)'\s+([\d.]+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)/);
    if (match) {
      entries.push({
        caller: match[1],
        avg_ns: parseFloat(match[2]),
        count: parseInt(match[3], 10),
        max_ns: parseFloat(match[4]),
        total_ns: parseFloat(match[5]),
      });
    }
  }
  return entries;
}

export async function klockstat(params: {
  host?: string;
  duration_sec?: number;
  top_n?: number;
  pid?: number;
}): Promise<ToolResponse<KlockstatData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const topN = params.top_n ?? 20;
  const pid = params.pid;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("klockstat");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "KLOCKSTAT_MISSING",
      "klockstat (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = [`-d`, String(duration), `-n`, String(topN)];
  if (pid !== undefined) flags.push(`-p ${pid}`);
  const cmd = `${binary} ${flags.join(" ")} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "KLOCKSTAT_NO_OUTPUT",
      "klockstat produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Needs CONFIG_LOCK_STAT or BPF lock tracing.",
    );
  }

  try {
    const lines = output.split("\n");

    // Split into spin and hold sections
    let inSpin = false;
    let inHold = false;
    const spinLines: string[] = [];
    const holdLines: string[] = [];

    for (const line of lines) {
      if (line.includes("Avg Spin")) {
        inSpin = true;
        inHold = false;
        continue;
      }
      if (line.includes("Avg Hold")) {
        inSpin = false;
        inHold = true;
        continue;
      }
      if (line.trim().startsWith("Tracing") || line.trim().startsWith("Detaching")) continue;

      if (inSpin) spinLines.push(line);
      if (inHold) holdLines.push(line);
    }

    const spinEntries = parseKlockstatTable(spinLines);
    const holdEntries = parseKlockstatTable(holdLines);

    const warnings: string[] = [];
    // Warn on high spin contention
    for (const e of spinEntries) {
      if (e.max_ns > 1_000_000) {
        warnings.push(
          `Lock at ${e.caller}: max spin ${(e.max_ns / 1_000_000).toFixed(1)} ms — severe contention`,
        );
      }
    }
    // Warn on long hold times
    for (const e of holdEntries) {
      if (e.max_ns > 10_000_000) {
        warnings.push(
          `Lock at ${e.caller}: max hold ${(e.max_ns / 1_000_000).toFixed(1)} ms — long critical section`,
        );
      }
    }

    return okResponse<KlockstatData>({
      duration_sec: duration,
      top_n: topN,
      spin_contention: spinEntries,
      hold_times: holdEntries,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "KLOCKSTAT_PARSE_ERROR",
      `Failed to parse klockstat output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
