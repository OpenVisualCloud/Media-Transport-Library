/**
 * mtl-usdt-frame-trace.ts — Real-time frame lifecycle tracing via USDT probes.
 *
 * Traces st20/st20p/st22/st22p frame events:
 *   TX: tx_frame_next → tx_frame_done (frame lifecycle)
 *   RX: rx_frame_available → rx_frame_put (delivery lifecycle)
 *   Errors: rx_frame_incomplete, rx_no_framebuffer, tx_frame_drop
 *
 * Computes:
 *   - Real-time FPS from inter-frame intervals
 *   - Inter-frame interval statistics (avg/min/max/stddev)
 *   - Incomplete frame count, no-framebuffer count
 *   - Per-session breakdown
 *
 * This provides frame-level visibility that's impossible from log-based stat dumps,
 * which only report aggregated 10-second counters.
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import type { UsdtFrameTraceData, UsdtFrameTraceStream, UsdtFrameEvent } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export const mtlUsdtFrameTraceSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process to trace. If omitted, discovers automatically.",
  ),
  duration_sec: z.number().min(1).max(30).optional().describe(
    "Trace duration in seconds (default: 5)",
  ),
  protocol: z.enum(["st20", "st20p", "st22", "st22p", "all"]).optional().describe(
    "Which protocol to trace (default: all)",
  ),
  direction: z.enum(["tx", "rx", "both"]).optional().describe(
    "Trace TX, RX, or both directions (default: both)",
  ),
  max_events: z.number().optional().describe(
    "Maximum events to capture per stream (default: 1000, saves memory on high-FPS streams)",
  ),
});

/**
 * Discover MTL PIDs (same logic as session-stats).
 */
async function discoverMtlPids(): Promise<{ pid: number; name: string }[]> {
  const output = await sshExecSafe("localhost",
    `for pid in /proc/[0-9]*/task/*/comm; do
      if grep -qP '^mtl_sch_' "$pid" 2>/dev/null; then
        ppid=$(echo "$pid" | cut -d/ -f3)
        comm=$(cat "/proc/$ppid/comm" 2>/dev/null)
        echo "$ppid|$comm"
      fi
    done | sort -u -t'|' -k1,1`);

  if (!output?.trim()) return [];
  const seen = new Set<number>();
  return output.trim().split("\n").flatMap(line => {
    const [pidStr, name] = line.split("|");
    const pid = parseInt(pidStr, 10);
    if (!isNaN(pid) && !seen.has(pid)) { seen.add(pid); return [{ pid, name: name ?? "" }]; }
    return [];
  });
}

/**
 * Build bpftrace script for frame-level tracing.
 *
 * Strategy: use printf with structured delimiters for each event type.
 * Format: FRAME_EVENT:<provider>:<probe>:<m_idx>:<s_idx>:<f_idx>:<nsec>[:extra=val]
 *
 * USDT probe signatures from mt_usdt_provider.d:
 *   st20:tx_frame_next(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st20:tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st20:rx_frame_available(int m_idx, int s_idx, int f_idx, uint32_t tmstamp, uint64_t data_addr)
 *   st20:rx_frame_put(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st20:rx_frame_incomplete(int m_idx, int s_idx, int f_idx, uint32_t tmstamp, uint32_t data_size, uint32_t expect_size)
 *   st20:rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp)
 *   st20p:tx_frame_get(int m_idx, int s_idx, int f_idx)
 *   st20p:tx_frame_put(int m_idx, int s_idx, int f_idx)
 *   st20p:tx_frame_drop(int m_idx, int s_idx, int f_idx)
 *   st20p:rx_frame_get(int m_idx, int s_idx, int f_idx)
 *   st20p:rx_frame_put(int m_idx, int s_idx, int f_idx)
 *   st20p:rx_frame_available(int m_idx, int s_idx, int f_idx)
 *   st22:tx_frame_next(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st22:tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st22:rx_frame_available(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st22:rx_frame_put(int m_idx, int s_idx, int f_idx, uint32_t tmstamp)
 *   st22:rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp)
 *   st22p: same as st20p + encode/decode get/put
 */
function buildFrameTraceScript(
  libmtlPath: string,
  protocol: string,
  direction: string,
): string {
  const probes: string[] = [];
  const protocols = protocol === "all" ? ["st20", "st22", "st20p", "st22p"] : [protocol];

  for (const proto of protocols) {
    const isPipeline = proto.endsWith("p");

    if (direction === "tx" || direction === "both") {
      if (isPipeline) {
        // st20p/st22p: tx_frame_get, tx_frame_put (+ tx_frame_drop for st20p only; st22p lacks it)
        probes.push(`usdt:${libmtlPath}:${proto}:tx_frame_get { printf("FRAME_EVENT:${proto}:tx_frame_get:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
        probes.push(`usdt:${libmtlPath}:${proto}:tx_frame_put { printf("FRAME_EVENT:${proto}:tx_frame_put:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
        if (proto !== "st22p") {
          probes.push(`usdt:${libmtlPath}:${proto}:tx_frame_drop { printf("FRAME_EVENT:${proto}:tx_frame_drop:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
        }
      } else {
        // st20/st22: tx_frame_next, tx_frame_done (with tmstamp arg3)
        probes.push(`usdt:${libmtlPath}:${proto}:tx_frame_next { printf("FRAME_EVENT:${proto}:tx_frame_next:%d:%d:%d:%lld:tmstamp=%d\\n", arg0, arg1, arg2, nsecs, arg3); }`);
        probes.push(`usdt:${libmtlPath}:${proto}:tx_frame_done { printf("FRAME_EVENT:${proto}:tx_frame_done:%d:%d:%d:%lld:tmstamp=%d\\n", arg0, arg1, arg2, nsecs, arg3); }`);
      }
    }

    if (direction === "rx" || direction === "both") {
      if (isPipeline) {
        // st20p/st22p: rx_frame_available, rx_frame_get, rx_frame_put
        probes.push(`usdt:${libmtlPath}:${proto}:rx_frame_available { printf("FRAME_EVENT:${proto}:rx_frame_available:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
        probes.push(`usdt:${libmtlPath}:${proto}:rx_frame_get { printf("FRAME_EVENT:${proto}:rx_frame_get:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
        probes.push(`usdt:${libmtlPath}:${proto}:rx_frame_put { printf("FRAME_EVENT:${proto}:rx_frame_put:%d:%d:%d:%lld\\n", arg0, arg1, arg2, nsecs); }`);
      } else {
        // st20/st22: rx_frame_available, rx_frame_put (with tmstamp)
        probes.push(`usdt:${libmtlPath}:${proto}:rx_frame_available { printf("FRAME_EVENT:${proto}:rx_frame_available:%d:%d:%d:%lld:tmstamp=%d\\n", arg0, arg1, arg2, nsecs, arg3); }`);
        probes.push(`usdt:${libmtlPath}:${proto}:rx_frame_put { printf("FRAME_EVENT:${proto}:rx_frame_put:%d:%d:%d:%lld:tmstamp=%d\\n", arg0, arg1, arg2, nsecs, arg3); }`);
      }

      // Error probes (raw protocol only)
      if (!isPipeline) {
        if (proto === "st20") {
          probes.push(`usdt:${libmtlPath}:st20:rx_frame_incomplete { printf("FRAME_EVENT:st20:rx_frame_incomplete:%d:%d:%d:%lld:data_size=%d:expect_size=%d\\n", arg0, arg1, arg2, nsecs, arg4, arg5); }`);
        }
        probes.push(`usdt:${libmtlPath}:${proto}:rx_no_framebuffer { printf("FRAME_EVENT:${proto}:rx_no_framebuffer:%d:%d:0:%lld\\n", arg0, arg1, nsecs); }`);
      }
    }
  }

  return probes.join("\n");
}

/**
 * Parse bpftrace output into structured frame events.
 */
function parseFrameEvents(
  stdout: string,
  maxEventsPerStream: number,
): Map<string, UsdtFrameEvent[]> {
  // Key: "protocol:session_idx:direction"
  const streams = new Map<string, UsdtFrameEvent[]>();

  for (const line of stdout.split("\n")) {
    if (!line.startsWith("FRAME_EVENT:")) continue;

    const parts = line.slice("FRAME_EVENT:".length).split(":");
    if (parts.length < 6) continue;

    const provider = parts[0];
    const probe = parts[1];
    const m_idx = parseInt(parts[2], 10);
    const s_idx = parseInt(parts[3], 10);
    const f_idx = parseInt(parts[4], 10);
    const timestamp_ns = parseInt(parts[5], 10);

    // Parse extra fields (key=value pairs after the 6th field)
    const extra: Record<string, number> = {};
    for (let i = 6; i < parts.length; i++) {
      const kv = parts[i].match(/(\w+)=(-?\d+)/);
      if (kv) extra[kv[1]] = parseInt(kv[2], 10);
    }

    // Determine direction
    const direction = probe.startsWith("tx_") ? "tx" : "rx";
    const key = `${provider}:${s_idx}:${direction}`;

    let events = streams.get(key);
    if (!events) {
      events = [];
      streams.set(key, events);
    }

    if (events.length >= maxEventsPerStream) continue;

    events.push({
      timestamp_ns,
      provider,
      probe,
      m_idx,
      s_idx,
      f_idx,
      extra: Object.keys(extra).length > 0 ? extra : undefined,
    });
  }

  return streams;
}

/**
 * Compute stream-level statistics from frame events.
 */
function computeStreamStats(events: UsdtFrameEvent[]): Partial<UsdtFrameTraceStream> {
  const stats: Partial<UsdtFrameTraceStream> = {};

  // Count error events
  stats.incomplete_count = events.filter(e => e.probe === "rx_frame_incomplete").length;
  stats.no_framebuffer_count = events.filter(e => e.probe === "rx_no_framebuffer").length;
  stats.drop_count = events.filter(e => e.probe === "tx_frame_drop").length;

  // Compute IFI from "done" or "available" events (frame delivery points)
  const deliveryProbes = ["tx_frame_done", "tx_frame_put", "rx_frame_available"];
  const deliveryEvents = events.filter(e => deliveryProbes.includes(e.probe));

  if (deliveryEvents.length >= 2) {
    deliveryEvents.sort((a, b) => a.timestamp_ns - b.timestamp_ns);
    const intervals: number[] = [];
    for (let i = 1; i < deliveryEvents.length; i++) {
      intervals.push(deliveryEvents[i].timestamp_ns - deliveryEvents[i - 1].timestamp_ns);
    }

    if (intervals.length > 0) {
      const avg = intervals.reduce((s, v) => s + v, 0) / intervals.length;
      const min = Math.min(...intervals);
      const max = Math.max(...intervals);
      const variance = intervals.reduce((s, v) => s + (v - avg) ** 2, 0) / intervals.length;

      stats.ifi_avg_ns = Math.round(avg);
      stats.ifi_min_ns = min;
      stats.ifi_max_ns = max;
      stats.ifi_stddev_ns = Math.round(Math.sqrt(variance));

      // FPS = 1e9 / avg_ifi
      if (avg > 0) {
        stats.computed_fps = Math.round((1e9 / avg) * 100) / 100;
      }
    }
  }

  return stats;
}

export async function mtlUsdtFrameTrace(
  params: z.infer<typeof mtlUsdtFrameTraceSchema>,
): Promise<ToolResponse<UsdtFrameTraceData>> {
  const bridge = getBpftraceBridge();
  const meta = await buildMeta("usdt");

  if (!bridge.isAvailable || !bridge.libmtlPath) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      bridge.isAvailable ? "libmtl.so not found" : "bpftrace not available",
      "Install bpftrace and MTL with USDT support");
  }

  const durationSec = params.duration_sec ?? 5;
  const protocol = params.protocol ?? "all";
  const direction = params.direction ?? "both";
  const maxEvents = params.max_events ?? 1000;

  // Discover PID
  let pid = params.pid;
  let processName = "";
  if (!pid) {
    const procs = await discoverMtlPids();
    if (procs.length === 0) {
      return errorResponse(meta, "NO_MTL_PROCESS", "No running MTL processes found");
    }
    pid = procs[0].pid;
    processName = procs[0].name;
  } else {
    const comm = await sshExecSafe("localhost", `cat /proc/${pid}/comm 2>/dev/null`);
    processName = comm?.trim() || "";
  }

  // Build and run the tracing script
  const script = buildFrameTraceScript(bridge.libmtlPath, protocol, direction);
  const result = await bridge.runScript(script, pid, durationSec * 1000);

  if (result.exitCode !== 0 && !result.timedOut && !result.stdout.includes("FRAME_EVENT:")) {
    return errorResponse(meta, "BPFTRACE_FAILED",
      `bpftrace exited with code ${result.exitCode}: ${result.stderr.slice(0, 300)}`,
      "Ensure bpftrace runs as root and the process is still alive");
  }

  // Parse events
  const streamEvents = parseFrameEvents(result.stdout, maxEvents);

  // Build stream summaries
  const streams: UsdtFrameTraceStream[] = [];
  let totalEvents = 0;

  for (const [key, events] of streamEvents) {
    const [proto, sidxStr, dir] = key.split(":");
    const stats = computeStreamStats(events);
    totalEvents += events.length;

    streams.push({
      session_idx: parseInt(sidxStr, 10),
      direction: dir as "tx" | "rx",
      protocol: proto as "st20" | "st20p" | "st22" | "st22p",
      events,
      event_count: events.length,
      computed_fps: stats.computed_fps,
      incomplete_count: stats.incomplete_count,
      no_framebuffer_count: stats.no_framebuffer_count,
      drop_count: stats.drop_count,
      ifi_avg_ns: stats.ifi_avg_ns,
      ifi_min_ns: stats.ifi_min_ns,
      ifi_max_ns: stats.ifi_max_ns,
      ifi_stddev_ns: stats.ifi_stddev_ns,
    });
  }

  // Sort by session_idx
  streams.sort((a, b) => a.session_idx - b.session_idx || a.direction.localeCompare(b.direction));

  return okResponse<UsdtFrameTraceData>({
    pid,
    process_name: processName,
    duration_ms: durationSec * 1000,
    streams,
    stream_count: streams.length,
    total_events: totalEvents,
  }, meta);
}
