/**
 * softirq_snapshot(window_ms, core_filter)
 * Delta parse /proc/softirqs per CPU.
 */
import type { ToolResponse, SoftirqSnapshotData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readProcSoftirqs, computeSoftirqDeltas } from "../collectors/softirqs.js";
import { sleep } from "../utils/proc-reader.js";
import { parseCoreFilter } from "../utils/helpers.js";

export async function softirqSnapshot(params: {
  window_ms?: number;
  core_filter?: string | null;
  top_n?: number | null;
  host?: string;
}): Promise<ToolResponse<SoftirqSnapshotData>> {
  const windowMs = params.window_ms ?? 1000;
  const coreFilterSet = parseCoreFilter(params.core_filter);
  const host = params.host;
  const meta = await buildMeta("fallback", windowMs);

  try {
    const before = await readProcSoftirqs(host);
    await sleep(windowMs);
    const after = await readProcSoftirqs(host);

    const deltas = computeSoftirqDeltas(before, after);

    // Apply core filter
    if (coreFilterSet !== null) {
      deltas.per_cpu = deltas.per_cpu.filter((c) => coreFilterSet.has(c.cpu));
      deltas.hot_cpus = deltas.hot_cpus.filter((cpu) => coreFilterSet.has(cpu));
    }

    // Apply top_n — sort by total_delta descending and limit
    const topN = params.top_n;
    if (topN && topN > 0) {
      deltas.per_cpu = [...deltas.per_cpu]
        .sort((a, b) => b.total_delta - a.total_delta)
        .slice(0, topN);
    }

    return okResponse(deltas, meta);
  } catch (err) {
    return errorResponse(meta, "SOFTIRQ_SNAPSHOT_ERROR", `Failed: ${err}`);
  }
}
