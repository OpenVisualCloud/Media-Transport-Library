/**
 * devlink_health — NIC firmware health reporters via devlink.
 *
 * Reads NIC-internal health reporters exposed through the Linux devlink
 * subsystem.  mlx5 (ConnectX) and ice (Intel E810) drivers expose reporters
 * for firmware crashes, TX hangs, memory-domain drops (mdd), and internal
 * errors that are invisible to ethtool or RDMA counters.
 *
 * Key reporters:
 *   - fw: firmware errors (mlx5)
 *   - fw_fatal: unrecoverable FW crash (mlx5)
 *   - tx_hang: TX ring stuck (both mlx5 and ice)
 *   - mdd: malicious driver detection — VF sent bad traffic (mlx5)
 *
 * Source: `devlink health show` (from iproute2 package).
 * Works on any host with devlink-capable NICs.
 */
import type { ToolResponse, DevlinkHealthData, DevlinkReporter } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function devlinkHealth(params: {
  host?: string;
  pci_slot?: string;
}): Promise<ToolResponse<DevlinkHealthData>> {
  const host = params.host ?? "localhost";
  const pciSlot = params.pci_slot;

  const meta = await buildMeta("fallback");

  // Check devlink availability
  const check = await sshExecSafe(host, "command -v devlink 2>/dev/null");
  if (!check || !check.trim()) {
    return errorResponse(
      meta,
      "DEVLINK_MISSING",
      "devlink CLI not found on target host",
      "Install iproute2: apt-get install iproute2",
    );
  }

  try {
    // Try JSON output first (newer iproute2 versions)
    const jsonFilter = pciSlot ? ` dev pci/${pciSlot}` : "";
    const jsonCmd = `devlink health show${jsonFilter} -j 2>/dev/null`;
    const jsonOutput = await sshExecSafe(host, jsonCmd, 10_000);

    let reporters: DevlinkReporter[];

    if (jsonOutput && jsonOutput.trim().startsWith("{")) {
      reporters = parseDevlinkHealthJson(jsonOutput);
    } else {
      // Fallback to text output
      const textCmd = `devlink health show${jsonFilter} 2>/dev/null`;
      const textOutput = await sshExecSafe(host, textCmd, 10_000);
      if (!textOutput || !textOutput.trim()) {
        return errorResponse(
          meta,
          "DEVLINK_HEALTH_NO_OUTPUT",
          "devlink health show produced no output",
          "Ensure NICs support devlink (mlx5, ice). Check: devlink dev show",
        );
      }
      reporters = parseDevlinkHealthText(textOutput);
    }

    // Count unique PCI devices
    const devicesChecked = new Set(reporters.map((r) => r.pci_slot)).size;

    const warnings: string[] = [];
    for (const r of reporters) {
      if (r.error_count > 0) {
        warnings.push(
          `${r.pci_slot} reporter "${r.reporter}": ${r.error_count} error(s) — ` +
          `NIC firmware detected internal errors`,
        );
      }
      if (r.state === "error") {
        warnings.push(
          `${r.pci_slot} reporter "${r.reporter}" in ERROR state — ` +
          `NIC self-diagnostics report a problem`,
        );
      }
    }

    return okResponse<DevlinkHealthData>({
      reporters,
      devices_checked: devicesChecked,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "DEVLINK_HEALTH_ERROR",
      `Failed to read devlink health: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse JSON output from `devlink health show -j`.
 *
 * Format:
 * ```json
 * {
 *   "health": {
 *     "pci/0000:27:00.0": [
 *       {
 *         "reporter": "mdd",
 *         "state": "healthy",
 *         "error": 0,
 *         "recover": 0,
 *         ...
 *       }
 *     ]
 *   }
 * }
 * ```
 */
function parseDevlinkHealthJson(output: string): DevlinkReporter[] {
  const reporters: DevlinkReporter[] = [];

  try {
    const parsed = JSON.parse(output);
    const health = parsed.health || parsed;

    for (const [pciPath, reporterList] of Object.entries(health)) {
      const pciSlot = pciPath.replace(/^pci\//, "");
      if (!Array.isArray(reporterList)) continue;

      for (const r of reporterList as Array<Record<string, unknown>>) {
        reporters.push({
          pci_slot: pciSlot,
          reporter: String(r.reporter ?? "unknown"),
          state: String(r.state ?? "unknown"),
          error_count: Number(r.error ?? 0),
          recover_count: Number(r.recover ?? 0),
          grace_period_ms: r.grace_period !== undefined ? Number(r.grace_period) : undefined,
          auto_recover: r.auto_recover !== undefined ? Boolean(r.auto_recover) : undefined,
          auto_dump: r.auto_dump !== undefined ? Boolean(r.auto_dump) : undefined,
        });
      }
    }
  } catch {
    // JSON parse failed — return empty
  }

  return reporters;
}

/**
 * Parse text output from `devlink health show`.
 *
 * Format:
 * ```
 * pci/0000:27:00.0:
 *   reporter mdd
 *     state healthy error 0 recover 0 grace_period 0
 *     auto_recover true auto_dump true
 *   reporter tx_hang
 *     state healthy error 0 recover 0
 * ```
 */
function parseDevlinkHealthText(output: string): DevlinkReporter[] {
  const reporters: DevlinkReporter[] = [];
  let currentPci = "";
  let currentReporter: Partial<DevlinkReporter> | null = null;

  const flush = () => {
    if (currentReporter && currentReporter.reporter) {
      reporters.push({
        pci_slot: currentPci,
        reporter: currentReporter.reporter,
        state: currentReporter.state ?? "unknown",
        error_count: currentReporter.error_count ?? 0,
        recover_count: currentReporter.recover_count ?? 0,
        grace_period_ms: currentReporter.grace_period_ms,
        auto_recover: currentReporter.auto_recover,
        auto_dump: currentReporter.auto_dump,
      });
    }
    currentReporter = null;
  };

  for (const line of output.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed) continue;

    // PCI device header: "pci/0000:27:00.0:"
    const pciMatch = trimmed.match(/^pci\/(\S+?)(?::)?\s*$/i);
    if (pciMatch) {
      flush();
      currentPci = pciMatch[1].replace(/:$/, "");
      continue;
    }

    // Reporter line: "reporter mdd"
    const reporterMatch = trimmed.match(/^reporter\s+(\S+)/);
    if (reporterMatch) {
      flush();
      currentReporter = { reporter: reporterMatch[1] };
      continue;
    }

    if (!currentReporter) continue;

    // Parse key-value pairs on the current line
    const stateMatch = trimmed.match(/state\s+(\S+)/);
    if (stateMatch) currentReporter.state = stateMatch[1];

    const errorMatch = trimmed.match(/error\s+(\d+)/);
    if (errorMatch) currentReporter.error_count = parseInt(errorMatch[1], 10);

    const recoverMatch = trimmed.match(/recover\s+(\d+)/);
    if (recoverMatch) currentReporter.recover_count = parseInt(recoverMatch[1], 10);

    const graceMatch = trimmed.match(/grace_period\s+(\d+)/);
    if (graceMatch) currentReporter.grace_period_ms = parseInt(graceMatch[1], 10);

    const autoRecoverMatch = trimmed.match(/auto_recover\s+(\S+)/);
    if (autoRecoverMatch) currentReporter.auto_recover = autoRecoverMatch[1] === "true";

    const autoDumpMatch = trimmed.match(/auto_dump\s+(\S+)/);
    if (autoDumpMatch) currentReporter.auto_dump = autoDumpMatch[1] === "true";
  }

  flush();
  return reporters;
}
