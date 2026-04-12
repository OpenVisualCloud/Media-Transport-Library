/**
 * socket_diag — TCP/UDP socket diagnostics via `ss`.
 *
 * Reads all TCP (and optionally UDP) sockets with extended info: state,
 * queue sizes, associated PID/process, and TCP internal metrics (RTT, CWND,
 * retransmits, etc.).  Useful for identifying:
 *
 *   - Large send/recv queues (backpressure building up)
 *   - TCP connections in SYN-SENT or CLOSE-WAIT (stuck connections)
 *   - High retransmit counts on specific flows
 *   - Process-to-socket mapping for unknown traffic
 *
 * Source: `ss` from iproute2 (reads kernel socket tables via netlink).
 * Universal — works on any Linux host.
 */
import type { ToolResponse, SocketDiagData, SocketEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function socketDiag(params: {
  host?: string;
  protocol?: "tcp" | "udp" | "all";
  state_filter?: string;
  process_filter?: string;
}): Promise<ToolResponse<SocketDiagData>> {
  const host = params.host ?? "localhost";
  const protocol = params.protocol ?? "tcp";
  const stateFilter = params.state_filter;
  const processFilter = params.process_filter;

  const meta = await buildMeta("fallback");

  // Check ss availability
  const check = await sshExecSafe(host, "command -v ss 2>/dev/null");
  if (!check || !check.trim()) {
    return errorResponse(
      meta,
      "SS_MISSING",
      "ss not found on target host",
      "Install iproute2: apt-get install iproute2",
    );
  }

  try {
    // Build ss command
    // -t: TCP, -u: UDP, -a: all states, -n: numeric, -e: extended, -p: process, -i: internal TCP info
    let protoFlag = "-t";
    if (protocol === "udp") protoFlag = "-u";
    else if (protocol === "all") protoFlag = "-tu";

    let stateFlag = "";
    if (stateFilter) {
      stateFlag = ` state ${stateFilter}`;
    }

    const cmd = `ss ${protoFlag}anep -i${stateFlag} 2>/dev/null`;
    const output = await sshExecSafe(host, cmd, 15_000);
    if (!output || !output.trim()) {
      return okResponse<SocketDiagData>({
        sockets: [],
        total_count: 0,
        state_summary: {},
        warnings: [],
      }, meta);
    }

    let sockets = parseSsOutput(output);

    // Apply process filter if specified
    if (processFilter) {
      const filterLower = processFilter.toLowerCase();
      sockets = sockets.filter((s) =>
        (s.comm && s.comm.toLowerCase().includes(filterLower)) ||
        (s.pid !== undefined && s.pid.toString() === processFilter),
      );
    }

    // Compute state summary
    const stateSummary: Record<string, number> = {};
    for (const s of sockets) {
      stateSummary[s.state] = (stateSummary[s.state] ?? 0) + 1;
    }

    const warnings: string[] = [];
    // Check for concerning patterns
    const closeWait = stateSummary["CLOSE-WAIT"] ?? 0;
    if (closeWait > 5) {
      warnings.push(`${closeWait} CLOSE-WAIT sockets — peer closed but application hasn't (possible fd leak)`);
    }

    const synSent = stateSummary["SYN-SENT"] ?? 0;
    if (synSent > 10) {
      warnings.push(`${synSent} SYN-SENT sockets — many outbound connections pending (firewall or unresponsive peer?)`);
    }

    // Large queues
    for (const s of sockets) {
      if (s.recv_q > 100000) {
        warnings.push(
          `${s.local_addr}:${s.local_port} recv_q=${s.recv_q} — large receive backlog (application too slow?)`,
        );
      }
      if (s.send_q > 100000) {
        warnings.push(
          `${s.local_addr}:${s.local_port} send_q=${s.send_q} — large send queue (peer not reading?)`,
        );
      }
    }

    return okResponse<SocketDiagData>({
      sockets,
      total_count: sockets.length,
      state_summary: stateSummary,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "SOCKET_DIAG_ERROR",
      `Failed to read socket info: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}

/**
 * Parse `ss -tanep -i` output.
 *
 * Format:
 * ```
 * State  Recv-Q  Send-Q  Local Address:Port  Peer Address:Port  Process
 * ESTAB  0       0       10.0.0.1:22         10.0.0.2:54321     users:(("sshd",pid=1234,fd=3))
 *         cubic wscale:7,7 rto:200 rtt:0.5/0.25 ato:40 mss:1448 pmtu:1500 rcvmss:1448 advmss:1448 cwnd:10 ssthresh:7 bytes_sent:1234
 * ```
 */
function parseSsOutput(output: string): SocketEntry[] {
  const lines = output.split("\n");
  const sockets: SocketEntry[] = [];

  // Skip header
  let i = 0;
  while (i < lines.length && !lines[i].trim().match(/^(State|ESTAB|LISTEN|SYN|TIME|CLOSE|FIN|LAST|UNCONN)/)) {
    i++;
  }
  if (i < lines.length && lines[i].trim().startsWith("State")) i++;

  let currentSocket: SocketEntry | null = null;

  for (; i < lines.length; i++) {
    const line = lines[i];
    const trimmed = line.trim();
    if (!trimmed) continue;

    // Check if this is a socket line (starts with a state keyword) or a TCP info continuation
    const stateMatch = trimmed.match(
      /^(ESTAB|LISTEN|SYN-SENT|SYN-RECV|FIN-WAIT-1|FIN-WAIT-2|TIME-WAIT|CLOSE-WAIT|CLOSE|LAST-ACK|CLOSING|UNCONN)\s+/,
    );

    if (stateMatch) {
      // Flush previous
      if (currentSocket) sockets.push(currentSocket);

      const parts = trimmed.split(/\s+/);
      if (parts.length < 5) continue;

      const state = parts[0];
      const recvQ = parseInt(parts[1], 10);
      const sendQ = parseInt(parts[2], 10);
      const { addr: localAddr, port: localPort } = parseAddrPort(parts[3]);
      const { addr: remoteAddr, port: remotePort } = parseAddrPort(parts[4]);

      // Extract process info if present
      let pid: number | undefined;
      let comm: string | undefined;
      const processMatch = trimmed.match(/users:\(\("([^"]+)",pid=(\d+)/);
      if (processMatch) {
        comm = processMatch[1];
        pid = parseInt(processMatch[2], 10);
      }

      currentSocket = {
        protocol: state === "UNCONN" ? "udp" : "tcp",
        state,
        local_addr: localAddr,
        local_port: localPort,
        remote_addr: remoteAddr,
        remote_port: remotePort,
        recv_q: isNaN(recvQ) ? 0 : recvQ,
        send_q: isNaN(sendQ) ? 0 : sendQ,
        pid,
        comm,
      };
    } else if (currentSocket && trimmed.length > 0) {
      // TCP info continuation line
      if (!currentSocket.tcp_info) currentSocket.tcp_info = {};
      // Parse key:value or key space value pairs
      const kvParts = trimmed.split(/\s+/);
      for (const part of kvParts) {
        const colonIdx = part.indexOf(":");
        if (colonIdx > 0) {
          const key = part.slice(0, colonIdx);
          const val = part.slice(colonIdx + 1);
          currentSocket.tcp_info[key] = val;
        }
      }
    }
  }

  if (currentSocket) sockets.push(currentSocket);
  return sockets;
}

/**
 * Parse address:port from ss output.
 * Handles IPv4 (10.0.0.1:22), IPv6 ([::1]:22), and wildcard (*:22).
 */
function parseAddrPort(s: string): { addr: string; port: number } {
  // IPv6: [::1]:8080
  const ipv6Match = s.match(/^\[([^\]]+)\]:(\d+|\*)$/);
  if (ipv6Match) {
    return { addr: ipv6Match[1], port: ipv6Match[2] === "*" ? 0 : parseInt(ipv6Match[2], 10) };
  }

  // IPv4 or wildcard: last colon separates addr and port
  const lastColon = s.lastIndexOf(":");
  if (lastColon > 0) {
    const addr = s.slice(0, lastColon);
    const portStr = s.slice(lastColon + 1);
    return { addr, port: portStr === "*" ? 0 : parseInt(portStr, 10) };
  }

  return { addr: s, port: 0 };
}
