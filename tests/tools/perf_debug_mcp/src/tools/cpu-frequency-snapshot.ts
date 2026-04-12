/**
 * cpu_frequency_snapshot(core_filter) — Per-CPU frequency, governor, min/max.
 */
import type { ToolResponse, CpuFrequencySnapshotData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readAllCpuFreq, hasCpuFreq } from "../collectors/sysfs.js";
import { parseCoreFilter } from "../utils/helpers.js";

export async function cpuFrequencySnapshot(params?: {
  core_filter?: string | null;
  host?: string;
}): Promise<ToolResponse<CpuFrequencySnapshotData>> {
  const coreFilterSet = parseCoreFilter(params?.core_filter);
  const host = params?.host;
  const meta = await buildMeta("fallback");

  try {
    if (!(await hasCpuFreq(host))) {
      return errorResponse(
        meta,
        "CPUFREQ_UNAVAILABLE",
        "cpufreq interface not available",
        "The kernel may not support frequency scaling, or cpufreq driver is not loaded"
      );
    }

    let cpus = await readAllCpuFreq(host);
    if (coreFilterSet !== null) {
      cpus = cpus.filter((c) => coreFilterSet.has(c.cpu));
    }
    return okResponse({ cpus }, meta);
  } catch (err) {
    return errorResponse(meta, "CPU_FREQUENCY_ERROR", `Failed: ${err}`);
  }
}
