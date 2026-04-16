#!/usr/bin/env node
/**
 * cpu-debug-mcp — Entry point
 *
 * Usage:
 *   cpu-debug-mcp                    # stdio transport (for VS Code MCP)
 *   cpu-debug-mcp --tcp --port 3001  # TCP transport (for debugging)
 */
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { createServer } from "./server.js";
import { getEbpfBridge } from "./collectors/ebpf-bridge.js";
import { getPcmBridge } from "./collectors/pcm-bridge.js";
import { getEmonBridge } from "./collectors/emon-bridge.js";
import { getBpftraceBridge } from "./collectors/bpftrace-bridge.js";

async function main() {
  const args = process.argv.slice(2);
  const useTcp = args.includes("--tcp");
  const portIdx = args.indexOf("--port");
  const port = portIdx !== -1 && args[portIdx + 1] ? parseInt(args[portIdx + 1], 10) : 3001;

  // Detect eBPF capabilities
  const bridge = getEbpfBridge();
  await bridge.detect();

  // Detect Intel PCM (pcm-sensor-server)
  const pcmBridge = getPcmBridge();
  await pcmBridge.detect();

  // Detect Intel EMON (SEP/EMON event monitor)
  const emonBridge = getEmonBridge();
  await emonBridge.detect();

  // Detect bpftrace / USDT probe support
  const bpftraceBridge = getBpftraceBridge();
  await bpftraceBridge.detect();

  // Create MCP server
  const server = createServer();

  if (useTcp) {
    // TCP transport for debugging
    // StreamableHTTP: create transport once, connect once, handle all requests
    const { StreamableHTTPServerTransport } = await import(
      "@modelcontextprotocol/sdk/server/streamableHttp.js"
    );
    const http = await import("http");

    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: undefined,
    });
    await server.connect(transport);

    const httpServer = http.createServer(async (req, res) => {
      // CORS headers for local debugging
      res.setHeader("Access-Control-Allow-Origin", "*");
      res.setHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
      res.setHeader("Access-Control-Allow-Headers", "Content-Type, mcp-session-id");

      if (req.method === "OPTIONS") {
        res.writeHead(204);
        res.end();
        return;
      }

      await transport.handleRequest(req, res);
    });

    httpServer.listen(port, "127.0.0.1", () => {
      console.error(`cpu-debug-mcp TCP server listening on http://127.0.0.1:${port}/mcp`);
      console.error(`eBPF mode: ${bridge.isEnabled ? "enabled" : "disabled (fallback only)"}`);
      console.error(`PCM server: ${pcmBridge.isAvailable ? "connected" : "not available"}`);
      console.error(`EMON: ${emonBridge.isAvailable ? `available (V${emonBridge.version}, ${emonBridge.eventCount} events)` : "not available"}`);
      console.error(`USDT/bpftrace: ${bpftraceBridge.isAvailable ? `v${bpftraceBridge.version}, ${bpftraceBridge.probeCount} probes` : "not available"}`);
    });
  } else {
    // stdio transport (default, for VS Code MCP integration)
    const transport = new StdioServerTransport();
    await server.connect(transport);

    // Log to stderr (stdout is used for MCP protocol)
    console.error("cpu-debug-mcp started (stdio transport)");
    console.error(`eBPF mode: ${bridge.isEnabled ? "enabled" : "disabled (fallback only)"}`);
    console.error(`PCM server: ${pcmBridge.isAvailable ? "connected" : "not available"}`);
    console.error(`EMON: ${emonBridge.isAvailable ? `available (V${emonBridge.version}, ${emonBridge.eventCount} events)` : "not available"}`);
    console.error(`USDT/bpftrace: ${bpftraceBridge.isAvailable ? `v${bpftraceBridge.version}, ${bpftraceBridge.probeCount} probes` : "not available"}`);
  }

  // Handle cleanup
  process.on("SIGINT", () => {
    bridge.shutdown();
    process.exit(0);
  });
  process.on("SIGTERM", () => {
    bridge.shutdown();
    process.exit(0);
  });
}

main().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
