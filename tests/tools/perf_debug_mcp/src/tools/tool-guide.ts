/**
 * tool_guide() — Categorized tool index with a decision tree for LLM tool selection.
 *
 * Returns tools grouped by category with "start here" guidance, drill-down chains,
 * and prerequisite info. Designed to help an LLM pick the right tool from 70+ options.
 */
import type { ToolResponse } from "../types.js";
import { buildMeta, okResponse } from "../meta.js";

interface ToolEntry {
  name: string;
  one_liner: string;
  requires?: string;
}

interface ToolCategory {
  category: string;
  description: string;
  start_here?: string;
  tools: ToolEntry[];
}

interface ToolGuideData {
  decision_tree: Record<string, string>;
  categories: ToolCategory[];
  drill_down_chains: Record<string, string[]>;
}

export async function toolGuide(): Promise<ToolResponse<ToolGuideData>> {
  const meta = await buildMeta("fallback");

  const decision_tree: Record<string, string> = {
    "General performance triage": "perf_debug_snapshot → then drill into specific tools",
    "What's running on my DPDK core?": "running_on_core (who IS running) — NOT allowed_on_core (who CAN run, returns thousands)",
    "Is my core isolated properly?": "isolation_summary → then softirq_snapshot + irq_distribution for that core",
    "Why is IPC low / core busy?": "pcm_core_counters → pcm_cache_analysis (L2/L3) → emon_collect (deep PMU)",
    "Memory bandwidth or NUMA issue?": "pcm_memory_bandwidth → pcm_numa_traffic per core → process_numa_placement",
    "RDMA errors or congestion?": "rdma_health → dcb_status → nic_ethtool_stats",
    "MTL/ST2110 pipeline issue?": "pipeline_health → mtl_session_stats → mtl_usdt_session_stats",
    "Thread missing deadlines?": "starvation_report (all-in-one diagnosis for a specific thread)",
    "NIC/driver problem?": "devlink_health → nic_ethtool_stats → nic_irq_queue_map",
    "Scheduling latency?": "runq_latency (BCC) → cpudist → offcpu_time",
    "Which tools are available?": "capabilities (checks PCM, EMON, BCC, USDT, RDMA availability)",
  };

  const categories: ToolCategory[] = [
    {
      category: "Start Here",
      description: "Entry-point tools that combine multiple checks. Call one of these first before drilling down.",
      tools: [
        { name: "capabilities", one_liner: "Check which subsystems are available (PCM, EMON, BCC, USDT, RDMA)" },
        { name: "perf_debug_snapshot", one_liner: "All-in-one: core loads + IRQs + softirqs + frequency + isolation" },
        { name: "pipeline_health", one_liner: "MTL/ST2110 pipeline health: sessions, errors, NIC stats, RDMA" },
        { name: "diagnose_fps_drop", one_liner: "Automated FPS drop diagnosis across CPU, NIC, RDMA, MTL" },
      ],
    },
    {
      category: "CPU Per-Core (LOCAL ONLY)",
      description: "Per-CPU utilization, frequency, scheduling. Read from /proc and sysfs — local machine only.",
      start_here: "core_load_snapshot",
      tools: [
        { name: "core_load_snapshot", one_liner: "Per-core CPU utilization (user/sys/irq/idle) over sampling window" },
        { name: "running_on_core", one_liner: "Tasks OBSERVED RUNNING on a CPU (who DID run, top-N by runtime)" },
        { name: "allowed_on_core", one_liner: "Tasks whose affinity INCLUDES a CPU (who CAN run — WARNING: returns thousands)" },
        { name: "kernel_threads_on_core", one_liner: "Kernel threads on a CPU (ksoftirqd, rcu, kworker, etc.)" },
        { name: "cpu_frequency_snapshot", one_liner: "Per-CPU frequency, governor, min/max from cpufreq sysfs" },
        { name: "runqueue_snapshot", one_liner: "Load averages + per-CPU runnable count (instant, no sampling)" },
        { name: "context_switch_rate", one_liner: "Global and per-CPU context switches/sec over a window" },
      ],
    },
    {
      category: "Isolation & Scheduling (LOCAL ONLY)",
      description: "CPU isolation config verification and thread starvation analysis.",
      start_here: "isolation_summary",
      tools: [
        { name: "isolation_summary", one_liner: "Check isolcpus/nohz_full/rcu_nocbs from /proc/cmdline" },
        { name: "starvation_report", one_liner: "Starvation diagnosis for a specific thread (co-runners, IRQ pressure, throttling)" },
        { name: "throttling_summary", one_liner: "Detect thermal, pstate, RT bandwidth, and governor throttling" },
        { name: "cgroup_cpu_limits", one_liner: "cgroup v2 CPU quotas, cpuset pins, throttle counters for a PID" },
        { name: "process_numa_placement", one_liner: "NUMA placement for a process (allowed CPUs, memory nodes)" },
        { name: "numa_topology", one_liner: "NUMA node layout: CPUs per node, memory, distance matrix" },
      ],
    },
    {
      category: "IRQ & Softirq (LOCAL ONLY)",
      description: "Interrupt distribution and affinity. Key for verifying DPDK core isolation from IRQs.",
      start_here: "irq_distribution",
      tools: [
        { name: "irq_distribution", one_liner: "IRQ counts per CPU over a delta window + per-IRQ breakdown" },
        { name: "irq_affinity", one_liner: "Configured + effective CPU affinity for IRQs (by number or regex)" },
        { name: "nic_irq_queue_map", one_liner: "NIC queue → IRQ → CPU mapping for an interface" },
        { name: "softirq_snapshot", one_liner: "Per-CPU softirq counts (NET_RX, NET_TX, TIMER, RCU, etc.)" },
      ],
    },
    {
      category: "Intel PCM (Hardware Counters)",
      description: "MSR-level per-core metrics via pcm-sensor-server. Requires pcm-sensor-server on port 9738.",
      start_here: "pcm_core_counters",
      tools: [
        { name: "pcm_core_counters", one_liner: "Per-core IPC, instructions, L2/L3 cache, SMI, frequency, memory BW", requires: "pcm-sensor-server" },
        { name: "pcm_cache_analysis", one_liner: "Per-core L2/L3 hit ratios, miss rates, L3 occupancy", requires: "pcm-sensor-server" },
        { name: "pcm_memory_bandwidth", one_liner: "Per-socket DRAM read/write bandwidth and NUMA ratio", requires: "pcm-sensor-server" },
        { name: "pcm_numa_traffic", one_liner: "Per-core local vs remote memory traffic", requires: "pcm-sensor-server" },
        { name: "pcm_power_thermal", one_liner: "Per-socket power (watts), temperature, TMA bottleneck analysis", requires: "pcm-sensor-server" },
        { name: "pcm_qpi_upi_link", one_liner: "Inter-socket UPI/QPI link utilization and bandwidth", requires: "pcm-sensor-server" },
        { name: "pcm_pcie_bandwidth", one_liner: "PCIe read/write bandwidth per socket", requires: "pcm-sensor-server" },
      ],
    },
    {
      category: "Intel EMON (Deep PMU)",
      description: "Event-based CPU profiling with Intel EMON. Deeper than PCM — runs specific event groups (E1-E8).",
      start_here: "emon_capabilities",
      tools: [
        { name: "emon_capabilities", one_liner: "Check EMON availability, version, supported event count", requires: "emon" },
        { name: "emon_collect", one_liner: "Collect PMU events for a specific group (E1-E8) with per-core data", requires: "emon" },
        { name: "emon_triage", one_liner: "Run key EMON groups and summarize bottlenecks", requires: "emon" },
        { name: "emon_pcie_topology", one_liner: "PCIe device topology and NUMA affinity", requires: "emon" },
      ],
    },
    {
      category: "MTL (Media Transport Library)",
      description: "ST2110 media pipeline monitoring. Most tools accept a 'host' parameter for remote machines.",
      start_here: "pipeline_health",
      tools: [
        { name: "mtl_manager_status", one_liner: "MTL manager daemon status and instance list" },
        { name: "mtl_instance_processes", one_liner: "Running MTL processes with PIDs and ports" },
        { name: "mtl_session_stats", one_liner: "Per-session TX/RX stats from MTL log files" },
        { name: "mtl_app_latency", one_liner: "Application-level latency from MTL telemetry" },
        { name: "mtl_live_stats", one_liner: "Real-time MTL session statistics" },
        { name: "mtl_dpdk_telemetry", one_liner: "DPDK telemetry counters (queues, mbufs, rings)" },
        { name: "mtl_nic_pf_stats", one_liner: "NIC PF ethtool stats + driver info + link details" },
        { name: "mtl_hugepage_usage", one_liner: "Hugepage allocation and usage per NUMA node" },
        { name: "mtl_lcore_shm", one_liner: "DPDK lcore shared memory mappings" },
        { name: "mtl_scheduler_map", one_liner: "MTL scheduler-to-lcore mapping from logs" },
        { name: "mtl_influxdb_query", one_liner: "Query MTL metrics from InfluxDB" },
      ],
    },
    {
      category: "MTL USDT (bpftrace probes)",
      description: "Deep MTL tracing via USDT probes. Requires bpftrace and libmtl with USDT support.",
      start_here: "mtl_usdt_list_probes",
      tools: [
        { name: "mtl_usdt_list_probes", one_liner: "Enumerate available USDT probes in libmtl", requires: "bpftrace" },
        { name: "mtl_usdt_session_stats", one_liner: "Per-session packet/frame counters via USDT", requires: "bpftrace" },
        { name: "mtl_usdt_frame_trace", one_liner: "Frame-level tracing (build/transmit/receive lifecycle)", requires: "bpftrace" },
        { name: "mtl_usdt_ptp_trace", one_liner: "PTP clock synchronization tracing", requires: "bpftrace" },
        { name: "mtl_usdt_tasklet_timing", one_liner: "Tasklet execution timing and scheduling", requires: "bpftrace" },
        { name: "mtl_usdt_session_timing", one_liner: "Session-level timing breakdown", requires: "bpftrace" },
        { name: "mtl_usdt_cni_pcap", one_liner: "CNI packet capture via USDT probes", requires: "bpftrace" },
        { name: "mtl_usdt_log_monitor", one_liner: "Real-time MTL log monitoring via USDT", requires: "bpftrace" },
      ],
    },
    {
      category: "BCC / eBPF Tracing",
      description: "Kernel-level tracing tools from BCC. Require root/CAP_BPF. Accept 'host' for remote.",
      start_here: "runq_latency",
      tools: [
        { name: "runq_latency", one_liner: "Run queue latency histogram (time waiting to be scheduled)", requires: "BCC" },
        { name: "offcpu_time", one_liner: "Off-CPU time analysis (why threads are sleeping/blocked)", requires: "BCC" },
        { name: "hardirq_latency", one_liner: "Hard IRQ handler latency histogram", requires: "BCC" },
        { name: "cpudist", one_liner: "CPU run length distribution (time per scheduling slice)", requires: "BCC" },
        { name: "critical_sections", one_liner: "Lock contention / critical section timing", requires: "BCC" },
        { name: "wakeup_sources", one_liner: "What wakes up threads (wakeup call stacks)", requires: "BCC" },
        { name: "llc_stat", one_liner: "Per-process LLC (Last Level Cache) stats", requires: "BCC" },
        { name: "funclatency", one_liner: "Latency histogram for a kernel/user function", requires: "BCC" },
      ],
    },
    {
      category: "RDMA / DCB / NIC",
      description: "RDMA health, NIC counters, DCB config, devlink. Most accept 'host' for remote.",
      start_here: "rdma_health",
      tools: [
        { name: "rdma_health", one_liner: "RDMA HW error counters, QP states, throughput (delta mode for rates)" },
        { name: "rdma_counters", one_liner: "RDMA traffic volume counters (InRdmaWrites, cnpSent, ECN)" },
        { name: "dcb_status", one_liner: "PFC/ETS configuration, MTU, link state for RDMA interfaces" },
        { name: "nic_ethtool_stats", one_liner: "NIC hardware counters from ethtool -S (drops, errors, queues)" },
        { name: "nic_vf_stats", one_liner: "SR-IOV VF traffic and spoofcheck stats" },
        { name: "devlink_health", one_liner: "NIC firmware health reporters (FW crashes, TX hangs, MDD)" },
        { name: "network_stats", one_liner: "Kernel TCP/UDP/IP counters from nstat (retransmits, drops)" },
        { name: "socket_diag", one_liner: "TCP/UDP socket states, queue sizes, per-flow RTT" },
      ],
    },
    {
      category: "Composite / A-B Testing",
      description: "Multi-tool aggregators and synchronized capture for before/after comparisons.",
      tools: [
        { name: "perf_debug_snapshot", one_liner: "All-in-one CPU triage (loads + IRQs + freq + isolation)" },
        { name: "pipeline_health", one_liner: "All-in-one MTL/RDMA/NIC pipeline check" },
        { name: "diagnose_fps_drop", one_liner: "Automated FPS drop root cause across CPU/NIC/RDMA/MTL" },
        { name: "correlated_snapshot", one_liner: "Synchronized multi-tool capture with common timestamp" },
        { name: "turbostat_snapshot", one_liner: "MSR-based per-core: frequency, C-states, IPC, temperature" },
        { name: "log_search", one_liner: "Search system/application logs for error patterns" },
      ],
    },
  ];

  const drill_down_chains: Record<string, string[]> = {
    "CPU load investigation": [
      "core_load_snapshot (which cores are busy?)",
      "running_on_core (what's running on the busy core?)",
      "pcm_core_counters (why is it busy — IPC, cache misses?)",
      "emon_collect (deep PMU event analysis)",
    ],
    "Isolation verification": [
      "isolation_summary (are boot params correct?)",
      "softirq_snapshot with core_filter (softirqs on isolated core?)",
      "irq_distribution with core_filter (IRQs on isolated core?)",
      "kernel_threads_on_core (kernel threads on isolated core?)",
      "running_on_core (any unexpected userspace threads?)",
    ],
    "RDMA debugging": [
      "rdma_health (errors, QP states, throughput rates)",
      "dcb_status (PFC/ETS config correct?)",
      "nic_ethtool_stats (NIC-level drops?)",
      "devlink_health (firmware errors?)",
      "network_stats (kernel TCP/UDP stack issues?)",
    ],
    "MTL pipeline debugging": [
      "pipeline_health (one-shot overview)",
      "mtl_session_stats (per-session packet counts)",
      "mtl_usdt_session_stats (deep per-session tracing)",
      "mtl_usdt_frame_trace (frame lifecycle)",
      "mtl_usdt_ptp_trace (PTP sync issues)",
    ],
    "Scheduling latency": [
      "runq_latency (how long do threads wait in runqueue?)",
      "cpudist (how long do threads run per slice?)",
      "offcpu_time (why are threads sleeping?)",
      "starvation_report (all-in-one for a specific thread)",
    ],
  };

  return okResponse<ToolGuideData>({ decision_tree, categories, drill_down_chains }, meta);
}
