/**
 * nic_vf_stats — Per-VF packet/byte/drop counters on a given PF.
 *
 * For each SR-IOV Virtual Function on a Physical Function NIC, reports
 * traffic stats, drops, and multicast counters.  Optionally computes
 * rates via delta sampling.
 *
 * Data sources:
 *   - `ip -s link show <PF>` — VF representor stats
 *   - /sys/class/net/<PF>/device/virtfn<N>/ — VF BDFs
 *   - `ip -j link show` — VF info (MAC, trust, spoofcheck)
 *
 * Universal — works with any PF NIC that supports SR-IOV.
 */
import type { ToolResponse } from "../types.js";
import type { NicVfStatsData, VfStatsEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Parse VF stats from `ip -s link show <iface>` output.
 * VF info blocks follow the pattern:
 *   vf N ..., link-state ...
 *     RX: bytes  packets  mcast   bcast   dropped
 *     <values>
 *     TX: bytes  packets   dropped
 *     <values>
 */
async function readVfStats(host: string, iface: string): Promise<VfStatsEntry[]> {
  const output = await sshExecSafe(
    host,
    `ip -s link show ${iface} 2>/dev/null`,
    10_000,
  );
  if (!output) return [];

  const entries: VfStatsEntry[] = [];
  const lines = output.split("\n");

  let currentVf = -1;
  let expectRx = false;
  let expectTx = false;
  let currentEntry: VfStatsEntry | null = null;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    // vf N ...
    const vfMatch = line.match(/^\s+vf\s+(\d+)/);
    if (vfMatch) {
      if (currentEntry) entries.push(currentEntry);
      currentVf = parseInt(vfMatch[1], 10);
      currentEntry = {
        vf_index: currentVf,
        rx_bytes: 0,
        tx_bytes: 0,
        rx_pkts: 0,
        tx_pkts: 0,
        rx_dropped: 0,
        tx_dropped: 0,
        rx_multicast: 0,
      };
      expectRx = false;
      expectTx = false;
      continue;
    }

    if (!currentEntry) continue;

    // RX: header line
    if (/^\s+RX:/.test(line)) {
      expectRx = true;
      expectTx = false;
      continue;
    }

    // TX: header line
    if (/^\s+TX:/.test(line)) {
      expectTx = true;
      expectRx = false;
      continue;
    }

    // Data line after RX: header
    if (expectRx) {
      const vals = line.trim().split(/\s+/).map((v) => parseInt(v, 10));
      if (vals.length >= 5) {
        currentEntry.rx_bytes = vals[0] || 0;
        currentEntry.rx_pkts = vals[1] || 0;
        currentEntry.rx_multicast = vals[2] || 0;
        // broadcast at vals[3]
        currentEntry.rx_dropped = vals[4] || 0;
      }
      expectRx = false;
      continue;
    }

    // Data line after TX: header
    if (expectTx) {
      const vals = line.trim().split(/\s+/).map((v) => parseInt(v, 10));
      if (vals.length >= 3) {
        currentEntry.tx_bytes = vals[0] || 0;
        currentEntry.tx_pkts = vals[1] || 0;
        currentEntry.tx_dropped = vals[2] || 0;
      }
      expectTx = false;
      continue;
    }
  }

  if (currentEntry) entries.push(currentEntry);

  return entries;
}

/**
 * Resolve VF BDFs from sysfs virtfn* symlinks.
 */
async function resolveVfBdfs(host: string, iface: string): Promise<Map<number, string>> {
  const bdfs = new Map<number, string>();
  const script = `pci=$(readlink /sys/class/net/${iface}/device 2>/dev/null | xargs basename 2>/dev/null)
if [ -n "$pci" ]; then
  for vf in /sys/bus/pci/devices/$pci/virtfn*; do
    idx=\${vf##*virtfn}
    bdf=$(basename "$(readlink "$vf" 2>/dev/null)")
    echo "$idx|$bdf"
  done 2>/dev/null
fi`;
  const output = await sshExecSafe(host, script, 5_000);
  if (!output) return bdfs;

  for (const line of output.split("\n")) {
    const parts = line.split("|");
    if (parts.length === 2) {
      const idx = parseInt(parts[0], 10);
      if (!isNaN(idx)) bdfs.set(idx, parts[1]);
    }
  }
  return bdfs;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function nicVfStats(params: {
  host?: string;
  pf_interface: string;
  seconds?: number;
}): Promise<ToolResponse<NicVfStatsData>> {
  const host = params.host ?? "localhost";
  const iface = params.pf_interface;
  const seconds = params.seconds ?? 0;

  const meta = await buildMeta("fallback", seconds > 0 ? seconds * 1000 : undefined);

  try {
    // Read first snapshot
    const vfs0 = await readVfStats(host, iface);
    if (vfs0.length === 0) {
      return okResponse<NicVfStatsData>(
        { pf_interface: iface, vf_count: 0, vfs: [], warnings: [`No VFs found on ${iface}`] },
        meta,
      );
    }

    // Resolve BDFs
    const bdfs = await resolveVfBdfs(host, iface);
    for (const vf of vfs0) {
      vf.vf_bdf = bdfs.get(vf.vf_index);
    }

    // Delta mode
    if (seconds > 0) {
      const t0 = process.hrtime.bigint();
      await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
      const t1 = process.hrtime.bigint();
      const elapsedSec = Number(t1 - t0) / 1e9;

      const vfs1 = await readVfStats(host, iface);
      const map0 = new Map(vfs0.map((v) => [v.vf_index, v]));

      for (const vf of vfs1) {
        vf.vf_bdf = bdfs.get(vf.vf_index);
        const before = map0.get(vf.vf_index);
        if (before) {
          vf.delta_seconds = elapsedSec;
          vf.rx_bytes_per_sec = Math.round((vf.rx_bytes - before.rx_bytes) / elapsedSec);
          vf.tx_bytes_per_sec = Math.round((vf.tx_bytes - before.tx_bytes) / elapsedSec);
        }
      }

      const warnings: string[] = [];
      for (const vf of vfs1) {
        if (vf.rx_dropped > 0) warnings.push(`VF${vf.vf_index}: ${vf.rx_dropped} rx_dropped`);
        if (vf.tx_dropped > 0) warnings.push(`VF${vf.vf_index}: ${vf.tx_dropped} tx_dropped`);
      }

      return okResponse<NicVfStatsData>(
        { pf_interface: iface, vf_count: vfs1.length, vfs: vfs1, warnings },
        meta,
      );
    }

    // Snapshot mode
    const warnings: string[] = [];
    for (const vf of vfs0) {
      if (vf.rx_dropped > 0) warnings.push(`VF${vf.vf_index}: ${vf.rx_dropped} rx_dropped`);
      if (vf.tx_dropped > 0) warnings.push(`VF${vf.vf_index}: ${vf.tx_dropped} tx_dropped`);
    }

    return okResponse<NicVfStatsData>(
      { pf_interface: iface, vf_count: vfs0.length, vfs: vfs0, warnings },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "NIC_VF_STATS_ERROR",
      `Failed to read VF stats: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
