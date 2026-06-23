/**
 * /proc/stat parser — per-CPU time counters.
 */
import { readFile } from "fs/promises";
import { readFileHost, isLocalHost } from "../remote/host-utils.js";
import type { ProcStatCpuCounters } from "../types.js";

/**
 * Parse /proc/stat and return per-CPU counters.
 * Each cpu line: cpu<N> user nice system idle iowait irq softirq steal guest guest_nice
 */
export async function readProcStat(host?: string): Promise<ProcStatCpuCounters[]> {
  const content = isLocalHost(host)
    ? await readFile("/proc/stat", "utf-8")
    : await readFileHost(host, "/proc/stat");
  const cpus: ProcStatCpuCounters[] = [];

  for (const line of content.split("\n")) {
    const match = line.match(/^cpu(\d+)\s+(.+)/);
    if (!match) continue;

    const cpuId = parseInt(match[1], 10);
    const fields = match[2].trim().split(/\s+/).map(Number);

    cpus.push({
      cpu: cpuId,
      user: fields[0] ?? 0,
      nice: fields[1] ?? 0,
      system: fields[2] ?? 0,
      idle: fields[3] ?? 0,
      iowait: fields[4] ?? 0,
      irq: fields[5] ?? 0,
      softirq: fields[6] ?? 0,
      steal: fields[7] ?? 0,
      guest: fields[8] ?? 0,
      guest_nice: fields[9] ?? 0,
    });
  }

  return cpus.sort((a, b) => a.cpu - b.cpu);
}

/**
 * Read total context switches from /proc/stat.
 */
export async function readProcStatCtxt(host?: string): Promise<number> {
  const content = isLocalHost(host)
    ? await readFile("/proc/stat", "utf-8")
    : await readFileHost(host, "/proc/stat");
  for (const line of content.split("\n")) {
    if (line.startsWith("ctxt ")) {
      return parseInt(line.split(/\s+/)[1], 10);
    }
  }
  return 0;
}

/**
 * Compute per-CPU delta utilization between two snapshots.
 */
export function computeCpuDeltas(
  before: ProcStatCpuCounters[],
  after: ProcStatCpuCounters[]
): Array<{
  cpu: number;
  util_pct: number;
  user_pct: number;
  system_pct: number;
  irq_pct: number;
  softirq_pct: number;
  iowait_pct: number;
  idle_pct: number;
}> {
  const result: Array<{
    cpu: number;
    util_pct: number;
    user_pct: number;
    system_pct: number;
    irq_pct: number;
    softirq_pct: number;
    iowait_pct: number;
    idle_pct: number;
  }> = [];

  const afterMap = new Map(after.map((c) => [c.cpu, c]));

  for (const b of before) {
    const a = afterMap.get(b.cpu);
    if (!a) continue;

    // user field in /proc/stat includes guest time, subtract it
    const userD = (a.user - b.user) - (a.guest - b.guest);
    const niceD = (a.nice - b.nice) - (a.guest_nice - b.guest_nice);
    const systemD = a.system - b.system;
    const idleD = a.idle - b.idle;
    const iowaitD = a.iowait - b.iowait;
    const irqD = a.irq - b.irq;
    const softirqD = a.softirq - b.softirq;
    const stealD = a.steal - b.steal;

    const total = userD + niceD + systemD + idleD + iowaitD + irqD + softirqD + stealD;
    if (total <= 0) {
      result.push({
        cpu: b.cpu,
        util_pct: 0,
        user_pct: 0,
        system_pct: 0,
        irq_pct: 0,
        softirq_pct: 0,
        iowait_pct: 0,
        idle_pct: 100,
      });
      continue;
    }

    const pct = (v: number) => Math.round((v / total) * 10000) / 100;

    result.push({
      cpu: b.cpu,
      util_pct: pct(total - idleD - iowaitD),
      user_pct: pct(userD + niceD),
      system_pct: pct(systemD),
      irq_pct: pct(irqD),
      softirq_pct: pct(softirqD),
      iowait_pct: pct(iowaitD),
      idle_pct: pct(idleD),
    });
  }

  return result.sort((a, b) => a.cpu - b.cpu);
}
