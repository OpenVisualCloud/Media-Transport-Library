/**
 * mtl_session_stats — Parse MTL stat dumps from log files.
 *
 * MTL writes periodic stat dumps (every ~10s) to stdout/stderr (often redirected
 * to log files in /dev/shm/ or other configurable directories).
 *
 * The dumps are delimited by:
 *   "M T    D E V   S T A T E"  (begin)
 *   "E N D    S T A T E"       (end)
 *
 * Between these markers, lines contain per-port rates, per-scheduler loop times,
 * CNI rates, PTP timestamps, and per-video-session stats.
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - log_path: path to the log file containing MTL stat dumps
 *   - tail_lines: how many lines from the end of the log to parse (default 200)
 */
import type { ToolResponse } from "../types.js";
import type {
  MtlSessionStatsData,
  MtlStatDump,
  MtlDevStats,
  MtlSchStats,
  MtlSchTaskletStats,
  MtlCniStats,
  MtlPtpStats,
  MtlVideoSessionStats,
  MtlSessionStatsTrend,
  MtlSessionStatsWarning,
  MtlSessionAggregate,
  MtlAggregateStats,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/**
 * Parse a single MTL stat dump block (lines between STATE markers).
 */
export function parseMtlStatBlock(lines: string[]): MtlStatDump {
  const devStats: MtlDevStats[] = [];
  const schStats: MtlSchStats[] = [];
  const cniStats: MtlCniStats[] = [];
  const ptpStats: MtlPtpStats[] = [];
  const videoSessions: MtlVideoSessionStats[] = [];
  let timestamp = "";

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    // Extract timestamp from any MTL: line
    if (!timestamp) {
      const tsMatch = line.match(/MTL:\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})/);
      if (tsMatch) timestamp = tsMatch[1];
    }

    // ── DEV rate line ────────────────────────────────────────────────
    // DEV(n): Avr rate, tx: X Mb/s, rx: Y Mb/s, pkts, tx: T, rx: R
    const devMatch = line.match(
      /DEV\((\d+)\):\s*Avr rate,\s*tx:\s*([\d.]+)\s*Mb\/s,\s*rx:\s*([\d.]+)\s*Mb\/s,\s*pkts,\s*tx:\s*(\d+),\s*rx:\s*(\d+)/,
    );
    if (devMatch) {
      const devStat: MtlDevStats = {
        port_index: parseInt(devMatch[1], 10),
        tx_rate_mbps: parseFloat(devMatch[2]),
        rx_rate_mbps: parseFloat(devMatch[3]),
        tx_pkts: parseInt(devMatch[4], 10),
        rx_pkts: parseInt(devMatch[5], 10),
      };

      // [BUG FIX] Look ahead for DEV error/status line:
      //   err("DEV(%d): Status: rx_hw_dropped_packets N rx_err_packets N rx_nombuf_packets N tx_err_packets N")
      // Logged at err() level → may appear as "Error: DEV(N): Status: ..."
      const portIdx = devMatch[1];
      for (let j = i + 1; j < Math.min(i + 5, lines.length); j++) {
        const errMatch = lines[j].match(
          new RegExp(`DEV\\(${portIdx}\\):\\s*Status:\\s*rx_hw_dropped_packets\\s+(\\d+)\\s+rx_err_packets\\s+(\\d+)\\s+rx_nombuf_packets\\s+(\\d+)\\s+tx_err_packets\\s+(\\d+)`),
        );
        if (errMatch) {
          devStat.rx_hw_dropped = parseInt(errMatch[1], 10);
          devStat.rx_err = parseInt(errMatch[2], 10);
          devStat.rx_nombuf = parseInt(errMatch[3], 10);
          devStat.tx_err = parseInt(errMatch[4], 10);
          break;
        }
        // Stop if we hit a different section header
        if (lines[j].match(/^(?:SCH|CNI|PTP|DEV\(\d+\):\s*Avr|[RT]X_VIDEO_SESSION)/)) break;
      }

      devStats.push(devStat);
      continue;
    }

    // ── SCH header line ──────────────────────────────────────────────
    // SCH(n:name): tasklets T, lcore L(t_pid: P), avg loop X ns
    const schMatch = line.match(
      /SCH\((\d+):([^)]+)\):\s*tasklets\s+(\d+),\s*lcore\s+(\d+)\(t_pid:\s*(\d+)\),\s*avg loop\s+(\d+)\s*ns/,
    );
    if (schMatch) {
      const sch: MtlSchStats = {
        sch_index: parseInt(schMatch[1], 10),
        name: schMatch[2],
        tasklets: parseInt(schMatch[3], 10),
        lcore: parseInt(schMatch[4], 10),
        thread_pid: parseInt(schMatch[5], 10),
        avg_loop_ns: parseInt(schMatch[6], 10),
      };

      // [BUG FIX] Look ahead for SCH timing and per-tasklet timing:
      //   SCH(N): time avg X.XXus max Y.YYus min Z.ZZus
      //   SCH(N,M): tasklet NAME, avg X.XXus max Y.YYus min Z.ZZus
      const schIdx = schMatch[1];
      const taskletDetails: MtlSchTaskletStats[] = [];
      for (let j = i + 1; j < Math.min(i + 20, lines.length); j++) {
        // Scheduler-level timing
        const schTimeMatch = lines[j].match(
          new RegExp(`SCH\\(${schIdx}\\):\\s*time avg\\s+([\\d.]+)us\\s+max\\s+([\\d.]+)us\\s+min\\s+([\\d.]+)us`),
        );
        if (schTimeMatch) {
          sch.time_avg_us = parseFloat(schTimeMatch[1]);
          sch.time_max_us = parseFloat(schTimeMatch[2]);
          sch.time_min_us = parseFloat(schTimeMatch[3]);
          continue;
        }
        // Per-tasklet timing
        const taskletMatch = lines[j].match(
          new RegExp(`SCH\\(${schIdx},(\\d+)\\):\\s*tasklet\\s+([^,]+),\\s*avg\\s+([\\d.]+)us\\s+max\\s+([\\d.]+)us\\s+min\\s+([\\d.]+)us`),
        );
        if (taskletMatch) {
          taskletDetails.push({
            tasklet_index: parseInt(taskletMatch[1], 10),
            name: taskletMatch[2].trim(),
            avg_us: parseFloat(taskletMatch[3]),
            max_us: parseFloat(taskletMatch[4]),
            min_us: parseFloat(taskletMatch[5]),
          });
          continue;
        }
        // SCH sleep line is also part of this block
        if (lines[j].match(new RegExp(`SCH\\(${schIdx}\\):\\s*sleep`))) continue;
        // Stop if we hit a different section
        if (lines[j].match(/^(?:SCH\(\d+:[^)]+\)|CNI|PTP|DEV|[RT]X_VIDEO_SESSION)/)) break;
      }
      if (taskletDetails.length > 0) sch.tasklet_details = taskletDetails;

      schStats.push(sch);
      continue;
    }

    // CNI(n): eth_rx_rate X Mb/s, eth_rx_cnt N
    const cniMatch = line.match(
      /CNI\((\d+)\):\s*eth_rx_rate\s+([\d.]+)\s*Mb\/s,\s*eth_rx_cnt\s+(\d+)/,
    );
    if (cniMatch) {
      cniStats.push({
        port_index: parseInt(cniMatch[1], 10),
        eth_rx_rate_mbps: parseFloat(cniMatch[2]),
        eth_rx_cnt: parseInt(cniMatch[3], 10),
      });
      continue;
    }

    // PTP(n): time T, YYYY-MM-DD HH:MM:SS
    const ptpMatch = line.match(
      /PTP\((\d+)\):\s*time\s+(\d+),\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})/,
    );
    if (ptpMatch) {
      ptpStats.push({
        port_index: parseInt(ptpMatch[1], 10),
        time_ns: parseInt(ptpMatch[2], 10),
        time_human: ptpMatch[3],
      });
      continue;
    }

    // ── RX_VIDEO_SESSION ─────────────────────────────────────────────
    // RX_VIDEO_SESSION(sch,idx:name): fps F frames N pkts P [slices S] [+ redundant R]
    const rxVideoMatch = line.match(
      /RX_VIDEO_SESSION\((\d+),(\d+):([^)]+)\):\s*fps\s+([\d.]+)\s+frames\s+(\d+)\s+pkts\s+(\d+)(?:\s+redundant\s+(\d+))?/,
    );
    if (rxVideoMatch) {
      const session: MtlVideoSessionStats = {
        sch_index: parseInt(rxVideoMatch[1], 10),
        session_index: parseInt(rxVideoMatch[2], 10),
        name: rxVideoMatch[3],
        fps: parseFloat(rxVideoMatch[4]),
        frames: parseInt(rxVideoMatch[5], 10),
        pkts: parseInt(rxVideoMatch[6], 10),
        redundant_pkts: rxVideoMatch[7] ? parseInt(rxVideoMatch[7], 10) : undefined,
        throughput_mbps: 0,
        cpu_busy: 0,
        burst_max: 0,
        burst_avg: 0,
      };

      // [BUG FIX] Scan forward for ALL continuation lines belonging to this session.
      // MTL prints many conditional lines per RX session — the old lookahead of 2 lines
      // missed burst, incomplete, out-of-order, and tasklet-time lines that appear at
      // positions i+3..i+7 or later.  Scan up to 25 lines (real sessions have ≤20
      // continuation lines) and stop at the next session header or section break.
      const rxPrefix = `RX_VIDEO_SESSION(${session.sch_index},${session.session_index})`;
      for (let j = i + 1; j < Math.min(i + 25, lines.length); j++) {
        const jline = lines[j];
        // Stop if we hit a new session header (has ":name") or different section
        if (jline.match(/[RT]X_VIDEO_SESSION\(\d+,\d+:[^)]+\)/) ||
            jline.match(/^(?:SCH|CNI|PTP|DEV|E N D)/) ||
            jline.includes("M T") && jline.includes("S T A T E")) break;
        // Only parse lines for the same session
        if (!jline.includes(rxPrefix)) continue;

        // throughput + cpu busy
        const tpMatch = jline.match(
          /throughput\s+([\d.]+)\s*Mb\/s,\s*cpu busy\s+([\d.]+)/,
        );
        if (tpMatch) {
          session.throughput_mbps = parseFloat(tpMatch[1]);
          session.cpu_busy = parseFloat(tpMatch[2]);
          continue;
        }

        // [BUG FIX] incomplete frames + pkt error breakdown
        const incMatch = jline.match(
          /incomplete frames\s+(\d+),\s*pkts\s*\(idx error:\s*(\d+),\s*offset error:\s*(\d+),\s*idx out of bitmap:\s*(\d+),\s*missed:\s*(\d+)\)/,
        );
        if (incMatch) {
          session.incomplete_frames = parseInt(incMatch[1], 10);
          session.idx_error = parseInt(incMatch[2], 10);
          session.offset_error = parseInt(incMatch[3], 10);
          session.idx_out_of_bitmap = parseInt(incMatch[4], 10);
          session.missed_pkts = parseInt(incMatch[5], 10);
          continue;
        }

        // [BUG FIX] out of order pkts
        const oooMatch = jline.match(/out of order pkts\s+(\d+)/);
        if (oooMatch) {
          session.out_of_order_pkts = parseInt(oooMatch[1], 10);
          continue;
        }

        // succ burst max + avg
        const burstMatch = jline.match(
          /succ burst max\s+(\d+),\s*avg\s+([\d.]+)/,
        );
        if (burstMatch) {
          session.burst_max = parseInt(burstMatch[1], 10);
          session.burst_avg = parseFloat(burstMatch[2]);
          continue;
        }

        // [BUG FIX] tasklet time avg/max/min (microseconds)
        const taskletTimeMatch = jline.match(
          /tasklet time avg\s+([\d.]+)us\s+max\s+([\d.]+)us\s+min\s+([\d.]+)us/,
        );
        if (taskletTimeMatch) {
          session.tasklet_time_avg_us = parseFloat(taskletTimeMatch[1]);
          session.tasklet_time_max_us = parseFloat(taskletTimeMatch[2]);
          session.tasklet_time_min_us = parseFloat(taskletTimeMatch[3]);
          continue;
        }
      }

      videoSessions.push(session);
      continue;
    }

    // ── TX_VIDEO_SESSION ─────────────────────────────────────────────
    // TX_VIDEO_SESSION(sch,idx:name): fps F frames N pkts P:P2 inflight I:I2
    const txVideoMatch = line.match(
      /TX_VIDEO_SESSION\((\d+),(\d+):([^)]+)\):\s*fps\s+([\d.]+)\s+frames\s+(\d+)\s+pkts\s+(\d+)/,
    );
    if (txVideoMatch) {
      const session: MtlVideoSessionStats = {
        sch_index: parseInt(txVideoMatch[1], 10),
        session_index: parseInt(txVideoMatch[2], 10),
        name: txVideoMatch[3],
        fps: parseFloat(txVideoMatch[4]),
        frames: parseInt(txVideoMatch[5], 10),
        pkts: parseInt(txVideoMatch[6], 10),
        throughput_mbps: 0,
        cpu_busy: 0,
        burst_max: 0,
        burst_avg: 0,
      };

      // [BUG FIX] Wider lookahead + fix TX throughput regex.
      // TX format: "throughput X Mb/s: Y Mb/s, cpu busy Z"  (note ": Y Mb/s" part)
      const txPrefix = `TX_VIDEO_SESSION(${session.sch_index},${session.session_index})`;
      for (let j = i + 1; j < Math.min(i + 25, lines.length); j++) {
        const jline = lines[j];
        if (jline.match(/[RT]X_VIDEO_SESSION\(\d+,\d+:[^)]+\)/) ||
            jline.match(/^(?:SCH|CNI|PTP|DEV|E N D)/) ||
            jline.includes("M T") && jline.includes("S T A T E")) break;
        if (!jline.includes(txPrefix)) continue;

        // [BUG FIX] TX throughput has two values: "throughput X Mb/s: Y Mb/s, cpu busy Z"
        // The ": Y Mb/s" part is optional (handles both TX and any future format changes).
        const tpMatch = jline.match(
          /throughput\s+([\d.]+)\s*Mb\/s(?::\s*[\d.]+\s*Mb\/s)?,\s*cpu busy\s+([\d.]+)/,
        );
        if (tpMatch) {
          session.throughput_mbps = parseFloat(tpMatch[1]);
          session.cpu_busy = parseFloat(tpMatch[2]);
          continue;
        }

        // [BUG FIX] TX tasklet time (same format as RX)
        const taskletTimeMatch = jline.match(
          /tasklet time avg\s+([\d.]+)us\s+max\s+([\d.]+)us\s+min\s+([\d.]+)us/,
        );
        if (taskletTimeMatch) {
          session.tasklet_time_avg_us = parseFloat(taskletTimeMatch[1]);
          session.tasklet_time_max_us = parseFloat(taskletTimeMatch[2]);
          session.tasklet_time_min_us = parseFloat(taskletTimeMatch[3]);
          continue;
        }
      }

      videoSessions.push(session);
      continue;
    }
  }

  return {
    timestamp,
    dev_stats: devStats,
    sch_stats: schStats,
    cni_stats: cniStats,
    ptp_stats: ptpStats,
    video_sessions: videoSessions,
  };
}

/**
 * Compute aggregate statistics from an array of numbers.
 */
function computeStats(values: number[]): { mean: number; stddev: number; min: number; max: number } | null {
  if (values.length === 0) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const mean = values.reduce((s, v) => s + v, 0) / values.length;
  const variance = values.reduce((s, v) => s + (v - mean) ** 2, 0) / values.length;
  const stddev = Math.sqrt(variance);
  return {
    mean: Math.round(mean * 100) / 100,
    stddev: Math.round(stddev * 100) / 100,
    min: Math.round(min * 100) / 100,
    max: Math.round(max * 100) / 100,
  };
}

export async function mtlSessionStats(params: {
  host?: string;
  log_path: string;
  tail_lines?: number;
  last_dumps?: number;
  alert_threshold_fps?: number;
  session_filter?: string;
}): Promise<ToolResponse<MtlSessionStatsData>> {
  const host = params.host ?? "localhost";
  const logPath = params.log_path;
  const tailLines = params.tail_lines ?? 500;
  const lastDumps = params.last_dumps ?? 5;
  const alertFps = params.alert_threshold_fps;
  const sessionFilter = params.session_filter ?? null;

  const meta = await buildMeta("fallback");

  try {
    // Read tail of log file
    const output = await sshExecSafe(host, `tail -${tailLines} ${logPath} 2>/dev/null`);
    if (!output || !output.trim()) {
      return okResponse<MtlSessionStatsData>(
        { log_file: logPath, latest_dump: null, dumps_found: 0, steady_state_dumps: 0 },
        meta,
      );
    }

    const rawOutput = output.split("\n");

    // Strip ANSI escape codes that can appear in console/log output
    const allLines = rawOutput.map(l => l.replace(/\x1b\[[0-9;]*[a-zA-Z]/g, ""));

    // Find ALL stat dump blocks (between STATE markers).
    // Use regex for flexible matching — markers may have varied spacing, log prefixes,
    // timestamps, or ANSI artifacts.  The key tokens are:
    //   Begin: "M T" + "D E V" + "S T A T E"
    //   End:   "E N D" + "S T A T E"
    const BEGIN_RE = /M\s*T.*D\s*E\s*V.*S\s*T\s*A\s*T\s*E/;
    const END_RE   = /E\s*N\s*D.*S\s*T\s*A\s*T\s*E/;

    const allDumps: MtlStatDump[] = [];
    let blockStart = -1;

    for (let i = 0; i < allLines.length; i++) {
      if (BEGIN_RE.test(allLines[i])) {
        blockStart = i + 1;
      } else if (END_RE.test(allLines[i]) && blockStart >= 0) {
        const blockLines = allLines.slice(blockStart, i);
        const dump = parseMtlStatBlock(blockLines);
        allDumps.push(dump);
        blockStart = -1;
      }
    }

    // ── CRITICAL FIX: Use only the LAST N dumps (steady-state) ──────────
    // Early dumps capture startup transients (e.g. 48fps ramp-up) which
    // corrupt A/B test comparisons.  Only the last N dumps reflect actual
    // steady-state performance.
    const dumps = allDumps.slice(-lastDumps);

    // Parse tx_build_timeouts from the full output
    let txBuildTimeouts = 0;
    for (const line of allLines) {
      if (line.includes("mask as busy") && line.includes("build time out")) {
        txBuildTimeouts++;
      }
      // Also match: "mask as busy as build timeout"
      const tmoMatch = line.match(/mask as busy.*(?:build time ?out|build_timeout)/i);
      if (tmoMatch) {
        // Already counted above if the exact pattern matched, avoid double-count
        if (!line.includes("mask as busy") || !line.includes("build time out")) {
          txBuildTimeouts++;
        }
      }
    }

    // ── Per-session aggregation across the steady-state dumps ────────────
    // Track metrics per session name across all dumps
    const sessionMetrics = new Map<string, {
      fps: number[];
      throughput: number[];
      cpu_busy: number[];
      burst_max: number[];
      burst_avg: number[];
    }>();

    for (const dump of dumps) {
      for (const session of dump.video_sessions) {
        const key = `${session.name}(${session.sch_index},${session.session_index})`;
        if (sessionFilter && !key.includes(sessionFilter) && session.name !== sessionFilter) continue;

        let metrics = sessionMetrics.get(key);
        if (!metrics) {
          metrics = { fps: [], throughput: [], cpu_busy: [], burst_max: [], burst_avg: [] };
          sessionMetrics.set(key, metrics);
        }
        metrics.fps.push(session.fps);
        if (session.throughput_mbps > 0) metrics.throughput.push(session.throughput_mbps);
        if (session.cpu_busy > 0) metrics.cpu_busy.push(session.cpu_busy);
        if (session.burst_max > 0) metrics.burst_max.push(session.burst_max);
        if (session.burst_avg > 0) metrics.burst_avg.push(session.burst_avg);
      }
    }

    // Build per-session aggregated stats
    const sessionAggregates: MtlSessionAggregate[] = [];
    for (const [sessionName, metrics] of sessionMetrics) {
      const fpsStats = computeStats(metrics.fps);
      if (!fpsStats) continue;

      const agg: MtlSessionAggregate = {
        session: sessionName,
        sample_count: metrics.fps.length,
        fps: fpsStats,
      };

      const tpStats = computeStats(metrics.throughput);
      if (tpStats) agg.throughput_mbps = tpStats;

      const cpuStats = computeStats(metrics.cpu_busy);
      if (cpuStats) agg.cpu_busy = cpuStats;

      const burstMaxStats = computeStats(metrics.burst_max);
      if (burstMaxStats) agg.burst_max = burstMaxStats;

      const burstAvgStats = computeStats(metrics.burst_avg);
      if (burstAvgStats) agg.burst_avg = burstAvgStats;

      sessionAggregates.push(agg);
    }

    // Build trend data from steady-state dumps only
    const fpsTrend: MtlSessionStatsTrend[] = [];
    const throughputTrend: MtlSessionStatsTrend[] = [];
    const allFps: number[] = [];

    for (const dump of dumps) {
      const sessions = sessionFilter
        ? dump.video_sessions.filter(s => s.name === sessionFilter || `${s.name}(${s.sch_index},${s.session_index})`.includes(sessionFilter))
        : dump.video_sessions;

      if (sessions.length > 0) {
        const session = sessions[0];
        const ts = dump.timestamp || "";
        fpsTrend.push({ timestamp: ts, value: session.fps });
        allFps.push(session.fps);
        if (session.throughput_mbps > 0) {
          throughputTrend.push({ timestamp: ts, value: session.throughput_mbps });
        }
      }
    }

    // Compute FPS statistics from steady-state dumps only
    const fpsStats = computeStats(allFps);

    // Generate warnings
    const warnings: MtlSessionStatsWarning[] = [];
    if (alertFps !== undefined && dumps.length > 0) {
      const latestDump = dumps[dumps.length - 1];
      for (const session of latestDump.video_sessions) {
        if (session.fps < alertFps) {
          warnings.push({
            session: `${session.name}(${session.sch_index},${session.session_index})`,
            severity: "error",
            message: `FPS ${session.fps} is below threshold ${alertFps}`,
          });
        }
      }
    }
    // Warn if steady-state FPS stddev is high (unstable)
    if (fpsStats && fpsStats.stddev > 2.0) {
      warnings.push({
        session: "global",
        severity: "warning",
        message: `FPS stddev is ${fpsStats.stddev} (high variability across ${allFps.length} steady-state dumps) — mean=${fpsStats.mean}, range=${fpsStats.min}–${fpsStats.max}`,
      });
    }
    if (txBuildTimeouts > 0) {
      warnings.push({
        session: "global",
        severity: txBuildTimeouts > 100 ? "error" : "warning",
        message: `${txBuildTimeouts} TX build timeouts detected (mask as busy)`,
      });
    }
    // Warn if we discarded startup dumps
    if (allDumps.length > dumps.length) {
      warnings.push({
        session: "global",
        severity: "info",
        message: `Using last ${dumps.length} of ${allDumps.length} total dumps (discarded ${allDumps.length - dumps.length} startup/early dumps)`,
      });
    }

    const result: MtlSessionStatsData = {
      log_file: logPath,
      latest_dump: dumps.length > 0 ? dumps[dumps.length - 1] : null,
      dumps_found: allDumps.length,
      steady_state_dumps: dumps.length,
    };

    if (sessionAggregates.length > 0) result.session_aggregates = sessionAggregates;
    if (fpsTrend.length > 0) result.fps_trend = fpsTrend;
    if (fpsStats) {
      result.fps_mean = fpsStats.mean;
      result.fps_min = fpsStats.min;
      result.fps_max = fpsStats.max;
      result.fps_stddev = fpsStats.stddev;
    }
    if (throughputTrend.length > 0) result.throughput_trend = throughputTrend;
    if (txBuildTimeouts > 0) result.tx_build_timeouts = txBuildTimeouts;
    if (warnings.length > 0) result.warnings = warnings;

    return okResponse<MtlSessionStatsData>(result, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_SESSION_STATS_ERROR",
      `Failed to parse MTL session stats: ${err instanceof Error ? err.message : String(err)}`,
      `Ensure the log file exists at ${logPath} on the target host`,
    );
  }
}
