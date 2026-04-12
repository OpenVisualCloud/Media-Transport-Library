/**
 * pcm_iio — Per-PCIe-stack I/O bandwidth monitoring via pcm-iio CLI.
 *
 * Shows inbound (DMA into system) and outbound (CPU MMIO to device) bandwidth
 * broken down by PCIe IIO stack and partition/port within each stack.
 *
 * Metrics per stack/part:
 *   - IB write/read (bytes/sec) — PCIe device DMA to/from system memory
 *   - OB read/write (bytes/sec) — CPU MMIO to/from PCIe device
 *   - IOMMU metrics: IOTLB lookup/miss, context cache hit, page walk cache hits
 *
 * Much more granular than pcm_pcie_bandwidth (which is socket-aggregate only).
 *
 * Requires: pcm-iio binary in PATH, root access.
 */
import type { ToolResponse, PcmIioData, PcmIioEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

function parseIioCsv(raw: string, nonZeroOnly: boolean): PcmIioEntry[] {
  const entries: PcmIioEntry[] = [];

  // CSV header:
  // Date,Time,Socket,Name,Part,IB write,IB read,OB read,OB write,IOTLB Lookup,...
  const lines = raw.split("\n");
  let headerFound = false;
  let headers: string[] = [];

  for (const line of lines) {
    const trimmed = line.trim();

    if (trimmed.startsWith("Date,Time,Socket")) {
      headerFound = true;
      headers = trimmed.split(",").map(h => h.trim());
      continue;
    }

    if (!headerFound) continue;
    if (!trimmed || trimmed.startsWith("Cleaning") || trimmed.startsWith(" ")) continue;

    // Data lines start with a date
    if (!/^\d{4}-\d{2}-\d{2}/.test(trimmed)) continue;

    const parts = trimmed.split(",");
    if (parts.length < 9) continue;

    const socket = parts[2]?.trim() ?? "";
    const stackName = parts[3]?.trim() ?? "";
    const part = parts[4]?.trim() ?? "";
    const ibWrite = parseFloat(parts[5]) || 0;
    const ibRead = parseFloat(parts[6]) || 0;
    const obRead = parseFloat(parts[7]) || 0;
    const obWrite = parseFloat(parts[8]) || 0;
    const iotlbLookup = parts.length > 9 ? (parseFloat(parts[9]) || 0) : undefined;
    const iotlbMiss = parts.length > 10 ? (parseFloat(parts[10]) || 0) : undefined;

    if (nonZeroOnly && ibWrite === 0 && ibRead === 0 && obRead === 0 && obWrite === 0) {
      continue;
    }

    const entry: PcmIioEntry = {
      socket,
      stack_name: stackName,
      part,
      ib_write_bps: ibWrite,
      ib_read_bps: ibRead,
      ob_read_bps: obRead,
      ob_write_bps: obWrite,
    };

    if (iotlbLookup !== undefined && iotlbLookup > 0) entry.iotlb_lookup = iotlbLookup;
    if (iotlbMiss !== undefined && iotlbMiss > 0) entry.iotlb_miss = iotlbMiss;

    entries.push(entry);
  }

  return entries;
}

export async function pcmIio(params: {
  non_zero_only?: boolean;
  host?: string;
}): Promise<ToolResponse<PcmIioData>> {
  const host = params.host ?? "localhost";
  const nonZeroOnly = params.non_zero_only ?? true;

  const meta = await buildMeta("fallback");

  const cmd = `pcm-iio -csv -i=1 -silent 2>&1`;
  const result = await sshExec(host, cmd, 20_000);
  const raw = result.stdout;

  if (raw.includes("not found") || raw.includes("No such file")) {
    return errorResponse(
      meta,
      "PCM_NOT_FOUND",
      "pcm-iio binary not found",
      "Install Intel PCM: https://github.com/intel/pcm"
    );
  }

  if (result.exitCode !== 0 && !raw.includes("Date,Time")) {
    return errorResponse(
      meta,
      "PCM_EXEC_ERROR",
      `pcm-iio failed (exit ${result.exitCode}): ${raw.slice(0, 300)}`,
      "Ensure pcm-iio is installed and you have root privileges"
    );
  }

  const entries = parseIioCsv(raw, nonZeroOnly);

  return okResponse({ entries, non_zero_only: nonZeroOnly }, meta);
}
