/**
 * cpu-debug-mcp — MCP Server
 *
 * Provides read-only Linux performance-debugging tools for CPU/core scheduling,
 * affinity, starvation, IRQ/softirq interference, and broader performance debugging.
 *
 * Supports stdio transport (preferred for VS Code) and optionally TCP.
 */
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { capabilities } from "./tools/capabilities.js";
import { coreLoadSnapshot } from "./tools/core-load-snapshot.js";
import { allowedOnCore } from "./tools/allowed-on-core.js";
import { runningOnCore } from "./tools/running-on-core.js";
import { starvationReport } from "./tools/starvation-report.js";
import { irqDistribution } from "./tools/irq-distribution.js";
import { irqAffinity } from "./tools/irq-affinity.js";
import { nicIrqQueueMap } from "./tools/nic-irq-queue-map.js";
import { softirqSnapshot } from "./tools/softirq-snapshot.js";
import { kernelThreadsOnCore } from "./tools/kernel-threads-on-core.js";
import { runqueueSnapshot } from "./tools/runqueue-snapshot.js";
import { contextSwitchRate } from "./tools/context-switch-rate.js";
import { isolationSummary } from "./tools/isolation-summary.js";
import { cpuFrequencySnapshot } from "./tools/cpu-frequency-snapshot.js";
import { throttlingSummary } from "./tools/throttling-summary.js";
import { cgroupCpuLimits } from "./tools/cgroup-cpu-limits.js";
import { numaTopology } from "./tools/numa-topology.js";
import { processNumaPlacement } from "./tools/process-numa-placement.js";
import { perfDebugSnapshot } from "./tools/perf-debug-snapshot.js";
import { pcmCoreCounters } from "./tools/pcm-core-counters.js";
import { pcmMemoryBandwidth } from "./tools/pcm-memory-bandwidth.js";
import { pcmCacheAnalysis } from "./tools/pcm-cache-analysis.js";
import { pcmPowerThermal } from "./tools/pcm-power-thermal.js";
import { pcmQpiUpiLink } from "./tools/pcm-qpi-upi-link.js";
import { pcmPcieBandwidth } from "./tools/pcm-pcie-bandwidth.js";
import { pcmNumaTraffic } from "./tools/pcm-numa-traffic.js";
import { pcmLatency } from "./tools/pcm-latency.js";
import { pcmAccel } from "./tools/pcm-accel.js";
import { pcmIio } from "./tools/pcm-iio.js";
import { pcmMemoryPerChannel } from "./tools/pcm-memory-per-channel.js";
import { pcmBwHistogram } from "./tools/pcm-bw-histogram.js";
import { pcmTsx } from "./tools/pcm-tsx.js";

// Intel RDT (Resource Director Technology) tools
import { rdtCacheMonitoring } from "./tools/rdt-cache-monitoring.js";
import { rdtMemoryBandwidth } from "./tools/rdt-memory-bandwidth.js";
import { rdtCacheAllocation } from "./tools/rdt-cache-allocation.js";
import { rdtMbaConfig } from "./tools/rdt-mba-config.js";

// Methodology & Analysis tools
import { useMethodChecklist } from "./tools/use-method-checklist.js";
import { ipcAnalysis } from "./tools/ipc-analysis.js";
import { stallCycleBreakdown } from "./tools/stall-cycle-breakdown.js";
import { tmaAnalysis } from "./tools/tma-analysis.js";
import { snapshotDiff } from "./tools/snapshot-diff.js";
import { offCpuAnalysis } from "./tools/off-cpu-analysis.js";
import { dpdkTelemetryDeep } from "./tools/dpdk-telemetry-deep.js";
import { workingSetSize } from "./tools/working-set-size.js";

// EMON (Intel Event Monitor) tools
import { emonCapabilities } from "./tools/emon-capabilities.js";
import { emonCollect } from "./tools/emon-collect.js";
import { emonTriage } from "./tools/emon-triage.js";
import { emonPcieTopology } from "./tools/emon-pcie-topology.js";

// MTL (Media Transport Library) tools
import { mtlManagerStatus } from "./tools/mtl-manager-status.js";
import { mtlDpdkTelemetry } from "./tools/mtl-dpdk-telemetry.js";
import { mtlInstanceProcesses } from "./tools/mtl-instance-processes.js";
import { mtlSessionStats } from "./tools/mtl-session-stats.js";
import { mtlAppLatency } from "./tools/mtl-app-latency.js";
import { mtlLiveStats } from "./tools/mtl-live-stats.js";
import { mtlNicPfStats } from "./tools/mtl-nic-pf-stats.js";
import { mtlHugepageUsage } from "./tools/mtl-hugepage-usage.js";
import { mtlLcoreShm } from "./tools/mtl-lcore-shm.js";
import { mtlInfluxdbQuery } from "./tools/mtl-influxdb-query.js";
import { mtlSchedulerMap } from "./tools/mtl-scheduler-map.js";

// MTL USDT tools
import { mtlUsdtProbes, mtlUsdtProbesSchema } from "./tools/mtl-usdt-probes.js";
import { mtlUsdtSessionStats, mtlUsdtSessionStatsSchema } from "./tools/mtl-usdt-session-stats.js";
import { mtlUsdtFrameTrace, mtlUsdtFrameTraceSchema } from "./tools/mtl-usdt-frame-trace.js";
import { mtlUsdtPtp, mtlUsdtPtpSchema } from "./tools/mtl-usdt-ptp.js";
import { mtlUsdtTaskletTiming, mtlUsdtTaskletTimingSchema } from "./tools/mtl-usdt-tasklet-timing.js";
import { mtlUsdtSessionTiming, mtlUsdtSessionTimingSchema } from "./tools/mtl-usdt-session-timing.js";
import { mtlUsdtCniPcap, mtlUsdtCniPcapSchema } from "./tools/mtl-usdt-cni-pcap.js";
import { mtlUsdtLogMonitor, mtlUsdtLogMonitorSchema } from "./tools/mtl-usdt-log-monitor.js";

// RDMA / DCB / NIC / Pipeline tools
import { rdmaHealth } from "./tools/rdma-health.js";
import { dcbStatus } from "./tools/dcb-status.js";
import { nicEthtoolStats } from "./tools/nic-ethtool-stats.js";
import { pipelineHealth } from "./tools/pipeline-health.js";
import { logSearch } from "./tools/log-search.js";

// Turbostat (MSR-based CPU metrics)
import { turbostatSnapshot } from "./tools/turbostat-snapshot.js";

// Composite diagnostic + VF stats tools
import { diagnoseFpsDrop } from "./tools/diagnose-fps-drop.js";
import { nicVfStats } from "./tools/nic-vf-stats.js";

// BCC / eBPF tracing tools
import { runqLatency } from "./tools/runq-latency.js";
import { offcpuTime } from "./tools/offcpu-time.js";
import { hardirqLatency } from "./tools/hardirq-latency.js";
import { cpudist } from "./tools/cpudist.js";
import { criticalSections } from "./tools/critical-sections.js";
import { wakeupSources } from "./tools/wakeup-sources.js";
import { llcStat } from "./tools/llc-stat.js";
import { funclatency } from "./tools/funclatency.js";
import { cpuUnclaimed } from "./tools/cpuunclaimed.js";
import { runqLen } from "./tools/runqlen.js";
import { runqSlower } from "./tools/runqslower.js";
import { offWakeTime } from "./tools/offwaketime.js";
import { profileFlamegraph } from "./tools/profile-flamegraph.js";
import { klockstat } from "./tools/klockstat.js";
import { softirqLatency } from "./tools/softirq-latency.js";
import { cachestat } from "./tools/cachestat.js";
import { biolatency } from "./tools/biolatency.js";
import { memleak } from "./tools/memleak.js";
import { numaSched } from "./tools/numasched.js";
import { wqlat } from "./tools/wqlat.js";

// RDMA / network / devlink tools
import { rdmaCounters } from "./tools/rdma-counters.js";
import { networkStats } from "./tools/network-stats.js";
import { devlinkHealth } from "./tools/devlink-health.js";
import { socketDiag } from "./tools/socket-diag.js";

// Correlated snapshot (A/B testing support)
import { correlatedSnapshot } from "./tools/correlated-snapshot.js";

// Tool guide (categorized index)
import { toolGuide } from "./tools/tool-guide.js";

export function createServer(): McpServer {
  const server = new McpServer({
    name: "cpu-debug-mcp",
    version: "0.5.2",
  });

  // ── capabilities ──────────────────────────────────────────────────
  server.tool(
    "capabilities",
    "Return environment info and enabled features: available subsystems (PCM, EMON, BCC/eBPF, USDT/bpftrace), " +
    "OS/kernel version, CPU model, tool count, and per-subsystem readiness (drivers loaded, binaries present, permissions). " +
    "Call this FIRST in any debugging session to know which tools are available before planning your diagnostic approach.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
    },
    async (params) => {
      const result = await capabilities(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── tool_guide ────────────────────────────────────────────────────
  server.tool(
    "tool_guide",
    "Categorized tool index with decision tree for choosing the right tool from 70+ options. " +
    "Returns: (1) decision_tree — maps common questions to recommended tools, " +
    "(2) categories — tools grouped by domain (CPU, IRQ, PCM, MTL, RDMA, BCC, etc.) with one-liner descriptions, " +
    "(3) drill_down_chains — step-by-step investigation sequences. " +
    "Call this when unsure which tool to use, or to discover tools by category.",
    {},
    async () => {
      const result = await toolGuide();
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── core_load_snapshot ────────────────────────────────────────────
  server.tool(
    "core_load_snapshot",
    "Compute per-core CPU utilization over a sampling window. Returns user/system/irq/softirq/iowait/idle breakdown. " +
    "Lightweight and fast (~250ms default). Use for a quick check of which cores are busy vs idle. " +
    "For a broader first-look (loads + IRQs + frequency + isolation in one call), use perf_debug_snapshot instead. " +
    "For MSR-level detail (IPC, C-states, SMI counts, temperature), use turbostat_snapshot.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(50).max(10000).default(250).describe("Sampling window in ms"),
      breakdown: z.boolean().default(true).describe("Include per-category breakdown"),
      mode: z.enum(["auto", "fallback", "ebpf"]).default("auto").describe("Data collection mode"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      top_n: z.number().int().min(1).max(1000).optional().describe("Return only the N most loaded cores (sorted by util_pct desc). Omit for all cores."),
    },
    async (params) => {
      const result = await coreLoadSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── allowed_on_core ───────────────────────────────────────────────
  server.tool(
    "allowed_on_core",
    "Enumerate all tasks whose CPU affinity mask INCLUDES the specified core — i.e., who CAN be scheduled there. " +
    "WARNING: Returns potentially thousands of tasks (every process with a wide affinity mask). " +
    "Only use to audit isolated DPDK cores for unexpected affinity (e.g., a rogue thread with a wide mask). " +
    "If you want to see who IS ACTUALLY RUNNING on a core (typically 1-20 tasks), use running_on_core instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      cpu: z.number().int().min(0).describe("CPU core number"),
      scope: z.enum(["threads", "processes"]).default("threads").describe("Enumerate threads or processes"),
      include_kernel_threads: z.boolean().default(true).describe("Include kernel threads"),
      limit: z.number().int().min(1).max(50000).default(5000).describe("Max tasks to return"),
    },
    async (params) => {
      const result = await allowedOnCore(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── running_on_core ───────────────────────────────────────────────
  server.tool(
    "running_on_core",
    "Return tasks OBSERVED RUNNING on a specific CPU within a sampling window — i.e., who IS actually consuming CPU time. " +
    "Shows runtime attribution in nanoseconds and context switch count per task. " +
    "Unlike allowed_on_core (who CAN run — returns thousands), this shows who DID run (typically 1-20 tasks). " +
    "Use to verify an isolated DPDK core has exactly one thread, or to identify unexpected co-runners stealing cycles.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      cpu: z.number().int().min(0).describe("CPU core number"),
      window_ms: z.number().min(50).max(10000).default(250).describe("Observation window in ms"),
      mode: z.enum(["auto", "fallback", "ebpf"]).default("auto").describe("Data collection mode"),
      top_n: z.number().int().min(1).max(500).default(50).describe("Max tasks to return"),
    },
    async (params) => {
      const result = await runningOnCore(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── starvation_report ─────────────────────────────────────────────
  server.tool(
    "starvation_report",
    "High-level starvation analysis for a specific process/thread: identifies co-runners on the same core, " +
    "IRQ/softirq interference percentage, thermal/cgroup throttling, and generates ranked actionable recommendations. " +
    "Use when a thread misses deadlines or shows high latency — this tool combines multiple checks into one diagnosis. " +
    "Needs a target (target_pid, target_tid, or target_comm_regex). Optionally set solo_cpu_hint (the core it should own exclusively). " +
    "For raw per-core data without recommendations, use running_on_core + irq_distribution separately.",
    {
      target_pid: z.number().int().optional().describe("Target process ID (flat param, preferred over nested target)"),
      target_tid: z.number().int().optional().describe("Target thread ID (flat param, preferred over nested target)"),
      target_comm_regex: z.string().optional().describe("Regex to match process name (flat param, preferred over nested target)"),
      target: z.object({
        pid: z.number().int().optional().describe("Process ID"),
        tid: z.number().int().optional().describe("Thread ID"),
        comm_regex: z.string().optional().describe("Regex to match process name"),
      }).optional().describe("Target specification (legacy nested form — prefer flat target_pid/target_tid/target_comm_regex)"),
      window_ms: z.number().min(100).max(30000).default(2000).describe("Observation window in ms"),
      solo_cpu_hint: z.number().int().min(0).optional().describe("Expected solo CPU for pinned tasks"),
      mode: z.enum(["auto", "fallback", "ebpf"]).default("auto").describe("Data collection mode"),
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      thresholds: z.object({
        co_runner_count_threshold: z.number().int().default(2).optional(),
        softirq_pct_threshold: z.number().default(5).optional(),
        irq_pct_threshold: z.number().default(5).optional(),
      }).optional().describe("Starvation detection thresholds"),
    },
    async (params) => {
      const result = await starvationReport(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── irq_distribution ──────────────────────────────────────────────
  server.tool(
    "irq_distribution",
    "Show IRQ distribution per CPU over a delta window: total IRQ count per core and per-IRQ/per-interface breakdown. " +
    "Use when investigating unexpected CPU load on isolated cores (IRQs can fire on any core regardless of isolcpus " +
    "unless affinity-pinned). Also useful to verify irdma/ice IRQs are steered away from DPDK cores. " +
    "For IRQ affinity CONFIG (not runtime counts), use irq_affinity instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(100).max(10000).default(1000).describe("Observation window in ms"),
      cpu_filter: z.number().int().min(0).optional().describe("Filter to a specific CPU (legacy, prefer core_filter)"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      include_per_irq: z.boolean().default(true).describe("Include per-IRQ breakdown"),
      include_per_iface: z.boolean().default(true).describe("Group IRQs by network interface"),
    },
    async (params) => {
      const result = await irqDistribution(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── irq_affinity ──────────────────────────────────────────────────
  server.tool(
    "irq_affinity",
    "Show the configured and effective CPU affinity mask for IRQs. Pass an IRQ number or regex to match IRQ names " +
    "(e.g., 'irdma' to find all RDMA IRQs, 'ice-ens1np0' for NIC-specific IRQs). " +
    "Use when verifying IRQ steering: are NIC/RDMA IRQs pinned away from DPDK-isolated cores? " +
    "Shows both the configured smp_affinity and the kernel's effective_affinity (actual delivery CPU). " +
    "For runtime IRQ COUNT distribution, use irq_distribution instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      irq_or_regex: z.string().describe("IRQ number or regex to match IRQ names"),
      include_effective: z.boolean().default(true).describe("Include effective affinity"),
    },
    async (params) => {
      const result = await irqAffinity(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── nic_irq_queue_map ─────────────────────────────────────────────
  server.tool(
    "nic_irq_queue_map",
    "Map NIC hardware queues → IRQ numbers → CPU cores for a network interface. Shows RSS (Receive Side Scaling) " +
    "queue distribution and which core handles each queue's interrupts. " +
    "Use when diagnosing uneven packet processing (one core overloaded while others idle) or verifying " +
    "NIC IRQ pinning after reconfiguration. Input: interface name (e.g., 'ens255np0').",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      iface: z.string().describe("Network interface name (e.g., eth0, ens5)"),
    },
    async (params) => {
      const result = await nicIrqQueueMap(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── softirq_snapshot ──────────────────────────────────────────────
  server.tool(
    "softirq_snapshot",
    "Delta parse /proc/softirqs per CPU over a window. Shows NET_RX, NET_TX, TIMER, SCHED, RCU, etc. counts per core. " +
    "Use when investigating unexpected CPU overhead on cores: high NET_RX on an isolated DPDK core means NIC interrupt " +
    "processing is landing there; high RCU means rcu_nocbs isn't effective. " +
    "Complements irq_distribution (hard IRQs) — softirqs are deferred work triggered by hard IRQs.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(100).max(10000).default(1000).describe("Observation window in ms"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      top_n: z.number().int().min(1).max(1000).optional().describe("Return only the N CPUs with highest softirq activity. Omit for all CPUs."),
    },
    async (params) => {
      const result = await softirqSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── kernel_threads_on_core ────────────────────────────────────────
  server.tool(
    "kernel_threads_on_core",
    "List kernel threads observed on a specific CPU: ksoftirqd, rcu_preempt, migration, watchdog, kworker, etc. " +
    "Use to verify isolated DPDK cores are clean — only the expected DPDK scheduler thread should appear. " +
    "If kernel threads like ksoftirqd or rcu are running on an isolated core, it indicates isolation config issues. " +
    "More specific than running_on_core (which shows all threads including userspace).",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      cpu: z.number().int().min(0).describe("CPU core number"),
      window_ms: z.number().min(100).max(10000).default(1000).describe("Observation window in ms"),
      top_n: z.number().int().min(1).max(200).default(30).describe("Max tasks to return"),
    },
    async (params) => {
      const result = await kernelThreadsOnCore(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── runqueue_snapshot ─────────────────────────────────────────────
  server.tool(
    "runqueue_snapshot",
    "System load averages, total runnable task count, and per-CPU scheduling stats from /proc/schedstat. " +
    "An isolated DPDK core should show exactly 1 runnable task (the poll-mode thread). " +
    "Use for a fast O(1) system-wide overload check — no sampling window needed (instant snapshot). " +
    "For detailed per-task attribution on a specific core, use running_on_core instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
    },
    async (params) => {
      const result = await runqueueSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── context_switch_rate ───────────────────────────────────────────
  server.tool(
    "context_switch_rate",
    "Measure global and per-CPU context switch rate (switches/sec) over a sampling window. " +
    "On isolated DPDK poll-mode cores, context switches should be near zero — any significant rate means " +
    "the core is being time-shared with other tasks, hurting deterministic latency. " +
    "High global rates suggest general CPU contention or too many runnable tasks per core.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(100).max(10000).default(1000).describe("Observation window in ms"),
    },
    async (params) => {
      const result = await contextSwitchRate(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── isolation_summary ─────────────────────────────────────────────
  server.tool(
    "isolation_summary",
    "Summarize CPU isolation configuration: isolcpus, nohz_full, rcu_nocbs from /proc/cmdline " +
    "and detect misconfigurations (e.g., isolated cores not in nohz_full, or core 0 accidentally isolated). " +
    "Use to verify GRUB boot parameters match intended DPDK core isolation after system changes or reboots. " +
    "Cross-references all three lists for consistency.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
    },
    async (params) => {
      const result = await isolationSummary(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── cpu_frequency_snapshot ────────────────────────────────────────
  server.tool(
    "cpu_frequency_snapshot",
    "Per-CPU current frequency, governor policy, and min/max scaling limits from cpufreq sysfs. " +
    "Instant snapshot, no sampling window. Use for a quick frequency check. " +
    "For deeper MSR-based frequency analysis (actual MPERF/APERF, C-state residency, IPC, temperature), use turbostat_snapshot. " +
    "Useful when turbostat is unavailable or you only need governor/scaling settings.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
    },
    async (params) => {
      const result = await cpuFrequencySnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── throttling_summary ────────────────────────────────────────────
  server.tool(
    "throttling_summary",
    "Detect CPU throttling from multiple sources: thermal throttling (PROCHOT), " +
    "Intel pstate driver limits, RT bandwidth throttling (sched_rt_runtime), and frequency governor constraints. " +
    "Use when cores show unexpectedly low throughput or IPC despite low utilization — throttling silently reduces " +
    "clock speed. For per-core thermal details and C-state residency, use turbostat_snapshot instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(100).max(10000).default(1000).optional().describe("Observation window in ms"),
    },
    async (params) => {
      const result = await throttlingSummary(params ?? {});
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── cgroup_cpu_limits ─────────────────────────────────────────────
  server.tool(
    "cgroup_cpu_limits",
    "Show cgroup v2 CPU quotas, weights, throttle counters, and cpuset CPU restrictions for a specific process. " +
    "Detects container/systemd CPU limits that silently cap throughput (e.g., cpu.max quota, cpuset pinning). " +
    "Use when a process runs slower than expected and isn't bottlenecked on CPU load or memory — " +
    "cgroup throttling is an invisible limiter. Shows nr_throttled and throttled_usec counters.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      target_pid: z.number().int().min(1).describe("Target process ID"),
    },
    async (params) => {
      const result = await cgroupCpuLimits(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── numa_topology ─────────────────────────────────────────────────
  server.tool(
    "numa_topology",
    "NUMA node layout: CPUs per node, memory per node (total/free), and inter-node distance matrix. " +
    "Use when planning process-to-NUMA placement or diagnosing cross-NUMA memory access penalties. " +
    "Shows which cores belong to which NUMA node — essential for verifying TX processes on NUMA 0 and RX on NUMA 1. " +
    "For checking a specific process's NUMA placement, use process_numa_placement instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
    },
    async (params) => {
      const result = await numaTopology(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── process_numa_placement ────────────────────────────────────────
  server.tool(
    "process_numa_placement",
    "Show NUMA placement for a specific process: allowed CPUs, allowed memory nodes (from numactl/cgroup), and " +
    "cross-node warnings if the process is touching memory on the wrong NUMA node. " +
    "Use when a process shows unexpectedly high memory latency — it may be allocated on the wrong NUMA node. " +
    "For the system-wide NUMA layout, use numa_topology; for memory bandwidth measurement, use pcm_numa_traffic.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      target_pid: z.number().int().min(1).describe("Target process ID"),
    },
    async (params) => {
      const result = await processNumaPlacement(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── perf_debug_snapshot ───────────────────────────────────────────
  server.tool(
    "perf_debug_snapshot",
    "All-in-one performance snapshot: per-core CPU loads, running tasks per core, IRQ/softirq distribution, " +
    "CPU frequency, isolation config, and optional cgroup limits for a focus PID. " +
    "Use as your FIRST diagnostic tool for general CPU performance triaging — covers the most common checks in one call. " +
    "Use 'collectors' to limit which sub-tools run (reduces output size and latency). " +
    "For deeper analysis, follow up with targeted tools (turbostat_snapshot for MSR data, pcm_core_counters for IPC, " +
    "starvation_report for specific thread analysis).",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      window_ms: z.number().min(100).max(10000).default(2000).describe("Observation window in ms"),
      focus_cpu: z.number().int().min(0).optional().describe("Focus on a specific CPU"),
      focus_target: z.number().int().min(1).optional().describe("Focus PID for cgroup analysis"),
      collectors: z.array(z.enum(["core_load", "running_on_cores", "irq_distribution", "softirq_snapshot", "cpu_frequency", "isolation", "pcm"])).optional().describe(
        "Sub-tools to include. Empty/omit for all. Options: core_load, running_on_cores, irq_distribution, softirq_snapshot, cpu_frequency, isolation, pcm"
      ),
    },
    async (params) => {
      const result = await perfDebugSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ══════════════════════════════════════════════════════════════════
  // Intel PCM tools (require pcm-sensor-server running on port 9738)
  // ══════════════════════════════════════════════════════════════════

  // ── pcm_core_counters ───────────────────────────────────────────
  server.tool(
    "pcm_core_counters",
    "Per-core hardware performance counters from Intel PCM: IPC (instructions per cycle), instructions retired, " +
    "L2/L3 cache hits/misses, SMI count, active frequency, local/remote memory bandwidth per core. " +
    "Use when core_load_snapshot shows a busy core but you need to know WHY: low IPC = memory stalls, " +
    "high L3 miss = cache thrashing, non-zero SMI = firmware steals. " +
    "For cache-focused analysis (hit ratios, L3 occupancy), use pcm_cache_analysis. " +
    "For cross-NUMA traffic per core, use pcm_numa_traffic. Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds (1=last second, up to 30)"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      samples: z.number().int().min(1).max(20).default(1).describe("Number of samples to collect. When >1, returns multi_sample with mean/stddev/min/max per metric per core. Default 1 = single snapshot (backward compatible)."),
      interval_s: z.number().int().min(1).max(30).default(2).describe("Seconds between samples when samples>1 (default 2)."),
    },
    async (params) => {
      const result = await pcmCoreCounters(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_memory_bandwidth ────────────────────────────────────────
  server.tool(
    "pcm_memory_bandwidth",
    "Per-socket aggregate memory bandwidth from Intel PCM: DRAM reads/writes (bytes and MB/s), persistent memory, " +
    "memory controller IA/GT/IO request breakdown, NUMA local/remote ratio. " +
    "Use when investigating memory-bandwidth saturation (e.g., multiple DPDK processes competing for DRAM). " +
    "For per-CORE memory traffic (which core is remote-heavy?), use pcm_numa_traffic instead. " +
    "Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
    },
    async (params) => {
      const result = await pcmMemoryBandwidth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_cache_analysis ──────────────────────────────────────────
  server.tool(
    "pcm_cache_analysis",
    "Per-core L2/L3 cache hit/miss analysis from Intel PCM: hit ratios, miss rates, L3 occupancy (bytes), " +
    "and system-wide aggregates. Use when pcm_core_counters shows low IPC and you suspect cache thrashing. " +
    "L2 miss + L3 hit = working set fits in L3; L3 miss = working set overflows to DRAM. " +
    "For per-PROCESS LLC stats (BCC-based), use llc_stat instead. Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      samples: z.number().int().min(1).max(20).default(1).describe("Number of samples to collect. When >1, returns multi_sample with mean/stddev/min/max per metric per core. Default 1 = single snapshot (backward compatible)."),
      interval_s: z.number().int().min(1).max(30).default(2).describe("Seconds between samples when samples>1 (default 2)."),
    },
    async (params) => {
      const result = await pcmCacheAnalysis(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_power_thermal ───────────────────────────────────────────
  server.tool(
    "pcm_power_thermal",
    "Power, thermal, and Top-Down Microarchitecture Analysis (TMA) from Intel PCM: per-core thermal headroom (°C to TjMax), " +
    "C-state residency, per-socket RAPL energy (package/PP0/PP1/DRAM Joules), TMA Level 1+2 bottleneck breakdown " +
    "(Frontend Bound, Backend Bound, Bad Speculation, Retiring). " +
    "Use when investigating WHERE cycles are wasted at the microarchitecture level. TMA is the most structured " +
    "approach to CPU bottleneck analysis. For simpler thermal/frequency checks, use turbostat_snapshot. " +
    "Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
      include_tma: z.boolean().default(true).describe("Include Top-Down Microarchitecture Analysis breakdown"),
    },
    async (params) => {
      const result = await pcmPowerThermal(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_qpi_upi_link ────────────────────────────────────────────
  server.tool(
    "pcm_qpi_upi_link",
    "QPI/UPI inter-socket link traffic and utilization from Intel PCM: per-link incoming/outgoing bytes and utilization %. " +
    "Only meaningful on multi-socket systems (2+ CPUs). Use when pcm_numa_traffic shows high remote memory access " +
    "and you need to check if the UPI interconnect is a bottleneck. High utilization (>70%) means cross-socket " +
    "traffic is saturating the link. Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
    },
    async (params) => {
      const result = await pcmQpiUpiLink(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_pcie_bandwidth ──────────────────────────────────────────
  server.tool(
    "pcm_pcie_bandwidth",
    "Aggregate PCIe/memory controller IO bandwidth from Intel PCM: IA (CPU-initiated), GT (GPU), and IO (PCIe/DMA) " +
    "requests per socket. NOTE: aggregate per socket, NOT per individual PCIe device. " +
    "Use when investigating whether total PCIe bandwidth is saturated across all NICs/devices on a socket. " +
    "For per-PORT PCIe traffic, use emon_collect E0 + emon_pcie_topology instead. Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
    },
    async (params) => {
      const result = await pcmPcieBandwidth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_numa_traffic ────────────────────────────────────────────
  server.tool(
    "pcm_numa_traffic",
    "Per-core and per-socket local vs remote memory bandwidth from Intel PCM, with local ratio percentage. " +
    "Identifies NUMA-unfriendly cores (local_ratio < 90% is suspicious, < 70% is critical for latency-sensitive DPDK). " +
    "Use when process_numa_placement shows the right membind but latency is still high — this shows ACTUAL traffic patterns. " +
    "For socket-aggregate memory bandwidth, use pcm_memory_bandwidth. Requires pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). PCM sensor server will be auto-started if not running."),
      seconds: z.number().int().min(1).max(30).default(1).describe("Delta window in seconds"),
      socket_filter: z.number().int().min(0).optional().describe("Filter to a specific socket"),
      core_filter: z.string().optional().describe("Filter to specific core IDs. Accepts: single ('4'), list ('4,5,6'), range ('4-13'), or mixed ('4-13,20,30-35'). Omit for all cores."),
      top_n: z.number().int().min(0).max(1000).default(0).describe("Return only top N cores by remote BW (0=all)"),
    },
    async (params) => {
      const result = await pcmNumaTraffic(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_latency ─────────────────────────────────────────────────
  server.tool(
    "pcm_latency",
    "DDR/PMM memory access latency per socket via pcm-latency CLI. " +
    "Shows average memory read latency in nanoseconds. " +
    "Use to diagnose memory latency issues independently from bandwidth. " +
    "Note: requires ALL CPU cores to be online (pcm-latency limitation). " +
    "For PMM (Optane) latency, set pmm=true. Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-latency locally or via SSH."),
      pmm: z.boolean().default(false).describe("Include PMM (Optane Persistent Memory) latency metrics"),
    },
    async (params) => {
      const result = await pcmLatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_accel ───────────────────────────────────────────────────
  server.tool(
    "pcm_accel",
    "Intel accelerator (IAA/DSA/QAT) bandwidth and request monitoring via pcm-accel CLI. " +
    "Shows per-accelerator-instance inbound/outbound bandwidth (B/s) and work queue request counts. " +
    "Available on Sapphire Rapids+ (4th Gen Xeon). IAA=analytics, DSA=data streaming, QAT=crypto/compression. " +
    "Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-accel locally or via SSH."),
      target: z.enum(["iaa", "dsa", "qat"]).default("iaa").describe("Accelerator type to monitor"),
    },
    async (params) => {
      const result = await pcmAccel(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_iio ─────────────────────────────────────────────────────
  server.tool(
    "pcm_iio",
    "Per-PCIe-IIO-stack I/O bandwidth monitoring via pcm-iio CLI. " +
    "Shows inbound (DMA) and outbound (MMIO) read/write bandwidth per PCIe stack and partition. " +
    "Much more granular than pcm_pcie_bandwidth (which is socket-aggregate). " +
    "Use to identify which PCIe device/stack is the bottleneck. " +
    "Includes IOMMU/IOTLB stats. Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-iio locally or via SSH."),
      non_zero_only: z.boolean().default(true).describe("Filter to only show stacks/parts with non-zero traffic"),
    },
    async (params) => {
      const result = await pcmIio(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_memory_per_channel ──────────────────────────────────────
  server.tool(
    "pcm_memory_per_channel",
    "Per-DDR-channel memory bandwidth via pcm-memory CLI. " +
    "Shows read/write MB/s for each physical DDR channel (Ch0-Ch7) per socket, plus PMM per-channel. " +
    "More granular than pcm_memory_bandwidth (which only shows socket-aggregate). " +
    "Use to detect imbalanced DIMM population, single-channel bottlenecks, or failed DIMMs. " +
    "Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-memory locally or via SSH."),
    },
    async (params) => {
      const result = await pcmMemoryPerChannel(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_bw_histogram ────────────────────────────────────────────
  server.tool(
    "pcm_bw_histogram",
    "Memory bandwidth utilization histogram via rapid pcm-memory sampling. " +
    "Runs pcm-memory at high frequency (default 50ms) for a specified duration, " +
    "then bins total memory bandwidth into 10 GB/s ranges per socket. " +
    "Reveals the DISTRIBUTION of bandwidth: bursty vs sustained, intermittent saturation. " +
    "Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-memory locally or via SSH."),
      duration_sec: z.number().min(1).max(60).default(2).describe("Measurement duration in seconds"),
      sample_interval_ms: z.number().min(5).max(1000).default(50).describe("Sampling interval in milliseconds"),
    },
    async (params) => {
      const result = await pcmBwHistogram(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pcm_tsx ─────────────────────────────────────────────────────
  server.tool(
    "pcm_tsx",
    "Intel TSX (Transactional Synchronization Extensions) monitoring via pcm-tsx CLI. " +
    "Reports per-core TSX transaction cycles, abort counts, and abort types (capacity/conflict). " +
    "Many CPUs have TSX disabled (post-TAA vulnerability); returns { supported: false } gracefully. " +
    "Does NOT use pcm-sensor-server.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Runs pcm-tsx locally or via SSH."),
    },
    async (params) => {
      const result = await pcmTsx(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════
  // Intel RDT (Resource Director Technology) tools
  // ═══════════════════════════════════════════════════════════════════

  // ── rdt_cache_monitoring ──────────────────────────────────────────
  server.tool(
    "rdt_cache_monitoring",
    "Per-core L3 cache occupancy (KB), LLC misses, IPC, and local/remote memory bandwidth via Intel RDT hardware counters (CMT + MBM). " +
    "More accurate than software LLC monitoring — uses dedicated Intel uncore hardware counters. " +
    "Shows which cores consume the most cache, detects noisy-neighbor pollution, and verifies CAT effectiveness. " +
    "Complementary to pcm_cache_analysis (which is socket-aggregate from PCM). Requires intel-cmt-cat + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(1).describe("Monitoring duration in seconds"),
      cores: z.string().default("0-55").describe("Core range to monitor (e.g., '0-55', '4,5,6', '0-111'). Default covers first socket."),
    },
    async (params) => {
      const result = await rdtCacheMonitoring(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── rdt_memory_bandwidth ─────────────────────────────────────────
  server.tool(
    "rdt_memory_bandwidth",
    "Per-core memory bandwidth (local + remote MB/s) via Intel RDT MBM hardware counters. " +
    "More accurate than software-based BW measurement — uses dedicated Intel uncore counters. " +
    "Shows per-core local vs remote BW to identify NUMA-unfriendly cores and bandwidth hogs. " +
    "Complementary to pcm_memory_bandwidth (socket-aggregate) and pcm_numa_traffic (IPC-derived). Requires intel-cmt-cat + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(1).describe("Monitoring duration in seconds"),
      cores: z.string().default("0-55").describe("Core range to monitor (e.g., '0-55', '4,5,6')"),
    },
    async (params) => {
      const result = await rdtMemoryBandwidth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── rdt_cache_allocation ─────────────────────────────────────────
  server.tool(
    "rdt_cache_allocation",
    "Show current L3 Cache Allocation Technology (CAT) configuration from Intel RDT. " +
    "Reports per-socket COS (Class of Service) definitions with cache way bitmasks and " +
    "core-to-COS assignments. CAT divides L3 cache into ways and assigns subsets to COS classes — " +
    "cores in different COS classes get isolated cache partitions. " +
    "Default: all COS have all ways (no isolation). Read-only. Requires intel-cmt-cat + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
    },
    async (params) => {
      const result = await rdtCacheAllocation(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── rdt_mba_config ───────────────────────────────────────────────
  server.tool(
    "rdt_mba_config",
    "Show current Memory Bandwidth Allocation (MBA) configuration from Intel RDT. " +
    "Reports per-socket COS bandwidth percentage limits and core-to-COS assignments. " +
    "MBA throttles memory bandwidth per COS class — default 100% (no throttling). " +
    "Lower values restrict bandwidth to prevent memory-intensive tasks from starving latency-sensitive workloads. " +
    "Works with CAT for full resource isolation. Read-only. Requires intel-cmt-cat + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
    },
    async (params) => {
      const result = await rdtMbaConfig(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════
  // Methodology & Analysis tools
  // ═══════════════════════════════════════════════════════════════════

  // ── use_method_checklist ─────────────────────────────────────────
  server.tool(
    "use_method_checklist",
    "Brendan Gregg USE Method automated health check. For every resource (CPU, Memory, Network, Disk) auto-checks " +
    "Utilization, Saturation, and Errors using /proc/stat, /proc/meminfo, /proc/net/dev, /proc/diskstats, " +
    "PSI (/proc/pressure/*), and /proc/vmstat. Returns a prioritized list of bottlenecks found. " +
    "Call as a first-pass diagnostic to quickly identify which resource is the bottleneck before drilling deeper.",
    {
      host: z.string().default("localhost").describe("Target host"),
      sample_ms: z.number().min(100).max(5000).default(500).describe("Sampling window in ms for rate metrics (CPU util, NIC throughput)"),
    },
    async (params) => {
      const result = await useMethodChecklist(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── ipc_analysis ────────────────────────────────────────────────
  server.tool(
    "ipc_analysis",
    "Instructions Per Cycle analysis via perf stat hardware counters. The key insight from Brendan Gregg's 'CPU Utilization is Wrong': " +
    "IPC < 1.0 means the CPU is memory-stalled (cycles wasted waiting for cache/memory), IPC ≥ 1.0 means instruction-bound (doing useful work). " +
    "Also reports cache miss rate and branch misprediction rate. Use to determine if high CPU utilization is real computation or just stalling.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(2).describe("Measurement duration"),
      cores: z.string().default("0-55").describe("Core range (e.g., '0-55', '4,5,6')"),
      pid: z.number().optional().describe("Target specific PID instead of cores"),
    },
    async (params) => {
      const result = await ipcAnalysis(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── stall_cycle_breakdown ───────────────────────────────────────
  server.tool(
    "stall_cycle_breakdown",
    "Break down CPU utilization into instruction-retired cycles vs memory-stalled cycles using PMU cycle_activity events. " +
    "Shows true CPU efficiency: what fraction of cycles are doing useful work vs stalled on L1/L2/L3 cache misses. " +
    "If stalls_total is 70% of cycles, 70% of reported CPU utilization is the CPU waiting, not computing. " +
    "Requires Intel CPU with cycle_activity events.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(2).describe("Measurement duration"),
      cores: z.string().default("0-55").describe("Core range"),
      pid: z.number().optional().describe("Target specific PID instead of cores"),
    },
    async (params) => {
      const result = await stallCycleBreakdown(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── tma_analysis ────────────────────────────────────────────────
  server.tool(
    "tma_analysis",
    "Intel Top-Down Microarchitecture Analysis via perf stat --topdown. The gold standard for CPU bottleneck classification. " +
    "Level 1: Retiring (useful) vs Backend Bound (stalls) vs Frontend Bound (fetch) vs Bad Speculation (wasted). " +
    "Level 2: Memory Bound vs Core Bound, Fetch Latency vs Bandwidth, Branch Mispredicts vs Machine Clears. " +
    "Level 3: L1/L2/L3/DRAM bound, store-bound, port utilization, etc. Requires Intel CPU and perf 5.x+.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(2).describe("Measurement duration"),
      cores: z.string().default("0-55").describe("Core range"),
      level: z.number().min(1).max(3).default(2).describe("TMA depth: 1=top-level, 2=sub-categories, 3=full detail"),
      pid: z.number().optional().describe("Target specific PID instead of cores"),
    },
    async (params) => {
      const result = await tmaAnalysis(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── snapshot_diff ───────────────────────────────────────────────
  server.tool(
    "snapshot_diff",
    "A/B comparison: captures two correlated snapshots separated by an interval and computes deltas for all numeric metrics. " +
    "Essential for measuring impact of configuration changes, load shifts, or tuning (e.g., before/after affinity change). " +
    "Uses the correlated_snapshot infrastructure. Compare core_load, irq_distribution, softirq, context_switches, cpu_frequency.",
    {
      host: z.string().default("localhost").describe("Target host"),
      interval_sec: z.number().min(1).max(300).default(5).describe("Seconds between snapshot A and B"),
      collectors: z.array(z.string()).default([]).describe("Collectors to compare (default: all diffable: core_load, irq_distribution, softirq_snapshot, context_switch_rate, cpu_frequency)"),
      core_filter: z.string().optional().describe("Core filter for per-core metrics"),
      label_before: z.string().default("before").describe("Label for first snapshot"),
      label_after: z.string().default("after").describe("Label for second snapshot"),
    },
    async (params) => {
      const result = await snapshotDiff(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── off_cpu_analysis ────────────────────────────────────────────
  server.tool(
    "off_cpu_analysis",
    "Per-task scheduling latency analysis via /proc/*/schedstat delta snapshots. " +
    "Shows which tasks experience the worst scheduling delays: runtime, context switches, avg/max scheduling latency. " +
    "Complementary to offcpu_time (BCC) which shows per-stack off-CPU time — this tool gives per-TASK aggregate view. " +
    "Works without perf sched, BCC, or root — only needs readable /proc. " +
    "Use to identify tasks starved by scheduling or waiting in run queue.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(5).describe("Recording duration"),
      cores: z.string().default("0-55").describe("Core range to trace"),
      top_n: z.number().min(1).max(200).default(30).describe("Number of top tasks to return"),
    },
    async (params) => {
      const result = await offCpuAnalysis(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── dpdk_telemetry_deep ─────────────────────────────────────────
  server.tool(
    "dpdk_telemetry_deep",
    "Extended DPDK telemetry: per-queue xstats (hundreds of NIC-specific counters invisible to kernel tools), " +
    "detailed error/drop counters, and ring buffer occupancy. Goes deeper than mtl_dpdk_telemetry which covers " +
    "basic ethdev stats and mempool. Use when investigating NIC-level packet drops, per-queue imbalances, " +
    "or flow director issues in DPDK/MTL workloads. Requires running DPDK application with telemetry enabled.",
    {
      host: z.string().default("localhost").describe("Target host"),
      dpdk_base_dir: z.string().default("/var/run/dpdk").describe("Base directory for DPDK runtime sockets"),
      file_prefix: z.string().default("MT_DPDK").describe("DPDK EAL file prefix"),
      filter: z.string().default("").describe("Filter xstat names containing this string (e.g., 'error', 'rx_q0')"),
    },
    async (params) => {
      const result = await dpdkTelemetryDeep(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── working_set_size ────────────────────────────────────────────
  server.tool(
    "working_set_size",
    "Estimate process Working Set Size (WSS) by clearing page table Referenced bits, waiting, then reading " +
    "which pages were accessed. If WSS > LLC size, expect cache misses. If WSS > NUMA node memory, expect " +
    "cross-socket penalty. WARNING: writing to clear_refs briefly impacts TLB for the target process — " +
    "use with care on latency-critical production workloads. Based on Brendan Gregg's wss.pl methodology.",
    {
      host: z.string().default("localhost").describe("Target host"),
      pid: z.number().describe("Target process PID (required)"),
      window_sec: z.number().min(0.1).max(60).default(1).describe("Measurement window in seconds"),
    },
    async (params) => {
      const result = await workingSetSize(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════
  // MTL (Media Transport Library) tools
  // ═══════════════════════════════════════════════════════════════════

  // ── mtl_manager_status ──────────────────────────────────────────
  server.tool(
    "mtl_manager_status",
    "Check MtlManager daemon status: running/stopped, version, Unix socket path, and per-client lcore allocations. " +
    "MtlManager coordinates lcore usage across multiple MTL instances. " +
    "Use when processes fail to start (MtlManager may be down or have stale state), or to verify which " +
    "clients are connected and their lcore grants. For the raw SysV shared memory lcore map, use mtl_lcore_shm.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Use 'localhost' for local execution."),
      socket_path: z.string().default("/var/run/imtl/mtl_manager.sock").describe("Path to MtlManager Unix socket"),
      log_path: z.string().optional().describe("Path to MtlManager log file (auto-detected if omitted)"),
    },
    async (params) => {
      const result = await mtlManagerStatus(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_dpdk_telemetry ──────────────────────────────────────────
  server.tool(
    "mtl_dpdk_telemetry",
    "Query DPDK telemetry v2 sockets for live internal state: ethdev stats (rx/tx packets, bytes, errors per DPDK port), " +
    "ethdev info (driver, speed, MTU, RSS config), mempool occupancy, memory heap usage, and EAL parameters. " +
    "Auto-discovers all running DPDK instances via /var/run/dpdk. " +
    "Use when investigating DPDK-level issues invisible to kernel tools: port-level drops, mempool exhaustion, " +
    "or verifying DPDK port configuration. Different from nic_ethtool_stats (kernel/ethtool) — this reads DPDK's own counters.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      dpdk_base_dir: z.string().default("/var/run/dpdk").describe("Base directory for DPDK runtime sockets"),
      file_prefix: z.string().default("MT_DPDK").describe("DPDK file prefix (for MTL: MT_DPDK)"),
      endpoints: z.array(z.enum(["ethdev_stats", "ethdev_info", "mempool", "heap", "eal"]))
        .default(["ethdev_stats", "ethdev_info"])
        .describe("Which DPDK telemetry endpoints to query"),
    },
    async (params) => {
      const result = await mtlDpdkTelemetry(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_instance_processes ──────────────────────────────────────
  server.tool(
    "mtl_instance_processes",
    "Scan for all running MTL/DPDK processes and their threads: PID, thread names, CPU affinity masks, " +
    "scheduling policy, and voluntary/involuntary context switches. " +
    "Use as a FIRST STEP to discover running pipeline processes and their PIDs before deeper per-process analysis " +
    "(e.g., starvation_report, process_numa_placement, offcpu_time all need a PID). " +
    "Also shows whether thread affinity matches expected DPDK lcore assignments.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      process_regex: z.string().default("MtlManager|mtl_|dpdk-").describe("Regex to match MTL-related process/thread names"),
    },
    async (params) => {
      const result = await mtlInstanceProcesses(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_session_stats ───────────────────────────────────────────
  server.tool(
    "mtl_session_stats",
    "Parse MTL stat dumps from LOG FILES (DEV STATE blocks) to extract throughput, scheduler timings, CNI stats, PTP timestamps, and per-session video stats (fps, Mbps, cpu_busy). " +
    "LOG-ONLY: reads from disk files. For LIVE capture prefer mtl_usdt_session_stats which also activates timing probes. " +
    "IMPORTANT: Only uses the last N dumps (default 5) to avoid startup transients that corrupt A/B test comparisons. " +
    "Returns per-session aggregated stats (mean/stddev/min/max for FPS, throughput, cpu_busy), FPS trend across steady-state dumps, and detects TX build timeouts.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname). Use 'localhost' for local execution."),
      log_path: z.string().describe("Path to MTL log file containing stat dumps (e.g. /dev/shm/kahawai_log_*.log)"),
      tail_lines: z.number().int().min(50).max(50000).default(500).describe("Number of lines from end of file to parse (default 500 ≈ 25 dumps)"),
      last_dumps: z.number().int().min(1).max(100).default(5).describe(
        "Use only the LAST N stat dumps for statistics, discarding earlier startup transients. Default 5 = ~50s of steady-state data."
      ),
      alert_threshold_fps: z.number().optional().describe(
        "If set, generate warnings when session FPS drops below this threshold"
      ),
      session_filter: z.string().optional().describe(
        "Filter to a specific session by name (e.g. 'video_0'). Only matching sessions are included in aggregation."
      ),
    },
    async (params) => {
      const result = await mtlSessionStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_app_latency ─────────────────────────────────────────────
  server.tool(
    "mtl_app_latency",
    "Parse application-level JSON latency samples from log files. Extracts per-stage latency buckets written by the " +
    "poc/poc_16 pipeline applications (e.g., rdma_latency_us, e2e_latency_us, sender/receiver breakdown). " +
    "Different from mtl_session_stats (which parses MTL library stat dumps for FPS/throughput) — this reads " +
    "APPLICATION-emitted JSON containing end-to-end and RDMA latency measurements. " +
    "Use when investigating latency spikes in the RDMA pipeline, not MTL-internal timing.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      log_path: z.string().describe("Path to log file containing JSON latency lines (e.g. /dev/shm/*.log)"),
      tail_lines: z.number().int().min(10).max(10000).default(100).describe("Number of lines from end to search for JSON"),
      role_filter: z.enum(["sender", "receiver"]).optional().describe("Only return samples for this role"),
    },
    async (params) => {
      const result = await mtlAppLatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_live_stats ──────────────────────────────────────────────
  server.tool(
    "mtl_live_stats",
    "Read a single JSON stats file from a running MTL application (e.g., /dev/shm/poc_stat.json, /dev/shm/poc16_stat.json). " +
    "These files are continuously updated by pipeline processes and contain live key-value metrics: FPS, throughput, " +
    "latency samples, RDMA connection state, frame counts, drop counts. " +
    "Use for an instant snapshot of a running process without parsing logs. " +
    "For historical trend analysis across multiple stat dumps, use mtl_session_stats instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      stats_path: z.string().describe("Full path to the JSON stats file"),
    },
    async (params) => {
      const result = await mtlLiveStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_nic_pf_stats ───────────────────────────────────────────
  server.tool(
    "mtl_nic_pf_stats",
    "Collect NIC Physical Function (PF) stats via ethtool: driver version, link speed/state, and ALL hardware counters. " +
    "Similar to nic_ethtool_stats but also returns driver info and link details in one call. " +
    "Use for MTL pipeline NICs (e.g., ens1np0, ens255np0, enp171s0np0) to check for packet drops, errors, or bandwidth saturation. " +
    "For delta/rate mode (Gbps), use nic_ethtool_stats with seconds>0 instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      interface: z.string().describe("Network interface name (e.g. ens801f0)"),
      filter: z.string().optional().describe("Regex to filter stat names (e.g. 'rx_bytes|tx_bytes|error')"),
    },
    async (params) => {
      const result = await mtlNicPfStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_hugepage_usage ──────────────────────────────────────────
  server.tool(
    "mtl_hugepage_usage",
    "Report hugepage allocation and usage: total/free/in-use for each page size (2MB, 1GB), plus DPDK hugepage file count in /dev/hugepages. " +
    "Use when DPDK processes fail to start (EAL init errors are often hugepage exhaustion) or when memory usage " +
    "seems abnormal. Check this before and after starting pipeline processes to verify allocation.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      hugepage_dir: z.string().default("/dev/hugepages").describe("Hugepage mount directory"),
    },
    async (params) => {
      const result = await mtlHugepageUsage(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_lcore_shm ──────────────────────────────────────────────
  server.tool(
    "mtl_lcore_shm",
    "Read MTL SysV shared memory segment that tracks lcore allocation across instances. Shows which lcores are claimed, " +
    "by whom (PID, comm, hostname), and detects conflicts (two processes claiming the same lcore). " +
    "Use when DPDK processes fail to start with 'no available lcore' errors, or to verify lcore assignments " +
    "match machine_env.sh expectations. Also useful after a crash to check for stale claims from dead processes. " +
    "For thread-level affinity verification, use mtl_instance_processes instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      ftok_path: z.string().default("/dev/null").describe("Path used for ftok key generation"),
      ftok_proj_id: z.number().int().default(21).describe("Project ID used for ftok"),
    },
    async (params) => {
      const result = await mtlLcoreShm(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_influxdb_query ──────────────────────────────────────────
  server.tool(
    "mtl_influxdb_query",
    "Query InfluxDB v2 for HISTORICAL time-series metrics: NIC throughput, latency percentiles, RDMA counters, VF stats. " +
    "Executes a Flux query and returns parsed rows. Use for trend analysis over minutes/hours (e.g., 'did throughput degrade " +
    "over the last hour?') or A/B comparisons (aggregate=true computes mean/stddev/min/max per field). " +
    "Data is pushed to InfluxDB by vf_stats_collector.py and pipeline processes. " +
    "For LIVE instant snapshots, use mtl_live_stats or mtl_session_stats instead.",
    {
      influxdb_host: z.string().default("localhost").describe("InfluxDB host (IP or hostname)"),
      port: z.number().int().default(8086).describe("InfluxDB port"),
      org: z.string().default("").describe("InfluxDB organization"),
      bucket: z.string().describe("InfluxDB bucket name"),
      measurement: z.string().describe("InfluxDB measurement name"),
      token: z.string().default("").describe("InfluxDB auth token (empty for no auth)"),
      range: z.string().default("-5m").describe("Flux time range (e.g. '-5m', '-1h')"),
      field_filter: z.string().optional().describe("Regex to filter field names"),
      ssh_host: z.string().optional().describe("If set, run curl from this host via SSH (for firewalled InfluxDB)"),
      limit: z.number().int().min(1).max(10000).default(100).describe("Max rows to return"),
      aggregate: z.boolean().default(false).describe("When true, compute mean/stddev/min/max per field+tag group across all returned points. Useful for A/B comparisons."),
    },
    async (params) => {
      const result = await mtlInfluxdbQuery(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ══════════════════════════════════════════════════════════════════
  // Intel EMON tools (require EMON/SEP installed with drivers loaded)
  // ══════════════════════════════════════════════════════════════════

  // ── emon_capabilities ──────────────────────────────────────────
  server.tool(
    "emon_capabilities",
    "Return EMON availability, version, loaded SEP kernel drivers (pax, sep5, socperf3, vtss_ppt), supported PMU types, " +
    "and available presets (E0–E4). Call this FIRST before using emon_collect or emon_triage to verify drivers are loaded. " +
    "If drivers are missing, the tool reports how to load them. EMON provides deeper uncore/mesh analysis than PCM.",
    {},
    async () => {
      const result = await emonCapabilities();
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── emon_collect ───────────────────────────────────────────────
  server.tool(
    "emon_collect",
    "Run a single EMON preset collection for targeted uncore/mesh analysis. Returns structured per-unit PMU counter data. " +
    "Presets: E0_iio_pcie_per_port (PCIe bandwidth per IIO stack/port — use with emon_pcie_topology), " +
    "E1_cha_llc_snoop (LLC hit/miss per CHA — deeper than pcm_cache_analysis), " +
    "E2_mesh_stall_latency (memory latency via CHA+M2M stall cycles), " +
    "E3_upi_detailed (per-link UPI — deeper than pcm_qpi_upi_link), " +
    "E4_core_stall_deep (per-CPU TMA + execution — deeper than pcm_power_thermal TMA). " +
    "Use when you need a SPECIFIC preset. For automatic cascading analysis, use emon_triage instead. " +
    "Requires EMON with SEP drivers (check emon_capabilities first).",
    {
      preset: z.enum([
        "E0_iio_pcie_per_port",
        "E1_cha_llc_snoop",
        "E2_mesh_stall_latency",
        "E3_upi_detailed",
        "E4_core_stall_deep",
      ]).describe("EMON preset to run"),
      window_ms: z.number().min(100).max(30000).optional().describe("Collection window in ms (default: preset-specific)"),
      cpu_filter: z.array(z.number().int().min(0)).optional().describe("Limit core-event collection to specific CPUs"),
    },
    async (params) => {
      const result = await emonCollect(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── emon_triage ────────────────────────────────────────────────
  server.tool(
    "emon_triage",
    "Cascading waterfall triage: runs EMON presets E0→E1→E2→E3→E4 in order, each informing the next. " +
    "Returns structured diagnosis with severity, evidence, and recommendations. " +
    "Use this as the PRIMARY EMON entry point — it automatically sequences the right analysis. " +
    "For a single targeted preset (e.g., only PCIe bandwidth), use emon_collect with a specific preset instead. " +
    "Requires EMON with SEP drivers (check emon_capabilities first).",
    {
      focus_cpu: z.number().int().min(0).optional().describe("Focus analysis on a specific CPU"),
      window_ms: z.number().min(100).max(30000).default(1000).describe("Collection window per preset in ms"),
      quick: z.boolean().default(false).describe("Stop early if critical issues found"),
    },
    async (params) => {
      const result = await emonTriage(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── emon_pcie_topology ─────────────────────────────────────────
  server.tool(
    "emon_pcie_topology",
    "Map PCIe devices to IIO stacks via sysfs: BDF, driver, NUMA node, network interface, and IIO socket:stack:port for each device. " +
    "Use TOGETHER with emon_collect E0 to correlate per-IIO-stack bandwidth numbers with physical NIC/device identities. " +
    "Without this mapping, E0 output shows abstract stack IDs that can't be linked to specific NICs.",
    {},
    async () => {
      const result = await emonPcieTopology();
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_scheduler_map ──────────────────────────────────────────
  server.tool(
    "mtl_scheduler_map",
    "Map MTL scheduler-to-session assignments for all running instances from LOG FILES. Shows per-scheduler session count, lcore, quota utilization, " +
    "and warns about imbalanced session distribution. Use when investigating uneven CPU load across DPDK scheduler cores. " +
    "For LIVE per-tasklet timing (avg/max/min per scheduler and per tasklet), use mtl_usdt_tasklet_timing instead.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      log_paths: z.array(z.string()).optional().describe(
        "Explicit log file paths to check (e.g. ['/dev/shm/poc_8k_logs/compositor.log']). " +
        "If omitted, scans well-known /dev/shm/*_logs/ directories."
      ),
    },
    async (params) => {
      const result = await mtlSchedulerMap(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_list_probes ──────────────────────────────────────────
  server.tool(
    "mtl_usdt_list_probes",
    "Discovery tool: list all USDT probes available in the installed libmtl.so. " +
    "Groups probes by provider (sys, ptp, st20, st20p, st22, st22p, st30, st30p, st40, st40p, st41). " +
    "Call this first to verify USDT support is available before using other mtl_usdt_* tools. " +
    "82 probes across 11 providers on a standard MTL build. Requires bpftrace and root.",
    mtlUsdtProbesSchema.shape,
    async (params) => {
      const result = await mtlUsdtProbes(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_session_stats ─────────────────────────────────────────
  server.tool(
    "mtl_usdt_session_stats",
    "PREFERRED: Capture MTL session stats (FPS, throughput, drops, cpu_busy) with three-tier fallback: USDT → logs → InfluxDB. " +
    "USDT tier attaches to sys:log_msg + sys:tasklet_time_measure + sys:sessions_time_measure, capturing a " +
    "complete stat dump with ALL timing data activated. " +
    "Use this as the default 'get session stats' tool — it auto-detects the best data source. " +
    "Falls back to log parsing if USDT is unavailable (no bpftrace, not root, no running process), " +
    "then to InfluxDB as last resort. Use force_mode to bypass fallback chain. " +
    "For focused analysis, use specialized tools instead: mtl_usdt_tasklet_timing (scheduler bottleneck), " +
    "mtl_usdt_session_timing (per-session CPU), mtl_usdt_log_monitor (raw log messages).",
    mtlUsdtSessionStatsSchema.shape,
    async (params) => {
      const result = await mtlUsdtSessionStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_frame_trace ───────────────────────────────────────────
  server.tool(
    "mtl_usdt_frame_trace",
    "Real-time frame lifecycle tracing via USDT probes. Traces st20/st20p/st22/st22p frame events " +
    "(tx_frame_next→done, rx_frame_available→put) to compute LIVE FPS, inter-frame interval statistics, " +
    "and error counts (incomplete, no_framebuffer, drops). " +
    "Provides FRAME-LEVEL visibility impossible from stat dumps (which only report 10-second aggregates). " +
    "Use when investigating frame-level jitter, individual frame drops, or short-lived FPS dips " +
    "that are smoothed away in stat dump averages. " +
    "Returns per-stream breakdown with computed_fps, IFI avg/min/max/stddev, drop counts. " +
    "Requires bpftrace and root. Duration 1-30 seconds.",
    mtlUsdtFrameTraceSchema.shape,
    async (params) => {
      const result = await mtlUsdtFrameTrace(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_ptp_trace ────────────────────────────────────────────
  server.tool(
    "mtl_usdt_ptp_trace",
    "Trace PTP synchronization quality via the ptp:ptp_result USDT probe. Captures PTP offset samples " +
    "(delta_ns and correct_delta_ns) from the running MTL process. Computes statistics over the trace window: " +
    "avg/min/max/stddev of PTP delta. Use when investigating PTP sync issues, clock drift, timestamp jitter, " +
    "or inter-frame timing anomalies that may be caused by clock offset. " +
    "Requires bpftrace and root.",
    mtlUsdtPtpSchema.shape,
    async (params) => {
      const result = await mtlUsdtPtp(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_tasklet_timing ────────────────────────────────────────
  server.tool(
    "mtl_usdt_tasklet_timing",
    "ACTIVATION PROBE: Enable and capture per-scheduler and per-tasklet timing breakdown. " +
    "Attaches to sys:tasklet_time_measure USDT probe, which ACTIVATES timing that is normally DISABLED in production. " +
    "While attached, MTL reports in its next stat dump:" +
    "\n  - SCH(n): time avg/max/min (scheduler loop timing)" +
    "\n  - SCH(n,m): tasklet NAME, avg/max/min (per-tasklet breakdown)" +
    "\nUse when avg_loop_ns is high and you need to find WHICH TASKLET is the bottleneck in a scheduler. " +
    "This is the first tool to reach for after mtl_usdt_session_stats shows a problem — it tells you WHERE " +
    "in the scheduler pipeline the time is being spent. " +
    "For per-SESSION timing (which session is slow?), use mtl_usdt_session_timing instead. " +
    "SIDE EFFECT: Activates timing measurement in the target process (minor CPU overhead). " +
    "Requires bpftrace and root.",
    mtlUsdtTaskletTimingSchema.shape,
    async (params) => {
      const result = await mtlUsdtTaskletTiming(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_session_timing ────────────────────────────────────────
  server.tool(
    "mtl_usdt_session_timing",
    "ACTIVATION PROBE: Enable and capture per-session tasklet timing. " +
    "Attaches to sys:sessions_time_measure USDT probe, which ACTIVATES per-session timing that is " +
    "DISABLED in production. While attached, MTL reports in its next stat dump:" +
    "\n  - TX_VIDEO_SESSION(m,s): tasklet time avg/max/min" +
    "\n  - RX_VIDEO_SESSION(m,s): tasklet time avg/max/min" +
    "\nUse when you know WHICH SCHEDULER is slow (from mtl_usdt_tasklet_timing) and need to identify " +
    "WHICH SESSION within it is consuming the most CPU. " +
    "Complementary to mtl_usdt_tasklet_timing: tasklet_timing → 'which scheduler/tasklet?', " +
    "session_timing → 'which video session?' " +
    "SIDE EFFECT: Activates session timing measurement (minor CPU overhead). " +
    "Requires bpftrace and root.",
    mtlUsdtSessionTimingSchema.shape,
    async (params) => {
      const result = await mtlUsdtSessionTiming(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_cni_pcap ─────────────────────────────────────────────
  server.tool(
    "mtl_usdt_cni_pcap",
    "ACTIVATION PROBE: Trigger CNI (Control/Network/Interface) packet capture to pcap files. " +
    "Attaches to sys:cni_pcap_dump USDT probe, which ACTIVATES pcap dumping on the DPDK tasklet path. " +
    "While attached, MTL writes pcapng files (one per port, every ~10s) containing CNI packets. " +
    "Reports which files were created, port indices, and packet counts — files remain on disk for " +
    "offline analysis with Wireshark/tshark. " +
    "Use when debugging unexpected packets, routing issues, or ARP/IGMP behavior at the DPDK port level. " +
    "⚠ SIDE EFFECTS: (1) Creates pcap files on disk, (2) Runs on tasklet path — MAY IMPACT PERFORMANCE. " +
    "Keep duration short. Requires bpftrace and root.",
    mtlUsdtCniPcapSchema.shape,
    async (params) => {
      const result = await mtlUsdtCniPcap(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── mtl_usdt_log_monitor ──────────────────────────────────────────
  server.tool(
    "mtl_usdt_log_monitor",
    "READ-ONLY: Capture raw MTL log messages in real-time via USDT. " +
    "Attaches ONLY to sys:log_msg — NO timing activation, NO packet capture, NO side effects. " +
    "Captures ALL log messages regardless of the process's log-level setting (the USDT probe fires " +
    "on every log call BEFORE the level filter). " +
    "Use when: (1) You want to see what MTL is outputting without affecting performance, " +
    "(2) Log files aren't available or are filtered too aggressively, " +
    "(3) Quick sanity check: 'is this process producing output?' " +
    "Supports level filtering (error/warning/notice/info/debug) and pattern matching. " +
    "Unlike log_search (reads disk files), this captures messages directly from the running process. " +
    "Unlike mtl_usdt_session_stats (activates timing probes), this has zero side effects. " +
    "Requires bpftrace and root.",
    mtlUsdtLogMonitorSchema.shape,
    async (params) => {
      const result = await mtlUsdtLogMonitor(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── rdma_health ────────────────────────────────────────────────
  server.tool(
    "rdma_health",
    "Read RDMA device health: hardware error counters (retransmissions, ECN marks, CNP stats, protocol errors), " +
    "QP (Queue Pair) state enumeration, and throughput estimation. " +
    "Key counters: RetransSegs (packet retransmissions — should be 0 under PFC), RxECNMrkd (ECN congestion marks), " +
    "cnpSent/cnpHandled (DCQCN flow control), InOptErrors/InProtoErrors (protocol errors). " +
    "Use seconds>0 for delta mode (rates: writes/sec, retrans/sec, Gbps). " +
    "Use summary_only=true to suppress the full counter dump (saves output size). " +
    "Use when investigating RDMA throughput drops, congestion, or broken connections (QPs not in RTS state). " +
    "For traffic VOLUME counters (InRdmaWrites, cnpSent totals), use rdma_counters instead. " +
    "For NIC-level hardware drops, use nic_ethtool_stats. For PFC/ETS config verification, use dcb_status.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      device_filter: z.string().optional().describe("Regex to filter RDMA device names (e.g. 'rocep42s0')"),
      port_filter: z.array(z.number().int().min(1)).optional().describe("Filter to specific port numbers (e.g. [1] or [1,2]). Omit for all ports."),
      seconds: z.number().min(0).max(30).default(0).describe(
        "Delta window. 0 = single snapshot. >0 = two samples, compute rates (writes/sec, retrans/sec, Gbps)"
      ),
      include_qps: z.boolean().default(true).describe("Include Queue Pair state enumeration via rdma tool"),
      summary_only: z.boolean().default(false).describe("Omit all_counters array from output (returns only key counters, QP summary, warnings). Saves ~70% output size."),
    },
    async (params) => {
      const result = await rdmaHealth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── dcb_status ─────────────────────────────────────────────────
  server.tool(
    "dcb_status",
    "Read DCB (Data Center Bridging) configuration for network interfaces: PFC (Priority Flow Control) enabled priorities, " +
    "ETS (Enhanced Transmission Selection) bandwidth allocation, link MTU/state/speed, and optionally VF trust/link-state. " +
    "Auto-discovers RDMA-capable interfaces if none specified. " +
    "Use when verifying PFC/ETS config hasn't drifted after reboots or NIC resets, when diagnosing RoCEv2 congestion " +
    "(PFC should be on priority 3 for RDMA NICs), or when checking MTU (must be 9000 for jumbo frames). " +
    "For runtime PFC pause COUNTS, check the Arista switch (switch_show_pfc) or nic_ethtool_stats with filter 'pfc'.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      interfaces: z.array(z.string()).optional().describe(
        "Interface names to check (e.g. ['ens255np0', 'enp171s0np0']). Omit to auto-discover RDMA interfaces."
      ),
      include_vf_info: z.boolean().default(false).describe("Include SR-IOV VF configuration (trust, link-state, MAC)"),
    },
    async (params) => {
      const result = await dcbStatus(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── nic_ethtool_stats ──────────────────────────────────────────
  server.tool(
    "nic_ethtool_stats",
    "Read NIC hardware counters via ethtool -S: traffic (rx/tx bytes, packets), errors (CRC, oversize, timeout), " +
    "drops, and optionally per-queue counters. Works with any Linux NIC driver (ice, mlx5, igc, etc.). " +
    "Use seconds>0 for DELTA MODE with rate computation (bytes/sec, pps, Gbps) — essential for measuring live throughput. " +
    "Filters zero-valued counters by default (non_zero_only=true) to reduce noise. " +
    "For PF snapshot with driver/link info, use mtl_nic_pf_stats. For per-VF breakdown, use nic_vf_stats. " +
    "For DPDK-internal counters (invisible to ethtool), use mtl_dpdk_telemetry.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      interface: z.string().describe("Network interface name (e.g. 'ens255np0', 'eth0')"),
      seconds: z.number().min(0).max(30).default(0).describe(
        "Delta window. 0 = snapshot. >0 = two samples with rate computation (bytes/sec, pps, Gbps)"
      ),
      include_queues: z.boolean().default(false).describe("Include per-queue tx/rx packet and byte counters"),
      filter: z.string().optional().describe("Regex to filter counter names (e.g. 'rx_drop|crc|error')"),
      non_zero_only: z.boolean().default(true).describe(
        "If true (default), suppress counters where both value and delta are zero"
      ),
    },
    async (params) => {
      const result = await nicEthtoolStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── pipeline_health ────────────────────────────────────────────
  server.tool(
    "pipeline_health",
    "Composite health check for MTL/RDMA media pipelines. Discovers running MTL instances, checks process liveness, " +
    "parses FPS and drops from log stat dumps and SHM JSON stats files, verifies thumbnail freshness, and probes " +
    "HTTP stats endpoints. Enriches with LIVE USDT FPS when bpftrace is available (attaches to running processes for ~5s). " +
    "Scans logs for real errors (filtering out informational MTL stat dump lines). " +
    "Use as the FIRST diagnostic tool when starting a debugging session — gives a complete pipeline overview in one call. " +
    "For deeper per-session analysis, follow up with mtl_usdt_session_stats or mtl_usdt_tasklet_timing.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      log_dirs: z.array(z.string()).optional().describe(
        "Directories to scan for pipeline log files. Defaults to /dev/shm/*_logs/ directories."
      ),
      shm_stats_pattern: z.string().default("/dev/shm/*stats*.json").describe(
        "Glob pattern for SHM JSON stats files written by pipeline processes"
      ),
      thumbnail_dirs: z.array(z.string()).optional().describe(
        "Directories containing thumbnail JPEGs. Defaults to /dev/shm/*_thumbs*/ directories."
      ),
      http_endpoints: z.array(z.object({
        url: z.string().describe("HTTP URL to check"),
        label: z.string().describe("Human-readable label for the endpoint"),
      })).optional().describe(
        "HTTP endpoints to probe for stats JSON. If omitted, discovers by scanning common ports (8081-8089, 9916, 9999)."
      ),
      max_thumbnail_age_sec: z.number().default(5).describe("Warn if any thumbnail JPEG is older than this many seconds"),
      max_log_errors_window_min: z.number().default(5).describe("Look back this many minutes in logs for errors"),
    },
    async (params) => {
      const result = await pipelineHealth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── log_search ─────────────────────────────────────────────────
  server.tool(
    "log_search",
    "Search pipeline log files for errors, warnings, and patterns with structured deduplication. " +
    "Distinguishes real errors from MTL informational stat dump lines (which use 'Error:' prefix for NIC counter reports). " +
    "Returns timestamped, classified, and optionally deduplicated matches. " +
    "Use when investigating pipeline failures, drops, or behavioral anomalies.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      log_dirs: z.array(z.string()).optional().describe(
        "Directories to search for *.log files. Defaults to /dev/shm/*_logs/ directories."
      ),
      log_paths: z.array(z.string()).optional().describe("Explicit log file paths to search"),
      pattern: z.string().optional().describe(
        "Regex search pattern (case-insensitive). Defaults to 'fail|timeout|abort|fatal|panic|exception|refused|broken|segfault'"
      ),
      minutes: z.number().min(1).max(1440).default(5).describe("Look back this many minutes from now"),
      severity: z.enum(["error", "warning", "all"]).default("error").describe(
        "'error' = real errors only, 'warning' = errors + warnings, 'all' = all matches (including stat dumps)"
      ),
      exclude_stat_dumps: z.boolean().default(true).describe(
        "Filter out MTL stat dump lines containing 'Error:' that are NIC counter reports, not real errors"
      ),
      max_results: z.number().min(1).max(500).default(50).describe("Maximum matches to return"),
      dedup: z.boolean().default(true).describe("Collapse similar lines (same message template) keeping newest"),
    },
    async (params) => {
      const result = await logSearch(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── turbostat_snapshot ──────────────────────────────────────────────── */
  server.tool(
    "turbostat_snapshot",
    "MSR-based per-core CPU metrics: actual frequency (MPERF/APERF), C-state " +
    "residency (C1/C1E/C6), SMI counts, IPC, per-core temperature, and RAPL " +
    "package power. Detects invisible stalls (SMIs), deep-sleep on isolated " +
    "DPDK cores, and thermal throttling. Auto-resolves HT siblings for cpu_filter. " +
    "Use dpdk_only=true to auto-discover and filter to DPDK lcore CPUs. " +
    "Detects IPC anomalies (>30% deviation from peer mean). Requires turbostat + root/CAP_SYS_RAWIO.",
    {
      host: z.string().default("localhost").describe(
        "Target host (SSH target or 'localhost')"
      ),
      interval_sec: z.number().min(1).max(30).default(1).describe(
        "Measurement window in seconds (turbostat --interval). 1s is usually enough; " +
        "increase for more stable averages"
      ),
      cpu_filter: z.array(z.number()).optional().describe(
        "Optional list of logical CPU IDs to include in output. HT siblings are auto-added. " +
        "If omitted, all CPUs are returned."
      ),
      dpdk_only: z.boolean().default(false).describe(
        "If true, auto-discover DPDK lcore CPUs (mtl_sch_*, lcore-*, eal-* threads) and show only those"
      ),
    },
    async (params) => {
      const result = await turbostatSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── diagnose_fps_drop ───────────────────────────────────────────────── */
  server.tool(
    "diagnose_fps_drop",
    "One-shot composite diagnostic for pipeline FPS drops. Tries USDT first for live FPS capture, then falls back to log parsing. " +
    "Maps the process to VF BDFs and PF netdev, runs turbostat on lcores (with HT siblings), checks NIC " +
    "stats and RDMA health, scans for log errors (tx_build_timeout, hw_dropped). Ranks suspects by severity. " +
    "Use as the primary entry point when investigating FPS or throughput drops. " +
    "For scheduler-level bottleneck analysis, follow up with mtl_usdt_tasklet_timing.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      log_path: z.string().describe("Path to the pipeline log file showing the FPS drop"),
      target_fps: z.number().optional().describe(
        "Expected/target FPS (e.g., 59.94). If omitted, auto-detected from nearest standard rate."
      ),
    },
    async (params) => {
      const result = await diagnoseFpsDrop(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── nic_vf_stats ────────────────────────────────────────────────────── */
  server.tool(
    "nic_vf_stats",
    "Read per-VF (Virtual Function) packet/byte/drop counters on a given PF (Physical Function) NIC via ip link show. " +
    "Shows rx/tx bytes, packets, drops, and multicast per VF index. Resolves VF BDFs from sysfs. " +
    "Use seconds>0 for delta mode with per-VF rate computation. " +
    "Essential for diagnosing SR-IOV VF-level traffic imbalances or drops (e.g., one VF receiving 0 while others are active). " +
    "For PF-aggregate counters, use nic_ethtool_stats. For DPDK-internal per-port stats, use mtl_dpdk_telemetry.",
    {
      host: z.string().default("localhost").describe("Target host (IP or hostname)"),
      pf_interface: z.string().describe("PF network interface name (e.g., 'ens255np0')"),
      seconds: z.number().min(0).max(30).default(0).describe(
        "Delta window. 0 = snapshot. >0 = two samples with rate computation (bytes/sec per VF)"
      ),
    },
    async (params) => {
      const result = await nicVfStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════════════
  // BCC / eBPF Tracing Tools
  // ═══════════════════════════════════════════════════════════════════════════

  /* ── runq_latency ────────────────────────────────────────────────────── */
  server.tool(
    "runq_latency",
    "Measure run-queue (scheduler) latency distribution using BCC runqlat: how long tasks WAIT in the CPU run queue " +
    "before being scheduled. Returns histogram with p50/p99 (microseconds). " +
    "On isolated DPDK cores, p99 should be <10µs. High latency = CPU contention (too many runnable tasks). " +
    "Use when context_switch_rate shows elevated switches, or when real-time tasks miss deadlines. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      per_cpu: z.boolean().default(false).describe("Show separate histogram per CPU"),
      pid: z.number().optional().describe("Filter to specific PID"),
    },
    async (params) => {
      const result = await runqLatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── offcpu_time ─────────────────────────────────────────────────────── */
  server.tool(
    "offcpu_time",
    "Trace off-CPU time with kernel+user stacks showing WHERE threads sleep or block (locks, I/O, futex, sleep, network). " +
    "Returns top stacks ranked by total off-CPU time. Use when a thread should be running but isn't — " +
    "the stacks reveal the exact blocking cause. Essential for diagnosing latency in non-DPDK threads " +
    "(bridge workers, MXL progress, consumer threads). Pair with wakeup_sources to see WHAT wakes them. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      pid: z.number().optional().describe("Filter to specific PID"),
      min_block_usec: z.number().min(0).default(1000).describe("Minimum off-CPU time to record (µs). Default 1000 (1ms)"),
      top_n: z.number().min(1).max(100).default(20).describe("Return top N stacks by total off-CPU time"),
    },
    async (params) => {
      const result = await offcpuTime(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── hardirq_latency ─────────────────────────────────────────────────── */
  server.tool(
    "hardirq_latency",
    "Measure hard IRQ handler execution time and count using BCC hardirqs. Shows which interrupt handlers are " +
    "running and how long each invocation takes. Long handlers (>100µs) steal CPU from any thread on that core, " +
    "including DPDK poll-mode threads. Use when core_load_snapshot shows high irq% on a DPDK core, " +
    "or when turbostat shows unexplained IPC drops. Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      mode: z.enum(["time", "count"]).default("time").describe("'time' = total handler time, 'count' = event count"),
    },
    async (params) => {
      const result = await hardirqLatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── cpudist ─────────────────────────────────────────────────────────── */
  server.tool(
    "cpudist",
    "CPU on-time or off-time distribution per-process using BCC cpudist. Histogram of how long threads run " +
    "before being descheduled (on-CPU mode) or how long they're off-CPU between runs (off-CPU mode). " +
    "Short on-CPU bursts (<1ms) = excessive preemption or contention. Use to diagnose scheduling fairness issues " +
    "and compare thread behavior across workloads. Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      mode: z.enum(["on-cpu", "off-cpu"]).default("on-cpu").describe("'on-cpu' = CPU burst length, 'off-cpu' = sleep duration"),
      per_process: z.boolean().default(false).describe("Show separate histogram per process"),
      pid: z.number().optional().describe("Filter to specific PID"),
    },
    async (params) => {
      const result = await cpudist(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── critical_sections ───────────────────────────────────────────────── */
  server.tool(
    "critical_sections",
    "Detect long kernel critical sections (preempt-disabled or IRQ-disabled) using BCC criticalstat. " +
    "Reports sections exceeding a threshold with stack traces — a hidden source of latency spikes invisible " +
    "to all other tools. While preemption is disabled, no other task can be scheduled on that core, " +
    "causing unbounded latency for DPDK poll-mode threads. " +
    "Use when hardirq_latency and softirq_snapshot look clean but latency spikes persist. " +
    "Requires bpfcc-tools + root + CONFIG_PREEMPT_TRACER.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      threshold_usec: z.number().min(1).default(100).describe("Only report sections longer than this (µs)"),
    },
    async (params) => {
      const result = await criticalSections(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── wakeup_sources ──────────────────────────────────────────────────── */
  server.tool(
    "wakeup_sources",
    "Trace WHAT wakes up blocked threads using BCC wakeuptime: shows kernel stacks at the wakeup point, " +
    "attributed by waker process. Complementary to offcpu_time: offcpu_time shows WHERE threads SLEEP, " +
    "wakeup_sources shows WHAT WAKES THEM. Use together for full blocking analysis. " +
    "Example: offcpu shows a thread blocked on futex; wakeup shows which other thread releases the lock. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      pid: z.number().optional().describe("Filter to specific PID"),
      top_n: z.number().min(1).max(100).default(20).describe("Return top N waker stacks"),
    },
    async (params) => {
      const result = await wakeupSources(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── llc_stat ────────────────────────────────────────────────────────── */
  server.tool(
    "llc_stat",
    "Last-Level Cache (LLC/L3) hit/miss statistics per process using BCC llcstat with hardware perf counters. " +
    "Shows LLC references, misses, and hit ratio per PID. High miss rates indicate working set exceeds cache, " +
    "cross-NUMA memory access, or cache pollution from co-running processes. " +
    "For per-CORE L2/L3 analysis with PCM (no PID attribution), use pcm_cache_analysis instead. " +
    "Requires bpfcc-tools + root + hardware PMU.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Sampling duration in seconds"),
      sample_period: z.number().min(1).max(10000).default(100).describe("Perf event sample period"),
    },
    async (params) => {
      const result = await llcStat(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── funclatency ─────────────────────────────────────────────────────── */
  server.tool(
    "funclatency",
    "Function latency histogram using BCC funclatency. Traces a kernel or user-space function and shows its " +
    "execution-time distribution. Use for targeted latency analysis of specific functions " +
    "(e.g., 'do_nanosleep', 'tcp_sendmsg', 'vfs_read', 'ib_post_send'). " +
    "Unlike offcpu_time (shows WHERE threads block), this shows HOW LONG a specific function takes across all invocations. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      function_pattern: z.string().describe("Kernel function name (e.g., 'do_nanosleep', 'tcp_sendmsg')"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      unit: z.enum(["nsecs", "usecs", "msecs"]).default("usecs").describe("Histogram bucket unit"),
      pid: z.number().optional().describe("Filter to specific PID"),
    },
    async (params) => {
      const result = await funclatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── cpuunclaimed ────────────────────────────────────────────────────
  server.tool(
    "cpuunclaimed",
    "Detect idle CPUs while tasks are waiting on other CPUs (scheduler imbalance). " +
    "Samples all run queues to find wasted capacity: if tasks wait on overloaded CPUs while other CPUs sit idle, " +
    "that's 'unclaimed' work the scheduler failed to migrate. " +
    "Note: may fail on systems with aggressive power management (C-states). " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Measurement duration in seconds"),
      sample_interval_ms: z.number().min(100).max(10000).default(1000).describe("Sampling interval in milliseconds"),
    },
    async (params) => {
      const result = await cpuUnclaimed(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── runqlen ─────────────────────────────────────────────────────────
  server.tool(
    "runqlen",
    "Run queue length distribution as a histogram — shows CPU saturation depth. " +
    "Complementary to runq_latency (which shows wait TIME): runqlen shows queue DEPTH. " +
    "Queue length 0=idle, 1=running (no contention), 2+=tasks waiting. " +
    "Sustained avg > 1 means CPU saturation. Use per_cpu=true to identify hot CPUs. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Measurement duration in seconds"),
      per_cpu: z.boolean().default(false).describe("Show per-CPU histograms instead of aggregate"),
    },
    async (params) => {
      const result = await runqLen(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── runqslower ──────────────────────────────────────────────────────
  server.tool(
    "runqslower",
    "Trace individual scheduling delays above a threshold (per-event, not histogram). " +
    "While runq_latency shows the distribution, runqslower shows EACH slow event with task name, PID, and delay. " +
    "Perfect for finding specific latency spikes hidden in histograms. " +
    "Default threshold 10ms — lower for latency-sensitive workloads (e.g., 1000 µs for DPDK). " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      threshold_usec: z.number().min(1).default(10000).describe("Minimum delay in µs to report (default 10ms)"),
      pid: z.number().optional().describe("Filter to specific PID"),
      show_previous: z.boolean().default(true).describe("Also show the previous task that was running"),
    },
    async (params) => {
      const result = await runqSlower(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── offwaketime ─────────────────────────────────────────────────────
  server.tool(
    "offwaketime",
    "Combined off-CPU + waker stack analysis — shows WHY threads blocked AND WHO woke them. " +
    "More informative than offcpu_time alone: traces causality (e.g., 'Thread A blocked in futex_wait, " +
    "woken by Thread B calling futex_wake from its work path'). " +
    "Output has blocked stack and waker stack separated by '--'. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      pid: z.number().optional().describe("Filter to specific PID"),
      min_block_usec: z.number().min(0).default(1000).describe("Minimum block time in µs to capture (default 1ms)"),
      top_n: z.number().min(1).max(100).default(20).describe("Number of top stacks to return"),
    },
    async (params) => {
      const result = await offWakeTime(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── profile_flamegraph ──────────────────────────────────────────────
  server.tool(
    "profile_flamegraph",
    "CPU stack sampling for flame graph generation. Returns folded-stack output (frame1;frame2 count) " +
    "suitable for direct input to flamegraph.pl or speedscope. Also returns top stacks as structured data. " +
    "Use for identifying CPU-hot code paths. Default 49 Hz sampling avoids lock-step artifacts. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Sampling duration in seconds"),
      frequency_hz: z.number().min(1).max(999).default(49).describe("Sampling frequency (default 49 Hz)"),
      pid: z.number().optional().describe("Filter to specific PID"),
      user_only: z.boolean().default(false).describe("Only sample user-space stacks"),
      kernel_only: z.boolean().default(false).describe("Only sample kernel stacks"),
    },
    async (params) => {
      const result = await profileFlamegraph(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── klockstat ───────────────────────────────────────────────────────
  server.tool(
    "klockstat",
    "Kernel mutex/spinlock contention statistics — hold time, wait time, acquisition count per caller. " +
    "Reports two tables: spin contention (time waiting to acquire) and hold times (time locks held). " +
    "Long hold times cause contention; high spin times indicate lock-heavy code paths. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      top_n: z.number().min(1).max(100).default(20).describe("Number of top callers to return per table"),
      pid: z.number().optional().describe("Filter to specific PID"),
    },
    async (params) => {
      const result = await klockstat(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── softirq_latency ─────────────────────────────────────────────────
  server.tool(
    "softirq_latency",
    "Per-softirq-type execution latency distribution (histogram per: net_rx, timer, block, tasklet, etc.). " +
    "More detailed than softirq_snapshot (which only shows counts from /proc/softirqs) — this traces actual execution times. " +
    "Long net_rx/timer softirqs steal CPU from user-space and cause jitter. " +
    "Uses BCC 'softirqs -d' (distribution mode). Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
    },
    async (params) => {
      const result = await softirqLatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── cachestat ───────────────────────────────────────────────────────
  server.tool(
    "cachestat",
    "Page cache hit/miss ratio per interval. Shows file system caching effectiveness: " +
    "hits, misses, dirty pages, hit ratio %, buffer/cached memory sizes. " +
    "Low hit ratio (<90%) means the working set doesn't fit in RAM — I/O goes to disk, " +
    "which indirectly causes CPU scheduling delays via I/O wait. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Measurement duration in seconds"),
      interval_sec: z.number().min(1).max(60).default(1).describe("Reporting interval in seconds"),
    },
    async (params) => {
      const result = await cachestat(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── biolatency ──────────────────────────────────────────────────────
  server.tool(
    "biolatency",
    "Block I/O latency distribution as histogram — shows storage performance from the OS perspective. " +
    "Relevant when storage-backed workloads affect CPU scheduling (I/O wait shows up as off-CPU time). " +
    "p99 > 10ms = slow storage, p99 > 100ms = severely degraded. Use per_disk=true for per-device breakdown. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
      per_disk: z.boolean().default(false).describe("Show per-disk histograms"),
    },
    async (params) => {
      const result = await biolatency(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── memleak ─────────────────────────────────────────────────────────
  server.tool(
    "memleak",
    "Trace outstanding memory allocations not freed — kernel or user-space leak detection. " +
    "Without PID: traces kernel allocations (kmalloc/get_free_pages). With PID: traces user-space malloc/calloc. " +
    "Shows allocation stacks sorted by bytes outstanding. Memory leaks cause gradual pressure → OOM kills → CPU stalls. " +
    "Requires bpfcc-tools + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(30).default(5).describe("Tracing duration in seconds"),
      pid: z.number().optional().describe("Trace user-space allocations for this PID (omit for kernel)"),
      top_n: z.number().min(1).max(50).default(10).describe("Number of top allocation stacks to return"),
    },
    async (params) => {
      const result = await memleak(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── numasched ───────────────────────────────────────────────────────
  server.tool(
    "numasched",
    "Track cross-NUMA task migrations — counts how often tasks move between NUMA nodes. " +
    "Cross-NUMA migrations are expensive: the migrated task loses cache locality and must access remote memory. " +
    "Complementary to process_numa_placement (current placement) and pcm_numa_traffic (bandwidth). " +
    "This shows migration EVENTS. Uses bpftrace with sched_migrate_task tracepoint. Requires bpftrace + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
    },
    async (params) => {
      const result = await numaSched(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ── wqlat ───────────────────────────────────────────────────────────
  server.tool(
    "wqlat",
    "Kernel workqueue latency — measures delay from work queuing to execution start. " +
    "High workqueue latency indicates kworker thread starvation (often from CPU oversubscription " +
    "on cores handling kernel workers). Shows per-workqueue-function average and total latency. " +
    "Uses bpftrace with workqueue tracepoints. Requires bpftrace + root.",
    {
      host: z.string().default("localhost").describe("Target host"),
      duration_sec: z.number().min(1).max(60).default(5).describe("Tracing duration in seconds"),
    },
    async (params) => {
      const result = await wqlat(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════════════
  // RDMA / Network / Devlink Tools
  // ═══════════════════════════════════════════════════════════════════════════

  /* ── rdma_counters ───────────────────────────────────────────────────── */
  server.tool(
    "rdma_counters",
    "RDMA transport-level counters from 'rdma stat show': InRdmaWrites, OutRdmaReads, cnpSent, cnpReceived, RxECNMrkd, etc. " +
    "Shows traffic VOLUME and congestion signaling. Different from rdma_health (which reads HW error counters + QP states). " +
    "Use rdma_counters for 'how much RDMA traffic and congestion?' vs rdma_health for 'are there errors or broken QPs?' " +
    "Critical for PFC/ECN/DCQCN debugging. Use seconds>0 for per-second rates. Requires rdma-core.",
    {
      host: z.string().default("localhost").describe("Target host"),
      device: z.string().optional().describe("Filter to specific RDMA device (e.g., 'rocep42s0')"),
      seconds: z.number().min(0).max(30).default(0).describe("Delta window. 0=snapshot, >0=two samples with per-second rates"),
    },
    async (params) => {
      const result = await rdmaCounters(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── network_stats ───────────────────────────────────────────────────── */
  server.tool(
    "network_stats",
    "Kernel network stack counters from 'nstat -az': TCP retransmissions (RetransSegs), UDP buffer overflows " +
    "(RcvbufErrors), IP fragmentation, listen queue drops, ICMP stats, and more. " +
    "These are KERNEL-LEVEL counters invisible to ethtool (which only sees NIC hardware). " +
    "Use when application connections fail, timeouts occur, or UDP data is lost at the socket layer. " +
    "seconds>0 for delta mode showing only changed counters. Universal (iproute2 package).",
    {
      host: z.string().default("localhost").describe("Target host"),
      seconds: z.number().min(0).max(30).default(0).describe("Delta window. 0=snapshot, >0=delta with only changed counters"),
      filter: z.string().optional().describe("Filter counter names containing this string (e.g., 'Tcp', 'Udp')"),
    },
    async (params) => {
      const result = await networkStats(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── devlink_health ──────────────────────────────────────────────────── */
  server.tool(
    "devlink_health",
    "NIC firmware-level health reporters via 'devlink health show': FW crash counts, TX hang events, " +
    "MDD (malicious driver detection) errors. These are NIC-INTERNAL errors invisible to both ethtool and RDMA counters. " +
    "Use when all software-level counters look clean but the NIC is misbehaving (link flaps, silent packet loss). " +
    "Check after unexpected link-down events or NIC resets. Works on ice (Intel E810) and mlx5 (ConnectX) drivers.",
    {
      host: z.string().default("localhost").describe("Target host"),
      pci_slot: z.string().optional().describe("Filter to specific PCI slot (e.g., '0000:27:00.0')"),
    },
    async (params) => {
      const result = await devlinkHealth(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  /* ── socket_diag ─────────────────────────────────────────────────────── */
  server.tool(
    "socket_diag",
    "TCP/UDP socket diagnostics via 'ss -tnpi' / 'ss -unpi'. Shows socket states, send/receive queue sizes, " +
    "PID/process name, and TCP internals (RTT, CWND, retransmits, congestion window). " +
    "Use when investigating stuck connections (CLOSE-WAIT), send queue backlogs (data not draining), " +
    "or per-flow TCP performance issues. For NIC-level counters, use nic_ethtool_stats; " +
    "for kernel-level aggregate stats, use network_stats. Universal (iproute2 package).",
    {
      host: z.string().default("localhost").describe("Target host"),
      protocol: z.enum(["tcp", "udp", "all"]).default("tcp").describe("Protocol to inspect"),
      state_filter: z.string().optional().describe("Filter by socket state (e.g., 'established', 'close-wait')"),
      process_filter: z.string().optional().describe("Filter by process name or PID"),
    },
    async (params) => {
      const result = await socketDiag(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  // ═══════════════════════════════════════════════════════════════════
  // Correlated snapshot — synchronized multi-tool capture for A/B testing
  // ═══════════════════════════════════════════════════════════════════
  server.tool(
    "correlated_snapshot",
    "Capture metrics from multiple tools within the SAME time window for reliable A/B test comparisons. " +
    "Runs selected collectors in parallel with a common timestamp, ensuring all metrics come from the same " +
    "time slice (critical when comparing before/after a config change). " +
    "Available collectors: core_load, running_on_cores, irq_distribution, softirq_snapshot, " +
    "cpu_frequency, context_switch_rate, pcm_core_counters, pcm_cache_analysis, mtl_session_stats. " +
    "Pass an empty array to run ALL collectors. Use core_filter/focus_cpus to limit to DPDK cores.",
    {
      collectors: z.array(z.string()).default([]).describe(
        "List of collector names to run. Empty array = all. " +
        "Options: core_load, running_on_cores, irq_distribution, softirq_snapshot, " +
        "cpu_frequency, context_switch_rate, pcm_core_counters, pcm_cache_analysis, mtl_session_stats"
      ),
      window_s: z.number().min(1).max(30).default(2).describe("Capture window in seconds. PCM and windowed collectors share this window."),
      core_filter: z.string().optional().describe("Core filter for PCM tools. Accepts: '4', '4,5,6', '4-13', '4-13,20'. Omit for all cores."),
      focus_cpus: z.string().optional().describe("CPUs to inspect with running_on_cores. Same format as core_filter (e.g. '4-13'). Required if running_on_cores is requested."),
      host: z.string().default("localhost").describe("Target host for core collectors (IP or hostname)"),
      mtl_host: z.string().default("localhost").describe("Target host for MTL session stats"),
      mtl_log_path: z.string().optional().describe("Path to MTL log file (required if mtl_session_stats collector is used)"),
      mtl_last_dumps: z.number().int().min(1).max(100).default(5).describe("Number of last MTL stat dumps to use (steady-state only)"),
      pcm_seconds: z.number().int().min(1).max(30).optional().describe("PCM delta window (defaults to min(window_s, 5))"),
    },
    async (params) => {
      const result = await correlatedSnapshot(params);
      return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
    }
  );

  return server;
}
