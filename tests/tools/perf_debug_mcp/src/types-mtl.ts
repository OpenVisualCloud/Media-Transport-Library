/**
 * Type definitions for all MTL-related MCP tools.
 *
 * These tools collect metrics from Media Transport Library (MTL) instances
 * running on local or remote Linux hosts.  Data sources include:
 *   - MtlManager Unix socket + log file
 *   - DPDK telemetry sockets (SOCK_SEQPACKET)
 *   - /proc per-process/thread scanning
 *   - /dev/shm (or configurable log directory) log files with stat dumps
 *   - InfluxDB v2 HTTP API (time-series history)
 *   - USDT probes via bpftrace (preferred live data source)
 *   - ethtool / sysfs NIC counters
 *   - Hugepage stats from /proc/meminfo
 */

// ─── mtl_manager_status ─────────────────────────────────────────────────────

export interface MtlManagerClient {
  pid: number;
  lcores: number[];
}

export interface MtlManagerStatusData {
  running: boolean;
  pid: number | null;
  version: string | null;
  socket_path: string;
  log_path: string | null;
  clients: MtlManagerClient[];
  client_count: number;
}

// ─── mtl_dpdk_telemetry ─────────────────────────────────────────────────────

export interface DpdkEthdevStats {
  port_id: number;
  name: string;
  ipackets: number;
  opackets: number;
  ibytes: number;
  obytes: number;
  imissed: number;
  ierrors: number;
  oerrors: number;
}

export interface DpdkEthdevInfo {
  port_id: number;
  name: string;
  driver: string;
  mtu: number;
  mac: string;
  numa_node: number;
  rx_queues: number;
  tx_queues: number;
  link_speed: number;
  link_status: string;
  link_duplex: string;
}

export interface DpdkMempoolInfo {
  name: string;
  size: number;
  avail_count: number;
  in_use_count: number;
}

export interface DpdkHeapInfo {
  heap_id: number;
  total_bytes: number;
  alloc_bytes: number;
  free_bytes: number;
  alloc_count: number;
}

export interface DpdkTelemetryInstanceData {
  socket_path: string;
  file_prefix: string;
  eal_params: string[];
  lcores: number[];
  ethdev_stats: DpdkEthdevStats[];
  ethdev_info: DpdkEthdevInfo[];
  mempool_info: DpdkMempoolInfo[];
  heap_info: DpdkHeapInfo[];
}

export interface MtlDpdkTelemetryData {
  instances: DpdkTelemetryInstanceData[];
  instance_count: number;
}

// ─── mtl_instance_processes ─────────────────────────────────────────────────

export interface MtlThreadInfo {
  tid: number;
  name: string;
  cpu_affinity: number[];
  utime_ticks: number;
  stime_ticks: number;
  voluntary_ctx_switches: number;
  nonvoluntary_ctx_switches: number;
  state: string;
}

export interface MtlProcessInfo {
  pid: number;
  comm: string;
  cmdline: string;
  cpu_affinity: number[];
  threads: MtlThreadInfo[];
  thread_count: number;
  rss_kb: number;
  vsize_kb: number;
  start_time: string | null;
}

export interface MtlInstanceProcessesData {
  processes: MtlProcessInfo[];
  process_count: number;
  manager: {
    pid: number | null;
    running: boolean;
  };
}

// ─── mtl_session_stats ──────────────────────────────────────────────────────

export interface MtlDevStats {
  port_index: number;
  tx_rate_mbps: number;
  rx_rate_mbps: number;
  tx_pkts: number;
  rx_pkts: number;
  /** rx_hw_dropped_packets from DEV Status line (only present when non-zero) */
  rx_hw_dropped?: number;
  /** rx_err_packets from DEV Status line */
  rx_err?: number;
  /** rx_nombuf_packets from DEV Status line */
  rx_nombuf?: number;
  /** tx_err_packets from DEV Status line */
  tx_err?: number;
}

export interface MtlSchTaskletStats {
  tasklet_index: number;
  name: string;
  avg_us: number;
  max_us: number;
  min_us: number;
}

export interface MtlSchStats {
  sch_index: number;
  name: string;
  tasklets: number;
  lcore: number;
  thread_pid: number;
  avg_loop_ns: number;
  /** Scheduler-level timing: tasklet time avg (microseconds) */
  time_avg_us?: number;
  /** Scheduler-level timing: tasklet time max (microseconds) */
  time_max_us?: number;
  /** Scheduler-level timing: tasklet time min (microseconds) */
  time_min_us?: number;
  /** Per-tasklet timing breakdowns */
  tasklet_details?: MtlSchTaskletStats[];
}

export interface MtlCniStats {
  port_index: number;
  eth_rx_rate_mbps: number;
  eth_rx_cnt: number;
}

export interface MtlPtpStats {
  port_index: number;
  time_ns: number;
  time_human: string;
}

export interface MtlVideoSessionStats {
  sch_index: number;
  session_index: number;
  name: string;
  fps: number;
  frames: number;
  pkts: number;
  redundant_pkts?: number;
  throughput_mbps: number;
  cpu_busy: number;
  burst_max: number;
  burst_avg: number;
  /** Incomplete frames in this stat period (RX only, conditional) */
  incomplete_frames?: number;
  /** Packets dropped due to idx error (RX only) */
  idx_error?: number;
  /** Packets dropped due to offset error (RX only) */
  offset_error?: number;
  /** Packets with idx out of bitmap (RX only) */
  idx_out_of_bitmap?: number;
  /** Estimated missed packets for incomplete frames (RX only) */
  missed_pkts?: number;
  /** Out-of-order packets (RX only, conditional) */
  out_of_order_pkts?: number;
  /** Tasklet time average in microseconds (both TX and RX, conditional) */
  tasklet_time_avg_us?: number;
  /** Tasklet time max in microseconds */
  tasklet_time_max_us?: number;
  /** Tasklet time min in microseconds */
  tasklet_time_min_us?: number;
}

export interface MtlStatDump {
  timestamp: string;
  dev_stats: MtlDevStats[];
  sch_stats: MtlSchStats[];
  cni_stats: MtlCniStats[];
  ptp_stats: MtlPtpStats[];
  video_sessions: MtlVideoSessionStats[];
}

export interface MtlSessionStatsTrend {
  timestamp: string;
  value: number;
}

export interface MtlSessionStatsWarning {
  session: string;
  severity: "info" | "warning" | "error";
  message: string;
}

/** Aggregated statistics for a single metric (mean/stddev/min/max). */
export interface MtlAggregateStats {
  mean: number;
  stddev: number;
  min: number;
  max: number;
}

/** Per-session aggregated metrics across multiple steady-state dumps. */
export interface MtlSessionAggregate {
  session: string;
  sample_count: number;
  fps: MtlAggregateStats;
  throughput_mbps?: MtlAggregateStats;
  cpu_busy?: MtlAggregateStats;
  burst_max?: MtlAggregateStats;
  burst_avg?: MtlAggregateStats;
}

export interface MtlSessionStatsData {
  log_file: string;
  latest_dump: MtlStatDump | null;
  dumps_found: number;
  /** Number of dumps actually used for statistics (last N, excluding startup transients). */
  steady_state_dumps: number;
  /** Per-session aggregated statistics across steady-state dumps. */
  session_aggregates?: MtlSessionAggregate[];
  fps_trend?: MtlSessionStatsTrend[];
  fps_mean?: number;
  fps_min?: number;
  fps_max?: number;
  fps_stddev?: number;
  throughput_trend?: MtlSessionStatsTrend[];
  tx_build_timeouts?: number;
  warnings?: MtlSessionStatsWarning[];
  /** Data source used: "usdt" | "log" | "influxdb". Present when using USDT session stats. */
  data_source?: UsdtSessionStatsMode;
  /** PID of the traced process (USDT mode). */
  usdt_pid?: number;
  /** Reason the preferred source was unavailable and we fell back. */
  fallback_reason?: string;
}

// ─── mtl_app_latency ────────────────────────────────────────────────────────

export interface LatencyBucket {
  min: number;
  max: number;
  avg: number;
  n: number;
}

export interface MtlAppLatencySample {
  role: string;
  time_s: number;
  rx_frames: number;
  rx_incomplete: number;
  rx_drops: number;
  bridge_frames: number;
  bridge_copies: number;
  fabrics_transfers: number;
  target_events?: number;
  consumer_frames?: number;
  latency: Record<string, LatencyBucket>;
}

export interface MtlAppLatencyData {
  log_file: string;
  latest_sample: MtlAppLatencySample | null;
  samples_found: number;
}

// ─── mtl_live_stats ─────────────────────────────────────────────────────────

export interface MtlLiveStatsData {
  file_path: string;
  stats: Record<string, number | string>;
  file_exists: boolean;
}

// ─── mtl_nic_pf_stats ───────────────────────────────────────────────────────

export interface MtlNicPfStatsData {
  interface: string;
  driver: string;
  link_speed_mbps: number;
  link_detected: boolean;
  stats: Record<string, number>;
  stat_count: number;
}

// ─── mtl_hugepage_usage ─────────────────────────────────────────────────────

export interface HugepageSizeInfo {
  page_size_kb: number;
  total: number;
  free: number;
  reserved: number;
  surplus: number;
  in_use: number;
  total_bytes: number;
}

export interface MtlHugepageUsageData {
  sizes: HugepageSizeInfo[];
  dpdk_hugepage_files: number;
  dpdk_hugepage_dir: string;
}

// ─── mtl_lcore_shm ──────────────────────────────────────────────────────────

export interface MtlLcoreShmEntry {
  lcore_id: number;
  hostname: string;
  user: string;
  comm: string;
  pid: number;
  active: boolean;
}

export interface MtlLcoreShmData {
  shm_exists: boolean;
  shm_key: string;
  used_count: number;
  entries: MtlLcoreShmEntry[];
  nattch: number;
  stale_warning: string | null;
}

// ─── mtl_influxdb_query ─────────────────────────────────────────────────────

export interface InfluxDbSeriesPoint {
  time: string;
  field: string;
  value: number | string;
  tags: Record<string, string>;
}

export interface InfluxDbFieldAggregate {
  field: string;
  tags: Record<string, string>;
  n: number;
  mean: number;
  stddev: number;
  min: number;
  max: number;
}

export interface MtlInfluxdbQueryData {
  bucket: string;
  measurement: string;
  range: string;
  series: InfluxDbSeriesPoint[];
  row_count: number;
  truncated: boolean;
  aggregates?: InfluxDbFieldAggregate[];
}

// ─── mtl_scheduler_map ──────────────────────────────────────────────────────

export interface MtlSchedulerSessionInfo {
  session_index: number;
  name: string;
  fps: number;
  throughput_mbps: number;
}

export interface MtlSchedulerInfo {
  sch_index: number;
  name: string;
  lcore: number;
  thread_pid: number;
  tasklet_count: number;
  avg_loop_ns: number;
  sessions: MtlSchedulerSessionInfo[];
  session_count: number;
}

export interface MtlSchedulerMapInstance {
  pid: number;
  comm: string;
  socket_path: string | null;
  log_path: string | null;
  data_source: "stat_dump" | "thread_scan";
  schedulers: MtlSchedulerInfo[];
  scheduler_count: number;
  imbalance_warning: string | null;
}

export interface MtlSchedulerMapData {
  instances: MtlSchedulerMapInstance[];
  instance_count: number;
}

// ── pipeline_health ──────────────────────────────────────────────────────────

export interface PipelineProcessInfo {
  pid: number;
  comm: string;
  scheduler_count: number;
  lcores: number[];
  vf_bdfs?: string[];
  pf_netdev?: string;
}

export interface PipelineLogHealth {
  log_path: string;
  total_lines: number;
  real_errors: number;
  stat_dump_info_lines: number;
  last_error?: string;
  last_fps?: number;
  last_throughput_mbps?: number;
  app_fps?: number;
}

export interface PipelineShmStats {
  path: string;
  age_sec: number;
  data: Record<string, number | string>;
}

export interface PipelineHttpEndpoint {
  label: string;
  url: string;
  reachable: boolean;
  response_data?: Record<string, number | string>;
}

export interface PipelineThumbnail {
  path: string;
  age_sec: number;
  size_bytes: number;
  stale: boolean;
}

export interface PipelineHealthWarning {
  scope: string;
  severity: "info" | "warning" | "critical";
  message: string;
}

export interface PipelineHealthData {
  processes: PipelineProcessInfo[];
  process_count: number;
  logs: PipelineLogHealth[];
  shm_stats: PipelineShmStats[];
  http_endpoints: PipelineHttpEndpoint[];
  thumbnails: PipelineThumbnail[];
  warnings: PipelineHealthWarning[];
}

// ── log_search ───────────────────────────────────────────────────────────────

export interface LogSearchMatch {
  log_path: string;
  line_number: number;
  timestamp?: string;
  raw_line: string;
  classification: "error" | "warning" | "info" | "stat_dump";
  dedup_count?: number;
}

export interface LogSearchFileResult {
  log_path: string;
  lines_scanned: number;
  matches_found: number;
  stat_dump_lines_filtered: number;
}

export interface LogSearchData {
  matches: LogSearchMatch[];
  match_count: number;
  truncated: boolean;
  files: LogSearchFileResult[];
  files_scanned: number;
}

// ─── USDT probe listing ─────────────────────────────────────────────────────

export interface UsdtProbeEntry {
  provider: string;
  probe: string;
  full_name: string;
}

export interface UsdtProbesByProvider {
  provider: string;
  probes: string[];
  count: number;
}

export interface UsdtProbeListData {
  libmtl_path: string;
  total_probes: number;
  providers: UsdtProbesByProvider[];
  /** If a PID was specified, probes resolvable for that process */
  pid?: number;
}

// ─── USDT session stats (from sys:log_msg stat dumps) ────────────────────────

export type UsdtSessionStatsMode = "usdt" | "log" | "influxdb";

export interface UsdtSessionStatsData {
  /** Which data source was actually used */
  data_source: UsdtSessionStatsMode;
  /** Set when USDT was tried but fell back */
  fallback_reason?: string;
  /** PID of the traced process (USDT mode) */
  pid?: number;
  /** Full session data (same shape as log-based parseMtlStatBlock) */
  sessions: MtlVideoSessionStats[];
  device?: MtlDevStats;
  raw_stat_block?: string;
}

// ─── USDT frame trace (st20/st20p/st22/st22p frame lifecycle) ────────────────

export interface UsdtFrameEvent {
  timestamp_ns: number;
  provider: string;
  probe: string;
  /** MTL interface index */
  m_idx: number;
  /** Session index */
  s_idx: number;
  /** Frame index */
  f_idx: number;
  /** Additional probe-specific fields */
  extra?: Record<string, number>;
}

export interface UsdtFrameTraceStream {
  session_idx: number;
  direction: "tx" | "rx";
  protocol: "st20" | "st20p" | "st22" | "st22p";
  events: UsdtFrameEvent[];
  event_count: number;
  computed_fps?: number;
  /** For RX: count of incomplete/no_framebuffer events */
  incomplete_count?: number;
  no_framebuffer_count?: number;
  /** For TX: count of drop events */
  drop_count?: number;
  /** Inter-frame interval stats (ns) */
  ifi_avg_ns?: number;
  ifi_min_ns?: number;
  ifi_max_ns?: number;
  ifi_stddev_ns?: number;
}

export interface UsdtFrameTraceData {
  pid: number;
  process_name: string;
  duration_ms: number;
  streams: UsdtFrameTraceStream[];
  stream_count: number;
  total_events: number;
}

// ─── USDT PTP trace (ptp:ptp_result) ─────────────────────────────────────────

export interface UsdtPtpSample {
  timestamp_ns: number;
  delta_ns: number;
  correct_delta_ns: number;
}

export interface UsdtPtpTraceData {
  pid: number;
  process_name: string;
  duration_ms: number;
  samples: UsdtPtpSample[];
  sample_count: number;
  stats?: {
    delta_avg_ns: number;
    delta_min_ns: number;
    delta_max_ns: number;
    delta_stddev_ns: number;
    correct_delta_avg_ns: number;
  };
}
