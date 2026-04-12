/**
 * pcm_cache_analysis(seconds, socket_filter, core_filter)
 *
 * Per-core L2/L3 cache hit and miss analysis from Intel PCM:
 *   - L3 and L2 hit/miss counts
 *   - Derived hit ratios and miss ratios
 *   - L3 cache occupancy (per-core)
 *   - System-wide aggregated hit/miss ratios
 *
 * Data source: pcm-sensor-server per-thread "Core Counters" and
 * system "Core Aggregate".
 *
 * When to use:
 *   - Diagnosing cache thrashing (low L3 hit ratio)
 *   - Identifying cores with pathological miss rates
 *   - Understanding working set size relative to cache capacity
 *   - Comparing cache efficiency across NUMA sockets
 */
import type {
  ToolResponse,
  PcmCacheAnalysisData,
  PcmCacheCoreEntry,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num, sub } from "../collectors/pcm-bridge.js";
import { parseCoreFilter, computeStats } from "../utils/helpers.js";

const sleep = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));

/** Compute ratio safely: hits / (hits + misses), returning 0 if denominator is 0. */
function hitRatio(hits: number, misses: number): number {
  const total = hits + misses;
  if (total === 0) return 0;
  return Math.round((hits / total) * 10000) / 10000;  // 4 decimal places (0.9834)
}

export async function pcmCacheAnalysis(params: {
  seconds?: number;
  socket_filter?: number | null;
  core_filter?: string | null;
  samples?: number;
  interval_s?: number;
  host?: string;
}): Promise<ToolResponse<PcmCacheAnalysisData>> {
  const seconds = params.seconds ?? 1;
  const socketFilter = params.socket_filter ?? null;
  const coreFilterSet = parseCoreFilter(params.core_filter);
  const samples = params.samples ?? 1;
  const intervalS = params.interval_s ?? 2;

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
    // ── Multi-sample accumulator (keyed by os_id) ───────────────────
    const TRACKED_METRICS = [
      "l3_hit_ratio", "l3_miss_ratio", "l2_hit_ratio", "l2_miss_ratio",
      "l3_cache_occupancy", "l3_misses", "l3_hits", "l2_misses", "l2_hits",
    ] as const;

    const accumulator = new Map<number, {
      core: number; socket: number;
      metrics: Record<string, number[]>;
    }>();

    let cores: PcmCacheCoreEntry[] = [];
    let intervalUs = 0;
    let lastJson: any = null;

    for (let s = 0; s < samples; s++) {
      const json = await bridge.getJson(seconds);
      lastJson = json;
      intervalUs = num(json, "Interval us");

      // Per-core cache data
      cores = bridge.walkThreads(
        json,
        (thread, core, socket) => {
          const socketId = num(socket, "Socket ID");
          const coreId = num(core, "Core ID");
          const osId = num(thread, "OS ID");

          if (socketFilter !== null && socketId !== socketFilter) return null;
          // Filter by OS ID (Linux CPU number), not PCM Core ID
          if (coreFilterSet !== null && !coreFilterSet.has(osId)) return null;

          const cc = sub(thread, "Core Counters") ?? {};

          const l3Hits = num(cc, "L3 Cache Hits");
          const l3Misses = num(cc, "L3 Cache Misses");
          const l2Hits = num(cc, "L2 Cache Hits");
          const l2Misses = num(cc, "L2 Cache Misses");

          const entry = {
            socket: socketId,
            core: coreId,
            thread: num(thread, "Thread ID"),
            os_id: num(thread, "OS ID"),
            l3_hit_ratio: hitRatio(l3Hits, l3Misses),
            l3_miss_ratio: hitRatio(l3Misses, l3Hits),
            l2_hit_ratio: hitRatio(l2Hits, l2Misses),
            l2_miss_ratio: hitRatio(l2Misses, l2Hits),
            l3_cache_occupancy: num(cc, "L3 Cache Occupancy"),
            l3_misses: l3Misses,
            l3_hits: l3Hits,
            l2_misses: l2Misses,
            l2_hits: l2Hits,
          };

          // Accumulate for multi-sample stats
          if (samples > 1) {
            let acc = accumulator.get(entry.os_id);
            if (!acc) {
              acc = { core: coreId, socket: socketId, metrics: {} };
              for (const m of TRACKED_METRICS) acc.metrics[m] = [];
              accumulator.set(entry.os_id, acc);
            }
            for (const m of TRACKED_METRICS) {
              acc.metrics[m].push(entry[m]);
            }
          }

          return entry;
        }
      );

      // Sleep between samples (not after last)
      if (samples > 1 && s < samples - 1) {
        await sleep(intervalS * 1000);
      }
    }

    // System-wide aggregates from "Core Aggregate" (last sample)
    const sysAgg = sub(lastJson as any, "Core Aggregate");
    const sysCC = sub(sysAgg, "Core Counters") ?? {};

    const sysL3Hits = num(sysCC, "L3 Cache Hits");
    const sysL3Misses = num(sysCC, "L3 Cache Misses");
    const sysL2Hits = num(sysCC, "L2 Cache Hits");
    const sysL2Misses = num(sysCC, "L2 Cache Misses");

    // ── Build result ────────────────────────────────────────────────
    const result: PcmCacheAnalysisData = {
      interval_us: intervalUs,
      cores,
      system_l3_hit_ratio: hitRatio(sysL3Hits, sysL3Misses),
      system_l3_miss_ratio: hitRatio(sysL3Misses, sysL3Hits),
      system_l2_hit_ratio: hitRatio(sysL2Hits, sysL2Misses),
      system_l2_miss_ratio: hitRatio(sysL2Misses, sysL2Hits),
    };

    if (samples > 1 && accumulator.size > 0) {
      const perCore: Array<{
        os_id: number; core: number; socket: number;
        metrics: Record<string, { mean: number; stddev: number; min: number; max: number }>;
      }> = [];

      for (const [osId, acc] of accumulator) {
        const statsMap: Record<string, { mean: number; stddev: number; min: number; max: number }> = {};
        for (const m of TRACKED_METRICS) {
          const st = computeStats(acc.metrics[m]);
          if (st) statsMap[m] = st;
        }
        perCore.push({ os_id: osId, core: acc.core, socket: acc.socket, metrics: statsMap });
      }
      perCore.sort((a, b) => a.os_id - b.os_id);

      result.multi_sample = { samples, interval_s: intervalS, per_core: perCore };
    }

    return okResponse(result, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM cache analysis: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
