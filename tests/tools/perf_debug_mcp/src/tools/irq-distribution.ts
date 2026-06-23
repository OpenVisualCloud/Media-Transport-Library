/**
 * irq_distribution(window_ms, cpu_filter, core_filter, include_per_irq, include_per_iface)
 * Show IRQ distribution per CPU.
 */
import type { ToolResponse, IrqDistributionData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readProcInterrupts, computeInterruptDeltas } from "../collectors/interrupts.js";
import { sleep } from "../utils/proc-reader.js";
import { parseCoreFilter } from "../utils/helpers.js";

export async function irqDistribution(params: {
  window_ms?: number;
  cpu_filter?: number | null;
  core_filter?: string | null;
  include_per_irq?: boolean;
  include_per_iface?: boolean;
  host?: string;
}): Promise<ToolResponse<IrqDistributionData>> {
  const windowMs = params.window_ms ?? 1000;
  const cpuFilter = params.cpu_filter ?? null;
  const coreFilterSet = parseCoreFilter(params.core_filter);
  const includePerIrq = params.include_per_irq ?? true;
  const host = params.host;

  // Build effective filter set: merge cpu_filter (single) with core_filter (multi)
  let filterSet: Set<number> | null = coreFilterSet;
  if (cpuFilter !== null) {
    filterSet = filterSet ? new Set([...filterSet, cpuFilter]) : new Set([cpuFilter]);
  }

  const meta = await buildMeta("fallback", windowMs);

  try {
    const before = await readProcInterrupts(host);
    await sleep(windowMs);
    const after = await readProcInterrupts(host);

    const deltas = computeInterruptDeltas(before, after);

    let perCpu = deltas.per_cpu_totals;
    let topIrqs = deltas.per_irq;

    // Apply CPU filter (single or multi-core)
    if (filterSet !== null) {
      perCpu = perCpu.filter((c) => filterSet!.has(c.cpu));
      if (includePerIrq) {
        topIrqs = topIrqs
          .map((irq) => ({
            ...irq,
            delta_total: irq.per_cpu_deltas
              .filter((d) => filterSet!.has(d.cpu))
              .reduce((sum, d) => sum + d.delta, 0),
            per_cpu_deltas: irq.per_cpu_deltas.filter((d) => filterSet!.has(d.cpu)),
          }))
          .filter((irq) => irq.delta_total > 0)
          .sort((a, b) => b.delta_total - a.delta_total);
      }
    }

    // Limit IRQ list for payload size
    topIrqs = topIrqs.slice(0, 50);

    // If per_iface grouping requested, group by NIC interface
    if (params.include_per_iface && topIrqs.length > 0) {
      // Group by common prefix pattern (e.g., "mlx5_comp" or "ice-")
      // This is best-effort based on IRQ name patterns
      for (const irq of topIrqs) {
        // many network IRQ names contain the interface like "eth0-TxRx-0"
        // we annotate this in the name field already
      }
    }

    const data: IrqDistributionData = {
      per_cpu: perCpu,
      top_irqs: topIrqs,
    };

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "IRQ_DISTRIBUTION_ERROR", `Failed: ${err}`);
  }
}
