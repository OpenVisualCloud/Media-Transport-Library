/**
 * rdt_cache_allocation — Show L3 Cache Allocation Technology (CAT) config.
 *
 * Reports current L3 CAT configuration from Intel RDT:
 *   - Per-socket COS (Class of Service) definitions with cache way bitmasks
 *   - Core-to-COS assignments showing which cores share which cache partition
 *
 * CAT divides the L3 cache into "ways" and assigns subsets of ways to
 * different COS classes.  Cores are then assigned to a COS to control
 * which cache ways they can use.  This prevents noisy neighbors from
 * evicting cache lines of latency-sensitive workloads.
 *
 * Default state: all COS classes have all ways (no isolation).
 * After configuration: critical cores get dedicated ways.
 *
 * Uses: pqos -I --show (read-only — does not modify configuration).
 * Requires: intel-cmt-cat package (pqos), root.
 */
import type {
  ToolResponse,
  RdtCacheAllocationData,
  RdtL3CatSocket,
  RdtL3CosDefinition,
  RdtCoreAssignment,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

function popcount(hex: string): number {
  let n = 0;
  for (const ch of hex.replace(/^0x/i, "")) {
    const nibble = parseInt(ch, 16);
    // Count bits in nibble
    n += ((nibble >> 0) & 1) + ((nibble >> 1) & 1) + ((nibble >> 2) & 1) + ((nibble >> 3) & 1);
  }
  return n;
}

function parsePqosShow(output: string): {
  sockets: RdtL3CatSocket[];
  coreAssignments: RdtCoreAssignment[];
  totalL3Ways: number;
} {
  const sockets: RdtL3CatSocket[] = [];
  const coreAssignments: RdtCoreAssignment[] = [];
  let totalL3Ways = 0;

  const lines = output.split("\n");
  let currentSocket = -1;
  let inCoreSection = false;
  let coreSocket = -1;

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

    // L3CA COS0 => MASK 0x7fff
    const l3Match = trimmed.match(/L3CA COS(\d+)\s*=>\s*MASK\s+(0x[0-9a-fA-F]+)/);
    if (l3Match && currentSocket >= 0) {
      const cosId = parseInt(l3Match[1], 10);
      const mask = l3Match[2];
      const ways = popcount(mask);
      if (ways > totalL3Ways) totalL3Ways = ways;

      const socketObj = sockets.find((s) => s.socket === currentSocket);
      if (socketObj) {
        socketObj.cos_definitions.push({
          cos_id: cosId,
          mask_hex: mask,
          ways,
        });
      }
      continue;
    }

    // Core information for socket N:
    const coreSecMatch = trimmed.match(/Core information for socket (\d+)/);
    if (coreSecMatch) {
      coreSocket = parseInt(coreSecMatch[1], 10);
      inCoreSection = true;
      currentSocket = -1; // stop L3CA parsing
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

  return { sockets, coreAssignments, totalL3Ways };
}

export async function rdtCacheAllocation(params: {
  host?: string;
}): Promise<ToolResponse<RdtCacheAllocationData>> {
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
      "Ensure root access. Check if CAT is supported: pqos -I -d",
    );
  }

  const { sockets, coreAssignments, totalL3Ways } = parsePqosShow(output);

  if (sockets.length === 0) {
    return errorResponse(
      meta,
      "RDT_CAT_UNSUPPORTED",
      "No L3 CAT configuration found in pqos output",
      "Check CPU supports CAT: pqos -I -d",
    );
  }

  const warnings: string[] = [];

  // Check if all COS have the same mask (no isolation configured)
  for (const s of sockets) {
    const masks = new Set(s.cos_definitions.map((d) => d.mask_hex));
    if (masks.size === 1) {
      warnings.push(
        `Socket ${s.socket}: all COS classes have the same cache mask — no L3 cache isolation is configured`,
      );
    }
  }

  // Check if all cores are in COS0 (default, no partitioning)
  const uniqueCos = new Set(coreAssignments.map((c) => c.cos_id));
  if (uniqueCos.size === 1 && uniqueCos.has(0)) {
    warnings.push(
      "All cores assigned to COS0 (default) — no cache partitioning active",
    );
  }

  return okResponse<RdtCacheAllocationData>({
    sockets,
    core_assignments: coreAssignments,
    total_l3_ways: totalL3Ways,
    warnings,
  }, meta);
}
