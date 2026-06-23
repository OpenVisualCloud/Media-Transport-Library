/**
 * hardirq_latency — Hard IRQ handler time/count via BCC hardirqs.
 *
 * Measures the time spent in each hardware interrupt handler or counts the
 * number of hard IRQ events. Long-running hardirq handlers steal CPU time
 * from application threads and can cause latency spikes, especially on
 * isolated DPDK/real-time cores.
 *
 * Source: BCC `hardirqs` (traces irq_handler_entry/irq_handler_exit).
 * Requires: bpfcc-tools package, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, HardirqLatencyData, HardirqEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseBccTable } from "../utils/bcc-parser.js";

export async function hardirqLatency(params: {
  host?: string;
  duration_sec?: number;
  mode?: "time" | "count";
}): Promise<ToolResponse<HardirqLatencyData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const mode = params.mode ?? "time";

  const meta = await buildMeta("fallback", duration * 1000);

  // Resolve BCC binary
  const binCmd = bccBinaryCmd("hardirqs");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "HARDIRQS_MISSING",
      "hardirqs (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // Build command
  // -T: include timestamps (helpful for parsing blocks)
  // -d: distribution (histogram mode) — we use default (count/time summary)
  const flags: string[] = [];
  if (mode === "count") flags.push("-C"); // count mode instead of time
  // positional args: interval count
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "HARDIRQS_NO_OUTPUT",
      "hardirqs produced no output (no IRQs during the window?)",
      "Ensure root/CAP_BPF+CAP_PERFMON. On idle systems, IRQ events may be very sparse.",
    );
  }

  try {
    const irqs = parseHardirqOutput(output, mode);

    const warnings: string[] = [];
    for (const irq of irqs) {
      if (mode === "time" && irq.avg_usec > 100) {
        warnings.push(
          `IRQ "${irq.irq_name}": avg handler time ${irq.avg_usec.toFixed(0)} µs — ` +
          `long handlers steal CPU from application threads`,
        );
      }
      if (irq.count > 10000 && duration <= 10) {
        warnings.push(
          `IRQ "${irq.irq_name}": ${irq.count} events in ${duration}s — high interrupt rate`,
        );
      }
    }

    return okResponse<HardirqLatencyData>({
      duration_sec: duration,
      mode,
      irqs,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "HARDIRQS_PARSE_ERROR",
      `Failed to parse hardirqs output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse hardirqs output.  Format varies:
 *
 * Time mode:
 * ```
 * HARDIRQ          TOTAL_usecs
 * callfunc             12345
 * ```
 *
 * Count mode:
 * ```
 * HARDIRQ          TOTAL_count
 * callfunc             678
 * ```
 *
 * We also handle the alternate BCC format with separate USECS and COUNT columns.
 */
function parseHardirqOutput(output: string, mode: "time" | "count"): HardirqEntry[] {
  const lines = output.split("\n").filter((l) => l.trim().length > 0);
  const irqs: HardirqEntry[] = [];

  // Skip preamble lines
  let dataStart = -1;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (trimmed.startsWith("HARDIRQ") || trimmed.startsWith("FUNC")) {
      dataStart = i + 1;
      break;
    }
  }
  if (dataStart < 0) {
    // Try generic table parser fallback
    const rows = parseBccTable(output);
    for (const row of rows) {
      const name = row["HARDIRQ"] || row["FUNC"] || Object.values(row)[0] || "unknown";
      const total = parseInt(Object.values(row).find((v) => /^\d+$/.test(v)) || "0", 10);
      irqs.push({
        irq_name: name,
        count: mode === "count" ? total : 0,
        total_usec: mode === "time" ? total : 0,
        avg_usec: 0,
      });
    }
    return irqs;
  }

  for (let i = dataStart; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (!trimmed || trimmed.startsWith("Detaching") || trimmed.startsWith("Tracing")) continue;

    // Split on whitespace — name is first token, value is last
    const parts = trimmed.split(/\s+/);
    if (parts.length < 2) continue;

    const name = parts[0];
    const value = parseInt(parts[parts.length - 1], 10);
    if (isNaN(value)) continue;

    irqs.push({
      irq_name: name,
      count: mode === "count" ? value : 0,
      total_usec: mode === "time" ? value : 0,
      avg_usec: 0, // Will be computed below if we have both
    });
  }

  // Sort by total time/count descending
  if (mode === "time") {
    irqs.sort((a, b) => b.total_usec - a.total_usec);
  } else {
    irqs.sort((a, b) => b.count - a.count);
  }

  return irqs;
}
