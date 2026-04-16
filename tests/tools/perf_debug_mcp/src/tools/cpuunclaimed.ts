/**
 * cpuunclaimed — Detect idle CPUs while tasks are waiting (scheduler imbalance).
 *
 * Samples all run queues and compares against idle CPUs.  If there are tasks
 * waiting in one CPU's run queue while another CPU is idle, those are
 * "unclaimed" cycles — the scheduler failed to migrate work optimally.
 *
 * Note: cpuunclaimed can produce timing-skew errors on systems with aggressive
 * power management (C-states).  The tool handles this gracefully.
 *
 * Source: BCC `cpuunclaimed`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, CpuUnclaimedData, CpuUnclaimedSample } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function cpuUnclaimed(params: {
  host?: string;
  duration_sec?: number;
  sample_interval_ms?: number;
}): Promise<ToolResponse<CpuUnclaimedData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const interval = params.sample_interval_ms ?? 1000;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("cpuunclaimed");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "CPUUNCLAIMED_MISSING",
      "cpuunclaimed (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  // cpuunclaimed <interval_sec> <count> — -T for timestamps
  const intervalSec = Math.max(1, Math.round(interval / 1000));
  const count = Math.max(1, Math.round(duration / intervalSec));
  const cmd = `${binary} -T ${intervalSec} ${count} 2>&1`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "CPUUNCLAIMED_NO_OUTPUT",
      "cpuunclaimed produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Check kernel BPF tracepoint support.",
    );
  }

  const raw = output.trim();

  // Handle timing-skew gracefully — this is common with aggressive C-states
  // and does NOT mean the tool failed completely.  Return partial data + warning.
  if (raw.includes("CPU samples arrived at skewed offsets")) {
    return okResponse<CpuUnclaimedData>({
      duration_sec: duration,
      samples: [],
      avg_idle_pct: 0,
      warnings: [
        "cpuunclaimed detected CPU timing skew (CPU samples arrived at skewed offsets). " +
        "This is common on systems with aggressive C-states — CPUs powered down when idle " +
        "cause TSC skew. Try: echo 1 > /sys/devices/system/cpu/intel_idle/max_cstate to limit C-states, " +
        "or use runq_latency / core_load_snapshot instead.",
      ],
      raw_output: raw.slice(0, 3000),
    }, meta);
  }

  // Parse output lines:
  // TIME       %CPU  0   1   2   3  ... (run queue lengths per CPU)
  // or: %CPU idle: 95.46%  (may appear as summary)
  const samples: CpuUnclaimedSample[] = [];
  const lines = raw.split("\n");

  for (const line of lines) {
    const trimmed = line.trim();
    // Sample lines look like: "12:34:56  95.46  0 0 1 0 ..."
    const match = trimmed.match(/^(\d{2}:\d{2}:\d{2})\s+([\d.]+)%?\s+(.*)/);
    if (match) {
      const timestamp = match[1];
      const idlePct = parseFloat(match[2]);
      const cpuValues = match[3].trim().split(/\s+/).map(Number);
      const busyCpus = cpuValues.filter((v) => v > 0).length;
      samples.push({
        timestamp,
        idle_pct: idlePct,
        busy_cpus: busyCpus,
        total_cpus: cpuValues.length,
      });
    }
  }

  const avgIdle = samples.length > 0
    ? Math.round((samples.reduce((s, x) => s + x.idle_pct, 0) / samples.length) * 100) / 100
    : 0;

  const warnings: string[] = [];
  if (avgIdle < 50) {
    warnings.push(`Average idle ${avgIdle}% — CPUs are mostly busy, scheduler imbalance may be impactful`);
  }

  return okResponse<CpuUnclaimedData>({
    duration_sec: duration,
    samples,
    avg_idle_pct: avgIdle,
    warnings,
    raw_output: raw.slice(0, 3000),
  }, meta);
}
