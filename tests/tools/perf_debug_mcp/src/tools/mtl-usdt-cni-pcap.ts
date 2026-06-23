/**
 * mtl-usdt-cni-pcap.ts — CNI packet capture via USDT probe.
 *
 * Attaches to sys:cni_pcap_dump.
 *
 * WHY THIS IS A SEPARATE TOOL:
 *   MTL normally does NOT capture CNI (Control/Network/Interface) packets.
 *   When the sys:cni_pcap_dump USDT probe is attached, MTL starts dumping
 *   incoming CNI packets to pcap files on the tasklet path.
 *
 * ⚠ SIDE EFFECTS:
 *   - Creates pcap files on disk (one per port, every ~10s while attached)
 *   - Runs on the tasklet path — MAY AFFECT PERFORMANCE under heavy load
 *   - Files are named: cni_pN_10000_XXXXXX.pcapng
 *
 * PROBE SIGNATURE:
 *   cni_pcap_dump(int port, char* dump_file, uint32_t pkts)
 *     arg0 = DPDK port index (0, 1, ...)
 *     arg1 = pcap filename (string)
 *     arg2 = number of packets dumped in this batch
 *
 * USE WHEN:
 *   - Debugging unexpected packets on CNI path (ARP, IGMP, etc.)
 *   - Investigating packet drops or routing issues at the DPDK port level
 *   - Need pcap evidence for offline analysis (Wireshark/tshark)
 *
 * OUTPUT:
 *   Reports which pcap files were created, on which ports, with packet counts.
 *   Does NOT retrieve file contents — files remain on disk for manual analysis.
 */
import { z } from "zod";
import type { ToolResponse } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export const mtlUsdtCniPcapSchema = z.object({
  pid: z.number().optional().describe(
    "PID of the MTL process to trace. If omitted, discovers MTL processes automatically.",
  ),
  duration_sec: z.number().min(5).max(120).default(20).describe(
    "Capture duration in seconds (default: 20). CNI dumps every ~10s, so 20s gets ~2 dumps. " +
    "WARNING: Capture runs on tasklet path and may impact performance — keep short.",
  ),
});

export interface CniPcapDumpEntry {
  /** DPDK port index */
  port: number;
  /** Pcap filename reported by MTL */
  filename: string;
  /** Number of packets captured in this dump */
  packets: number;
  /** Timestamp string from bpftrace */
  timestamp?: string;
}

export interface CniPcapData {
  pid: number;
  process_name: string;
  duration_sec: number;
  /** Pcap dump events captured during the trace window */
  dumps: CniPcapDumpEntry[];
  /** Total packets across all dumps */
  total_packets: number;
  /** Unique pcap files created (deduplicated by filename) */
  unique_files: string[];
  /** Warning about tasklet-path performance impact */
  performance_warning: string;
}

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

export async function mtlUsdtCniPcap(
  params: z.infer<typeof mtlUsdtCniPcapSchema>,
): Promise<ToolResponse<CniPcapData>> {
  const meta = await buildMeta("usdt");
  const bridge = getBpftraceBridge();

  if (!bridge.isAvailable || !bridge.libmtlPath) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      "bpftrace or libmtl.so not available",
      "Install bpftrace and ensure MTL is built with USDT support");
  }

  let pid = params.pid;
  let processName = "";
  if (!pid) {
    const procs = await discoverMtlPids();
    if (procs.length === 0) {
      return errorResponse(meta, "NO_MTL_PROCESS",
        "No running MTL processes found", "Start an MTL pipeline first");
    }
    pid = procs[0].pid;
    processName = procs[0].name;
  } else {
    try {
      const comm = await sshExecSafe("localhost", `cat /proc/${pid}/comm 2>/dev/null`);
      processName = comm?.trim() ?? "";
    } catch { /* ignore */ }
  }

  const durationSec = params.duration_sec ?? 20;
  const libmtlPath = bridge.libmtlPath;

  // bpftrace script: attach cni_pcap_dump — MTL starts dumping when this probe is attached
  // Each fire reports: port, filename, packet_count
  const script = `
usdt:${libmtlPath}:sys:cni_pcap_dump {
  printf("CNI_DUMP:%s:p%d:%s:%u\\n", strftime("%H:%M:%S", nsecs), arg0, str(arg1), arg2);
}
`;

  const result = await bridge.runScript(script, pid, durationSec * 1000, {
    BPFTRACE_MAX_STRLEN: "200",
  });

  if (result.exitCode !== 0 && !result.timedOut) {
    return errorResponse(meta, "BPFTRACE_FAILED",
      `bpftrace failed (exit ${result.exitCode}): ${result.stderr.slice(0, 300)}`,
      "Ensure running as root and PID is valid");
  }

  // Parse CNI_DUMP lines: CNI_DUMP:HH:MM:SS:pN:filename:pkts
  const dumps: CniPcapDumpEntry[] = [];
  for (const line of result.stdout.split("\n")) {
    const m = line.match(/^CNI_DUMP:(\d{2}:\d{2}:\d{2}):p(\d+):(.+):(\d+)$/);
    if (m) {
      dumps.push({
        port: parseInt(m[2], 10),
        filename: m[3],
        packets: parseInt(m[4], 10),
        timestamp: m[1],
      });
    }
  }

  const totalPackets = dumps.reduce((s, d) => s + d.packets, 0);
  const uniqueFiles = [...new Set(dumps.map(d => d.filename))];

  const data: CniPcapData = {
    pid,
    process_name: processName,
    duration_sec: durationSec,
    dumps,
    total_packets: totalPackets,
    unique_files: uniqueFiles,
    performance_warning:
      "CNI pcap capture runs on the tasklet path and may impact real-time performance. " +
      "Keep capture duration short and monitor FPS during capture.",
  };

  return okResponse(data, meta);
}
