/**
 * /proc/softirqs parser — per-CPU softirq counters.
 */
import { readFile } from "fs/promises";
import { readFileHost, isLocalHost } from "../remote/host-utils.js";

export interface SoftirqLine {
  name: string;       // e.g., "NET_RX", "TIMER", "SCHED", "RCU"
  per_cpu_counts: number[];
}

/**
 * Parse /proc/softirqs.
 * Format is similar to /proc/interrupts:
 *                    CPU0       CPU1       ...
 *          HI:       nnn        nnn   ...
 *       TIMER:       nnn        nnn   ...
 */
export async function readProcSoftirqs(host?: string): Promise<{ cpu_count: number; lines: SoftirqLine[] }> {
  const content = isLocalHost(host)
    ? await readFile("/proc/softirqs", "utf-8")
    : await readFileHost(host, "/proc/softirqs");
  const rawLines = content.split("\n");
  if (rawLines.length < 2) return { cpu_count: 0, lines: [] };

  // First line is CPU header
  const header = rawLines[0].trim().split(/\s+/);
  const cpuCount = header.filter((h) => h.startsWith("CPU")).length;

  const lines: SoftirqLine[] = [];

  for (let i = 1; i < rawLines.length; i++) {
    const line = rawLines[i].trim();
    if (!line) continue;

    const colonIdx = line.indexOf(":");
    if (colonIdx === -1) continue;

    const name = line.substring(0, colonIdx).trim();
    const rest = line.substring(colonIdx + 1).trim();
    const counts = rest.split(/\s+/).map(Number);

    // Pad if fewer columns
    while (counts.length < cpuCount) counts.push(0);

    lines.push({ name, per_cpu_counts: counts.slice(0, cpuCount) });
  }

  return { cpu_count: cpuCount, lines };
}

/**
 * Compute per-softirq, per-CPU deltas.
 */
export function computeSoftirqDeltas(
  before: { cpu_count: number; lines: SoftirqLine[] },
  after: { cpu_count: number; lines: SoftirqLine[] }
): {
  per_cpu: { cpu: number; total_delta: number; breakdown: Record<string, number> }[];
  hot_cpus: number[];
} {
  const cpuCount = after.cpu_count;
  const afterMap = new Map(after.lines.map((l) => [l.name, l]));

  // Initialize per-cpu accumulators
  const perCpu: { cpu: number; total_delta: number; breakdown: Record<string, number> }[] = [];
  for (let c = 0; c < cpuCount; c++) {
    perCpu.push({ cpu: c, total_delta: 0, breakdown: {} });
  }

  for (const bLine of before.lines) {
    const aLine = afterMap.get(bLine.name);
    if (!aLine) continue;

    for (let c = 0; c < cpuCount; c++) {
      const delta = Math.max(0, (aLine.per_cpu_counts[c] ?? 0) - (bLine.per_cpu_counts[c] ?? 0));
      perCpu[c].total_delta += delta;
      perCpu[c].breakdown[bLine.name] = delta;
    }
  }

  // Find hot CPUs (top quartile by total delta)
  const sorted = [...perCpu].sort((a, b) => b.total_delta - a.total_delta);
  const threshold = sorted.length > 0 ? sorted[0].total_delta * 0.5 : 0;
  const hotCpus = perCpu
    .filter((c) => c.total_delta > 0 && c.total_delta >= threshold)
    .map((c) => c.cpu)
    .sort((a, b) => a - b);

  return { per_cpu: perCpu, hot_cpus: hotCpus };
}
