/**
 * rdma_counters — RDMA transport-level counters from `rdma stat`.
 *
 * Reads hardware RDMA (RoCE/InfiniBand) transport counters: InRdmaWrites,
 * OutRdmaReads, cnpSent, cnpReceived, RxECNMrkd, etc.  These counters are
 * separate from the QP-state and hw_counters read by rdma_health — they
 * show real-time RDMA traffic rates and congestion notification events.
 *
 * Use case: PFC/ECN debugging — if cnpSent is rising, the NIC is being
 * told to slow down (ECN marks → CNP generation).  If RxECNMrkd is high,
 * upstream is marking packets.
 *
 * Source: `rdma stat show` (from rdma-core / iproute2-rdma).
 * Works on any host with RDMA-capable NICs and rdma-core installed.
 */
import type { ToolResponse, RdmaCountersData, RdmaPortCounters } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe, sshExec } from "../utils/ssh-exec.js";

export async function rdmaCounters(params: {
  host?: string;
  device?: string;
  seconds?: number;
}): Promise<ToolResponse<RdmaCountersData>> {
  const host = params.host ?? "localhost";
  const device = params.device;
  const seconds = params.seconds ?? 0;

  const windowMs = seconds > 0 ? seconds * 1000 : undefined;
  const meta = await buildMeta("fallback", windowMs);

  // Check rdma availability
  const check = await sshExecSafe(host, "command -v rdma 2>/dev/null");
  if (!check || !check.trim()) {
    return errorResponse(
      meta,
      "RDMA_MISSING",
      "rdma CLI tool not found on target host",
      "Install rdma-core: apt-get install rdma-core",
    );
  }

  try {
    // Read first sample
    // rdma stat show uses "link DEV/PORT" syntax, not "dev DEV"
    const devFilter = device
      ? ` link ${device.includes("/") ? device : `${device}/1`}`
      : "";
    const cmd = `rdma stat show${devFilter} 2>/dev/null`;
    const sample1 = await sshExecSafe(host, cmd, 10_000);
    if (!sample1 || !sample1.trim()) {
      return errorResponse(
        meta,
        "RDMA_STAT_NO_OUTPUT",
        "rdma stat show produced no output",
        "Ensure RDMA devices are present (check: rdma dev). Requires rdma-core package.",
      );
    }

    const devices1 = parseRdmaStatOutput(sample1);

    if (seconds <= 0) {
      // Snapshot mode
      const warnings = generateRdmaWarnings(devices1);
      return okResponse<RdmaCountersData>({
        devices: devices1,
        warnings,
      }, meta);
    }

    // Delta mode — take two samples
    await new Promise((r) => setTimeout(r, seconds * 1000));
    const sample2Raw = await sshExecSafe(host, cmd, 10_000);
    if (!sample2Raw) {
      return errorResponse(meta, "RDMA_STAT_SECOND_SAMPLE_FAILED", "Failed to collect second RDMA stat sample");
    }

    const devices2 = parseRdmaStatOutput(sample2Raw);
    const devicesWithRates = computeRdmaDeltas(devices1, devices2, seconds);
    const warnings = generateRdmaWarnings(devicesWithRates);

    return okResponse<RdmaCountersData>({
      devices: devicesWithRates,
      delta_seconds: seconds,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "RDMA_COUNTERS_ERROR",
      `Failed to read RDMA counters: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse `rdma stat show` output.
 *
 * Typical format:
 * ```
 * link rocep42s0/1 rx_write_requests 0 rx_read_requests 0 rx_atomic_requests 0 ...
 * ```
 */
function parseRdmaStatOutput(output: string): RdmaPortCounters[] {
  const lines = output.split("\n").filter((l) => l.trim().length > 0);
  const devices: RdmaPortCounters[] = [];

  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed.startsWith("link ")) continue;

    // "link rocep42s0/1 key1 val1 key2 val2 ..."
    const parts = trimmed.split(/\s+/);
    if (parts.length < 4) continue;

    const devPort = parts[1]; // "rocep42s0/1"
    const slashIdx = devPort.lastIndexOf("/");
    const deviceName = slashIdx >= 0 ? devPort.slice(0, slashIdx) : devPort;
    const port = slashIdx >= 0 ? parseInt(devPort.slice(slashIdx + 1), 10) : 1;

    const counters: Record<string, number> = {};
    for (let i = 2; i < parts.length - 1; i += 2) {
      const key = parts[i];
      const val = parseInt(parts[i + 1], 10);
      if (!isNaN(val)) {
        counters[key] = val;
      }
    }

    devices.push({ device: deviceName, port, counters });
  }

  return devices;
}

/**
 * Compute per-second rates between two samples.
 */
function computeRdmaDeltas(
  before: RdmaPortCounters[],
  after: RdmaPortCounters[],
  seconds: number,
): RdmaPortCounters[] {
  const beforeMap = new Map<string, RdmaPortCounters>();
  for (const d of before) {
    beforeMap.set(`${d.device}/${d.port}`, d);
  }

  return after.map((a) => {
    const key = `${a.device}/${a.port}`;
    const b = beforeMap.get(key);
    if (!b) return a;

    const rates: Record<string, number> = {};
    for (const [k, v] of Object.entries(a.counters)) {
      const prev = b.counters[k] ?? 0;
      const delta = v - prev;
      if (delta > 0 && seconds > 0) {
        rates[k] = Math.round((delta / seconds) * 100) / 100;
      }
    }

    return { ...a, rates };
  });
}

/**
 * Generate warnings for concerning RDMA counter values.
 */
function generateRdmaWarnings(devices: RdmaPortCounters[]): string[] {
  const warnings: string[] = [];

  for (const dev of devices) {
    const c = dev.rates ?? dev.counters;
    const prefix = `${dev.device}/${dev.port}`;

    // CNP sent = NIC is being congestion-notified (ECN marks → CNP)
    const cnpSent = c["cnp_sent"] ?? c["cnpSent"] ?? 0;
    if (cnpSent > 0) {
      warnings.push(`${prefix}: cnpSent=${cnpSent} — NIC responding to ECN congestion marks`);
    }

    // CNP received = this NIC being told to slow down
    const cnpRecv = c["cnp_received"] ?? c["cnpReceived"] ?? 0;
    if (cnpRecv > 0) {
      warnings.push(`${prefix}: cnpReceived=${cnpRecv} — NIC receiving congestion notifications (being told to slow down)`);
    }

    // ECN marked packets
    const ecnMrkd = c["rx_ecn_marked_pkts"] ?? c["RxECNMrkd"] ?? 0;
    if (ecnMrkd > 0) {
      warnings.push(`${prefix}: RxECNMrkd=${ecnMrkd} — incoming packets marked with ECN congestion`);
    }

    // Out-of-sequence
    const oos = c["out_of_sequence"] ?? 0;
    if (oos > 0) {
      warnings.push(`${prefix}: out_of_sequence=${oos} — packets arriving out of order`);
    }
  }

  return warnings;
}
