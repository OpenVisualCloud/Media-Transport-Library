/**
 * mtl-usdt-session-stats.ts — Capture MTL session stats via USDT probes.
 *
 * Three-tier data source with user-specified fallback order:
 *   1. USDT (preferred) — attach to sys:log_msg + sys:sessions_time_measure
 *      + sys:tasklet_time_measure probes, capture one complete stat dump
 *   2. Log file — tail + parse (existing parseMtlStatBlock)
 *   3. InfluxDB — query last_dump measurement
 *
 * The USDT approach captures the EXACT same stat dump text that goes to logs,
 * but with these advantages:
 *   - Works even if logs are not written to disk (stdout-only)
 *   - Activates sys:sessions_time_measure and sys:tasklet_time_measure probes
 *     by attachment → enables timing data that's disabled when nothing is tracing
 *   - Lower latency — no need to wait for log rotation / buffering
 *   - More reliable — no risk of partial dump from tail truncation
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import type {
  MtlSessionStatsData,
  MtlStatDump,
  UsdtSessionStatsMode,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";

export const mtlUsdtSessionStatsSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process. If omitted, discovers MTL processes automatically.",
  ),
  host: z.string().optional().describe("Target hostname/IP (default: localhost)"),
  log_path: z.string().optional().describe(
    "Log file path (used as fallback if USDT is unavailable)",
  ),
  duration_sec: z.number().min(3).max(60).optional().describe(
    "How many seconds to trace for stat dump capture (default: 15). " +
    "MTL dumps stats every ~10s, so 15s guarantees at least one full dump.",
  ),
  tail_lines: z.number().optional().describe("Lines to tail from log (fallback mode, default: 500)"),
  last_dumps: z.number().optional().describe("Number of steady-state dumps to use (default: 5)"),
  alert_threshold_fps: z.number().optional().describe("Warn if FPS below this threshold"),
  session_filter: z.string().optional().describe("Filter by session name"),
  force_mode: z.enum(["usdt", "log", "influxdb"]).optional().describe(
    "Force a specific data source instead of auto-fallback",
  ),
});

/**
 * Discover PIDs of running MTL processes.
 */
async function discoverMtlPids(host: string): Promise<{ pid: number; name: string }[]> {
  // MTL processes have threads named mtl_sch_*
  const output = await sshExecSafe(host,
    `for pid in /proc/[0-9]*/task/*/comm; do
      if grep -qP '^mtl_sch_' "$pid" 2>/dev/null; then
        ppid=$(echo "$pid" | cut -d/ -f3)
        comm=$(cat "/proc/$ppid/comm" 2>/dev/null)
        echo "$ppid|$comm"
      fi
    done | sort -u -t'|' -k1,1`);

  if (!output?.trim()) return [];

  const seen = new Set<number>();
  const results: { pid: number; name: string }[] = [];
  for (const line of output.trim().split("\n")) {
    const [pidStr, name] = line.split("|");
    const pid = parseInt(pidStr, 10);
    if (!isNaN(pid) && !seen.has(pid)) {
      seen.add(pid);
      results.push({ pid, name: name ?? "" });
    }
  }
  return results;
}

/**
 * Capture a stat dump via USDT sys:log_msg probe using bpftrace.
 *
 * The bpftrace script:
 *   1. Attaches to sys:log_msg (fires for every MTL log message)
 *   2. Also attaches to sys:tasklet_time_measure and sys:sessions_time_measure
 *      (mere attachment enables the timing code paths in MTL)
 *   3. Captures log lines and looks for STATE markers
 *   4. Prints dump lines to stdout
 *
 * sys:log_msg probe signature: (int level, char* msg)
 *   - arg0 = log level (0=ERR, 1=WARN, 2=NOTICE, 3=INFO, 4=DEBUG)
 *   - arg1 = message string pointer
 */
async function captureViaUsdt(
  pid: number,
  durationSec: number,
  libmtlPath: string,
): Promise<{ dump: MtlStatDump | null; raw: string; error?: string }> {
  const bridge = getBpftraceBridge();

  // bpftrace script that:
  // - Attaches to sys:log_msg to capture stat dump lines
  // - Attaches to sys:tasklet_time_measure + sys:sessions_time_measure
  //   (empty handlers — attachment alone activates timing in MTL)
  // - Tracks STATE markers to identify complete dumps
  // - Prints each line with a USDT_LOG: prefix for easy parsing
  const script = `
usdt:${libmtlPath}:sys:log_msg {
  $msg = str(arg1);
  printf("USDT_LOG:%d:%s\\n", arg0, $msg);
}
usdt:${libmtlPath}:sys:tasklet_time_measure { }
usdt:${libmtlPath}:sys:sessions_time_measure { }
`;

  const result = await bridge.runScript(script, pid, durationSec * 1000, {
    BPFTRACE_MAX_STRLEN: "200",
  });

  if (result.exitCode !== 0 && !result.timedOut) {
    return { dump: null, raw: "", error: `bpftrace failed: ${result.stderr.slice(0, 500)}` };
  }

  // Parse output — look for complete stat blocks
  const lines = result.stdout.split("\n");
  const logLines: string[] = [];
  for (const line of lines) {
    const m = line.match(/^USDT_LOG:\d+:(.*)$/);
    if (m) logLines.push(m[1]);
  }

  // Find last complete stat dump block
  let blockStartIdx = -1;
  let blockEndIdx = -1;
  for (let i = logLines.length - 1; i >= 0; i--) {
    if (logLines[i].includes("E N D") && logLines[i].includes("S T A T E")) {
      blockEndIdx = i;
    }
    if (blockEndIdx > 0 && logLines[i].includes("M T") && logLines[i].includes("D E V") && logLines[i].includes("S T A T E")) {
      blockStartIdx = i + 1;
      break;
    }
  }

  if (blockStartIdx < 0 || blockEndIdx <= blockStartIdx) {
    return {
      dump: null,
      raw: logLines.join("\n"),
      error: `No complete stat dump captured in ${durationSec}s (got ${logLines.length} log lines). ` +
        "MTL dumps stats every ~10s — try increasing duration_sec.",
    };
  }

  const blockLines = logLines.slice(blockStartIdx, blockEndIdx);
  const dump = parseMtlStatBlock(blockLines);
  return { dump, raw: blockLines.join("\n") };
}

/**
 * Query InfluxDB for session stats (tier 3 fallback).
 */
async function queryInfluxDb(
  host: string,
): Promise<{ data: MtlSessionStatsData | null; error?: string }> {
  // InfluxDB fallback: query last data point from poc bucket
  const influxUrl = process.env.INFLUXDB_URL || `http://${host}:8086`;
  const influxToken = process.env.INFLUXDB_TOKEN || "";
  const influxOrg = process.env.INFLUXDB_ORG || "";
  const influxBucket = process.env.INFLUXDB_BUCKET || "poc";

  try {
    const fluxQuery = `from(bucket: "${influxBucket}")
  |> range(start: -30s)
  |> filter(fn: (r) => r._measurement == "mtl_session_stats" or r._measurement == "mtl_stats")
  |> last()`;

    const curlCmd = `curl -s --max-time 5 -XPOST '${influxUrl}/api/v2/query?org=${influxOrg}' ` +
      `-H 'Authorization: Token ${influxToken}' ` +
      `-H 'Content-Type: application/vnd.flux' ` +
      `-H 'Accept: application/csv' ` +
      `--data-raw '${fluxQuery}'`;

    const output = await sshExecSafe("localhost", curlCmd, 10_000);
    if (!output?.trim() || output.includes("error")) {
      return { data: null, error: "InfluxDB query returned no data or error" };
    }

    // For now, InfluxDB fallback returns raw CSV — we'd need to parse this
    // into MtlSessionStatsData. This is a placeholder for the tier-3 fallback.
    return { data: null, error: "InfluxDB CSV parsing not yet implemented (data was reachable)" };
  } catch (err) {
    return { data: null, error: `InfluxDB query failed: ${err instanceof Error ? err.message : String(err)}` };
  }
}

export async function mtlUsdtSessionStats(
  params: z.infer<typeof mtlUsdtSessionStatsSchema>,
): Promise<ToolResponse<MtlSessionStatsData>> {
  const host = params.host ?? "localhost";
  const durationSec = params.duration_sec ?? 15;
  const forceMode = params.force_mode;
  const tailLines = params.tail_lines ?? 500;
  const lastDumps = params.last_dumps ?? 5;

  // Import the existing mtlSessionStats for log-based fallback
  const { mtlSessionStats } = await import("./mtl-session-stats.js");

  // Determine fallback order
  const modes: UsdtSessionStatsMode[] = forceMode
    ? [forceMode]
    : ["usdt", "log", "influxdb"];

  let lastError = "";

  for (const mode of modes) {
    // ── TIER 1: USDT ──────────────────────────────────────────────
    if (mode === "usdt") {
      const bridge = getBpftraceBridge();
      if (!bridge.isAvailable || !bridge.libmtlPath) {
        lastError = "USDT unavailable: " + (
          !bridge.isAvailable ? "bpftrace not found" : "libmtl.so not found"
        );
        continue;
      }

      // Discover PID
      let pid = params.pid;
      if (!pid) {
        const mtlProcs = await discoverMtlPids(host);
        if (mtlProcs.length === 0) {
          lastError = "No running MTL processes found";
          continue;
        }
        pid = mtlProcs[0].pid;
      }

      const capture = await captureViaUsdt(pid, durationSec, bridge.libmtlPath);
      if (capture.dump) {
        const meta = await buildMeta("usdt");
        const result: MtlSessionStatsData = {
          log_file: `usdt:pid=${pid}`,
          latest_dump: capture.dump,
          dumps_found: 1,
          steady_state_dumps: 1,
          data_source: "usdt",
          usdt_pid: pid,
        };
        return okResponse(result, meta);
      }
      lastError = capture.error || "USDT capture returned no dump";
      continue;
    }

    // ── TIER 2: Log file ──────────────────────────────────────────
    if (mode === "log") {
      if (!params.log_path) {
        // Try to auto-discover log paths
        const logDirs = ["/dev/shm/poc_logs", "/dev/shm/poc16_logs", "/dev/shm/poc_8k_logs"];
        let foundLog: string | null = null;
        for (const dir of logDirs) {
          const ls = await sshExecSafe(host, `ls -t ${dir}/*.log 2>/dev/null | head -1`);
          if (ls?.trim()) { foundLog = ls.trim(); break; }
        }
        if (!foundLog) {
          lastError = "No log_path specified and no log files found in /dev/shm/*_logs/";
          continue;
        }
        // Use the discovered log path
        try {
          const logResult = await mtlSessionStats({
            host,
            log_path: foundLog,
            tail_lines: tailLines,
            last_dumps: lastDumps,
            alert_threshold_fps: params.alert_threshold_fps,
            session_filter: params.session_filter,
          });
          if (logResult.ok && logResult.data?.latest_dump) {
            // Tag with data_source
            logResult.data.data_source = "log";
            logResult.data.fallback_reason = lastError || undefined;
            return logResult;
          }
          lastError = "Log parsing returned no dumps";
          continue;
        } catch (err) {
          lastError = `Log parsing failed: ${err instanceof Error ? err.message : String(err)}`;
          continue;
        }
      }

      try {
        const logResult = await mtlSessionStats({
          host,
          log_path: params.log_path,
          tail_lines: tailLines,
          last_dumps: lastDumps,
          alert_threshold_fps: params.alert_threshold_fps,
          session_filter: params.session_filter,
        });
        if (logResult.ok && logResult.data?.latest_dump) {
          logResult.data.data_source = "log";
          logResult.data.fallback_reason = lastError || undefined;
          return logResult;
        }
        lastError = "Log parsing returned no dumps from " + params.log_path;
        continue;
      } catch (err) {
        lastError = `Log parsing failed: ${err instanceof Error ? err.message : String(err)}`;
        continue;
      }
    }

    // ── TIER 3: InfluxDB ──────────────────────────────────────────
    if (mode === "influxdb") {
      const influxResult = await queryInfluxDb(host);
      if (influxResult.data) {
        const meta = await buildMeta("fallback");
        influxResult.data.data_source = "influxdb";
        influxResult.data.fallback_reason = lastError || undefined;
        return okResponse(influxResult.data, meta);
      }
      lastError = influxResult.error || "InfluxDB returned no data";
      continue;
    }
  }

  // All tiers exhausted
  const meta = await buildMeta("fallback");
  return errorResponse(meta, "ALL_SOURCES_FAILED",
    `All data sources failed. Last error: ${lastError}`,
    "Provide a log_path, ensure MTL is running, or check InfluxDB");
}
