/**
 * mtl-usdt-ptp.ts — Trace PTP synchronization quality via USDT probes.
 *
 * Attaches to ptp:ptp_result probe to capture PTP delta/correct_delta samples.
 * Computes statistics over the trace window.
 *
 * ptp:ptp_result probe signature (from mt_usdt_provider.d):
 *   probe ptp_result(int m_idx, int delta, int correct_delta)
 *   - m_idx: interface index
 *   - delta: raw PTP offset in nanoseconds
 *   - correct_delta: corrected PTP offset in nanoseconds
 *
 * Also attaches to ptp:ptp_msg for additional PTP message visibility.
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import type { UsdtPtpTraceData, UsdtPtpSample } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export const mtlUsdtPtpSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process. If omitted, discovers automatically.",
  ),
  duration_sec: z.number().min(2).max(60).optional().describe(
    "Trace duration in seconds (default: 10)",
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

export async function mtlUsdtPtp(
  params: z.infer<typeof mtlUsdtPtpSchema>,
): Promise<ToolResponse<UsdtPtpTraceData>> {
  const bridge = getBpftraceBridge();
  const meta = await buildMeta("usdt");

  if (!bridge.isAvailable || !bridge.libmtlPath) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      bridge.isAvailable ? "libmtl.so not found" : "bpftrace not available",
      "Install bpftrace and MTL with USDT support");
  }

  const durationSec = params.duration_sec ?? 10;

  // Discover PID
  let pid = params.pid;
  let processName = "";
  if (!pid) {
    const procs = await discoverMtlPids();
    if (procs.length === 0) {
      return errorResponse(meta, "NO_MTL_PROCESS", "No running MTL processes found");
    }
    pid = procs[0].pid;
    processName = procs[0].name;
  } else {
    const comm = await sshExecSafe("localhost", `cat /proc/${pid}/comm 2>/dev/null`);
    processName = comm?.trim() || "";
  }

  // bpftrace script:
  //   ptp:ptp_result(int m_idx, int delta, int correct_delta)
  const script = `
usdt:${bridge.libmtlPath}:ptp:ptp_result {
  printf("PTP_RESULT:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs);
}
`;

  const result = await bridge.runScript(script, pid, durationSec * 1000);

  if (result.exitCode !== 0 && !result.timedOut && !result.stdout.includes("PTP_RESULT:")) {
    return errorResponse(meta, "BPFTRACE_FAILED",
      `bpftrace exited with code ${result.exitCode}: ${result.stderr.slice(0, 300)}`);
  }

  // Parse PTP samples
  const samples: UsdtPtpSample[] = [];
  for (const line of result.stdout.split("\n")) {
    if (!line.startsWith("PTP_RESULT:")) continue;
    const parts = line.slice("PTP_RESULT:".length).split(":");
    if (parts.length < 4) continue;
    // m_idx, delta, correct_delta, nsecs
    samples.push({
      timestamp_ns: parseInt(parts[3], 10),
      delta_ns: parseInt(parts[1], 10),
      correct_delta_ns: parseInt(parts[2], 10),
    });
  }

  // Compute stats
  let stats: UsdtPtpTraceData["stats"];
  if (samples.length > 0) {
    const deltas = samples.map(s => s.delta_ns);
    const corrDeltas = samples.map(s => s.correct_delta_ns);

    const avg = (arr: number[]) => arr.reduce((s, v) => s + v, 0) / arr.length;
    const stddev = (arr: number[], mean: number) =>
      Math.sqrt(arr.reduce((s, v) => s + (v - mean) ** 2, 0) / arr.length);

    const deltaAvg = avg(deltas);
    stats = {
      delta_avg_ns: Math.round(deltaAvg),
      delta_min_ns: Math.min(...deltas),
      delta_max_ns: Math.max(...deltas),
      delta_stddev_ns: Math.round(stddev(deltas, deltaAvg)),
      correct_delta_avg_ns: Math.round(avg(corrDeltas)),
    };
  }

  return okResponse<UsdtPtpTraceData>({
    pid,
    process_name: processName,
    duration_ms: durationSec * 1000,
    samples,
    sample_count: samples.length,
    stats,
  }, meta);
}
