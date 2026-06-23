/**
 * pcm_numa_traffic(seconds, socket_filter, top_n)
 *
 * NUMA traffic analysis from Intel PCM:
 *   - Per-core local vs remote memory bandwidth
 *   - Per-core local ratio (percentage)
 *   - Per-socket aggregated local/remote bandwidth and ratio
 *
 * Data source: pcm-sensor-server per-thread "Core Memory Bandwidth Counters"
 * and per-socket "Uncore Counters" (Local/Remote Memory Request Ratio).
 *
 * When to use:
 *   - Diagnosing NUMA-unfriendly memory access patterns
 *   - Identifying cores generating excessive remote memory traffic
 *   - Validating NUMA-aware application placement
 *   - Comparing local vs remote traffic ratio across sockets
 *
 * Interpretation:
 *   - local_ratio_pct > 90%: good NUMA locality
 *   - local_ratio_pct < 70%: likely NUMA-unfriendly, consider improving
 *     memory/thread affinity
 *   - local_ratio_pct < 50%: severe NUMA penalty, investigate immediately
 */
import type {
  ToolResponse,
  PcmNumaTrafficData,
  PcmNumaCoreMemBw,
  PcmNumaSocketSummary,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num, sub } from "../collectors/pcm-bridge.js";
import { parseCoreFilter } from "../utils/helpers.js";

export async function pcmNumaTraffic(params: {
  seconds?: number;
  socket_filter?: number | null;
  core_filter?: string | null;
  top_n?: number;
  host?: string;
}): Promise<ToolResponse<PcmNumaTrafficData>> {
  const seconds = params.seconds ?? 1;
  const socketFilter = params.socket_filter ?? null;
  const coreFilterSet = parseCoreFilter(params.core_filter);
  const topN = params.top_n ?? 0;  // 0 = return all

  const meta = await buildMeta("fallback", seconds * 1000);

  const bridge = getPcmBridgeForHost(params.host);
  await bridge.ensureRunning();
  if (!bridge.isAvailable) {
    return errorResponse(
      meta,
      "PCM_UNAVAILABLE",
      bridge.connectionError ?? "pcm-sensor-server is not available",
      "Start pcm-sensor-server with: sudo pcm-sensor-server -p 9738"
    );
  }

  try {
    const json = await bridge.getJson(seconds);
    const intervalUs = num(json, "Interval us");

    // Per-core local/remote memory bandwidth
    let perCore: PcmNumaCoreMemBw[] = bridge.walkThreads(
      json,
      (thread, core, socket) => {
        const socketId = num(socket, "Socket ID");
        const coreId = num(core, "Core ID");
        const osId = num(thread, "OS ID");
        if (socketFilter !== null && socketId !== socketFilter) return null;
        // Filter by OS ID (Linux CPU number), not PCM Core ID
        if (coreFilterSet !== null && !coreFilterSet.has(osId)) return null;

        const membw = sub(thread, "Core Memory Bandwidth Counters") ?? {};
        const localBw = num(membw, "Local Memory Bandwidth");
        const remoteBw = num(membw, "Remote Memory Bandwidth");
        const total = localBw + remoteBw;

        return {
          socket: socketId,
          core: num(core, "Core ID"),
          thread: num(thread, "Thread ID"),
          os_id: num(thread, "OS ID"),
          local_memory_bw_bytes: localBw,
          remote_memory_bw_bytes: remoteBw,
          local_ratio_pct: total > 0 ? Math.round((localBw / total) * 10000) / 100 : 100,
        };
      }
    );

    // If top_n is specified, sort by remote BW descending and take top N
    // This surfaces the worst NUMA offenders first
    if (topN > 0) {
      perCore = perCore
        .sort((a, b) => b.remote_memory_bw_bytes - a.remote_memory_bw_bytes)
        .slice(0, topN);
    }

    // Per-socket summary: aggregate local/remote BW across all cores in each socket
    const socketMap = new Map<number, { local: number; remote: number }>();

    // Walk all threads (not filtered by topN) for accurate socket summaries
    bridge.walkThreads(json, (thread, _core, socket) => {
      const socketId = num(socket, "Socket ID");
      if (socketFilter !== null && socketId !== socketFilter) return null;

      const membw = sub(thread, "Core Memory Bandwidth Counters") ?? {};
      const localBw = num(membw, "Local Memory Bandwidth");
      const remoteBw = num(membw, "Remote Memory Bandwidth");

      const existing = socketMap.get(socketId) ?? { local: 0, remote: 0 };
      existing.local += localBw;
      existing.remote += remoteBw;
      socketMap.set(socketId, existing);
      return null;
    });

    const perSocket: PcmNumaSocketSummary[] = [...socketMap.entries()]
      .map(([socketId, { local, remote }]) => {
        const total = local + remote;
        return {
          socket: socketId,
          total_local_bw_bytes: local,
          total_remote_bw_bytes: remote,
          local_ratio_pct: total > 0 ? Math.round((local / total) * 10000) / 100 : 100,
        };
      })
      .sort((a, b) => a.socket - b.socket);

    return okResponse(
      {
        interval_us: intervalUs,
        per_core: perCore,
        per_socket: perSocket,
      },
      meta
    );
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM NUMA traffic data: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
