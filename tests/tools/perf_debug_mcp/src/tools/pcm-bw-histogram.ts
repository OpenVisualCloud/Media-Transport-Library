/**
 * pcm_bw_histogram — Memory bandwidth utilization histogram via pcm-memory CLI.
 *
 * Runs pcm-memory at a fast sampling interval (default 50ms) for a specified
 * duration, then bins the per-socket total memory bandwidth into 10 GB/s ranges.
 *
 * This reveals the *distribution* of bandwidth usage over time — useful for
 * detecting bursty vs. sustained workloads, or intermittent bandwidth saturation.
 *
 * Requires: pcm-memory binary in PATH, root access.
 */
import type {
  ToolResponse,
  PcmBwHistogramData,
  PcmBwHistogramBucket,
  PcmBwHistogramSocketData,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

const BIN_SIZE_GBPS = 10;
const MAX_BIN = 32; // up to 320 GB/s

function buildHistogram(values: number[]): PcmBwHistogramBucket[] {
  const counts = new Array<number>(MAX_BIN).fill(0);
  const n = values.length;
  if (n === 0) return [];

  for (const v of values) {
    const mbps = v; // value is already in MB/s
    const gbps = mbps / 1000;
    const bin = Math.min(Math.floor(gbps / BIN_SIZE_GBPS), MAX_BIN - 1);
    counts[bin]++;
  }

  const buckets: PcmBwHistogramBucket[] = [];
  for (let i = 0; i < MAX_BIN; i++) {
    if (counts[i] > 0) {
      buckets.push({
        range_gbps: `${i * BIN_SIZE_GBPS}-${(i + 1) * BIN_SIZE_GBPS}`,
        count: counts[i],
        time_pct: Math.round((counts[i] / n) * 10000) / 100,
      });
    }
  }
  return buckets;
}

function parseMemoryColumns(raw: string): {
  socketMemory: Map<number, number[]>;
} {
  // Replace semicolons with commas (some PCM versions use ; as CSV separator)
  const csv = raw.replace(/;/g, ",");
  const lines = csv.split("\n").filter((l) => l.trim().length > 0);
  const socketMemory = new Map<number, number[]>();

  // Find header row
  let hdrIdx = -1;
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes("Date,Time,")) {
      hdrIdx = i;
      break;
    }
  }
  if (hdrIdx < 0) return { socketMemory };

  const headers = lines[hdrIdx].split(",").map((h) => h.trim());

  // Find "Memory (MB/s)" columns — one per socket, then one for system
  // We need per-socket: find Memory (MB/s) columns that appear before
  // the system section (which starts with DRAMRead)
  const memColIndices: number[] = [];
  const dramReadIdx = headers.indexOf("DRAMRead");
  const limit = dramReadIdx > 0 ? dramReadIdx : headers.length;

  for (let i = 2; i < limit; i++) {
    if (headers[i] === "Memory (MB/s)") {
      memColIndices.push(i);
    }
  }

  // Initialize socket arrays
  for (let s = 0; s < memColIndices.length; s++) {
    socketMemory.set(s, []);
  }

  // Parse all data rows
  for (let i = hdrIdx + 1; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!/^\d{4}-\d{2}-\d{2}/.test(line)) continue;
    const cols = line.split(",");

    for (let s = 0; s < memColIndices.length; s++) {
      const val = parseFloat(cols[memColIndices[s]]) || 0;
      socketMemory.get(s)!.push(val);
    }
  }

  return { socketMemory };
}

export async function pcmBwHistogram(params: {
  duration_sec?: number;
  sample_interval_ms?: number;
  host?: string;
}): Promise<ToolResponse<PcmBwHistogramData>> {
  const host = params.host ?? "localhost";
  const durationSec = params.duration_sec ?? 2;
  const sampleIntervalMs = params.sample_interval_ms ?? 50;
  const sampleIntervalSec = sampleIntervalMs / 1000;

  const meta = await buildMeta("fallback", durationSec * 1000);

  // Run pcm-memory at fast interval, write CSV to temp file, cat + remove
  const cmd = [
    `TMP=$(mktemp /tmp/pcm_bw_XXXXXX)`,
    `pcm-memory ${sampleIntervalSec} -nc -csv=$TMP -- sleep ${durationSec} 2>/dev/null`,
    `cat $TMP`,
    `rm -f $TMP`,
  ].join(" && ");

  const timeout = (durationSec + 10) * 1000;
  const result = await sshExec(host, cmd, timeout);
  const raw = result.stdout;

  if (raw.includes("not found") || raw.includes("No such file")) {
    return errorResponse(
      meta,
      "PCM_NOT_FOUND",
      "pcm-memory binary not found",
      "Install Intel PCM: https://github.com/intel/pcm"
    );
  }

  if (!raw.includes("Date,Time")) {
    return errorResponse(
      meta,
      "PCM_EXEC_ERROR",
      `pcm-memory produced no CSV output: ${raw.slice(0, 300)}`,
      "Ensure pcm-memory is installed and you have root privileges"
    );
  }

  const { socketMemory } = parseMemoryColumns(raw);

  const sockets: PcmBwHistogramSocketData[] = [];
  for (const [socketId, values] of socketMemory) {
    sockets.push({
      socket: socketId,
      dram_histogram: buildHistogram(values),
    });
  }

  return okResponse(
    { duration_sec: durationSec, sockets },
    meta
  );
}
