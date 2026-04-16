/**
 * mtl_nic_pf_stats — Collect NIC (PF) statistics via ethtool on a target host.
 *
 * Runs `ethtool -S <iface>` and `ethtool <iface>` to collect:
 *   - Per-queue TX/RX packet and byte counters
 *   - NIC-level drops, errors, priority flow control
 *   - Link speed and status
 *   - Driver info
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - interface: NIC interface name (e.g., ens1np0)
 *   - filter: optional regex to filter stat names (e.g., "drop|error|vf")
 */
import type { ToolResponse } from "../types.js";
import type { MtlNicPfStatsData } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function mtlNicPfStats(params: {
  host?: string;
  interface: string;
  filter?: string;
}): Promise<ToolResponse<MtlNicPfStatsData>> {
  const host = params.host ?? "localhost";
  const iface = params.interface;
  const filter = params.filter ?? null;

  const meta = await buildMeta("fallback");

  try {
    // Get driver info
    const driverOutput = await sshExecSafe(host, `ethtool -i ${iface} 2>/dev/null | head -3`);
    let driver = "unknown";
    if (driverOutput) {
      const driverMatch = driverOutput.match(/driver:\s*(\S+)/);
      if (driverMatch) driver = driverMatch[1];
    }

    // Get link speed and status
    const linkOutput = await sshExecSafe(
      host,
      `ethtool ${iface} 2>/dev/null | grep -iE 'Speed:|Link detected:'`,
    );
    let linkSpeedMbps = 0;
    let linkDetected = false;
    if (linkOutput) {
      const speedMatch = linkOutput.match(/Speed:\s*(\d+)/);
      if (speedMatch) linkSpeedMbps = parseInt(speedMatch[1], 10);
      linkDetected = /Link detected:\s*yes/i.test(linkOutput);
    }

    // Get ethtool -S stats
    let statsCmd = `ethtool -S ${iface} 2>/dev/null`;
    if (filter) {
      statsCmd += ` | grep -iE '${filter}'`;
    }
    const statsOutput = await sshExecSafe(host, statsCmd);

    const stats: Record<string, number> = {};
    if (statsOutput) {
      for (const line of statsOutput.split("\n")) {
        const match = line.match(/^\s+(\S+):\s*(-?\d+)/);
        if (match) {
          stats[match[1]] = parseInt(match[2], 10);
        }
      }
    }

    return okResponse<MtlNicPfStatsData>(
      {
        interface: iface,
        driver,
        link_speed_mbps: linkSpeedMbps,
        link_detected: linkDetected,
        stats,
        stat_count: Object.keys(stats).length,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_NIC_STATS_ERROR",
      `Failed to collect NIC stats: ${err instanceof Error ? err.message : String(err)}`,
      `Ensure ethtool is available and interface ${iface} exists on the target host`,
    );
  }
}
