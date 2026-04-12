/**
 * snapshot_diff — A/B comparison of two correlated snapshots.
 *
 * Captures two correlated snapshots separated by a configurable interval
 * and computes the delta for all numeric metrics.  Essential for measuring
 * the impact of configuration changes, load shifts, or tuning adjustments.
 *
 * Reuses the correlated_snapshot infrastructure and extracts comparable
 * numeric fields from core_load, irq_distribution, softirq_snapshot,
 * context_switch_rate, and cpu_frequency collectors.
 */
import type { ToolResponse, SnapshotDiffData, SnapshotDiffEntry, SnapshotDiffCategory } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { correlatedSnapshot } from "./correlated-snapshot.js";

const DIFFABLE_COLLECTORS = [
  "core_load",
  "irq_distribution",
  "softirq_snapshot",
  "context_switch_rate",
  "cpu_frequency",
] as const;

export async function snapshotDiff(params: {
  host?: string;
  interval_sec?: number;
  collectors?: string[];
  core_filter?: string;
  label_before?: string;
  label_after?: string;
}): Promise<ToolResponse<SnapshotDiffData>> {
  const host = params.host ?? "localhost";
  const intervalSec = params.interval_sec ?? 5;
  const collectors = (params.collectors && params.collectors.length > 0)
    ? params.collectors
    : [...DIFFABLE_COLLECTORS];
  const coreFilter = params.core_filter ?? null;
  const labelBefore = params.label_before ?? "before";
  const labelAfter = params.label_after ?? "after";

  const meta = await buildMeta("fallback", intervalSec * 1000);

  try {
    // Capture snapshot A
    const snapA = await correlatedSnapshot({
      collectors,
      window_s: 1,
      core_filter: coreFilter,
      focus_cpus: null,
      host,
    });
    if (!snapA.ok || !snapA.data) {
      return errorResponse(meta, "SNAP_A_FAILED", `First snapshot failed: ${snapA.error?.message ?? "unknown"}`);
    }

    // Wait for the interval
    await new Promise(resolve => setTimeout(resolve, intervalSec * 1000));

    // Capture snapshot B
    const snapB = await correlatedSnapshot({
      collectors,
      window_s: 1,
      core_filter: coreFilter,
      focus_cpus: null,
      host,
    });
    if (!snapB.ok || !snapB.data) {
      return errorResponse(meta, "SNAP_B_FAILED", `Second snapshot failed: ${snapB.error?.message ?? "unknown"}`);
    }

    const diffs: SnapshotDiffCategory[] = [];
    const summary: string[] = [];
    const warnings: string[] = [];
    const compared: string[] = [];

    // ── Core Load diff ──────────────────────────────────────────────
    if (snapA.data.core_load && snapB.data.core_load) {
      compared.push("core_load");
      const entries: SnapshotDiffEntry[] = [];
      const coresA = extractCoreLoadMetrics(snapA.data.core_load);
      const coresB = extractCoreLoadMetrics(snapB.data.core_load);
      for (const [key, valA] of Object.entries(coresA)) {
        const valB = coresB[key];
        if (valB !== undefined) {
          entries.push(makeDiffEntry(key, valA, valB));
        }
      }
      if (entries.length > 0) diffs.push({ category: "core_load", entries });
      const avgDelta = entries.filter(e => e.metric.includes("_pct")).reduce((s, e) => s + Math.abs(e.delta), 0) / Math.max(entries.filter(e => e.metric.includes("_pct")).length, 1);
      if (avgDelta > 10) summary.push(`Core load shifted by avg ${round(avgDelta)}% between snapshots`);
    }

    // ── Context Switch Rate diff ────────────────────────────────────
    if (snapA.data.context_switch_rate && snapB.data.context_switch_rate) {
      compared.push("context_switch_rate");
      const entries: SnapshotDiffEntry[] = [];
      const a = snapA.data.context_switch_rate as Record<string, unknown>;
      const b = snapB.data.context_switch_rate as Record<string, unknown>;
      for (const key of ["total_switches_per_sec", "voluntary_per_sec", "involuntary_per_sec"]) {
        const va = typeof a[key] === "number" ? a[key] as number : 0;
        const vb = typeof b[key] === "number" ? b[key] as number : 0;
        entries.push(makeDiffEntry(key, va, vb));
      }
      diffs.push({ category: "context_switch_rate", entries });
    }

    // ── CPU Frequency diff ──────────────────────────────────────────
    if (snapA.data.cpu_frequency && snapB.data.cpu_frequency) {
      compared.push("cpu_frequency");
      const entries: SnapshotDiffEntry[] = [];
      const a = snapA.data.cpu_frequency as Record<string, unknown>;
      const b = snapB.data.cpu_frequency as Record<string, unknown>;
      for (const key of ["avg_mhz", "min_mhz", "max_mhz"]) {
        const va = typeof a[key] === "number" ? a[key] as number : 0;
        const vb = typeof b[key] === "number" ? b[key] as number : 0;
        if (va > 0 || vb > 0) entries.push(makeDiffEntry(key, va, vb));
      }
      if (entries.length > 0) diffs.push({ category: "cpu_frequency", entries });
    }

    // ── IRQ Distribution diff ───────────────────────────────────────
    if (snapA.data.irq_distribution && snapB.data.irq_distribution) {
      compared.push("irq_distribution");
      const entries: SnapshotDiffEntry[] = [];
      const a = snapA.data.irq_distribution as Record<string, unknown>;
      const b = snapB.data.irq_distribution as Record<string, unknown>;
      for (const key of ["total_irqs_per_sec"]) {
        const va = typeof a[key] === "number" ? a[key] as number : 0;
        const vb = typeof b[key] === "number" ? b[key] as number : 0;
        if (va > 0 || vb > 0) entries.push(makeDiffEntry(key, va, vb));
      }
      if (entries.length > 0) diffs.push({ category: "irq_distribution", entries });
    }

    // ── Softirq diff ────────────────────────────────────────────────
    if (snapA.data.softirq_snapshot && snapB.data.softirq_snapshot) {
      compared.push("softirq_snapshot");
      const entries: SnapshotDiffEntry[] = [];
      const a = snapA.data.softirq_snapshot as Record<string, unknown>;
      const b = snapB.data.softirq_snapshot as Record<string, unknown>;
      for (const key of ["total_softirqs_per_sec"]) {
        const va = typeof a[key] === "number" ? a[key] as number : 0;
        const vb = typeof b[key] === "number" ? b[key] as number : 0;
        if (va > 0 || vb > 0) entries.push(makeDiffEntry(key, va, vb));
      }
      if (entries.length > 0) diffs.push({ category: "softirq_snapshot", entries });
    }

    // Generate warnings for large deltas
    for (const cat of diffs) {
      for (const e of cat.entries) {
        if (e.delta_pct !== null && Math.abs(e.delta_pct) > 50) {
          warnings.push(`${cat.category}.${e.metric}: changed ${round(e.delta_pct)}% (${round(e.before)} → ${round(e.after)})`);
        }
      }
    }

    if (compared.length === 0) {
      warnings.push("No collectors produced comparable numeric data");
    }

    return okResponse<SnapshotDiffData>({
      interval_sec: intervalSec,
      label_before: labelBefore,
      label_after: labelAfter,
      collectors_compared: compared,
      diffs,
      summary,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "SNAPSHOT_DIFF_ERROR", `Snapshot diff failed: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function makeDiffEntry(metric: string, before: number, after: number): SnapshotDiffEntry {
  const delta = after - before;
  const deltaPct = before !== 0 ? (delta / Math.abs(before)) * 100 : null;
  return { metric, before: round(before), after: round(after), delta: round(delta), delta_pct: deltaPct !== null ? round(deltaPct) : null };
}

function round(n: number): number { return Math.round(n * 100) / 100; }

function extractCoreLoadMetrics(coreLoad: Record<string, unknown>): Record<string, number> {
  const result: Record<string, number> = {};
  // Extract summary metrics
  for (const key of ["avg_user_pct", "avg_system_pct", "avg_idle_pct", "avg_iowait_pct", "avg_total_busy_pct"]) {
    if (typeof coreLoad[key] === "number") result[key] = coreLoad[key] as number;
  }
  // Extract per-core if available
  const cores = coreLoad["cores"] as Array<Record<string, unknown>> | undefined;
  if (Array.isArray(cores)) {
    for (const c of cores) {
      const cpu = c["cpu"] as number;
      if (typeof c["total_pct"] === "number") {
        result[`core_${cpu}_total_pct`] = c["total_pct"] as number;
      }
    }
  }
  return result;
}
