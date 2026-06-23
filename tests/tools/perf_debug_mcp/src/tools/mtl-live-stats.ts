/**
 * mtl_live_stats — Read live JSON stats files that MTL applications
 * continuously overwrite (e.g., /dev/shm/poc_8k_sender_stats.json).
 *
 * These files contain a single JSON object with application-specific
 * metrics like fps, drops, and pipeline stage timings.
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - stats_path: path to the JSON stats file
 */
import type { ToolResponse } from "../types.js";
import type { MtlLiveStatsData } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe, sshExec } from "../utils/ssh-exec.js";

export async function mtlLiveStats(params: {
  host?: string;
  stats_path: string;
}): Promise<ToolResponse<MtlLiveStatsData>> {
  const host = params.host ?? "localhost";
  const statsPath = params.stats_path;

  const meta = await buildMeta("fallback");

  try {
    // Check file existence and read it
    const checkResult = await sshExec(host, `test -f ${statsPath} && cat ${statsPath} 2>/dev/null`);
    if (checkResult.exitCode !== 0 || !checkResult.stdout.trim()) {
      return okResponse<MtlLiveStatsData>(
        { file_path: statsPath, stats: {}, file_exists: false },
        meta,
      );
    }

    let stats: Record<string, number | string> = {};
    try {
      const raw = JSON.parse(checkResult.stdout.trim());
      // Flatten nested objects into dotted keys
      stats = flattenObject(raw);
    } catch {
      // If not valid JSON, return raw content in a single key
      stats = { raw: checkResult.stdout.trim() };
    }

    return okResponse<MtlLiveStatsData>(
      { file_path: statsPath, stats, file_exists: true },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_LIVE_STATS_ERROR",
      `Failed to read live stats: ${err instanceof Error ? err.message : String(err)}`,
      `Check that ${statsPath} exists on the target host`,
    );
  }
}

/**
 * Flatten a nested JSON object into dotted key paths.
 * { "a": { "b": 1 }, "c": 2 } → { "a.b": 1, "c": 2 }
 */
export function flattenObject(
  obj: Record<string, unknown>,
  prefix = "",
): Record<string, number | string> {
  const result: Record<string, number | string> = {};
  for (const [key, val] of Object.entries(obj)) {
    const fullKey = prefix ? `${prefix}.${key}` : key;
    if (typeof val === "object" && val !== null && !Array.isArray(val)) {
      Object.assign(result, flattenObject(val as Record<string, unknown>, fullKey));
    } else if (typeof val === "number" || typeof val === "string") {
      result[fullKey] = val;
    } else if (typeof val === "boolean") {
      result[fullKey] = val ? "true" : "false";
    }
  }
  return result;
}
