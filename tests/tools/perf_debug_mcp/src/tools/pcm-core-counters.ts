/**
 * pcm_core_counters(seconds, socket_filter, core_filter)
 *
 * Per-core hardware performance counters from Intel PCM:
 *   - Instructions Retired, Clock Unhalted (Thread/Ref)
 *   - IPC (derived: instructions / cycles)
 *   - Active core frequency
 *   - L3/L2 cache hits and misses
 *   - L3 cache occupancy
 *   - SMI count
 *   - Local / remote memory bandwidth per core
 *   - Socket and system aggregates
 *
 * Data source: pcm-sensor-server /persecond/{seconds} JSON endpoint.
 * The server internally samples every 1 second and we request deltas
 * over the specified window.
 *
 * When to use:
 *   - Investigating IPC regression (should be >1.0 for most workloads)
 *   - Identifying cores with high SMI counts (firmware interference)
 *   - Comparing per-core instruction throughput
 *   - Correlating cache misses with specific cores
 */
import type {
  ToolResponse,
  PcmCoreCountersData,
  PcmCoreCounterEntry,
  PcmCoreAggregateEntry,
  PcmImbalanceWarning,
  PcmImbalanceWarningCore,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import {
  getPcmBridgeForHost,
  num,
  sub,
  type PcmRawThread,
  type PcmRawCore,
  type PcmRawSocket,
  type PcmRawJson,
} from "../collectors/pcm-bridge.js";
import { execFile } from "child_process";
import { promisify } from "util";
import { parseCoreFilter, computeStats } from "../utils/helpers.js";

const execFileAsync = promisify(execFile);
const sleep = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));

/**
 * Scan /proc for mtl_sch_* threads and map CPU → { pid, comm, threadName }.
 * This tells us which MTL scheduler thread is pinned to each core so we can
 * detect load imbalance across schedulers belonging to the same process.
 */
async function findSchedulerThreads(): Promise<Map<number, { pid: number; comm: string; threadName: string }>> {
  const result = new Map<number, { pid: number; comm: string; threadName: string }>();

  try {
    const script = `
for task in /proc/[0-9]*/task/[0-9]*/; do
  comm=$(cat "$task/comm" 2>/dev/null)
  if [ "\${comm#mtl_sch_}" != "$comm" ]; then
    tid_path=\${task%/}
    pid_path=\${tid_path%/task/*}
    pid=\${pid_path##*/}
    cpu=$(awk '{print $39}' "$task/stat" 2>/dev/null)
    pcomm=$(cat "/proc/$pid/comm" 2>/dev/null)
    echo "$cpu|$pid|$pcomm|$comm"
  fi
done 2>/dev/null`;

    const { stdout } = await execFileAsync("bash", ["-c", script], { timeout: 3000 });

    for (const line of stdout.split("\n")) {
      const trimmed = line.trim();
      if (!trimmed) continue;
      const parts = trimmed.split("|");
      if (parts.length >= 4) {
        const cpu = parseInt(parts[0], 10);
        const pid = parseInt(parts[1], 10);
        const comm = parts[2];
        const threadName = parts[3];
        if (!isNaN(cpu) && !isNaN(pid)) {
          result.set(cpu, { pid, comm, threadName });
        }
      }
    }
  } catch {
    // Non-fatal: return empty map if /proc scan fails
  }

  return result;
}

/**
 * Detect imbalance across cores belonging to the same MTL process.
 * Groups cores by parent PID, always reports per-process core data,
 * and flags when max/min ratio exceeds 2× for any key metric.
 */
function detectCoreImbalance(
  cores: PcmCoreCounterEntry[],
  schMap: Map<number, { pid: number; comm: string; threadName: string }>,
): PcmImbalanceWarning[] {
  const warnings: PcmImbalanceWarning[] = [];

  // Group cores by parent PID
  const byProcess = new Map<number, { comm: string; entries: { core: PcmCoreCounterEntry; thread: string }[] }>();

  for (const core of cores) {
    const sch = schMap.get(core.os_id);
    if (!sch) continue;

    let group = byProcess.get(sch.pid);
    if (!group) {
      group = { comm: sch.comm, entries: [] };
      byProcess.set(sch.pid, group);
    }
    group.entries.push({ core, thread: sch.threadName });
  }

  const IMBALANCE_THRESHOLD = 2.0;

  // For each process with ≥2 cores, always report data + flag if imbalanced
  for (const [pid, group] of byProcess) {
    if (group.entries.length < 2) continue;

    const coreInfos: PcmImbalanceWarningCore[] = group.entries.map((e) => ({
      os_id: e.core.os_id,
      sch_thread: e.thread,
      local_memory_bw: e.core.local_memory_bw,
      instructions_retired: e.core.instructions_retired,
      ipc: e.core.ipc,
    }));

    const coreIds = group.entries.map((e) => e.core.os_id).join(",");

    // Check local_memory_bw
    const memBws = group.entries.map((e) => e.core.local_memory_bw).filter((v) => v > 0);
    let memRatio = 1;
    let memMax = 0;
    let memMin = 0;
    if (memBws.length >= 2) {
      memMax = Math.max(...memBws);
      memMin = Math.min(...memBws);
      memRatio = memMin > 0 ? Math.round((memMax / memMin) * 10) / 10 : 1;
    }

    // Check instructions_retired
    const instrs = group.entries.map((e) => e.core.instructions_retired).filter((v) => v > 0);
    let instrRatio = 1;
    let instrMax = 0;
    let instrMin = 0;
    if (instrs.length >= 2) {
      instrMax = Math.max(...instrs);
      instrMin = Math.min(...instrs);
      instrRatio = instrMin > 0 ? Math.round((instrMax / instrMin) * 10) / 10 : 1;
    }

    // Pick the worst metric for the primary report
    const useMemBw = memRatio >= instrRatio;
    const ratio = useMemBw ? memRatio : instrRatio;
    const metric = useMemBw ? "local_memory_bw" : "instructions_retired";
    const maxVal = useMemBw ? memMax : instrMax;
    const minVal = useMemBw ? memMin : instrMin;
    const flagged = ratio > IMBALANCE_THRESHOLD;

    let message: string;
    if (flagged) {
      if (useMemBw) {
        message = `${group.comm} cores ${coreIds}: ${ratio}\u00d7 memory bandwidth asymmetry ` +
          `(${memMax} vs ${memMin} MB/s) \u2014 suggests uneven MTL session distribution across schedulers`;
      } else {
        message = `${group.comm} cores ${coreIds}: ${ratio}\u00d7 instruction throughput asymmetry ` +
          `(${instrMax} vs ${instrMin}) \u2014 suggests uneven MTL session distribution across schedulers`;
      }
    } else {
      message = `${group.comm} cores ${coreIds}: balanced (${ratio}\u00d7 ratio)`;
    }

    warnings.push({
      process_pid: pid,
      process_comm: group.comm,
      cores: coreInfos,
      flagged,
      metric,
      max_value: maxVal,
      min_value: minVal,
      ratio,
      message,
    });
  }

  return warnings;
}

export async function pcmCoreCounters(params: {
  seconds?: number;
  socket_filter?: number | null;
  core_filter?: string | null;
  samples?: number;
  interval_s?: number;
  host?: string;
}): Promise<ToolResponse<PcmCoreCountersData>> {
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
      "ipc", "active_frequency_mhz", "l3_cache_misses", "l3_cache_hits",
      "l2_cache_misses", "l2_cache_hits", "l3_cache_occupancy",
      "instructions_retired", "local_memory_bw", "remote_memory_bw", "smi_count",
    ] as const;

    const accumulator = new Map<number, {
      core: number; socket: number;
      metrics: Record<string, number[]>;
    }>();

    let cores: PcmCoreCounterEntry[] = [];
    let intervalUs = 0;
    let lastJson: any = null;

    for (let s = 0; s < samples; s++) {
      const json = await bridge.getJson(seconds);
      lastJson = json;
      intervalUs = num(json, "Interval us");

      // Walk the thread tree and build core counter entries
      cores = bridge.walkThreads(
        json,
        (thread, core, socket) => {
          const socketId = num(socket, "Socket ID");
          const coreId = num(core, "Core ID");
          const osId = num(thread, "OS ID");

          // Apply filters — core_filter uses OS ID (Linux CPU number), not PCM Core ID
          if (socketFilter !== null && socketId !== socketFilter) return null;
          if (coreFilterSet !== null && !coreFilterSet.has(osId)) return null;

          const cc = sub(thread, "Core Counters") ?? {};
          const membw = sub(thread, "Core Memory Bandwidth Counters") ?? {};

          const instrRetired = num(cc, "Instructions Retired Any");
          const clockThread = num(cc, "Clock Unhalted Thread");

          const entry = {
            socket: socketId,
            core: coreId,
            thread: num(thread, "Thread ID"),
            os_id: num(thread, "OS ID"),
            instructions_retired: instrRetired,
            clock_unhalted_thread: clockThread,
            clock_unhalted_ref: num(cc, "Clock Unhalted Ref"),
            ipc: clockThread > 0 ? Math.round((instrRetired / clockThread) * 1000) / 1000 : 0,
            active_frequency_mhz: Math.round(num(cc, "Core Frequency") / 1_000_000 * 100) / 100,
            l3_cache_misses: num(cc, "L3 Cache Misses"),
            l3_cache_hits: num(cc, "L3 Cache Hits"),
            l2_cache_misses: num(cc, "L2 Cache Misses"),
            l2_cache_hits: num(cc, "L2 Cache Hits"),
            l3_cache_occupancy: num(cc, "L3 Cache Occupancy"),
            smi_count: num(cc, "SMI Count"),
            local_memory_bw: num(membw, "Local Memory Bandwidth"),
            remote_memory_bw: num(membw, "Remote Memory Bandwidth"),
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

    // Build aggregates from socket-level and system-level "Core Aggregate"
    // (uses the last sample's json)
    const aggregates: PcmCoreAggregateEntry[] = [];

    // Per-socket aggregates
    bridge.walkSockets(lastJson, (socket) => {
      const socketId = num(socket, "Socket ID");
      if (socketFilter !== null && socketId !== socketFilter) return null;

      const agg = sub(socket, "Core Aggregate");
      const cc = sub(agg, "Core Counters") ?? {};

      const instrRetired = num(cc, "Instructions Retired Any");
      const clockThread = num(cc, "Clock Unhalted Thread");

      aggregates.push({
        scope: "socket",
        socket_id: socketId,
        instructions_retired: instrRetired,
        clock_unhalted_thread: clockThread,
        clock_unhalted_ref: num(cc, "Clock Unhalted Ref"),
        ipc: clockThread > 0 ? Math.round((instrRetired / clockThread) * 1000) / 1000 : 0,
        l3_cache_misses: num(cc, "L3 Cache Misses"),
        l3_cache_hits: num(cc, "L3 Cache Hits"),
        l2_cache_misses: num(cc, "L2 Cache Misses"),
        l2_cache_hits: num(cc, "L2 Cache Hits"),
      });
      return null;
    });

    // System aggregate (top-level "Core Aggregate")
    if (socketFilter === null) {
      const sysAgg = sub(lastJson as any, "Core Aggregate");
      const cc = sub(sysAgg, "Core Counters") ?? {};
      const instrRetired = num(cc, "Instructions Retired Any");
      const clockThread = num(cc, "Clock Unhalted Thread");

      aggregates.push({
        scope: "system",
        instructions_retired: instrRetired,
        clock_unhalted_thread: clockThread,
        clock_unhalted_ref: num(cc, "Clock Unhalted Ref"),
        ipc: clockThread > 0 ? Math.round((instrRetired / clockThread) * 1000) / 1000 : 0,
        l3_cache_misses: num(cc, "L3 Cache Misses"),
        l3_cache_hits: num(cc, "L3 Cache Hits"),
        l2_cache_misses: num(cc, "L2 Cache Misses"),
        l2_cache_hits: num(cc, "L2 Cache Hits"),
      });
    }

    // ── Imbalance detection ─────────────────────────────────────────
    // Map cores to MTL scheduler threads and detect load asymmetry
    let imbalanceWarnings: PcmImbalanceWarning[] = [];

    if (cores.length >= 2) {
      try {
        const schMap = await findSchedulerThreads();
        if (schMap.size > 0) {
          imbalanceWarnings = detectCoreImbalance(cores, schMap);
        }
      } catch {
        // Non-fatal: imbalance detection is best-effort
      }
    }

    // ── Build multi_sample stats (only when samples > 1) ────────────
    const result: PcmCoreCountersData = {
      interval_us: intervalUs,
      cores,
      aggregates,
      imbalance_warnings: imbalanceWarnings,
    };

    if (samples > 1 && accumulator.size > 0) {
      const perCore: Array<{
        os_id: number; core: number; socket: number;
        metrics: Record<string, { mean: number; stddev: number; min: number; max: number }>;
      }> = [];

      for (const [osId, acc] of accumulator) {
        const statsMap: Record<string, { mean: number; stddev: number; min: number; max: number }> = {};
        for (const m of TRACKED_METRICS) {
          const s = computeStats(acc.metrics[m]);
          if (s) statsMap[m] = s;
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
      `Failed to fetch PCM core counters: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
