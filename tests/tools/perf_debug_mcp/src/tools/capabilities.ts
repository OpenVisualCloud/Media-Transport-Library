/**
 * capabilities() — Return environment info and enabled features.
 */
import type { ToolResponse, Capabilities } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getOnlineCpus, getNumaNodeCount } from "../collectors/sysfs.js";
import { getEbpfBridge } from "../collectors/ebpf-bridge.js";
import { getPcmBridgeForHost } from "../collectors/pcm-bridge.js";
import { getEmonBridge } from "../collectors/emon-bridge.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { isReadable } from "../utils/proc-reader.js";
import { readFileHost, isReadableHost, isLocalHost } from "../remote/host-utils.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { readFile } from "fs/promises";

const ALL_TOOLS = [
  "capabilities",
  "tool_guide",
  "core_load_snapshot",
  "allowed_on_core",
  "running_on_core",
  "starvation_report",
  "irq_distribution",
  "irq_affinity",
  "nic_irq_queue_map",
  "softirq_snapshot",
  "kernel_threads_on_core",
  "runqueue_snapshot",
  "context_switch_rate",
  "isolation_summary",
  "cpu_frequency_snapshot",
  "throttling_summary",
  "cgroup_cpu_limits",
  "numa_topology",
  "process_numa_placement",
  "perf_debug_snapshot",
  "pcm_core_counters",
  "pcm_memory_bandwidth",
  "pcm_cache_analysis",
  "pcm_power_thermal",
  "pcm_qpi_upi_link",
  "pcm_pcie_bandwidth",
  "pcm_numa_traffic",
  // MTL (Media Transport Library) tools
  "mtl_manager_status",
  "mtl_dpdk_telemetry",
  "mtl_instance_processes",
  "mtl_session_stats",
  "mtl_app_latency",
  "mtl_live_stats",
  "mtl_nic_pf_stats",
  "mtl_hugepage_usage",
  "mtl_lcore_shm",
  "mtl_influxdb_query",
  "mtl_scheduler_map",
  // MTL USDT (bpftrace) tools
  "mtl_usdt_list_probes",
  "mtl_usdt_session_stats",
  "mtl_usdt_frame_trace",
  "mtl_usdt_ptp_trace",
  "mtl_usdt_tasklet_timing",
  "mtl_usdt_session_timing",
  "mtl_usdt_cni_pcap",
  "mtl_usdt_log_monitor",
  // EMON (Intel Event Monitor) tools
  "emon_capabilities",
  "emon_collect",
  "emon_triage",
  "emon_pcie_topology",
  // RDMA / DCB / NIC / Pipeline tools
  "rdma_health",
  "dcb_status",
  "nic_ethtool_stats",
  "pipeline_health",
  "log_search",
  "turbostat_snapshot",
  "diagnose_fps_drop",
  "nic_vf_stats",
  "correlated_snapshot",
  // BCC / eBPF tracing tools
  "runq_latency",
  "offcpu_time",
  "hardirq_latency",
  "cpudist",
  "critical_sections",
  "wakeup_sources",
  "llc_stat",
  "funclatency",
  // RDMA / Network / Devlink tools
  "rdma_counters",
  "network_stats",
  "devlink_health",
  "socket_diag",
];

export async function capabilities(params?: {
  host?: string;
}): Promise<ToolResponse<Capabilities>> {
  const host = params?.host;
  const local = isLocalHost(host);
  const meta = await buildMeta("auto", undefined, true);

  try {
    // Kernel version
    let kernelVersion = "unknown";
    try {
      const ver = isLocalHost(host)
        ? await readFile("/proc/version", "utf-8")
        : await readFileHost(host, "/proc/version");
      const match = ver.match(/Linux version (\S+)/);
      if (match) kernelVersion = match[1];
    } catch { /* ignore */ }

    // CPU count
    const cpus = await getOnlineCpus(host);
    const cpuCount = cpus.length;

    // NUMA nodes
    const numaNodes = await getNumaNodeCount(host);

    // Check proc readability
    const canReadProc = local
      ? await isReadable("/proc/stat")
      : await isReadableHost(host, "/proc/stat");

    // eBPF detection — only meaningful locally
    let ebpfAvailable = false;
    let ebpfEnabled = false;
    const missingCaps: string[] = [];
    if (local) {
      const bridge = getEbpfBridge();
      ebpfAvailable = bridge.isAvailable;
      ebpfEnabled = bridge.isEnabled;
      missingCaps.push(...bridge.missingCapabilities);
    }

    // PCM detection — host-aware
    const pcmBridge = getPcmBridgeForHost(host);
    const pcmAvailable = pcmBridge.isAvailable;
    const pcmHost = local ? (process.env.PCM_HOST ?? "127.0.0.1") : host;
    const pcmEndpoint = `http://${pcmHost}:${process.env.PCM_PORT ?? "9738"}`;

    // EMON detection — local-only (reads MSRs / loads drivers)
    let emonAvailable = false;
    let emonBridge: { version?: string; eventCount?: number } = {};
    if (local) {
      const eb = getEmonBridge();
      emonAvailable = eb.isAvailable;
      emonBridge = { version: eb.version, eventCount: eb.eventCount };
    }

    // Check for common missing capabilities — local only
    if (local && !ebpfAvailable) {
      try {
        const statusContent = await readFile("/proc/self/status", "utf-8");
        const capBnd = statusContent.match(/CapBnd:\s*([0-9a-f]+)/i);
        if (capBnd) {
          const caps = BigInt("0x" + capBnd[1]);
          // CAP_BPF = 39, CAP_PERFMON = 38, CAP_SYS_ADMIN = 21
          if (!(caps & (1n << 39n))) missingCaps.push("CAP_BPF");
          if (!(caps & (1n << 38n))) missingCaps.push("CAP_PERFMON");
          if (!(caps & (1n << 21n))) missingCaps.push("CAP_SYS_ADMIN");
        }
      } catch { /* ignore */ }
    }

    // BCC tools detection — probe on target host
    const targetHost = local ? "localhost" : host!;
    const bccToolNames = ["runqlat", "offcputime", "hardirqs", "cpudist", "criticalstat", "wakeuptime", "llcstat", "funclatency"];
    const bccDetected: string[] = [];
    for (const tool of bccToolNames) {
      const found = await sshExecSafe(targetHost, `command -v ${tool}-bpfcc 2>/dev/null || command -v ${tool} 2>/dev/null`);
      if (found && found.trim()) bccDetected.push(tool);
    }
    const bccAvailable = bccDetected.length > 0;

    // USDT / bpftrace detection
    let usdtAvailable = false;
    let bpftraceBridge: { version?: string; probeCount?: number; libmtlPath?: string } = {};
    if (local) {
      const bb = getBpftraceBridge();
      usdtAvailable = bb.isAvailable;
      bpftraceBridge = { version: bb.version, probeCount: bb.probeCount, libmtlPath: bb.libmtlPath ?? undefined };
    } else {
      // Check if bpftrace exists on remote host
      const bpfCheck = await sshExecSafe(targetHost, "command -v bpftrace 2>/dev/null");
      usdtAvailable = !!(bpfCheck && bpfCheck.trim());
      if (usdtAvailable) {
        const verOut = await sshExecSafe(targetHost, "bpftrace --version 2>/dev/null");
        if (verOut) {
          const match = verOut.match(/(\d+\.\d+\.\d+)/);
          if (match) bpftraceBridge.version = match[1];
        }
      }
    }

    // RDMA detection — on target host
    const rdmaCheck = await sshExecSafe(targetHost, "command -v rdma 2>/dev/null");
    const rdmaAvailable = !!(rdmaCheck && rdmaCheck.trim());

    // devlink detection — on target host
    const devlinkCheck = await sshExecSafe(targetHost, "command -v devlink 2>/dev/null");
    const devlinkAvailable = !!(devlinkCheck && devlinkCheck.trim());

    const data: Capabilities = {
      kernel_version: kernelVersion,
      cpu_count: cpuCount,
      numa_nodes: numaNodes,
      can_read_proc: canReadProc,
      ebpf_mode_available: ebpfAvailable,
      ebpf_mode_enabled: ebpfEnabled,
      pcm_server_available: pcmAvailable,
      pcm_server_endpoint: pcmEndpoint,
      emon_available: emonAvailable,
      emon_version: emonBridge.version || undefined,
      emon_event_count: emonBridge.eventCount || undefined,
      bcc_tools_available: bccAvailable,
      bcc_tools_detected: bccDetected.length > 0 ? bccDetected : undefined,
      usdt_available: usdtAvailable,
      usdt_bpftrace_version: bpftraceBridge.version || undefined,
      usdt_probes_detected: bpftraceBridge.probeCount || undefined,
      usdt_libmtl_path: bpftraceBridge.libmtlPath || undefined,
      rdma_available: rdmaAvailable,
      devlink_available: devlinkAvailable,
      missing_capabilities: [...new Set(missingCaps)],
      default_sampling_resolution_ms: 250,
      max_recommended_window_ms: 10000,
      available_tools: ALL_TOOLS,
    };

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "CAPABILITIES_ERROR", `Failed to gather capabilities: ${err}`);
  }
}
