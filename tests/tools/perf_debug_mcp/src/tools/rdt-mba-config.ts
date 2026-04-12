/**
 * rdt_mba_config — Show Memory Bandwidth Allocation (MBA) configuration.
 *
 * Reports current MBA configuration from Intel RDT:
 *   - Per-socket COS (Class of Service) definitions with bandwidth percentages
 *   - Core-to-COS assignments showing which cores have which BW limits
 *
 * MBA allows throttling memory bandwidth per COS class.  Default is 100%
 * (no throttling).  Lower values restrict bandwidth, useful for preventing
 * memory-intensive background tasks from starving latency-sensitive workloads.
 *
 * MBA works together with CAT: CAT controls cache allocation, MBA controls
 * memory bandwidth.  Together they provide full resource isolation for
 * real-time/media workloads.
 *
 * Uses: pqos -I --show (read-only — does not modify configuration).
 * Requires: intel-cmt-cat package (pqos), root.
 */
import type {
  ToolResponse,
  RdtMbaConfigData,
  RdtMbaSocket,
  RdtMbaCosDefinition,
  RdtCoreAssignment,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

function parseMbaConfig(output: string): {
  sockets: RdtMbaSocket[];
  coreAssignments: RdtCoreAssignment[];
} {
  const sockets: RdtMbaSocket[] = [];
  const coreAssignments: RdtCoreAssignment[] = [];

  const lines = output.split("\n");
  let currentSocket = -1;
  let inCoreSection = false;

  for (const line of lines) {
    const trimmed = line.trim();

    // L3CA/MBA COS definitions for Socket N:
    const socketMatch = trimmed.match(/L3CA\/MBA COS definitions for Socket (\d+)/);
    if (socketMatch) {
      currentSocket = parseInt(socketMatch[1], 10);
      sockets.push({ socket: currentSocket, cos_definitions: [] });
      inCoreSection = false;
      continue;
    }

    // MBA COS0 => 100% available
    const mbaMatch = trimmed.match(/MBA COS(\d+)\s*=>\s*(\d+)%\s*available/);
    if (mbaMatch && currentSocket >= 0) {
      const cosId = parseInt(mbaMatch[1], 10);
      const bwPct = parseInt(mbaMatch[2], 10);

      const socketObj = sockets.find((s) => s.socket === currentSocket);
      if (socketObj) {
        socketObj.cos_definitions.push({
          cos_id: cosId,
          bandwidth_pct: bwPct,
        });
      }
      continue;
    }

    // Core information for socket N:
    const coreSecMatch = trimmed.match(/Core information for socket (\d+)/);
    if (coreSecMatch) {
      inCoreSection = true;
      currentSocket = -1;
      continue;
    }

    // Core 0, L2ID 0, L3ID 0 => COS0
    if (inCoreSection) {
      const coreMatch = trimmed.match(
        /Core (\d+),\s*L2ID (\d+),\s*L3ID (\d+)\s*=>\s*COS(\d+)/
      );
      if (coreMatch) {
        coreAssignments.push({
          core: parseInt(coreMatch[1], 10),
          l2_id: parseInt(coreMatch[2], 10),
          l3_id: parseInt(coreMatch[3], 10),
          cos_id: parseInt(coreMatch[4], 10),
        });
      }
    }
  }

  return { sockets, coreAssignments };
}

export async function rdtMbaConfig(params: {
  host?: string;
}): Promise<ToolResponse<RdtMbaConfigData>> {
  const host = params.host ?? "localhost";

  const meta = await buildMeta("fallback");

  const which = await sshExecSafe(host, "command -v pqos 2>/dev/null", 5_000);
  if (!which || !which.trim()) {
    return errorResponse(
      meta,
      "PQOS_MISSING",
      "pqos (intel-cmt-cat) not found on target host",
      "Install intel-cmt-cat: apt-get install intel-cmt-cat",
    );
  }

  const cmd = `pqos -I --show 2>&1`;
  const output = await sshExecSafe(host, cmd, 15_000);

  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "PQOS_NO_OUTPUT",
      "pqos --show produced no output",
      "Ensure root access. Check if MBA is supported: pqos -I -d",
    );
  }

  const { sockets, coreAssignments } = parseMbaConfig(output);

  if (sockets.length === 0) {
    return errorResponse(
      meta,
      "RDT_MBA_UNSUPPORTED",
      "No MBA configuration found in pqos output",
      "Check CPU supports MBA: pqos -I -d",
    );
  }

  const warnings: string[] = [];

  // Check if all COS have 100% (no throttling)
  for (const s of sockets) {
    const allFull = s.cos_definitions.every((d) => d.bandwidth_pct === 100);
    if (allFull) {
      warnings.push(
        `Socket ${s.socket}: all MBA COS at 100% — no memory bandwidth throttling configured`,
      );
    }

    // Warn about any throttled COS
    for (const d of s.cos_definitions) {
      if (d.bandwidth_pct < 100) {
        warnings.push(
          `Socket ${s.socket} COS${d.cos_id}: MBA throttled to ${d.bandwidth_pct}%`,
        );
      }
    }
  }

  // Check core→COS diversity
  const uniqueCos = new Set(coreAssignments.map((c) => c.cos_id));
  if (uniqueCos.size === 1 && uniqueCos.has(0)) {
    warnings.push(
      "All cores assigned to COS0 (default) — no MBA differentiation active",
    );
  }

  return okResponse<RdtMbaConfigData>({
    sockets,
    core_assignments: coreAssignments,
    warnings,
  }, meta);
}
