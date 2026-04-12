/**
 * perf_debug_snapshot(window_ms, focus_cpu, focus_target)
 * Convenience aggregator that calls multiple tools and returns one JSON bundle.
 */
import type { ToolResponse, PerfDebugSnapshotData, ModeUsed } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { coreLoadSnapshot } from "./core-load-snapshot.js";
import { runningOnCore } from "./running-on-core.js";
import { irqDistribution } from "./irq-distribution.js";
import { softirqSnapshot } from "./softirq-snapshot.js";
import { cpuFrequencySnapshot } from "./cpu-frequency-snapshot.js";
import { isolationSummary } from "./isolation-summary.js";
import { cgroupCpuLimits } from "./cgroup-cpu-limits.js";
import { getOnlineCpus } from "../collectors/sysfs.js";
import { getPcmBridge } from "../collectors/pcm-bridge.js";
import { pcmCoreCounters } from "./pcm-core-counters.js";
import { pcmPowerThermal } from "./pcm-power-thermal.js";

export async function perfDebugSnapshot(params: {
  window_ms?: number;
  focus_cpu?: number | null;
  focus_target?: number | null;
  collectors?: string[];
  host?: string;
}): Promise<ToolResponse<PerfDebugSnapshotData>> {
  const windowMs = params.window_ms ?? 2000;
  const focusCpu = params.focus_cpu ?? null;
  const focusTarget = params.focus_target ?? null;
  const host = params.host;
  const ALL_COLLECTORS = ["core_load", "running_on_cores", "irq_distribution", "softirq_snapshot", "cpu_frequency", "isolation", "pcm"];
  const requested = params.collectors && params.collectors.length > 0
    ? new Set(params.collectors)
    : new Set(ALL_COLLECTORS);

  const meta = await buildMeta("fallback", windowMs);

  try {
    // Phase 1: Non-windowed tools (instant)
    const [freqResult, isolResult] = await Promise.all([
      requested.has("cpu_frequency") ? cpuFrequencySnapshot({ host }) : Promise.resolve(null),
      requested.has("isolation") ? isolationSummary({ host }) : Promise.resolve(null),
    ]);

    // Phase 2: Windowed tools
    const subWindowMs = Math.min(windowMs, 1000);

    const [loadResult, irqResult, softirqResult] = await Promise.all([
      requested.has("core_load") ? coreLoadSnapshot({ window_ms: subWindowMs, host }) : Promise.resolve(null),
      requested.has("irq_distribution") ? irqDistribution({ window_ms: subWindowMs, host }) : Promise.resolve(null),
      requested.has("softirq_snapshot") ? softirqSnapshot({ window_ms: subWindowMs, host }) : Promise.resolve(null),
    ]);

    // Phase 3: running_on_core for focus CPU or top loaded CPUs
    const runningOnCores: Record<number, any[]> = {};

    if (requested.has("running_on_cores")) {
      if (focusCpu !== null) {
        const result = await runningOnCore({ cpu: focusCpu, window_ms: subWindowMs, top_n: 20, host });
        if (result.ok && result.data) {
          runningOnCores[focusCpu] = result.data.tasks;
        }
      } else {
        // Get top 3 most loaded CPUs
        const cpus = loadResult && loadResult.ok && loadResult.data
          ? loadResult.data.cores
              .sort((a, b) => b.util_pct - a.util_pct)
              .slice(0, 3)
              .map((c) => c.cpu)
          : [];

        for (const cpu of cpus) {
          const result = await runningOnCore({ cpu, window_ms: Math.min(subWindowMs, 250), top_n: 10, host });
          if (result.ok && result.data) {
            runningOnCores[cpu] = result.data.tasks;
          }
        }
      }
    }

    // Phase 4: Optional cgroup limits for focus target
    let cgroupData = undefined;
    if (focusTarget !== null) {
      const result = await cgroupCpuLimits({ target_pid: focusTarget, host });
      if (result.ok && result.data) {
        cgroupData = result.data;
      }
    }

    const data: PerfDebugSnapshotData = {
      core_load: loadResult && loadResult.ok && loadResult.data
        ? loadResult.data
        : { cores: [] },
      running_on_cores: runningOnCores,
      irq_distribution: irqResult && irqResult.ok && irqResult.data
        ? irqResult.data
        : { per_cpu: [], top_irqs: [] },
      softirq_snapshot: softirqResult && softirqResult.ok && softirqResult.data
        ? softirqResult.data
        : { per_cpu: [], hot_cpus: [] },
      cpu_frequency: freqResult && freqResult.ok && freqResult.data
        ? freqResult.data
        : { cpus: [] },
      isolation: isolResult && isolResult.ok && isolResult.data
        ? isolResult.data
        : { cmdline_flags: {}, isolated_cpus: [], nohz_full_cpus: [], rcu_nocbs_cpus: [], warnings: [] },
    };

    if (cgroupData) data.cgroup_limits = cgroupData;

    // Phase 5: Optional Intel PCM data (if pcm-sensor-server is running)
    if (requested.has("pcm")) {
      const pcmBridge = getPcmBridge();
      if (pcmBridge.isAvailable) {
        try {
          const [pcmCores, pcmPower] = await Promise.all([
            pcmCoreCounters({ seconds: 1, socket_filter: null, core_filter: focusCpu !== null ? String(focusCpu) : null }),
            pcmPowerThermal({ seconds: 1, socket_filter: null, include_tma: true }),
          ]);
          if (pcmCores.ok && pcmCores.data) data.pcm_core_counters = pcmCores.data;
          if (pcmPower.ok && pcmPower.data) data.pcm_power_thermal = pcmPower.data;
        } catch { /* PCM data is optional — do not fail the snapshot */ }
      }
    }

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "PERF_DEBUG_SNAPSHOT_ERROR", `Failed: ${err}`);
  }
}
