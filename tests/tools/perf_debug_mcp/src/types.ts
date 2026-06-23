/**
 * cpu-debug-mcp — Shared type definitions
 * All tool responses follow the Output Contract from the spec.
 */

// ─── Top-level response envelope ────────────────────────────────────────────

export type ModeUsed = "fallback" | "ebpf" | "auto" | "usdt";

export interface HostInfo {
  hostname: string;
  kernel: string;
  cpu_count: number;
}

export interface ResponseMeta {
  timestamp_wall: string;       // ISO 8601
  t_monotonic_ns: number;
  mode_used: ModeUsed;
  window_ms?: number;
  host?: HostInfo;
}

export interface ResponseError {
  code: string;
  message: string;
  hint?: string;
}

export interface ToolResponse<T = unknown> {
  ok: boolean;
  meta: ResponseMeta;
  data?: T;
  error?: ResponseError;
}

// ─── Capabilities ───────────────────────────────────────────────────────────

export interface Capabilities {
  kernel_version: string;
  cpu_count: number;
  numa_nodes: number;
  can_read_proc: boolean;
  ebpf_mode_available: boolean;
  ebpf_mode_enabled: boolean;
  pcm_server_available: boolean;
  pcm_server_endpoint: string;
  emon_available: boolean;
  emon_version?: string;
  emon_event_count?: number;
  bcc_tools_available: boolean;
  bcc_tools_detected?: string[];
  usdt_available: boolean;
  usdt_bpftrace_version?: string;
  usdt_probes_detected?: number;
  usdt_libmtl_path?: string;
  rdma_available: boolean;
  devlink_available: boolean;
  missing_capabilities: string[];
  default_sampling_resolution_ms: number;
  max_recommended_window_ms: number;
  available_tools: string[];
}

// ─── Core load snapshot ─────────────────────────────────────────────────────

export interface CoreUtilization {
  cpu: number;
  util_pct: number;
  user_pct: number;
  system_pct: number;
  irq_pct: number;
  softirq_pct: number;
  iowait_pct: number;
  idle_pct: number;
}

export interface CoreLoadSnapshotData {
  cores: CoreUtilization[];
}

// ─── Task info (shared) ─────────────────────────────────────────────────────

export type SchedPolicy =
  | "SCHED_OTHER"
  | "SCHED_FIFO"
  | "SCHED_RR"
  | "SCHED_BATCH"
  | "SCHED_ISO"
  | "SCHED_IDLE"
  | "SCHED_DEADLINE"
  | "UNKNOWN";

export interface TaskInfo {
  pid: number;
  tid: number;
  comm: string;
  affinity_cpus?: number[];
  affinity_hexmask?: string;
  policy?: SchedPolicy;
  rt_priority?: number;
  nice?: number;
  is_kernel_thread?: boolean;
  cpuset_cpus?: number[];
  state?: string;
}

// ─── allowed_on_core ────────────────────────────────────────────────────────

export interface AllowedOnCoreData {
  cpu: number;
  task_count: number;
  tasks: TaskInfo[];
}

// ─── running_on_core ────────────────────────────────────────────────────────

export interface RunningTask {
  pid: number;
  tid: number;
  comm: string;
  runtime_ns?: number;
  runtime_pct?: number;
  switches?: number;
  last_seen_ms_ago?: number;
  policy?: SchedPolicy;
  rt_priority?: number;
  state?: string;
}

export interface RunningOnCoreData {
  cpu: number;
  tasks: RunningTask[];
}

// ─── starvation_report ──────────────────────────────────────────────────────

export type StarvationFlag =
  | "RUNQUEUE_LATENCY_HIGH"
  | "UNEXPECTED_CO_RUNNERS_ON_SOLO_CPU"
  | "SOFTIRQ_PRESSURE"
  | "IRQ_PRESSURE"
  | "FREQUENT_PREEMPTION"
  | "CPU_THROTTLING_DETECTED"
  | "RT_THROTTLING_DETECTED";

export interface StarvationTarget {
  pid: number;
  tid: number;
  comm: string;
}

export interface StarvationThreadReport {
  tid: number;
  comm: string;
  runtime_ns?: number;
  runtime_pct?: number;
  cpu_observed: number[];
  switches?: number;
  wakeup_latency_ns?: number;
}

export interface StarvationReportData {
  target_resolution: StarvationTarget[];
  affinity_summary: {
    cpus_allowed: number[];
    is_single_cpu_pinned: boolean;
    cpuset_cpus?: number[];
  };
  runtime_per_thread: StarvationThreadReport[];
  top_interferers: RunningTask[];
  starvation_flags: StarvationFlag[];
  recommended_next_steps: string[];
}

// ─── IRQ tools ──────────────────────────────────────────────────────────────

export interface IrqPerCpu {
  cpu: number;
  irq_total_delta: number;
}

export interface IrqDetail {
  irq: string;
  name: string;
  delta_total: number;
  per_cpu_deltas: { cpu: number; delta: number }[];
}

export interface IrqDistributionData {
  per_cpu: IrqPerCpu[];
  top_irqs: IrqDetail[];
}

export interface IrqAffinityEntry {
  irq: string;
  name?: string;
  affinity_list: number[];
  effective_affinity_list?: number[];
}

export interface IrqAffinityData {
  irqs: IrqAffinityEntry[];
}

export interface NicQueueEntry {
  queue: string;
  direction: "rx" | "tx" | "combined";
  irq?: string;
  irq_name?: string;
  affinity_list?: number[];
}

export interface NicIrqQueueMapData {
  iface: string;
  queues: NicQueueEntry[];
}

// ─── Softirq ────────────────────────────────────────────────────────────────

export interface SoftirqCpuBreakdown {
  cpu: number;
  total_delta: number;
  breakdown: Record<string, number>;
}

export interface SoftirqSnapshotData {
  per_cpu: SoftirqCpuBreakdown[];
  hot_cpus: number[];
}

// ─── Kernel threads on core ─────────────────────────────────────────────────

export interface KernelThreadsOnCoreData {
  cpu: number;
  tasks: RunningTask[];
}

// ─── Runqueue snapshot ──────────────────────────────────────────────────────

export interface RunqueueGlobal {
  loadavg_1: number;
  loadavg_5: number;
  loadavg_15: number;
  runnable: number;
  running: number;
}

export interface RunqueuePerCpu {
  cpu: number;
  runnable_estimate?: number;
  context_switch_rate?: number;
}

export interface RunqueueSnapshotData {
  global: RunqueueGlobal;
  per_cpu?: RunqueuePerCpu[];
}

// ─── Context switch rate ────────────────────────────────────────────────────

export interface ContextSwitchRateData {
  global_ctxt_per_s: number;
  per_cpu?: { cpu: number; ctxt_per_s: number }[];
}

// ─── Isolation summary ──────────────────────────────────────────────────────

export interface IsolationSummaryData {
  cmdline_flags: Record<string, string>;
  isolated_cpus: number[];
  nohz_full_cpus: number[];
  rcu_nocbs_cpus: number[];
  warnings: string[];
}

// ─── CPU frequency ──────────────────────────────────────────────────────────

export interface CpuFrequencyEntry {
  cpu: number;
  cur_khz: number | null;
  min_khz: number | null;
  max_khz: number | null;
  governor: string | null;
}

export interface CpuFrequencySnapshotData {
  cpus: CpuFrequencyEntry[];
}

// ─── Throttling summary ─────────────────────────────────────────────────────

export interface ThrottlingSummaryData {
  thermal_throttle_count?: Record<string, number>;
  notes: string[];
}

// ─── cgroup CPU limits ──────────────────────────────────────────────────────

export interface CgroupCpuLimitsData {
  cgroup_path: string;
  cpu_max: string | null;
  cpu_weight: number | null;
  throttled_usec_delta?: number;
  nr_throttled_delta?: number;
  cpuset_effective?: number[];
}

// ─── NUMA ───────────────────────────────────────────────────────────────────

export interface NumaNode {
  node: number;
  cpus: number[];
  memory_mb?: number;
}

export interface NumaTopologyData {
  nodes: NumaNode[];
  distances?: number[][];
}

export interface ProcessNumaPlacementData {
  pid: number;
  cpus_allowed_list: number[];
  mems_allowed_list: number[];
  notes: string[];
}

// ─── Perf debug snapshot (aggregator) ───────────────────────────────────────

export interface PerfDebugSnapshotData {
  core_load: CoreLoadSnapshotData;
  running_on_cores: Record<number, RunningTask[]>;
  irq_distribution: IrqDistributionData;
  softirq_snapshot: SoftirqSnapshotData;
  cpu_frequency: CpuFrequencySnapshotData;
  isolation: IsolationSummaryData;
  cgroup_limits?: CgroupCpuLimitsData;
  pcm_core_counters?: PcmCoreCountersData;
  pcm_power_thermal?: PcmPowerThermalData;
}

// ─── eBPF bridge types ──────────────────────────────────────────────────────

export interface EbpfSchedEntry {
  cpu: number;
  pid: number;
  tid: number;
  comm: string;
  runtime_ns: number;
  switches: number;
}

export interface EbpfIrqEntry {
  cpu: number;
  irq: number;
  time_ns: number;
  count: number;
}

export interface EbpfSoftirqEntry {
  cpu: number;
  vec: number;
  time_ns: number;
  count: number;
}

export interface EbpfSnapshot {
  sched: EbpfSchedEntry[];
  irq?: EbpfIrqEntry[];
  softirq?: EbpfSoftirqEntry[];
  window_ms: number;
}

// ─── Intel PCM (pcm-sensor-server) types ────────────────────────────────────

/** Status of the pcm-sensor-server connection */
export interface PcmConnectionStatus {
  available: boolean;
  endpoint: string;
  error?: string;
}

// ── pcm_core_counters ────────────────────────────────────────────────────────

export interface PcmCoreCounterEntry {
  socket: number;
  core: number;
  thread: number;
  os_id: number;
  instructions_retired: number;
  clock_unhalted_thread: number;
  clock_unhalted_ref: number;
  ipc: number;                        // instructions_retired / clock_unhalted_thread
  active_frequency_mhz: number;
  l3_cache_misses: number;
  l3_cache_hits: number;
  l2_cache_misses: number;
  l2_cache_hits: number;
  l3_cache_occupancy: number;
  smi_count: number;
  local_memory_bw: number;
  remote_memory_bw: number;
}

export interface PcmCoreAggregateEntry {
  scope: "socket" | "system";
  socket_id?: number;
  instructions_retired: number;
  clock_unhalted_thread: number;
  clock_unhalted_ref: number;
  ipc: number;
  l3_cache_misses: number;
  l3_cache_hits: number;
  l2_cache_misses: number;
  l2_cache_hits: number;
}

export interface PcmImbalanceWarningCore {
  os_id: number;
  sch_thread: string;
  local_memory_bw: number;
  instructions_retired: number;
  ipc: number;
}

export interface PcmImbalanceWarning {
  process_pid: number;
  process_comm: string;
  cores: PcmImbalanceWarningCore[];
  flagged: boolean;
  metric: string;
  max_value: number;
  min_value: number;
  ratio: number;
  message: string;
}

export interface PcmCoreCountersData {
  interval_us: number;
  cores: PcmCoreCounterEntry[];
  aggregates: PcmCoreAggregateEntry[];
  imbalance_warnings: PcmImbalanceWarning[];
  multi_sample?: {
    samples: number;
    interval_s: number;
    per_core: Array<{
      os_id: number;
      core: number;
      socket: number;
      metrics: Record<string, { mean: number; stddev: number; min: number; max: number }>;
    }>;
  };
}

// ── pcm_memory_bandwidth ─────────────────────────────────────────────────────

export interface PcmMemoryBandwidthSocket {
  socket: number;
  dram_reads_bytes: number;
  dram_writes_bytes: number;
  dram_reads_mbps: number;
  dram_writes_mbps: number;
  pmm_reads_bytes: number;
  pmm_writes_bytes: number;
  edram_reads_bytes: number;
  edram_writes_bytes: number;
  mc_ia_requests_bytes: number;
  mc_gt_requests_bytes: number;
  mc_io_requests_bytes: number;
  local_memory_request_ratio_pct: number;
  remote_memory_request_ratio_pct: number;
}

export interface PcmMemoryBandwidthData {
  interval_us: number;
  sockets: PcmMemoryBandwidthSocket[];
}

// ── pcm_cache_analysis ───────────────────────────────────────────────────────

export interface PcmCacheCoreEntry {
  socket: number;
  core: number;
  thread: number;
  os_id: number;
  l3_hit_ratio: number;
  l3_miss_ratio: number;
  l2_hit_ratio: number;
  l2_miss_ratio: number;
  l3_cache_occupancy: number;
  l3_misses: number;
  l3_hits: number;
  l2_misses: number;
  l2_hits: number;
}

export interface PcmCacheAnalysisData {
  interval_us: number;
  cores: PcmCacheCoreEntry[];
  system_l3_hit_ratio: number;
  system_l3_miss_ratio: number;
  system_l2_hit_ratio: number;
  system_l2_miss_ratio: number;
  multi_sample?: {
    samples: number;
    interval_s: number;
    per_core: Array<{
      os_id: number;
      core: number;
      socket: number;
      metrics: Record<string, { mean: number; stddev: number; min: number; max: number }>;
    }>;
  };
}

// ── pcm_power_thermal ────────────────────────────────────────────────────────

export interface PcmTmaBreakdown {
  frontend_bound_pct: number;
  bad_speculation_pct: number;
  backend_bound_pct: number;
  retiring_pct: number;
  fetch_latency_bound_pct?: number;
  fetch_bandwidth_bound_pct?: number;
  branch_misprediction_bound_pct?: number;
  machine_clears_bound_pct?: number;
  memory_bound_pct?: number;
  core_bound_pct?: number;
  heavy_operations_bound_pct?: number;
  light_operations_bound_pct?: number;
}

export interface PcmCorePowerEntry {
  socket: number;
  core: number;
  thread: number;
  os_id: number;
  thermal_headroom_c: number;
  c_state_residency: Record<string, number>;
}

export interface PcmSocketPowerEntry {
  socket: number;
  package_joules: number;
  pp0_joules: number;
  pp1_joules: number;
  dram_joules: number;
  package_c_state_residency: Record<string, number>;
  uncore_frequencies_mhz: number[];
}

export interface PcmPowerThermalData {
  interval_us: number;
  cores: PcmCorePowerEntry[];
  sockets: PcmSocketPowerEntry[];
  tma?: PcmTmaBreakdown;
}

// ── pcm_qpi_upi_link ────────────────────────────────────────────────────────

export interface PcmQpiLinkEntry {
  link: number;
  incoming_bytes: number;
  outgoing_bytes: number;
  incoming_utilization: number;
  outgoing_utilization: number;
}

export interface PcmQpiSocketEntry {
  socket: number;
  cxl_write_cache_bytes: number;
  cxl_write_mem_bytes: number;
  links: PcmQpiLinkEntry[];
}

export interface PcmQpiUpiLinkData {
  interval_us: number;
  sockets: PcmQpiSocketEntry[];
}

// ── pcm_pcie_bandwidth ───────────────────────────────────────────────────────

/** PCIe bandwidth from uncore counters (what pcm-sensor-server exposes) */
export interface PcmPcieBandwidthSocket {
  socket: number;
  mc_ia_requests_bytes: number;
  mc_gt_requests_bytes: number;
  mc_io_requests_bytes: number;
}

export interface PcmPcieBandwidthData {
  interval_us: number;
  sockets: PcmPcieBandwidthSocket[];
  note: string;
}

// ── pcm_numa_traffic ─────────────────────────────────────────────────────────

export interface PcmNumaCoreMemBw {
  socket: number;
  core: number;
  thread: number;
  os_id: number;
  local_memory_bw_bytes: number;
  remote_memory_bw_bytes: number;
  local_ratio_pct: number;
}

export interface PcmNumaSocketSummary {
  socket: number;
  total_local_bw_bytes: number;
  total_remote_bw_bytes: number;
  local_ratio_pct: number;
}

export interface PcmNumaTrafficData {
  interval_us: number;
  per_core: PcmNumaCoreMemBw[];
  per_socket: PcmNumaSocketSummary[];
}

// ─── Internal helpers ───────────────────────────────────────────────────────

/** Raw /proc/stat CPU line counters */
export interface ProcStatCpuCounters {
  cpu: number;
  user: number;
  nice: number;
  system: number;
  idle: number;
  iowait: number;
  irq: number;
  softirq: number;
  steal: number;
  guest: number;
  guest_nice: number;
}

/** Raw /proc/<pid>/task/<tid>/stat fields we care about */
export interface ProcTaskStat {
  pid: number;
  comm: string;
  state: string;
  ppid: number;
  pgrp: number;
  session: number;
  tty_nr: number;
  flags: number;
  minflt: number;
  majflt: number;
  utime: number;
  stime: number;
  priority: number;
  nice: number;
  num_threads: number;
  starttime: number;
  vsize: number;
  rss: number;
  processor: number; // last CPU
  rt_priority: number;
  policy: number;
}

// ── rdma_health ──────────────────────────────────────────────────────────────

export interface RdmaHwCounter {
  name: string;
  value: number;
  delta?: number;
  rate_per_sec?: number;
}

export interface RdmaQpInfo {
  ifname: string;
  lqpn: number;
  rqpn?: number;
  type: string;
  state: string;
  pid?: number;
  comm?: string;
}

export interface RdmaDeviceHealth {
  device: string;
  port: number;
  state: string;
  physical_state: string;
  netdev: string | null;

  // Summary fields first
  qp_count?: number;
  qps_in_error?: number;
  qp_summary?: RdmaQpStateSummary;
  process_summary?: RdmaProcessSummary[];

  // Key counters
  out_rdma_writes: number;
  in_rdma_writes: number;
  out_rdma_reads: number;
  in_rdma_reads: number;
  retrans_segs: number;
  rx_ecn_marked: number;
  cnp_sent: number;
  cnp_handled: number;
  cnp_ignored: number;
  in_opt_errors: number;
  in_proto_errors: number;
  ip4_in_discards: number;
  ip4_out_octets: number;
  ip4_in_octets: number;

  delta_seconds?: number;
  writes_per_sec?: number;
  retrans_per_sec?: number;
  throughput_out_gbps?: number;
  throughput_in_gbps?: number;

  qps?: RdmaQpInfo[];

  // Full counter dump last
  all_counters: RdmaHwCounter[];
}

export interface RdmaHealthWarning {
  device: string;
  severity: "info" | "warning" | "critical";
  message: string;
}

export interface RdmaQpStateSummary {
  total: number;
  by_state: Record<string, number>;
}

export interface RdmaProcessSummary {
  pid: number;
  comm: string;
  qp_count: number;
}

export interface RdmaHealthData {
  devices: RdmaDeviceHealth[];
  device_count: number;
  warnings: RdmaHealthWarning[];
}

// ── dcb_status ───────────────────────────────────────────────────────────────

export interface DcbPfcStatus {
  pfc_cap: number;
  macsec_bypass: boolean;
  delay: number;
  prio_pfc: Record<number, boolean>;
  enabled_priorities: number[];
}

export interface DcbEtsTcConfig {
  tc: number;
  bandwidth_pct: number;
  tsa: string;
}

export interface DcbEtsStatus {
  willing: boolean;
  ets_cap: number;
  cbs: boolean;
  tc_config: DcbEtsTcConfig[];
  prio_to_tc: Record<number, number>;
}

export interface DcbVfInfo {
  vf_id: number;
  mac: string;
  link_state: string;
  trust: boolean;
  spoof_check: boolean;
  vlan?: number;
}

export interface DcbInterfaceStatus {
  interface: string;
  mtu: number;
  operstate: string;
  speed_mbps: number | null;
  rdma_device: string | null;
  pfc: DcbPfcStatus | null;
  ets: DcbEtsStatus | null;
  vfs?: DcbVfInfo[];
}

export interface DcbStatusWarning {
  interface: string;
  severity: "info" | "warning";
  message: string;
}

export interface DcbStatusData {
  interfaces: DcbInterfaceStatus[];
  warnings: DcbStatusWarning[];
}

// ── nic_ethtool_stats ────────────────────────────────────────────────────────

export interface EthtoolCounter {
  name: string;
  value: number;
  delta?: number;
  rate_per_sec?: number;
}

export interface EthtoolQueueStats {
  queue: number;
  direction: "tx" | "rx";
  packets: number;
  bytes: number;
  delta_packets?: number;
  delta_bytes?: number;
}

export interface NicEthtoolStatsData {
  interface: string;
  driver: string | null;
  counter_count: number;

  rx_bytes: number;
  tx_bytes: number;
  rx_unicast: number;
  tx_unicast: number;
  rx_multicast: number;
  rx_broadcast: number;
  rx_dropped: number;
  tx_errors: number;
  rx_crc_errors: number;
  rx_oversize: number;
  tx_timeout: number;

  delta_seconds?: number;
  rx_bytes_per_sec?: number;
  tx_bytes_per_sec?: number;
  rx_rate_gbps?: number;
  tx_rate_gbps?: number;
  rx_pps?: number;
  tx_pps?: number;

  counters: EthtoolCounter[];
  queues?: EthtoolQueueStats[];
  internal_replication?: {
    tx_multicast_nic: number;
    wire_tx_pkts: number;
    replication_factor: number;
  };
  warnings: string[];
}

/* ── Turbostat types ─────────────────────────────────────────────────────── */

/** Per-logical-CPU turbostat row. */
export interface TurbostatCoreEntry {
  core: number;
  cpu: number;
  avg_mhz?: number;
  busy_pct?: number;
  bzy_mhz?: number;
  tsc_mhz?: number;
  ipc?: number;
  irq?: number;
  smi?: number;
  c1_pct?: number;
  c1e_pct?: number;
  c6_pct?: number;
  poll_pct?: number;
  core_tmp_c?: number;
}

/** Package-level summary from turbostat. */
export interface TurbostatPackageSummary {
  avg_mhz?: number;
  busy_pct?: number;
  bzy_mhz?: number;
  tsc_mhz?: number;
  ipc?: number;
  total_smi?: number;
  c1_pct?: number;
  c1e_pct?: number;
  c6_pct?: number;
  pkg_tmp_c?: number;
  pkg_watt?: number;
  ram_watt?: number;
  pkg_pc2_pct?: number;
  pkg_pc6_pct?: number;
  unc_mhz?: number;
}

export interface TurbostatWarning {
  cpu: number;          // -1 for package-level
  severity: "warning" | "critical";
  message: string;
}

export interface TurbostatIpcAnomaly {
  cpu: number;
  metric: string;
  value: number;
  peer_avg: number;
  deviation_pct: number;
  message: string;
}

export interface TurbostatSnapshotData {
  interval_sec: number;
  columns_available: string[];
  package_summary: TurbostatPackageSummary;
  cores: TurbostatCoreEntry[];
  core_count: number;
  total_cores_on_system: number;
  requested_cpus?: number[];
  ht_siblings_added?: number[];
  dpdk_cores_detected?: number[];
  ipc_anomalies?: TurbostatIpcAnomaly[];
  warnings: TurbostatWarning[];
}

// ── nic_vf_stats ─────────────────────────────────────────────────────────────

export interface VfStatsEntry {
  vf_index: number;
  vf_bdf?: string;
  rx_bytes: number;
  tx_bytes: number;
  rx_pkts: number;
  tx_pkts: number;
  rx_dropped: number;
  tx_dropped: number;
  rx_multicast: number;
  delta_seconds?: number;
  rx_bytes_per_sec?: number;
  tx_bytes_per_sec?: number;
}

export interface NicVfStatsData {
  pf_interface: string;
  vf_count: number;
  vfs: VfStatsEntry[];
  warnings: string[];
}

// ── diagnose_fps_drop ────────────────────────────────────────────────────────

export interface DiagnosisSuspect {
  rank: number;
  category: string;
  evidence: string;
  affected_cores?: number[];
  affected_nic?: string;
  severity: "info" | "warning" | "critical";
}

export interface DiagnoseFpsDropData {
  fps_actual?: number;
  fps_target?: number;
  deficit_pct?: number;
  log_path: string;
  suspects: DiagnosisSuspect[];
  raw_data: {
    turbostat?: Record<string, unknown>;
    nic_stats?: Record<string, unknown>;
    rdma_stats?: Record<string, unknown>;
    nstat?: Record<string, number>;
    log_errors: string[];
    tx_build_timeouts: number;
    vf_bdfs: string[];
    lcores: number[];
  };
  warnings: string[];
}

// ── runq_latency ─────────────────────────────────────────────────────────────

export interface RunqLatencyBucket {
  lo: number;
  hi: number;
  count: number;
}

export interface RunqLatencyHistogram {
  label: string;
  unit: "usecs" | "msecs";
  total_count: number;
  avg_usec: number;
  p50_usec: number;
  p99_usec: number;
  buckets: RunqLatencyBucket[];
}

export interface RunqLatencyData {
  duration_sec: number;
  per_cpu: boolean;
  histograms: RunqLatencyHistogram[];
  warnings: string[];
}

// ── offcpu_time ──────────────────────────────────────────────────────────────

export interface OffcpuStackEntry {
  frames: string[];
  total_usec: number;
}

export interface OffcpuTimeData {
  duration_sec: number;
  pid_filter?: number;
  min_block_usec: number;
  top_stacks: OffcpuStackEntry[];
  total_stacks_captured: number;
  warnings: string[];
}

// ── hardirq_latency ──────────────────────────────────────────────────────────

export interface HardirqEntry {
  irq_name: string;
  count: number;
  total_usec: number;
  avg_usec: number;
}

export interface HardirqLatencyData {
  duration_sec: number;
  mode: "time" | "count";
  irqs: HardirqEntry[];
  warnings: string[];
}

// ── rdma_counters ────────────────────────────────────────────────────────────

export interface RdmaPortCounters {
  device: string;
  port: number;
  counters: Record<string, number>;
  /** Computed rates when delta mode is used (per-second values). */
  rates?: Record<string, number>;
}

export interface RdmaCountersData {
  devices: RdmaPortCounters[];
  delta_seconds?: number;
  warnings: string[];
}

// ── network_stats ────────────────────────────────────────────────────────────

export interface NetworkStatsData {
  /** All nstat counters as key-value pairs. */
  counters: Record<string, number>;
  /** Computed deltas when delta mode used. */
  deltas?: Record<string, number>;
  delta_seconds?: number;
  /** Key metrics highlighted for quick scanning. */
  highlights: NetworkStatsHighlight[];
  warnings: string[];
}

export interface NetworkStatsHighlight {
  counter: string;
  value: number;
  severity: "info" | "warning" | "critical";
  message: string;
}

// ── devlink_health ───────────────────────────────────────────────────────────

export interface DevlinkReporter {
  pci_slot: string;
  reporter: string;
  state: string;
  error_count: number;
  recover_count: number;
  grace_period_ms?: number;
  auto_recover?: boolean;
  auto_dump?: boolean;
  last_dump_time?: string;
}

export interface DevlinkHealthData {
  reporters: DevlinkReporter[];
  devices_checked: number;
  warnings: string[];
}

// ── cpudist ──────────────────────────────────────────────────────────────────

export interface CpudistHistogram {
  label: string;
  unit: "usecs" | "msecs";
  total_count: number;
  avg_usec: number;
  p50_usec: number;
  p99_usec: number;
  buckets: RunqLatencyBucket[];
}

export interface CpudistData {
  duration_sec: number;
  mode: "on-cpu" | "off-cpu";
  per_process: boolean;
  histograms: CpudistHistogram[];
  warnings: string[];
}

// ── critical_sections ────────────────────────────────────────────────────────

export interface CriticalSectionEntry {
  latency_usec: number;
  caller: string;
  stack?: string[];
}

export interface CriticalSectionsData {
  duration_sec: number;
  threshold_usec: number;
  entries: CriticalSectionEntry[];
  total_violations: number;
  warnings: string[];
}

// ── wakeup_sources ───────────────────────────────────────────────────────────

export interface WakeupStackEntry {
  frames: string[];
  total_usec: number;
}

export interface WakeupSourcesData {
  duration_sec: number;
  pid_filter?: number;
  top_stacks: WakeupStackEntry[];
  total_stacks_captured: number;
  warnings: string[];
}

// ── llc_stat ─────────────────────────────────────────────────────────────────

export interface LlcStatEntry {
  pid: number;
  comm: string;
  cpu: number;
  references: number;
  misses: number;
  hit_pct: number;
}

export interface LlcStatData {
  duration_sec: number;
  sample_period: number;
  entries: LlcStatEntry[];
  summary: {
    total_references: number;
    total_misses: number;
    overall_hit_pct: number;
  };
  warnings: string[];
}

// ── socket_diag ──────────────────────────────────────────────────────────────

export interface SocketEntry {
  protocol: string;
  state: string;
  local_addr: string;
  local_port: number;
  remote_addr: string;
  remote_port: number;
  recv_q: number;
  send_q: number;
  pid?: number;
  comm?: string;
  /** TCP-specific info: rto, rtt, cwnd, etc. */
  tcp_info?: Record<string, string>;
}

export interface SocketDiagData {
  sockets: SocketEntry[];
  total_count: number;
  state_summary: Record<string, number>;
  warnings: string[];
}

// ── funclatency ──────────────────────────────────────────────────────────────

export interface FunclatencyHistogram {
  label: string;
  unit: "usecs" | "msecs" | "nsecs";
  total_count: number;
  avg_value: number;
  p50: number;
  p99: number;
  buckets: RunqLatencyBucket[];
}

export interface FunclatencyData {
  function_pattern: string;
  duration_sec: number;
  histograms: FunclatencyHistogram[];
  warnings: string[];
}

// ── pcm_latency (CLI-based) ─────────────────────────────────────────────────

export interface PcmLatencySocketData {
  socket: number;
  ddr_latency_ns: number;
  pmm_latency_ns?: number;
}

export interface PcmLatencyData {
  sockets: PcmLatencySocketData[];
  raw_output: string;
  note?: string;
}

// ── pcm_accel (CLI-based — IAA/DSA/QAT accelerators) ────────────────────────

export interface PcmAccelEntry {
  accelerator: string;
  socket: number;
  inbound_bw_bps: number;
  outbound_bw_bps: number;
  share_wq_req_nb: number;
  dedicate_wq_req_nb: number;
}

export interface PcmAccelData {
  target: "iaa" | "dsa" | "qat";
  devices: PcmAccelEntry[];
}

// ── pcm_iio (CLI-based — per-PCIe-stack I/O bandwidth) ──────────────────────

export interface PcmIioEntry {
  socket: string;
  stack_name: string;
  part: string;
  ib_write_bps: number;
  ib_read_bps: number;
  ob_read_bps: number;
  ob_write_bps: number;
  iotlb_lookup?: number;
  iotlb_miss?: number;
}

export interface PcmIioData {
  entries: PcmIioEntry[];
  non_zero_only: boolean;
}

// ── pcm_memory (CLI-based — per-channel memory bandwidth) ────────────────────

export interface PcmMemoryChannelData {
  channel: number;
  read_mbps: number;
  write_mbps: number;
  pmm_read_mbps: number;
  pmm_write_mbps: number;
}

export interface PcmMemorySocketData {
  socket: number;
  channels: PcmMemoryChannelData[];
  total_read_mbps: number;
  total_write_mbps: number;
  total_pmm_read_mbps: number;
  total_pmm_write_mbps: number;
  total_memory_mbps: number;
}

export interface PcmMemoryPerChannelData {
  sockets: PcmMemorySocketData[];
  system_dram_read_mbps: number;
  system_dram_write_mbps: number;
  system_total_memory_mbps: number;
}

// ── pcm_bw_histogram (CLI-based — memory bandwidth utilization histogram) ────

export interface PcmBwHistogramBucket {
  range_gbps: string;
  count: number;
  time_pct: number;
}

export interface PcmBwHistogramSocketData {
  socket: number;
  dram_histogram: PcmBwHistogramBucket[];
}

export interface PcmBwHistogramData {
  duration_sec: number;
  sockets: PcmBwHistogramSocketData[];
}

// ── pcm_tsx (CLI-based — Transactional Synchronization Extensions) ───────────

export interface PcmTsxCoreEntry {
  core: number;
  tsx_cycles_pct: number;
  tsx_aborts: number;
  tsx_capacity_aborts: number;
  tsx_conflict_aborts: number;
}

export interface PcmTsxData {
  supported: boolean;
  cores?: PcmTsxCoreEntry[];
  raw_output: string;
}

// ═══════════════════════════════════════════════════════════════════════════
// BCC/eBPF tools (new batch)
// ═══════════════════════════════════════════════════════════════════════════

// ── cpuunclaimed ─────────────────────────────────────────────────────────

export interface CpuUnclaimedSample {
  timestamp: string;
  idle_pct: number;
  /** Per-CPU run-queue lengths at sample time (non-zero only). */
  busy_cpus: number;
  total_cpus: number;
}

export interface CpuUnclaimedData {
  duration_sec: number;
  samples: CpuUnclaimedSample[];
  avg_idle_pct: number;
  warnings: string[];
  raw_output: string;
}

// ── runqlen ──────────────────────────────────────────────────────────────

export interface RunqLenHistogram {
  label: string;
  unit: "runqlen";
  total_samples: number;
  avg_len: number;
  max_len: number;
  buckets: RunqLatencyBucket[];
}

export interface RunqLenData {
  duration_sec: number;
  per_cpu: boolean;
  histograms: RunqLenHistogram[];
  warnings: string[];
}

// ── runqslower ───────────────────────────────────────────────────────────

export interface RunqSlowerEvent {
  task: string;
  pid: number;
  delta_usec: number;
  prev_task?: string;
  prev_pid?: number;
}

export interface RunqSlowerData {
  duration_sec: number;
  threshold_usec: number;
  events: RunqSlowerEvent[];
  total_events: number;
  warnings: string[];
}

// ── offwaketime ──────────────────────────────────────────────────────────

export interface OffWakeEntry {
  /** Off-CPU (blocked) stack frames. */
  blocked_frames: string[];
  /** Waker stack frames (who woke the blocked thread). */
  waker_frames: string[];
  total_usec: number;
}

export interface OffWakeTimeData {
  duration_sec: number;
  pid_filter?: number;
  min_block_usec: number;
  top_entries: OffWakeEntry[];
  total_entries: number;
  warnings: string[];
}

// ── profile (flame graph) ────────────────────────────────────────────────

export interface ProfileFlamegraphData {
  duration_sec: number;
  frequency_hz: number;
  pid_filter?: number;
  /** Folded stack lines: "frame1;frame2;frame3 count" — paste into flamegraph.pl */
  folded_stacks: string;
  total_samples: number;
  top_stacks: Array<{ frames: string[]; count: number }>;
  warnings: string[];
}

// ── klockstat ────────────────────────────────────────────────────────────

export interface KlockstatEntry {
  caller: string;
  avg_ns: number;
  count: number;
  max_ns: number;
  total_ns: number;
}

export interface KlockstatData {
  duration_sec: number;
  top_n: number;
  spin_contention: KlockstatEntry[];
  hold_times: KlockstatEntry[];
  warnings: string[];
}

// ── softirqs (enhanced — latency histogram per softirq type) ─────────────

export interface SoftirqHistogram {
  softirq: string;
  unit: "usecs" | "msecs";
  total_count: number;
  avg_usec: number;
  p50_usec: number;
  p99_usec: number;
  buckets: RunqLatencyBucket[];
}

export interface SoftirqSlowerData {
  duration_sec: number;
  histograms: SoftirqHistogram[];
  warnings: string[];
}

// ── cachestat ────────────────────────────────────────────────────────────

export interface CachestatSample {
  hits: number;
  misses: number;
  dirties: number;
  hit_ratio_pct: number;
  buffers_mb: number;
  cached_mb: number;
}

export interface CachestatData {
  duration_sec: number;
  interval_sec: number;
  samples: CachestatSample[];
  avg_hit_ratio_pct: number;
  warnings: string[];
}

// ── biolatency ───────────────────────────────────────────────────────────

export interface BiolatencyHistogram {
  label: string;
  unit: "usecs" | "msecs";
  total_count: number;
  avg_usec: number;
  p50_usec: number;
  p99_usec: number;
  buckets: RunqLatencyBucket[];
}

export interface BiolatencyData {
  duration_sec: number;
  per_disk: boolean;
  histograms: BiolatencyHistogram[];
  warnings: string[];
}

// ── memleak ──────────────────────────────────────────────────────────────

export interface MemleakAllocation {
  bytes: number;
  allocations: number;
  stack: string[];
}

export interface MemleakData {
  duration_sec: number;
  pid_filter?: number;
  top_n: number;
  outstanding: MemleakAllocation[];
  total_bytes: number;
  total_allocations: number;
  warnings: string[];
}

// ── numasched ────────────────────────────────────────────────────────────

export interface NumaSchedMigration {
  task: string;
  pid: number;
  src_node: number;
  dst_node: number;
  count: number;
}

export interface NumaSchedData {
  duration_sec: number;
  migrations: NumaSchedMigration[];
  total_cross_numa: number;
  warnings: string[];
}

// ── wqlat (kernel workqueue latency) ─────────────────────────────────────

export interface WqlatEntry {
  workqueue: string;
  count: number;
  avg_usec: number;
  max_usec: number;
  total_usec: number;
}

export interface WqlatData {
  duration_sec: number;
  entries: WqlatEntry[];
  warnings: string[];
}

// ═══════════════════════════════════════════════════════════════════════════
// Intel RDT (Resource Director Technology) tools
// ═══════════════════════════════════════════════════════════════════════════

// ── rdt_cache_monitoring (CMT — Cache Monitoring Technology) ──────────────

export interface RdtCacheMonEntry {
  core: number;
  ipc: number;
  llc_misses_k: number;
  llc_occupancy_kb: number;
  mbl_mbps: number;
  mbr_mbps: number;
}

export interface RdtCacheMonData {
  duration_sec: number;
  cores_monitored: string;
  entries: RdtCacheMonEntry[];
  warnings: string[];
}

// ── rdt_memory_bandwidth (MBM — Memory Bandwidth Monitoring) ─────────────

export interface RdtMemBwEntry {
  core: number;
  mbl_mbps: number;
  mbr_mbps: number;
  mbt_mbps: number;
}

export interface RdtMemBwData {
  duration_sec: number;
  cores_monitored: string;
  entries: RdtMemBwEntry[];
  warnings: string[];
}

// ── rdt_cache_allocation (CAT — Cache Allocation Technology) ─────────────

export interface RdtL3CosDefinition {
  cos_id: number;
  mask_hex: string;
  /** Number of cache ways assigned (popcount of mask). */
  ways: number;
}

export interface RdtL3CatSocket {
  socket: number;
  cos_definitions: RdtL3CosDefinition[];
}

export interface RdtCoreAssignment {
  core: number;
  l2_id: number;
  l3_id: number;
  cos_id: number;
}

export interface RdtCacheAllocationData {
  sockets: RdtL3CatSocket[];
  core_assignments: RdtCoreAssignment[];
  total_l3_ways: number;
  warnings: string[];
}

// ── rdt_mba_config (MBA — Memory Bandwidth Allocation) ───────────────────

export interface RdtMbaCosDefinition {
  cos_id: number;
  /** Percentage of bandwidth available (e.g. 100 = no throttling). */
  bandwidth_pct: number;
}

export interface RdtMbaSocket {
  socket: number;
  cos_definitions: RdtMbaCosDefinition[];
}

export interface RdtMbaConfigData {
  sockets: RdtMbaSocket[];
  core_assignments: RdtCoreAssignment[];
  warnings: string[];
}

// ── use_method_checklist (Brendan Gregg USE Method) ──────────────────────

export interface UseMethodCheck {
  metric: string;
  value: number;
  unit: string;
  status: "ok" | "warning" | "critical";
  detail?: string;
}

export interface UseMethodResource {
  resource: string;
  utilization: UseMethodCheck[];
  saturation: UseMethodCheck[];
  errors: UseMethodCheck[];
}

export interface UseMethodFinding {
  severity: "critical" | "warning" | "info";
  resource: string;
  category: "utilization" | "saturation" | "errors";
  message: string;
}

export interface UseMethodChecklistData {
  resources: UseMethodResource[];
  findings: UseMethodFinding[];
  summary: { critical: number; warning: number; info: number; healthy: number };
}

// ── ipc_analysis (Instructions Per Cycle) ────────────────────────────────

export interface IpcAnalysisData {
  cores: string;
  duration_sec: number;
  instructions: number;
  cycles: number;
  ipc: number;
  cache_references: number;
  cache_misses: number;
  cache_miss_pct: number;
  branch_instructions: number;
  branch_misses: number;
  branch_miss_pct: number;
  classification: "memory-stalled" | "instruction-bound" | "balanced";
  interpretation: string;
  warnings: string[];
}

// ── stall_cycle_breakdown ────────────────────────────────────────────────

export interface StallCycleBreakdownData {
  cores: string;
  duration_sec: number;
  total_cycles: number;
  instructions: number;
  ipc: number;
  stalls_total: number;
  stalls_total_pct: number;
  stalls_l1d_miss: number;
  stalls_l2_miss: number;
  stalls_l3_miss: number;
  cycles_mem_any: number;
  busy_cycles: number;
  busy_pct: number;
  memory_stall_pct: number;
  interpretation: string;
  warnings: string[];
}

// ── tma_analysis (Top-Down Microarchitecture Analysis) ───────────────────

export interface TmaLevel1 {
  retiring_pct: number;
  bad_speculation_pct: number;
  frontend_bound_pct: number;
  backend_bound_pct: number;
}

export interface TmaLevel2 {
  fetch_latency_pct: number;
  fetch_bandwidth_pct: number;
  branch_mispredicts_pct: number;
  machine_clears_pct: number;
  light_operations_pct: number;
  heavy_operations_pct: number;
  memory_bound_pct: number;
  core_bound_pct: number;
}

export interface TmaAnalysisData {
  cores: string;
  duration_sec: number;
  level: number;
  level1: TmaLevel1;
  level2?: TmaLevel2;
  level3?: Record<string, number>;
  dominant_bottleneck: string;
  interpretation: string;
  warnings: string[];
}

// ── snapshot_diff (A/B correlated snapshot comparison) ────────────────────

export interface SnapshotDiffEntry {
  metric: string;
  before: number;
  after: number;
  delta: number;
  delta_pct: number | null;
}

export interface SnapshotDiffCategory {
  category: string;
  entries: SnapshotDiffEntry[];
}

export interface SnapshotDiffData {
  interval_sec: number;
  label_before: string;
  label_after: string;
  collectors_compared: string[];
  diffs: SnapshotDiffCategory[];
  summary: string[];
  warnings: string[];
}

// ── off_cpu_analysis (scheduling latency per task) ───────────────────────

export interface OffCpuTaskSummary {
  comm: string;
  pid: number;
  runtime_ms: number;
  switches: number;
  avg_delay_ms: number;
  max_delay_ms: number;
}

export interface OffCpuAnalysisData {
  cores: string;
  duration_sec: number;
  tasks: OffCpuTaskSummary[];
  total_tasks: number;
  warnings: string[];
}

// ── dpdk_telemetry_deep (extended xstats) ────────────────────────────────

export interface DpdkXstatEntry {
  name: string;
  value: number;
}

export interface DpdkXstatsPort {
  port_id: number;
  port_name: string;
  xstats: DpdkXstatEntry[];
  error_xstats: DpdkXstatEntry[];
}

export interface DpdkTelemetryDeepData {
  instance_socket: string;
  ports: DpdkXstatsPort[];
  ring_info: { name: string; count: number; size: number }[];
  warnings: string[];
}

// ── working_set_size (WSS estimation via clear_refs + smaps) ─────────────

export interface WssRegion {
  name: string;
  wss_kb: number;
  rss_kb: number;
}

export interface WorkingSetSizeData {
  pid: number;
  comm: string;
  window_sec: number;
  wss_pages: number;
  wss_kb: number;
  wss_mb: number;
  rss_kb: number;
  rss_mb: number;
  wss_to_rss_ratio: number;
  page_size_kb: number;
  regions: WssRegion[];
  warnings: string[];
}

