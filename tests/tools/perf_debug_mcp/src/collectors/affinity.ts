/**
 * Task affinity collector.
 * Reads CPU affinity from /proc/<pid>/status (Cpus_allowed, Cpus_allowed_list).
 * Also reads cpuset constraints from cgroup if available.
 */
import { readFile } from "fs/promises";
import { parseCpuList, parseHexMask, readFileTrimmed } from "../utils/proc-reader.js";
import { readFileHost, readFileTrimmedHost, isLocalHost } from "../remote/host-utils.js";

export interface AffinityInfo {
  cpus_allowed: number[];
  cpus_allowed_hex: string;
  cpuset_cpus?: number[];
}

/**
 * Read affinity for a specific task (using /proc/<pid>/task/<tid>/status or /proc/<pid>/status).
 */
export async function readTaskAffinity(pid: number, tid?: number, host?: string): Promise<AffinityInfo | null> {
  const path = tid ? `/proc/${pid}/task/${tid}/status` : `/proc/${pid}/status`;
  let content: string;
  try {
    content = isLocalHost(host)
      ? await readFile(path, "utf-8")
      : await readFileHost(host, path);
  } catch {
    return null;
  }

  let cpus_allowed_hex = "";
  let cpus_allowed_list: number[] = [];

  for (const line of content.split("\n")) {
    if (line.startsWith("Cpus_allowed:")) {
      cpus_allowed_hex = line.split(":")[1].trim();
    } else if (line.startsWith("Cpus_allowed_list:")) {
      cpus_allowed_list = parseCpuList(line.split(":")[1].trim());
    }
  }

  if (cpus_allowed_list.length === 0 && cpus_allowed_hex) {
    cpus_allowed_list = parseHexMask(cpus_allowed_hex);
  }

  // Try to read cpuset constraints
  const cpuset_cpus = await readCpusetCpus(pid, host);

  const result: AffinityInfo = {
    cpus_allowed: cpus_allowed_list,
    cpus_allowed_hex,
  };
  if (cpuset_cpus) result.cpuset_cpus = cpuset_cpus;
  return result;
}

/**
 * Read cpuset.cpus.effective for a task's cgroup.
 */
async function readCpusetCpus(pid: number, host?: string): Promise<number[] | undefined> {
  try {
    // Read cgroup path
    const cgroupContent = isLocalHost(host)
      ? await readFile(`/proc/${pid}/cgroup`, "utf-8")
      : await readFileHost(host, `/proc/${pid}/cgroup`);
    // cgroup v2 has line starting with "0::"
    for (const line of cgroupContent.split("\n")) {
      if (line.startsWith("0::")) {
        const cgPath = line.substring(3).trim();
        // Try cgroup v2 cpuset
        const cpusetPath = `/sys/fs/cgroup${cgPath}/cpuset.cpus.effective`;
        const val = isLocalHost(host)
          ? await readFileTrimmed(cpusetPath)
          : await readFileTrimmedHost(host, cpusetPath);
        if (val) return parseCpuList(val);
        // Also try just cpuset.cpus
        const cpusetPath2 = `/sys/fs/cgroup${cgPath}/cpuset.cpus`;
        const val2 = isLocalHost(host)
          ? await readFileTrimmed(cpusetPath2)
          : await readFileTrimmedHost(host, cpusetPath2);
        if (val2) return parseCpuList(val2);
      }
    }
  } catch {
    // cgroup not available
  }
  return undefined;
}

/**
 * Check if a task's actual effective CPUs include a specific CPU.
 * Takes into account both affinity mask and cpuset.
 */
export function isAllowedOnCpu(affinity: AffinityInfo, cpu: number): boolean {
  // Must be in affinity mask
  if (!affinity.cpus_allowed.includes(cpu)) return false;
  // If cpuset is defined, must also be in cpuset
  if (affinity.cpuset_cpus && !affinity.cpuset_cpus.includes(cpu)) return false;
  return true;
}
