/**
 * rdt_memory_bandwidth — Per-core memory bandwidth via Intel RDT (MBM).
 *
 * Uses pqos (intel-cmt-cat) to measure per-core local (MBL) and remote (MBR)
 * memory bandwidth in MB/s using hardware MBM counters.
 *
 * More accurate than software-based bandwidth measurement because MBM uses
 * dedicated Intel hardware counters at the uncore level, not approximated
 * from LLC miss counts.
 *
 * Key use cases:
 *   - Identify memory-bandwidth-hungry cores
 *   - Detect NUMA imbalance (high remote BW)
 *   - Verify MBA (Memory Bandwidth Allocation) throttling
 *   - Complement pcm_memory_bandwidth (which is socket-aggregate)
 *
 * Requires: intel-cmt-cat package (pqos), root.
 */
import type { ToolResponse, RdtMemBwData, RdtMemBwEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec, sshExecSafe } from "../utils/ssh-exec.js";

export async function rdtMemoryBandwidth(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
}): Promise<ToolResponse<RdtMemBwData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 1;
  const coresParam = params.cores ?? "0-55";

  const meta = await buildMeta("fallback", duration * 1000);

  const which = await sshExecSafe(host, "command -v pqos 2>/dev/null", 5_000);
  if (!which || !which.trim()) {
    return errorResponse(
      meta,
      "PQOS_MISSING",
      "pqos (intel-cmt-cat) not found on target host",
      "Install intel-cmt-cat: apt-get install intel-cmt-cat",
    );
  }

  // Reset monitoring state first, then monitor.
  // Use -I for OS interface (not MSR).
  // Wrap pqos with `timeout` because on high-core-count systems pqos can
  // take much longer than the requested duration (large output), causing
  // the Node.js execFile timeout to kill it.  Use sshExec (not sshExecSafe)
  // so we still get stdout even if pqos exits non-zero or is killed.
  const killAfter = duration + 5;
  const cmd = `pqos -I -r 2>/dev/null; timeout ${killAfter} pqos -I -m "all:${coresParam}" -t ${duration} 2>/dev/null`;
  const timeoutMs = (killAfter + 10) * 1000;
  const result = await sshExec(host, cmd, timeoutMs);
  const output = result.stdout;

  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "PQOS_NO_OUTPUT",
      "pqos monitoring produced no output" +
        (result.exitCode !== 0 ? ` (exit ${result.exitCode})` : ""),
      "Ensure root access. Check if RDT/MBM is supported: pqos -I -d",
    );
  }

  // Parse the last snapshot block
  const blocks = output.split(/^TIME\s+/m);
  const lastBlock = blocks[blocks.length - 1] || "";
  const entries: RdtMemBwEntry[] = [];

  const lines = lastBlock.split("\n");
  for (const line of lines) {
    // Format: CORE  IPC  MISSES  LLC[KB]  MBL[MB/s]  MBR[MB/s]
    const match = line.match(
      /^\s*(\d+)\s+[\d.]+\s+[\d.]+k\s+[\d.]+\s+([\d.]+)\s+([\d.]+)/
    );
    if (match) {
      const mbl = parseFloat(match[2]);
      const mbr = parseFloat(match[3]);
      entries.push({
        core: parseInt(match[1], 10),
        mbl_mbps: mbl,
        mbr_mbps: mbr,
        mbt_mbps: Math.round((mbl + mbr) * 100) / 100,
      });
    }
  }

  if (entries.length === 0) {
    return errorResponse(
      meta,
      "PQOS_PARSE_ERROR",
      "Could not parse pqos bandwidth output",
      "Raw output: " + output.slice(0, 500),
    );
  }

  const warnings: string[] = [];

  // Sort by total BW descending for top-talker analysis
  const sorted = [...entries].sort((a, b) => b.mbt_mbps - a.mbt_mbps);
  const topCore = sorted[0];
  if (topCore && topCore.mbt_mbps > 1000) {
    warnings.push(
      `Core ${topCore.core}: ${topCore.mbt_mbps} MB/s total memory BW — bandwidth-intensive`,
    );
  }

  // Detect remote BW dominance
  for (const e of entries) {
    if (e.mbr_mbps > 0 && e.mbl_mbps > 0) {
      const remotePct = (e.mbr_mbps / e.mbt_mbps) * 100;
      if (remotePct > 30) {
        warnings.push(
          `Core ${e.core}: ${remotePct.toFixed(0)}% remote memory BW — consider NUMA pinning`,
        );
      }
    }
  }

  return okResponse<RdtMemBwData>({
    duration_sec: duration,
    cores_monitored: coresParam,
    entries,
    warnings,
  }, meta);
}
