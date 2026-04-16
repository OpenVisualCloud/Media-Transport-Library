/**
 * rdt_cache_monitoring — Per-core L3 cache occupancy and IPC via Intel RDT (CMT).
 *
 * Uses pqos (intel-cmt-cat) to monitor L3 cache occupancy (KB), LLC misses,
 * IPC, and per-core memory bandwidth (local + remote MB/s).
 *
 * This is hardware-level monitoring via MSR/resctrl — more accurate than
 * software-based LLC monitoring (e.g., perf stat) because it uses dedicated
 * Intel hardware counters (CMT = Cache Monitoring Technology, MBM = Memory
 * Bandwidth Monitoring).
 *
 * Key use cases:
 *   - See which cores/tasks consume the most LLC
 *   - Detect noisy-neighbor cache pollution
 *   - Verify CAT (Cache Allocation Technology) is working as intended
 *
 * Requires: intel-cmt-cat package (pqos), root.
 */
import type { ToolResponse, RdtCacheMonData, RdtCacheMonEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec, sshExecSafe } from "../utils/ssh-exec.js";

function parsePqosMonitoring(output: string): RdtCacheMonEntry[] {
  const entries: RdtCacheMonEntry[] = [];
  const lines = output.split("\n");

  for (const line of lines) {
    // Data lines come in several formats depending on pqos version:
    //   Format 1 (with "k" suffix): "    0        0.28       7.5k       224.0         0.0         0.0"
    //   Format 2 (no suffix):       "    0        0.28       7500       224.0         0.0         0.0"
    //   Format 3 (with units):      "    0        0.28       7.5       224.0         0.0         0.0"
    // The key pattern is: core IPC LLC_misses LLC_occupancy MBL MBR
    const match = line.match(
      /^\s*(\d+)\s+([\d.]+)\s+([\d.]+)k?\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)/
    );
    if (match) {
      const rawMisses = match[3];
      // If original text had "k" suffix, multiply by 1000
      const missesK = line.match(new RegExp(`${rawMisses.replace(".", "\\.")}k`))
        ? parseFloat(rawMisses)
        : parseFloat(rawMisses) / 1000;

      entries.push({
        core: parseInt(match[1], 10),
        ipc: parseFloat(match[2]),
        llc_misses_k: Math.round(missesK * 1000) / 1000,
        llc_occupancy_kb: parseFloat(match[4]),
        mbl_mbps: parseFloat(match[5]),
        mbr_mbps: parseFloat(match[6]),
      });
    }
  }
  return entries;
}

export async function rdtCacheMonitoring(params: {
  host?: string;
  duration_sec?: number;
  cores?: string;
}): Promise<ToolResponse<RdtCacheMonData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 1;
  const coresParam = params.cores ?? "0-55"; // sensible default — first socket

  const meta = await buildMeta("fallback", duration * 1000);

  // Check pqos availability
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
      "Ensure root access. Check if RDT/CMT is supported: pqos -I -d",
    );
  }

  if (output.includes("not supported") ||
      output.includes("Monitoring capability not detected") ||
      output.match(/error[\s:].*(?:init|open|connect|permission|msr)/i)) {
    return errorResponse(
      meta,
      "RDT_UNSUPPORTED",
      `RDT monitoring not supported or permission denied: ${output.slice(0, 300)}`,
      "Check CPU supports CMT/MBM: pqos -I -d. Ensure root access.",
    );
  }

  // Parse the LAST snapshot (pqos outputs multiple samples within the duration)
  // Find the last TIME header and parse entries after it
  const blocks = output.split(/^TIME\s+/m);
  const lastBlock = blocks[blocks.length - 1] || "";
  const entries = parsePqosMonitoring(lastBlock);

  if (entries.length === 0) {
    // Try parsing the full output as fallback
    const allEntries = parsePqosMonitoring(output);
    if (allEntries.length === 0) {
      return errorResponse(
        meta,
        "PQOS_PARSE_ERROR",
        "Could not parse pqos monitoring output",
        "Raw output: " + output.slice(0, 500),
      );
    }
    // Deduplicate: take last entry per core
    const byCore = new Map<number, RdtCacheMonEntry>();
    for (const e of allEntries) byCore.set(e.core, e);
    const dedupedEntries = Array.from(byCore.values()).sort((a, b) => a.core - b.core);
    return buildResult(dedupedEntries, coresParam, duration, meta);
  }

  return buildResult(entries, coresParam, duration, meta);
}

function buildResult(
  entries: RdtCacheMonEntry[],
  coresParam: string,
  duration: number,
  meta: ReturnType<typeof buildMeta> extends Promise<infer R> ? R : never,
): ToolResponse<RdtCacheMonData> {
  const warnings: string[] = [];

  // Flag cores with very high LLC occupancy (possible cache hog)
  const maxLlc = Math.max(...entries.map((e) => e.llc_occupancy_kb));
  for (const e of entries) {
    if (e.llc_occupancy_kb > 10000) {
      warnings.push(
        `Core ${e.core}: ${e.llc_occupancy_kb} KB LLC occupancy — potential cache hog`,
      );
    }
  }

  // Flag cores with high remote memory bandwidth (NUMA-unfriendly)
  for (const e of entries) {
    if (e.mbr_mbps > 0 && e.mbl_mbps > 0) {
      const remotePct = (e.mbr_mbps / (e.mbl_mbps + e.mbr_mbps)) * 100;
      if (remotePct > 30) {
        warnings.push(
          `Core ${e.core}: ${remotePct.toFixed(0)}% remote memory BW — NUMA-unfriendly`,
        );
      }
    }
  }

  return okResponse<RdtCacheMonData>({
    duration_sec: duration,
    cores_monitored: coresParam,
    entries,
    warnings,
  }, meta);
}
