/**
 * correlated_snapshot — Synchronized multi-tool capture for A/B testing.
 *
 * Captures metrics from multiple sources within the same time window,
 * ensuring data correlation. Essential for reliable A/B comparison where
 * independent tool calls would capture data at different moments.
 *
 * Runs all requested collectors in parallel and returns results in a
 * single response with a common timestamp and capture window.
 */
import type { ToolResponse, PcmCoreCountersData, PcmCacheAnalysisData } from "../types.js";
import type { MtlSessionStatsData } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { coreLoadSnapshot } from "./core-load-snapshot.js";
import { runningOnCore } from "./running-on-core.js";
import { irqDistribution } from "./irq-distribution.js";
import { softirqSnapshot } from "./softirq-snapshot.js";
import { cpuFrequencySnapshot } from "./cpu-frequency-snapshot.js";
import { contextSwitchRate } from "./context-switch-rate.js";
import { getPcmBridgeForHost } from "../collectors/pcm-bridge.js";
import { pcmCoreCounters } from "./pcm-core-counters.js";
import { pcmCacheAnalysis } from "./pcm-cache-analysis.js";
import { mtlSessionStats } from "./mtl-session-stats.js";

export interface CorrelatedSnapshotData {
  /** Capture window in seconds */
  capture_window_s: number;
  /** Which collectors were requested */
  collectors_requested: string[];
  /** Which collectors succeeded */
  collectors_succeeded: string[];
  /** Which collectors failed (with error messages) */
  collectors_failed: { name: string; error: string }[];
  /** Core filter applied (if any) */
  core_filter_applied?: string;

  // ── Collector results (each key present only if that collector was requested) ──
  core_load?: any;
  running_on_cores?: Record<number, any[]>;
  irq_distribution?: any;
  softirq_snapshot?: any;
  cpu_frequency?: any;
  context_switch_rate?: any;
  pcm_core_counters?: PcmCoreCountersData;
  pcm_cache_analysis?: PcmCacheAnalysisData;
  mtl_session_stats?: MtlSessionStatsData;
}

/** All available collector names */
const ALL_COLLECTORS = [
  "core_load",
  "running_on_cores",
  "irq_distribution",
  "softirq_snapshot",
  "cpu_frequency",
  "context_switch_rate",
  "pcm_core_counters",
  "pcm_cache_analysis",
  "mtl_session_stats",
] as const;

type CollectorName = typeof ALL_COLLECTORS[number];

export async function correlatedSnapshot(params: {
  collectors: string[];
  window_s?: number;
  core_filter?: string | null;
  focus_cpus?: string | null;
  host?: string;
  mtl_host?: string;
  mtl_log_path?: string;
  mtl_last_dumps?: number;
  pcm_seconds?: number;
}): Promise<ToolResponse<CorrelatedSnapshotData>> {
  const collectors = params.collectors.length > 0
    ? params.collectors as CollectorName[]
    : [...ALL_COLLECTORS];
  const windowS = params.window_s ?? 2;
  const windowMs = windowS * 1000;
  const coreFilter = params.core_filter ?? null;
  const focusCpuStr = params.focus_cpus ?? null;
  const host = params.host;
  const mtlHost = params.mtl_host ?? "localhost";
  const mtlLogPath = params.mtl_log_path ?? undefined;
  const mtlLastDumps = params.mtl_last_dumps ?? 5;
  const pcmSeconds = params.pcm_seconds ?? Math.min(windowS, 5);

  const meta = await buildMeta("fallback", windowMs);

  // Parse focus CPUs for running_on_core
  const focusCpus: number[] = [];
  if (focusCpuStr) {
    for (const part of focusCpuStr.split(",")) {
      const trimmed = part.trim();
      const rangeParts = trimmed.split("-");
      if (rangeParts.length === 2) {
        const start = parseInt(rangeParts[0], 10);
        const end = parseInt(rangeParts[1], 10);
        if (!isNaN(start) && !isNaN(end)) {
          for (let i = start; i <= end; i++) focusCpus.push(i);
        }
      } else {
        const num = parseInt(trimmed, 10);
        if (!isNaN(num)) focusCpus.push(num);
      }
    }
  }

  const requested = new Set(collectors);
  const succeeded: string[] = [];
  const failed: { name: string; error: string }[] = [];

  const data: CorrelatedSnapshotData = {
    capture_window_s: windowS,
    collectors_requested: [...requested],
    collectors_succeeded: [],
    collectors_failed: [],
  };

  if (coreFilter) data.core_filter_applied = coreFilter;

  try {
    // ── Run all collectors in parallel ──────────────────────────────
    const promises: Promise<void>[] = [];

    const collect = <T>(name: string, fn: () => Promise<ToolResponse<T>>, assign: (d: T) => void): void => {
      promises.push(
        fn()
          .then(r => {
            if (r.ok && r.data) { assign(r.data); succeeded.push(name); }
            else { failed.push({ name, error: r.error?.message ?? "unknown" }); }
          })
          .catch(e => { failed.push({ name, error: String(e) }); })
      );
    };

    if (requested.has("core_load")) {
      collect("core_load", () => coreLoadSnapshot({ window_ms: Math.min(windowMs, 1000), host }), d => { data.core_load = d; });
    }

    if (requested.has("irq_distribution")) {
      collect("irq_distribution", () => irqDistribution({ window_ms: Math.min(windowMs, 1000), host }), d => { data.irq_distribution = d; });
    }

    if (requested.has("softirq_snapshot")) {
      collect("softirq_snapshot", () => softirqSnapshot({ window_ms: Math.min(windowMs, 1000), host }), d => { data.softirq_snapshot = d; });
    }

    if (requested.has("cpu_frequency")) {
      collect("cpu_frequency", () => cpuFrequencySnapshot({ host }), d => { data.cpu_frequency = d; });
    }

    if (requested.has("context_switch_rate")) {
      collect("context_switch_rate", () => contextSwitchRate({ window_ms: Math.min(windowMs, 1000), host }), d => { data.context_switch_rate = d; });
    }

    // PCM tools
    const pcmBridge = getPcmBridgeForHost(host);
    if (requested.has("pcm_core_counters")) {
      if (pcmBridge.isAvailable) {
        collect("pcm_core_counters", () => pcmCoreCounters({ seconds: pcmSeconds, socket_filter: null, core_filter: coreFilter, host }), d => { data.pcm_core_counters = d; });
      } else {
        failed.push({ name: "pcm_core_counters", error: "pcm-sensor-server not available" });
      }
    }

    if (requested.has("pcm_cache_analysis")) {
      if (pcmBridge.isAvailable) {
        collect("pcm_cache_analysis", () => pcmCacheAnalysis({ seconds: pcmSeconds, socket_filter: null, core_filter: coreFilter, host }), d => { data.pcm_cache_analysis = d; });
      } else {
        failed.push({ name: "pcm_cache_analysis", error: "pcm-sensor-server not available" });
      }
    }

    // MTL session stats
    if (requested.has("mtl_session_stats")) {
      if (mtlLogPath) {
        collect("mtl_session_stats", () => mtlSessionStats({ host: mtlHost, log_path: mtlLogPath, last_dumps: mtlLastDumps }), d => { data.mtl_session_stats = d; });
      } else {
        failed.push({ name: "mtl_session_stats", error: "mtl_log_path parameter is required for mtl_session_stats" });
      }
    }

    // Wait for all parallel collectors
    await Promise.all(promises);

    // Phase 2: Sequential — running_on_core needs to run per-CPU
    if (requested.has("running_on_cores") && focusCpus.length > 0) {
      const runningResults: Record<number, any[]> = {};
      // Run up to 8 in parallel to save time
      const batches: number[][] = [];
      for (let i = 0; i < focusCpus.length; i += 8) {
        batches.push(focusCpus.slice(i, i + 8));
      }
      try {
        for (const batch of batches) {
          const results = await Promise.all(
            batch.map(cpu => runningOnCore({ cpu, window_ms: Math.min(windowMs, 500), top_n: 10, host }))
          );
          for (let j = 0; j < batch.length; j++) {
            const r = results[j];
            if (r.ok && r.data) runningResults[batch[j]] = r.data.tasks;
          }
        }
        data.running_on_cores = runningResults;
        succeeded.push("running_on_cores");
      } catch (e) {
        failed.push({ name: "running_on_cores", error: String(e) });
      }
    } else if (requested.has("running_on_cores") && focusCpus.length === 0) {
      failed.push({ name: "running_on_cores", error: "focus_cpus parameter required for running_on_cores collector" });
    }

    data.collectors_succeeded = succeeded;
    data.collectors_failed = failed;

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "CORRELATED_SNAPSHOT_ERROR",
      `Failed to capture correlated snapshot: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
