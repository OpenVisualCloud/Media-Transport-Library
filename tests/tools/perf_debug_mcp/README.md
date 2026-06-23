# perf_debug_mcp

An MCP (Model Context Protocol) server that provides **101 read-only Linux
performance debugging tools** for LLM agents in Visual Studio Code.
Designed for real-time triage of latency-sensitive
[Media Transport Library](https://github.com/OpenVisualCloud/Media-Transport-Library)
(MTL) pipelines on Intel platforms.

## Overview

perf_debug_mcp turns your Linux host into a structured debugging data source
that an AI coding assistant (GitHub Copilot) can query conversationally.
Instead of manually running `perf`, `turbostat`, `ethtool`, or reading `/proc`
files, the LLM calls named tools and receives typed JSON responses — complete
with metadata, severity flags, and actionable recommendations.

### Why MCP for MTL debugging

MTL ST 2110 pipelines are sensitive to CPU scheduling, NUMA placement, NIC
interrupt affinity, PFC back-pressure, and DPDK hugepage configuration.
Diagnosing FPS drops or frame losses requires correlating data from multiple
subsystems simultaneously. This server lets an LLM do that correlation
automatically:

- **20 MTL-specific tools** — session stats via USDT probes, DPDK telemetry,
  scheduler mapping, hugepage monitoring, InfluxDB queries, FPS-drop diagnosis
- **20 core CPU tools** — per-core utilization, affinity masks, starvation
  detection, IRQ/softirq interference, NUMA topology
- **13 Intel PCM tools** — IPC, cache hit rates, memory bandwidth, power,
  QPI/UPI, PCIe bandwidth
- **19 BCC/eBPF tracing tools** — runqueue latency, off-CPU analysis,
  function latency, lock contention, memory leaks
- **8 RDMA/network tools** — RDMA health, DCB/PFC status, NIC HW counters
- **Plus** EMON, Intel RDT, perf analysis, and composite diagnostic tools

### MTL USDT integration

The 8 USDT tracing tools attach directly to MTL's 82 built-in USDT probes
(11 providers: sys, ptp, st20, st20p, st22, st22p, st30, st30p, st40, st40p,
st41). USDT is the preferred data source — zero overhead when not attached,
real-time introspection when active:

| Tool | What it measures |
| ------ | ----------------- |
| `mtl_usdt_session_stats` | 3-tier fallback: USDT then logs then InfluxDB |
| `mtl_usdt_tasklet_timing` | Per-scheduler and per-tasklet timing |
| `mtl_usdt_session_timing` | Per-session processing latency |
| `mtl_usdt_frame_trace` | Frame lifecycle with per-stream FPS |
| `mtl_usdt_ptp_trace` | PTP synchronization delta statistics |
<!-- textlint-disable terminology -->
| `mtl_usdt_cni_pcap` | CNI packet capture to pcapng files |
<!-- textlint-enable terminology -->
| `mtl_usdt_log_monitor` | Real-time log capture (read-only) |
| `mtl_usdt_list_probes` | Enumerate all available USDT probes |

## Quick start

### Prerequisites

| Requirement | Version | Notes |
| ------------- | --------- | ------- |
| Node.js | >= 18 | Runtime |
| Linux kernel | >= 4.18 | cgroup v2 for cgroup tools |
| `bpftrace` | *(optional)* | USDT tracing tools (8 tools) |
| `bpfcc-tools` | *(optional)* | BCC tracing tools (19 tools) |
| Intel PCM | *(optional)* | `pcm-sensor-server` (13 tools) |
| Intel EMON/SEP | *(optional)* | Uncore PMU counters (4 tools) |
| `rdma-core` | *(optional)* | RDMA tools (`rdma`, `ibstat`) |
| `intel-cmt-cat` | *(optional)* | Intel RDT tools (`pqos`, 4 tools) |
| Rust toolchain | *(optional)* | Native eBPF helper |

### Build

```bash
cd tests/tools/perf_debug_mcp
npm install
npm run build            # TypeScript -> dist/
npm run build:native     # Rust eBPF helper (optional)
```

Or use the included setup script for a full install of all dependencies:

```bash
chmod +x setup.sh
sudo ./setup.sh          # installs Node.js, PCM, BCC, RDMA, etc.
```

### Register with Visual Studio Code

Add to your workspace `.vscode/mcp.json`:

```jsonc
{
  "servers": {
    "perf-debug": {
      "type": "stdio",
      "command": "node",
      "args": ["dist/index.js"],
      "cwd": "/path/to/Media-Transport-Library/tests/tools/perf_debug_mcp"
    }
  }
}
```

Restart Visual Studio Code. All 101 tools appear in the Copilot tool list prefixed with
`mcp_perf-debug_`.

### TCP mode (for debugging)

```bash
node dist/index.js --tcp --port 3001
```

## Tool inventory (101 tools)

Every tool works independently. Optional subsystems (PCM, EMON, BCC, USDT)
degrade gracefully when absent.

| Category | Count | Requires | Description |
| ---------- | ------- | ---------- | ------------- |
| [Core CPU / Scheduling](#core-cpu--scheduling-20-tools) | 20 | `/proc`, `/sys` | Utilization, affinity, starvation, IRQ, NUMA, cgroups |
| [Intel PCM](#intel-pcm-13-tools) | 13 | `pcm-sensor-server` | IPC, cache, memory BW, power, QPI/UPI, accelerators |
| [Intel EMON](#intel-emon-4-tools) | 4 | EMON/SEP + drivers | IIO, CHA, mesh stalls, UPI, core stall TMA |
| [Intel RDT](#intel-rdt-4-tools) | 4 | `pqos` | Cache allocation (CAT), MBA, occupancy, MBM |
| [Perf / PMU Analysis](#perf--pmu-analysis-4-tools) | 4 | `perf` | IPC analysis, TMA, stall breakdown, flame graphs |
| [MTL / Media Transport](#mtl--media-transport-12-tools) | 12 | MTL/DPDK processes | Stats, telemetry, FPS, scheduler mapping, InfluxDB |
| [MTL USDT Tracing](#mtl-usdt-tracing-8-tools) | 8 | `bpftrace` + root | Session stats, timing, frame tracing, PTP, pcap |
| [BCC / eBPF Tracing](#bcc--ebpf-tracing-19-tools) | 19 | `bpfcc-tools` + root | runqlat, offcputime, funclatency, memleak, and more |
| [RDMA / Network](#rdma--network-8-tools) | 8 | System tools | RDMA health, DCB, ethtool, devlink, socket diag |
| [Composite and Meta](#composite-and-meta-8-tools) | 8 | Various | FPS-drop triage, pipeline health, USE checklist |
| [Log Search](#log-search-1-tool) | 1 | Log files | Pattern search with deduplication |

### Core CPU / Scheduling (20 tools)

Read `/proc`, `/sys`, and scheduler syscalls. Work **unprivileged** on any
Linux >= 4.18.

| Tool | Description |
| ------ | ------------- |
| `capabilities` | Environment info and enabled features — **call first** |
| `core_load_snapshot` | Per-CPU utilization (user/system/irq/softirq/iowait) |
| `allowed_on_core` | Tasks whose affinity mask includes a CPU |
| `running_on_core` | Tasks observed running on a CPU (with runtime ns) |
| `starvation_report` | Starvation analysis with IRQ interference and recommendations |
| `irq_distribution` | IRQ distribution per CPU (delta) |
| `irq_affinity` | `smp_affinity` for IRQs by number or name regular expression |
| `nic_irq_queue_map` | NIC queue to IRQ to CPU mapping |
| `softirq_snapshot` | Per-CPU softirq deltas (NET_RX, TIMER, SCHED, RCU) |
| `kernel_threads_on_core` | Kernel threads pinned/seen on a CPU |
| `runqueue_snapshot` | Load averages + per-CPU schedstat |
| `context_switch_rate` | Global and per-CPU context switch rate |
| `isolation_summary` | `isolcpus`, `nohz_full`, `rcu_nocbs` warnings |
| `cpu_frequency_snapshot` | cpufreq governor, current/min/max frequency |
| `throttling_summary` | Thermal and RT throttle detection |
| `cgroup_cpu_limits` | cgroup v2 quotas, weights, throttle counters |
| `numa_topology` | NUMA nodes, CPUs, memory, distance matrix |
| `process_numa_placement` | NUMA placement and cross-node warnings |
| `perf_debug_snapshot` | All-in-one triage (loads, IRQ, softirq, frequency, PCM) |
| `off_cpu_analysis` | Per-task scheduling latency via schedstat deltas |

### Intel PCM (13 tools)

Query [Intel PCM](https://github.com/intel/pcm) hardware counters via
`pcm-sensor-server` on port 9738.

| Tool | Description |
| ------ | ------------- |
| `pcm_core_counters` | Per-core IPC, cache, frequency, memory BW (multi-sample) |
| `pcm_memory_bandwidth` | Per-socket DRAM/PMM bandwidth |
| `pcm_cache_analysis` | L2/L3 hit/miss ratios and occupancy (multi-sample) |
| `pcm_power_thermal` | Thermal headroom, C-states, energy, TMA |
| `pcm_qpi_upi_link` | Inter-socket link traffic and utilization |
| `pcm_pcie_bandwidth` | Aggregate PCIe/MC IO bandwidth per socket |
| `pcm_numa_traffic` | Local vs remote memory BW with locality ratio |
| `pcm_latency` | DDR/PMM memory access latency |
| `pcm_accel` | Intel accelerator (IAA/DSA/QAT) utilization |
| `pcm_iio` | Per-PCIe-stack I/O bandwidth |
| `pcm_memory_per_channel` | Per-DDR-channel memory bandwidth |
| `pcm_bw_histogram` | Memory bandwidth utilization histogram |
| `pcm_tsx` | Intel TSX transaction/abort monitoring |

### Intel EMON (4 tools)

[Intel EMON/SEP](https://www.intel.com/content/www/us/en/developer/articles/tool/emon-user-guide.html)
uncore PMU collection with 5 curated presets for Sapphire Rapids.

| Tool | Description |
| ------ | ------------- |
| `emon_capabilities` | EMON version, drivers, PMU types, available presets |
| `emon_collect` | Single preset collection (E0-E4) with structured output |
| `emon_triage` | Cascading waterfall: E0 then E4, severity-ranked diagnosis |
| `emon_pcie_topology` | PCIe device to IIO stack mapping via sysfs |

### Intel RDT (4 tools)

Monitor and configure
[Intel Resource Director Technology](https://www.intel.com/content/www/us/en/architecture-and-technology/resource-director-technology.html)
via `pqos`.

| Tool | Description |
| ------ | ------------- |
| `rdt_cache_allocation` | L3 CAT configuration |
| `rdt_cache_monitoring` | Per-core L3 cache occupancy and IPC |
| `rdt_mba_config` | Memory Bandwidth Allocation configuration |
| `rdt_memory_bandwidth` | Per-core memory bandwidth via RDT/MBM |

### Perf / PMU Analysis (4 tools)

| Tool | Description |
| ------ | ------------- |
| `ipc_analysis` | IPC via `perf stat` — memory-stalled vs instruction-bound |
| `tma_analysis` | Top-Down Microarchitecture Analysis via `perf stat --topdown` |
| `stall_cycle_breakdown` | CPU cycle breakdown — work vs cache/memory/TLB stalls |
| `profile_flamegraph` | CPU profiling with folded-stack output for flame graphs |

### MTL / Media Transport (12 tools)

Monitor
[MTL](https://github.com/OpenVisualCloud/Media-Transport-Library)
DPDK-based ST 2110 media pipelines.

| Tool | Description |
| ------ | ------------- |
| `mtl_manager_status` | MtlManager daemon status and lcore allocations |
| `mtl_dpdk_telemetry` | DPDK telemetry (ethdev stats, mempool, heap) |
| `mtl_instance_processes` | MTL/DPDK process discovery with affinity |
| `mtl_session_stats` | FPS, throughput, scheduler timings from log files |
| `mtl_app_latency` | Per-stage latency from JSON log samples |
| `mtl_live_stats` | Real-time stats from shared memory JSON |
| `mtl_nic_pf_stats` | NIC PF counters via ethtool |
| `mtl_hugepage_usage` | Hugepage allocation and DPDK file count |
| `mtl_lcore_shm` | SysV shared memory lcore allocation table |
| `mtl_influxdb_query` | InfluxDB v2 queries with `aggregate` mode for A/B testing |
| `mtl_scheduler_map` | Scheduler-to-session mapping and balance analysis |
| `dpdk_telemetry_deep` | Extended DPDK telemetry: xstats, ring info, mempool |

### MTL USDT Tracing (8 tools)

Live tracing of
[MTL USDT probes](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/doc/usdt.md)
via `bpftrace`. Requires `bpftrace` and root. MTL embeds 82 USDT probes
across 11 providers.

**USDT is the preferred data source** — zero overhead when not attached,
rich real-time measurements when active. The fallback order is:
USDT then logs then InfluxDB.

| Tool | Description |
| ------ | ------------- |
| `mtl_usdt_list_probes` | Enumerate all USDT probes in libmtl.so |
| `mtl_usdt_session_stats` | 3-tier session stats (USDT then logs then InfluxDB) |
| `mtl_usdt_tasklet_timing` | Per-scheduler and per-tasklet timing breakdown |
| `mtl_usdt_session_timing` | Per-session processing latency |
| `mtl_usdt_frame_trace` | Frame lifecycle with per-stream FPS |
| `mtl_usdt_ptp_trace` | PTP sync delta statistics (avg/min/max/stddev) |
<!-- textlint-disable terminology -->
| `mtl_usdt_cni_pcap` | Trigger CNI packet capture to pcapng files |
<!-- textlint-enable terminology -->
| `mtl_usdt_log_monitor` | Real-time log capture (read-only, no side effects) |

### BCC / eBPF Tracing (19 tools)

Deep kernel tracing via [BCC](https://github.com/iovisor/bcc). Requires
`bpfcc-tools` and root.

| Tool | Description |
| ------ | ------------- |
| `runq_latency` | Run-queue latency distribution (p50/p99) |
| `offcpu_time` | Off-CPU blocking stacks |
| `hardirq_latency` | Hard IRQ handler execution time |
| `cpudist` | CPU on-time / off-time distribution per process |
| `critical_sections` | Long preempt/IRQ-disabled kernel sections |
| `wakeup_sources` | What wakes blocked threads |
| `llc_stat` | LLC hit/miss per process via perf counters |
| `funclatency` | Latency histogram for any kernel or user function |
| `biolatency` | Block I/O latency distribution |
| `cachestat` | Page cache hit/miss ratio |
| `cpuunclaimed` | Idle CPUs while tasks wait — scheduler imbalance |
| `klockstat` | Kernel mutex/spinlock contention |
| `memleak` | Trace outstanding memory allocations not yet freed |
| `offwaketime` | Combined off-CPU + waker stack analysis |
| `runqlen` | Run queue length distribution |
| `runqslower` | Scheduling delays above a threshold |
| `softirq_latency` | Per-softirq-type latency distribution |
| `working_set_size` | Process working set size via clear_refs |
| `wqlat` | Kernel workqueue latency |

### RDMA / Network (8 tools)

| Tool | Description |
| ------ | ------------- |
| `rdma_health` | RDMA device counters, QP states, ECN/CNP |
| `dcb_status` | PFC priorities, ETS bandwidth, VF config |
| `nic_ethtool_stats` | NIC HW counters via ethtool (delta, per-queue) |
| `nic_vf_stats` | Per-VF packet/byte/drop counters |
| `rdma_counters` | Transport-level RDMA stats |
| `network_stats` | Kernel nstat (TCP retrans, UDP overflow) |
| `devlink_health` | NIC firmware health reporters |
| `socket_diag` | TCP/UDP socket states, queue sizes, RTT |

### Composite and Meta (8 tools)

| Tool | Description |
| ------ | ------------- |
| `turbostat_snapshot` | Per-core freq, C-states, IPC, temp, RAPL power |
| `diagnose_fps_drop` | One-shot FPS triage: logs, turbostat, NIC, RDMA |
| `pipeline_health` | Composite pipeline health check |
| `correlated_snapshot` | Synchronized multi-tool capture for A/B testing |
| `snapshot_diff` | A/B comparison with numeric deltas |
| `use_method_checklist` | Brendan Gregg USE Method automated health check |
| `tool_guide` | Categorized tool index for LLM tool selection |
| `numasched` | Cross-NUMA task migration tracking |

### Log Search (1 tool)

| Tool | Description |
| ------ | ------------- |
| `log_search` | Error/warning pattern search with deduplication |

## Architecture

```text
+------------------------------+
|  Visual Studio Code / GitHub Copilot    |
|  MCP Client (stdio)          |
+----------+-------------------+
           | JSON-RPC / MCP
+----------v-------------------+
|  TypeScript MCP Server       |  <- src/server.ts (101 tools)
|  @modelcontextprotocol/sdk   |
+--+------+------+------+-----+
   |      |      |      |
   |  subprocess | HTTP  | ssh
   |      |      |(9738) +---- Remote hosts
   |      |      |      |
+--v--+ +-v----++v-----+
|/proc| | BCC  || PCM  |
|/sys | | EMON ||sensor|
|sysfs| |turbo ||server|
+-----+ +------++------+
```

### Operating modes

| Mode | Privilege | Description |
| ------ | ----------- | ------------- |
| **Fallback** | Unprivileged | `/proc` + `/sys` + syscalls — always works |
| **eBPF** | `CAP_BPF` + `CAP_PERFMON` | sched tracepoints via Rust helper |
| **USDT** | Root + `bpftrace` | MTL USDT probes — **preferred** for MTL data |
| **BCC** | Root | Full BCC tracer suite |
| **PCM** | `pcm-sensor-server` | Hardware performance counters |
| **EMON** | Root + SEP drivers | Uncore PMU via EMON/SEP |

The `capabilities` tool reports which modes are available at runtime.

### Remote execution

Many tools accept a `host` parameter. When set to anything other than
`"localhost"`, commands execute over SSH on the remote host.

## Output contract

Every tool returns a `ToolResponse<T>` envelope:

```json
{
  "ok": true,
  "meta": {
    "timestamp_wall": "2025-01-15T12:34:56.789Z",
    "t_monotonic_ns": 123456789,
    "mode_used": "fallback",
    "window_ms": 250,
    "host": { "hostname": "myhost", "kernel": "6.8.0", "cpu_count": 224 }
  },
  "data": {}
}
```

On error:

```json
{
  "ok": false,
  "meta": {},
  "error": {
    "code": "EPERM",
    "message": "Cannot read /proc/1/status",
    "hint": "Run with elevated privileges or check pid"
  }
}
```

## Testing

```bash
npm test    # 224 tests (unit, integration, PCM, MTL)
```

## Environment variables

| Variable | Default | Description |
| ---------- | --------- | ------------- |
| `PCM_HOST` | `127.0.0.1` | pcm-sensor-server hostname |
| `PCM_PORT` | `9738` | pcm-sensor-server port |

## Technology stack

| Component | Version |
| ----------- | --------- |
| TypeScript | ^5.4 (strict, ECMAScript 2022, Node16 ESM) |
| MCP SDK | `@modelcontextprotocol/sdk` ^1.12.1 |
| Testing | Jest 29 + ts-jest (ESM) |
| Runtime | Node.js >= 18 |
| Native | Rust (optional) — `libbpf-rs`, `serde`, `clap` |

## License

BSD 3-Clause License. See [LICENSE](LICENSE) for full terms.
