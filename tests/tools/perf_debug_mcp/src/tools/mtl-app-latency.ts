/**
 * mtl_app_latency — Parse application-level JSON latency stats from MTL log files.
 *
 * MTL POC applications write single-line JSON objects with latency breakdowns
 * to their log files (interleaved with MTL stat dumps).
 *
 * Each JSON line has:
 *   { "role":"sender"|"receiver", "time_s": ...,
 *     "lat_queue_us": {"min":N, "max":N, "avg":N, "n":N },
 *     "lat_bridge_us": {...}, "lat_rdma_us": {...}, "lat_total_us": {...},
 *     "lat_consume_us": {...}, "lat_e2e_us": {...}, ... }
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - log_path: path to the log file
 *   - tail_lines: how many lines from end to scan (default 100)
 *   - role_filter: optionally filter by "sender" or "receiver"
 */
import type { ToolResponse } from "../types.js";
import type { MtlAppLatencyData, MtlAppLatencySample, LatencyBucket } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

function parseLatencyBucket(obj: Record<string, unknown>): LatencyBucket {
  return {
    min: Number(obj.min ?? 0),
    max: Number(obj.max ?? 0),
    avg: Number(obj.avg ?? 0),
    n: Number(obj.n ?? 0),
  };
}

export async function mtlAppLatency(params: {
  host?: string;
  log_path: string;
  tail_lines?: number;
  role_filter?: string;
}): Promise<ToolResponse<MtlAppLatencyData>> {
  const host = params.host ?? "localhost";
  const logPath = params.log_path;
  const tailLines = params.tail_lines ?? 100;
  const roleFilter = params.role_filter ?? null;

  const meta = await buildMeta("fallback");

  try {
    // Read tail of log, extract only JSON lines
    const output = await sshExecSafe(
      host,
      `tail -${tailLines} ${logPath} 2>/dev/null | grep '^{' | tail -20`,
    );
    if (!output || !output.trim()) {
      return okResponse<MtlAppLatencyData>(
        { log_file: logPath, latest_sample: null, samples_found: 0 },
        meta,
      );
    }

    const jsonLines = output.trim().split("\n").filter((l) => l.startsWith("{"));
    const samples: MtlAppLatencySample[] = [];

    for (const line of jsonLines) {
      try {
        const obj = JSON.parse(line) as Record<string, unknown>;
        const role = String(obj.role ?? "unknown");

        if (roleFilter && role !== roleFilter) continue;

        // Extract latency buckets (keys matching lat_*_us)
        const latency: Record<string, LatencyBucket> = {};
        for (const [key, val] of Object.entries(obj)) {
          if (key.startsWith("lat_") && key.endsWith("_us") && typeof val === "object" && val !== null) {
            const stageName = key.replace(/^lat_/, "").replace(/_us$/, "");
            latency[stageName] = parseLatencyBucket(val as Record<string, unknown>);
          }
        }

        samples.push({
          role,
          time_s: Number(obj.time_s ?? 0),
          rx_frames: Number(obj.rx_frames ?? 0),
          rx_incomplete: Number(obj.rx_incomplete ?? 0),
          rx_drops: Number(obj.rx_drops ?? 0),
          bridge_frames: Number(obj.bridge_frames ?? 0),
          bridge_copies: Number(obj.bridge_copies ?? 0),
          fabrics_transfers: Number(obj.fabrics_transfers ?? 0),
          target_events: obj.target_events !== undefined ? Number(obj.target_events) : undefined,
          consumer_frames: obj.consumer_frames !== undefined ? Number(obj.consumer_frames) : undefined,
          latency,
        });
      } catch {
        // Skip unparseable lines
      }
    }

    return okResponse<MtlAppLatencyData>(
      {
        log_file: logPath,
        latest_sample: samples.length > 0 ? samples[samples.length - 1] : null,
        samples_found: samples.length,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_APP_LATENCY_ERROR",
      `Failed to parse app latency stats: ${err instanceof Error ? err.message : String(err)}`,
      `Ensure the log file exists at ${logPath}`,
    );
  }
}
