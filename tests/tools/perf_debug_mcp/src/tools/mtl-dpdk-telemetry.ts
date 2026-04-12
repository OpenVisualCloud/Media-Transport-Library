/**
 * mtl_dpdk_telemetry — Query DPDK telemetry sockets for MTL instances.
 *
 * Connects to DPDK telemetry SOCK_SEQPACKET sockets on the target host and
 * queries ethdev stats/info, mempool utilization, EAL config, and heap info.
 *
 * Since SOCK_SEQPACKET is local-only, for remote hosts we execute a small
 * Python script via SSH that connects to the socket and queries endpoints.
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - dpdk_base_dir: base directory for DPDK runtime sockets (default /var/run/dpdk)
 *   - file_prefix: DPDK EAL file prefix (default MT_DPDK — MTL's default)
 *   - endpoints: which telemetry data to collect (ethdev_stats, ethdev_info, mempool, heap, eal)
 */
import type { ToolResponse } from "../types.js";
import type {
  MtlDpdkTelemetryData,
  DpdkTelemetryInstanceData,
  DpdkEthdevStats,
  DpdkEthdevInfo,
  DpdkMempoolInfo,
  DpdkHeapInfo,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe, sshExecOrThrow } from "../utils/ssh-exec.js";

/**
 * Python one-liner (heredoc) that connects to a DPDK telemetry socket,
 * sends commands, and returns results as JSON.
 *
 * Parameter: $SOCK — the socket path.
 * Parameter: $CMDS — newline-separated list of telemetry commands.
 *
 * Output: JSON object { "<command>": <response>, ... }
 */
function buildTelemetryScript(socketPath: string, commands: string[]): string {
  const cmdsJson = JSON.stringify(commands);
  // Using a heredoc so we don't have quoting nightmares over SSH
  return `python3 - <<'__DPDK_TEL_EOF__'
import socket, json, sys, os

SOCK_PATH = ${JSON.stringify(socketPath)}
CMDS = ${cmdsJson}

def query(sock, cmd):
    sock.send(cmd.encode())
    data = b""
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        data += chunk
        try:
            json.loads(data.decode())
            break
        except json.JSONDecodeError:
            continue
    try:
        return json.loads(data.decode())
    except Exception:
        return None

try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(5)
    s.connect(SOCK_PATH)
    init = s.recv(4096)  # initial JSON banner
    results = {}
    for cmd in CMDS:
        try:
            r = query(s, cmd)
            if r is not None:
                v = r.get(cmd.split(",")[0], r)
                results[cmd] = v
        except Exception as e:
            results[cmd] = {"error": str(e)}
    s.close()
    print(json.dumps(results))
except Exception as e:
    print(json.dumps({"__error__": str(e)}))
__DPDK_TEL_EOF__`;
}

function parseEthdevStats(raw: Record<string, unknown>, portId: number): DpdkEthdevStats {
  const r = (raw ?? {}) as Record<string, number>;
  return {
    port_id: portId,
    name: "",
    ipackets: r.ipackets ?? 0,
    opackets: r.opackets ?? 0,
    ibytes: r.ibytes ?? 0,
    obytes: r.obytes ?? 0,
    imissed: r.imissed ?? 0,
    ierrors: r.ierrors ?? 0,
    oerrors: r.oerrors ?? 0,
  };
}

function parseEthdevInfo(raw: Record<string, unknown>, portId: number): DpdkEthdevInfo {
  const r = raw as Record<string, unknown>;
  return {
    port_id: portId,
    name: String(r.name ?? ""),
    driver: String(r.driver_name ?? ""),
    mtu: Number(r.mtu ?? 0),
    mac: String(r.mac_addr ?? ""),
    numa_node: Number(r.numa_node ?? -1),
    rx_queues: Number(r.nb_rx_queues ?? 0),
    tx_queues: Number(r.nb_tx_queues ?? 0),
    link_speed: Number(r.link_speed ?? 0),
    link_status: String(r.link_status ?? "unknown"),
    link_duplex: String(r.link_duplex ?? "unknown"),
  };
}

export async function mtlDpdkTelemetry(params: {
  host?: string;
  dpdk_base_dir?: string;
  file_prefix?: string;
  endpoints?: string[];
}): Promise<ToolResponse<MtlDpdkTelemetryData>> {
  const host = params.host ?? "localhost";
  const baseDir = params.dpdk_base_dir ?? "/var/run/dpdk";
  const filePrefix = params.file_prefix ?? "MT_DPDK";
  const endpoints = params.endpoints ?? ["ethdev_stats", "ethdev_info", "mempool", "heap", "eal"];

  const meta = await buildMeta("fallback");

  try {
    // Find all telemetry sockets for this file prefix
    const sockList = await sshExecSafe(
      host,
      `find ${baseDir}/${filePrefix}/ -maxdepth 1 -name 'dpdk_telemetry.v2*' -type s 2>/dev/null | sort`,
    );
    if (!sockList || !sockList.trim()) {
      return okResponse<MtlDpdkTelemetryData>(
        { instances: [], instance_count: 0 },
        meta,
      );
    }

    const socketPaths = sockList.trim().split("\n").filter(Boolean);
    const instances: DpdkTelemetryInstanceData[] = [];

    for (const sockPath of socketPaths) {
      // Build command list based on requested endpoints
      const commands: string[] = [];

      // Always get port list first
      commands.push("/ethdev/list");

      if (endpoints.includes("eal")) {
        commands.push("/eal/params");
        commands.push("/eal/lcore/list");
      }

      // We'll need port IDs to query per-port endpoints.  First pass: get port list.
      const listScript = buildTelemetryScript(sockPath, ["/ethdev/list"]);
      const listResult = await sshExecSafe(host, listScript);
      let portIds: number[] = [];
      if (listResult) {
        try {
          const parsed = JSON.parse(listResult.trim());
          const listVal = parsed["/ethdev/list"];
          if (Array.isArray(listVal)) portIds = listVal;
        } catch { /* ignore */ }
      }

      // Build full command set including per-port queries
      const allCommands: string[] = [];
      if (endpoints.includes("eal")) {
        allCommands.push("/eal/params", "/eal/lcore/list");
      }
      for (const portId of portIds) {
        if (endpoints.includes("ethdev_stats")) {
          allCommands.push(`/ethdev/stats,${portId}`);
        }
        if (endpoints.includes("ethdev_info")) {
          allCommands.push(`/ethdev/info,${portId}`);
          allCommands.push(`/ethdev/link_status,${portId}`);
        }
      }
      if (endpoints.includes("mempool")) {
        allCommands.push("/mempool/list");
      }
      if (endpoints.includes("heap")) {
        allCommands.push("/eal/heap_list");
      }

      // Execute full query
      const script = buildTelemetryScript(sockPath, allCommands);
      const output = await sshExecSafe(host, script, 20_000);
      if (!output) continue;

      let telData: Record<string, unknown>;
      try {
        telData = JSON.parse(output.trim());
      } catch {
        continue;
      }

      if (telData.__error__) continue;

      // Parse EAL info
      const ealParams = telData["/eal/params"] as string[] | undefined;
      const ealLcores = telData["/eal/lcore/list"] as number[] | undefined;

      // Parse ethdev stats and info
      const ethdevStats: DpdkEthdevStats[] = [];
      const ethdevInfo: DpdkEthdevInfo[] = [];

      for (const portId of portIds) {
        if (endpoints.includes("ethdev_stats")) {
          const statsRaw = telData[`/ethdev/stats,${portId}`] as Record<string, unknown> | undefined;
          if (statsRaw) {
            const stats = parseEthdevStats(statsRaw, portId);
            // Try to get name from info
            const infoRaw = telData[`/ethdev/info,${portId}`] as Record<string, unknown> | undefined;
            if (infoRaw) stats.name = String(infoRaw.name ?? "");
            ethdevStats.push(stats);
          }
        }
        if (endpoints.includes("ethdev_info")) {
          const infoRaw = telData[`/ethdev/info,${portId}`] as Record<string, unknown> | undefined;
          const linkRaw = telData[`/ethdev/link_status,${portId}`] as Record<string, unknown> | undefined;
          if (infoRaw) {
            const info = parseEthdevInfo(infoRaw, portId);
            if (linkRaw) {
              info.link_speed = Number(linkRaw.speed ?? info.link_speed);
              info.link_status = String(linkRaw.status ?? info.link_status);
              info.link_duplex = String(linkRaw.duplex ?? info.link_duplex);
            }
            ethdevInfo.push(info);
          }
        }
      }

      // Parse mempool info
      const mempoolInfo: DpdkMempoolInfo[] = [];
      if (endpoints.includes("mempool")) {
        const poolList = telData["/mempool/list"] as string[] | undefined;
        if (poolList && poolList.length > 0) {
          // Query individual pools (batch)
          const poolCmds = poolList.map((p) => `/mempool/info,${p}`);
          const poolScript = buildTelemetryScript(sockPath, poolCmds);
          const poolOutput = await sshExecSafe(host, poolScript, 20_000);
          if (poolOutput) {
            try {
              const poolData = JSON.parse(poolOutput.trim());
              for (const poolName of poolList) {
                const pi = poolData[`/mempool/info,${poolName}`] as Record<string, unknown> | undefined;
                if (pi) {
                  mempoolInfo.push({
                    name: poolName,
                    size: Number(pi.size ?? 0),
                    avail_count: Number(pi.avail_count ?? 0),
                    in_use_count: Number(pi.size ?? 0) - Number(pi.avail_count ?? 0),
                  });
                }
              }
            } catch { /* ignore */ }
          }
        }
      }

      // Parse heap info
      const heapInfo: DpdkHeapInfo[] = [];
      if (endpoints.includes("heap")) {
        const heapList = telData["/eal/heap_list"] as number[] | undefined;
        if (heapList && heapList.length > 0) {
          const heapCmds = heapList.map((h) => `/eal/heap_info,${h}`);
          const heapScript = buildTelemetryScript(sockPath, heapCmds);
          const heapOutput = await sshExecSafe(host, heapScript, 20_000);
          if (heapOutput) {
            try {
              const heapData = JSON.parse(heapOutput.trim());
              for (const heapId of heapList) {
                const hi = heapData[`/eal/heap_info,${heapId}`] as Record<string, unknown> | undefined;
                if (hi) {
                  heapInfo.push({
                    heap_id: heapId,
                    total_bytes: Number(hi.Total_bytes ?? 0),
                    alloc_bytes: Number(hi.Alloc_bytes ?? 0),
                    free_bytes: Number(hi.Free_bytes ?? 0),
                    alloc_count: Number(hi.Alloc_count ?? 0),
                  });
                }
              }
            } catch { /* ignore */ }
          }
        }
      }

      instances.push({
        socket_path: sockPath,
        file_prefix: filePrefix,
        eal_params: ealParams ?? [],
        lcores: ealLcores ?? [],
        ethdev_stats: ethdevStats,
        ethdev_info: ethdevInfo,
        mempool_info: mempoolInfo,
        heap_info: heapInfo,
      });
    }

    return okResponse<MtlDpdkTelemetryData>(
      { instances, instance_count: instances.length },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "DPDK_TELEMETRY_ERROR",
      `Failed to query DPDK telemetry: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure MTL instances are running and DPDK telemetry sockets exist",
    );
  }
}
