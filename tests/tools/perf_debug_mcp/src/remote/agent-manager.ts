/**
 * Remote agent manager — deploy and invoke the bash agent on remote hosts.
 *
 * The agent is auto-deployed on first use and version-checked thereafter.
 */
import { sshExec, sshExecOrThrow, sshExecSafe } from "../utils/ssh-exec.js";
import { AGENT_SCRIPT, AGENT_VERSION, AGENT_PATH, AGENT_INSTALL_DIR } from "./agent-script.js";
import { isLocalHost } from "./host-utils.js";
import { parseCpuList, parseHexMask } from "../utils/proc-reader.js";
import type { ProcTaskStat, CpuFrequencyEntry } from "../types.js";
import { parseProcTaskStatLine } from "../collectors/proc-pid-stat.js";
import type { AffinityInfo } from "../collectors/affinity.js";

// ── Deployment cache ────────────────────────────────────────────────

/** Hosts where the agent is confirmed deployed and at the correct version. */
const deployedHosts = new Map<string, boolean>();

/**
 * Ensure the agent is deployed on the remote host.
 * Checks version, deploys if missing or outdated. Caches result.
 */
export async function ensureAgent(host: string): Promise<void> {
  if (isLocalHost(host)) return;
  if (deployedHosts.get(host)) return;

  // Check existing version
  const version = await sshExecSafe(host, `cat '${AGENT_INSTALL_DIR}/.version' 2>/dev/null`);
  if (version?.trim() === AGENT_VERSION) {
    deployedHosts.set(host, true);
    return;
  }

  // Deploy via base64 to avoid shell quoting issues
  const encoded = Buffer.from(AGENT_SCRIPT).toString("base64");
  const deployCmd = [
    `mkdir -p '${AGENT_INSTALL_DIR}'`,
    `echo '${encoded}' | base64 -d > '${AGENT_PATH}'`,
    `chmod +x '${AGENT_PATH}'`,
    `printf '%s' '${AGENT_VERSION}' > '${AGENT_INSTALL_DIR}/.version'`,
  ].join(" && ");

  await sshExecOrThrow(host, deployCmd, 15_000);
  deployedHosts.set(host, true);
}

/**
 * Invoke the agent with given arguments.
 */
export async function invokeAgent(
  host: string,
  args: string,
  timeoutMs: number = 30_000,
): Promise<string> {
  await ensureAgent(host);
  return sshExecOrThrow(host, `'${AGENT_PATH}' ${args}`, timeoutMs);
}

// ── Section parser ──────────────────────────────────────────────────

const DELIM = "===CPU_DBG_AGENT===";

/**
 * Parse agent output into named sections.
 */
export function parseAgentSections(raw: string): Map<string, string> {
  const sections = new Map<string, string>();
  const lines = raw.split("\n");
  let currentKey: string | null = null;
  const currentLines: string[] = [];

  const flush = () => {
    if (currentKey !== null) {
      sections.set(currentKey, currentLines.join("\n").trim());
      currentLines.length = 0;
    }
  };

  for (const line of lines) {
    if (line.startsWith(DELIM)) {
      flush();
      currentKey = line.substring(DELIM.length).trim();
    } else {
      currentLines.push(line);
    }
  }
  flush();

  return sections;
}

// ── Typed agent operations ──────────────────────────────────────────

/**
 * Build a task table on a remote host via the agent.
 */
export async function remoteTaskTable(
  host: string,
  options?: {
    limit?: number;
    filterPids?: Set<number>;
    onlyRunnable?: boolean;
  },
): Promise<ProcTaskStat[]> {
  const args: string[] = ["tasks"];
  if (options?.limit) args.push("--limit", String(options.limit));
  if (options?.filterPids && options.filterPids.size > 0) {
    args.push("--pids", [...options.filterPids].join(","));
  }
  if (options?.onlyRunnable) args.push("--runnable");

  const timeout = Math.max(30_000, (options?.limit ?? 100_000) > 50_000 ? 60_000 : 30_000);
  const raw = await invokeAgent(host, args.join(" "), timeout);
  const sections = parseAgentSections(raw);
  const taskBlock = sections.get("TASKS") ?? "";

  const tasks: ProcTaskStat[] = [];
  for (const line of taskBlock.split("\n")) {
    if (!line.trim()) continue;
    const parsed = parseProcTaskStatLine(line.trim());
    if (parsed) tasks.push(parsed);
  }

  return tasks;
}

/**
 * Batch-read affinities on a remote host via the agent.
 */
export async function remoteAffinities(
  host: string,
  tids: number[],
): Promise<Map<number, AffinityInfo>> {
  if (tids.length === 0) return new Map();

  const raw = await invokeAgent(host, `affinities ${tids.join(" ")}`, 30_000);
  const sections = parseAgentSections(raw);
  const result = new Map<number, AffinityInfo>();

  // Parse TID sections from AFFINITIES block
  // The raw output has sub-sections: ===CPU_DBG_AGENT===TID <n>
  for (const [key, content] of sections) {
    const tidMatch = key.match(/^TID (\d+)$/);
    if (!tidMatch) continue;
    const tid = parseInt(tidMatch[1], 10);

    let cpus_allowed_hex = "";
    let cpus_allowed_list: number[] = [];
    let cpuset_cpus: number[] | undefined;

    for (const line of content.split("\n")) {
      if (line.startsWith("Cpus_allowed:")) {
        cpus_allowed_hex = line.split(":")[1].trim();
      } else if (line.startsWith("Cpus_allowed_list:")) {
        cpus_allowed_list = parseCpuList(line.split(":")[1].trim());
      } else if (line.startsWith("Cpuset_cpus:")) {
        const val = line.split(":")[1].trim();
        if (val) cpuset_cpus = parseCpuList(val);
      }
    }

    if (cpus_allowed_list.length === 0 && cpus_allowed_hex) {
      cpus_allowed_list = parseHexMask(cpus_allowed_hex);
    }

    const info: AffinityInfo = { cpus_allowed: cpus_allowed_list, cpus_allowed_hex };
    if (cpuset_cpus) info.cpuset_cpus = cpuset_cpus;
    result.set(tid, info);
  }

  return result;
}

/**
 * Parsed sysfs topology from the remote agent.
 */
export interface RemoteSysfsData {
  online: string;
  offline: string;
  isolated: string;
  cmdline: string;
  freq: CpuFrequencyEntry[];
  numa: { node: number; cpulist: string; distance: string }[];
}

/**
 * Collect full sysfs topology from a remote host via the agent.
 */
export async function remoteSysfs(host: string): Promise<RemoteSysfsData> {
  const raw = await invokeAgent(host, "sysfs", 30_000);
  const sections = parseAgentSections(raw);

  // Parse SYSFS section
  const sysfsBlock = sections.get("SYSFS") ?? "";
  const kv: Record<string, string> = {};
  for (const line of sysfsBlock.split("\n")) {
    const eq = line.indexOf("=");
    if (eq !== -1) {
      kv[line.substring(0, eq).trim()] = line.substring(eq + 1).trim();
    }
  }

  // Parse FREQ section
  const freqBlock = sections.get("FREQ") ?? "";
  const freq: CpuFrequencyEntry[] = [];
  for (const line of freqBlock.split("\n")) {
    if (!line.trim()) continue;
    const parts = line.trim().split(/\s+/);
    if (parts.length < 5) continue;
    const cpuNum = parseInt(parts[0], 10);
    if (isNaN(cpuNum)) continue;
    freq.push({
      cpu: cpuNum,
      cur_khz: parts[1] ? parseInt(parts[1], 10) || null : null,
      min_khz: parts[2] ? parseInt(parts[2], 10) || null : null,
      max_khz: parts[3] ? parseInt(parts[3], 10) || null : null,
      governor: parts[4] || null,
    });
    // parts[5] is throttle count — stored separately if needed
  }

  // Parse NUMA section
  const numaBlock = sections.get("NUMA") ?? "";
  const numa: { node: number; cpulist: string; distance: string }[] = [];
  for (const line of numaBlock.split("\n")) {
    if (!line.trim()) continue;
    const parts = line.trim().split(/\s+/);
    if (parts.length < 2) continue;
    const node = parseInt(parts[0], 10);
    if (isNaN(node)) continue;
    // Format: <node> <cpulist> <distance values...>
    numa.push({
      node,
      cpulist: parts[1] || "",
      distance: parts.slice(2).join(" "),
    });
  }

  return {
    online: kv["online"] ?? "",
    offline: kv["offline"] ?? "",
    isolated: kv["isolated"] ?? "",
    cmdline: kv["cmdline"] ?? "",
    freq: freq.sort((a, b) => a.cpu - b.cpu),
    numa: numa.sort((a, b) => a.node - b.node),
  };
}
