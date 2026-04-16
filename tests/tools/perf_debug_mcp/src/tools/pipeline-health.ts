/**
 * pipeline_health — Composite health check for MTL/RDMA media pipelines.
 *
 * Discovers running MTL instances, checks process liveness, parses FPS/drops
 * from stat dumps and SHM JSON files, checks HTTP stats endpoints, verifies
 * thumbnail freshness, and scans logs for recent errors.
 *
 * Data source fallback order: USDT → logs → SHM JSON → HTTP endpoints
 *
 * This tool is generic — it discovers pipelines dynamically from running MTL
 * processes and their associated log/stats files.  No hardcoded pipeline names.
 *
 * Data sources:
 *   - USDT probes via bpftrace (preferred — live FPS + timing data)
 *   - /proc thread scan (mtl_sch_* threads) — process discovery
 *   - Log stat dump parsing via parseMtlStatBlock()
 *   - /dev/shm/*stats*.json — SHM stats files
 *   - HTTP endpoints — liveness probes
 *   - Thumbnail JPEG mtime — freshness check
 *   - Log files — error scanning (filtering stat dumps)
 */
import type { ToolResponse } from "../types.js";
import type {
  PipelineHealthData,
  PipelineProcessInfo,
  PipelineLogHealth,
  PipelineShmStats,
  PipelineHttpEndpoint,
  PipelineThumbnail,
  PipelineHealthWarning,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";

/* ── constants ───────────────────────────────────────────────────────────── */

const STANDARD_FPS_RATES = [23.976, 24, 25, 29.97, 30, 50, 59.94, 60];

/** Patterns that indicate real errors (case-insensitive) */
const REAL_ERROR_RE = /\b(fail(?!over)|timeout|abort|fatal|panic|exception|segfault|refused|broken\s*pipe|SIGKILL|SIGSEGV|core\s+dump)\b/i;

/** Patterns that indicate MTL stat dump lines masquerading as "Error:" */
const STAT_DUMP_RE = /MTL:.*Error:.*(?:Status:.*_packets|rx_good_|tx_good_|rx_bytes|rx_multicast|rx_nombuf|SCH\()/;

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Discover MTL processes by scanning for threads named mtl_sch_*.
 */
async function discoverProcesses(host: string): Promise<PipelineProcessInfo[]> {
  const script = `
for pid in /proc/[0-9]*/; do
  pid_num=\${pid#/proc/}
  pid_num=\${pid_num%/}
  found=0
  lcores=""
  count=0
  if [ -d "$pid/task" ]; then
    for tid_dir in $pid/task/[0-9]*/; do
      tcomm=$(cat "$tid_dir/comm" 2>/dev/null)
      if [ "\${tcomm#mtl_sch_}" != "$tcomm" ]; then
        if [ $found -eq 0 ]; then
          pcomm=$(cat "/proc/$pid_num/comm" 2>/dev/null)
          echo "PID:$pid_num|$pcomm"
          found=1
        fi
        cpu=$(awk '{print $39}' "$tid_dir/stat" 2>/dev/null)
        lcores="$lcores $cpu"
        count=$((count+1))
      fi
    done
    if [ $found -eq 1 ]; then
      echo "INFO:$count|$lcores"
    fi
  fi
done 2>/dev/null`;

  const output = await sshExecSafe(host, script, 15_000);
  if (!output || !output.trim()) return [];

  const processes: PipelineProcessInfo[] = [];
  let current: PipelineProcessInfo | null = null;

  for (const line of output.split("\n")) {
    if (line.startsWith("PID:")) {
      const parts = line.slice(4).split("|");
      current = {
        pid: parseInt(parts[0], 10),
        comm: parts[1] ?? "",
        scheduler_count: 0,
        lcores: [],
      };
      processes.push(current);
    } else if (line.startsWith("INFO:") && current) {
      const parts = line.slice(5).split("|");
      current.scheduler_count = parseInt(parts[0], 10) || 0;
      current.lcores = (parts[1] ?? "")
        .trim()
        .split(/\s+/)
        .filter(Boolean)
        .map((s) => parseInt(s, 10))
        .filter((n) => !isNaN(n));
    }
  }

  return processes;
}

/**
 * Resolve VF BDFs and PF netdev for each MTL process by scanning /proc/<pid>/fd
 * for VFIO device symlinks pointing to PCI VF BDFs.
 */
async function resolveVfMapping(
  host: string,
  processes: PipelineProcessInfo[],
): Promise<void> {
  if (processes.length === 0) return;
  const pids = processes.map((p) => p.pid).join(" ");
  // For each PID, check /proc/<pid>/fd for VFIO group symlinks -> BDF
  const script = `for pid in ${pids}; do
  echo "PID:$pid"
  for fd in /proc/$pid/fd/*; do
    target=$(readlink "$fd" 2>/dev/null)
    case "$target" in /dev/vfio/*)
      grpid=\${target#/dev/vfio/}
      # Find the BDF(s) in this VFIO group
      if [ -d "/sys/kernel/iommu_groups/$grpid/devices" ]; then
        for dev in /sys/kernel/iommu_groups/$grpid/devices/*; do
          bdf=$(basename "$dev")
          # Check if it's a VF (has physfn pointer)
          if [ -L "$dev/physfn" ]; then
            pf_bdf=$(basename "$(readlink "$dev/physfn")")
            pf_net=$(ls /sys/bus/pci/devices/$pf_bdf/net/ 2>/dev/null | head -1)
            echo "VF:$bdf|$pf_net"
          fi
        done
      fi
    ;; esac
  done 2>/dev/null
done 2>/dev/null`;

  const output = await sshExecSafe(host, script, 10_000);
  if (!output) return;

  const pidMap = new Map(processes.map((p) => [p.pid, p]));
  let currentProc: PipelineProcessInfo | null = null;

  for (const line of output.split("\n")) {
    if (line.startsWith("PID:")) {
      const pid = parseInt(line.slice(4), 10);
      currentProc = pidMap.get(pid) ?? null;
    } else if (line.startsWith("VF:") && currentProc) {
      const parts = line.slice(3).split("|");
      const bdf = parts[0];
      const pfNetdev = parts[1] || undefined;
      if (!currentProc.vf_bdfs) currentProc.vf_bdfs = [];
      if (!currentProc.vf_bdfs.includes(bdf)) currentProc.vf_bdfs.push(bdf);
      if (pfNetdev && !currentProc.pf_netdev) currentProc.pf_netdev = pfNetdev;
    }
  }
}

/**
 * Discover log files in given directories (or default /dev/shm patterns).
 */
async function discoverLogFiles(host: string, logDirs?: string[]): Promise<string[]> {
  let cmd: string;
  if (logDirs && logDirs.length > 0) {
    const finds = logDirs.map((d) => `find "${d}" -name '*.log' -maxdepth 2 2>/dev/null`).join("; ");
    cmd = finds;
  } else {
    // Discover /dev/shm/*_logs/ directories dynamically
    cmd = `find /dev/shm -maxdepth 2 -name '*.log' -path '*_logs/*' 2>/dev/null; find /tmp -maxdepth 2 -name '*.log' -newer /proc/1/stat 2>/dev/null | head -20`;
  }

  const output = await sshExecSafe(host, cmd, 10_000);
  if (!output) return [];
  return output.trim().split("\n").filter(Boolean).sort();
}

/**
 * Analyze a log file: parse latest stat dump for FPS and count real errors.
 */
async function analyzeLog(
  host: string,
  logPath: string,
  windowMin: number,
): Promise<PipelineLogHealth> {
  const output = await sshExecSafe(host, `tail -500 "${logPath}" 2>/dev/null`, 10_000);
  const totalLines = output ? output.split("\n").length : 0;

  const result: PipelineLogHealth = {
    log_path: logPath,
    total_lines: totalLines,
    real_errors: 0,
    stat_dump_info_lines: 0,
  };

  if (!output || !output.trim()) return result;

  const lines = output.split("\n");
  const cutoff = Date.now() - windowMin * 60_000;
  let inStatBlock = false;

  // Parse the last stat block for FPS
  let lastBlockLines: string[] = [];
  let currentBlock: string[] = [];

  for (const line of lines) {
    // Detect stat dump block boundaries
    if (line.includes("M T") && line.includes("D E V") && line.includes("S T A T E")) {
      inStatBlock = true;
      currentBlock = [];
      continue;
    }
    if (line.includes("E N D") && line.includes("S T A T E") && inStatBlock) {
      lastBlockLines = currentBlock;
      inStatBlock = false;
      continue;
    }
    if (inStatBlock) {
      currentBlock.push(line);
      continue;
    }

    // Check timestamp within window
    const tsMatch = line.match(/MTL:\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})/);
    let lineTime: number | null = null;
    if (tsMatch) {
      lineTime = new Date(tsMatch[1].replace(" ", "T") + "Z").getTime();
    }

    // Classify line
    if (STAT_DUMP_RE.test(line)) {
      result.stat_dump_info_lines++;
    } else if (REAL_ERROR_RE.test(line)) {
      // Only count if within time window (or no timestamp to filter by)
      if (lineTime === null || lineTime >= cutoff) {
        result.real_errors++;
        result.last_error = line.trim();
      }
    }
  }

  // Parse last stat block for FPS/throughput
  if (lastBlockLines.length > 0) {
    const dump = parseMtlStatBlock(lastBlockLines);
    if (dump.video_sessions.length > 0) {
      const lastSession = dump.video_sessions[dump.video_sessions.length - 1];
      result.last_fps = lastSession.fps;
      result.last_throughput_mbps = lastSession.throughput_mbps || undefined;
    }
  }

  // App-level FPS fallback: when no MTL stat dump has FPS, scan for
  // fps=<float>, FPS: <float>, [COMP] ... fps=<float>
  if (result.last_fps === undefined) {
    const fpsPatterns = [
      /fps[=:]\s*([\d.]+)/i,
      /\[COMP\].*fps[=:]\s*([\d.]+)/i,
      /FPS\s*[:=]\s*([\d.]+)/i,
    ];
    // Scan from bottom up for latest FPS line
    for (let i = lines.length - 1; i >= 0; i--) {
      for (const re of fpsPatterns) {
        const m = lines[i].match(re);
        if (m) {
          const fps = parseFloat(m[1]);
          if (!isNaN(fps) && fps > 0 && fps < 1000) {
            result.app_fps = fps;
            break;
          }
        }
      }
      if (result.app_fps !== undefined) break;
    }
  }

  return result;
}

/**
 * Discover and read SHM stats JSON files.
 */
async function readShmStats(host: string, pattern: string): Promise<PipelineShmStats[]> {
  const now = Math.floor(Date.now() / 1000);
  const script = `for f in ${pattern}; do [ -f "$f" ] && echo "FILE:$f|$(stat -c '%Y' "$f" 2>/dev/null)|$(cat "$f" 2>/dev/null)"; done 2>/dev/null`;

  const output = await sshExecSafe(host, script, 10_000);
  if (!output || !output.trim()) return [];

  const stats: PipelineShmStats[] = [];
  for (const line of output.split("\n")) {
    if (!line.startsWith("FILE:")) continue;
    const pipeIdx = line.indexOf("|", 5);
    if (pipeIdx < 0) continue;
    const path = line.slice(5, pipeIdx);
    const rest = line.slice(pipeIdx + 1);
    const pipeIdx2 = rest.indexOf("|");
    if (pipeIdx2 < 0) continue;

    const mtime = parseInt(rest.slice(0, pipeIdx2), 10);
    const jsonStr = rest.slice(pipeIdx2 + 1);

    let data: Record<string, number | string> = {};
    try {
      data = JSON.parse(jsonStr);
    } catch {
      data = { _parse_error: "invalid JSON" };
    }

    stats.push({
      path,
      age_sec: now - mtime,
      data,
    });
  }

  return stats;
}

/**
 * Probe HTTP endpoints via wget.
 */
async function probeHttpEndpoints(
  host: string,
  endpoints?: Array<{ url: string; label: string }>,
): Promise<PipelineHttpEndpoint[]> {
  const results: PipelineHttpEndpoint[] = [];

  if (!endpoints || endpoints.length === 0) {
    // Auto-discover by scanning common ports
    const ports = [8081, 8082, 8083, 8084, 8085, 8086, 8087, 8088, 8089, 9916, 9999];
    const connHost = host === "localhost" || host === "127.0.0.1" ? "localhost" : host;
    const portCheck = await sshExecSafe(
      host,
      `for p in ${ports.join(" ")}; do (echo >/dev/tcp/${connHost}/$p) 2>/dev/null && echo "OPEN:$p"; done`,
      5_000,
    );

    if (portCheck) {
      for (const line of portCheck.split("\n")) {
        const m = line.match(/^OPEN:(\d+)$/);
        if (m) {
          endpoints = endpoints || [];
          const port = m[1];
          const connHost2 = connHost;
          // Try multiple paths per port
          for (const path of ["/stats", "/health", "/", "/target_info.json"]) {
            endpoints.push({ url: `http://${connHost2}:${port}${path}`, label: `port-${port}${path}` });
          }
        }
      }
    }
  }

  if (!endpoints || endpoints.length === 0) return results;

  for (const ep of endpoints) {
    const output = await sshExecSafe(
      host,
      `wget -q -O- --timeout=2 "${ep.url}" 2>/dev/null`,
      5_000,
    );

    const entry: PipelineHttpEndpoint = {
      label: ep.label,
      url: ep.url,
      reachable: output !== null && output.trim().length > 0,
    };

    if (entry.reachable && output) {
      try {
        entry.response_data = JSON.parse(output);
      } catch {
        // Not JSON — just note it's reachable, don't include raw text
      }
    }

    results.push(entry);
  }

  return results;
}

/**
 * Check thumbnail freshness.
 */
async function checkThumbnails(
  host: string,
  thumbnailDirs?: string[],
  maxAge: number = 5,
): Promise<PipelineThumbnail[]> {
  let cmd: string;
  if (thumbnailDirs && thumbnailDirs.length > 0) {
    const finds = thumbnailDirs.map((d) => `find "${d}" -name '*.jpg' -o -name '*.jpeg' 2>/dev/null`).join("; ");
    cmd = finds;
  } else {
    // Discover /dev/shm/*_thumbs*/ directories dynamically
    cmd = `find /dev/shm -maxdepth 3 \\( -name '*.jpg' -o -name '*.jpeg' \\) -path '*thumb*' 2>/dev/null`;
  }

  const filesOutput = await sshExecSafe(host, cmd, 5_000);
  if (!filesOutput || !filesOutput.trim()) return [];

  const files = filesOutput.trim().split("\n").filter(Boolean);
  if (files.length === 0) return [];

  // Get mtime and size for all files in one call
  const statCmd = files.map((f) => `stat -c '%Y %s' "${f}" 2>/dev/null && echo "PATH:${f}"`).join("; ");
  const statOutput = await sshExecSafe(host, statCmd, 10_000);
  if (!statOutput) return [];

  const now = Math.floor(Date.now() / 1000);
  const thumbnails: PipelineThumbnail[] = [];
  const statLines = statOutput.split("\n");

  for (let i = 0; i < statLines.length; i++) {
    const line = statLines[i].trim();
    if (line.startsWith("PATH:")) {
      const path = line.slice(5);
      // Previous line should have mtime and size
      if (i > 0) {
        const parts = statLines[i - 1].trim().split(/\s+/);
        if (parts.length >= 2) {
          const mtime = parseInt(parts[0], 10);
          const size = parseInt(parts[1], 10);
          const age = now - mtime;
          thumbnails.push({
            path,
            age_sec: age,
            size_bytes: size,
            stale: age > maxAge,
          });
        }
      }
    }
  }

  return thumbnails;
}

/**
 * Find nearest standard FPS rate.
 */
function nearestStandardFps(fps: number): number {
  let nearest = STANDARD_FPS_RATES[0];
  let minDiff = Math.abs(fps - nearest);
  for (const rate of STANDARD_FPS_RATES) {
    const diff = Math.abs(fps - rate);
    if (diff < minDiff) {
      minDiff = diff;
      nearest = rate;
    }
  }
  return nearest;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function pipelineHealth(params: {
  host?: string;
  log_dirs?: string[];
  shm_stats_pattern?: string;
  thumbnail_dirs?: string[];
  http_endpoints?: Array<{ url: string; label: string }>;
  max_thumbnail_age_sec?: number;
  max_log_errors_window_min?: number;
}): Promise<ToolResponse<PipelineHealthData>> {
  const host = params.host ?? "localhost";
  const shmPattern = params.shm_stats_pattern ?? "/dev/shm/*stats*.json";
  const maxThumbAge = params.max_thumbnail_age_sec ?? 5;
  const logWindowMin = params.max_log_errors_window_min ?? 5;

  const meta = await buildMeta("fallback");

  try {
    const warnings: PipelineHealthWarning[] = [];

    // 1. Process discovery
    const processes = await discoverProcesses(host);
    if (processes.length === 0) {
      warnings.push({
        scope: "system",
        severity: "info",
        message: `No MTL processes found on ${host}`,
      });
    }

    // 1b. VF BDF resolution for each process
    await resolveVfMapping(host, processes);

    // 1c. USDT enrichment: try to get live FPS from USDT probes for each process.
    // This is faster and more accurate than log parsing for FPS detection.
    const bridge = getBpftraceBridge();
    const usdtFpsMap = new Map<number, number>(); // pid → live FPS
    if (bridge.isAvailable && bridge.libmtlPath && processes.length > 0) {
      // Quick USDT FPS check for each process (non-blocking best-effort)
      for (const proc of processes.slice(0, 4)) { // limit to 4 processes to avoid long traces
        try {
          const script = `
usdt:${bridge.libmtlPath}:sys:log_msg {
  $msg = str(arg1);
  printf("USDT_LOG:%d:%s\\n", arg0, $msg);
}
usdt:${bridge.libmtlPath}:sys:sessions_time_measure { }
`;
          const result = await bridge.runScript(script, proc.pid, 12000, { BPFTRACE_STRLEN: "512" });
          if (result.stdout.includes("USDT_LOG:")) {
            const logLines: string[] = [];
            for (const line of result.stdout.split("\n")) {
              const m = line.match(/^USDT_LOG:\d+:(.*)$/);
              if (m) logLines.push(m[1]);
            }
            let bStart = -1, bEnd = -1;
            for (let i = logLines.length - 1; i >= 0; i--) {
              if (logLines[i].includes("E N D") && logLines[i].includes("S T A T E")) bEnd = i;
              if (bEnd > 0 && logLines[i].includes("M T") && logLines[i].includes("D E V") && logLines[i].includes("S T A T E")) { bStart = i + 1; break; }
            }
            if (bStart >= 0 && bEnd > bStart) {
              const dump = parseMtlStatBlock(logLines.slice(bStart, bEnd));
              if (dump.video_sessions.length > 0) {
                usdtFpsMap.set(proc.pid, dump.video_sessions[0].fps);
              }
            }
          }
        } catch { /* USDT enrichment is best-effort */ }
      }
      if (usdtFpsMap.size > 0) {
        warnings.push({
          scope: "system",
          severity: "info",
          message: `USDT live FPS available for ${usdtFpsMap.size} process(es): ${[...usdtFpsMap.entries()].map(([p, f]) => `PID ${p} → ${f} fps`).join(", ")}`,
        });
      }
    }

    // 2. Log analysis
    const logFiles = await discoverLogFiles(host, params.log_dirs);
    const logs: PipelineLogHealth[] = [];
    for (const logPath of logFiles) {
      const logHealth = await analyzeLog(host, logPath, logWindowMin);
      logs.push(logHealth);

      // Warnings
      if (logHealth.real_errors > 10) {
        warnings.push({
          scope: logPath,
          severity: "warning",
          message: `${logPath}: ${logHealth.real_errors} errors in last ${logWindowMin} minutes`,
        });
      }
      if (logHealth.last_fps !== undefined && logHealth.last_fps > 0) {
        const expected = nearestStandardFps(logHealth.last_fps);
        if (logHealth.last_fps < expected * 0.95) {
          warnings.push({
            scope: logPath,
            severity: "warning",
            message: `${logPath}: FPS ${logHealth.last_fps} (below expected ${expected})`,
          });
        }
      }
      // Check app-level FPS if no MTL FPS available
      if (logHealth.last_fps === undefined && logHealth.app_fps !== undefined && logHealth.app_fps > 0) {
        const expected = nearestStandardFps(logHealth.app_fps);
        if (logHealth.app_fps < expected * 0.95) {
          warnings.push({
            scope: logPath,
            severity: "warning",
            message: `${logPath}: app-level FPS ${logHealth.app_fps} (below expected ${expected})`,
          });
        }
      }
    }

    // 3. SHM stats
    const shmStats = await readShmStats(host, shmPattern);
    for (const stat of shmStats) {
      const drops = stat.data.drops;
      if (typeof drops === "number" && drops > 0) {
        warnings.push({
          scope: stat.path,
          severity: "warning",
          message: `${stat.path}: ${drops} drops reported`,
        });
      }
    }

    // 4. HTTP endpoints
    const httpEndpoints = await probeHttpEndpoints(host, params.http_endpoints);
    for (const ep of httpEndpoints) {
      if (!ep.reachable) {
        warnings.push({
          scope: ep.label,
          severity: "warning",
          message: `${ep.label} (${ep.url}): unreachable`,
        });
      }
    }

    // 5. Thumbnails
    const thumbnails = await checkThumbnails(host, params.thumbnail_dirs, maxThumbAge);
    for (const thumb of thumbnails) {
      if (thumb.stale) {
        warnings.push({
          scope: thumb.path,
          severity: "warning",
          message: `${thumb.path}: stale (${thumb.age_sec}s old, threshold ${maxThumbAge}s)`,
        });
      }
    }

    return okResponse<PipelineHealthData>(
      {
        processes,
        process_count: processes.length,
        logs,
        shm_stats: shmStats,
        http_endpoints: httpEndpoints,
        thumbnails,
        warnings,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "PIPELINE_HEALTH_ERROR",
      `Failed to check pipeline health: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
