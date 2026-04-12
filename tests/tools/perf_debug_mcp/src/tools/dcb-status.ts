/**
 * dcb_status — Read DCB (Data Center Bridging) configuration.
 *
 * Shows PFC (Priority Flow Control) priorities, ETS (Enhanced Transmission
 * Selection) bandwidth allocation, and link properties for network interfaces.
 *
 * Data sources:
 *   - `dcb pfc show dev <iface>` — PFC configuration
 *   - `dcb ets show dev <iface>` — ETS configuration
 *   - `ip -j link show <iface>` — link state, MTU
 *   - `ethtool <iface>` — speed/duplex
 *   - `rdma link show` — RDMA device correlation
 *
 * Universal — works with any NIC that supports DCB.
 */
import type { ToolResponse } from "../types.js";
import type {
  DcbStatusData,
  DcbInterfaceStatus,
  DcbPfcStatus,
  DcbEtsStatus,
  DcbEtsTcConfig,
  DcbVfInfo,
  DcbStatusWarning,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Build a map of netdev → RDMA device name from `rdma link show`.
 */
async function buildRdmaMap(host: string): Promise<Map<string, { device: string; state: string }>> {
  const map = new Map<string, { device: string; state: string }>();
  const output = await sshExecSafe(host, "rdma link show 2>/dev/null");
  if (!output) return map;

  for (const line of output.split("\n")) {
    const m = line.match(
      /link\s+(\S+)\/\d+\s+state\s+(\S+)\s+physical_state\s+\S+(?:\s+netdev\s+(\S+))?/,
    );
    if (m && m[3]) {
      map.set(m[3], { device: m[1], state: m[2] });
    }
  }
  return map;
}

/**
 * Discover interfaces: from RDMA map, or fall back to non-loopback non-virtual interfaces.
 */
async function discoverInterfaces(host: string, rdmaMap: Map<string, { device: string; state: string }>): Promise<string[]> {
  if (rdmaMap.size > 0) {
    return [...rdmaMap.keys()].sort();
  }

  // Fall back: all non-loopback physical interfaces
  const output = await sshExecSafe(host, "ip -j link show 2>/dev/null");
  if (!output) return [];

  try {
    const links = JSON.parse(output) as Array<{ ifname: string; link_type: string; flags: string[] }>;
    return links
      .filter((l) => l.ifname !== "lo" && !l.ifname.startsWith("veth") && !l.ifname.startsWith("docker") && !l.ifname.startsWith("br-"))
      .map((l) => l.ifname)
      .sort();
  } catch {
    return [];
  }
}

/**
 * Parse `dcb pfc show dev <iface>` output.
 */
function parsePfcShow(output: string): DcbPfcStatus | null {
  if (!output || !output.trim()) return null;

  let pfcCap = 0;
  let macsecBypass = false;
  let delay = 0;
  const prioPfc: Record<number, boolean> = {};

  for (const line of output.split("\n")) {
    // pfc-cap 8 macsec-bypass off delay 1949
    const capMatch = line.match(/pfc-cap\s+(\d+)/);
    if (capMatch) pfcCap = parseInt(capMatch[1], 10);

    const macsecMatch = line.match(/macsec-bypass\s+(\S+)/);
    if (macsecMatch) macsecBypass = macsecMatch[1] === "on";

    const delayMatch = line.match(/delay\s+(\d+)/);
    if (delayMatch) delay = parseInt(delayMatch[1], 10);

    // prio-pfc 0:off 1:off 2:off 3:on 4:off 5:off 6:off 7:off
    if (line.includes("prio-pfc")) {
      const pairs = line.match(/(\d+):(on|off)/g);
      if (pairs) {
        for (const pair of pairs) {
          const [prio, val] = pair.split(":");
          prioPfc[parseInt(prio, 10)] = val === "on";
        }
      }
    }
  }

  const enabledPriorities = Object.entries(prioPfc)
    .filter(([, v]) => v)
    .map(([k]) => parseInt(k, 10))
    .sort();

  return { pfc_cap: pfcCap, macsec_bypass: macsecBypass, delay, prio_pfc: prioPfc, enabled_priorities: enabledPriorities };
}

/**
 * Parse `dcb ets show dev <iface>` output.
 */
function parseEtsShow(output: string): DcbEtsStatus | null {
  if (!output || !output.trim()) return null;

  let willing = false;
  let etsCap = 0;
  let cbs = false;
  const tcBw: Record<number, number> = {};
  const tcTsa: Record<number, string> = {};
  const prioTc: Record<number, number> = {};

  for (const line of output.split("\n")) {
    if (line.match(/^\s*willing\s+(on|off)/)) willing = line.includes("willing on");
    const capMatch = line.match(/ets-cap\s+(\d+)/);
    if (capMatch) etsCap = parseInt(capMatch[1], 10);
    if (line.includes("cbs on")) cbs = true;

    // tc-bw 0:90 1:10 2:0 ...
    if (line.trim().startsWith("tc-bw")) {
      const pairs = line.match(/(\d+):(\d+)/g);
      if (pairs) {
        for (const pair of pairs) {
          const [tc, bw] = pair.split(":");
          tcBw[parseInt(tc, 10)] = parseInt(bw, 10);
        }
      }
    }

    // tc-tsa 0:ets 1:ets 2:strict ...
    if (line.trim().startsWith("tc-tsa")) {
      const pairs = line.match(/(\d+):(\S+)/g);
      if (pairs) {
        for (const pair of pairs) {
          const [tc, tsa] = pair.split(":");
          tcTsa[parseInt(tc, 10)] = tsa;
        }
      }
    }

    // prio-tc 0:0 1:0 2:0 3:1 ...
    if (line.trim().startsWith("prio-tc")) {
      const pairs = line.match(/(\d+):(\d+)/g);
      if (pairs) {
        for (const pair of pairs) {
          const [prio, tc] = pair.split(":");
          prioTc[parseInt(prio, 10)] = parseInt(tc, 10);
        }
      }
    }
  }

  const tcConfig: DcbEtsTcConfig[] = [];
  for (let tc = 0; tc < 8; tc++) {
    tcConfig.push({
      tc,
      bandwidth_pct: tcBw[tc] ?? 0,
      tsa: tcTsa[tc] ?? "strict",
    });
  }

  return { willing, ets_cap: etsCap, cbs, tc_config: tcConfig, prio_to_tc: prioTc };
}

/**
 * Get link info (MTU, operstate) via `ip -j link show`.
 */
async function getLinkInfo(host: string, iface: string): Promise<{ mtu: number; operstate: string }> {
  const output = await sshExecSafe(host, `ip -j link show ${iface} 2>/dev/null`);
  if (!output) return { mtu: 0, operstate: "UNKNOWN" };

  try {
    const links = JSON.parse(output) as Array<{ mtu: number; operstate: string }>;
    if (links.length > 0) {
      return { mtu: links[0].mtu ?? 0, operstate: (links[0].operstate ?? "UNKNOWN").toUpperCase() };
    }
  } catch { /* fallthrough */ }
  return { mtu: 0, operstate: "UNKNOWN" };
}

/**
 * Get interface speed from ethtool.
 */
async function getSpeed(host: string, iface: string): Promise<number | null> {
  const output = await sshExecSafe(host, `ethtool ${iface} 2>/dev/null`);
  if (!output) return null;

  const m = output.match(/Speed:\s+(\d+)/);
  return m ? parseInt(m[1], 10) : null;
}

/**
 * Parse VF info from `ip -j link show <iface>`.
 */
async function getVfInfo(host: string, iface: string): Promise<DcbVfInfo[]> {
  const output = await sshExecSafe(host, `ip -j link show ${iface} 2>/dev/null`);
  if (!output) return [];

  try {
    const links = JSON.parse(output) as Array<{ vfinfo_list?: Array<Record<string, unknown>> }>;
    if (!links[0]?.vfinfo_list) return [];

    return links[0].vfinfo_list.map((vf) => ({
      vf_id: Number(vf.vf ?? 0),
      mac: String(vf.mac ?? ""),
      link_state: String(vf.link_state ?? "auto"),
      trust: vf.trust === "on" || vf.trust === true,
      spoof_check: vf.spoofchk === "on" || vf.spoofchk === true,
      vlan: vf.vlan != null ? Number(vf.vlan) : undefined,
    }));
  } catch {
    return [];
  }
}

/**
 * Check if an RDMA device has active (non-zero-traffic) QPs.
 * Returns true if there are QPs in RTS state, false otherwise.
 */
async function hasActiveRdmaTraffic(host: string, rdmaDevice: string): Promise<boolean> {
  // Check for QPs in Ready-To-Send state
  const qpOutput = await sshExecSafe(
    host,
    `rdma -j res show qp dev ${rdmaDevice} 2>/dev/null`,
    5_000,
  );
  if (!qpOutput || !qpOutput.trim()) return false;

  try {
    const qps = JSON.parse(qpOutput) as Array<{ state: string; type: string }>;
    // Filter out management QPs, count RTS QPs
    const rtsQps = qps.filter((q) => q.state === "RTS" && q.type !== "GSI" && q.type !== "SMI");
    return rtsQps.length > 0;
  } catch {
    return false;
  }
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function dcbStatus(params: {
  host?: string;
  interfaces?: string[];
  include_vf_info?: boolean;
}): Promise<ToolResponse<DcbStatusData>> {
  const host = params.host ?? "localhost";
  const includeVf = params.include_vf_info ?? false;

  const meta = await buildMeta("fallback");

  try {
    // Build RDMA device map for correlation
    const rdmaMap = await buildRdmaMap(host);

    // Determine interfaces to query
    const ifaces = params.interfaces && params.interfaces.length > 0
      ? params.interfaces
      : await discoverInterfaces(host, rdmaMap);

    if (ifaces.length === 0) {
      return okResponse<DcbStatusData>(
        { interfaces: [], warnings: [{ interface: "system", severity: "info", message: "No interfaces discovered" }] },
        meta,
      );
    }

    // Check if dcb tool is available
    const dcbAvailable = !!(await sshExecSafe(host, "command -v dcb 2>/dev/null"));

    const results: DcbInterfaceStatus[] = [];
    const warnings: DcbStatusWarning[] = [];

    for (const iface of ifaces) {
      const linkInfo = await getLinkInfo(host, iface);
      const speed = await getSpeed(host, iface);
      const rdma = rdmaMap.get(iface);

      let pfc: DcbPfcStatus | null = null;
      let ets: DcbEtsStatus | null = null;

      if (dcbAvailable) {
        const pfcOutput = await sshExecSafe(host, `dcb pfc show dev ${iface} 2>/dev/null`);
        if (pfcOutput) pfc = parsePfcShow(pfcOutput);

        const etsOutput = await sshExecSafe(host, `dcb ets show dev ${iface} 2>/dev/null`);
        if (etsOutput) ets = parseEtsShow(etsOutput);
      } else {
        warnings.push({
          interface: iface,
          severity: "info",
          message: `${iface}: dcb tool not available — PFC/ETS status unavailable. Install iproute2.`,
        });
      }

      const entry: DcbInterfaceStatus = {
        interface: iface,
        mtu: linkInfo.mtu,
        operstate: linkInfo.operstate,
        speed_mbps: speed,
        rdma_device: rdma?.device ?? null,
        pfc,
        ets,
      };

      if (includeVf) {
        entry.vfs = await getVfInfo(host, iface);
      }

      results.push(entry);

      // Generate warnings
      if (linkInfo.operstate !== "UP" && linkInfo.operstate !== "UNKNOWN") {
        warnings.push({
          interface: iface,
          severity: "warning",
          message: `${iface}: link is ${linkInfo.operstate}`,
        });
      }

      if (pfc) {
        if (pfc.enabled_priorities.length > 1) {
          warnings.push({
            interface: iface,
            severity: "info",
            message: `${iface}: PFC enabled on priorities [${pfc.enabled_priorities.join(", ")}] (typically only 1 priority for RoCEv2)`,
          });
        }
        if (pfc.enabled_priorities.length === 0 && rdma && rdma.state === "ACTIVE") {
          // Suppress false positive: only warn if the RDMA device has active QPs with traffic
          const hasTraffic = await hasActiveRdmaTraffic(host, rdma.device);
          if (hasTraffic) {
            warnings.push({
              interface: iface,
              severity: "warning",
              message: `${iface}: PFC disabled but RDMA device ${rdma.device} has active QPs — RoCEv2 may drop under congestion`,
            });
          } else {
            warnings.push({
              interface: iface,
              severity: "info",
              message: `${iface}: PFC disabled, RDMA device ${rdma.device} is ACTIVE but has no active QPs (no kernel RDMA traffic)`,
            });
          }
        }
      }

      if (rdma && linkInfo.mtu < 4096) {
        warnings.push({
          interface: iface,
          severity: "info",
          message: `${iface}: MTU ${linkInfo.mtu} — jumbo frames (9000) recommended for RDMA`,
        });
      }
    }

    return okResponse<DcbStatusData>({ interfaces: results, warnings }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "DCB_STATUS_ERROR",
      `Failed to read DCB status: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
