/**
 * pcm_qpi_upi_link(seconds)
 *
 * QPI/UPI (inter-socket) link traffic and utilization from Intel PCM:
 *   - Per-link incoming/outgoing data traffic (bytes)
 *   - Per-link incoming/outgoing utilization (0.0–1.0)
 *   - CXL write cache/mem bytes (when CXL is present)
 *
 * Data source: pcm-sensor-server "QPI/UPI Links" section.
 * Only meaningful on multi-socket systems. On single-socket systems,
 * the data will be empty or show zero values.
 *
 * When to use:
 *   - Diagnosing NUMA cross-socket traffic saturation
 *   - Identifying asymmetric UPI link utilization
 *   - Monitoring CXL device traffic
 *   - Understanding inter-socket bandwidth consumption
 */
import type {
  ToolResponse,
  PcmQpiUpiLinkData,
  PcmQpiSocketEntry,
  PcmQpiLinkEntry,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getPcmBridgeForHost, num } from "../collectors/pcm-bridge.js";

export async function pcmQpiUpiLink(params: {
  seconds?: number;
  host?: string;
}): Promise<ToolResponse<PcmQpiUpiLinkData>> {
  const seconds = params.seconds ?? 1;

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

    const rawLinks = bridge.extractQpiLinks(json);

    const sockets: PcmQpiSocketEntry[] = rawLinks.map(({ socket, links: rawLinkData }) => {
      // Extract CXL counters
      const cxlWriteCache = num(rawLinkData, "CXL Write Cache");
      const cxlWriteMem = num(rawLinkData, "CXL Write Mem");

      // Extract per-link data
      // Keys follow the pattern:
      //   "Incoming Data Traffic On Link 0"
      //   "Outgoing Data And Non-Data Traffic On Link 0"
      //   "Utilization Incoming Data Traffic On Link 0"
      //   "Utilization Outgoing Data And Non-Data Traffic On Link 0"
      const linkMap = new Map<number, Partial<PcmQpiLinkEntry>>();

      for (const [key, value] of Object.entries(rawLinkData)) {
        const incomingMatch = key.match(/^Incoming Data Traffic On Link (\d+)$/);
        if (incomingMatch) {
          const linkId = parseInt(incomingMatch[1], 10);
          const entry = linkMap.get(linkId) ?? { link: linkId };
          entry.incoming_bytes = typeof value === "number" ? value : 0;
          linkMap.set(linkId, entry);
          continue;
        }

        const outgoingMatch = key.match(/^Outgoing Data And Non-Data Traffic On Link (\d+)$/);
        if (outgoingMatch) {
          const linkId = parseInt(outgoingMatch[1], 10);
          const entry = linkMap.get(linkId) ?? { link: linkId };
          entry.outgoing_bytes = typeof value === "number" ? value : 0;
          linkMap.set(linkId, entry);
          continue;
        }

        const utilInMatch = key.match(/^Utilization Incoming Data Traffic On Link (\d+)$/);
        if (utilInMatch) {
          const linkId = parseInt(utilInMatch[1], 10);
          const entry = linkMap.get(linkId) ?? { link: linkId };
          entry.incoming_utilization = typeof value === "number" ? Math.round(value * 10000) / 10000 : 0;
          linkMap.set(linkId, entry);
          continue;
        }

        const utilOutMatch = key.match(/^Utilization Outgoing Data And Non-Data Traffic On Link (\d+)$/);
        if (utilOutMatch) {
          const linkId = parseInt(utilOutMatch[1], 10);
          const entry = linkMap.get(linkId) ?? { link: linkId };
          entry.outgoing_utilization = typeof value === "number" ? Math.round(value * 10000) / 10000 : 0;
          linkMap.set(linkId, entry);
          continue;
        }
      }

      // Convert the map to sorted array, filling in missing fields
      const links: PcmQpiLinkEntry[] = [...linkMap.values()]
        .map((entry) => ({
          link: entry.link ?? 0,
          incoming_bytes: entry.incoming_bytes ?? 0,
          outgoing_bytes: entry.outgoing_bytes ?? 0,
          incoming_utilization: entry.incoming_utilization ?? 0,
          outgoing_utilization: entry.outgoing_utilization ?? 0,
        }))
        .sort((a, b) => a.link - b.link);

      return {
        socket,
        cxl_write_cache_bytes: cxlWriteCache,
        cxl_write_mem_bytes: cxlWriteMem,
        links,
      };
    });

    return okResponse({ interval_us: intervalUs, sockets }, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCM_FETCH_ERROR",
      `Failed to fetch PCM QPI/UPI link data: ${err?.message ?? err}`,
      "Ensure pcm-sensor-server is running and accessible"
    );
  }
}
