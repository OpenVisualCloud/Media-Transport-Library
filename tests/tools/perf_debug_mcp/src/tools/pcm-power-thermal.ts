/**
 * pcm_power_thermal(seconds, socket_filter, include_tma)
 *
 * Intel PCM power, thermal, and Top-Down Microarchitecture Analysis:
 *
 * Per-core:
 *   - Thermal headroom (°C below TjMax)
 *   - C-state residency (C0 through C10+)
 *
 * Per-socket:
 *   - Package / PP0 / PP1 / DRAM energy consumption (Joules)
 *   - Package-level C-state residency
 *   - Uncore frequencies per die
 *
 * Top-Down Microarchitecture Analysis (TMA):
 *   - Level 1: Frontend Bound, Bad Speculation, Backend Bound, Retiring
 *   - Level 2: Fetch Latency/Bandwidth, Branch Misprediction, Machine Clears,
 *              Memory Bound, Core Bound, Heavy/Light Operations
 *
 * When to use:
 *   - Understanding thermal throttling (low thermal headroom)
 *   - Checking power consumption per socket
 *   - Diagnosing deep C-state exit latency issues
 *   - TMA: identifying bottleneck category (frontend? backend? memory?)
 */
import type {
  ToolResponse,
  PcmPowerThermalData,
  PcmCorePowerEntry,
  PcmSocketPowerEntry,
  PcmTmaBreakdown,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num, sub } from "../collectors/pcm-bridge.js";

/** Extract all CStateResidency[N] entries from an Energy Counters block */
function extractCStates(ec: Record<string, any> | undefined): Record<string, number> {
  const result: Record<string, number> = {};
  if (!ec) return result;
  for (const [key, value] of Object.entries(ec)) {
    if (key.startsWith("CStateResidency[")) {
      result[key] = typeof value === "number" ? value : 0;
    }
  }
  return result;
}

/** Extract Uncore Frequency Die N entries */
function extractUncoreFreqs(uc: Record<string, any> | undefined): number[] {
  const freqs: number[] = [];
  if (!uc) return freqs;
  for (const [key, value] of Object.entries(uc)) {
    if (key.startsWith("Uncore Frequency Die")) {
      freqs.push(typeof value === "number" ? value : 0);
    }
  }
  return freqs;
}

/** Extract TMA breakdown from system Core Aggregate's Core Counters */
function extractTma(cc: Record<string, any> | undefined): PcmTmaBreakdown | undefined {
  if (!cc) return undefined;

  const fb = num(cc, "Frontend Bound");
  const bs = num(cc, "Bad Speculation");
  const bb = num(cc, "Backend Bound");
  const rt = num(cc, "Retiring");

  // If all zeros, TMA data is probably not available
  if (fb === 0 && bs === 0 && bb === 0 && rt === 0) return undefined;

  const tma: PcmTmaBreakdown = {
    frontend_bound_pct: fb,
    bad_speculation_pct: bs,
    backend_bound_pct: bb,
    retiring_pct: rt,
  };

  // Level 2 fields (may be 0 on CPUs that don't support them)
  const flb = num(cc, "Fetch Latency Bound");
  const fbb = num(cc, "Fetch Bandwidth Bound");
  const bmp = num(cc, "Branch Misprediction Bound");
  const mcb = num(cc, "Machine Clears Bound");
  const mb = num(cc, "Memory Bound");
  const cb = num(cc, "Core Bound");
  const hob = num(cc, "Heavy Operations Bound");
  const lob = num(cc, "Light Operations Bound");

  if (flb > 0) tma.fetch_latency_bound_pct = flb;
  if (fbb > 0) tma.fetch_bandwidth_bound_pct = fbb;
  if (bmp > 0) tma.branch_misprediction_bound_pct = bmp;
  if (mcb > 0) tma.machine_clears_bound_pct = mcb;
  if (mb > 0) tma.memory_bound_pct = mb;
  if (cb > 0) tma.core_bound_pct = cb;
  if (hob > 0) tma.heavy_operations_bound_pct = hob;
  if (lob > 0) tma.light_operations_bound_pct = lob;

  return tma;
}

export async function pcmPowerThermal(params: {
  seconds?: number;
  socket_filter?: number | null;
  include_tma?: boolean;
  host?: string;
}): Promise<ToolResponse<PcmPowerThermalData>> {
  const seconds = params.seconds ?? 1;
  const socketFilter = params.socket_filter ?? null;
  const includeTma = params.include_tma ?? true;

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

    // Per-core thermal/C-state data
    const cores: PcmCorePowerEntry[] = bridge.walkThreads(
      json,
      (thread, core, socket) => {
        const socketId = num(socket, "Socket ID");
        if (socketFilter !== null && socketId !== socketFilter) return null;

        const ec = sub(thread, "Energy Counters") ?? {};

        return {
          socket: socketId,
          core: num(core, "Core ID"),
          thread: num(thread, "Thread ID"),
          os_id: num(thread, "OS ID"),
          thermal_headroom_c: num(ec, "Thermal Headroom"),
          c_state_residency: extractCStates(ec),
        };
      }
    );

    // Per-socket power and package C-states
    const sockets: PcmSocketPowerEntry[] = bridge.walkSockets(json, (socket) => {
      const socketId = num(socket, "Socket ID");
      if (socketFilter !== null && socketId !== socketFilter) return null;

      const uncoreBlock = sub(socket, "Uncore");
      const uc = sub(uncoreBlock, "Uncore Counters") ?? {};

      return {
        socket: socketId,
        package_joules: num(uc, "Package Joules Consumed"),
        pp0_joules: num(uc, "PP0 Joules Consumed"),
        pp1_joules: num(uc, "PP1 Joules Consumed"),
        dram_joules: num(uc, "DRAM Joules Consumed"),
        package_c_state_residency: extractCStates(uc),
        uncore_frequencies_mhz: extractUncoreFreqs(uc),
      };
    });

    // TMA from system Core Aggregate
    let tma: PcmTmaBreakdown | undefined;
    if (includeTma) {
      const sysAgg = sub(json as any, "Core Aggregate");
      const sysCC = sub(sysAgg, "Core Counters");
      tma = extractTma(sysCC);
    }

    const data: PcmPowerThermalData = {
      interval_us: intervalUs,
      cores,
      sockets,
    };
    if (tma) data.tma = tma;

    return okResponse(data, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM power/thermal data: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
