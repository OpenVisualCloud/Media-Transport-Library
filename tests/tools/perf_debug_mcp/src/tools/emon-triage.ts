/**
 * emon_triage(focus_cpu, window_ms) — Cascading waterfall triage using EMON presets.
 *
 * The triage engine runs EMON presets in a specific order, each check
 * informing whether the next is needed. This avoids unnecessary collection
 * and builds a diagnosis chain:
 *
 *   Step 1: E0 (IIO PCIe) — Is there a PCIe bandwidth hot-port?
 *   Step 2: E1 (CHA LLC)  — Is LLC under pressure? Snoop storm?
 *   Step 3: E2 (Mesh)     — Is memory latency elevated?
 *   Step 4: E3 (UPI)      — Is cross-socket traffic causing congestion?
 *   Step 5: E4 (Core)     — What is the core-level bottleneck?
 *
 * Each step produces TriageCause entries with severity, evidence, and
 * recommendations. The waterfall can short-circuit when a root cause
 * is found at high severity, or continue for a comprehensive report.
 *
 * When to use:
 *   - First tool to call when investigating performance issues
 *   - Provides a systematic top-down diagnosis path
 *   - Runs all presets and correlates findings
 */
import type { ToolResponse } from "../types.js";
import type {
  EmonPresetId,
  EmonTriageResult,
  TriageCause,
  TriageSeverity,
  TriageEvidence,
} from "../types-emon.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getEmonBridge } from "../collectors/emon-bridge.js";

// ─── Thresholds ─────────────────────────────────────────────────────────────

/** IIO: ports with >1000 MB/s total are considered "hot" */
const IIO_HOT_PORT_MBPS = 1000;
/** IIO: ports with >5000 MB/s are critical */
const IIO_CRITICAL_PORT_MBPS = 5000;

/** CHA: LLC hit ratio below this is concerning */
const LLC_HIT_RATIO_WARNING = 80;
/** CHA: LLC hit ratio below this is critical */
const LLC_HIT_RATIO_CRITICAL = 50;
/** CHA: average TOR occupancy above this indicates latency issues */
const TOR_OCCUPANCY_WARNING = 10;
const TOR_OCCUPANCY_CRITICAL = 50;

/** Mesh: estimated memory latency above this (ns) is concerning */
const MEM_LATENCY_WARNING_NS = 100;
const MEM_LATENCY_CRITICAL_NS = 200;

/** UPI: data-to-snoop ratio below this indicates excessive snooping */
const UPI_SNOOP_RATIO_WARNING = 2;
const UPI_SNOOP_RATIO_CRITICAL = 1;
/** UPI: any CRC errors is a warning */
const UPI_CRC_ERROR_WARNING = 0;

/** Core: IPC below this is concerning */
const IPC_WARNING = 0.5;
const IPC_CRITICAL = 0.2;
/** Core: execution starvation above this is concerning */
const EXEC_STARVATION_WARNING_PCT = 50;
const EXEC_STARVATION_CRITICAL_PCT = 75;

export async function emonTriage(params: {
  focus_cpu?: number | null;
  window_ms?: number;
  quick?: boolean;
}): Promise<ToolResponse<EmonTriageResult>> {
  const windowMs = params.window_ms ?? 1000;
  const quick = params.quick ?? false;
  const meta = await buildMeta("fallback", windowMs);

  const bridge = getEmonBridge();
  if (!bridge.isAvailable) {
    // Attempt to auto-load SEP drivers
    const { ready, autoLoadMessage } = await bridge.ensureReady();
    if (!ready) {
      return errorResponse(
        meta,
        "EMON_UNAVAILABLE",
        bridge.connectionError ?? "EMON is not available",
        autoLoadMessage ?? "Ensure EMON is installed at /opt/intel/sep/ and SEP drivers are loaded",
      );
    }
  }

  const triageStart = Date.now();
  const causes: TriageCause[] = [];
  const presetsUsed: EmonPresetId[] = [];
  let overallSeverity: TriageSeverity = "info";

  const updateSeverity = (s: TriageSeverity) => {
    if (s === "critical") overallSeverity = "critical";
    else if (s === "warning" && overallSeverity !== "critical") overallSeverity = "warning";
  };

  const cpuFilter = params.focus_cpu != null ? [params.focus_cpu] : undefined;

  // ── Step 1: E0 — IIO PCIe per-port ──────────────────────────────
  try {
    const e0 = await bridge.collectPreset("E0_iio_pcie_per_port", windowMs);
    presetsUsed.push("E0_iio_pcie_per_port");

    if (e0.iio) {
      for (const port of e0.iio.ports) {
        const totalMbps = port.read_mbps + port.write_mbps;
        if (totalMbps >= IIO_CRITICAL_PORT_MBPS) {
          causes.push({
            id: "pcie_hot_port_critical",
            title: "Critical PCIe Hot Port",
            severity: "critical",
            description: `IIO socket ${port.socket} stack ${port.stack} part ${port.part} is pushing ${Math.round(totalMbps)} MB/s (read: ${port.read_mbps}, write: ${port.write_mbps}).`,
            evidence: [
              { source: "E0_iio", metric: "total_mbps", value: Math.round(totalMbps), threshold: IIO_CRITICAL_PORT_MBPS },
              { source: "E0_iio", metric: "read_mbps", value: port.read_mbps },
              { source: "E0_iio", metric: "write_mbps", value: port.write_mbps },
              { source: "E0_iio", metric: "socket:stack:part", value: `${port.socket}:${port.stack}:${port.part}` },
            ],
            recommendations: [
              "Check if this NIC/device is expected to be at this bandwidth level",
              "Use emon_pcie_topology to map this IIO stack to the physical PCIe device",
              "Consider if DPDK/RDMA traffic pattern is optimal",
              "Check for retransmissions or excessive small-packet traffic",
            ],
          });
          updateSeverity("critical");
        } else if (totalMbps >= IIO_HOT_PORT_MBPS) {
          causes.push({
            id: "pcie_hot_port_warning",
            title: "High PCIe Port Traffic",
            severity: "warning",
            description: `IIO socket ${port.socket} stack ${port.stack} part ${port.part} at ${Math.round(totalMbps)} MB/s.`,
            evidence: [
              { source: "E0_iio", metric: "total_mbps", value: Math.round(totalMbps), threshold: IIO_HOT_PORT_MBPS },
            ],
            recommendations: [
              "Monitor this port — may indicate growing traffic",
              "Use emon_pcie_topology to identify the device",
            ],
          });
          updateSeverity("warning");
        }
      }
    }
  } catch (err: any) {
    causes.push({
      id: "e0_collection_failed",
      title: "IIO Collection Failed",
      severity: "info",
      description: `Could not collect E0 IIO data: ${err?.message ?? err}`,
      evidence: [],
      recommendations: ["Check EMON drivers and PMU availability"],
    });
  }

  // ── Step 2: E1 — CHA LLC snoop/coherence ──────────────────────
  try {
    const e1 = await bridge.collectPreset("E1_cha_llc_snoop", windowMs);
    presetsUsed.push("E1_cha_llc_snoop");

    if (e1.cha) {
      for (const st of e1.cha.socket_totals) {
        if (st.hit_ratio_pct < LLC_HIT_RATIO_CRITICAL) {
          causes.push({
            id: `llc_miss_storm_s${st.socket}`,
            title: `LLC Miss Storm (Socket ${st.socket})`,
            severity: "critical",
            description: `Socket ${st.socket} LLC hit ratio is ${st.hit_ratio_pct}% (critical threshold: ${LLC_HIT_RATIO_CRITICAL}%). Total misses: ${st.total_llc_misses.toLocaleString()}.`,
            evidence: [
              { source: "E1_cha", metric: "hit_ratio_pct", value: st.hit_ratio_pct, threshold: LLC_HIT_RATIO_CRITICAL },
              { source: "E1_cha", metric: "total_llc_misses", value: st.total_llc_misses },
              { source: "E1_cha", metric: "avg_tor_occupancy", value: st.avg_tor_occupancy },
            ],
            recommendations: [
              "Working set likely exceeds LLC capacity — check process memory footprint",
              "Verify NUMA affinity — process may be accessing remote memory",
              "Consider prefetch tuning or memory access pattern optimization",
            ],
          });
          updateSeverity("critical");
        } else if (st.hit_ratio_pct < LLC_HIT_RATIO_WARNING) {
          causes.push({
            id: `llc_pressure_s${st.socket}`,
            title: `LLC Pressure (Socket ${st.socket})`,
            severity: "warning",
            description: `Socket ${st.socket} LLC hit ratio is ${st.hit_ratio_pct}%.`,
            evidence: [
              { source: "E1_cha", metric: "hit_ratio_pct", value: st.hit_ratio_pct, threshold: LLC_HIT_RATIO_WARNING },
            ],
            recommendations: [
              "Monitor LLC usage trends",
              "Check for processes with large working sets co-located on this socket",
            ],
          });
          updateSeverity("warning");
        }

        if (st.avg_tor_occupancy > TOR_OCCUPANCY_CRITICAL) {
          causes.push({
            id: `tor_congestion_s${st.socket}`,
            title: `TOR Congestion (Socket ${st.socket})`,
            severity: "critical",
            description: `Socket ${st.socket} average TOR occupancy is ${st.avg_tor_occupancy} (indicates deep memory access queue).`,
            evidence: [
              { source: "E1_cha", metric: "avg_tor_occupancy", value: st.avg_tor_occupancy, threshold: TOR_OCCUPANCY_CRITICAL },
            ],
            recommendations: [
              "Memory subsystem is overloaded — check DRAM bandwidth utilization",
              "Look for NUMA-remote accesses causing extra mesh hops",
              "Consider memory interleaving or process migration",
            ],
          });
          updateSeverity("critical");
        } else if (st.avg_tor_occupancy > TOR_OCCUPANCY_WARNING) {
          causes.push({
            id: `tor_elevated_s${st.socket}`,
            title: `Elevated TOR Occupancy (Socket ${st.socket})`,
            severity: "warning",
            description: `Socket ${st.socket} average TOR occupancy is ${st.avg_tor_occupancy}.`,
            evidence: [
              { source: "E1_cha", metric: "avg_tor_occupancy", value: st.avg_tor_occupancy, threshold: TOR_OCCUPANCY_WARNING },
            ],
            recommendations: [
              "Run E2_mesh_stall_latency for detailed memory latency estimate",
            ],
          });
          updateSeverity("warning");
        }
      }
    }
  } catch (err: any) {
    causes.push({
      id: "e1_collection_failed",
      title: "CHA Collection Failed",
      severity: "info",
      description: `Could not collect E1 CHA data: ${err?.message ?? err}`,
      evidence: [],
      recommendations: [],
    });
  }

  // In quick mode, skip remaining presets if we already found critical issues
  if (quick && (overallSeverity as string) === "critical") {
    return okResponse(
      {
        causes,
        overall_severity: overallSeverity,
        summary: buildSummary(causes, overallSeverity),
        presets_used: presetsUsed,
        triage_duration_ms: Date.now() - triageStart,
      },
      meta,
    );
  }

  // ── Step 3: E2 — Mesh stall / memory latency ──────────────────
  try {
    const e2 = await bridge.collectPreset("E2_mesh_stall_latency", windowMs);
    presetsUsed.push("E2_mesh_stall_latency");

    if (e2.mesh) {
      for (const sock of e2.mesh.sockets) {
        if (sock.estimated_mem_latency_ns != null) {
          if (sock.estimated_mem_latency_ns > MEM_LATENCY_CRITICAL_NS) {
            causes.push({
              id: `mem_latency_critical_s${sock.socket}`,
              title: `Critical Memory Latency (Socket ${sock.socket})`,
              severity: "critical",
              description: `Estimated memory access latency on socket ${sock.socket} is ~${sock.estimated_mem_latency_ns}ns.`,
              evidence: [
                { source: "E2_mesh", metric: "estimated_mem_latency_ns", value: sock.estimated_mem_latency_ns, threshold: MEM_LATENCY_CRITICAL_NS },
                { source: "E2_mesh", metric: "avg_tor_occupancy_drd", value: sock.avg_tor_occupancy_drd },
              ],
              recommendations: [
                "Memory subsystem is severely congested",
                "Check DRAM bandwidth saturation (pcm_memory_bandwidth)",
                "Look for NUMA-remote memory accesses",
                "Consider if workload can be partitioned to reduce cross-socket traffic",
              ],
            });
            updateSeverity("critical");
          } else if (sock.estimated_mem_latency_ns > MEM_LATENCY_WARNING_NS) {
            causes.push({
              id: `mem_latency_warning_s${sock.socket}`,
              title: `Elevated Memory Latency (Socket ${sock.socket})`,
              severity: "warning",
              description: `Estimated memory latency ~${sock.estimated_mem_latency_ns}ns on socket ${sock.socket}.`,
              evidence: [
                { source: "E2_mesh", metric: "estimated_mem_latency_ns", value: sock.estimated_mem_latency_ns, threshold: MEM_LATENCY_WARNING_NS },
              ],
              recommendations: [
                "Monitor memory bandwidth trends",
                "Verify NUMA affinity of latency-sensitive processes",
              ],
            });
            updateSeverity("warning");
          }
        }

        if (sock.m2m_tracker_occupancy != null && sock.m2m_tracker_occupancy > 0) {
          causes.push({
            id: `m2m_queue_s${sock.socket}`,
            title: `M2M Queue Depth (Socket ${sock.socket})`,
            severity: "info",
            description: `M2M tracker occupancy on socket ${sock.socket}: ${sock.m2m_tracker_occupancy}`,
            evidence: [
              { source: "E2_mesh", metric: "m2m_tracker_occupancy", value: sock.m2m_tracker_occupancy },
            ],
            recommendations: [],
          });
        }
      }
    }
  } catch (err: any) {
    causes.push({
      id: "e2_collection_failed",
      title: "Mesh Stall Collection Failed",
      severity: "info",
      description: `Could not collect E2 mesh data: ${err?.message ?? err}`,
      evidence: [],
      recommendations: [],
    });
  }

  // ── Step 4: E3 — UPI detailed ─────────────────────────────────
  try {
    const e3 = await bridge.collectPreset("E3_upi_detailed", windowMs);
    presetsUsed.push("E3_upi_detailed");

    if (e3.upi) {
      // Check for CRC errors (any is bad)
      for (const link of e3.upi.links) {
        if (link.crc_errors > UPI_CRC_ERROR_WARNING) {
          causes.push({
            id: `upi_crc_s${link.socket}_l${link.link}`,
            title: `UPI CRC Errors (S${link.socket} L${link.link})`,
            severity: "critical",
            description: `UPI link socket ${link.socket} link ${link.link} has ${link.crc_errors} CRC retransmit requests — indicates link integrity issues.`,
            evidence: [
              { source: "E3_upi", metric: "crc_errors", value: link.crc_errors, threshold: 0 },
            ],
            recommendations: [
              "CRC errors on UPI indicate hardware or signal integrity problems",
              "Check thermal conditions and power delivery",
              "May require hardware inspection or replacement",
            ],
          });
          updateSeverity("critical");
        }
      }

      // Check data-to-snoop ratio
      for (const st of e3.upi.socket_totals) {
        if (st.data_to_snoop_ratio > 0 && st.data_to_snoop_ratio < UPI_SNOOP_RATIO_CRITICAL) {
          causes.push({
            id: `upi_snoop_storm_s${st.socket}`,
            title: `UPI Snoop Storm (Socket ${st.socket})`,
            severity: "critical",
            description: `Socket ${st.socket} UPI data-to-snoop ratio is ${st.data_to_snoop_ratio} — more than half of UPI traffic is snoops/coherence.`,
            evidence: [
              { source: "E3_upi", metric: "data_to_snoop_ratio", value: st.data_to_snoop_ratio, threshold: UPI_SNOOP_RATIO_CRITICAL },
              { source: "E3_upi", metric: "total_tx_non_data_flits", value: st.total_tx_non_data_flits },
            ],
            recommendations: [
              "Excessive cross-socket coherence traffic",
              "Check for shared data structures accessed by both sockets",
              "Consider NUMA-pinning latency-sensitive threads",
            ],
          });
          updateSeverity("critical");
        } else if (st.data_to_snoop_ratio > 0 && st.data_to_snoop_ratio < UPI_SNOOP_RATIO_WARNING) {
          causes.push({
            id: `upi_high_snoops_s${st.socket}`,
            title: `Elevated UPI Snoops (Socket ${st.socket})`,
            severity: "warning",
            description: `Socket ${st.socket} UPI data-to-snoop ratio is ${st.data_to_snoop_ratio}.`,
            evidence: [
              { source: "E3_upi", metric: "data_to_snoop_ratio", value: st.data_to_snoop_ratio, threshold: UPI_SNOOP_RATIO_WARNING },
            ],
            recommendations: [
              "Monitor cross-socket coherence patterns",
            ],
          });
          updateSeverity("warning");
        }
      }
    }
  } catch (err: any) {
    causes.push({
      id: "e3_collection_failed",
      title: "UPI Collection Failed",
      severity: "info",
      description: `Could not collect E3 UPI data: ${err?.message ?? err}`,
      evidence: [],
      recommendations: [],
    });
  }

  // ── Step 5: E4 — Core stall deep ──────────────────────────────
  try {
    const e4 = await bridge.collectPreset("E4_core_stall_deep", windowMs, cpuFilter);
    presetsUsed.push("E4_core_stall_deep");

    if (e4.core_stall) {
      const avg = e4.core_stall.system_avg;

      // System-wide IPC check
      if (avg.ipc > 0 && avg.ipc < IPC_CRITICAL) {
        causes.push({
          id: "system_ipc_critical",
          title: "System-Wide IPC Crisis",
          severity: "critical",
          description: `System average IPC is ${avg.ipc} — extremely low processor throughput.`,
          evidence: [
            { source: "E4_core", metric: "system_ipc", value: avg.ipc, threshold: IPC_CRITICAL },
            { source: "E4_core", metric: "exec_starvation_ratio", value: avg.exec_starvation_ratio },
            { source: "E4_core", metric: "backend_bound_pct", value: avg.backend_bound_pct },
          ],
          recommendations: [
            "Check backend_bound_pct — if high, memory subsystem is the bottleneck",
            "Check frontend_bound_pct — if high, code fetch / branch prediction is the issue",
            "Run E1/E2 for LLC/memory latency analysis",
          ],
        });
        updateSeverity("critical");
      } else if (avg.ipc > 0 && avg.ipc < IPC_WARNING) {
        causes.push({
          id: "system_ipc_warning",
          title: "Low System IPC",
          severity: "warning",
          description: `System average IPC is ${avg.ipc}.`,
          evidence: [
            { source: "E4_core", metric: "system_ipc", value: avg.ipc, threshold: IPC_WARNING },
          ],
          recommendations: [
            "Investigate per-core IPC distribution for hot spots",
          ],
        });
        updateSeverity("warning");
      }

      // Execution starvation
      if (avg.exec_starvation_ratio > EXEC_STARVATION_CRITICAL_PCT) {
        causes.push({
          id: "exec_starvation_critical",
          title: "Severe Execution Starvation",
          severity: "critical",
          description: `${avg.exec_starvation_ratio}% of cycles have zero uops executing — cores are mostly stalled.`,
          evidence: [
            { source: "E4_core", metric: "exec_starvation_ratio", value: avg.exec_starvation_ratio, threshold: EXEC_STARVATION_CRITICAL_PCT },
          ],
          recommendations: [
            "Cores are waiting for data or instructions most of the time",
            "Cross-reference with LLC miss data (E1) and memory latency (E2)",
            "Check if workload has serialization bottlenecks",
          ],
        });
        updateSeverity("critical");
      } else if (avg.exec_starvation_ratio > EXEC_STARVATION_WARNING_PCT) {
        causes.push({
          id: "exec_starvation_warning",
          title: "Execution Starvation",
          severity: "warning",
          description: `${avg.exec_starvation_ratio}% of cycles have zero uops executing.`,
          evidence: [
            { source: "E4_core", metric: "exec_starvation_ratio", value: avg.exec_starvation_ratio, threshold: EXEC_STARVATION_WARNING_PCT },
          ],
          recommendations: [
            "Look at per-core breakdown for which CPUs are most affected",
          ],
        });
        updateSeverity("warning");
      }

      // Report per-CPU outliers if focus_cpu is specified
      if (params.focus_cpu != null) {
        const focusCore = e4.core_stall.cores.find(c => c.cpu === params.focus_cpu);
        if (focusCore) {
          causes.push({
            id: `focus_cpu_${params.focus_cpu}`,
            title: `Focus CPU ${params.focus_cpu} Analysis`,
            severity: focusCore.ipc < IPC_CRITICAL ? "critical" : focusCore.ipc < IPC_WARNING ? "warning" : "info",
            description: `CPU ${params.focus_cpu}: IPC=${focusCore.ipc}, exec_starvation=${focusCore.exec_starvation_ratio}%, backend=${focusCore.backend_bound_pct}%, frontend=${focusCore.frontend_bound_pct}%, retiring=${focusCore.retiring_pct}%.`,
            evidence: [
              { source: "E4_core", metric: "ipc", value: focusCore.ipc },
              { source: "E4_core", metric: "exec_starvation_ratio", value: focusCore.exec_starvation_ratio },
              { source: "E4_core", metric: "inst_retired", value: focusCore.inst_retired },
              { source: "E4_core", metric: "clk_unhalted", value: focusCore.clk_unhalted },
            ],
            recommendations: [],
          });
        }
      }
    }
  } catch (err: any) {
    causes.push({
      id: "e4_collection_failed",
      title: "Core Stall Collection Failed",
      severity: "info",
      description: `Could not collect E4 core data: ${err?.message ?? err}`,
      evidence: [],
      recommendations: [],
    });
  }

  // If no causes found, add an "all clear" entry
  if (causes.length === 0) {
    causes.push({
      id: "all_clear",
      title: "No Issues Detected",
      severity: "info",
      description: "All EMON triage checks passed without finding concerning patterns.",
      evidence: [],
      recommendations: [
        "System appears healthy at the hardware counter level",
        "If performance issues persist, check software-level metrics (scheduling, locks, IO)",
      ],
    });
  }

  return okResponse(
    {
      causes,
      overall_severity: overallSeverity,
      summary: buildSummary(causes, overallSeverity),
      presets_used: presetsUsed,
      triage_duration_ms: Date.now() - triageStart,
    },
    meta,
  );
}

// ─── Helpers ────────────────────────────────────────────────────────────────

function buildSummary(causes: TriageCause[], severity: TriageSeverity): string {
  const critical = causes.filter(c => c.severity === "critical");
  const warnings = causes.filter(c => c.severity === "warning");

  if (critical.length > 0) {
    return `CRITICAL: ${critical.length} critical issue(s) found — ${critical.map(c => c.title).join("; ")}`;
  }
  if (warnings.length > 0) {
    return `WARNING: ${warnings.length} warning(s) — ${warnings.map(c => c.title).join("; ")}`;
  }
  return "No significant hardware-level issues detected.";
}
