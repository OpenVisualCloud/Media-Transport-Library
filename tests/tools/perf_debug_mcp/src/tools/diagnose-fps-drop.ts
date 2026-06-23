/**
 * diagnose_fps_drop — Composite bottleneck analysis for pipeline FPS drops.
 *
 * One-shot diagnostic tool that:
 *   1. Tries USDT probes for live FPS → falls back to log parsing
 *   2. Maps process → lcores, VF BDFs, PF netdev
 *   3. Runs turbostat on those lcores → checks IPC, busy%, freq
 *   4. Runs nic_ethtool_stats on the PF → checks drops, errors
 *   5. Checks RDMA health if relevant
 *   6. Scans log for error patterns (tx_build_timeout, hw_dropped, etc.)
 *   7. Ranks suspects by severity and returns structured report
 *
 * Data source fallback order: USDT → logs → (InfluxDB planned)
 */
import type { ToolResponse, DiagnosisSuspect, DiagnoseFpsDropData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { parseMtlStatBlock } from "./mtl-session-stats.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";

/* ── helpers ─────────────────────────────────────────────────────────────── */

const STANDARD_FPS = [23.976, 24, 25, 29.97, 30, 50, 59.94, 60];

function nearestFps(fps: number): number {
  let best = STANDARD_FPS[0];
  let bestDiff = Math.abs(fps - best);
  for (const r of STANDARD_FPS) {
    const d = Math.abs(fps - r);
    if (d < bestDiff) { bestDiff = d; best = r; }
  }
  return best;
}

/**
 * Parse FPS from the last MTL stat dump in a log file, or from app-level patterns.
 */
async function parseFpsFromLog(
  host: string,
  logPath: string,
): Promise<{ fps: number | undefined; lcores: number[]; txBuildTimeouts: number; logErrors: string[] }> {
  const output = await sshExecSafe(host, `tail -1000 "${logPath}" 2>/dev/null`, 15_000);
  if (!output) return { fps: undefined, lcores: [], txBuildTimeouts: 0, logErrors: [] };

  const lines = output.split("\n");
  let fps: number | undefined;
  const lcores = new Set<number>();
  let txBuildTimeouts = 0;
  const logErrors: string[] = [];

  // Find stat dump blocks
  let blockStart = -1;
  let lastBlockLines: string[] = [];

  // Strip ANSI codes and use flexible regex for marker detection
  const BEGIN_RE = /M\s*T.*D\s*E\s*V.*S\s*T\s*A\s*T\s*E/;
  const END_RE   = /E\s*N\s*D.*S\s*T\s*A\s*T\s*E/;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].replace(/\x1b\[[0-9;]*[a-zA-Z]/g, "");

    if (BEGIN_RE.test(line)) {
      blockStart = i + 1;
    } else if (END_RE.test(line) && blockStart >= 0) {
      lastBlockLines = lines.slice(blockStart, i);
      blockStart = -1;
    }

    // Count tx_build_timeouts
    if (line.includes("mask as busy") && (line.includes("build time out") || line.includes("build_timeout"))) {
      txBuildTimeouts++;
    }

    // Collect lcore info from SCH lines
    const schMatch = line.match(/SCH\(\d+:[^)]+\):\s*tasklets\s+\d+,\s*lcore\s+(\d+)/);
    if (schMatch) lcores.add(parseInt(schMatch[1], 10));

    // Collect real error patterns
    const errorPatterns = /\b(fail|timeout|abort|fatal|hw_dropped|no available lcore|mask as busy|SIGSEGV|broken\s*pipe)\b/i;
    if (errorPatterns.test(line) && !line.includes("D E V") && !line.includes("S T A T E")) {
      logErrors.push(line.trim().slice(0, 200));
    }
  }

  // Parse last stat dump for FPS
  if (lastBlockLines.length > 0) {
    const dump = parseMtlStatBlock(lastBlockLines);
    if (dump.video_sessions.length > 0) {
      fps = dump.video_sessions[0].fps;
      for (const sch of dump.sch_stats) {
        lcores.add(sch.lcore);
      }
    }
  }

  // App-level FPS fallback
  if (fps === undefined) {
    for (let i = lines.length - 1; i >= 0; i--) {
      const m = lines[i].match(/fps[=:]\s*([\d.]+)/i);
      if (m) {
        const f = parseFloat(m[1]);
        if (!isNaN(f) && f > 0 && f < 1000) { fps = f; break; }
      }
    }
  }

  return { fps, lcores: [...lcores].sort((a, b) => a - b), txBuildTimeouts, logErrors: logErrors.slice(-20) };
}

/**
 * Resolve process PID from log path by checking which process has the log open.
 */
async function resolveProcessFromLog(host: string, logPath: string): Promise<{ pid: number; comm: string; vfBdfs: string[]; pfNetdev: string } | null> {
  // Find PID with this log file open
  const script = `fuser "${logPath}" 2>/dev/null | head -1; echo "---";
for pid in $(fuser "${logPath}" 2>&1 | awk '{for(i=1;i<=NF;i++) if($i~/^[0-9]+$/) print $i}'); do
  comm=$(cat /proc/$pid/comm 2>/dev/null)
  echo "PID:$pid|$comm"
  for fd in /proc/$pid/fd/*; do
    target=$(readlink "$fd" 2>/dev/null)
    case "$target" in /dev/vfio/*)
      grpid=\${target#/dev/vfio/}
      if [ -d "/sys/kernel/iommu_groups/$grpid/devices" ]; then
        for dev in /sys/kernel/iommu_groups/$grpid/devices/*; do
          bdf=$(basename "$dev")
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
  if (!output) return null;

  let pid = 0;
  let comm = "";
  const vfBdfs: string[] = [];
  let pfNetdev = "";

  for (const line of output.split("\n")) {
    if (line.startsWith("PID:")) {
      const parts = line.slice(4).split("|");
      pid = parseInt(parts[0], 10);
      comm = parts[1] ?? "";
    } else if (line.startsWith("VF:")) {
      const parts = line.slice(3).split("|");
      vfBdfs.push(parts[0]);
      if (parts[1] && !pfNetdev) pfNetdev = parts[1];
    }
  }

  return pid > 0 ? { pid, comm, vfBdfs, pfNetdev } : null;
}

/**
 * Quick turbostat check on specific CPUs.
 */
async function quickTurbostat(
  host: string,
  cpus: number[],
): Promise<Record<string, unknown> | null> {
  if (cpus.length === 0) return null;
  const cols = "Core,CPU,Avg_MHz,Busy%,Bzy_MHz,TSC_MHz,IPC,IRQ,SMI,C1%,C6%";
  const cmd = `turbostat --show ${cols} --interval 2 --num_iterations 1 2>/dev/null`;
  const output = await sshExecSafe(host, cmd, 15_000);
  if (!output) return null;

  const lines = output.split("\n").filter((l) => l.trim());
  if (lines.length < 2) return null;

  const headers = lines[0].split("\t");
  const cpuSet = new Set(cpus);
  // Also resolve HT siblings inline
  const expandScript = cpus
    .map((c) => `cat /sys/devices/system/cpu/cpu${c}/topology/thread_siblings_list 2>/dev/null`)
    .join("; echo '---'; ");
  const sibOutput = await sshExecSafe(host, expandScript, 3_000);
  if (sibOutput) {
    for (const block of sibOutput.split("---")) {
      for (const part of block.trim().split(",")) {
        const n = parseInt(part.trim(), 10);
        if (!isNaN(n)) cpuSet.add(n);
      }
    }
  }

  const filtered: Record<string, string>[] = [];
  for (let i = 1; i < lines.length; i++) {
    const fields = lines[i].split("\t");
    const row: Record<string, string> = {};
    for (let j = 0; j < headers.length; j++) row[headers[j]] = fields[j] ?? "";
    const cpuVal = parseInt(row["CPU"] ?? "-1", 10);
    if (cpuSet.has(cpuVal)) filtered.push(row);
  }

  return { cpu_count: filtered.length, cores: filtered };
}

/**
 * Quick ethtool 2-second delta on a NIC interface.
 * Takes two snapshots, returns both cumulative stats and per-second rates.
 */
async function quickEthtoolDelta(
  host: string,
  iface: string,
): Promise<Record<string, unknown> | null> {
  if (!iface) return null;
  const counterNames = "rx_bytes|tx_bytes|rx_dropped|tx_errors|rx_crc_errors|rx_multicast|tx_multicast";
  const grepCmd = `ethtool -S ${iface} 2>/dev/null | grep -E '(${counterNames})\\.nic:|^\\s+(${counterNames}):' | head -20`;

  const output0 = await sshExecSafe(host, grepCmd, 5_000);
  if (!output0) return null;

  const parseCounters = (out: string): Record<string, number> => {
    const stats: Record<string, number> = {};
    for (const line of out.split("\n")) {
      const m = line.match(/^\s+(\S+):\s+(\d+)/);
      if (m) stats[m[1]] = parseInt(m[2], 10);
    }
    return stats;
  };

  const stats0 = parseCounters(output0);

  // Take second sample after 2 seconds
  await new Promise((resolve) => setTimeout(resolve, 2000));
  const output1 = await sshExecSafe(host, grepCmd, 5_000);
  if (!output1) return { ...stats0, delta_seconds: 0 };

  const stats1 = parseCounters(output1);
  const elapsedSec = 2;

  // Compute deltas and rates
  const result: Record<string, number> = { ...stats1, delta_seconds: elapsedSec };
  for (const [name, val1] of Object.entries(stats1)) {
    const val0 = stats0[name];
    if (val0 !== undefined) {
      result[`${name}_delta`] = val1 - val0;
      result[`${name}_per_sec`] = Math.round((val1 - val0) / elapsedSec);
    }
  }

  return result;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function diagnoseFpsDrop(params: {
  host?: string;
  log_path: string;
  target_fps?: number;
}): Promise<ToolResponse<DiagnoseFpsDropData>> {
  const host = params.host ?? "localhost";
  const logPath = params.log_path;

  const meta = await buildMeta("fallback");

  try {
    const suspects: DiagnosisSuspect[] = [];
    const warnings: string[] = [];
    let rank = 0;
    let dataSource: "usdt" | "log" = "log";

    // 0. Try USDT first for live FPS (preferred source)
    let usdtFps: number | undefined;
    let usdtPid: number | undefined;
    const bridge = getBpftraceBridge();
    if (bridge.isAvailable && bridge.libmtlPath) {
      // Find which PID owns the log file
      const fuserOut = await sshExecSafe(host, `fuser "${logPath}" 2>/dev/null | tr -s ' ' '\\n' | grep -E '^[0-9]+$' | head -1`);
      const logPid = fuserOut ? parseInt(fuserOut.trim(), 10) : NaN;
      if (!isNaN(logPid) && logPid > 0) {
        // Quick 5s USDT trace for FPS via frame delivery events
        const script = `
usdt:${bridge.libmtlPath}:sys:log_msg {
  $msg = str(arg1);
  printf("USDT_LOG:%d:%s\\n", arg0, $msg);
}
usdt:${bridge.libmtlPath}:sys:tasklet_time_measure { }
usdt:${bridge.libmtlPath}:sys:sessions_time_measure { }
`;
        const usdtResult = await bridge.runScript(script, logPid, 15000, { BPFTRACE_STRLEN: "512" });
        if (usdtResult.stdout.includes("USDT_LOG:")) {
          // Parse for stat dump
          const logLines: string[] = [];
          for (const line of usdtResult.stdout.split("\n")) {
            const m = line.match(/^USDT_LOG:\d+:(.*)$/);
            if (m) logLines.push(m[1]);
          }
          // Find last complete stat dump
          let bStart = -1, bEnd = -1;
          for (let i = logLines.length - 1; i >= 0; i--) {
            if (logLines[i].includes("E N D") && logLines[i].includes("S T A T E")) bEnd = i;
            if (bEnd > 0 && logLines[i].includes("M T") && logLines[i].includes("D E V") && logLines[i].includes("S T A T E")) { bStart = i + 1; break; }
          }
          if (bStart >= 0 && bEnd > bStart) {
            const dump = parseMtlStatBlock(logLines.slice(bStart, bEnd));
            if (dump.video_sessions.length > 0) {
              usdtFps = dump.video_sessions[0].fps;
              usdtPid = logPid;
              dataSource = "usdt";
            }
          }
        }
      }
    }

    // 1. Parse log (always needed for error patterns and lcore discovery)
    const logData = await parseFpsFromLog(host, logPath);
    const fpsActual = usdtFps ?? logData.fps;
    const fpsTarget = params.target_fps ?? (fpsActual ? nearestFps(fpsActual) : undefined);
    const deficitPct = fpsActual !== undefined && fpsTarget !== undefined && fpsTarget > 0
      ? Math.round(((fpsTarget - fpsActual) / fpsTarget) * 100 * 10) / 10
      : undefined;

    // 2. Resolve process → VF BDFs
    const procInfo = await resolveProcessFromLog(host, logPath);
    const vfBdfs = procInfo?.vfBdfs ?? [];
    const pfNetdev = procInfo?.pfNetdev ?? "";
    const lcores = logData.lcores;

    // 3. Turbostat on relevant lcores
    let turboData: Record<string, unknown> | null = null;
    if (lcores.length > 0) {
      turboData = await quickTurbostat(host, lcores);
    }

    // 4. NIC stats
    let nicData: Record<string, unknown> | null = null;
    if (pfNetdev) {
      nicData = await quickEthtoolDelta(host, pfNetdev);
    }

    // 5. RDMA stats check (quick) — enhanced with rdma stat show
    let rdmaData: Record<string, unknown> | null = null;
    if (pfNetdev) {
      const rdmaCheck = await sshExecSafe(host, `rdma link show 2>/dev/null | grep "netdev ${pfNetdev}"`, 3_000);
      if (rdmaCheck && rdmaCheck.trim()) {
        const rdmaDev = rdmaCheck.match(/link\s+(\S+)\//);
        if (rdmaDev) {
          const devName = rdmaDev[1];
          // Try rdma stat show first (richer counters), fallback to sysfs
          const rdmaStatOutput = await sshExecSafe(
            host,
            `rdma stat show link ${devName}/1 2>/dev/null`,
            5_000,
          );
          if (rdmaStatOutput && rdmaStatOutput.trim()) {
            const stats: Record<string, number> = {};
            const kvRe = /(\w+)\s+(\d+)/g;
            let kv: RegExpExecArray | null;
            while ((kv = kvRe.exec(rdmaStatOutput)) !== null) {
              stats[kv[1]] = parseInt(kv[2], 10);
            }
            rdmaData = {
              device: devName,
              source: "rdma_stat_show",
              cnp_sent: stats["cnpSent"] ?? 0,
              cnp_received: stats["cnpReceived"] ?? 0,
              rx_ecn_marked: stats["RxECNMrkd"] ?? 0,
              out_of_sequence: stats["outOfSequence"] ?? 0,
              retrans_segs: stats["RetransSegs"] ?? 0,
              ...stats,
            };
          } else {
            // Fallback to sysfs hw_counters
            const countersOutput = await sshExecSafe(
              host,
              `cat /sys/class/infiniband/${devName}/ports/1/hw_counters/RetransSegs 2>/dev/null; echo "---"; cat /sys/class/infiniband/${devName}/ports/1/hw_counters/RxECNMrkd 2>/dev/null`,
              3_000,
            );
            if (countersOutput) {
              const parts = countersOutput.split("---");
              rdmaData = {
                device: devName,
                source: "sysfs",
                retrans_segs: parseInt(parts[0]?.trim() ?? "0", 10) || 0,
                rx_ecn_marked: parseInt(parts[1]?.trim() ?? "0", 10) || 0,
              };
            }
          }
        }
      }
    }

    // 5b. Quick nstat check — TCP retransmissions, timeouts, listen overflows
    // NOTE: These are HOST-GLOBAL counters, not per-process.  We use nstat -s
    // (delta since last nstat call) when possible to reduce noise from stale
    // cumulative counters.  Two quick calls bracketed ~1s apart give a fresh delta.
    let nstatData: Record<string, number> | null = null;
    {
      // Prime nstat history, then sample after a brief pause
      await sshExecSafe(host, `nstat -rsz 2>/dev/null`, 3_000);
      await new Promise(r => setTimeout(r, 1000));
      const nstatOutput = await sshExecSafe(
        host,
        `nstat -sz 2>/dev/null | grep -E 'TcpRetransSegs|TcpExtTCPTimeouts|TcpExtListenOverflows|TcpExtTCPSynRetrans|UdpRcvbufErrors' | grep -v '^#'`,
        5_000,
      );
      if (nstatOutput && nstatOutput.trim()) {
        const stats: Record<string, number> = {};
        for (const line of nstatOutput.split("\n")) {
          const m = line.match(/^(\S+)\s+(\d+)/);
          if (m) stats[m[1]] = parseInt(m[2], 10);
        }
        if (Object.keys(stats).length > 0) nstatData = stats;
      }
    }

    // 6. Analyze and rank suspects

    // TX build timeouts
    if (logData.txBuildTimeouts > 0) {
      rank++;
      suspects.push({
        rank,
        category: "tx_build_timeout",
        evidence: `${logData.txBuildTimeouts} TX build timeouts in log ("mask as busy")`,
        severity: logData.txBuildTimeouts > 100 ? "critical" : "warning",
      });
    }

    // IPC anomaly from turbostat
    if (turboData && Array.isArray((turboData as Record<string, unknown>).cores)) {
      const cores = (turboData as Record<string, unknown>).cores as Array<Record<string, string>>;
      const ipcs = cores
        .map((c) => ({ cpu: parseInt(c["CPU"] ?? "0", 10), ipc: parseFloat(c["IPC"] ?? "0") }))
        .filter((c) => !isNaN(c.ipc) && c.ipc > 0);

      if (ipcs.length > 1) {
        const sorted = [...ipcs.map((c) => c.ipc)].sort((a, b) => a - b);
        const mid = Math.floor(sorted.length / 2);
        const median = sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
        for (const c of ipcs) {
          const dev = ((c.ipc - median) / median) * 100;
          if (dev < -30) {
            rank++;
            suspects.push({
              rank,
              category: "ipc_anomaly",
              evidence: `CPU ${c.cpu}: IPC ${c.ipc.toFixed(2)} is ${Math.abs(Math.round(dev))}% below peer median ${median.toFixed(2)}`,
              affected_cores: [c.cpu],
              severity: "warning",
            });
          }
        }
      }

      // SMI detection
      for (const c of cores) {
        const smi = parseInt(c["SMI"] ?? "0", 10);
        if (smi > 0) {
          rank++;
          suspects.push({
            rank,
            category: "smi_stall",
            evidence: `CPU ${c["CPU"]}: ${smi} SMI(s) — invisible 1-10ms stalls`,
            affected_cores: [parseInt(c["CPU"] ?? "0", 10)],
            severity: "critical",
          });
        }
      }
    }

    // NIC contention (multicast replication)
    if (nicData && typeof nicData === "object") {
      const stats = nicData as Record<string, number>;
      const txMcast = stats["tx_multicast.nic"] ?? stats["tx_multicast"] ?? 0;
      const rxDropped = stats["rx_dropped.nic"] ?? stats["rx_dropped"] ?? 0;
      if (txMcast > 0 && vfBdfs.length > 1) {
        rank++;
        suspects.push({
          rank,
          category: "nic_internal_replication",
          evidence: `${vfBdfs.length} VFs on ${pfNetdev}, tx_multicast=${txMcast} — possible NIC-internal multicast replication bottleneck`,
          affected_nic: pfNetdev,
          severity: "warning",
        });
      }
      if (rxDropped > 0) {
        rank++;
        suspects.push({
          rank,
          category: "nic_rx_drops",
          evidence: `${pfNetdev}: ${rxDropped} rx_dropped`,
          affected_nic: pfNetdev,
          severity: "warning",
        });
      }
    }

    // RDMA issues
    if (rdmaData && typeof rdmaData === "object") {
      const rd = rdmaData as Record<string, number | string>;
      if (typeof rd.retrans_segs === "number" && rd.retrans_segs > 0) {
        rank++;
        suspects.push({
          rank,
          category: "rdma_retransmissions",
          evidence: `RDMA device ${rd.device}: ${rd.retrans_segs} retransmissions`,
          severity: "warning",
        });
      }
      if (typeof rd.cnp_sent === "number" && rd.cnp_sent > 1000) {
        rank++;
        suspects.push({
          rank,
          category: "rdma_congestion",
          evidence: `RDMA device ${rd.device}: ${rd.cnp_sent} CNP sent — PFC/ECN congestion`,
          severity: "warning",
        });
      }
      if (typeof rd.out_of_sequence === "number" && rd.out_of_sequence > 0) {
        rank++;
        suspects.push({
          rank,
          category: "rdma_out_of_sequence",
          evidence: `RDMA device ${rd.device}: ${rd.out_of_sequence} out-of-sequence packets`,
          severity: "warning",
        });
      }
    }

    // TCP/network issues from nstat (HOST-GLOBAL counters, 1-second delta)
    // These are informational — they reflect the entire host, not a specific workload.
    if (nstatData) {
      const retrans = nstatData["TcpRetransSegs"] ?? 0;
      const timeouts = nstatData["TcpExtTCPTimeouts"] ?? 0;
      const overflows = nstatData["TcpExtListenOverflows"] ?? 0;
      if (retrans > 10) {
        rank++;
        suspects.push({
          rank,
          category: "tcp_retransmissions",
          evidence: `[host-global, 1s delta] TcpRetransSegs=${retrans} — possible network congestion or packet loss (correlate with per-NIC counters)`,
          severity: retrans > 100 ? "warning" : "info",
        });
      }
      if (timeouts > 5) {
        rank++;
        suspects.push({
          rank,
          category: "tcp_timeouts",
          evidence: `[host-global, 1s delta] TcpExtTCPTimeouts=${timeouts} — connection stalls (may not relate to this workload)`,
          severity: "info",
        });
      }
      if (overflows > 0) {
        rank++;
        suspects.push({
          rank,
          category: "listen_overflows",
          evidence: `[host-global, 1s delta] TcpExtListenOverflows=${overflows} — accept queue full, connections dropped`,
          severity: "warning",
        });
      }
    }

    // Log errors
    if (logData.logErrors.length > 5) {
      rank++;
      suspects.push({
        rank,
        category: "excessive_log_errors",
        evidence: `${logData.logErrors.length} error-pattern lines in recent log tail`,
        severity: "warning",
      });
    }

    // FPS deficit warning
    if (deficitPct !== undefined && deficitPct > 5) {
      warnings.push(`FPS deficit: actual=${fpsActual?.toFixed(1)}, target=${fpsTarget?.toFixed(1)}, deficit=${deficitPct}%`);
    }

    // Sort suspects by severity (critical first)
    const severityOrder: Record<string, number> = { critical: 0, warning: 1, info: 2 };
    suspects.sort((a, b) => (severityOrder[a.severity] ?? 2) - (severityOrder[b.severity] ?? 2));
    suspects.forEach((s, i) => (s.rank = i + 1));

    return okResponse<DiagnoseFpsDropData>(
      {
        fps_actual: fpsActual,
        fps_target: fpsTarget,
        deficit_pct: deficitPct,
        log_path: logPath,
        suspects,
        raw_data: {
          turbostat: turboData ?? undefined,
          nic_stats: nicData ?? undefined,
          rdma_stats: rdmaData ?? undefined,
          nstat: nstatData ?? undefined,
          log_errors: logData.logErrors,
          tx_build_timeouts: logData.txBuildTimeouts,
          vf_bdfs: vfBdfs,
          lcores,
        },
        warnings,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "DIAGNOSE_FPS_DROP_ERROR",
      `Diagnosis failed: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
