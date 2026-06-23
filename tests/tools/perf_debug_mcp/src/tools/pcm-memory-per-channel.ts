/**
 * pcm_memory_per_channel — Per-channel memory bandwidth via pcm-memory CLI.
 *
 * More granular than pcm_memory_bandwidth: shows per-DDR-channel read/write
 * throughput (MB/s) plus optional PMM (Optane) metrics per channel.
 *
 * Per socket, 8 DDR channels are reported (Ch0-Ch7) with read/write/PMM_read/PMM_write.
 * Socket totals and system-wide totals are also provided.
 *
 * Requires: pcm-memory binary in PATH, root access.
 */
import type {
  ToolResponse,
  PcmMemoryPerChannelData,
  PcmMemorySocketData,
  PcmMemoryChannelData,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

/**
 * Parse pcm-memory CSV output.
 *
 * Header row (row 0): ,,SKT0,SKT0,...,SKT1,...,System,...
 * Header row (row 1): Date,Time,Ch0Read,Ch0Write,Ch0PMM_Read,Ch0PMM_Write,...,Mem Read (MB/s),...,Memory (MB/s),...
 * Data rows:          2026-04-02,07:58:51.607,8.90,7.54,0.00,...
 *
 * Per socket: Ch{0..N}Read,Ch{0..N}Write,Ch{0..N}PMM_Read,Ch{0..N}PMM_Write
 *             then Mem Read (MB/s), Mem Write (MB/s), PMM_Read (MB/s), PMM_Write (MB/s), Memory (MB/s)
 * System:     DRAMRead, DRAMWrite, PMMREAD, PMMWrite, Read, Write, Memory
 */
function parsePcmMemoryCsv(raw: string): {
  sockets: PcmMemorySocketData[];
  systemRead: number;
  systemWrite: number;
  systemTotal: number;
} {
  const lines = raw.split("\n").filter((l) => l.trim().length > 0);
  const sockets: PcmMemorySocketData[] = [];
  let systemRead = 0;
  let systemWrite = 0;
  let systemTotal = 0;

  // Find the socket-label row and header row
  let sktRow = -1;
  let hdrRow = -1;

  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes("SKT0") || lines[i].includes("SKT1")) {
      sktRow = i;
    }
    if (lines[i].startsWith("Date,Time,")) {
      hdrRow = i;
      break;
    }
  }
  if (hdrRow < 0) return { sockets, systemRead, systemWrite, systemTotal };

  const headers = lines[hdrRow].split(",").map((h) => h.trim());

  // Find the last data row (take the last one for a single-sample measurement)
  let dataLine = "";
  for (let i = lines.length - 1; i > hdrRow; i--) {
    if (/^\d{4}-\d{2}-\d{2}/.test(lines[i].trim())) {
      dataLine = lines[i];
      break;
    }
  }
  if (!dataLine) return { sockets, systemRead, systemWrite, systemTotal };

  const vals = dataLine.split(",").map((v) => parseFloat(v.trim()) || 0);

  // Identify socket boundaries from the socket-label row
  // Each socket group: N channels * 4 cols (Read, Write, PMM_Read, PMM_Write) + 5 totals
  // Then System group at end: 7 cols

  // Strategy: walk headers starting at index 2 (after Date, Time)
  // Detect socket spans using Ch0Read recurring
  const socketStarts: number[] = [];
  for (let i = 2; i < headers.length; i++) {
    if (headers[i] === "Ch0Read") socketStarts.push(i);
  }

  for (let s = 0; s < socketStarts.length; s++) {
    const start = socketStarts[s];
    const channels: PcmMemoryChannelData[] = [];

    // Read channels: each channel is 4 cols (ChNRead, ChNWrite, ChNPMM_Read, ChNPMM_Write)
    let i = start;
    let chIdx = 0;
    while (i < headers.length && headers[i].startsWith("Ch")) {
      channels.push({
        channel: chIdx,
        read_mbps: vals[i] || 0,
        write_mbps: vals[i + 1] || 0,
        pmm_read_mbps: vals[i + 2] || 0,
        pmm_write_mbps: vals[i + 3] || 0,
      });
      i += 4;
      chIdx++;
    }

    // After channels come socket totals: Mem Read (MB/s), Mem Write (MB/s),
    // PMM_Read (MB/s), PMM_Write (MB/s), Memory (MB/s)
    const totalRead = vals[i] || 0;
    const totalWrite = vals[i + 1] || 0;
    const totalPmmRead = vals[i + 2] || 0;
    const totalPmmWrite = vals[i + 3] || 0;
    const totalMemory = vals[i + 4] || 0;

    sockets.push({
      socket: s,
      channels,
      total_read_mbps: totalRead,
      total_write_mbps: totalWrite,
      total_pmm_read_mbps: totalPmmRead,
      total_pmm_write_mbps: totalPmmWrite,
      total_memory_mbps: totalMemory,
    });
  }

  // System-level (last 7 cols): DRAMRead, DRAMWrite, PMMREAD, PMMWrite, Read, Write, Memory
  const sysIdx = headers.indexOf("DRAMRead");
  if (sysIdx >= 0) {
    systemRead = vals[sysIdx] || 0; // DRAMRead
    systemWrite = vals[sysIdx + 1] || 0; // DRAMWrite
    // vals[sysIdx+4] = Read, vals[sysIdx+5] = Write
    systemTotal = vals[sysIdx + 6] || 0; // Memory
  }

  return { sockets, systemRead, systemWrite, systemTotal };
}

export async function pcmMemoryPerChannel(params: {
  host?: string;
}): Promise<ToolResponse<PcmMemoryPerChannelData>> {
  const host = params.host ?? "localhost";
  const meta = await buildMeta("fallback");

  const cmd = `pcm-memory -csv -i=1 -silent 2>&1`;
  const result = await sshExec(host, cmd, 20_000);
  const raw = result.stdout;

  if (raw.includes("not found") || raw.includes("No such file")) {
    return errorResponse(
      meta,
      "PCM_NOT_FOUND",
      "pcm-memory binary not found",
      "Install Intel PCM: https://github.com/intel/pcm"
    );
  }

  if (result.exitCode !== 0 && !raw.includes("Date,Time")) {
    return errorResponse(
      meta,
      "PCM_EXEC_ERROR",
      `pcm-memory failed (exit ${result.exitCode}): ${raw.slice(0, 300)}`,
      "Ensure pcm-memory is installed and you have root privileges"
    );
  }

  const parsed = parsePcmMemoryCsv(raw);

  return okResponse(
    {
      sockets: parsed.sockets,
      system_dram_read_mbps: parsed.systemRead,
      system_dram_write_mbps: parsed.systemWrite,
      system_total_memory_mbps: parsed.systemTotal,
    },
    meta
  );
}
