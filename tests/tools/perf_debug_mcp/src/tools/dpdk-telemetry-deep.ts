/**
 * dpdk_telemetry_deep — Extended DPDK telemetry: xstats, ring info.
 *
 * Goes deeper than mtl_dpdk_telemetry by querying ethdev xstats (per-queue
 * RX/TX counters, detailed error counters, flow director stats) and ring
 * information.  The xstats provide hundreds of NIC-specific counters that
 * are invisible to kernel tools when using DPDK's userspace driver.
 *
 * Complementary to mtl_dpdk_telemetry (basic ethdev stats, mempool, heap).
 *
 * Source: DPDK telemetry v2 socket (/ethdev/xstats, /rawdev/xstats).
 * Requires: running DPDK application with telemetry enabled.
 */
import type { ToolResponse, DpdkTelemetryDeepData, DpdkXstatsPort, DpdkXstatEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function dpdkTelemetryDeep(params: {
  host?: string;
  dpdk_base_dir?: string;
  file_prefix?: string;
  filter?: string;
}): Promise<ToolResponse<DpdkTelemetryDeepData>> {
  const host = params.host ?? "localhost";
  const baseDir = params.dpdk_base_dir ?? "/var/run/dpdk";
  const filePrefix = params.file_prefix ?? "MT_DPDK";
  const filter = params.filter ?? "";  // filter xstat names containing this string
  const meta = await buildMeta("fallback");

  try {
    // Find telemetry sockets
    const sockList = await sshExecSafe(
      host,
      `find ${baseDir}/${filePrefix}/ -maxdepth 1 -name 'dpdk_telemetry.v2*' -type s 2>/dev/null | sort`,
    );
    if (!sockList || !sockList.trim()) {
      return okResponse<DpdkTelemetryDeepData>(
        { instance_socket: "", ports: [], ring_info: [], warnings: ["No DPDK telemetry sockets found"] },
        meta,
      );
    }

    const socketPath = sockList.trim().split("\n")[0]; // Use first instance
    const warnings: string[] = [];

    // Build Python script to query xstats and ring info
    const script = buildXstatsScript(socketPath, filter);
    const output = await sshExecSafe(host, script, 30_000);
    if (!output || !output.trim()) {
      return errorResponse(meta, "DPDK_XSTATS_NO_OUTPUT", "DPDK telemetry query returned no data");
    }

    let data: Record<string, unknown>;
    try {
      data = JSON.parse(output.trim());
    } catch {
      return errorResponse(meta, "DPDK_XSTATS_PARSE", "Failed to parse DPDK telemetry JSON response");
    }

    if (data.__error__) {
      return errorResponse(meta, "DPDK_XSTATS_ERROR", `DPDK telemetry error: ${data.__error__}`);
    }

    const ports: DpdkXstatsPort[] = [];
    const portList = (data["port_list"] as number[]) ?? [];

    for (const portId of portList) {
      const xstatsRaw = data[`xstats_${portId}`] as Record<string, number> | undefined;
      const infoRaw = data[`info_${portId}`] as Record<string, unknown> | undefined;
      if (!xstatsRaw) continue;

      const allXstats: DpdkXstatEntry[] = [];
      const errorXstats: DpdkXstatEntry[] = [];

      for (const [name, value] of Object.entries(xstatsRaw)) {
        if (typeof value !== "number") continue;
        const entry: DpdkXstatEntry = { name, value };
        allXstats.push(entry);
        if (name.includes("error") || name.includes("drop") || name.includes("miss")) {
          if (value > 0) errorXstats.push(entry);
        }
      }

      ports.push({
        port_id: portId,
        port_name: String(infoRaw?.name ?? `port${portId}`),
        xstats: allXstats,
        error_xstats: errorXstats,
      });

      if (errorXstats.length > 0) {
        warnings.push(`Port ${portId}: ${errorXstats.length} non-zero error/drop counters`);
      }
    }

    // Ring info
    const ringInfo: { name: string; count: number; size: number }[] = [];
    const ringList = (data["ring_list"] as string[]) ?? [];
    for (const ringName of ringList) {
      const ri = data[`ring_${ringName}`] as Record<string, unknown> | undefined;
      if (ri) {
        ringInfo.push({
          name: ringName,
          count: Number(ri.count ?? 0),
          size: Number(ri.size ?? 0),
        });
      }
    }

    return okResponse<DpdkTelemetryDeepData>({
      instance_socket: socketPath,
      ports,
      ring_info: ringInfo,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "DPDK_DEEP_ERROR", `DPDK deep telemetry failed: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function buildXstatsScript(socketPath: string, filter: string): string {
  return `python3 - <<'__DPDK_XSTATS_EOF__'
import socket, json, sys

SOCK_PATH = ${JSON.stringify(socketPath)}
FILTER = ${JSON.stringify(filter)}

def query(sock, cmd):
    sock.send(cmd.encode())
    data = b""
    while True:
        chunk = sock.recv(65536)
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
    s.settimeout(10)
    s.connect(SOCK_PATH)
    init = s.recv(4096)

    results = {}

    # Get port list
    r = query(s, "/ethdev/list")
    port_list = r.get("/ethdev/list", []) if r else []
    results["port_list"] = port_list

    # Get xstats and info for each port
    for port_id in port_list:
        r = query(s, f"/ethdev/xstats,{port_id}")
        if r:
            xstats = r.get(f"/ethdev/xstats,{port_id}", r)
            if FILTER:
                xstats = {k: v for k, v in xstats.items() if FILTER.lower() in k.lower()} if isinstance(xstats, dict) else xstats
            results[f"xstats_{port_id}"] = xstats

        r = query(s, f"/ethdev/info,{port_id}")
        if r:
            results[f"info_{port_id}"] = r.get(f"/ethdev/info,{port_id}", r)

    # Get ring list
    r = query(s, "/ring/list")
    ring_list = r.get("/ring/list", []) if r else []
    results["ring_list"] = ring_list

    for ring_name in ring_list[:20]:  # limit to 20 rings
        r = query(s, f"/ring/info,{ring_name}")
        if r:
            results[f"ring_{ring_name}"] = r.get(f"/ring/info,{ring_name}", r)

    s.close()
    print(json.dumps(results))
except Exception as e:
    print(json.dumps({"__error__": str(e)}))
__DPDK_XSTATS_EOF__`;
}
