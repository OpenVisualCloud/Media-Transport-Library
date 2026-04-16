/**
 * core_load_snapshot(window_ms, breakdown, core_filter) — Per-core utilization.
 */
import type { ToolResponse, CoreLoadSnapshotData, ModeUsed } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readProcStat, computeCpuDeltas } from "../collectors/proc-stat.js";
import { getEbpfBridge } from "../collectors/ebpf-bridge.js";
import { sleep } from "../utils/proc-reader.js";
import { parseCoreFilter } from "../utils/helpers.js";
import { isLocalHost } from "../remote/host-utils.js";

export async function coreLoadSnapshot(params: {
  window_ms?: number;
  breakdown?: boolean;
  mode?: ModeUsed;
  core_filter?: string | null;
  top_n?: number | null;
  host?: string;
}): Promise<ToolResponse<CoreLoadSnapshotData>> {
  const windowMs = params.window_ms ?? 250;
  const breakdown = params.breakdown ?? true;
  const requestedMode = params.mode ?? "auto";
  const coreFilterSet = parseCoreFilter(params.core_filter);
  const host = params.host;

  let modeUsed: ModeUsed = "fallback";
  const meta = await buildMeta(modeUsed, windowMs);

  try {
    // Try eBPF mode first if requested (eBPF only works locally)
    const bridge = getEbpfBridge();
    if (isLocalHost(host) && (requestedMode === "ebpf" || requestedMode === "auto") && bridge.isEnabled) {
      const snapshot = await bridge.getSchedSnapshot(windowMs);
      if (snapshot) {
        modeUsed = "ebpf";
        meta.mode_used = modeUsed;
        // Convert eBPF sched data to core utilization
        // Group runtime by CPU
        const cpuRuntime = new Map<number, number>();
        for (const entry of snapshot.sched) {
          cpuRuntime.set(entry.cpu, (cpuRuntime.get(entry.cpu) ?? 0) + entry.runtime_ns);
        }
        const windowNs = windowMs * 1_000_000;
        const cores = [...cpuRuntime.entries()]
          .map(([cpu, runtime]) => ({
            cpu,
            util_pct: Math.round((runtime / windowNs) * 10000) / 100,
            user_pct: 0, // eBPF doesn't distinguish user/system easily
            system_pct: 0,
            irq_pct: 0,
            softirq_pct: 0,
            iowait_pct: 0,
            idle_pct: Math.round(((windowNs - runtime) / windowNs) * 10000) / 100,
          }))
          .filter((c) => coreFilterSet === null || coreFilterSet.has(c.cpu))
          .sort((a, b) => a.cpu - b.cpu);

        const topN = params.top_n;
        const finalCores = topN && topN > 0
          ? [...cores].sort((a, b) => b.util_pct - a.util_pct).slice(0, topN)
          : cores;

        return okResponse({ cores: finalCores }, meta);
      }
    }

    if (requestedMode === "ebpf") {
      return errorResponse(meta, "EBPF_UNAVAILABLE", "eBPF mode requested but not available", "Try mode=auto or mode=fallback");
    }

    // Fallback: /proc/stat delta
    const before = await readProcStat(host);
    await sleep(windowMs);
    const after = await readProcStat(host);

    const deltas = computeCpuDeltas(before, after);

    const cores = deltas.map((d) => {
      if (!breakdown) {
        return {
          cpu: d.cpu,
          util_pct: d.util_pct,
          user_pct: 0,
          system_pct: 0,
          irq_pct: 0,
          softirq_pct: 0,
          iowait_pct: 0,
          idle_pct: d.idle_pct,
        };
      }
      return d;
    }).filter((c) => coreFilterSet === null || coreFilterSet.has(c.cpu));

    const topN = params.top_n;
    const finalCores = topN && topN > 0
      ? [...cores].sort((a, b) => b.util_pct - a.util_pct).slice(0, topN)
      : cores;

    modeUsed = "fallback";
    meta.mode_used = modeUsed;
    return okResponse({ cores: finalCores }, meta);
  } catch (err) {
    return errorResponse(meta, "CORE_LOAD_ERROR", `Failed: ${err}`);
  }
}
