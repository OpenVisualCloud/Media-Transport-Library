/**
 * mtl_scheduler_map — Map MTL scheduler-to-session assignments.
 *
 * For each running MTL instance, reports the per-scheduler session count,
 * lcore assignment, tasklet count, and warns about imbalanced session
 * distribution across schedulers.
 *
 * Data sources (in priority order):
 *   1. MTL stat dump logs (M T D E V S T A T E blocks)
 *   2. /proc thread scan (mtl_sch_* threads + /proc/tid/stat)
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - log_paths: explicit log file paths to check (optional)
 */
import type { ToolResponse } from "../types.js";
import type {
  MtlSchedulerMapData,
  MtlSchedulerMapInstance,
  MtlSchedulerInfo,
  MtlSchedulerSessionInfo,
  MtlSchStats,
  MtlVideoSessionStats,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";

/**
 * Discover MTL processes by scanning for threads named mtl_sch_*.
 * Returns a map of PID → { comm, tids[] }.
 */
async function discoverMtlProcesses(
  host: string,
): Promise<Map<number, { comm: string; tids: { tid: number; name: string; cpu: number }[] }>> {
  const script = `
for pid in /proc/[0-9]*/; do
  pid_num=\${pid#/proc/}
  pid_num=\${pid_num%/}
  found=0
  if [ -d "$pid/task" ]; then
    for tid_dir in $pid/task/[0-9]*/; do
      tcomm=$(cat "$tid_dir/comm" 2>/dev/null)
      if [ "\${tcomm#mtl_sch_}" != "$tcomm" ]; then
        if [ $found -eq 0 ]; then
          pcomm=$(cat "/proc/$pid_num/comm" 2>/dev/null)
          echo "PID:$pid_num|$pcomm"
          found=1
        fi
        tid=\${tid_dir%/}
        tid=\${tid##*/}
        cpu=$(awk '{print $39}' "$tid_dir/stat" 2>/dev/null)
        echo "THREAD:$tid|$tcomm|$cpu"
      fi
    done
  fi
done 2>/dev/null`;

  const output = await sshExecSafe(host, script, 15_000);
  const result = new Map<number, { comm: string; tids: { tid: number; name: string; cpu: number }[] }>();

  if (!output || !output.trim()) return result;

  let currentPid = 0;
  for (const line of output.split("\n")) {
    if (line.startsWith("PID:")) {
      const parts = line.slice(4).split("|");
      currentPid = parseInt(parts[0], 10);
      const comm = parts[1] ?? "";
      result.set(currentPid, { comm, tids: [] });
    } else if (line.startsWith("THREAD:") && currentPid > 0) {
      const parts = line.slice(7).split("|");
      const entry = result.get(currentPid);
      if (entry) {
        entry.tids.push({
          tid: parseInt(parts[0], 10),
          name: parts[1] ?? "",
          cpu: parseInt(parts[2], 10) || 0,
        });
      }
    }
  }

  return result;
}

/**
 * Find DPDK telemetry socket for a process by checking /var/run/dpdk/.
 */
async function findDpdkSocket(host: string, pid: number): Promise<string | null> {
  const script = `ls /var/run/dpdk/*/dpdk_telemetry.v2* 2>/dev/null | while read sock; do
  # Check if the socket's PID matches by reading lsof or checking /proc/net/unix
  echo "$sock"
done`;
  // Simplified: just list sockets. Can't reliably match to PID without lsof.
  const output = await sshExecSafe(host, script, 5_000);
  if (!output || !output.trim()) return null;
  // Return first socket found (imprecise but useful for reference)
  const sockets = output.trim().split("\n").filter(Boolean);
  return sockets.length > 0 ? sockets[0] : null;
}

/**
 * Scan well-known log directories and match log files to an MTL process
 * by checking if any thread PID from the process appears in the log.
 */
async function findLogForProcess(
  host: string,
  pid: number,
  tids: number[],
  explicitPaths: string[] | undefined,
): Promise<string | null> {
  // Build list of candidate log files
  const dirs = [
    "/dev/shm/poc_logs",
    "/dev/shm/poc16_logs",
    "/dev/shm/poc_8k_logs",
    "/dev/shm/kahawai_logs",
    "/tmp",
  ];

  const pathsToCheck: string[] = [];
  if (explicitPaths && explicitPaths.length > 0) {
    // LLM provided explicit paths — use only those, skip well-known dir scan
    pathsToCheck.push(...explicitPaths);
  } else {
    // No explicit paths — fall back to scanning well-known directories
    const findScript = dirs
      .map((d) => `find ${d} -name '*.log' -maxdepth 1 2>/dev/null`)
      .join("\n");
    const logFilesOutput = await sshExecSafe(host, findScript, 5_000);
    if (logFilesOutput) {
      for (const f of logFilesOutput.split("\n")) {
        const trimmed = f.trim();
        if (trimmed) pathsToCheck.push(trimmed);
      }
    }
  }

  if (pathsToCheck.length === 0) return null;

  // For each candidate, grep for any t_pid that matches our thread TIDs
  // We check the last 500 lines of each file (MTL stat dumps are periodic)
  for (const logPath of pathsToCheck) {
    const tidPatterns = tids.map((t) => `t_pid: ${t}`).join("\\|");
    const grepScript = `tail -500 ${logPath} 2>/dev/null | grep -c '${tidPatterns}'`;
    const count = await sshExecSafe(host, grepScript, 3_000);
    if (count && parseInt(count.trim(), 10) > 0) {
      return logPath;
    }
  }

  return null;
}

/**
 * Parse the latest stat dump from a log file and extract scheduler + session info.
 */
async function parseStatDumpFromLog(
  host: string,
  logPath: string,
): Promise<{ schStats: MtlSchStats[]; videoSessions: MtlVideoSessionStats[] } | null> {
  const output = await sshExecSafe(host, `tail -300 ${logPath} 2>/dev/null`, 10_000);
  if (!output || !output.trim()) return null;

  const allLines = output.split("\n");

  // Find the last complete stat dump block
  let lastBlockStart = -1;
  let lastBlockEnd = -1;
  let blockStart = -1;

  for (let i = 0; i < allLines.length; i++) {
    if (
      allLines[i].includes("M T") &&
      allLines[i].includes("D E V") &&
      allLines[i].includes("S T A T E")
    ) {
      blockStart = i + 1;
    } else if (
      allLines[i].includes("E N D") &&
      allLines[i].includes("S T A T E") &&
      blockStart >= 0
    ) {
      lastBlockStart = blockStart;
      lastBlockEnd = i;
      blockStart = -1;
    }
  }

  if (lastBlockStart < 0 || lastBlockEnd < 0) return null;

  const blockLines = allLines.slice(lastBlockStart, lastBlockEnd);
  const dump = parseMtlStatBlock(blockLines);

  if (dump.sch_stats.length === 0) return null;

  return {
    schStats: dump.sch_stats,
    videoSessions: dump.video_sessions,
  };
}

/**
 * Build scheduler info from stat dump data.
 */
function buildSchedulersFromStatDump(
  schStats: MtlSchStats[],
  videoSessions: MtlVideoSessionStats[],
): MtlSchedulerInfo[] {
  // Group sessions by sch_index
  const sessionsByScheduler = new Map<number, MtlSchedulerSessionInfo[]>();
  for (const vs of videoSessions) {
    let list = sessionsByScheduler.get(vs.sch_index);
    if (!list) {
      list = [];
      sessionsByScheduler.set(vs.sch_index, list);
    }
    list.push({
      session_index: vs.session_index,
      name: vs.name,
      fps: vs.fps,
      throughput_mbps: vs.throughput_mbps,
    });
  }

  return schStats.map((sch) => ({
    sch_index: sch.sch_index,
    name: sch.name,
    lcore: sch.lcore,
    thread_pid: sch.thread_pid,
    tasklet_count: sch.tasklets,
    avg_loop_ns: sch.avg_loop_ns,
    sessions: sessionsByScheduler.get(sch.sch_index) ?? [],
    session_count: (sessionsByScheduler.get(sch.sch_index) ?? []).length,
  }));
}

/**
 * Build scheduler info from thread scan (fallback when no stat dump).
 */
function buildSchedulersFromThreadScan(
  tids: { tid: number; name: string; cpu: number }[],
): MtlSchedulerInfo[] {
  return tids
    .filter((t) => t.name.startsWith("mtl_sch_"))
    .map((t) => {
      const idxMatch = t.name.match(/mtl_sch_(\d+)/);
      const schIdx = idxMatch ? parseInt(idxMatch[1], 10) : 0;
      return {
        sch_index: schIdx,
        name: t.name.replace("mtl_", ""),
        lcore: t.cpu,
        thread_pid: t.tid,
        tasklet_count: -1,
        avg_loop_ns: -1,
        sessions: [],
        session_count: -1,
      };
    })
    .sort((a, b) => a.sch_index - b.sch_index);
}

/**
 * Check for imbalanced session distribution.
 */
function detectImbalance(schedulers: MtlSchedulerInfo[]): string | null {
  if (schedulers.length < 2) return null;

  // Only check schedulers where session_count is known (>= 0)
  const counts = schedulers
    .map((s) => s.session_count)
    .filter((c) => c >= 0);

  if (counts.length < 2) return null;

  const max = Math.max(...counts);
  const min = Math.min(...counts);

  if (min === 0 && max > 0) {
    return `${max}/0 session split across ${schedulers.length} schedulers — ` +
      `consider lowering data_quota_mbs_per_sch to balance load`;
  }

  if (min > 0 && max / min > 2.0) {
    return `${max}/${min} session split across ${schedulers.length} schedulers — ` +
      `consider lowering data_quota_mbs_per_sch to balance load`;
  }

  // Also check tasklet counts as a proxy when session counts are equal
  const tasklets = schedulers
    .map((s) => s.tasklet_count)
    .filter((t) => t > 0);

  if (tasklets.length >= 2) {
    const tMax = Math.max(...tasklets);
    const tMin = Math.min(...tasklets);
    if (tMin > 0 && tMax / tMin > 2.0) {
      return `${tMax}/${tMin} tasklet split across ${schedulers.length} schedulers — ` +
        `consider lowering data_quota_mbs_per_sch to balance load`;
    }
  }

  return null;
}

// ─── Main tool function ─────────────────────────────────────────────────────

export async function mtlSchedulerMap(params: {
  host?: string;
  log_paths?: string[];
}): Promise<ToolResponse<MtlSchedulerMapData>> {
  const host = params.host ?? "localhost";
  const logPaths = params.log_paths;

  const meta = await buildMeta("fallback");

  try {
    // Step 1: Discover MTL processes with mtl_sch_* threads
    const processes = await discoverMtlProcesses(host);

    if (processes.size === 0) {
      return okResponse<MtlSchedulerMapData>(
        { instances: [], instance_count: 0 },
        meta,
      );
    }

    const instances: MtlSchedulerMapInstance[] = [];

    // Step 2: For each MTL process, try to get scheduler info
    for (const [pid, procInfo] of processes) {
      const tidNumbers = procInfo.tids.map((t) => t.tid);

      // Try to find log file with stat dumps
      const logPath = await findLogForProcess(host, pid, tidNumbers, logPaths);

      let schedulers: MtlSchedulerInfo[];
      let dataSource: "stat_dump" | "thread_scan";
      let socketPath: string | null = null;

      if (logPath) {
        // Try to parse stat dump from log
        const dumpData = await parseStatDumpFromLog(host, logPath);

        if (dumpData && dumpData.schStats.length > 0) {
          schedulers = buildSchedulersFromStatDump(dumpData.schStats, dumpData.videoSessions);
          dataSource = "stat_dump";
        } else {
          // Log found but no stat dump — fall back to thread scan
          schedulers = buildSchedulersFromThreadScan(procInfo.tids);
          dataSource = "thread_scan";
        }
      } else {
        // No log file — use thread scan only
        schedulers = buildSchedulersFromThreadScan(procInfo.tids);
        dataSource = "thread_scan";
      }

      // Try to find DPDK socket (best-effort)
      try {
        socketPath = await findDpdkSocket(host, pid);
      } catch {
        // Non-critical
      }

      const imbalanceWarning = detectImbalance(schedulers);

      instances.push({
        pid,
        comm: procInfo.comm,
        socket_path: socketPath,
        log_path: logPath,
        data_source: dataSource,
        schedulers,
        scheduler_count: schedulers.length,
        imbalance_warning: imbalanceWarning,
      });
    }

    // Sort by PID for deterministic output
    instances.sort((a, b) => a.pid - b.pid);

    return okResponse<MtlSchedulerMapData>(
      { instances, instance_count: instances.length },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_SCHEDULER_MAP_ERROR",
      `Failed to map MTL schedulers: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure MTL processes are running on the target host",
    );
  }
}
