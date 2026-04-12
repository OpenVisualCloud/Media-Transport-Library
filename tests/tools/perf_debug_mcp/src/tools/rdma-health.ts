/**
 * rdma_health — RDMA device health from Linux sysfs and rdma tool.
 *
 * Reads hardware counters (retransmissions, ECN marks, CNP stats, errors),
 * QP states, and per-device throughput.  Optionally takes two snapshots to
 * compute rates.
 *
 * Data sources:
 *   - `rdma link show` — device discovery
 *   - /sys/class/infiniband/<dev>/ports/<port>/hw_counters/ — HW counters
 *   - `rdma -j res show qp` — Queue Pair state enumeration
 *
 * Universal — works with any RDMA driver (irdma, mlx5, rxe, etc.).
 */
import type { ToolResponse } from "../types.js";
import type {
  RdmaHealthData,
  RdmaDeviceHealth,
  RdmaHwCounter,
  RdmaQpInfo,
  RdmaHealthWarning,
  RdmaQpStateSummary,
  RdmaProcessSummary,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── helpers ─────────────────────────────────────────────────────────────── */

interface RdmaDevice {
  device: string;
  port: number;
  state: string;
  physical_state: string;
  netdev: string | null;
}

/**
 * Discover RDMA devices via `rdma link show`.
 */
async function discoverDevices(host: string): Promise<RdmaDevice[]> {
  const output = await sshExecSafe(host, "rdma link show 2>/dev/null");
  if (!output || !output.trim()) return [];

  const devices: RdmaDevice[] = [];
  for (const line of output.split("\n")) {
    // link rocep42s0/1 state ACTIVE physical_state LINK_UP netdev ens255np0
    const m = line.match(
      /link\s+(\S+)\/(\d+)\s+state\s+(\S+)\s+physical_state\s+(\S+)(?:\s+netdev\s+(\S+))?/,
    );
    if (m) {
      devices.push({
        device: m[1],
        port: parseInt(m[2], 10),
        state: m[3],
        physical_state: m[4],
        netdev: m[5] ?? null,
      });
    }
  }
  return devices;
}

/**
 * Read all hw_counters (or counters) for a device+port from sysfs.
 */
async function readHwCounters(
  host: string,
  device: string,
  port: number,
): Promise<Map<string, number>> {
  const counters = new Map<string, number>();

  // Try hw_counters first (irdma, mlx5), fall back to counters
  for (const dir of ["hw_counters", "counters"]) {
    const path = `/sys/class/infiniband/${device}/ports/${port}/${dir}`;
    const script = `if [ -d "${path}" ]; then for f in ${path}/*; do echo "$(basename "$f")=$(cat "$f" 2>/dev/null)"; done; fi`;
    const output = await sshExecSafe(host, script, 10_000);
    if (output && output.trim()) {
      for (const line of output.split("\n")) {
        const eq = line.indexOf("=");
        if (eq > 0) {
          const name = line.slice(0, eq).trim();
          const val = parseInt(line.slice(eq + 1).trim(), 10);
          if (!isNaN(val)) counters.set(name, val);
        }
      }
      break; // got counters from this dir, don't try fallback
    }
  }
  return counters;
}

/**
 * Enumerate QPs via `rdma -j res show qp`, filtering out GSI management QPs.
 */
async function readQps(host: string, deviceFilter?: RegExp): Promise<RdmaQpInfo[]> {
  const output = await sshExecSafe(host, "rdma -j res show qp 2>/dev/null", 10_000);
  if (!output || !output.trim()) return [];

  let raw: unknown[];
  try {
    raw = JSON.parse(output);
  } catch {
    return [];
  }
  if (!Array.isArray(raw)) return [];

  const qps: RdmaQpInfo[] = [];
  for (const entry of raw) {
    const e = entry as Record<string, unknown>;
    const ifname = String(e.ifname ?? "");
    const type = String(e.type ?? "");
    const state = String(e.state ?? "");

    // Filter out GSI management QPs
    if (type === "GSI" || type === "SMI") continue;
    if (deviceFilter && !deviceFilter.test(ifname)) continue;

    qps.push({
      ifname,
      lqpn: Number(e.lqpn ?? 0),
      rqpn: e.rqpn != null ? Number(e.rqpn) : undefined,
      type,
      state,
      pid: e.pid != null ? Number(e.pid) : undefined,
      comm: e.comm != null ? String(e.comm) : undefined,
    });
  }
  return qps;
}

/**
 * Get a counter value from the map, trying multiple name variants.
 */
function counter(m: Map<string, number>, ...names: string[]): number {
  for (const n of names) {
    const v = m.get(n);
    if (v !== undefined) return v;
  }
  return 0;
}

/**
 * Build one RdmaDeviceHealth from a single snapshot.
 */
function buildDeviceHealth(
  dev: RdmaDevice,
  counters: Map<string, number>,
): RdmaDeviceHealth {
  const allCounters: RdmaHwCounter[] = [];
  for (const [name, value] of [...counters.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
    allCounters.push({ name, value });
  }

  return {
    device: dev.device,
    port: dev.port,
    state: dev.state,
    physical_state: dev.physical_state,
    netdev: dev.netdev,

    // Key counters (summary) first
    out_rdma_writes: counter(counters, "OutRdmaWrites"),
    in_rdma_writes: counter(counters, "InRdmaWrites"),
    out_rdma_reads: counter(counters, "OutRdmaReads"),
    in_rdma_reads: counter(counters, "InRdmaReads"),
    retrans_segs: counter(counters, "RetransSegs"),
    rx_ecn_marked: counter(counters, "RxECNMrkd"),
    cnp_sent: counter(counters, "cnpSent"),
    cnp_handled: counter(counters, "cnpHandled"),
    cnp_ignored: counter(counters, "cnpIgnored"),
    in_opt_errors: counter(counters, "InOptErrors"),
    in_proto_errors: counter(counters, "InProtoErrors"),
    ip4_in_discards: counter(counters, "ip4InDiscards"),
    ip4_out_octets: counter(counters, "ip4OutOctets"),
    ip4_in_octets: counter(counters, "ip4InOctets"),

    // Full counter dump last
    all_counters: allCounters,
  };
}

/**
 * Compute delta fields between two device health snapshots.
 */
function applyDelta(
  before: RdmaDeviceHealth,
  after: RdmaDeviceHealth,
  elapsedSec: number,
): void {
  after.delta_seconds = elapsedSec;

  const writesBefore = before.out_rdma_writes + before.in_rdma_writes;
  const writesAfter = after.out_rdma_writes + after.in_rdma_writes;
  after.writes_per_sec = Math.round((writesAfter - writesBefore) / elapsedSec);

  const retransDelta = after.retrans_segs - before.retrans_segs;
  after.retrans_per_sec = Math.round((retransDelta / elapsedSec) * 100) / 100;

  const outBytesDelta = after.ip4_out_octets - before.ip4_out_octets;
  const inBytesDelta = after.ip4_in_octets - before.ip4_in_octets;
  after.throughput_out_gbps = Math.round((outBytesDelta * 8) / elapsedSec / 1e9 * 1000) / 1000;
  after.throughput_in_gbps = Math.round((inBytesDelta * 8) / elapsedSec / 1e9 * 1000) / 1000;

  // Update all_counters with delta info
  const beforeMap = new Map(before.all_counters.map((c) => [c.name, c.value]));
  for (const c of after.all_counters) {
    const bv = beforeMap.get(c.name);
    if (bv !== undefined) {
      c.delta = c.value - bv;
      c.rate_per_sec = Math.round((c.delta / elapsedSec) * 100) / 100;
    }
  }
}

/**
 * Generate warnings from a device health snapshot.
 */
function generateWarnings(
  dev: RdmaDeviceHealth,
  isDelta: boolean,
): RdmaHealthWarning[] {
  const warnings: RdmaHealthWarning[] = [];
  const d = dev.device;

  if (dev.state !== "ACTIVE") {
    warnings.push({ device: d, severity: "critical", message: `${d}: link state is ${dev.state}` });
  }

  if (dev.retrans_segs > 0) {
    if (isDelta && dev.retrans_per_sec !== undefined && dev.retrans_per_sec > 0) {
      // Rate-based severity: compute retrans rate relative to total writes
      const totalWrites = dev.writes_per_sec ?? 1;
      const retransPct = totalWrites > 0 ? (dev.retrans_per_sec / totalWrites) * 100 : 0;
      let severity: "info" | "warning" | "critical" = "info";
      if (retransPct > 0.1) severity = "critical";
      else if (retransPct > 0.01) severity = "warning";
      warnings.push({
        device: d,
        severity,
        message: `${d}: active retransmissions (${dev.retrans_per_sec}/sec, ${retransPct.toFixed(3)}% of writes)`,
      });
    } else {
      warnings.push({
        device: d,
        severity: "warning",
        message: `${d}: ${dev.retrans_segs} retransmitted segments (indicates packet loss or congestion)`,
      });
    }
  }

  if (dev.rx_ecn_marked > 0) {
    warnings.push({
      device: d,
      severity: "info",
      message: `${d}: ${dev.rx_ecn_marked} ECN-marked packets received (DCQCN active)`,
    });
  }

  if (dev.cnp_sent > 0 || dev.cnp_handled > 0) {
    warnings.push({
      device: d,
      severity: "info",
      message: `${d}: CNP flow control active (sent=${dev.cnp_sent}, handled=${dev.cnp_handled})`,
    });
  }

  if (dev.in_opt_errors > 0 || dev.in_proto_errors > 0) {
    warnings.push({
      device: d,
      severity: "critical",
      message: `${d}: protocol errors (opt=${dev.in_opt_errors}, proto=${dev.in_proto_errors})`,
    });
  }

  if (dev.ip4_in_discards > 0) {
    warnings.push({
      device: d,
      severity: "warning",
      message: `${d}: ${dev.ip4_in_discards} inbound discards`,
    });
  }

  // QP state warnings
  if (dev.qps) {
    for (const qp of dev.qps) {
      if (qp.state !== "RTS") {
        const owner = qp.comm ? `owned by ${qp.comm} pid ${qp.pid}` : "unknown owner";
        warnings.push({
          device: d,
          severity: "critical",
          message: `${d}: QP lqpn=${qp.lqpn} in state ${qp.state} (expected RTS), ${owner}`,
        });
      }
    }
  }

  return warnings;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function rdmaHealth(params: {
  host?: string;
  device_filter?: string;
  port_filter?: number[];
  seconds?: number;
  include_qps?: boolean;
  summary_only?: boolean;
}): Promise<ToolResponse<RdmaHealthData>> {
  const host = params.host ?? "localhost";
  const seconds = params.seconds ?? 0;
  const includeQps = params.include_qps ?? true;
  const summaryOnly = params.summary_only ?? false;
  const deviceFilter = params.device_filter ? new RegExp(params.device_filter, "i") : undefined;
  const portFilter = params.port_filter && params.port_filter.length > 0
    ? new Set(params.port_filter)
    : null;

  const meta = await buildMeta("fallback", seconds > 0 ? seconds * 1000 : undefined);

  // Check rdma tool availability
  const rdmaCheck = await sshExecSafe(host, "command -v rdma 2>/dev/null");
  if (!rdmaCheck || !rdmaCheck.trim()) {
    return errorResponse(
      meta,
      "RDMA_TOOL_MISSING",
      "rdma tool not found on target host",
      "Install iproute2 for rdma tool support: apt-get install iproute2",
    );
  }

  try {
    // Discover RDMA devices
    let devices = await discoverDevices(host);
    if (deviceFilter) {
      devices = devices.filter((d) => deviceFilter.test(d.device));
    }
    if (portFilter) {
      devices = devices.filter((d) => portFilter.has(d.port));
    }
    devices.sort((a, b) => a.device.localeCompare(b.device));

    if (devices.length === 0) {
      return okResponse<RdmaHealthData>(
        { devices: [], device_count: 0, warnings: [] },
        meta,
      );
    }

    // Read initial counters
    const t0Counters = new Map<string, Map<string, number>>();
    for (const dev of devices) {
      t0Counters.set(`${dev.device}/${dev.port}`, await readHwCounters(host, dev.device, dev.port));
    }

    // Delta mode: sleep and read again
    let t1Counters: Map<string, Map<string, number>> | null = null;
    let elapsedSec = 0;
    if (seconds > 0) {
      const t0 = process.hrtime.bigint();
      await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
      const t1 = process.hrtime.bigint();
      elapsedSec = Number(t1 - t0) / 1e9;

      t1Counters = new Map<string, Map<string, number>>();
      for (const dev of devices) {
        t1Counters.set(`${dev.device}/${dev.port}`, await readHwCounters(host, dev.device, dev.port));
      }
    }

    // Build device health objects
    const healthDevices: RdmaDeviceHealth[] = [];
    const isDelta = t1Counters !== null;

    for (const dev of devices) {
      const key = `${dev.device}/${dev.port}`;
      const srcCounters = t1Counters ? t1Counters.get(key)! : t0Counters.get(key)!;
      const health = buildDeviceHealth(dev, srcCounters);

      if (t1Counters) {
        const before = buildDeviceHealth(dev, t0Counters.get(key)!);
        applyDelta(before, health, elapsedSec);
      }

      healthDevices.push(health);
    }

    // QP enumeration
    if (includeQps) {
      const allQps = await readQps(host, deviceFilter);
      for (const dev of healthDevices) {
        const devQps = allQps.filter((q) => q.ifname === dev.device);

        // QP state summary
        const byState: Record<string, number> = {};
        for (const qp of devQps) {
          byState[qp.state] = (byState[qp.state] ?? 0) + 1;
        }

        // Assign summary fields before all_counters for JSON output ordering:
        // Remove all_counters, add summaries, add qps, then re-add all_counters last
        const savedCounters = dev.all_counters;
        delete (dev as any).all_counters;

        dev.qp_count = devQps.length;
        dev.qps_in_error = devQps.filter((q) => q.state !== "RTS").length;
        dev.qp_summary = { total: devQps.length, by_state: byState };

        // Process summary
        const procMap = new Map<number, { comm: string; count: number }>();
        for (const qp of devQps) {
          if (qp.pid !== undefined) {
            const entry = procMap.get(qp.pid);
            if (entry) {
              entry.count++;
            } else {
              procMap.set(qp.pid, { comm: qp.comm ?? "unknown", count: 1 });
            }
          }
        }
        if (procMap.size > 0) {
          dev.process_summary = [...procMap.entries()]
            .map(([pid, { comm, count }]) => ({ pid, comm, qp_count: count }))
            .sort((a, b) => b.qp_count - a.qp_count);
        }

        dev.qps = devQps;

        // Re-add all_counters at the end for JSON output ordering
        dev.all_counters = savedCounters;
      }
    }

    // Strip all_counters when summary_only is requested
    if (summaryOnly) {
      for (const dev of healthDevices) {
        delete (dev as any).all_counters;
      }
    }

    // Generate warnings
    const warnings: RdmaHealthWarning[] = [];
    for (const dev of healthDevices) {
      warnings.push(...generateWarnings(dev, isDelta));
    }

    return okResponse<RdmaHealthData>(
      {
        devices: healthDevices,
        device_count: healthDevices.length,
        warnings,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "RDMA_HEALTH_ERROR",
      `Failed to read RDMA health: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
