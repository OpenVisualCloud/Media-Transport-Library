/**
 * cgroup reader — CPU quotas and cpuset restrictions.
 */
import { readFile } from "fs/promises";
import { readFileTrimmed, readFileInt, parseCpuList, isReadable } from "../utils/proc-reader.js";
import {
  readFileHost, readFileTrimmedHost, readFileIntHost, isReadableHost, isLocalHost,
} from "../remote/host-utils.js";
import type { CgroupCpuLimitsData } from "../types.js";

/**
 * Resolve the cgroup v2 path for a given PID.
 */
export async function resolveCgroupPath(pid: number, host?: string): Promise<string | null> {
  try {
    const content = isLocalHost(host)
      ? await readFile(`/proc/${pid}/cgroup`, "utf-8")
      : await readFileHost(host, `/proc/${pid}/cgroup`);
    for (const line of content.split("\n")) {
      if (line.startsWith("0::")) {
        return line.substring(3).trim();
      }
    }
  } catch {
    // ignore
  }
  return null;
}

/**
 * Read cgroup CPU limits for a PID.
 */
export async function readCgroupCpuLimits(pid: number, host?: string): Promise<CgroupCpuLimitsData | null> {
  const cgPath = await resolveCgroupPath(pid, host);
  if (!cgPath) return null;

  const base = `/sys/fs/cgroup${cgPath}`;

  const readTr = isLocalHost(host) ? readFileTrimmed : (p: string) => readFileTrimmedHost(host, p);
  const readIn = isLocalHost(host) ? readFileInt : (p: string) => readFileIntHost(host, p);

  const [cpuMax, cpuWeight, cpuStat, cpusetEffective, cpusetCpus] = await Promise.all([
    readTr(`${base}/cpu.max`),
    readIn(`${base}/cpu.weight`),
    readCpuStat(`${base}/cpu.stat`, host),
    readTr(`${base}/cpuset.cpus.effective`),
    readTr(`${base}/cpuset.cpus`),
  ]);

  const result: CgroupCpuLimitsData = {
    cgroup_path: cgPath,
    cpu_max: cpuMax,
    cpu_weight: cpuWeight,
  };

  if (cpuStat) {
    result.throttled_usec_delta = cpuStat.throttled_usec;
    result.nr_throttled_delta = cpuStat.nr_throttled;
  }

  const effectiveCpus = cpusetEffective || cpusetCpus;
  if (effectiveCpus) {
    result.cpuset_effective = parseCpuList(effectiveCpus);
  }

  return result;
}

/**
 * Parse cpu.stat file for throttling info.
 */
async function readCpuStat(
  path: string,
  host?: string,
): Promise<{ throttled_usec: number; nr_throttled: number } | null> {
  const readable = isLocalHost(host) ? await isReadable(path) : await isReadableHost(host, path);
  if (!readable) return null;

  try {
    const content = isLocalHost(host)
      ? await readFile(path, "utf-8")
      : await readFileHost(host, path);
    let throttled_usec = 0;
    let nr_throttled = 0;

    for (const line of content.split("\n")) {
      const [key, val] = line.split(/\s+/);
      if (key === "throttled_usec") throttled_usec = parseInt(val, 10) || 0;
      if (key === "nr_throttled") nr_throttled = parseInt(val, 10) || 0;
    }

    return { throttled_usec, nr_throttled };
  } catch {
    return null;
  }
}
