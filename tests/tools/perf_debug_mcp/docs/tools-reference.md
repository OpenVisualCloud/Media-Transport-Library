# Tool Reference — perf-debug-mcp

> Complete reference for all 62 MCP tools. Each entry includes description, parameters, return type, and data-source details.

---

## Table of Contents

- [Core CPU / Scheduling (19 tools)](#core-cpu--scheduling)
- [Intel PCM — Hardware Counters (7 tools)](#intel-pcm--hardware-counters)
- [Intel EMON — Uncore PMU (4 tools)](#intel-emon--uncore-pmu)
- [MTL — Media Transport Library (11 tools)](#mtl--media-transport-library)
- [BCC / eBPF Tracing (8 tools)](#bcc--ebpf-tracing)
- [RDMA / DCB / NIC / Network (8 tools)](#rdma--dcb--nic--network)
- [Turbostat & Composite Diagnostics (4 tools)](#turbostat--composite-diagnostics)
- [Log Search (1 tool)](#log-search)

---

## Core CPU / Scheduling

These 19 tools read `/proc`, `/sys`, and the scheduler to provide CPU utilization, affinity, starvation, IRQ, NUMA, frequency, and cgroup analysis. They work unprivileged on any Linux ≥ 4.18 system.

### `capabilities`

Return environment info and enabled features. **Call this first** to discover what modes and tools are available.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `Capabilities` — kernel version, `cpu_count`, `numa_nodes`, eBPF / PCM / EMON / BCC / RDMA availability flags, `available_tools` list |
| **Data source** | `/proc/version`, `/proc/cpuinfo`, sysfs topology, runtime probes |

---

### `core_load_snapshot`

Per-core CPU utilization over a sampling window with user / system / IRQ / softirq / iowait / idle breakdown.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `250` | Sampling window (50 – 10 000 ms) |
| `breakdown` | boolean | `true` | Include per-category percentages |
| `mode` | `"auto" \| "fallback" \| "ebpf"` | `"auto"` | Data-collection mode |

**Returns:** `CoreLoadSnapshotData` — array of `CoreUtilization` per logical CPU.

**Data source:** `/proc/stat` delta (fallback) or eBPF `sched_switch` tracepoint (advanced).

---

### `allowed_on_core`

Enumerate tasks whose CPU affinity mask includes the specified core.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `cpu` | integer | *(required)* | Target CPU ID (≥ 0) |
| `scope` | `"threads" \| "processes"` | `"threads"` | Thread-level or process-level |
| `include_kernel_threads` | boolean | `true` | Include kthreads |
| `limit` | integer | `5000` | Max results (1 – 50 000) |

**Returns:** `AllowedOnCoreData` — `cpu`, `task_count`, `tasks[]` with pid / tid / comm / affinity mask / scheduling policy.

**Data source:** `sched_getaffinity()` syscall for every task in `/proc`.

---

### `running_on_core`

Tasks **actually observed running** on a specific CPU within a sampling window, with runtime attribution (ns) and context switches.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `cpu` | integer | *(required)* | Target CPU ID |
| `window_ms` | number | `250` | Sampling window (50 – 10 000 ms) |
| `mode` | `"auto" \| "fallback" \| "ebpf"` | `"auto"` | Data-collection mode |
| `top_n` | integer | `50` | Max tasks to return (1 – 500) |

**Returns:** `RunningOnCoreData` — `cpu`, `tasks[]` with `runtime_ns`, `pct`, `switches`.

**Data source:** `/proc/<pid>/task/<tid>/stat` delta (fallback) or eBPF `sched_switch` (advanced).

---

### `starvation_report`

High-level starvation analysis for a process or thread. Identifies co-runners, IRQ/softirq interference, throttling, and generates actionable recommendations.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `target` | object | *(required)* | `{ pid?, tid?, comm_regex? }` — identify the target |
| `window_ms` | number | `2000` | Analysis window (100 – 30 000 ms) |
| `solo_cpu_hint` | integer \| null | `null` | Expected dedicated CPU |
| `mode` | `"auto" \| "fallback" \| "ebpf"` | `"auto"` | Data-collection mode |
| `thresholds` | object | *(optional)* | `{ co_runner_count_threshold?, softirq_pct_threshold?, irq_pct_threshold? }` |

**Returns:** `StarvationReportData` — `target_resolution`, `affinity_summary`, `runtime_per_thread`, `top_interferers`, `starvation_flags[]`, `recommended_next_steps[]`.

---

### `irq_distribution`

IRQ distribution per CPU over a window. Identifies hot IRQs and which CPUs handle them.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `1000` | Delta window (100 – 10 000 ms) |
| `cpu_filter` | integer \| null | `null` | Filter to specific CPU |
| `include_per_irq` | boolean | `true` | Per-IRQ breakdown |
| `include_per_iface` | boolean | `true` | Per-interface aggregation |

**Returns:** `IrqDistributionData` — `per_cpu[]`, `top_irqs[]`.

**Data source:** `/proc/interrupts` delta.

---

### `irq_affinity`

Configured and effective CPU affinity for IRQs. Pass an IRQ number or regular expression to match names.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `irq_or_regex` | string | *(required)* | IRQ number or name regular expression |
| `include_effective` | boolean | `true` | Include effective affinity |

**Returns:** `IrqAffinityData` — `irqs[]` with `affinity_list`, `effective_affinity_list`.

**Data source:** `/proc/irq/<n>/smp_affinity_list`, `/proc/irq/<n>/effective_affinity_list`.

---

### `nic_irq_queue_map`

NIC queue → IRQ → CPU mapping for a network interface.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `iface` | string | *(required)* | Network interface name (e.g. `ens1np0`) |

**Returns:** `NicIrqQueueMapData` — `iface`, `queues[]` with `queue`, `direction`, `irq`, `affinity`.

**Data source:** `/proc/interrupts` + `/sys/class/net/<iface>/queues/`.

---

### `softirq_snapshot`

Delta-parse `/proc/softirqs` per CPU. Shows `NET_RX`, `NET_TX`, `TIMER`, `SCHED`, `RCU`, etc.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `1000` | Delta window (100 – 10 000 ms) |

**Returns:** `SoftirqSnapshotData` — `per_cpu[]` with breakdown, `hot_cpus[]`.

---

### `kernel_threads_on_core`

List kernel threads observed on a specific CPU (`ksoftirqd`, `rcu`, `migration`, `watchdog`, etc.).

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `cpu` | integer | *(required)* | Target CPU ID |
| `window_ms` | number | `1000` | Observation window (100 – 10 000 ms) |
| `top_n` | integer | `30` | Max results (1 – 200) |

**Returns:** `KernelThreadsOnCoreData` — `cpu`, `tasks[]`.

---

### `runqueue_snapshot`

Load averages, runnable task count, and per-CPU scheduling stats.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `RunqueueSnapshotData` — `global` (loadavg, runnable, running), `per_cpu[]` |

**Data source:** `/proc/loadavg`, `/proc/schedstat`.

---

### `context_switch_rate`

Global and per-CPU context switch rate over a window.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `1000` | Delta window (100 – 10 000 ms) |

**Returns:** `ContextSwitchRateData` — `global_ctxt_per_s`, `per_cpu[]`.

**Data source:** `/proc/stat` delta.

---

### `isolation_summary`

CPU isolation configuration: `isolcpus`, `nohz_full`, `rcu_nocbs` from kernel cmdline + sysfs. Flags mis-configurations.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `IsolationSummaryData` — `cmdline_flags`, isolated / nohz_full / rcu_nocbs CPU lists, `warnings[]` |

**Data source:** `/proc/cmdline`, sysfs topology.

---

### `cpu_frequency_snapshot`

Per-CPU frequency, governor, min/max values from cpufreq sysfs.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `CpuFrequencySnapshotData` — `cpus[]` with `cur_khz`, `min_khz`, `max_khz`, `governor` |

**Data source:** `/sys/devices/system/cpu/cpu*/cpufreq/`.

---

### `throttling_summary`

Thermal throttling indicators, pstate driver status, RT throttling, frequency governor analysis.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `1000` | Observation window (100 – 10 000 ms, optional) |

**Returns:** `ThrottlingSummaryData` — `thermal_throttle_count`, `notes[]`.

---

### `cgroup_cpu_limits`

Cgroup v2 CPU quotas, weights, throttle counters, and cpuset restrictions for a process.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `target_pid` | integer | *(required)* | Target process PID (≥ 1) |

**Returns:** `CgroupCpuLimitsData` — `cgroup_path`, `cpu_max`, `cpu_weight`, `throttled_usec_delta`, `cpuset_effective`.

**Data source:** cgroupfs (`cpu.max`, `cpu.stat`, `cpuset.cpus.effective`).

---

### `numa_topology`

NUMA node layout: CPUs per node, memory per node, inter-node distance matrix.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `NumaTopologyData` — `nodes[]`, `distances[][]` |

**Data source:** `/sys/devices/system/node/`.

---

### `process_numa_placement`

NUMA placement for a process: allowed CPUs, allowed memory nodes, cross-node warnings.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `target_pid` | integer | *(required)* | Target process PID (≥ 1) |

**Returns:** `ProcessNumaPlacementData` — `pid`, `cpus_allowed_list`, `mems_allowed_list`, `notes[]`.

**Data source:** `/proc/<pid>/status`.

---

### `perf_debug_snapshot`

**All-in-one** performance snapshot: core loads, running tasks, IRQ/softirq, frequency, isolation, optional cgroup limits. **Call this first for general triage.**

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `window_ms` | number | `2000` | Sampling window (100 – 10 000 ms) |
| `focus_cpu` | integer \| null | `null` | Focus on a specific CPU |
| `focus_target` | integer \| null | `null` | Focus on a specific PID |

**Returns:** `PerfDebugSnapshotData` — `core_load`, `running_on_cores`, `irq_distribution`, `softirq_snapshot`, `cpu_frequency`, `isolation`, `cgroup_limits?`, `pcm_core_counters?`, `pcm_power_thermal?`.

---

## Intel PCM — Hardware Counters

These 7 tools query [Intel PCM](https://github.com/intel/pcm) (`pcm-sensor-server`) for hardware performance counters inaccessible from `/proc` and `/sys`. Requires `pcm-sensor-server` running on port 9738.

**Graceful degradation:** If PCM is unavailable, tools return `{ ok: false, error: { code: "PCM_UNAVAILABLE" } }` with a hint. All other tools are unaffected.

### `pcm_core_counters`

Per-core IPC, instructions retired, cache hits/misses, SMI count, active frequency, local/remote memory bandwidth. Supports **multi-sample mode** for A/B testing — when `samples > 1`, collects N snapshots and returns per-core mean/stddev/min/max statistics.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to specific socket |
| `core_filter` | string \| null | `null` | Filter to specific core IDs. Accepts: single (`'4'`), list (`'4,5,6'`), range (`'4-13'`), or mixed (`'4-13,20,30-35'`). Omit for all cores. |
| `samples` | integer | `1` | Number of samples to collect (1 – 20). When >1, returns `multi_sample` with per-metric stats. |
| `interval_s` | integer | `2` | Seconds between samples when `samples > 1` (1 – 30). |

**Returns:** `PcmCoreCountersData` — `interval_us`, `cores[]`, `aggregates[]`, `imbalance_warnings[]`, `multi_sample?` (when `samples > 1`: `{ samples, interval_s, per_core[]: { os_id, core, socket, metrics: Record<metric, { mean, stddev, min, max }> } }`).

**Multi-sample metrics tracked:** `ipc`, `active_frequency_mhz`, `l3_cache_misses`, `l3_cache_hits`, `l2_cache_misses`, `l2_cache_hits`, `l3_cache_occupancy`, `instructions_retired`, `local_memory_bw`, `remote_memory_bw`, `smi_count`.

---

### `pcm_memory_bandwidth`

Per-socket DRAM/PMM/eDRAM bandwidth, MC request counts, NUMA local/remote ratio.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to specific socket |

**Returns:** `PcmMemoryBandwidthData` — `interval_us`, `sockets[]`.

---

### `pcm_cache_analysis`

Per-core L2/L3 cache hit/miss analysis with hit ratios, miss counts, L3 occupancy, and system-wide aggregates. Supports **multi-sample mode** for A/B testing.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to socket |
| `core_filter` | string \| null | `null` | Filter to specific core IDs. Accepts: single (`'4'`), list (`'4,5,6'`), range (`'4-13'`), or mixed (`'4-13,20,30-35'`). Omit for all cores. |
| `samples` | integer | `1` | Number of samples to collect (1 – 20). When >1, returns `multi_sample` with per-metric stats. |
| `interval_s` | integer | `2` | Seconds between samples when `samples > 1` (1 – 30). |

**Returns:** `PcmCacheAnalysisData` — `interval_us`, `cores[]`, system L2/L3 ratios, `multi_sample?` (when `samples > 1`: `{ samples, interval_s, per_core[]: { os_id, core, socket, metrics: Record<metric, { mean, stddev, min, max }> } }`).

**Multi-sample metrics tracked:** `l3_hit_ratio`, `l3_miss_ratio`, `l2_hit_ratio`, `l2_miss_ratio`, `l3_cache_occupancy`, `l3_misses`, `l3_hits`, `l2_misses`, `l2_hits`.

---

### `pcm_power_thermal`

Power, thermal, TMA Level 1+2: per-core thermal headroom, C-state residency, per-socket energy (pkg / PP0 / PP1 / DRAM Joules), TMA bottleneck breakdown.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to socket |
| `include_tma` | boolean | `true` | Include Top-Down Microarchitecture Analysis |

**Returns:** `PcmPowerThermalData` — `interval_us`, `cores[]`, `sockets[]`, `tma?`.

---

### `pcm_qpi_upi_link`

QPI/UPI inter-socket link traffic and utilization. Per-link incoming/outgoing bytes + utilization %. Multi-socket systems only.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |

**Returns:** `PcmQpiUpiLinkData` — `interval_us`, `sockets[].links[]`.

---

### `pcm_pcie_bandwidth`

Aggregate PCIe/MC IO bandwidth: IA, GT (GPU), IO (PCIe/DMA) request bytes per socket.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to socket |

**Returns:** `PcmPcieBandwidthData` — `interval_us`, `sockets[]`, `note`.

---

### `pcm_numa_traffic`

Per-core and per-socket local vs. remote memory bandwidth with locality ratio %. Identifies NUMA-unfriendly cores.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `seconds` | integer | `1` | Measurement interval (1 – 30 s) |
| `socket_filter` | integer \| null | `null` | Filter to socket |
| `top_n` | integer | `0` | Top N worst-locality cores (0 = all) |

**Returns:** `PcmNumaTrafficData` — `interval_us`, `per_core[]`, `per_socket[]`.

---

## Intel EMON — Uncore PMU

These 4 tools use [Intel EMON/SEP](https://www.intel.com/content/www/us/en/developer/articles/tool/emon-user-guide.html) for uncore PMU counters on Sapphire Rapids — IIO (PCIe), CHA (LLC), mesh interconnect, UPI links, and core-level micro-op stall analysis. Requires EMON/SEP binaries and drivers.

### `emon_capabilities`

EMON availability, version, loaded drivers, supported PMU types, and available presets.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `EmonConnectionStatus` + `presets[]` list (E0 – E4) |

---

### `emon_collect`

Run a single EMON preset collection and return structured per-unit PMU counter data.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `preset` | enum | *(required)* | `E0` (PCIe BW/IIO), `E1` (LLC/CHA), `E2` (memory latency/mesh), `E3` (UPI detailed), `E4` (core stall TMA) |
| `window_ms` | number | *(optional)* | Collection window (100 – 30 000 ms) |
| `cpu_filter` | integer[] | *(optional)* | Limit to specific CPUs |

**Returns:** `EmonCollectResult` — `preset`, `window_ms`, `actual_duration_ms`, + structured data (IIO port metrics, CHA metrics, mesh stall, UPI link, or core stall depending on preset).

**Preset details:**

| Preset | Name | Events | Output |
| -------- | ------ | -------- | -------- |
| E0 | PCIe BW per IIO | IIO bandwidth events | Per-port read/write bytes, utilization |
| E1 | LLC per CHA | CHA occupancy/lookups | Per-CHA hit ratio, occupancy |
| E2 | Memory latency | Mesh stall counters | Per-socket stall cycles, bandwidth |
| E3 | UPI detailed | UPI link events | Per-link utilization, flits, retries |
| E4 | Core stall TMA | Core uop breakdown | Per-core frontend/backend/retiring/bad-speculation |

---

### `emon_triage`

**Primary EMON debugging entry point.** Cascading waterfall triage: runs EMON presets E0 → E1 → E2 → E3 → E4 in sequence. Returns diagnosis with severity, evidence, and recommendations.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `focus_cpu` | integer \| null | `null` | Focus analysis on a specific CPU |
| `window_ms` | number | `1000` | Per-preset collection window (100 – 30 000 ms) |
| `quick` | boolean | `false` | Skip slow presets |

**Returns:** `EmonTriageResult` — `causes[]` (with severity, evidence, recommendations), `overall_severity`, `summary`, `presets_used[]`, `triage_duration_ms`.

---

### `emon_pcie_topology`

Map PCIe devices to IIO stacks via sysfs. Returns BDF, driver, NUMA node, net interface, IIO socket:stack:part.

| | |
| --- | --- |
| **Parameters** | *(none)* |
| **Returns** | `EmonPcieTopologyData` — `devices[]`, `iio_map{}` |

---

## MTL — Media Transport Library

These 11 tools monitor [Intel MTL (Media Transport Library)](https://github.com/OpenVisualCloud/Media-Transport-Library) DPDK-based ST 2110 media pipelines. They parse stats from shared memory, log files, DPDK telemetry sockets, and InfluxDB.

### `mtl_manager_status`

MtlManager daemon status: running state, version, socket path, per-client lcore allocations.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host (SSH if remote) |
| `socket_path` | string | `"/var/run/imtl/mtl_manager.sock"` | Manager socket |
| `log_path` | string | *(optional)* | Manager log file |

**Returns:** `MtlManagerStatusData` — `running`, `pid`, `version`, `clients[]`, `client_count`.

---

### `mtl_dpdk_telemetry`

Query DPDK telemetry sockets for ethdev stats, mempool, heap, and EAL info. Discovers all running DPDK instances.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `dpdk_base_dir` | string | `"/var/run/dpdk"` | DPDK runtime dir |
| `file_prefix` | string | `"MT_DPDK"` | File prefix filter |
| `endpoints` | enum[] | `["ethdev_stats","ethdev_info"]` | Telemetry endpoints |

**Returns:** `MtlDpdkTelemetryData` — `instances[]`, `instance_count`.

---

### `mtl_instance_processes`

Scan for MTL/DPDK processes and threads: PID, thread names, CPU affinity, scheduling stats.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `process_regex` | string | `"MtlManager\|mtl_\|dpdk-"` | Process name filter |

**Returns:** `MtlInstanceProcessesData` — `processes[]`, `process_count`, manager status.

---

### `mtl_session_stats`

Parse MTL stat dumps from log files: throughput, scheduler timings, CNI, PTP, per-session video stats (FPS, Mbps, `cpu_busy`). Uses only the **last N dumps** (default 5) to exclude startup transients. Includes per-session aggregation (mean/stddev/min/max) for FPS, throughput, and cpu_busy. FPS trend, TX build timeout detection.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_path` | string | *(required)* | MTL log file path |
| `tail_lines` | integer | `500` | Lines to read from end (50 – 50 000) |
| `last_dumps` | integer | `5` | Use only the last N stat dumps for statistics, discarding earlier startup transients (1 – 100) |
| `alert_threshold_fps` | number | *(optional)* | FPS threshold for warnings |
| `session_filter` | string | *(optional)* | Filter to a specific session by name (e.g. `'video_0'`). Only matching sessions are included in aggregation. |

**Returns:** `MtlSessionStatsData` — `log_file`, `latest_dump`, `dumps_found`, `fps_trend`, per-session `aggregation` (mean/stddev/min/max for FPS, throughput_mbps, cpu_busy), `warnings[]`.

---

### `mtl_app_latency`

Parse application-level JSON latency samples from MTL log files. Extracts per-stage latency buckets.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_path` | string | *(required)* | Log file path |
| `tail_lines` | integer | `100` | Lines to read (10 – 10 000) |
| `role_filter` | `"sender" \| "receiver"` | *(optional)* | Filter by role |

**Returns:** `MtlAppLatencyData` — `log_file`, `latest_sample`, `samples_found`.

---

### `mtl_live_stats`

Read a single JSON stats file from a running MTL app (e.g. `/dev/shm/poc_*_stat.json`).

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `stats_path` | string | *(required)* | Stats file path |

**Returns:** `MtlLiveStatsData` — `file_path`, `stats{}`, `file_exists`.

---

### `mtl_nic_pf_stats`

NIC physical function stats via `ethtool`: driver, link speed, all HW counters. Detect drops, errors, bandwidth saturation.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `interface` | string | *(required)* | NIC interface name |
| `filter` | string | *(optional)* | Regular expression filter for counter names |

**Returns:** `MtlNicPfStatsData` — `interface`, `driver`, `link_speed_mbps`, `stats{}`, `stat_count`.

---

### `mtl_hugepage_usage`

Hugepage allocation and usage: total/free/in-use per page size, plus DPDK hugepage file count.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `hugepage_dir` | string | `"/dev/hugepages"` | Hugepage mount |

**Returns:** `MtlHugepageUsageData` — `sizes[]`, `dpdk_hugepage_files`, `dpdk_hugepage_dir`.

---

### `mtl_lcore_shm`

Read MTL SysV shared memory segment tracking lcore allocation. Shows which lcores are claimed and by which process.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `ftok_path` | string | `"/dev/null"` | ftok path |
| `ftok_proj_id` | integer | `21` | ftok project ID |

**Returns:** `MtlLcoreShmData` — `shm_exists`, `used_count`, `entries[]`, `nattch`, `stale_warning`.

---

### `mtl_influxdb_query`

Query InfluxDB v2 for MTL-related metrics. Executes Flux query and returns parsed time-series data. Optional **aggregation mode** groups series by field+tags and computes mean/stddev/min/max — useful for A/B testing comparisons.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `influxdb_host` | string | `"localhost"` | InfluxDB host |
| `port` | integer | `8086` | InfluxDB port |
| `org` | string | *(required)* | InfluxDB organization |
| `bucket` | string | *(required)* | Data bucket |
| `measurement` | string | *(required)* | Measurement name |
| `token` | string | *(required)* | API token |
| `range` | string | `"-5m"` | Time range (Flux format) |
| `field_filter` | string | *(optional)* | Field name filter |
| `ssh_host` | string | *(optional)* | SSH tunnel host |
| `limit` | integer | `100` | Max rows (1 – 10 000) |
| `aggregate` | boolean | `false` | When true, compute mean/stddev/min/max per field+tag group across all returned points |

**Returns:** `MtlInfluxdbQueryData` — `bucket`, `measurement`, `range`, `series[]`, `row_count`, `truncated`, `aggregates?` (when `aggregate=true`: `InfluxDbFieldAggregate[]` with `field`, `tags`, `n`, `mean`, `stddev`, `min`, `max`).

---

### `mtl_scheduler_map`

Map MTL scheduler-to-session assignments. Per-scheduler session count, lcore, quota utilization. Warns about imbalanced distribution.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_paths` | string[] | *(optional)* | Log file paths |

**Returns:** `MtlSchedulerMapData` — `instances[]`, `instance_count`.

---

## BCC / eBPF Tracing

These 8 tools use [BCC (BPF Compiler Collection)](https://github.com/iovisor/bcc) for deep kernel tracing. They require `bpfcc-tools` installed and root privileges. All tools support remote execution over SSH via the `host` parameter.

### `runq_latency`

Run-queue (scheduler) latency distribution via `runqlat`. Shows how long tasks wait before being scheduled. **High latency = CPU contention.**

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `per_cpu` | boolean | `false` | Per-CPU histograms |
| `pid` | number | *(optional)* | Filter to specific PID |

**Returns:** `RunqLatencyData` — `duration_sec`, `per_cpu`, `histograms[]` with p50/p99, `warnings[]`.

---

### `offcpu_time`

Off-CPU time with kernel + user stacks showing **where** threads sleep/block. Top stacks reveal the exact blocking cause (locks, I/O, futex, sleep).

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `pid` | number | *(optional)* | Filter to PID |
| `min_block_usec` | number | `1000` | Minimum block time |
| `top_n` | integer | `20` | Top N stacks (1 – 100) |

**Returns:** `OffcpuTimeData` — `top_stacks[]` with `frames[]` / `total_usec`, `total_stacks_captured`.

---

### `hardirq_latency`

Hard IRQ handler time and count via `hardirqs`. Shows which hardware interrupt handlers run and how long they take.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `mode` | `"time" \| "count"` | `"time"` | Measure time or count |

**Returns:** `HardirqLatencyData` — `irqs[]` with `irq_name`, `count`, `total_usec`, `avg_usec`.

---

### `cpudist`

CPU on-time / off-time distribution per-process via `cpudist`. Histogram of how long threads run before being descheduled.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `mode` | `"on-cpu" \| "off-cpu"` | `"on-cpu"` | Measurement mode |
| `per_process` | boolean | `false` | Per-process histograms |
| `pid` | number | *(optional)* | Filter to PID |

**Returns:** `CpudistData` — `histograms[]` with p50/p99, `mode`.

---

### `critical_sections`

Detect long kernel critical sections (preempt/IRQ disabled) via `criticalstat`. Hidden source of latency spikes.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `threshold_usec` | number | `100` | Minimum section length |

**Returns:** `CriticalSectionsData` — `entries[]` with `latency_usec`, `caller`, `stack`, `total_violations`.

---

### `wakeup_sources`

Trace what wakes up blocked threads via `wakeuptime`. Kernel stacks at wakeup points. Complementary to `offcpu_time`.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `pid` | number | *(optional)* | Filter to PID |
| `top_n` | integer | `20` | Top N stacks (1 – 100) |

**Returns:** `WakeupSourcesData` — `top_stacks[]` with `frames[]` / `total_usec`.

---

### `llc_stat`

Last-Level Cache hit/miss statistics per process via `llcstat`. Uses hardware perf counters to sample LLC references and misses.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `sample_period` | number | `100` | Perf event sample period (1 – 10 000) |

**Returns:** `LlcStatData` — `entries[]` with `pid`, `comm`, `cpu`, `references`, `misses`, `hit_pct`, `summary`.

---

### `funclatency`

Function latency histogram via `funclatency`. Traces a kernel or user-space function and shows execution-time distribution.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `function_pattern` | string | *(required)* | Function to trace (e.g. `do_sys_open`, `c:malloc`) |
| `duration_sec` | number | `5` | Trace duration (1 – 60 s) |
| `unit` | `"nsecs" \| "usecs" \| "msecs"` | `"usecs"` | Histogram time unit |
| `pid` | number | *(optional)* | Filter to PID |

**Returns:** `FunclatencyData` — `histograms[]` with p50/p99, `function_pattern`.

---

## RDMA / DCB / NIC / Network

These 8 tools provide deep visibility into RDMA, DCB (Data Center Bridging), NIC hardware, and kernel network stack counters. Essential for diagnosing RoCEv2 performance issues, PFC storms, and packet loss.

### `rdma_health`

RDMA device health: HW counters (retransmissions, ECN, CNP, errors), QP states, throughput. Key for RDMA throughput drops and congestion diagnosis.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `device_filter` | string | *(optional)* | Regular expression filter for RDMA devices |
| `seconds` | number | `0` | Delta measurement (0 = snapshot) |
| `include_qps` | boolean | `true` | Include QP state breakdown |

**Returns:** `RdmaHealthData` — `devices[]` with counters, QP info, rates; `warnings[]`.

---

### `dcb_status`

DCB configuration: PFC enabled priorities, ETS bandwidth allocation, link MTU/state/speed, VF configuration. Auto-discovers RDMA-capable interfaces.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `interfaces` | string[] | *(optional)* | Limit to specific interfaces |
| `include_vf_info` | boolean | `false` | Include VF details |

**Returns:** `DcbStatusData` — `interfaces[]` with PFC/ETS/VF info; `warnings[]`.

---

### `nic_ethtool_stats`

NIC HW counters via `ethtool -S`: traffic, errors, drops, per-queue. Delta mode for rate computation. Filters zero counters by default.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `interface` | string | *(required)* | NIC interface |
| `seconds` | number | `0` | Delta measurement (0 = snapshot) |
| `include_queues` | boolean | `false` | Include per-queue counters |
| `filter` | string | *(optional)* | Regular expression filter |
| `non_zero_only` | boolean | `true` | Hide zero counters |

**Returns:** `NicEthtoolStatsData` — `counters[]`, `queues[]?`, `rates`, `warnings`.

---

### `nic_vf_stats`

Per-VF (Virtual Function) packet/byte/drop counters on a PF NIC. Resolves VF BDFs from sysfs. Delta mode with rate computation.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `pf_interface` | string | *(required)* | Physical function interface |
| `seconds` | number | `0` | Delta measurement (0 = snapshot) |

**Returns:** `NicVfStatsData` — `vfs[]` with rx/tx bytes, packets, drops, rates.

---

### `rdma_counters`

RDMA transport-level counters from `rdma stat show`: InRdmaWrites, OutRdmaReads, cnpSent, cnpReceived, RxECNMrkd. Critical for PFC/ECN debugging.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `device` | string | *(optional)* | Specific RDMA device |
| `seconds` | number | `0` | Delta measurement (0 = snapshot) |

**Returns:** `RdmaCountersData` — `devices[]` with `counters{}`, `rates{}`.

---

### `network_stats`

Kernel network stack counters from `nstat -az`: TCP retransmissions, UDP overflows, IP fragmentation, listen drops. Invisible to ethtool.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `seconds` | number | `0` | Delta measurement (0 = snapshot) |
| `filter` | string | *(optional)* | Counter name filter |

**Returns:** `NetworkStatsData` — `counters{}`, `deltas{}`, `highlights[]`.

---

### `devlink_health`

NIC firmware health reporters: FW crash counts, TX hang events, MDD errors. NIC-internal errors invisible to ethtool and RDMA counters. Works on mlx5 and ice.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `pci_slot` | string | *(optional)* | PCI slot filter |

**Returns:** `DevlinkHealthData` — `reporters[]`, `devices_checked`, `warnings[]`.

---

### `socket_diag`

TCP/UDP socket diagnostics via `ss`: socket states, queue sizes, PID/process, TCP internals (RTT, CWND, retransmits).

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `protocol` | `"tcp" \| "udp" \| "all"` | `"tcp"` | Protocol filter |
| `state_filter` | string | *(optional)* | Socket state filter |
| `process_filter` | string | *(optional)* | Process name filter |

**Returns:** `SocketDiagData` — `sockets[]`, `total_count`, `state_summary{}`.

---

## Turbostat & Composite Diagnostics

### `turbostat_snapshot`

MSR-based per-core CPU metrics: actual frequency (MPERF/APERF), C-state residency, SMI counts, IPC, temperature, RAPL power. Auto-resolves HT siblings. `dpdk_only=true` auto-discovers DPDK lcore CPUs.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `interval_sec` | number | `1` | Measurement interval (1 – 30 s) |
| `cpu_filter` | integer[] | *(optional)* | Limit to specific CPUs |
| `dpdk_only` | boolean | `false` | Auto-discover DPDK lcores |

**Returns:** `TurbostatSnapshotData` — `package_summary`, `cores[]`, `ipc_anomalies[]`, `warnings[]`.

---

### `diagnose_fps_drop`

**One-shot composite diagnostic** for pipeline FPS drops. Parses log for FPS + lcores, maps VF BDFs, runs turbostat, checks NIC/RDMA, scans log errors. Ranks suspects by severity.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_path` | string | *(required)* | Pipeline log file |
| `target_fps` | number | *(optional)* | Expected FPS target |

**Returns:** `DiagnoseFpsDropData` — `fps_actual`, `fps_target`, `suspects[]` ranked by severity, `raw_data{}`.

---

### `pipeline_health`

Composite health check for MTL/RDMA media pipelines. Discovers instances, checks liveness, parses FPS/drops, verifies thumbnails, probes HTTP endpoints.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_dirs` | string[] | *(optional)* | Log directories |
| `shm_stats_pattern` | string | `"/dev/shm/*stats*.json"` | Shared memory stats glob |
| `thumbnail_dirs` | string[] | *(optional)* | Thumbnail directories |
| `http_endpoints` | `{ url, label }[]` | *(optional)* | HTTP healthcheck endpoints |
| `max_thumbnail_age_sec` | number | `5` | Stale thumbnail threshold |
| `max_log_errors_window_min` | number | `5` | Error window (minutes) |

**Returns:** `PipelineHealthData` — `processes[]`, `logs[]`, `shm_stats[]`, `http_endpoints[]`, `thumbnails[]`, `warnings[]`.

---

### `correlated_snapshot`

**Synchronized multi-tool capture** for A/B test comparisons. Runs all requested collectors in parallel within a common time window, ensuring metrics are temporally correlated. Available collectors: `core_load`, `running_on_cores`, `irq_distribution`, `softirq_snapshot`, `cpu_frequency`, `context_switch_rate`, `pcm_core_counters`, `pcm_cache_analysis`, `mtl_session_stats`.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `collectors` | string[] | `[]` | Collector names to run. Empty array = all collectors. |
| `window_s` | number | `2` | Capture window in seconds (1 – 30). PCM and windowed collectors share this window. |
| `core_filter` | string \| null | `null` | Core filter for PCM tools. Accepts: `'4'`, `'4,5,6'`, `'4-13'`, `'4-13,20'`. Omit for all cores. |
| `focus_cpus` | string \| null | `null` | CPUs to inspect with `running_on_cores`. Same format as `core_filter`. Required if that collector is used. |
| `mtl_host` | string | `"localhost"` | Target host for MTL session stats |
| `mtl_log_path` | string | *(optional)* | Path to MTL log file (required if `mtl_session_stats` collector is used) |
| `mtl_last_dumps` | integer | `5` | Number of last MTL stat dumps to use (1 – 100) |
| `pcm_seconds` | integer | *(optional)* | PCM delta window (defaults to `min(window_s, 5)`) |

**Returns:** `CorrelatedSnapshotData` — `timestamp`, `window_s`, `collectors_run[]`, plus one key per collector with its result (e.g. `core_load`, `pcm_core_counters`, etc.). Each collector's data matches its standalone tool's return type.

---

## Log Search

### `log_search`

Search pipeline log files for errors, warnings, or custom patterns with structured deduplication. Distinguishes real errors from MTL informational stat dump lines.

| Parameter | Type | Default | Description |
| ----------- | ------ | --------- | ------------- |
| `host` | string | `"localhost"` | Target host |
| `log_dirs` | string[] | *(optional)* | Directories to search |
| `log_paths` | string[] | *(optional)* | Specific files |
| `pattern` | string | *(optional)* | Regular expression pattern |
| `minutes` | number | `5` | Look-back window (1 – 1440 min) |
| `severity` | `"error" \| "warning" \| "all"` | `"error"` | Filter severity |
| `exclude_stat_dumps` | boolean | `true` | Exclude MTL stat dump lines |
| `max_results` | number | `50` | Max matches (1 – 500) |
| `dedup` | boolean | `true` | Deduplicate similar messages |

**Returns:** `LogSearchData` — `matches[]`, `match_count`, `truncated`, `files[]`.
