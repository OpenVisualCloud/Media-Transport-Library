/**
 * emon-bridge.ts — Singleton bridge to Intel EMON command-line tool.
 *
 * EMON is fire-and-forget per window (unlike PCM's always-on HTTP).
 * Each collection:
 *   1. Spawns `emon -C "events" -t <seconds>`
 *   2. Captures tab-separated stdout
 *   3. Parses per-unit values back to typed structures
 *
 * Tab-separated output format:
 *   EVENT_NAME\tTIMESTAMP_US\tVAL0\tVAL1\t...\tVALN
 *
 * Value columns depend on PMU type:
 *   - Core events: 224 values (1 per logical CPU)
 *   - CHA events: 113 values (~56 per socket, plus possible padding)
 *   - IIO events: 24 values (12 stacks per socket × 2 sockets)
 *   - UPI events: 9 values
 *   - IMC events: 17 values
 *   - M2M events: 9 values
 *
 * Group separator: "----------" between event groups
 * End separator: "==========" at end of sample
 */
import { spawn } from "child_process";
import { existsSync } from "fs";
import { readFile, readdir } from "fs/promises";
import type {
  EmonConnectionStatus,
  EmonPresetId,
  EmonPresetInfo,
  EmonRawCollection,
  EmonRawSample,
  EmonEventLine,
  EmonIioData,
  EmonIioPortMetrics,
  EmonChaData,
  EmonChaMetrics,
  EmonMeshStallData,
  EmonMeshStallSocket,
  EmonUpiData,
  EmonUpiLinkMetrics,
  EmonCoreStallData,
  EmonCoreStallEntry,
  EmonCollectResult,
} from "../types-emon.js";

// ─── Configuration ──────────────────────────────────────────────────────────

const EMON_SEARCH_PATHS = [
  "/opt/intel/sep/bin64/emon",
  "/opt/intel/sep_private/bin64/emon",
  "/usr/local/bin/emon",
  "/usr/bin/emon",
];

/** Paths to the insmod-sep driver loading script */
const SEP_INSMOD_PATHS = [
  "/opt/intel/sep/sepdk/src/insmod-sep",
  "/opt/intel/sep_private/sepdk/src/insmod-sep",
];

/** Maximum collection window (seconds) to prevent runaway processes */
const MAX_COLLECTION_SECONDS = 30;

/** Extra time beyond collection window before we kill the process (ms) */
const SPAWN_OVERHEAD_MS = 5000;

// ─── Preset definitions ─────────────────────────────────────────────────────

/**
 * E0: IIO PCIe per-port bandwidth — the #1 gap PCM cannot fill.
 * Measures MEM_READ and MEM_WRITE for each of the 4 PARTs on every IIO stack.
 * This tells us exactly which NIC / PCIe device is generating traffic.
 */
const E0_EVENTS = [
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_READ.PART0",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_READ.PART1",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_READ.PART2",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_READ.PART3",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_WRITE.PART0",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_WRITE.PART1",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_WRITE.PART2",
  "UNC_IIO_DATA_REQ_OF_CPU.MEM_WRITE.PART3",
  "UNC_IIO_CLOCKTICKS",
].join(",");

/**
 * E1: CHA LLC snoop/coherence analysis.
 * Measures TOR insertions (hit/miss) and occupancy per CHA.
 * High TOR occupancy = high memory latency.
 */
const E1_EVENTS = [
  "UNC_CHA_TOR_INSERTS.IA_MISS_DRD",
  "UNC_CHA_TOR_INSERTS.IA_HIT_DRD",
  "UNC_CHA_TOR_INSERTS.IA_MISS_DRD_PREF",
  "UNC_CHA_TOR_INSERTS.IA_MISS_CRD",
  "UNC_CHA_TOR_INSERTS.IA_MISS_RFO",
  "UNC_CHA_TOR_OCCUPANCY.IA_MISS_DRD",
  "UNC_CHA_CLOCKTICKS",
].join(",");

/**
 * E2: Mesh stall / memory latency.
 * Combines CHA TOR with M2M tracker occupancy for a
 * comprehensive memory subsystem latency view.
 */
const E2_EVENTS = [
  // CHA side — overall miss rate and occupancy
  "UNC_CHA_TOR_INSERTS.IA_MISS_DRD",
  "UNC_CHA_TOR_OCCUPANCY.IA_MISS_DRD",
  "UNC_CHA_CLOCKTICKS",
].join(",") + ";" + [
  // M2M side — memory controller queue depth
  "UNC_M2M_TRACKER_OCCUPANCY.CH0",
  "UNC_M2M_TRACKER_OCCUPANCY.CH1",
  "UNC_M2M_CLOCKTICKS",
].join(",");

/**
 * E3: UPI detailed link analysis.
 * Per-link TX/RX data and non-data flits, power states, CRC errors.
 */
const E3_EVENTS = [
  "UNC_UPI_TxL_FLITS.ALL_DATA",
  "UNC_UPI_TxL_FLITS.NON_DATA",
  "UNC_UPI_RxL_FLITS.ALL_DATA",
  "UNC_UPI_RxL_FLITS.NON_DATA",
  "UNC_UPI_L1_POWER_CYCLES",
  "UNC_UPI_RxL0P_POWER_CYCLES",
  "UNC_UPI_TxL0P_POWER_CYCLES",
  "UNC_UPI_CLOCKTICKS",
].join(",") + ";" + [
  "UNC_UPI_RxL_CRC_LLR_REQ_TRANSMIT",
  "UNC_UPI_RxL_BYPASSED.SLOT0",
  "UNC_UPI_RxL_BYPASSED.SLOT1",
  "UNC_UPI_CLOCKTICKS",
].join(",");

/**
 * E4: Deep core micro-architectural stall analysis.
 * TOPDOWN with perf_metrics + uop execution port analysis.
 * Semicolons separate multiplexing groups.
 */
const E4_EVENTS = [
  "INST_RETIRED.ANY",
  "CPU_CLK_UNHALTED.THREAD",
  "CPU_CLK_UNHALTED.REF_TSC",
  "TOPDOWN.SLOTS:perf_metrics",
  "UOPS_EXECUTED.CORE_CYCLES_GE_1",
  "UOPS_EXECUTED.CORE_CYCLES_GE_2",
  "UOPS_EXECUTED.CORE_CYCLES_GE_3",
].join(",");

export const EMON_PRESETS: Record<EmonPresetId, EmonPresetInfo> = {
  E0_iio_pcie_per_port: {
    id: "E0_iio_pcie_per_port",
    name: "IIO PCIe Per-Port Bandwidth",
    description: "Per-IIO-stack, per-port PCIe read/write bandwidth. Maps NIC/device traffic to specific PCIe slots. The #1 gap PCM cannot fill.",
    events: E0_EVENTS,
    pmu_types: ["iio"],
    recommended_window_ms: 1000,
    conflicts_with_pcm: false,
  },
  E1_cha_llc_snoop: {
    id: "E1_cha_llc_snoop",
    name: "CHA LLC Snoop/Coherence",
    description: "Per-CHA Last Level Cache hit/miss analysis with TOR occupancy as latency proxy. Identifies hot LLC slices and coherence overhead.",
    events: E1_EVENTS,
    pmu_types: ["cha"],
    recommended_window_ms: 1000,
    conflicts_with_pcm: false,
  },
  E2_mesh_stall_latency: {
    id: "E2_mesh_stall_latency",
    name: "Mesh Stall / Memory Latency",
    description: "Combined CHA TOR + M2M tracker occupancy for memory subsystem latency estimation. High values indicate DRAM/mesh congestion.",
    events: E2_EVENTS,
    pmu_types: ["cha", "m2m"],
    recommended_window_ms: 2000,
    conflicts_with_pcm: false,
  },
  E3_upi_detailed: {
    id: "E3_upi_detailed",
    name: "UPI Link Detailed",
    description: "Per-link UPI analysis: data vs snoop flits, power states, CRC errors, bypass ratio. Detects cross-socket congestion and link errors.",
    events: E3_EVENTS,
    pmu_types: ["qpi"],
    recommended_window_ms: 2000,
    conflicts_with_pcm: false,
  },
  E4_core_stall_deep: {
    id: "E4_core_stall_deep",
    name: "Deep Core Stall Analysis",
    description: "Per-CPU top-down micro-architecture debugging: TMA Level 1 breakdown + execution port utilization. Identifies frontend/backend/speculation bottlenecks.",
    events: E4_EVENTS,
    pmu_types: ["core"],
    recommended_window_ms: 2000,
    conflicts_with_pcm: true,
  },
};

// ─── EMON output parser ─────────────────────────────────────────────────────

/**
 * Parse a single event line from EMON tab-separated output.
 * Format: EVENT_NAME<TAB>TIMESTAMP<TAB>VAL0<TAB>VAL1<TAB>...<TAB>VALN
 *
 * Values may contain commas (thousands separators) and whitespace.
 */
function parseEventLine(line: string): EmonEventLine | null {
  const fields = line.split("\t");
  if (fields.length < 3) return null;

  const event_name = fields[0].trim();
  if (!event_name || event_name.startsWith("#") || event_name.startsWith("Warning")) return null;

  const timestamp_us = parseEmonNumber(fields[1]);
  const values: number[] = [];

  for (let i = 2; i < fields.length; i++) {
    values.push(parseEmonNumber(fields[i]));
  }

  return { event_name, timestamp_us, values };
}

/**
 * Parse an emon number string: remove commas and whitespace, convert to number.
 */
function parseEmonNumber(s: string): number {
  const cleaned = s.trim().replace(/,/g, "");
  if (cleaned === "" || cleaned === "N/A") return 0;
  const n = Number(cleaned);
  return Number.isFinite(n) ? n : 0;
}

/**
 * Parse complete EMON stdout into structured samples.
 */
export function parseEmonOutput(stdout: string): EmonRawCollection {
  const lines = stdout.split("\n");
  const samples: EmonRawSample[] = [];
  let currentEvents: EmonEventLine[] = [];
  let cpuCount = 0;
  let collectionTime = 0;

  for (const line of lines) {
    const trimmed = line.trim();

    // Skip empty lines, warnings, version info lines
    if (!trimmed) continue;
    if (trimmed.startsWith("Warning:")) continue;
    if (trimmed.startsWith("Version Info:")) continue;

    // End of sample
    if (trimmed.startsWith("==========")) {
      if (currentEvents.length > 0) {
        samples.push({ events: currentEvents });
        currentEvents = [];
      }
      continue;
    }

    // Group separator within a sample (multiple event groups)
    if (trimmed.startsWith("----------")) {
      // Groups within the same sample — just continue
      continue;
    }

    // Parse collection time from summary line
    const timeMatch = trimmed.match(/^(\d+\.?\d*)s\s+real/);
    if (timeMatch) {
      collectionTime = parseFloat(timeMatch[1]);
      continue;
    }

    // Skip per-cpu summary lines (cpu N: ...)
    if (trimmed.startsWith("cpu ")) continue;

    // Try to parse as event line
    const evt = parseEventLine(line);
    if (evt) {
      // Track max CPU count from core events
      if (!evt.event_name.startsWith("UNC_") && !evt.event_name.startsWith("MSR_EVENT")) {
        cpuCount = Math.max(cpuCount, evt.values.length);
      }
      currentEvents.push(evt);
    }
  }

  // Flush any remaining events
  if (currentEvents.length > 0) {
    samples.push({ events: currentEvents });
  }

  return { samples, collection_time_s: collectionTime, cpu_count: cpuCount };
}

// ─── Event lookup helpers ───────────────────────────────────────────────────

/**
 * Find an event by name in a sample. Returns the values array or null.
 */
function findEvent(sample: EmonRawSample, name: string): number[] | null {
  for (const evt of sample.events) {
    if (evt.event_name === name) return evt.values;
  }
  return null;
}

/**
 * Find an event by partial name match (case-insensitive).
 */
function findEventLike(sample: EmonRawSample, pattern: string): number[] | null {
  const lower = pattern.toLowerCase();
  for (const evt of sample.events) {
    if (evt.event_name.toLowerCase().includes(lower)) return evt.values;
  }
  return null;
}

// ─── Preset data extractors ─────────────────────────────────────────────────

/**
 * SPR has 12 IIO stacks per socket, 2 sockets = 24 total.
 * Output columns are [socket0_stack0, ..., socket0_stack11, socket1_stack0, ..., socket1_stack11]
 */
const IIO_STACKS_PER_SOCKET = 12;
const NUM_SOCKETS = 2;

/**
 * Extract E0 IIO PCIe per-port data from a raw sample.
 */
function extractIioData(sample: EmonRawSample, windowS: number): EmonIioData {
  const ports: EmonIioPortMetrics[] = [];

  // 4 PARTs × 2 directions = 8 event series + clockticks
  const readPart: (number[] | null)[] = [];
  const writePart: (number[] | null)[] = [];
  for (let p = 0; p < 4; p++) {
    readPart.push(findEvent(sample, `UNC_IIO_DATA_REQ_OF_CPU.MEM_READ.PART${p}`));
    writePart.push(findEvent(sample, `UNC_IIO_DATA_REQ_OF_CPU.MEM_WRITE.PART${p}`));
  }
  const clockticks = findEvent(sample, "UNC_IIO_CLOCKTICKS");

  const totalPerSocket: Record<number, { read: number; write: number }> = {};

  for (let sock = 0; sock < NUM_SOCKETS; sock++) {
    totalPerSocket[sock] = { read: 0, write: 0 };
    for (let stack = 0; stack < IIO_STACKS_PER_SOCKET; stack++) {
      const idx = sock * IIO_STACKS_PER_SOCKET + stack;

      for (let part = 0; part < 4; part++) {
        const rd = readPart[part]?.[idx] ?? 0;
        const wr = writePart[part]?.[idx] ?? 0;

        // Skip empty ports
        if (rd === 0 && wr === 0) continue;

        // EMON IIO counts are in 4-byte granularity
        const readMbps = windowS > 0 ? (rd * 4) / windowS / 1e6 : 0;
        const writeMbps = windowS > 0 ? (wr * 4) / windowS / 1e6 : 0;

        ports.push({
          socket: sock,
          stack,
          part,
          mem_read_count: rd,
          mem_write_count: wr,
          read_mbps: Math.round(readMbps * 100) / 100,
          write_mbps: Math.round(writeMbps * 100) / 100,
          clockticks: clockticks?.[idx] ?? 0,
        });

        totalPerSocket[sock].read += readMbps;
        totalPerSocket[sock].write += writeMbps;
      }
    }
  }

  return {
    ports,
    socket_totals: Object.entries(totalPerSocket).map(([s, t]) => ({
      socket: parseInt(s),
      total_read_mbps: Math.round(t.read * 100) / 100,
      total_write_mbps: Math.round(t.write * 100) / 100,
    })),
  };
}

/**
 * SPR has 56 CHAs per socket (may be 57 with padding).
 * CHA output: columns are [socket0_cha0, ..., socket0_cha55, socket1_cha0, ..., socket1_cha55]
 * Total might be 113 with one padding column between sockets.
 */
function extractChaData(sample: EmonRawSample, windowS: number): EmonChaData {
  const missDrd = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_MISS_DRD");
  const hitDrd = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_HIT_DRD");
  const missDrdPref = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_MISS_DRD_PREF");
  const missCrd = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_MISS_CRD");
  const missRfo = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_MISS_RFO");
  const torOccMissDrd = findEvent(sample, "UNC_CHA_TOR_OCCUPANCY.IA_MISS_DRD");
  const clockticks = findEvent(sample, "UNC_CHA_CLOCKTICKS");

  if (!missDrd) return { chas: [], socket_totals: [] };

  const totalUnits = missDrd.length;
  // Determine CHAs per socket — may have an extra padding column
  const chasPerSocket = Math.ceil(totalUnits / NUM_SOCKETS);

  const chas: EmonChaMetrics[] = [];
  const socketAcc: Record<number, { misses: number; hits: number; torOcc: number; clk: number; count: number }> = {};

  for (let sock = 0; sock < NUM_SOCKETS; sock++) {
    socketAcc[sock] = { misses: 0, hits: 0, torOcc: 0, clk: 0, count: 0 };
    for (let cha = 0; cha < chasPerSocket; cha++) {
      const idx = sock * chasPerSocket + cha;
      if (idx >= totalUnits) break;

      const miss = missDrd[idx] ?? 0;
      const hit = hitDrd?.[idx] ?? 0;
      const clk = clockticks?.[idx] ?? 0;

      // Skip zero-clock CHAs (may be padding)
      if (clk === 0 && miss === 0 && hit === 0) continue;

      const entry: EmonChaMetrics = {
        socket: sock,
        cha_id: cha,
        tor_inserts_ia_miss_drd: miss,
        tor_inserts_ia_hit_drd: hit,
        tor_inserts_ia_miss_drd_pref: missDrdPref?.[idx] ?? 0,
        tor_inserts_ia_miss_crd: missCrd?.[idx] ?? 0,
        tor_inserts_ia_miss_rfo: missRfo?.[idx] ?? 0,
        tor_occupancy_ia_miss_drd: torOccMissDrd?.[idx] ?? 0,
        clockticks: clk,
      };
      chas.push(entry);

      socketAcc[sock].misses += miss;
      socketAcc[sock].hits += hit;
      socketAcc[sock].torOcc += torOccMissDrd?.[idx] ?? 0;
      socketAcc[sock].clk += clk;
      socketAcc[sock].count++;
    }
  }

  return {
    chas,
    socket_totals: Object.entries(socketAcc).map(([s, acc]) => {
      const total = acc.misses + acc.hits;
      return {
        socket: parseInt(s),
        total_llc_misses: acc.misses,
        total_llc_hits: acc.hits,
        hit_ratio_pct: total > 0 ? Math.round((acc.hits / total) * 10000) / 100 : 0,
        // TOR occupancy normalized by clockticks: avg entries in-flight across CHAs
        avg_tor_occupancy: acc.clk > 0 ? Math.round((acc.torOcc / acc.clk) * 100) / 100 : 0,
      };
    }),
  };
}

/**
 * Extract E2 mesh stall data from CHA TOR + M2M occupancy.
 */
function extractMeshStallData(sample: EmonRawSample, windowS: number): EmonMeshStallData {
  const missDrd = findEvent(sample, "UNC_CHA_TOR_INSERTS.IA_MISS_DRD");
  const torOcc = findEvent(sample, "UNC_CHA_TOR_OCCUPANCY.IA_MISS_DRD");
  const chaClk = findEvent(sample, "UNC_CHA_CLOCKTICKS");
  const m2mOccCh0 = findEvent(sample, "UNC_M2M_TRACKER_OCCUPANCY.CH0");
  const m2mOccCh1 = findEvent(sample, "UNC_M2M_TRACKER_OCCUPANCY.CH1");

  const sockets: EmonMeshStallSocket[] = [];

  // CHA data
  const chaTotal = missDrd?.length ?? 0;
  const chasPerSocket = chaTotal > 0 ? Math.ceil(chaTotal / NUM_SOCKETS) : 56;

  for (let sock = 0; sock < NUM_SOCKETS; sock++) {
    let totalTorOcc = 0;
    let totalInserts = 0;
    let totalClk = 0;
    let chaCount = 0;

    for (let cha = 0; cha < chasPerSocket; cha++) {
      const idx = sock * chasPerSocket + cha;
      if (idx >= chaTotal) break;

      const occ = torOcc?.[idx] ?? 0;
      const ins = missDrd?.[idx] ?? 0;
      const clk = chaClk?.[idx] ?? 0;

      if (clk > 0) {
        totalTorOcc += occ;
        totalInserts += ins;
        totalClk += clk;
        chaCount++;
      }
    }

    // Estimate memory latency: avg TOR occupancy / insert rate × clock period
    // TOR occupancy is in cycles, so avg_occ / inserts ≈ average cycles per miss
    const avgTorOcc = chaCount > 0 ? totalTorOcc / chaCount : 0;
    let estLatencyNs: number | undefined;
    if (totalInserts > 0 && totalClk > 0) {
      // Average cycles per miss across all CHAs
      const cyclesPerMiss = totalTorOcc / totalInserts;
      // CHA runs at uncore frequency (~2GHz typically), so 1 cycle ≈ 0.5ns
      // More precisely: cycles_per_miss / (total_clk / collection_time)
      const uncoreFreqHz = windowS > 0 ? totalClk / chaCount / windowS : 2e9;
      estLatencyNs = Math.round((cyclesPerMiss / uncoreFreqHz) * 1e9 * 100) / 100;
    }

    // M2M data — units are per M2M instance (SPR has 4 M2M per socket)
    const m2mTotal = m2mOccCh0?.length ?? 0;
    const m2mPerSocket = m2mTotal > 0 ? Math.ceil(m2mTotal / NUM_SOCKETS) : 0;
    let totalM2mOcc = 0;
    if (m2mPerSocket > 0) {
      for (let u = 0; u < m2mPerSocket; u++) {
        const idx = sock * m2mPerSocket + u;
        totalM2mOcc += (m2mOccCh0?.[idx] ?? 0) + (m2mOccCh1?.[idx] ?? 0);
      }
    }

    sockets.push({
      socket: sock,
      avg_tor_occupancy_drd: Math.round(avgTorOcc * 100) / 100,
      estimated_mem_latency_ns: estLatencyNs,
      m2m_tracker_occupancy: totalM2mOcc > 0 ? totalM2mOcc : undefined,
    });
  }

  return { sockets };
}

/**
 * SPR has 4 UPI links per socket = ~9 total columns.
 * Layout: [socket0_link0, ..., socket0_linkN, socket1_link0, ..., socket1_linkN]
 * We detect the split point from clockticks (active vs zero).
 */
function extractUpiData(sample: EmonRawSample, windowS: number): EmonUpiData {
  const txData = findEvent(sample, "UNC_UPI_TxL_FLITS.ALL_DATA");
  const txNonData = findEvent(sample, "UNC_UPI_TxL_FLITS.NON_DATA");
  const rxData = findEvent(sample, "UNC_UPI_RxL_FLITS.ALL_DATA");
  const rxNonData = findEvent(sample, "UNC_UPI_RxL_FLITS.NON_DATA");
  const l1Power = findEvent(sample, "UNC_UPI_L1_POWER_CYCLES");
  const rxL0p = findEvent(sample, "UNC_UPI_RxL0P_POWER_CYCLES");
  const txL0p = findEvent(sample, "UNC_UPI_TxL0P_POWER_CYCLES");
  const clockticks = findEvent(sample, "UNC_UPI_CLOCKTICKS");
  const crcErrors = findEvent(sample, "UNC_UPI_RxL_CRC_LLR_REQ_TRANSMIT");
  const bypassSlot0 = findEvent(sample, "UNC_UPI_RxL_BYPASSED.SLOT0");
  const bypassSlot1 = findEvent(sample, "UNC_UPI_RxL_BYPASSED.SLOT1");

  if (!txData || !clockticks) return { links: [], socket_totals: [] };

  const totalUnits = txData.length;
  // Detect links per socket by finding where clocktick values suggest a socket boundary
  // Simple heuristic: half the units belong to each socket
  const linksPerSocket = Math.ceil(totalUnits / NUM_SOCKETS);

  const links: EmonUpiLinkMetrics[] = [];
  const socketAcc: Record<number, { txD: number; rxD: number; txND: number; rxND: number }> = {};

  for (let sock = 0; sock < NUM_SOCKETS; sock++) {
    socketAcc[sock] = { txD: 0, rxD: 0, txND: 0, rxND: 0 };
    for (let link = 0; link < linksPerSocket; link++) {
      const idx = sock * linksPerSocket + link;
      if (idx >= totalUnits) break;

      const clk = clockticks[idx] ?? 0;
      if (clk === 0) continue; // Inactive link

      const entry: EmonUpiLinkMetrics = {
        socket: sock,
        link,
        tx_data_flits: txData[idx] ?? 0,
        tx_non_data_flits: txNonData?.[idx] ?? 0,
        rx_data_flits: rxData?.[idx] ?? 0,
        rx_non_data_flits: rxNonData?.[idx] ?? 0,
        l0p_cycles: (rxL0p?.[idx] ?? 0) + (txL0p?.[idx] ?? 0),
        l1_cycles: l1Power?.[idx] ?? 0,
        clockticks: clk,
        crc_errors: crcErrors?.[idx] ?? 0,
        rx_bypassed: (bypassSlot0?.[idx] ?? 0) + (bypassSlot1?.[idx] ?? 0),
      };
      links.push(entry);

      socketAcc[sock].txD += entry.tx_data_flits;
      socketAcc[sock].rxD += entry.rx_data_flits;
      socketAcc[sock].txND += entry.tx_non_data_flits;
      socketAcc[sock].rxND += entry.rx_non_data_flits;
    }
  }

  return {
    links,
    socket_totals: Object.entries(socketAcc).map(([s, acc]) => {
      const totalNonData = acc.txND + acc.rxND;
      const totalData = acc.txD + acc.rxD;
      return {
        socket: parseInt(s),
        total_tx_data_flits: acc.txD,
        total_rx_data_flits: acc.rxD,
        total_tx_non_data_flits: acc.txND,
        total_rx_non_data_flits: acc.rxND,
        data_to_snoop_ratio: totalNonData > 0 ? Math.round((totalData / totalNonData) * 100) / 100 : 0,
      };
    }),
  };
}

/**
 * Extract E4 core stall data.
 * Core events have 224 columns (1 per logical CPU).
 * perf_metrics gives us TMA Level 1 as ratios of TOPDOWN.SLOTS.
 *
 * SPR with perf_metrics: TOPDOWN.SLOTS:perf_metrics automatically populates
 * the metric registers, so the TMA values come from the same group as TOPDOWN.SLOTS.
 * We extract them from the EMON output if EMON reports them.
 *
 * NOTE: EMON with perf_metrics may output additional metric lines like
 * "perf_metrics.frontend_bound" etc. For now we derive from the raw events.
 */
function extractCoreStallData(sample: EmonRawSample, windowS: number): EmonCoreStallData {
  const instRetired = findEvent(sample, "INST_RETIRED.ANY");
  const clkThread = findEvent(sample, "CPU_CLK_UNHALTED.THREAD");
  const clkRef = findEvent(sample, "CPU_CLK_UNHALTED.REF_TSC");
  const topdownSlots = findEvent(sample, "TOPDOWN.SLOTS") ?? findEvent(sample, "TOPDOWN.SLOTS:perf_metrics");
  const uopsGe1 = findEvent(sample, "UOPS_EXECUTED.CORE_CYCLES_GE_1");
  const uopsGe2 = findEvent(sample, "UOPS_EXECUTED.CORE_CYCLES_GE_2");
  const uopsGe3 = findEvent(sample, "UOPS_EXECUTED.CORE_CYCLES_GE_3");

  // Also try to find perf_metrics TMA values (EMON may output these)
  const pmFrontend = findEventLike(sample, "perf_metrics.frontend_bound");
  const pmBadSpec = findEventLike(sample, "perf_metrics.bad_speculation");
  const pmBackend = findEventLike(sample, "perf_metrics.backend_bound");
  const pmRetiring = findEventLike(sample, "perf_metrics.retiring");

  if (!instRetired || !clkThread) {
    return {
      cores: [],
      system_avg: {
        ipc: 0, backend_bound_pct: 0, memory_bound_pct: 0,
        core_bound_pct: 0, frontend_bound_pct: 0, bad_speculation_pct: 0,
        retiring_pct: 0, exec_starvation_ratio: 0,
      },
    };
  }

  const cpuCount = instRetired.length;

  // We need to map CPU → socket/core/thread.
  // On SPR: CPUs 0-55 = socket0 core0-55 thread0, CPUs 56-111 = socket0 core0-55 thread1
  //         CPUs 112-167 = socket1 core0-55 thread0, CPUs 168-223 = socket1 core0-55 thread1
  // This is a simplification — actual mapping comes from /proc/cpuinfo, but this works for standard SPR layout
  const coresPerSocket = Math.ceil(cpuCount / (NUM_SOCKETS * 2));

  const cores: EmonCoreStallEntry[] = [];
  let totalInst = 0, totalClk = 0, totalSlots = 0;
  let totalFrontend = 0, totalBadSpec = 0, totalBackend = 0, totalRetiring = 0;
  let totalUopsGe1 = 0;
  let activeCount = 0;

  for (let cpu = 0; cpu < cpuCount; cpu++) {
    const inst = instRetired[cpu] ?? 0;
    const clk = clkThread[cpu] ?? 0;

    // Skip idle CPUs (very low instruction count)
    if (clk < 1000) continue;

    const socket = cpu < cpuCount / 2 ? 0 : 1;
    const cpuInSocket = cpu < cpuCount / 2 ? cpu : cpu - cpuCount / 2;
    const core = cpuInSocket < coresPerSocket ? cpuInSocket : cpuInSocket - coresPerSocket;
    const thread = cpuInSocket < coresPerSocket ? 0 : 1;

    const ipc = clk > 0 ? Math.round((inst / clk) * 1000) / 1000 : 0;
    const slots = topdownSlots?.[cpu] ?? 0;

    // TMA from perf_metrics (if available) — these are already percentages × 255
    // Actually, EMON with :perf_metrics modifier reports raw metric register values
    // We need to derive TMA from the TOPDOWN.SLOTS event group
    // For now, use perf_metrics values if present, else leave as 0
    let frontendPct = 0, badSpecPct = 0, backendPct = 0, retiringPct = 0;
    if (pmFrontend && pmRetiring && pmBadSpec && pmBackend) {
      // perf_metrics values are 0-255 scale → convert to percentage
      const fe = pmFrontend[cpu] ?? 0;
      const bs = pmBadSpec[cpu] ?? 0;
      const be = pmBackend[cpu] ?? 0;
      const rt = pmRetiring[cpu] ?? 0;
      const sum = fe + bs + be + rt;
      if (sum > 0) {
        frontendPct = Math.round((fe / sum) * 10000) / 100;
        badSpecPct = Math.round((bs / sum) * 10000) / 100;
        backendPct = Math.round((be / sum) * 10000) / 100;
        retiringPct = Math.round((rt / sum) * 10000) / 100;
      }
    }

    const ge1 = uopsGe1?.[cpu] ?? 0;
    const execStarvation = clk > 0 ? Math.round((1 - ge1 / clk) * 10000) / 100 : 0;

    cores.push({
      cpu,
      socket,
      core,
      thread,
      inst_retired: inst,
      clk_unhalted: clk,
      ipc,
      topdown_slots: slots,
      backend_bound_pct: backendPct,
      memory_bound_pct: 0, // Requires L2 TMA — not in E4 base preset
      core_bound_pct: 0,   // Requires L2 TMA
      frontend_bound_pct: frontendPct,
      bad_speculation_pct: badSpecPct,
      retiring_pct: retiringPct,
      uops_executed_cycles_ge1: ge1,
      uops_executed_cycles_ge3: uopsGe3?.[cpu] ?? 0,
      exec_starvation_ratio: Math.max(0, execStarvation),
    });

    totalInst += inst;
    totalClk += clk;
    totalSlots += slots;
    totalFrontend += frontendPct;
    totalBadSpec += badSpecPct;
    totalBackend += backendPct;
    totalRetiring += retiringPct;
    totalUopsGe1 += ge1;
    activeCount++;
  }

  return {
    cores,
    system_avg: {
      ipc: totalClk > 0 ? Math.round((totalInst / totalClk) * 1000) / 1000 : 0,
      backend_bound_pct: activeCount > 0 ? Math.round((totalBackend / activeCount) * 100) / 100 : 0,
      memory_bound_pct: 0,
      core_bound_pct: 0,
      frontend_bound_pct: activeCount > 0 ? Math.round((totalFrontend / activeCount) * 100) / 100 : 0,
      bad_speculation_pct: activeCount > 0 ? Math.round((totalBadSpec / activeCount) * 100) / 100 : 0,
      retiring_pct: activeCount > 0 ? Math.round((totalRetiring / activeCount) * 100) / 100 : 0,
      exec_starvation_ratio: totalClk > 0 ? Math.round((1 - totalUopsGe1 / totalClk) * 10000) / 100 : 0,
    },
  };
}

// ─── EmonBridge class ───────────────────────────────────────────────────────

export class EmonBridge {
  private emonPath: string | null = null;
  private _available: boolean = false;
  private _detected: boolean = false;
  private _version: string = "";
  private _eventCount: number = 0;
  private _pmuTypes: string[] = [];
  private _driversLoaded: boolean = false;
  private _error: string | undefined;

  // ─── Detection ──────────────────────────────────────────────────

  /**
   * Detect EMON availability: find binary, check version, verify drivers.
   * Called at startup and can be re-run after loading drivers.
   */
  async detect(force: boolean = false): Promise<void> {
    if (this._detected && !force) return;
    this._detected = true;

    // Reset state for re-detection
    if (force) {
      this._available = false;
      this._driversLoaded = false;
      this._error = undefined;
    }

    // Find emon binary — try multiple strategies in order of speed
    // 1. Check hardcoded search paths
    for (const p of EMON_SEARCH_PATHS) {
      if (existsSync(p)) {
        this.emonPath = p;
        break;
      }
    }

    // 2. Check EMON_PATH env var
    if (!this.emonPath && process.env["EMON_PATH"]) {
      const envPath = process.env["EMON_PATH"];
      if (existsSync(envPath)) {
        this.emonPath = envPath;
      }
    }

    // 3. Broader search: which, common install dirs, then filesystem search
    if (!this.emonPath) {
      this.emonPath = await this.searchForBinary("emon", [
        "/opt/intel/*/bin64/emon",
        "/opt/intel/oneapi/vtune/*/bin64/emon",
      ]);
    }

    if (!this.emonPath) {
      this._available = false;
      this._error = "EMON binary not found. Searched: hardcoded paths, $EMON_PATH, which, /opt/intel/*. " +
        "Install Intel SEP/VTune or set EMON_PATH env var.";
      return;
    }

    // Check version and drivers
    try {
      const versionOutput = await this.runEmon(["-v"], 5000);
      // Parse version line: "V11.49 ..."
      const verMatch = versionOutput.match(/V(\d+\.\d+)/);
      if (verMatch) this._version = verMatch[1];

      // Parse event count: "... NNN events"
      // Check with -1 for event count
      const eventOutput = await this.runEmon(["-1"], 10000);
      const lines = eventOutput.split("\n").filter(l => l.trim().length > 0);
      this._eventCount = lines.length;

      // Check PMU types
      const pmuOutput = await this.runEmon(["-pmu-types", "available"], 5000);
      this._pmuTypes = pmuOutput.split("\n")
        .map(l => l.trim())
        .filter(l => l.length > 0 && !l.startsWith("Available") && !l.startsWith("Warning") && !l.startsWith("Version"));

      // Check if SEP driver is loaded
      try {
        const lsmod = await this.runShell("lsmod | grep -E 'sep|socperf|pax'");
        this._driversLoaded = lsmod.includes("sep") || lsmod.includes("socperf");
      } catch {
        this._driversLoaded = false;
      }

      // Try a minimal collection to verify everything works
      try {
        await this.runEmon(["-C", "INST_RETIRED.ANY", "-t", "0.01"], 5000);
        this._available = true;
        this._error = undefined;
      } catch (err: any) {
        this._available = false;
        this._error = `EMON test collection failed: ${err?.message ?? err}`;
      }
    } catch (err: any) {
      this._available = false;
      this._error = `EMON detection failed: ${err?.message ?? err}`;
    }
  }

  // ─── Accessors ──────────────────────────────────────────────────

  get isAvailable(): boolean { return this._available; }
  get version(): string { return this._version; }
  get eventCount(): number { return this._eventCount; }
  get pmuTypes(): string[] { return this._pmuTypes; }
  get driversLoaded(): boolean { return this._driversLoaded; }
  get connectionError(): string | undefined { return this._error; }

  // ─── Auto-load SEP drivers ────────────────────────────────────

  /**
   * Attempt to load SEP drivers via insmod-sep, then re-detect.
   * Returns true if drivers were successfully loaded and EMON is now available.
   */
  async loadDrivers(): Promise<{ loaded: boolean; message: string }> {
    // Find insmod-sep script — try hardcoded paths first, then search
    let insmodPath: string | null = null;
    for (const p of SEP_INSMOD_PATHS) {
      if (existsSync(p)) {
        insmodPath = p;
        break;
      }
    }

    // If not at hardcoded paths, search near the discovered emon binary
    if (!insmodPath && this.emonPath) {
      // emon lives in .../bin64/emon, insmod-sep lives in .../sepdk/src/insmod-sep
      const sepRoot = this.emonPath.replace(/\/bin64\/emon$/, "");
      const candidate = `${sepRoot}/sepdk/src/insmod-sep`;
      if (existsSync(candidate)) {
        insmodPath = candidate;
      }
    }

    // Last resort: filesystem search
    if (!insmodPath) {
      insmodPath = await this.searchForBinary("insmod-sep", [
        "/opt/intel/*/sepdk/src/insmod-sep",
      ]);
    }

    if (!insmodPath) {
      return {
        loaded: false,
        message: "insmod-sep script not found in search paths: " + SEP_INSMOD_PATHS.join(", "),
      };
    }

    // Run insmod-sep
    try {
      const output = await this.runShell(`${insmodPath} 2>&1`);
      // Re-detect with force to pick up newly loaded drivers
      await this.detect(true);

      if (this._driversLoaded && this._available) {
        return {
          loaded: true,
          message: `SEP drivers loaded successfully via ${insmodPath}`,
        };
      } else {
        return {
          loaded: false,
          message: `insmod-sep ran but EMON still unavailable: ${this._error ?? "unknown error"}. Output: ${output.substring(0, 500)}`,
        };
      }
    } catch (err: any) {
      return {
        loaded: false,
        message: `insmod-sep failed: ${err?.message ?? err}. Ensure the MCP server runs as root (CAP_SYS_ADMIN).`,
      };
    }
  }

  /**
   * Ensure EMON is ready: if unavailable due to missing drivers, attempt to
   * load them automatically. Returns true if EMON is available after the check.
   */
  async ensureReady(): Promise<{ ready: boolean; autoLoadMessage?: string }> {
    if (this._available) {
      return { ready: true };
    }

    // Only attempt auto-load if we have the binary but drivers aren't loaded
    if (this.emonPath && !this._driversLoaded) {
      const result = await this.loadDrivers();
      return {
        ready: this._available,
        autoLoadMessage: result.message,
      };
    }

    return { ready: false };
  }

  getStatus(): EmonConnectionStatus {
    return {
      available: this._available,
      emon_path: this.emonPath ?? "",
      version: this._version || undefined,
      event_count: this._eventCount || undefined,
      pmu_types: this._pmuTypes.length > 0 ? this._pmuTypes : undefined,
      drivers_loaded: this._driversLoaded,
      error: this._error,
    };
  }

  // ─── Collection ─────────────────────────────────────────────────

  /**
   * Run a preset collection and return structured data.
   */
  async collectPreset(
    presetId: EmonPresetId,
    windowMs?: number,
    cpuFilter?: number[],
  ): Promise<EmonCollectResult> {
    const preset = EMON_PRESETS[presetId];
    if (!preset) {
      throw new Error(`Unknown preset: ${presetId}`);
    }

    const effectiveWindowMs = windowMs ?? preset.recommended_window_ms;
    const windowS = effectiveWindowMs / 1000;

    // Build emon command line.
    // IMPORTANT: EMON has NO -cpu flag — it always collects all CPUs.
    // cpu_filter is applied post-collection when extracting structured data.
    const args = ["-C", preset.events, "-t", String(windowS)];

    const start = Date.now();
    const stdout = await this.runEmon(args, effectiveWindowMs + SPAWN_OVERHEAD_MS);
    const actualDuration = Date.now() - start;

    // Parse raw output
    const raw = parseEmonOutput(stdout);

    if (raw.samples.length === 0) {
      throw new Error(`EMON collection returned no samples for preset ${presetId}`);
    }

    // Use the last (most complete) sample
    const sample = raw.samples[raw.samples.length - 1];

    // Extract structured data based on preset
    const result: EmonCollectResult = {
      preset: presetId,
      window_ms: effectiveWindowMs,
      actual_duration_ms: actualDuration,
    };

    switch (presetId) {
      case "E0_iio_pcie_per_port":
        result.iio = extractIioData(sample, windowS);
        break;
      case "E1_cha_llc_snoop":
        result.cha = extractChaData(sample, windowS);
        break;
      case "E2_mesh_stall_latency":
        result.mesh = extractMeshStallData(sample, windowS);
        break;
      case "E3_upi_detailed":
        result.upi = extractUpiData(sample, windowS);
        break;
      case "E4_core_stall_deep":
        result.core_stall = extractCoreStallData(sample, windowS);
        break;
    }

    // Post-collection CPU filter: EMON always collects all CPUs; we filter
    // the parsed results for E4 (per-CPU core stall data).
    // E0-E3 are uncore metrics (per-socket/stack/CHA/link) — filter doesn't apply.
    if (cpuFilter && cpuFilter.length > 0 && result.core_stall) {
      const cpuSet = new Set(cpuFilter);
      const filtered = result.core_stall.cores.filter(c => cpuSet.has(c.cpu));
      const n = filtered.length;
      if (n > 0) {
        const totInst = filtered.reduce((s, c) => s + c.inst_retired, 0);
        const totClk = filtered.reduce((s, c) => s + c.clk_unhalted, 0);
        const totUopsGe1 = filtered.reduce((s, c) => s + c.uops_executed_cycles_ge1, 0);
        result.core_stall = {
          cores: filtered,
          system_avg: {
            ipc: totClk > 0 ? Math.round((totInst / totClk) * 1000) / 1000 : 0,
            backend_bound_pct: Math.round(filtered.reduce((s, c) => s + c.backend_bound_pct, 0) / n * 100) / 100,
            memory_bound_pct: Math.round(filtered.reduce((s, c) => s + c.memory_bound_pct, 0) / n * 100) / 100,
            core_bound_pct: Math.round(filtered.reduce((s, c) => s + c.core_bound_pct, 0) / n * 100) / 100,
            frontend_bound_pct: Math.round(filtered.reduce((s, c) => s + c.frontend_bound_pct, 0) / n * 100) / 100,
            bad_speculation_pct: Math.round(filtered.reduce((s, c) => s + c.bad_speculation_pct, 0) / n * 100) / 100,
            retiring_pct: Math.round(filtered.reduce((s, c) => s + c.retiring_pct, 0) / n * 100) / 100,
            exec_starvation_ratio: totClk > 0 ? Math.round((1 - totUopsGe1 / totClk) * 10000) / 100 : 0,
          },
        };
      }
    }

    return result;
  }

  /**
   * Run a raw EMON collection with arbitrary events.
   */
  async collectRaw(events: string, windowMs: number): Promise<EmonRawCollection> {
    const windowS = windowMs / 1000;
    const args = ["-C", events, "-t", String(windowS)];
    const stdout = await this.runEmon(args, windowMs + SPAWN_OVERHEAD_MS);
    return parseEmonOutput(stdout);
  }

  // ─── Internals ──────────────────────────────────────────────────

  /**
   * Run emon with given arguments and capture stdout.
   */
  private runEmon(args: string[], timeoutMs: number): Promise<string> {
    return new Promise((resolve, reject) => {
      if (!this.emonPath) {
        reject(new Error("EMON binary not found"));
        return;
      }

      const proc = spawn(this.emonPath, args, {
        stdio: ["pipe", "pipe", "pipe"],
        env: { ...process.env, PATH: `${process.env["PATH"]}:/opt/intel/sep/bin64` },
      });

      let stdout = "";
      let stderr = "";

      proc.stdout?.on("data", (data: Buffer) => {
        stdout += data.toString();
      });
      proc.stderr?.on("data", (data: Buffer) => {
        stderr += data.toString();
      });

      const timer = setTimeout(() => {
        proc.kill("SIGKILL");
        reject(new Error(`EMON timed out after ${timeoutMs}ms`));
      }, timeoutMs);

      proc.on("error", (err) => {
        clearTimeout(timer);
        reject(err);
      });

      proc.on("close", (code) => {
        clearTimeout(timer);
        if (code === 0 || stdout.length > 0) {
          // EMON may exit with non-zero even when output is valid (e.g., warnings on stderr)
          resolve(stdout);
        } else {
          reject(new Error(`EMON exited with code ${code}: ${stderr.substring(0, 500)}`));
        }
      });
    });
  }

  /**
   * Run a shell command and return stdout.
   */
  private runShell(command: string): Promise<string> {
    return new Promise((resolve, reject) => {
      const proc = spawn("sh", ["-c", command], {
        stdio: ["pipe", "pipe", "pipe"],
        timeout: 5000,
      });

      let stdout = "";
      proc.stdout?.on("data", (data: Buffer) => { stdout += data.toString(); });
      proc.on("error", reject);
      proc.on("close", (code) => {
        if (code === 0) resolve(stdout);
        else reject(new Error(`Shell command failed with code ${code}`));
      });
    });
  }

  /**
   * Search for a binary by name using progressively slower strategies:
   * which, glob patterns, then scoped filesystem search under /opt/intel.
   * Returns the first executable path found, or null.
   */
  private async searchForBinary(name: string, globPatterns: string[]): Promise<string | null> {
    // Strategy 1: which
    try {
      const result = await this.runShell(`which ${name} 2>/dev/null`);
      const path = result.trim();
      if (path && existsSync(path)) return path;
    } catch (_) { /* not on PATH */ }

    // Strategy 2: glob patterns via shell expansion
    for (const pattern of globPatterns) {
      try {
        // Use ls with glob — shell expands it
        const result = await this.runShell(`ls -1 ${pattern} 2>/dev/null | head -1`);
        const path = result.trim();
        if (path && existsSync(path)) return path;
      } catch (_) { /* no matches */ }
    }

    // Strategy 3: scoped find in /opt/intel (where SEP/VTune are installed)
    try {
      const result = await this.runShell(`find /opt/intel -maxdepth 6 -name "${name}" -type f 2>/dev/null | head -1`);
      const path = result.trim();
      if (path && existsSync(path)) return path;
    } catch (_) { /* not found */ }

    return null;
  }
}

// ─── Singleton ──────────────────────────────────────────────────────────────

let _bridge: EmonBridge | null = null;

export function getEmonBridge(): EmonBridge {
  if (!_bridge) {
    _bridge = new EmonBridge();
  }
  return _bridge;
}
