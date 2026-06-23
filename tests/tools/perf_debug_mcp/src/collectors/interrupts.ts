/**
 * /proc/interrupts parser — IRQ distribution per CPU.
 */
import { readFile } from "fs/promises";
import { readFileHost, isLocalHost } from "../remote/host-utils.js";

export interface InterruptLine {
  irq: string;
  per_cpu_counts: number[];
  name: string;       // e.g., "IO-APIC  1-edge  i8042"
  device_name: string; // extracted device portion
}

/**
 * Parse /proc/interrupts.
 * Format:
 *            CPU0       CPU1       ...
 *   0:       nnn        nnn   ...   device_name
 *  ...
 */
export async function readProcInterrupts(host?: string): Promise<{ cpu_count: number; lines: InterruptLine[] }> {
  const content = isLocalHost(host)
    ? await readFile("/proc/interrupts", "utf-8")
    : await readFileHost(host, "/proc/interrupts");
  const rawLines = content.split("\n");
  if (rawLines.length < 2) return { cpu_count: 0, lines: [] };

  // First line is CPU header
  const header = rawLines[0].trim().split(/\s+/);
  const cpuCount = header.filter((h) => h.startsWith("CPU")).length;

  const lines: InterruptLine[] = [];

  for (let i = 1; i < rawLines.length; i++) {
    const line = rawLines[i].trim();
    if (!line) continue;

    // Format: "IRQ: count0 count1 ... name"
    const colonIdx = line.indexOf(":");
    if (colonIdx === -1) continue;

    const irq = line.substring(0, colonIdx).trim();
    const rest = line.substring(colonIdx + 1).trim();
    const parts = rest.split(/\s+/);

    // Extract per-CPU counts (first cpuCount numbers)
    const counts: number[] = [];
    let nameStartIdx = 0;
    for (let j = 0; j < cpuCount && j < parts.length; j++) {
      const val = parseInt(parts[j], 10);
      if (!isNaN(val)) {
        counts.push(val);
        nameStartIdx = j + 1;
      } else {
        break;
      }
    }

    // Pad if fewer columns
    while (counts.length < cpuCount) counts.push(0);

    const name = parts.slice(nameStartIdx).join(" ");
    // Extract device name — usually the last meaningful token
    const nameParts = name.trim().split(/\s{2,}/);
    const deviceName = nameParts[nameParts.length - 1] || name;

    lines.push({
      irq,
      per_cpu_counts: counts,
      name: name.trim(),
      device_name: deviceName.trim(),
    });
  }

  return { cpu_count: cpuCount, lines };
}

/**
 * Compute per-IRQ, per-CPU deltas between two snapshots.
 */
export function computeInterruptDeltas(
  before: { cpu_count: number; lines: InterruptLine[] },
  after: { cpu_count: number; lines: InterruptLine[] }
): {
  per_cpu_totals: { cpu: number; irq_total_delta: number }[];
  per_irq: { irq: string; name: string; delta_total: number; per_cpu_deltas: { cpu: number; delta: number }[] }[];
} {
  const afterMap = new Map(after.lines.map((l) => [l.irq, l]));
  const cpuCount = after.cpu_count;

  const perCpuTotals: number[] = new Array(cpuCount).fill(0);
  const perIrq: { irq: string; name: string; delta_total: number; per_cpu_deltas: { cpu: number; delta: number }[] }[] = [];

  for (const bLine of before.lines) {
    const aLine = afterMap.get(bLine.irq);
    if (!aLine) continue;

    let totalDelta = 0;
    const cpuDeltas: { cpu: number; delta: number }[] = [];

    for (let c = 0; c < cpuCount; c++) {
      const delta = (aLine.per_cpu_counts[c] ?? 0) - (bLine.per_cpu_counts[c] ?? 0);
      const d = Math.max(0, delta);
      cpuDeltas.push({ cpu: c, delta: d });
      perCpuTotals[c] += d;
      totalDelta += d;
    }

    if (totalDelta > 0) {
      perIrq.push({
        irq: bLine.irq,
        name: aLine.name || aLine.device_name,
        delta_total: totalDelta,
        per_cpu_deltas: cpuDeltas,
      });
    }
  }

  // Sort IRQs by delta_total descending
  perIrq.sort((a, b) => b.delta_total - a.delta_total);

  return {
    per_cpu_totals: perCpuTotals.map((v, i) => ({ cpu: i, irq_total_delta: v })),
    per_irq: perIrq,
  };
}
