/**
 * /proc/<pid>/task/<tid>/stat parser.
 * The comm field is in parentheses and may contain spaces, closing parens, etc.
 * We parse by finding the LAST ')' to split correctly.
 */
import { readFile, readdir } from "fs/promises";
import { readFileHost, listEntriesHost, isLocalHost } from "../remote/host-utils.js";
import { remoteTaskTable } from "../remote/agent-manager.js";
import type { ProcTaskStat } from "../types.js";

/**
 * Parse a single /proc/<pid>/task/<tid>/stat line.
 */
export function parseProcTaskStatLine(line: string): ProcTaskStat | null {
  // Find the last ')' — everything before the first '(' is pid,
  // between '(' and the last ')' is comm, rest is numeric fields.
  const openParen = line.indexOf("(");
  const closeParen = line.lastIndexOf(")");
  if (openParen === -1 || closeParen === -1) return null;

  const pid = parseInt(line.substring(0, openParen).trim(), 10);
  const comm = line.substring(openParen + 1, closeParen);
  const rest = line.substring(closeParen + 2).trim().split(/\s+/);

  if (rest.length < 50) return null;

  return {
    pid,
    comm,
    state: rest[0],
    ppid: parseInt(rest[1], 10),
    pgrp: parseInt(rest[2], 10),
    session: parseInt(rest[3], 10),
    tty_nr: parseInt(rest[4], 10),
    flags: parseInt(rest[6], 10),
    minflt: parseInt(rest[7], 10),
    majflt: parseInt(rest[9], 10),
    utime: parseInt(rest[11], 10),
    stime: parseInt(rest[12], 10),
    priority: parseInt(rest[15], 10),
    nice: parseInt(rest[16], 10),
    num_threads: parseInt(rest[17], 10),
    starttime: parseInt(rest[19], 10),
    vsize: parseInt(rest[20], 10),
    rss: parseInt(rest[21], 10),
    processor: parseInt(rest[36], 10),
    rt_priority: parseInt(rest[37], 10),
    policy: parseInt(rest[38], 10),
  };
}

/**
 * Read and parse /proc/<pid>/task/<tid>/stat.
 */
export async function readTaskStat(pid: number, tid: number, host?: string): Promise<ProcTaskStat | null> {
  try {
    const content = isLocalHost(host)
      ? await readFile(`/proc/${pid}/task/${tid}/stat`, "utf-8")
      : await readFileHost(host, `/proc/${pid}/task/${tid}/stat`);
    return parseProcTaskStatLine(content.trim());
  } catch {
    return null;
  }
}

/**
 * Read the comm of a task.
 */
export async function readTaskComm(pid: number, tid: number, host?: string): Promise<string | null> {
  try {
    const content = isLocalHost(host)
      ? await readFile(`/proc/${pid}/task/${tid}/comm`, "utf-8")
      : await readFileHost(host, `/proc/${pid}/task/${tid}/comm`);
    return content.trim();
  } catch {
    return null;
  }
}

/**
 * Enumerate all PIDs in /proc.
 */
export async function listPids(host?: string): Promise<number[]> {
  try {
    const entries = isLocalHost(host)
      ? await readdir("/proc")
      : await listEntriesHost(host, "/proc");
    return entries
      .filter((e) => /^\d+$/.test(e))
      .map((e) => parseInt(e, 10))
      .sort((a, b) => a - b);
  } catch {
    return [];
  }
}

/**
 * Enumerate all TIDs for a given PID.
 */
export async function listTids(pid: number, host?: string): Promise<number[]> {
  try {
    const entries = isLocalHost(host)
      ? await readdir(`/proc/${pid}/task`)
      : await listEntriesHost(host, `/proc/${pid}/task`);
    return entries
      .filter((e) => /^\d+$/.test(e))
      .map((e) => parseInt(e, 10))
      .sort((a, b) => a - b);
  } catch {
    return [];
  }
}

/**
 * Build a full task table: iterate all PIDs, all TIDs, read stat for each.
 * Returns an array of parsed task stats. Skips unreadable tasks.
 * For remote hosts, uses the deployed agent for efficiency.
 */
export async function buildTaskTable(options?: {
  limit?: number;
  filterPids?: Set<number>;
  onlyRunnable?: boolean;
  host?: string;
}): Promise<ProcTaskStat[]> {
  const host = options?.host;

  // Remote path: delegate to agent for efficient batch scanning
  if (!isLocalHost(host)) {
    return remoteTaskTable(host!, {
      limit: options?.limit,
      filterPids: options?.filterPids,
      onlyRunnable: options?.onlyRunnable,
    });
  }

  // Local path: existing implementation
  const limit = options?.limit ?? 100_000;
  const filterPids = options?.filterPids;
  const onlyRunnable = options?.onlyRunnable ?? false;

  const tasks: ProcTaskStat[] = [];
  let pids: number[];

  if (filterPids && filterPids.size > 0) {
    pids = [...filterPids].sort((a, b) => a - b);
  } else {
    pids = await listPids();
  }

  for (const pid of pids) {
    if (tasks.length >= limit) break;
    let tids: number[];
    try {
      tids = await listTids(pid);
    } catch {
      continue;
    }

    // Read all tids in parallel for this PID (batched)
    const batchSize = 64;
    for (let i = 0; i < tids.length; i += batchSize) {
      if (tasks.length >= limit) break;
      const batch = tids.slice(i, i + batchSize);
      const results = await Promise.allSettled(
        batch.map((tid) => readTaskStat(pid, tid))
      );
      for (const r of results) {
        if (r.status === "fulfilled" && r.value) {
          if (onlyRunnable && r.value.state !== "R") continue;
          tasks.push(r.value);
          if (tasks.length >= limit) break;
        }
      }
    }
  }

  return tasks;
}
