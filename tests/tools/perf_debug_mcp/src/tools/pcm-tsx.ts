/**
 * pcm_tsx — Intel Transactional Synchronization Extensions (TSX) monitoring.
 *
 * Reports per-core TSX usage metrics: cycles spent in transactions,
 * abort counts, and abort breakdown by type (capacity, conflict).
 *
 * Many CPUs have TSX disabled or removed (especially post-TAA vulnerability).
 * The tool gracefully returns { supported: false } when TSX is unavailable.
 *
 * Requires: pcm-tsx binary in PATH, root access.
 */
import type { ToolResponse, PcmTsxData, PcmTsxCoreEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

export async function pcmTsx(params: {
  host?: string;
}): Promise<ToolResponse<PcmTsxData>> {
  const host = params.host ?? "localhost";
  const meta = await buildMeta("fallback");

  const cmd = `pcm-tsx -i=1 2>&1`;
  const result = await sshExec(host, cmd, 20_000);
  const raw = result.stdout;

  if (raw.includes("not found") || raw.includes("No such file")) {
    return errorResponse(
      meta,
      "PCM_NOT_FOUND",
      "pcm-tsx binary not found",
      "Install Intel PCM: https://github.com/intel/pcm"
    );
  }

  // TSX support not available on this CPU
  if (raw.includes("No RTM support detected")) {
    return okResponse(
      { supported: false, raw_output: raw.trim().slice(0, 500) },
      meta
    );
  }

  // Parse CSV-like output if TSX is supported
  // Expected columns vary by PCM version; attempt best-effort parsing
  const cores: PcmTsxCoreEntry[] = [];
  const lines = raw.split("\n");

  for (const line of lines) {
    // Look for per-core data rows (format varies by PCM version)
    const coreMatch = line.match(
      /^\s*(\d+)\s+[\d.]+\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)/
    );
    if (coreMatch) {
      cores.push({
        core: parseInt(coreMatch[1], 10),
        tsx_cycles_pct: parseFloat(coreMatch[2]) || 0,
        tsx_aborts: parseInt(coreMatch[3], 10) || 0,
        tsx_capacity_aborts: parseInt(coreMatch[4], 10) || 0,
        tsx_conflict_aborts: parseInt(coreMatch[5], 10) || 0,
      });
    }
  }

  return okResponse(
    {
      supported: true,
      cores: cores.length > 0 ? cores : undefined,
      raw_output: raw.trim().slice(0, 2000),
    },
    meta
  );
}
