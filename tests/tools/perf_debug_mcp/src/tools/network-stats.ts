/**
 * network_stats — Kernel network stack counters from nstat.
 *
 * Reads all SNMP/SNMP6/netstat counters via `nstat -az`, providing visibility
 * into TCP retransmissions, ICMP errors, UDP overflows, IP fragmentation, etc.
 * These kernel counters are invisible to ethtool (which only sees NIC-level
 * stats) and reveal problems in the kernel network stack itself.
 *
 * Key counters for real-time video/RDMA workloads:
 *   - TcpRetransSegs: TCP retransmissions (congestion or packet loss)
 *   - TcpExtTCPTimeouts: TCP timeout events
 *   - UdpRcvbufErrors: UDP receive buffer overflows (dropped datagrams)
 *   - IpExtInOctets/OutOctets: aggregate IP throughput
 *
 * Source: `nstat -az` (from iproute2 package).
 * Universal — works on any Linux host.
 */
import type { ToolResponse, NetworkStatsData, NetworkStatsHighlight } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function networkStats(params: {
  host?: string;
  seconds?: number;
  filter?: string;
}): Promise<ToolResponse<NetworkStatsData>> {
  const host = params.host ?? "localhost";
  const seconds = params.seconds ?? 0;
  const filter = params.filter;

  const windowMs = seconds > 0 ? seconds * 1000 : undefined;
  const meta = await buildMeta("fallback", windowMs);

  // Check nstat availability
  const check = await sshExecSafe(host, "command -v nstat 2>/dev/null");
  if (!check || !check.trim()) {
    return errorResponse(
      meta,
      "NSTAT_MISSING",
      "nstat not found on target host",
      "Install iproute2: apt-get install iproute2",
    );
  }

  try {
    if (seconds <= 0) {
      // Snapshot mode — read current counters
      const output = await sshExecSafe(host, "nstat -az 2>/dev/null", 10_000);
      if (!output) {
        return errorResponse(meta, "NSTAT_NO_OUTPUT", "nstat produced no output");
      }

      const counters = parseNstatOutput(output, filter);
      const highlights = generateHighlights(counters);
      const warnings = generateNetworkWarnings(counters);

      return okResponse<NetworkStatsData>({
        counters,
        highlights,
        warnings,
      }, meta);
    }

    // Delta mode — reset, wait, read
    // nstat without -a shows only changed counters since last read
    // First: zero out internal counters with a dummy read
    await sshExecSafe(host, "nstat -z >/dev/null 2>&1", 5_000);

    // Wait for the configured interval
    await new Promise((r) => setTimeout(r, seconds * 1000));

    // Second read — this shows deltas since the zero
    const output = await sshExecSafe(host, "nstat -s 2>/dev/null", 10_000);
    if (!output) {
      return errorResponse(meta, "NSTAT_DELTA_FAILED", "nstat delta read failed");
    }

    const deltas = parseNstatOutput(output, filter);

    // Also get absolute values for context
    const absOutput = await sshExecSafe(host, "nstat -az 2>/dev/null", 10_000);
    const counters = absOutput ? parseNstatOutput(absOutput, filter) : {};

    const highlights = generateHighlights(deltas, seconds);
    const warnings = generateNetworkWarnings(deltas, seconds);

    return okResponse<NetworkStatsData>({
      counters,
      deltas,
      delta_seconds: seconds,
      highlights,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "NETWORK_STATS_ERROR",
      `Failed to read network stats: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse nstat output.
 *
 * Format:
 * ```
 * #kernel
 * IpInReceives                    12345                  0.0
 * IpInDelivers                    12345                  0.0
 * ```
 */
function parseNstatOutput(output: string, filter?: string): Record<string, number> {
  const counters: Record<string, number> = {};
  const filterLower = filter?.toLowerCase();

  for (const line of output.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#")) continue;

    const parts = trimmed.split(/\s+/);
    if (parts.length < 2) continue;

    const key = parts[0];
    const value = parseFloat(parts[1]);
    if (isNaN(value)) continue;

    // Apply filter if specified
    if (filterLower && !key.toLowerCase().includes(filterLower)) continue;

    counters[key] = value;
  }

  return counters;
}

/**
 * Generate highlights for key network metrics.
 */
function generateHighlights(counters: Record<string, number>, seconds?: number): NetworkStatsHighlight[] {
  const highlights: NetworkStatsHighlight[] = [];
  const rate = seconds && seconds > 0 ? seconds : 1;

  const check = (key: string, threshold: number, severity: "warning" | "critical", msg: string) => {
    const val = counters[key];
    if (val !== undefined && val > threshold) {
      highlights.push({ counter: key, value: val, severity, message: msg });
    }
  };

  // TCP retransmissions
  check("TcpRetransSegs", 0, "warning",
    `TCP retransmissions: ${counters["TcpRetransSegs"]} — packet loss or congestion`);

  // TCP timeouts
  check("TcpExtTCPTimeouts", 0, "critical",
    `TCP timeouts: ${counters["TcpExtTCPTimeouts"]} — connections stalling`);

  // UDP buffer overflows
  check("UdpRcvbufErrors", 0, "critical",
    `UDP receive buffer errors: ${counters["UdpRcvbufErrors"]} — datagrams dropped (application too slow or buffer too small)`);
  check("UdpSndbufErrors", 0, "warning",
    `UDP send buffer errors: ${counters["UdpSndbufErrors"]} — send buffer full`);

  // IP fragmentation
  check("IpReasmFails", 0, "warning",
    `IP reassembly failures: ${counters["IpReasmFails"]} — fragmented packets not fully received`);

  // ICMP errors
  check("IcmpInErrors", 0, "warning",
    `ICMP errors received: ${counters["IcmpInErrors"]}`);

  // ListenOverflows / ListenDrops
  check("TcpExtListenOverflows", 0, "critical",
    `TCP listen overflows: ${counters["TcpExtListenOverflows"]} — SYN dropped (listen backlog full)`);
  check("TcpExtListenDrops", 0, "critical",
    `TCP listen drops: ${counters["TcpExtListenDrops"]} — connections dropped at listen`);

  // Out-of-window
  check("TcpExtTCPACKSkippedSeq", 0, "warning",
    `TCP ACK skipped (out of sequence): ${counters["TcpExtTCPACKSkippedSeq"]}`);

  return highlights;
}

/**
 * Generate warnings from network counters.
 */
function generateNetworkWarnings(counters: Record<string, number>, seconds?: number): string[] {
  const warnings: string[] = [];

  const retrans = counters["TcpRetransSegs"] ?? 0;
  if (retrans > 100) {
    warnings.push(`High TCP retransmissions: ${retrans} — check for network congestion or packet loss`);
  }

  const udpErrors = counters["UdpRcvbufErrors"] ?? 0;
  if (udpErrors > 0) {
    warnings.push(`UDP receive buffer errors: ${udpErrors} — increase socket buffer (sysctl net.core.rmem_max) or speed up consumer`);
  }

  const listenOverflows = counters["TcpExtListenOverflows"] ?? 0;
  if (listenOverflows > 0) {
    warnings.push(`TCP listen backlog overflows: ${listenOverflows} — increase net.core.somaxconn or application backlog`);
  }

  return warnings;
}
