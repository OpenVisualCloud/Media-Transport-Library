/**
 * pcm_memory_bandwidth(seconds, socket_filter)
 *
 * Per-socket memory bandwidth counters from Intel PCM:
 *   - DRAM reads/writes (bytes and derived MB/s)
 *   - Persistent Memory reads/writes
 *   - Embedded DRAM reads/writes
 *   - Memory Controller IA/GT/IO request bytes
 *   - Local vs remote memory request ratio (NUMA)
 *
 * Data source: pcm-sensor-server "Uncore Counters" per socket.
 *
 * When to use:
 *   - Diagnosing memory bandwidth saturation
 *   - Identifying NUMA-remote traffic at the socket level
 *   - Comparing DRAM vs PMM (Optane) bandwidth split
 *   - Understanding memory controller request sources (IA vs IO vs GPU)
 */
import type {
  ToolResponse,
  PcmMemoryBandwidthData,
  PcmMemoryBandwidthSocket,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num, sub } from "../collectors/pcm-bridge.js";

export async function pcmMemoryBandwidth(params: {
  seconds?: number;
  socket_filter?: number | null;
  host?: string;
}): Promise<ToolResponse<PcmMemoryBandwidthData>> {
  const seconds = params.seconds ?? 1;
  const socketFilter = params.socket_filter ?? null;

  const meta = await buildMeta("fallback", seconds * 1000);

  const bridge = getPcmBridgeForHost(params.host);
  await bridge.ensureRunning();
  if (!bridge.isAvailable) {
    return errorResponse(
      meta,
      "PCM_UNAVAILABLE",
      bridge.connectionError ?? "pcm-sensor-server is not available",
      "Start pcm-sensor-server with: sudo pcm-sensor-server -p 9738"
    );
  }

  try {
    const json = await bridge.getJson(seconds);
    const intervalUs = num(json, "Interval us");
    const intervalSec = intervalUs > 0 ? intervalUs / 1_000_000 : 1;

    const sockets: PcmMemoryBandwidthSocket[] = bridge.walkSockets(json, (socket) => {
      const socketId = num(socket, "Socket ID");
      if (socketFilter !== null && socketId !== socketFilter) return null;

      const uncoreBlock = sub(socket, "Uncore");
      const uc = sub(uncoreBlock, "Uncore Counters") ?? {};

      const dramReads = num(uc, "DRAM Reads");
      const dramWrites = num(uc, "DRAM Writes");

      return {
        socket: socketId,
        dram_reads_bytes: dramReads,
        dram_writes_bytes: dramWrites,
        dram_reads_mbps: Math.round((dramReads / intervalSec / (1024 * 1024)) * 100) / 100,
        dram_writes_mbps: Math.round((dramWrites / intervalSec / (1024 * 1024)) * 100) / 100,
        pmm_reads_bytes: num(uc, "Persistent Memory Reads"),
        pmm_writes_bytes: num(uc, "Persistent Memory Writes"),
        edram_reads_bytes: num(uc, "Embedded DRAM Reads"),
        edram_writes_bytes: num(uc, "Embedded DRAM Writes"),
        mc_ia_requests_bytes: num(uc, "Memory Controller IA Requests"),
        mc_gt_requests_bytes: num(uc, "Memory Controller GT Requests"),
        mc_io_requests_bytes: num(uc, "Memory Controller IO Requests"),
        local_memory_request_ratio_pct: num(uc, "Local Memory Request Ratio"),
        remote_memory_request_ratio_pct: num(uc, "Remote Memory Request Ratio"),
      };
    });

    return okResponse({ interval_us: intervalUs, sockets }, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM memory bandwidth: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
