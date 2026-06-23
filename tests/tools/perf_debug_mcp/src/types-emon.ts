/**
 * types-emon.ts — Type definitions for Intel EMON integration.
 *
 * EMON (Event MONitor) is the low-level PMU event collection engine
 * behind Intel VTune / SEP. It provides direct access to core and
 * uncore performance monitoring hardware for:
 *   - Per-IIO-stack/port PCIe bandwidth (what PCM can't do)
 *   - Per-CHA LLC snoop/coherence analysis
 *   - UPI link-level detailed breakdown
 *   - Deep core micro-architectural stall analysis
 *   - Mesh interconnect latency
 *
 * This system is 2-socket SPR (Sapphire Rapids):
 *   - 224 logical CPUs (2 sockets × 56 cores × 2 threads)
 *   - 112 CHAs (56 per socket)
 *   - 24 IIO stacks (12 per socket, 10 active per socket)
 *   - 9 UPI links (4-5 per socket)
 *   - 17 IMC units (8-9 per socket)
 *   - 4 PARTs per IIO stack (PCIe root-port sub-divisions)
 */

// ─── EMON presets ───────────────────────────────────────────────────────────

/**
 * Preset identifiers for curated event groups.
 * Each preset focuses on a gap that PCM cannot reach.
 */
export type EmonPresetId =
  | "E0_iio_pcie_per_port"
  | "E1_cha_llc_snoop"
  | "E2_mesh_stall_latency"
  | "E3_upi_detailed"
  | "E4_core_stall_deep";

export interface EmonPresetInfo {
  id: EmonPresetId;
  name: string;
  description: string;
  /** Event formula string for emon -C (semicolons separate groups) */
  events: string;
  /** Which PMU types this preset reads */
  pmu_types: string[];
  /** Recommended collection window in ms */
  recommended_window_ms: number;
  /** Whether this preset needs to stop PCM first (PMU contention) */
  conflicts_with_pcm: boolean;
}

// ─── EMON connection/detection ──────────────────────────────────────────────

export interface EmonConnectionStatus {
  available: boolean;
  emon_path: string;
  version?: string;
  event_count?: number;
  pmu_types?: string[];
  drivers_loaded: boolean;
  error?: string;
  auto_load_message?: string;
}

// ─── Raw parsed EMON output ─────────────────────────────────────────────────

/**
 * One event line from EMON's tab-separated output.
 * Values are indexed by unit number (CPU index for core events,
 * unit index for uncore events).
 */
export interface EmonEventLine {
  event_name: string;
  timestamp_us: number;
  /** Per-unit values — indexed as EMON reports them (sequential across sockets) */
  values: number[];
}

/**
 * A complete sample (one time-point) from an EMON collection.
 * Groups are separated by "----------" in EMON output.
 * The entire collection ends with "==========".
 */
export interface EmonRawSample {
  /** All event lines in this sample, in order */
  events: EmonEventLine[];
  /** Wall-clock time of the sample (from EMON header or derived) */
  timestamp_us?: number;
}

/**
 * Complete raw collection result from one emon invocation.
 */
export interface EmonRawCollection {
  samples: EmonRawSample[];
  collection_time_s: number;
  cpu_count: number;
}

// ─── E0: IIO PCIe per-port metrics ─────────────────────────────────────────

export interface EmonIioPortMetrics {
  socket: number;
  stack: number;
  part: number;
  /** PCIe reads by this port from CPU memory (4-byte granularity) → bytes = count × 4 */
  mem_read_count: number;
  /** PCIe writes by this port to CPU memory */
  mem_write_count: number;
  /** Approximate read bandwidth in MB/s (count × 4 / window_s / 1e6) */
  read_mbps: number;
  /** Approximate write bandwidth in MB/s */
  write_mbps: number;
  /** IIO clockticks for this stack (for utilization calculation) */
  clockticks: number;
}

export interface EmonIioData {
  ports: EmonIioPortMetrics[];
  /** Total aggregated across all ports per socket */
  socket_totals: {
    socket: number;
    total_read_mbps: number;
    total_write_mbps: number;
  }[];
}

// ─── E1: CHA LLC snoop/coherence ────────────────────────────────────────────

export interface EmonChaMetrics {
  socket: number;
  cha_id: number;
  /** TOR insertions: IA miss DRD (demand read that missed LLC) */
  tor_inserts_ia_miss_drd: number;
  /** TOR insertions: IA hit DRD (demand read that hit LLC) */
  tor_inserts_ia_hit_drd: number;
  /** TOR insertions: IA miss DRD prefetch */
  tor_inserts_ia_miss_drd_pref: number;
  /** TOR insertions: IA miss CRD (code read miss) */
  tor_inserts_ia_miss_crd: number;
  /** TOR insertions: IA miss RFO (read-for-ownership miss — write miss) */
  tor_inserts_ia_miss_rfo: number;
  /** TOR occupancy: average IA miss DRD entries (latency proxy) */
  tor_occupancy_ia_miss_drd: number;
  /** CHA clockticks */
  clockticks: number;
}

export interface EmonChaData {
  chas: EmonChaMetrics[];
  socket_totals: {
    socket: number;
    total_llc_misses: number;
    total_llc_hits: number;
    hit_ratio_pct: number;
    /** Average TOR occupancy (higher = more outstanding misses = higher latency) */
    avg_tor_occupancy: number;
  }[];
}

// ─── E2: Mesh stall / latency ───────────────────────────────────────────────

export interface EmonMeshStallSocket {
  socket: number;
  /** Average CHA TOR occupancy for DRD misses — proxy for memory access latency */
  avg_tor_occupancy_drd: number;
  /** Total mesh credits stalled (if events available) */
  mesh_credits_stalled?: number;
  /** Average memory read latency estimate in ns (tor_occupancy / tor_inserts × cha_clock_period) */
  estimated_mem_latency_ns?: number;
  /** M2M tracker occupancy (mesh-to-memory queue depth) */
  m2m_tracker_occupancy?: number;
}

export interface EmonMeshStallData {
  sockets: EmonMeshStallSocket[];
}

// ─── E3: UPI detailed ───────────────────────────────────────────────────────

export interface EmonUpiLinkMetrics {
  socket: number;
  link: number;
  /** TX flits — all data */
  tx_data_flits: number;
  /** TX flits — non-data (snoops, acks, requests) */
  tx_non_data_flits: number;
  /** RX flits — all data */
  rx_data_flits: number;
  /** RX flits — non-data */
  rx_non_data_flits: number;
  /** L0p power state cycles (low-power) */
  l0p_cycles: number;
  /** L1 power state cycles */
  l1_cycles: number;
  /** Clockticks for utilization calculation */
  clockticks: number;
  /** CRC retransmit requests (link errors) */
  crc_errors: number;
  /** RX bypassed (slot0+slot1) */
  rx_bypassed: number;
}

export interface EmonUpiData {
  links: EmonUpiLinkMetrics[];
  socket_totals: {
    socket: number;
    total_tx_data_flits: number;
    total_rx_data_flits: number;
    total_tx_non_data_flits: number;
    total_rx_non_data_flits: number;
    /** Data:Non-data ratio (>5 typical, <2 indicates excessive snooping) */
    data_to_snoop_ratio: number;
  }[];
}

// ─── E4: Core stall deep ───────────────────────────────────────────────────

export interface EmonCoreStallEntry {
  cpu: number;
  socket: number;
  core: number;
  thread: number;
  /** Instructions retired */
  inst_retired: number;
  /** CPU_CLK_UNHALTED.THREAD */
  clk_unhalted: number;
  /** IPC = inst_retired / clk_unhalted */
  ipc: number;
  /** TOPDOWN.SLOTS */
  topdown_slots: number;
  /** Backend bound slots (from perf_metrics) */
  backend_bound_pct: number;
  /** Memory bound subset of backend */
  memory_bound_pct: number;
  /** Core bound subset of backend */
  core_bound_pct: number;
  /** Frontend bound */
  frontend_bound_pct: number;
  /** Bad speculation */
  bad_speculation_pct: number;
  /** Retiring (useful work) */
  retiring_pct: number;
  /** Uops executed — cycles with >= 1 uop (for port contention analysis) */
  uops_executed_cycles_ge1: number;
  /** Uops executed — cycles with >= 3 uops (measure of wide execution) */
  uops_executed_cycles_ge3: number;
  /** Execution starvation ratio: 1 - (cycles_ge1 / clk_unhalted) */
  exec_starvation_ratio: number;
}

export interface EmonCoreStallData {
  cores: EmonCoreStallEntry[];
  system_avg: {
    ipc: number;
    backend_bound_pct: number;
    memory_bound_pct: number;
    core_bound_pct: number;
    frontend_bound_pct: number;
    bad_speculation_pct: number;
    retiring_pct: number;
    exec_starvation_ratio: number;
  };
}

// ─── Unified collection result ──────────────────────────────────────────────

/**
 * Result of an emon_collect call. Contains the preset used,
 * timing info, and structured data for the specific preset type.
 */
export interface EmonCollectResult {
  preset: EmonPresetId;
  window_ms: number;
  /** How long the actual emon command took (may be longer due to startup overhead) */
  actual_duration_ms: number;
  /** Structured data — shape depends on preset */
  iio?: EmonIioData;
  cha?: EmonChaData;
  mesh?: EmonMeshStallData;
  upi?: EmonUpiData;
  core_stall?: EmonCoreStallData;
}

// ─── Triage engine ──────────────────────────────────────────────────────────

export type TriageSeverity = "info" | "warning" | "critical";

export interface TriageEvidence {
  source: string;
  metric: string;
  value: number | string;
  threshold?: number | string;
}

export interface TriageCause {
  id: string;
  title: string;
  severity: TriageSeverity;
  description: string;
  evidence: TriageEvidence[];
  recommendations: string[];
}

export interface EmonTriageResult {
  /** Waterfall order: each cause feeds into the next check */
  causes: TriageCause[];
  /** Overall severity */
  overall_severity: TriageSeverity;
  /** Summary one-liner */
  summary: string;
  /** Which presets were actually run during triage */
  presets_used: EmonPresetId[];
  /** Total triage wall-clock time in ms */
  triage_duration_ms: number;
}

// ─── PCIe topology mapping ──────────────────────────────────────────────────

export interface PcieDeviceMapping {
  bdf: string;              // e.g. "0000:51:00.0"
  device_name: string;      // e.g. "Ethernet Controller E810-C"
  driver: string;           // e.g. "ice", "vfio-pci"
  numa_node: number;
  socket: number;
  iio_stack: number;
  iio_part: number;
  net_interface?: string;   // e.g. "ens1np0"
}

export interface EmonPcieTopologyData {
  devices: PcieDeviceMapping[];
  /** Map from "socket:stack:part" → device for quick lookup */
  iio_map: Record<string, PcieDeviceMapping>;
}
