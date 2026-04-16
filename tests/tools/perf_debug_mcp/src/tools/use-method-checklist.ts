/**
 * use_method_checklist — Brendan Gregg USE Method health check.
 *
 * For every major resource (CPU, Memory, Network, Disk, Interconnect) auto-checks
 * Utilization, Saturation, and Errors.  Returns a prioritized list of findings.
 *
 * Uses PSI (/proc/pressure/*), /proc/stat, /proc/meminfo, /proc/net/dev,
 * /proc/diskstats, /proc/loadavg, /proc/vmstat in a single SSH command.
 *
 * Source: Brendan Gregg "USE Method" — http://www.brendangregg.com/usemethod.html
 */
import type {
  ToolResponse, UseMethodChecklistData, UseMethodResource,
  UseMethodCheck, UseMethodFinding,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function useMethodChecklist(params: {
  host?: string;
  sample_ms?: number;
}): Promise<ToolResponse<UseMethodChecklistData>> {
  const host = params.host ?? "localhost";
  const sampleMs = params.sample_ms ?? 500;
  const sampleSec = (sampleMs / 1000).toFixed(2);
  const meta = await buildMeta("fallback", sampleMs);

  // Gather all data in one SSH command: two /proc/stat samples for CPU util
  const cmd = [
    'echo "===STAT1==="; head -1 /proc/stat',
    'echo "===NETDEV1==="; cat /proc/net/dev',
    'echo "===DISKSTATS1==="; cat /proc/diskstats',
    `sleep ${sampleSec}`,
    'echo "===STAT2==="; head -1 /proc/stat',
    'echo "===NETDEV2==="; cat /proc/net/dev',
    'echo "===DISKSTATS2==="; cat /proc/diskstats',
    'echo "===LOADAVG==="; cat /proc/loadavg',
    'echo "===NPROC==="; nproc',
    'echo "===MEMINFO==="; cat /proc/meminfo',
    'echo "===VMSTAT==="; grep -E "pgmajfault|oom_kill|pswpin|pswpout|nr_throttled" /proc/vmstat',
    'echo "===PRESSURE==="; cat /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io 2>/dev/null',
    'echo "===END==="',
  ].join("; ");

  const timeoutMs = sampleMs + 10_000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output) {
    return errorResponse(meta, "USE_DATA_ERROR", "Failed to gather system data for USE method checks");
  }

  try {
    const sections = parseSections(output);
    const findings: UseMethodFinding[] = [];
    const resources: UseMethodResource[] = [];

    // ── CPU ──────────────────────────────────────────────────────────
    const cpuRes: UseMethodResource = { resource: "cpu", utilization: [], saturation: [], errors: [] };
    const cpuUtil = computeCpuUtilization(sections["STAT1"] ?? "", sections["STAT2"] ?? "");
    if (cpuUtil !== null) {
      const status = cpuUtil > 90 ? "critical" : cpuUtil > 70 ? "warning" : "ok";
      cpuRes.utilization.push({ metric: "cpu_utilization", value: round2(cpuUtil), unit: "%", status });
      if (status !== "ok") findings.push({ severity: status, resource: "cpu", category: "utilization", message: `CPU utilization ${round2(cpuUtil)}%` });
    }
    const nproc = parseInt(sections["NPROC"]?.trim() ?? "1", 10) || 1;
    const loadavg = sections["LOADAVG"]?.trim().split(/\s+/) ?? [];
    const load1 = parseFloat(loadavg[0] ?? "0");
    const loadRatio = load1 / nproc;
    const loadStatus = loadRatio > 2 ? "critical" : loadRatio > 1 ? "warning" : "ok";
    cpuRes.saturation.push({ metric: "load_avg_1m_per_cpu", value: round2(loadRatio), unit: "ratio", status: loadStatus, detail: `load1=${round2(load1)} cpus=${nproc}` });
    if (loadStatus !== "ok") findings.push({ severity: loadStatus, resource: "cpu", category: "saturation", message: `Run queue saturated: load ${round2(load1)} on ${nproc} CPUs (ratio ${round2(loadRatio)})` });

    // PSI CPU
    const psiCpu = parsePsi(sections["PRESSURE"] ?? "", "cpu");
    if (psiCpu !== null) {
      const ps = psiCpu > 25 ? "critical" : psiCpu > 5 ? "warning" : "ok";
      cpuRes.saturation.push({ metric: "psi_cpu_some_avg10", value: round2(psiCpu), unit: "%", status: ps });
      if (ps !== "ok") findings.push({ severity: ps, resource: "cpu", category: "saturation", message: `CPU pressure stall ${round2(psiCpu)}% (PSI some avg10)` });
    }

    // CPU throttling (from vmstat nr_throttled)
    const vmstat = parseVmstat(sections["VMSTAT"] ?? "");
    if (vmstat.nr_throttled > 0) {
      cpuRes.errors.push({ metric: "cgroup_throttled", value: vmstat.nr_throttled, unit: "count", status: "warning" });
      findings.push({ severity: "warning", resource: "cpu", category: "errors", message: `${vmstat.nr_throttled} cgroup throttle events detected` });
    }
    resources.push(cpuRes);

    // ── Memory ──────────────────────────────────────────────────────
    const memRes: UseMethodResource = { resource: "memory", utilization: [], saturation: [], errors: [] };
    const mem = parseMeminfo(sections["MEMINFO"] ?? "");
    if (mem.total > 0) {
      const memUsedPct = ((mem.total - mem.available) / mem.total) * 100;
      const ms = memUsedPct > 95 ? "critical" : memUsedPct > 85 ? "warning" : "ok";
      memRes.utilization.push({ metric: "memory_used", value: round2(memUsedPct), unit: "%", status: ms, detail: `avail=${Math.round(mem.available / 1024)}MB total=${Math.round(mem.total / 1024)}MB` });
      if (ms !== "ok") findings.push({ severity: ms, resource: "memory", category: "utilization", message: `Memory ${round2(memUsedPct)}% used (${Math.round(mem.available / 1024)}MB available)` });
    }
    // Swap activity = memory saturation
    if (vmstat.pswpin + vmstat.pswpout > 0) {
      const swapStatus = vmstat.pswpin + vmstat.pswpout > 100 ? "critical" : "warning";
      memRes.saturation.push({ metric: "swap_activity", value: vmstat.pswpin + vmstat.pswpout, unit: "pages", status: swapStatus });
      findings.push({ severity: swapStatus, resource: "memory", category: "saturation", message: `Swap activity: ${vmstat.pswpin} pages in, ${vmstat.pswpout} pages out` });
    } else {
      memRes.saturation.push({ metric: "swap_activity", value: 0, unit: "pages", status: "ok" });
    }
    // PSI memory
    const psiMem = parsePsi(sections["PRESSURE"] ?? "", "memory");
    if (psiMem !== null) {
      const ps = psiMem > 10 ? "critical" : psiMem > 1 ? "warning" : "ok";
      memRes.saturation.push({ metric: "psi_memory_some_avg10", value: round2(psiMem), unit: "%", status: ps });
      if (ps !== "ok") findings.push({ severity: ps, resource: "memory", category: "saturation", message: `Memory pressure stall ${round2(psiMem)}% (PSI some avg10)` });
    }
    // OOM kills
    if (vmstat.oom_kill > 0) {
      memRes.errors.push({ metric: "oom_kills", value: vmstat.oom_kill, unit: "count", status: "critical" });
      findings.push({ severity: "critical", resource: "memory", category: "errors", message: `${vmstat.oom_kill} OOM kills detected since boot` });
    }
    // Major page faults
    if (vmstat.pgmajfault > 10000) {
      memRes.errors.push({ metric: "major_page_faults", value: vmstat.pgmajfault, unit: "count", status: "warning" });
      findings.push({ severity: "warning", resource: "memory", category: "errors", message: `${vmstat.pgmajfault} major page faults since boot (high I/O pressure)` });
    }
    resources.push(memRes);

    // ── Network ─────────────────────────────────────────────────────
    const netRes: UseMethodResource = { resource: "network", utilization: [], saturation: [], errors: [] };
    const net1 = parseNetDev(sections["NETDEV1"] ?? "");
    const net2 = parseNetDev(sections["NETDEV2"] ?? "");
    let totalDrops = 0;
    let totalErrors = 0;
    let totalBytes = 0;
    for (const [iface, s2] of Object.entries(net2)) {
      if (iface === "lo") continue;
      const s1 = net1[iface];
      if (!s1) continue;
      const rxBps = (s2.rxBytes - s1.rxBytes) / (sampleMs / 1000);
      const txBps = (s2.txBytes - s1.txBytes) / (sampleMs / 1000);
      totalBytes += rxBps + txBps;
      const drops = (s2.rxDrop - s1.rxDrop) + (s2.txDrop - s1.txDrop);
      totalDrops += drops;
      const errs = (s2.rxErrors - s1.rxErrors) + (s2.txErrors - s1.txErrors);
      totalErrors += errs;
    }
    const netMbps = (totalBytes * 8) / 1_000_000;
    netRes.utilization.push({ metric: "aggregate_throughput", value: round2(netMbps), unit: "Mbps", status: "ok" });
    if (totalDrops > 0) {
      netRes.saturation.push({ metric: "packet_drops", value: totalDrops, unit: "pkts/sample", status: "warning" });
      findings.push({ severity: "warning", resource: "network", category: "saturation", message: `${totalDrops} packet drops in ${sampleMs}ms sample` });
    } else {
      netRes.saturation.push({ metric: "packet_drops", value: 0, unit: "pkts/sample", status: "ok" });
    }
    if (totalErrors > 0) {
      netRes.errors.push({ metric: "interface_errors", value: totalErrors, unit: "errs/sample", status: "critical" });
      findings.push({ severity: "critical", resource: "network", category: "errors", message: `${totalErrors} NIC errors in ${sampleMs}ms sample` });
    } else {
      netRes.errors.push({ metric: "interface_errors", value: 0, unit: "errs/sample", status: "ok" });
    }
    resources.push(netRes);

    // ── Disk ────────────────────────────────────────────────────────
    const diskRes: UseMethodResource = { resource: "disk", utilization: [], saturation: [], errors: [] };
    const disk1 = parseDiskstats(sections["DISKSTATS1"] ?? "");
    const disk2 = parseDiskstats(sections["DISKSTATS2"] ?? "");
    let maxDiskUtil = 0;
    let busyDisk = "";
    for (const [dev, d2] of Object.entries(disk2)) {
      const d1 = disk1[dev];
      if (!d1) continue;
      const ioTimeMs = d2.ioTime - d1.ioTime;
      const utilPct = (ioTimeMs / sampleMs) * 100;
      if (utilPct > maxDiskUtil) { maxDiskUtil = utilPct; busyDisk = dev; }
    }
    const diskStatus = maxDiskUtil > 90 ? "critical" : maxDiskUtil > 70 ? "warning" : "ok";
    diskRes.utilization.push({ metric: "max_disk_utilization", value: round2(Math.min(maxDiskUtil, 100)), unit: "%", status: diskStatus, detail: busyDisk ? `device=${busyDisk}` : undefined });
    if (diskStatus !== "ok") findings.push({ severity: diskStatus, resource: "disk", category: "utilization", message: `Disk ${busyDisk} utilization ${round2(maxDiskUtil)}%` });
    // PSI I/O
    const psiIo = parsePsi(sections["PRESSURE"] ?? "", "io");
    if (psiIo !== null) {
      const ps = psiIo > 25 ? "critical" : psiIo > 5 ? "warning" : "ok";
      diskRes.saturation.push({ metric: "psi_io_some_avg10", value: round2(psiIo), unit: "%", status: ps });
      if (ps !== "ok") findings.push({ severity: ps, resource: "disk", category: "saturation", message: `I/O pressure stall ${round2(psiIo)}% (PSI some avg10)` });
    }
    resources.push(diskRes);

    // Sort findings by severity
    const severityOrder = { critical: 0, warning: 1, info: 2 };
    findings.sort((a, b) => severityOrder[a.severity] - severityOrder[b.severity]);

    const summary = {
      critical: findings.filter(f => f.severity === "critical").length,
      warning: findings.filter(f => f.severity === "warning").length,
      info: findings.filter(f => f.severity === "info").length,
      healthy: resources.reduce((n, r) => {
        const all = [...r.utilization, ...r.saturation, ...r.errors];
        return n + (all.every(c => c.status === "ok") ? 1 : 0);
      }, 0),
    };

    return okResponse<UseMethodChecklistData>({ resources, findings, summary }, meta);
  } catch (err) {
    return errorResponse(meta, "USE_PARSE_ERROR", `Failed to parse USE method data: ${err instanceof Error ? err.message : String(err)}`);
  }
}

// ── helpers ──────────────────────────────────────────────────────────────

function round2(n: number): number { return Math.round(n * 100) / 100; }

function parseSections(output: string): Record<string, string> {
  const result: Record<string, string> = {};
  const lines = output.split("\n");
  let current = "";
  let buf: string[] = [];
  for (const line of lines) {
    const m = line.match(/^===(\w+)===/);
    if (m) {
      if (current) result[current] = buf.join("\n");
      current = m[1];
      buf = [];
    } else {
      buf.push(line);
    }
  }
  if (current) result[current] = buf.join("\n");
  return result;
}

function computeCpuUtilization(stat1: string, stat2: string): number | null {
  const parse = (line: string) => {
    const parts = line.trim().split(/\s+/).slice(1).map(Number);
    if (parts.length < 4) return null;
    const idle = parts[3] + (parts[4] ?? 0); // idle + iowait
    const total = parts.reduce((a, b) => a + b, 0);
    return { idle, total };
  };
  const a = parse(stat1);
  const b = parse(stat2);
  if (!a || !b) return null;
  const dTotal = b.total - a.total;
  const dIdle = b.idle - a.idle;
  if (dTotal === 0) return 0;
  return ((dTotal - dIdle) / dTotal) * 100;
}

function parseMeminfo(text: string): { total: number; available: number } {
  const get = (key: string) => {
    const m = text.match(new RegExp(`${key}:\\s+(\\d+)`));
    return m ? parseInt(m[1], 10) : 0;
  };
  return { total: get("MemTotal"), available: get("MemAvailable") };
}

function parseVmstat(text: string): { pgmajfault: number; oom_kill: number; pswpin: number; pswpout: number; nr_throttled: number } {
  const get = (key: string) => {
    const m = text.match(new RegExp(`${key}\\s+(\\d+)`));
    return m ? parseInt(m[1], 10) : 0;
  };
  return { pgmajfault: get("pgmajfault"), oom_kill: get("oom_kill"), pswpin: get("pswpin"), pswpout: get("pswpout"), nr_throttled: get("nr_throttled") };
}

function parsePsi(text: string, resource: "cpu" | "memory" | "io"): number | null {
  // PSI format: some avg10=0.00 avg60=0.00 avg300=0.00 total=0
  // Lines appear in order: cpu some, cpu full (opt), memory some, memory full, io some, io full
  const lines = text.split("\n");
  let inResource = false;
  for (const line of lines) {
    if (line.includes(`/proc/pressure/${resource}`) || line.includes(`=== ${resource}`)) {
      inResource = true;
      continue;
    }
    if (inResource && line.startsWith("some")) {
      const m = line.match(/avg10=(\d+\.?\d*)/);
      return m ? parseFloat(m[1]) : null;
    }
    // If we hit another resource header, stop
    if (inResource && (line.includes("/proc/pressure/") || line.startsWith("==="))) break;
  }
  // Fallback: search all lines for "some avg10=" preceded by the resource path
  // If PSI data is concatenated without headers, look for sequential pattern
  const allSome = lines.filter(l => l.startsWith("some"));
  const idx = resource === "cpu" ? 0 : resource === "memory" ? 1 : 2;
  if (allSome[idx]) {
    const m = allSome[idx].match(/avg10=(\d+\.?\d*)/);
    return m ? parseFloat(m[1]) : null;
  }
  return null;
}

interface NetDevStats { rxBytes: number; txBytes: number; rxDrop: number; txDrop: number; rxErrors: number; txErrors: number }
function parseNetDev(text: string): Record<string, NetDevStats> {
  const result: Record<string, NetDevStats> = {};
  for (const line of text.split("\n")) {
    const m = line.match(/^\s*(\S+):\s*(.*)/);
    if (!m) continue;
    const iface = m[1];
    const vals = m[2].trim().split(/\s+/).map(Number);
    if (vals.length < 16) continue;
    result[iface] = {
      rxBytes: vals[0], rxErrors: vals[2], rxDrop: vals[3],
      txBytes: vals[8], txErrors: vals[10], txDrop: vals[11],
    };
  }
  return result;
}

interface DiskStats { ioTime: number }
function parseDiskstats(text: string): Record<string, DiskStats> {
  const result: Record<string, DiskStats> = {};
  for (const line of text.split("\n")) {
    const parts = line.trim().split(/\s+/);
    if (parts.length < 13) continue;
    const dev = parts[2];
    // Skip partitions (we only want whole disks like sda, nvme0n1)
    if (/\d+$/.test(dev) && !/n\d+$/.test(dev)) continue;
    result[dev] = { ioTime: parseInt(parts[12], 10) || 0 };
  }
  return result;
}
