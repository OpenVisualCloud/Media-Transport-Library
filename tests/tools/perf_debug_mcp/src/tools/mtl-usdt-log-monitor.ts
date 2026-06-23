/**
 * mtl-usdt-log-monitor.ts — Real-time MTL log capture via USDT (read-only).
 *
 * Attaches ONLY to sys:log_msg.
 *
 * WHY THIS IS A SEPARATE TOOL:
 *   This is the SAFE, READ-ONLY USDT tool — it captures MTL log messages
 *   without activating any timing or packet-capture features. The other
 *   USDT tools (tasklet_timing, session_timing, cni_pcap) have side effects:
 *   they activate features that increase CPU usage or create files.
 *
 * WHAT IT CAPTURES:
 *   - ALL MTL log messages regardless of log-level configuration
 *   - Even messages not written to disk (MTL's log_msg USDT fires on EVERY
 *     log call, before the log-level filter)
 *   - Stat dumps, error messages, session state changes, warnings
 *
 * USE WHEN:
 *   - You want to see what MTL is outputting in real-time
 *   - Log files aren't available or are being filtered too aggressively
 *   - Quick sanity check: "is this MTL process producing output?"
 *   - Looking for specific error messages or warnings
 *   - You do NOT want to affect performance (no timing activation)
 *
 * DIFFERENCE FROM mtl_usdt_session_stats:
 *   - log_monitor → raw log messages, no side effects
 *   - session_stats → parses stat dumps AND activates timing probes
 *
 * DIFFERENCE FROM log_search:
 *   - log_search → reads log FILES on disk
 *   - log_monitor → captures via USDT in real-time, works even without disk logs
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export const mtlUsdtLogMonitorSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process to trace. If omitted, discovers MTL processes automatically.",
  ),
  duration_sec: z.number().min(2).max(60).default(10).describe(
    "Capture duration in seconds (default: 10). Captures all log messages in this window.",
  ),
  level_filter: z.enum(["all", "error", "warning", "notice", "info", "debug"]).default("all").describe(
    "Filter by minimum log level. 'all' returns everything. " +
    "Levels: 0=ERR, 1=WARN, 2=NOTICE, 3=INFO, 4=DEBUG.",
  ),
  pattern: z.string().optional().describe(
    "Optional grep-style pattern to filter log messages (case-insensitive substring match). " +
    "Examples: 'error', 'fps', 'timeout', 'drop'.",
  ),
});

const LOG_LEVELS: Record<string, number> = {
  error: 0, warning: 1, notice: 2, info: 3, debug: 4, all: 99,
};

const LOG_LEVEL_NAMES: Record<number, string> = {
  0: "ERR", 1: "WARN", 2: "NOTICE", 3: "INFO", 4: "DEBUG",
};

export interface LogMessage {
  level: number;
  level_name: string;
  message: string;
}

export interface LogMonitorData {
  pid: number;
  process_name: string;
  duration_sec: number;
  /** Captured log messages (after filtering) */
  messages: LogMessage[];
  /** Total messages captured (before pattern filter, after level filter) */
  total_captured: number;
  /** Messages filtered out by pattern */
  pattern_filtered: number;
  /** Level filter applied */
  level_filter: string;
  /** Pattern filter applied */
  pattern?: string;
  /** Whether errors were detected in the output */
  has_errors: boolean;
  /** Whether warnings were detected in the output */
  has_warnings: boolean;
}

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

export async function mtlUsdtLogMonitor(
  params: z.infer<typeof mtlUsdtLogMonitorSchema>,
): Promise<ToolResponse<LogMonitorData>> {
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

  const durationSec = params.duration_sec ?? 10;
  const libmtlPath = bridge.libmtlPath;

  // ONLY attach to log_msg — no timing probes, no pcap capture, no side effects
  const script = `
usdt:${libmtlPath}:sys:log_msg {
  printf("LOG:%d:%s\\n", arg0, str(arg1));
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

  // Parse LOG:level:message lines
  const minLevel = LOG_LEVELS[params.level_filter ?? "all"] ?? 99;
  const pattern = params.pattern?.toLowerCase();

  const allMessages: LogMessage[] = [];
  let totalCaptured = 0;
  let patternFiltered = 0;

  for (const line of result.stdout.split("\n")) {
    const m = line.match(/^LOG:(\d+):(.*)$/);
    if (!m) continue;

    const level = parseInt(m[1], 10);
    const message = m[2];

    // Level filter (lower level = higher severity)
    if (minLevel !== 99 && level > minLevel) continue;

    totalCaptured++;

    // Pattern filter
    if (pattern && !message.toLowerCase().includes(pattern)) {
      patternFiltered++;
      continue;
    }

    allMessages.push({
      level,
      level_name: LOG_LEVEL_NAMES[level] ?? `L${level}`,
      message,
    });
  }

  const hasErrors = allMessages.some(m => m.level === 0);
  const hasWarnings = allMessages.some(m => m.level <= 1);

  const data: LogMonitorData = {
    pid,
    process_name: processName,
    duration_sec: durationSec,
    messages: allMessages,
    total_captured: totalCaptured,
    pattern_filtered: patternFiltered,
    level_filter: params.level_filter ?? "all",
    pattern: params.pattern,
    has_errors: hasErrors,
    has_warnings: hasWarnings,
  };

  return okResponse(data, meta);
}
