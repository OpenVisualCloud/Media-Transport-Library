/**
 * nic_ethtool_stats — Read NIC hardware counters via ethtool -S.
 *
 * Returns key error/drop/traffic counters and optionally per-queue breakdowns.
 * Works with any Linux NIC driver (ice, mlx5, i40e, igb, etc.).
 *
 * Data sources:
 *   - `ethtool -S <iface>` — NIC statistics (350+ counters on ICE)
 *   - `ethtool -i <iface>` — driver name
 *
 * Universal — works with any NIC driver.
 */
import type { ToolResponse } from "../types.js";
import type {
  NicEthtoolStatsData,
  EthtoolCounter,
  EthtoolQueueStats,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Parse `ethtool -S <iface>` output into a Map of counter name → value.
 */
function parseEthtoolStats(output: string): Map<string, number> {
  const counters = new Map<string, number>();
  for (const line of output.split("\n")) {
    const m = line.match(/^\s+(\S+):\s+(\d+)\s*$/);
    if (m) {
      counters.set(m[1], parseInt(m[2], 10));
    }
  }
  return counters;
}

/**
 * Get NIC driver name from `ethtool -i`.
 */
async function getDriver(host: string, iface: string): Promise<string | null> {
  const output = await sshExecSafe(host, `ethtool -i ${iface} 2>/dev/null`);
  if (!output) return null;
  const m = output.match(/driver:\s+(\S+)/);
  return m ? m[1] : null;
}

/**
 * Get a counter value, trying multiple name variants (e.g. with/without .nic suffix).
 */
function getCounter(m: Map<string, number>, ...names: string[]): number {
  for (const name of names) {
    const v = m.get(name);
    if (v !== undefined) return v;
  }
  return 0;
}

/**
 * Extract per-queue stats from counter map.
 */
function extractQueues(counters: Map<string, number>): EthtoolQueueStats[] {
  const queueMap = new Map<string, EthtoolQueueStats>();

  for (const [name, value] of counters) {
    // tx_queue_0_packets, tx_queue_0_bytes, rx_queue_0_packets, ...
    const m = name.match(/^(tx|rx)_queue_(\d+)_(packets|bytes)$/);
    if (!m) continue;

    const direction = m[1] as "tx" | "rx";
    const queue = parseInt(m[2], 10);
    const field = m[3];
    const key = `${direction}_${queue}`;

    if (!queueMap.has(key)) {
      queueMap.set(key, { queue, direction, packets: 0, bytes: 0 });
    }
    const entry = queueMap.get(key)!;
    if (field === "packets") entry.packets = value;
    if (field === "bytes") entry.bytes = value;
  }

  return [...queueMap.values()].sort((a, b) => {
    if (a.direction !== b.direction) return a.direction < b.direction ? -1 : 1;
    return a.queue - b.queue;
  });
}

/**
 * Apply delta between two queue snapshots.
 */
function applyQueueDelta(before: EthtoolQueueStats[], after: EthtoolQueueStats[]): void {
  const beforeMap = new Map(before.map((q) => [`${q.direction}_${q.queue}`, q]));
  for (const q of after) {
    const bq = beforeMap.get(`${q.direction}_${q.queue}`);
    if (bq) {
      q.delta_packets = q.packets - bq.packets;
      q.delta_bytes = q.bytes - bq.bytes;
    }
  }
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function nicEthtoolStats(params: {
  host?: string;
  interface: string;
  seconds?: number;
  include_queues?: boolean;
  filter?: string;
  non_zero_only?: boolean;
}): Promise<ToolResponse<NicEthtoolStatsData>> {
  const host = params.host ?? "localhost";
  const iface = params.interface;
  const seconds = params.seconds ?? 0;
  const includeQueues = params.include_queues ?? false;
  const filterRe = params.filter ? new RegExp(params.filter, "i") : undefined;
  const nonZeroOnly = params.non_zero_only ?? true;

  const meta = await buildMeta("fallback", seconds > 0 ? seconds * 1000 : undefined);

  // Check ethtool availability
  const ethtoolCheck = await sshExecSafe(host, "command -v ethtool 2>/dev/null");
  if (!ethtoolCheck || !ethtoolCheck.trim()) {
    return errorResponse(
      meta,
      "ETHTOOL_MISSING",
      "ethtool not found on target host",
      "Install ethtool: apt-get install ethtool",
    );
  }

  try {
    const driver = await getDriver(host, iface);

    // Read first snapshot
    const output0 = await sshExecSafe(host, `ethtool -S ${iface} 2>/dev/null`, 15_000);
    if (!output0 || !output0.trim()) {
      return errorResponse(
        meta,
        "ETHTOOL_NO_STATS",
        `No statistics available for interface ${iface}`,
        "Check that the interface exists and the driver supports ethtool -S",
      );
    }

    const counters0 = parseEthtoolStats(output0);

    // Delta mode
    let countersFinal = counters0;
    let elapsedSec = 0;
    let counters1: Map<string, number> | null = null;

    if (seconds > 0) {
      const t0 = process.hrtime.bigint();
      await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
      const t1 = process.hrtime.bigint();
      elapsedSec = Number(t1 - t0) / 1e9;

      const output1 = await sshExecSafe(host, `ethtool -S ${iface} 2>/dev/null`, 15_000);
      if (output1) {
        counters1 = parseEthtoolStats(output1);
        countersFinal = counters1;
      }
    }

    // Build counter list
    let allCounters: EthtoolCounter[] = [];
    for (const [name, value] of [...countersFinal.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
      const entry: EthtoolCounter = { name, value };
      if (counters1 && counters0.has(name)) {
        entry.delta = value - (counters0.get(name) ?? 0);
        entry.rate_per_sec = Math.round((entry.delta / elapsedSec) * 100) / 100;
      }
      allCounters.push(entry);
    }

    // Apply filter
    if (filterRe) {
      allCounters = allCounters.filter((c) => filterRe.test(c.name));
    }

    // Apply non_zero_only filter
    if (nonZeroOnly) {
      allCounters = allCounters.filter((c) => {
        if (c.value !== 0) return true;
        if (c.delta !== undefined && c.delta !== 0) return true;
        return false;
      });
    }

    // Curated counters (check both plain and .nic suffix variants)
    const rxBytes = getCounter(countersFinal, "rx_bytes", "rx_bytes.nic");
    const txBytes = getCounter(countersFinal, "tx_bytes", "tx_bytes.nic");
    const rxUnicast = getCounter(countersFinal, "rx_unicast", "rx_unicast.nic");
    const txUnicast = getCounter(countersFinal, "tx_unicast", "tx_unicast.nic");
    const rxMulticast = getCounter(countersFinal, "rx_multicast", "rx_multicast.nic");
    const rxBroadcast = getCounter(countersFinal, "rx_broadcast", "rx_broadcast.nic");
    const rxDropped = getCounter(countersFinal, "rx_dropped", "rx_dropped.nic");
    const txErrors = getCounter(countersFinal, "tx_errors", "tx_errors.nic");
    const rxCrcErrors = getCounter(countersFinal, "rx_crc_errors", "rx_crc_errors.nic");
    const rxOversize = getCounter(countersFinal, "rx_oversize", "rx_oversize.nic");
    const txTimeout = getCounter(countersFinal, "tx_timeout", "tx_timeout.nic");

    const result: NicEthtoolStatsData = {
      interface: iface,
      driver,
      counter_count: countersFinal.size,

      rx_bytes: rxBytes,
      tx_bytes: txBytes,
      rx_unicast: rxUnicast,
      tx_unicast: txUnicast,
      rx_multicast: rxMulticast,
      rx_broadcast: rxBroadcast,
      rx_dropped: rxDropped,
      tx_errors: txErrors,
      rx_crc_errors: rxCrcErrors,
      rx_oversize: rxOversize,
      tx_timeout: txTimeout,

      counters: allCounters,
      warnings: [],
    };

    // Delta rates
    if (counters1) {
      result.delta_seconds = elapsedSec;
      const rxBytesBefore = getCounter(counters0, "rx_bytes", "rx_bytes.nic");
      const txBytesBefore = getCounter(counters0, "tx_bytes", "tx_bytes.nic");
      const rxUnicastBefore = getCounter(counters0, "rx_unicast", "rx_unicast.nic");
      const txUnicastBefore = getCounter(counters0, "tx_unicast", "tx_unicast.nic");

      result.rx_bytes_per_sec = Math.round((rxBytes - rxBytesBefore) / elapsedSec);
      result.tx_bytes_per_sec = Math.round((txBytes - txBytesBefore) / elapsedSec);
      result.rx_rate_gbps = Math.round(((rxBytes - rxBytesBefore) * 8 / elapsedSec / 1e9) * 1000) / 1000;
      result.tx_rate_gbps = Math.round(((txBytes - txBytesBefore) * 8 / elapsedSec / 1e9) * 1000) / 1000;
      result.rx_pps = Math.round((rxUnicast + rxMulticast + rxBroadcast - rxUnicastBefore - getCounter(counters0, "rx_multicast", "rx_multicast.nic") - getCounter(counters0, "rx_broadcast", "rx_broadcast.nic")) / elapsedSec);
      result.tx_pps = Math.round((txUnicast - txUnicastBefore) / elapsedSec);
    }

    // Queue stats
    if (includeQueues) {
      const queuesAfter = extractQueues(countersFinal);
      if (counters1) {
        const queuesBefore = extractQueues(counters0);
        applyQueueDelta(queuesBefore, queuesAfter);
      }
      result.queues = queuesAfter;
    }

    // Internal replication detection (E810 multicast):
    // tx_multicast.nic includes NIC-internal VF-to-VF replication.
    // Compare against sum of per-queue TX pkts (wire-only).
    const txMcastNic = getCounter(countersFinal, "tx_multicast.nic", "tx_multicast");
    if (txMcastNic > 0) {
      let wireTxPkts = 0;
      for (const [name, value] of countersFinal) {
        if (/^tx_queue_\d+_packets$/.test(name)) wireTxPkts += value;
      }
      if (wireTxPkts > 0 && txMcastNic > wireTxPkts * 1.1) {
        const replicationFactor = Math.round((txMcastNic / wireTxPkts) * 100) / 100;
        result.internal_replication = {
          tx_multicast_nic: txMcastNic,
          wire_tx_pkts: wireTxPkts,
          replication_factor: replicationFactor,
        };
        result.warnings.push(
          `${iface}: tx_multicast.nic (${txMcastNic}) >> wire TX (${wireTxPkts}), ` +
          `replication_factor=${replicationFactor} — likely NIC-internal VF-to-VF multicast replication`,
        );
      }
    }

    // Generate warnings
    if (rxDropped > 0) {
      result.warnings.push(`${iface}: ${rxDropped} rx_dropped (kernel ring buffer overflow or filter)`);
    }
    if (rxCrcErrors > 0) {
      result.warnings.push(`${iface}: ${rxCrcErrors} CRC errors (check cable/transceiver)`);
    }
    if (txErrors > 0) {
      result.warnings.push(`${iface}: ${txErrors} tx_errors`);
    }
    if (txTimeout > 0) {
      result.warnings.push(`${iface}: ${txTimeout} tx_timeout (NIC hang detected)`);
    }
    if (rxOversize > 0) {
      result.warnings.push(`${iface}: ${rxOversize} oversized frames (MTU mismatch?)`);
    }

    return okResponse(result, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "ETHTOOL_ERROR",
      `Failed to read ethtool stats: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
