/**
 * pcm_pcie_bandwidth(seconds, socket_filter)
 *
 * PCIe/memory-controller bandwidth from Intel PCM uncore counters:
 *   - Memory Controller IA Requests (CPU/application-generated)
 *   - Memory Controller GT Requests (integrated GPU)
 *   - Memory Controller IO Requests (PCIe/DMA devices)
 *
 * Data source: pcm-sensor-server "Uncore Counters" per socket.
 *
 * Note: This provides aggregate PCIe bandwidth at the memory controller level,
 * not per-port PCIe breakdown. For per-port PCIe bandwidth you would need
 * pcm-pcie or pcm-iio CLI tools (not available via pcm-sensor-server).
 *
 * When to use:
 *   - Estimating total PCIe/DMA bandwidth consumed by IO devices
 *   - Comparing CPU (IA) vs GPU (GT) vs IO memory requests
 *   - Identifying if IO bandwidth is a bottleneck
 */
import type {
  ToolResponse,
  PcmPcieBandwidthData,
  PcmPcieBandwidthSocket,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num, sub } from "../collectors/pcm-bridge.js";

export async function pcmPcieBandwidth(params: {
  seconds?: number;
  socket_filter?: number | null;
  host?: string;
}): Promise<ToolResponse<PcmPcieBandwidthData>> {
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

    const sockets: PcmPcieBandwidthSocket[] = bridge.walkSockets(json, (socket) => {
      const socketId = num(socket, "Socket ID");
      if (socketFilter !== null && socketId !== socketFilter) return null;

      const uncoreBlock = sub(socket, "Uncore");
      const uc = sub(uncoreBlock, "Uncore Counters") ?? {};

      return {
        socket: socketId,
        mc_ia_requests_bytes: num(uc, "Memory Controller IA Requests"),
        mc_gt_requests_bytes: num(uc, "Memory Controller GT Requests"),
        mc_io_requests_bytes: num(uc, "Memory Controller IO Requests"),
      };
    });

    return okResponse(
      {
        interval_us: intervalUs,
        sockets,
        note: "These are aggregate memory controller IO request bytes, not per-PCIe-port breakdown. For per-port PCIe bandwidth, use pcm-pcie or pcm-iio CLI tools.",
      },
      meta
    );
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM PCIe bandwidth: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
