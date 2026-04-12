/**
 * emon_collect(preset, window_ms, cpu_filter) — Run a single EMON preset collection.
 *
 * Spawns EMON with the preset's curated event list, collects for the
 * specified window, parses the tab-separated output, and returns
 * structured data specific to the preset type.
 *
 * Presets:
 *   E0_iio_pcie_per_port  — Per-IIO-stack/port PCIe read/write bandwidth
 *   E1_cha_llc_snoop      — Per-CHA LLC hit/miss with TOR occupancy
 *   E2_mesh_stall_latency — CHA TOR + M2M tracker for memory latency
 *   E3_upi_detailed       — Per-link UPI data/snoop flits, power, CRC
 *   E4_core_stall_deep    — Per-CPU TMA L1 + execution port analysis
 *
 * When to use:
 *   - E0: Identify which NIC/PCIe device is generating traffic
 *   - E1: Investigate LLC contention between cores
 *   - E2: Diagnose memory latency spikes
 *   - E3: Detect cross-socket congestion or UPI link errors
 *   - E4: Profile core micro-architecture bottlenecks (frontend/backend/speculation)
 */
import type { ToolResponse } from "../types.js";
import type { EmonPresetId, EmonCollectResult } from "../types-emon.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getEmonBridge, EMON_PRESETS } from "../collectors/emon-bridge.js";

export async function emonCollect(params: {
  preset: string;
  window_ms?: number;
  cpu_filter?: number[];
}): Promise<ToolResponse<EmonCollectResult>> {
  const presetId = params.preset as EmonPresetId;
  const preset = EMON_PRESETS[presetId];
  const windowMs = params.window_ms ?? preset?.recommended_window_ms ?? 1000;

  const meta = await buildMeta("fallback", windowMs);

  // Validate preset
  if (!preset) {
    const validPresets = Object.keys(EMON_PRESETS).join(", ");
    return errorResponse(
      meta,
      "EMON_INVALID_PRESET",
      `Unknown preset: ${params.preset}. Valid presets: ${validPresets}`,
    );
  }

  // Check EMON availability — auto-load SEP drivers if needed
  const bridge = getEmonBridge();
  if (!bridge.isAvailable) {
    const { ready, autoLoadMessage } = await bridge.ensureReady();
    if (!ready) {
      return errorResponse(
        meta,
        "EMON_UNAVAILABLE",
        bridge.connectionError ?? "EMON is not available",
        autoLoadMessage ?? "Ensure EMON is installed at /opt/intel/sep/ and SEP drivers are loaded (insmod-sep)",
      );
    }
  }

  // Validate window
  if (windowMs < 100 || windowMs > 30000) {
    return errorResponse(
      meta,
      "EMON_INVALID_WINDOW",
      `Window must be 100-30000ms, got ${windowMs}`,
    );
  }

  try {
    const result = await bridge.collectPreset(presetId, windowMs, params.cpu_filter);
    return okResponse(result, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "EMON_COLLECTION_ERROR",
      `EMON collection failed for preset ${presetId}: ${err?.message ?? err}`,
      "Check that SEP drivers are loaded and you have CAP_SYS_ADMIN. PMU counters may be in use by another tool.",
    );
  }
}
