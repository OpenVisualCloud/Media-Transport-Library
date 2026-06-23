/**
 * mtl-usdt-session-timing.ts — Per-session tasklet timing via USDT.
 *
 * Attaches to sys:sessions_time_measure + sys:log_msg.
 *
 * WHY THIS IS A SEPARATE TOOL:
 *   MTL normally does NOT report per-session tasklet timing in stat dumps.
 *   The per-session timing code path activates when MTL detects that the
 *   sys:sessions_time_measure USDT probe is attached.
 *
 * WHAT IT UNLOCKS (visible in the next stat dump):
 *   - TX_VIDEO_SESSION(m,s): tasklet time avg X.XXus max X.XXus min X.XXus
 *   - RX_VIDEO_SESSION(m,s): tasklet time avg X.XXus max X.XXus min X.XXus
 *   - TX_AUDIO_SESSION(m,s): tasklet time avg, tx delta avg
 *   - RX_AUDIO_SESSION(m,s): tasklet time avg
 *   - TX_ANC_SESSION(s): tasklet time avg
 *   - RX_ANC_SESSION(s): tasklet time avg
 *
 * USE WHEN:
 *   - A specific video session has high cpu_busy and you need to know
 *     how much time its tasklet actually consumes
 *   - Comparing session tasklet timing across streams in a multi-stream pipeline
 *   - Investigating whether a session is CPU-bound (high avg) or bursty (high max)
 *
 * DIFFERENCE FROM mtl_usdt_tasklet_timing:
 *   - tasklet_timing → SCH-level: "which scheduler loop is slow?"
 *   - session_timing → session-level: "which session's processing is slow?"
 *   Both are complementary. Use tasklet_timing first to find the hot scheduler,
 *   then session_timing to identify the hot session within it.
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import type { MtlVideoSessionStats, MtlStatDump } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";

export const mtlUsdtSessionTimingSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process to trace. If omitted, discovers MTL processes automatically.",
  ),
  duration_sec: z.number().min(5).max(60).default(15).describe(
    "Trace duration in seconds (default: 15). Needs at least one 10-second stat dump cycle.",
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

export interface SessionTimingEntry {
  session: string;
  direction: "tx" | "rx";
  type: "video" | "audio" | "ancillary";
  tasklet_time_avg_us?: number;
  tasklet_time_max_us?: number;
  tasklet_time_min_us?: number;
  fps?: number;
  cpu_busy?: number;
}

export interface SessionTimingData {
  pid: number;
  process_name: string;
  duration_sec: number;
  /** Per-session timing entries extracted from the stat dump */
  sessions: SessionTimingEntry[];
  /** Whether session timing data was actually present (probe activation confirmed) */
  timing_activated: boolean;
  /** Full parsed stat dump for context */
  full_dump: MtlStatDump | null;
}

export async function mtlUsdtSessionTiming(
  params: z.infer<typeof mtlUsdtSessionTimingSchema>,
): Promise<ToolResponse<SessionTimingData>> {
  const meta = await buildMeta("usdt");
  const bridge = getBpftraceBridge();

  if (!bridge.isAvailable || !bridge.libmtlPath) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      "bpftrace or libmtl.so not available",
      "Install bpftrace and ensure MTL is built with USDT support");
  }

  let pid = params.pid;
  let processName = "";
  if (!pid) {
    const procs = await discoverMtlPids();
    if (procs.length === 0) {
      return errorResponse(meta, "NO_MTL_PROCESS",
        "No running MTL processes found", "Start an MTL pipeline first");
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

  // Attach sessions_time_measure (activates per-session timing) + log_msg (captures dump)
  const script = `
usdt:${libmtlPath}:sys:sessions_time_measure { }
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

  // Extract per-session timing from video sessions
  const sessions: SessionTimingEntry[] = [];
  for (const vs of dump.video_sessions) {
    const direction = vs.name.startsWith("TX") ? "tx" as const : "rx" as const;
    sessions.push({
      session: vs.name,
      direction,
      type: "video",
      tasklet_time_avg_us: vs.tasklet_time_avg_us,
      tasklet_time_max_us: vs.tasklet_time_max_us,
      tasklet_time_min_us: vs.tasklet_time_min_us,
      fps: vs.fps,
      cpu_busy: vs.cpu_busy,
    });
  }

  const timingActivated = sessions.some(s => s.tasklet_time_avg_us != null);

  const data: SessionTimingData = {
    pid,
    process_name: processName,
    duration_sec: durationSec,
    sessions,
    timing_activated: timingActivated,
    full_dump: dump,
  };

  return okResponse(data, meta);
}
