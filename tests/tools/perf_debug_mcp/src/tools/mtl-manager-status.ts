/**
 * mtl_manager_status — Check MtlManager status, version, and connected clients.
 *
 * Data sources:
 *   1. Process check (pgrep MtlManager)
 *   2. Socket existence check (configurable path, default /var/run/imtl/mtl_manager.sock)
 *   3. MtlManager log file parse (configurable path, tries /dev/shm/mtl_manager.log)
 *
 * All parameters are configurable — host, socket path, log path.
 */
import type { ToolResponse } from "../types.js";
import type { MtlManagerStatusData, MtlManagerClient } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec, sshExecSafe } from "../utils/ssh-exec.js";

export async function mtlManagerStatus(params: {
  host?: string;
  socket_path?: string;
  log_path?: string;
}): Promise<ToolResponse<MtlManagerStatusData>> {
  const host = params.host ?? "localhost";
  const socketPath = params.socket_path ?? "/var/run/imtl/mtl_manager.sock";
  const logPath = params.log_path ?? "";  // auto-detect

  const meta = await buildMeta("fallback");

  try {
    // 1. Check if MtlManager process is running
    const pidResult = await sshExecSafe(host, "pgrep -x MtlManager | head -1");
    const pid = pidResult ? parseInt(pidResult.trim(), 10) : null;
    const running = pid !== null && !isNaN(pid);

    // 2. Check socket existence
    const socketCheck = await sshExec(host, `test -S ${socketPath} && echo yes || echo no`);
    const socketExists = socketCheck.stdout.trim() === "yes";

    // 3. Find and parse log file
    let resolvedLogPath: string | null = null;
    let version: string | null = null;
    const clients: MtlManagerClient[] = [];

    // Try provided path first, then common locations
    const logCandidates = logPath
      ? [logPath]
      : [
          "/dev/shm/mtl_manager.log",
          "/tmp/mtl_manager.log",
          "/var/log/mtl_manager.log",
        ];

    for (const candidate of logCandidates) {
      const exists = await sshExec(host, `test -f ${candidate} && echo yes || echo no`);
      if (exists.stdout.trim() === "yes") {
        resolvedLogPath = candidate;
        break;
      }
    }

    if (resolvedLogPath) {
      // Parse version from log
      const verLine = await sshExecSafe(
        host,
        `grep -m1 'MTL lib version' ${resolvedLogPath} 2>/dev/null || grep -m1 'version' ${resolvedLogPath} 2>/dev/null`,
      );
      if (verLine) {
        const match = verLine.match(/version[:\s]+(\S+)/i);
        if (match) version = match[1];
      }

      // Parse client lcore allocations from log
      // Pattern: "lcore <N> assigned to pid <P>" or similar
      const clientLines = await sshExecSafe(
        host,
        `grep -i 'lcore.*pid\\|client.*pid\\|assigned.*pid\\|pid.*lcore' ${resolvedLogPath} 2>/dev/null | tail -100`,
      );
      if (clientLines) {
        const pidLcoreMap = new Map<number, number[]>();
        for (const line of clientLines.split("\n")) {
          if (!line.trim()) continue;
          // Try multiple patterns
          let pidMatch: RegExpMatchArray | null = null;
          let lcoreMatch: RegExpMatchArray | null = null;

          // Pattern: "lcore <N> ... pid <P>"
          lcoreMatch = line.match(/lcore\s+(\d+)/i);
          pidMatch = line.match(/pid\s+(\d+)/i);

          if (pidMatch && lcoreMatch) {
            const clientPid = parseInt(pidMatch[1], 10);
            const lcore = parseInt(lcoreMatch[1], 10);
            if (!isNaN(clientPid) && !isNaN(lcore)) {
              const existing = pidLcoreMap.get(clientPid) ?? [];
              if (!existing.includes(lcore)) {
                existing.push(lcore);
              }
              pidLcoreMap.set(clientPid, existing);
            }
          }
        }
        for (const [clientPid, lcores] of pidLcoreMap) {
          clients.push({ pid: clientPid, lcores: lcores.sort((a, b) => a - b) });
        }
      }
    }

    const data: MtlManagerStatusData = {
      running,
      pid: running ? pid : null,
      version,
      socket_path: socketPath,
      log_path: resolvedLogPath,
      clients: clients.sort((a, b) => a.pid - b.pid),
      client_count: clients.length,
    };

    if (!running && !socketExists) {
      data.running = false;
      // Still return data — useful to know it's not running
    }

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_MANAGER_ERROR",
      `Failed to check MtlManager status: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure SSH access to the target host is configured (ssh-copy-id)",
    );
  }
}
