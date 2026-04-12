/**
 * mtl-usdt-tasklet-timing.ts — Per-scheduler & per-tasklet timing via USDT.
 *
 * Attaches to sys:tasklet_time_measure + sys:log_msg.
 *
 * WHY THIS IS A SEPARATE TOOL:
 *   MTL normally does NOT report per-tasklet timing in stat dumps.
 *   The timing code path is gated behind MTL_FLAG_TASKLET_TIME_MEASURE.
 *   Simply ATTACHING to the sys:tasklet_time_measure USDT probe activates
 *   the timing at runtime — zero config change, zero restart.
 *
 * WHAT IT UNLOCKS (visible in the next stat dump):
 *   - SCH(n): time avg X.XXus max X.XXus min X.XXus   ← scheduler loop timing
 *   - SCH(n,m): tasklet NAME, avg X.XXus max X.XXus min X.XXus  ← per-tasklet
 *
 * USE WHEN:
 *   - avg_loop_ns is high and you need to find which tasklet is the bottleneck
 *   - Investigating scheduler overload (too many sessions per scheduler)
 *   - Comparing per-tasklet timing before/after a code change
 *
 * Attaches to log_msg to capture the stat dump text that contains the
 * timing data. The timing data is only generated while attached.
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import type { MtlSchStats, MtlSchTaskletStats, MtlStatDump } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";

export const mtlUsdtTaskletTimingSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process to trace. If omitted, discovers MTL processes automatically.",
  ),
  duration_sec: z.number().min(5).max(60).default(15).describe(
    "How many seconds to trace (default: 15). MTL dumps stats every ~10s, " +
    "so 15s guarantees at least one dump with timing data included.",
  ),
});

async function discoverMtlPids(): Promise<{ pid: number; name: string }[]> {
  const output = await sshExecSafe("localhost",
    `for pid in /proc/[0-9]*/task/*/comm; do
      if grep -qP '^mtl_sch_' "$pid" 2>/dev/null; then
        ppid=$(echo "$pid" | cut -d/ -f3)
        comm=$(cat "/proc/$ppid/comm" 2>/dev/null)
        echo "$ppid|$comm"
      fi
    done | sort -u -t'|' -k1,1`);
  if (!output?.trim()) return [];
  const seen = new Set<number>();
  return output.trim().split("\n").flatMap(line => {
    const [pidStr, name] = line.split("|");
    const pid = parseInt(pidStr, 10);
    if (!isNaN(pid) && !seen.has(pid)) { seen.add(pid); return [{ pid, name: name ?? "" }]; }
    return [];
  });
}

export interface TaskletTimingData {
  pid: number;
  process_name: string;
  duration_sec: number;
  /** Per-scheduler timing from the stat dump */
  schedulers: MtlSchStats[];
  /** Raw stat dump for context */
  full_dump: MtlStatDump | null;
  /** Whether timing data was actually present (probe activation confirmed) */
  timing_activated: boolean;
  raw_lines?: string;
}

export async function mtlUsdtTaskletTiming(
  params: z.infer<typeof mtlUsdtTaskletTimingSchema>,
): Promise<ToolResponse<TaskletTimingData>> {
  const meta = await buildMeta("usdt");
  const bridge = getBpftraceBridge();

  if (!bridge.isAvailable || !bridge.libmtlPath) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      "bpftrace or libmtl.so not available",
      "Install bpftrace and ensure MTL is built with USDT support");
  }

  // Discover PID
  let pid = params.pid;
  let processName = "";
  if (!pid) {
    const procs = await discoverMtlPids();
    if (procs.length === 0) {
      return errorResponse(meta, "NO_MTL_PROCESS",
        "No running MTL processes found",
        "Start an MTL pipeline first");
    }
    pid = procs[0].pid;
    processName = procs[0].name;
  } else {
    try {
      const comm = await sshExecSafe("localhost", `cat /proc/${pid}/comm 2>/dev/null`);
      processName = comm?.trim() ?? "";
    } catch { /* ignore */ }
  }

  const durationSec = params.duration_sec ?? 15;
  const libmtlPath = bridge.libmtlPath;

  // bpftrace script: attach tasklet_time_measure (activates timing) + log_msg (captures dump)
  const script = `
usdt:${libmtlPath}:sys:tasklet_time_measure { }
usdt:${libmtlPath}:sys:log_msg {
  printf("USDT_LOG:%d:%s\\n", arg0, str(arg1));
}
`;

  const result = await bridge.runScript(script, pid, durationSec * 1000, {
    BPFTRACE_MAX_STRLEN: "200",
  });

  if (result.exitCode !== 0 && !result.timedOut) {
    return errorResponse(meta, "BPFTRACE_FAILED",
      `bpftrace failed (exit ${result.exitCode}): ${result.stderr.slice(0, 300)}`,
      "Ensure running as root and PID is valid");
  }

  // Parse log lines
  const logLines: string[] = [];
  for (const line of result.stdout.split("\n")) {
    const m = line.match(/^USDT_LOG:\d+:(.*)$/);
    if (m) logLines.push(m[1]);
  }

  // Find last complete stat dump
  let blockStart = -1;
  let blockEnd = -1;
  for (let i = logLines.length - 1; i >= 0; i--) {
    if (logLines[i].includes("E N D") && logLines[i].includes("S T A T E")) blockEnd = i;
    if (blockEnd > 0 && logLines[i].includes("M T") && logLines[i].includes("D E V") && logLines[i].includes("S T A T E")) {
      blockStart = i + 1;
      break;
    }
  }

  if (blockStart < 0 || blockEnd <= blockStart) {
    return errorResponse(meta, "NO_STAT_DUMP",
      `No complete stat dump captured in ${durationSec}s (got ${logLines.length} log lines)`,
      "MTL dumps stats every ~10s — try increasing duration_sec");
  }

  const blockLines = logLines.slice(blockStart, blockEnd);
  const dump = parseMtlStatBlock(blockLines);

  // Check if timing data is actually present
  const timingActivated = dump.sch_stats.some(s => s.time_avg_us != null);

  const data: TaskletTimingData = {
    pid,
    process_name: processName,
    duration_sec: durationSec,
    schedulers: dump.sch_stats,
    full_dump: dump,
    timing_activated: timingActivated,
  };

  return okResponse(data, meta);
}
