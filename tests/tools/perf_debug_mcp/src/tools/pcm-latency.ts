/**
 * pcm_latency — DDR/PMM memory access latency measurement via pcm-latency CLI.
 *
 * Unlike the pcm-sensor-server-based tools, this runs the pcm-latency binary
 * directly (locally or via SSH) because the sensor server does not expose
 * latency data.
 *
 * Measures:
 *   - DDR memory access latency per socket (nanoseconds)
 *   - Optionally: Persistent Memory (PMM) latency
 *
 * Requires: pcm-latency binary in PATH, root/sudo access.
 * Known limitation: pcm-latency fails if any CPU cores are offline.
 */
import type { ToolResponse, PcmLatencyData, PcmLatencySocketData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

function parseLatencyOutput(raw: string, pmm: boolean): PcmLatencySocketData[] {
  const sockets: PcmLatencySocketData[] = [];

  // pcm-latency prints lines like:
  //  Socket 0: DDR Latency: 76.42 ns
  //  Socket 1: DDR Latency: 83.17 ns
  // or with PMM:
  //  Socket 0: DDR Latency: 76.42 ns  PMM Read Latency: 170.5 ns
  const latencyRegex = /Socket\s+(\d+).*?(?:DDR|DRAM)\s+Latency:\s*([\d.]+)\s*ns/gi;
  const pmmRegex = /Socket\s+(\d+).*?PMM\s+(?:Read\s+)?Latency:\s*([\d.]+)\s*ns/gi;

  let match;
  while ((match = latencyRegex.exec(raw)) !== null) {
    const socketId = parseInt(match[1], 10);
    const ddrNs = parseFloat(match[2]);
    const entry: PcmLatencySocketData = { socket: socketId, ddr_latency_ns: ddrNs };
    sockets.push(entry);
  }

  if (pmm) {
    while ((match = pmmRegex.exec(raw)) !== null) {
      const socketId = parseInt(match[1], 10);
      const pmmNs = parseFloat(match[2]);
      const existing = sockets.find(s => s.socket === socketId);
      if (existing) {
        existing.pmm_latency_ns = pmmNs;
      }
    }
  }

  return sockets;
}

export async function pcmLatency(params: {
  pmm?: boolean;
  host?: string;
}): Promise<ToolResponse<PcmLatencyData>> {
  const host = params.host ?? "localhost";
  const pmm = params.pmm ?? false;

  const meta = await buildMeta("fallback");

  const pmmFlag = pmm ? " --PMM" : "";
  const cmd = `pcm-latency -i=1${pmmFlag} 2>&1`;

  const result = await sshExec(host, cmd, 20_000);
  const raw = result.stdout;

  // Check for common failure modes
  if (raw.includes("Number of online cores should be equal to number of available cores")) {
    return errorResponse(
      meta,
      "PCM_OFFLINE_CORES",
      "pcm-latency requires all CPU cores to be online. Offline cores were detected.",
      "Bring all cores online: echo 1 > /sys/devices/system/cpu/cpu<N>/online"
    );
  }

  if (result.exitCode !== 0 && !raw.includes("Latency")) {
    // Check if pcm-latency exists
    if (raw.includes("not found") || raw.includes("No such file")) {
      return errorResponse(
        meta,
        "PCM_NOT_FOUND",
        "pcm-latency binary not found",
        "Install Intel PCM: https://github.com/intel/pcm"
      );
    }
    return errorResponse(
      meta,
      "PCM_EXEC_ERROR",
      `pcm-latency failed (exit ${result.exitCode}): ${raw.slice(0, 300)}`,
      "Ensure pcm-latency is installed and you have root privileges"
    );
  }

  const sockets = parseLatencyOutput(raw, pmm);

  if (sockets.length === 0) {
    return okResponse(
      { sockets: [], raw_output: raw.slice(0, 1000), note: "No latency data found in output" },
      meta
    );
  }

  return okResponse({ sockets, raw_output: raw.slice(0, 2000) }, meta);
}
