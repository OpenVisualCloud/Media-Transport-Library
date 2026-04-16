/**
 * sysfs readers — CPU topology, frequency, online/offline, etc.
 */
import { readFileTrimmed, readFileInt, listEntries, parseCpuList, isReadable } from "../utils/proc-reader.js";
import {
  readFileTrimmedHost, readFileIntHost, listEntriesHost, isReadableHost, isLocalHost,
} from "../remote/host-utils.js";
import { remoteSysfs } from "../remote/agent-manager.js";
import type { CpuFrequencyEntry } from "../types.js";

const CPU_SYS_BASE = "/sys/devices/system/cpu";

/**
 * Get the list of online CPUs.
 */
export async function getOnlineCpus(host?: string): Promise<number[]> {
  const content = isLocalHost(host)
    ? await readFileTrimmed(`${CPU_SYS_BASE}/online`)
    : await readFileTrimmedHost(host, `${CPU_SYS_BASE}/online`);
  return content ? parseCpuList(content) : [];
}

/**
 * Get the list of offline CPUs.
 */
export async function getOfflineCpus(host?: string): Promise<number[]> {
  const content = isLocalHost(host)
    ? await readFileTrimmed(`${CPU_SYS_BASE}/offline`)
    : await readFileTrimmedHost(host, `${CPU_SYS_BASE}/offline`);
  return content ? parseCpuList(content) : [];
}

/**
 * Get the list of isolated CPUs from sysfs.
 */
export async function getIsolatedCpus(host?: string): Promise<number[]> {
  const content = isLocalHost(host)
    ? await readFileTrimmed(`${CPU_SYS_BASE}/isolated`)
    : await readFileTrimmedHost(host, `${CPU_SYS_BASE}/isolated`);
  return content ? parseCpuList(content) : [];
}

/**
 * Get total CPU count (from /sys/devices/system/cpu/).
 */
export async function getCpuCount(host?: string): Promise<number> {
  const entries = isLocalHost(host)
    ? await listEntries(CPU_SYS_BASE)
    : await listEntriesHost(host, CPU_SYS_BASE);
  return entries.filter((e) => /^cpu\d+$/.test(e)).length;
}

/**
 * Get NUMA node count.
 */
export async function getNumaNodeCount(host?: string): Promise<number> {
  const entries = isLocalHost(host)
    ? await listEntries("/sys/devices/system/node")
    : await listEntriesHost(host, "/sys/devices/system/node");
  return entries.filter((e) => /^node\d+$/.test(e)).length;
}

/**
 * Get CPUs for a NUMA node.
 */
export async function getNumaNodeCpus(node: number, host?: string): Promise<number[]> {
  const content = isLocalHost(host)
    ? await readFileTrimmed(`/sys/devices/system/node/node${node}/cpulist`)
    : await readFileTrimmedHost(host, `/sys/devices/system/node/node${node}/cpulist`);
  return content ? parseCpuList(content) : [];
}

/**
 * Get NUMA distance matrix.
 * For remote hosts, uses the agent to batch-read all node distances.
 */
export async function getNumaDistances(host?: string): Promise<number[][] | null> {
  if (!isLocalHost(host)) {
    // Use agent for batch read
    const data = await remoteSysfs(host!);
    if (data.numa.length === 0) return null;
    return data.numa.map((n) =>
      n.distance ? n.distance.split(/\s+/).map(Number) : []
    );
  }

  const nodeEntries = await listEntries("/sys/devices/system/node");
  const nodes = nodeEntries
    .filter((e) => /^node\d+$/.test(e))
    .map((e) => parseInt(e.replace("node", ""), 10))
    .sort((a, b) => a - b);

  if (nodes.length === 0) return null;

  const matrix: number[][] = [];
  for (const node of nodes) {
    const content = await readFileTrimmed(`/sys/devices/system/node/node${node}/distance`);
    if (!content) return null;
    matrix.push(content.split(/\s+/).map(Number));
  }
  return matrix;
}

/**
 * Read CPU frequency info for a single CPU.
 */
export async function readCpuFreq(cpu: number, host?: string): Promise<CpuFrequencyEntry> {
  const base = `${CPU_SYS_BASE}/cpu${cpu}/cpufreq`;

  if (isLocalHost(host)) {
    const [curKhz, minKhz, maxKhz, governor] = await Promise.all([
      readFileInt(`${base}/scaling_cur_freq`),
      readFileInt(`${base}/scaling_min_freq`),
      readFileInt(`${base}/scaling_max_freq`),
      readFileTrimmed(`${base}/scaling_governor`),
    ]);
    return { cpu, cur_khz: curKhz, min_khz: minKhz, max_khz: maxKhz, governor };
  }

  const [curKhz, minKhz, maxKhz, governor] = await Promise.all([
    readFileIntHost(host, `${base}/scaling_cur_freq`),
    readFileIntHost(host, `${base}/scaling_min_freq`),
    readFileIntHost(host, `${base}/scaling_max_freq`),
    readFileTrimmedHost(host, `${base}/scaling_governor`),
  ]);
  return { cpu, cur_khz: curKhz, min_khz: minKhz, max_khz: maxKhz, governor };
}

/**
 * Read CPU frequency info for all online CPUs.
 * For remote hosts, uses the agent to batch-read all frequencies.
 */
export async function readAllCpuFreq(host?: string): Promise<CpuFrequencyEntry[]> {
  if (!isLocalHost(host)) {
    const data = await remoteSysfs(host!);
    return data.freq;
  }

  const cpus = await getOnlineCpus();
  const results = await Promise.all(cpus.map((cpu) => readCpuFreq(cpu)));
  return results.sort((a, b) => a.cpu - b.cpu);
}

/**
 * Read thermal throttle count if available.
 */
export async function readThermalThrottleCount(cpu: number, host?: string): Promise<number | null> {
  const path = `${CPU_SYS_BASE}/cpu${cpu}/thermal_throttle/core_throttle_count`;
  return isLocalHost(host) ? readFileInt(path) : readFileIntHost(host, path);
}

/**
 * Check if cpufreq is accessible.
 */
export async function hasCpuFreq(host?: string): Promise<boolean> {
  const path = `${CPU_SYS_BASE}/cpu0/cpufreq/scaling_cur_freq`;
  return isLocalHost(host) ? isReadable(path) : isReadableHost(host, path);
}

/**
 * Parse kernel command line for isolation-related flags.
 */
export async function parseKernelCmdline(host?: string): Promise<Record<string, string>> {
  const content = isLocalHost(host)
    ? await readFileTrimmed("/proc/cmdline")
    : await readFileTrimmedHost(host, "/proc/cmdline");
  if (!content) return {};

  const flags: Record<string, string> = {};
  const interestingKeys = ["isolcpus", "nohz_full", "rcu_nocbs", "irqaffinity", "rcu_nocb_poll"];

  for (const token of content.split(/\s+/)) {
    for (const key of interestingKeys) {
      if (token.startsWith(`${key}=`)) {
        flags[key] = token.substring(key.length + 1);
      } else if (token === key) {
        flags[key] = "true";
      }
    }
  }

  return flags;
}
