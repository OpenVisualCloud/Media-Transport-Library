/**
 * pcm-bridge.ts — HTTP client for Intel PCM pcm-sensor-server.
 *
 * pcm-sensor-server runs on port 9738 by default and exposes:
 *   GET /               — absolute counters since daemon start (JSON if Accept: application/json)
 *   GET /persecond      — delta between last 2 internal 1-second samples
 *   GET /persecond/X    — delta between samples X seconds apart (1 ≤ X ≤ 30)
 *   GET /metrics         — Prometheus format (always)
 *
 * We always request JSON from /persecond/X because delta values give us
 * meaningful rates (IPC, bandwidth, hit ratios). Absolute values from /
 * would require us to keep our own state and compute deltas.
 *
 * Architecture:
 *   1. Singleton PcmBridge with lazy detection
 *   2. detect() probes the server with a HEAD / request
 *   3. getJson(seconds) fetches /persecond/{seconds} with Accept: application/json
 *   4. Each tool calls getJson() and extracts the fields it needs
 *   5. Graceful degradation: if pcm-sensor-server is absent, tools return
 *      an error with a helpful hint instead of crashing
 *
 * The JSON structure from pcm-sensor-server is deeply nested:
 *   { "Interval us": N,
 *     "Number of sockets": N,
 *     "Sockets": [
 *       { "Socket ID": N,
 *         "Number of cores": N,
 *         "Cores": [
 *           { "Core ID": N, "HW Core ID": N, "Module ID": N, "Tile ID": N,
 *             "Die ID": N, "Die Group ID": N, "Socket ID": N,
 *             "Threads": [
 *               { "Thread ID": N, "OS ID": N,
 *                 "Core Counters": { "Instructions Retired Any": N, ... },
 *                 "Energy Counters": { "Thermal Headroom": N, "CStateResidency[0]": N, ... },
 *                 "Core Memory Bandwidth Counters": { "Local Memory Bandwidth": N, "Remote Memory Bandwidth": N }
 *               }
 *             ]
 *           }
 *         ],
 *         "Uncore": { "Uncore Counters": { "DRAM Writes": N, "DRAM Reads": N, ... } },
 *         "Core Aggregate": { "Core Counters": { ... }, "Energy Counters": { ... } }
 *       }
 *     ],
 *     "QPI/UPI Links": { "QPI Counters Socket 0": { ... }, ... },
 *     "Core Aggregate": { "Core Counters": { ... }, "Energy Counters": { ... } },
 *     "Uncore Aggregate": { "Uncore Counters": { ... } }
 *   }
 */
import { request as httpRequest, IncomingMessage } from "http";
import { execFile } from "child_process";
import { promisify } from "util";
import { sshExecSafe } from "../utils/ssh-exec.js";
import type { PcmConnectionStatus } from "../types.js";

const execFileAsync = promisify(execFile);

// ─── Configuration ──────────────────────────────────────────────────────────

const PCM_DEFAULT_HOST = "127.0.0.1";
const PCM_DEFAULT_PORT = 9738;
const PCM_CONNECT_TIMEOUT_MS = 3000;
const PCM_READ_TIMEOUT_MS = 10000;

// ─── Raw JSON shapes from pcm-sensor-server ─────────────────────────────────

/**
 * We define these as loose Record<string, any> interfaces
 * because pcm-sensor-server's JSON keys contain spaces and
 * brackets (e.g. "Instructions Retired Any", "CStateResidency[0]").
 * Strict typing would require hundreds of string-literal keys.
 * Instead, each tool will do typed extraction with safe accessors.
 */
export interface PcmRawJson {
  "Interval us"?: number;
  "Number of sockets"?: number;
  "Sockets"?: PcmRawSocket[];
  "QPI/UPI Links"?: Record<string, any>;
  "Core Aggregate"?: PcmRawAggregateBlock;
  "Uncore Aggregate"?: PcmRawUncoreBlock;
  "Accelerators"?: Record<string, any>;
  [key: string]: any;
}

export interface PcmRawSocket {
  "Object"?: string;
  "Socket ID"?: number;
  "Number of cores"?: number;
  "Cores"?: PcmRawCore[];
  "Uncore"?: PcmRawUncoreBlock;
  "Core Aggregate"?: PcmRawAggregateBlock;
  [key: string]: any;
}

export interface PcmRawCore {
  "Object"?: string;
  "Core ID"?: number;
  "HW Core ID"?: number;
  "Module ID"?: number;
  "Tile ID"?: number;
  "Die ID"?: number;
  "Die Group ID"?: number;
  "Socket ID"?: number;
  "Number of threads"?: number;
  "Threads"?: PcmRawThread[];
  [key: string]: any;
}

export interface PcmRawThread {
  "Object"?: string;
  "Thread ID"?: number;
  "OS ID"?: number;
  "Core Counters"?: PcmRawCoreCounters;
  "Energy Counters"?: PcmRawEnergyCounters;
  "Core Memory Bandwidth Counters"?: PcmRawCoreMemBwCounters;
  [key: string]: any;
}

export interface PcmRawCoreCounters {
  "Instructions Retired Any"?: number;
  "Clock Unhalted Thread"?: number;
  "Clock Unhalted Ref"?: number;
  "L3 Cache Misses"?: number;
  "L3 Cache Hits"?: number;
  "L2 Cache Misses"?: number;
  "L2 Cache Hits"?: number;
  "L3 Cache Occupancy"?: number;
  "Invariant TSC"?: number;
  "SMI Count"?: number;
  "Core Frequency"?: number;
  "Frontend Bound"?: number;
  "Bad Speculation"?: number;
  "Backend Bound"?: number;
  "Retiring"?: number;
  "Fetch Latency Bound"?: number;
  "Fetch Bandwidth Bound"?: number;
  "Branch Misprediction Bound"?: number;
  "Machine Clears Bound"?: number;
  "Memory Bound"?: number;
  "Core Bound"?: number;
  "Heavy Operations Bound"?: number;
  "Light Operations Bound"?: number;
  [key: string]: any;
}

export interface PcmRawEnergyCounters {
  "Thermal Headroom"?: number;
  [key: string]: any;              // "CStateResidency[0]" through "CStateResidency[N]"
}

export interface PcmRawCoreMemBwCounters {
  "Local Memory Bandwidth"?: number;
  "Remote Memory Bandwidth"?: number;
  [key: string]: any;
}

export interface PcmRawUncoreCounters {
  "DRAM Writes"?: number;
  "DRAM Reads"?: number;
  "Persistent Memory Writes"?: number;
  "Persistent Memory Reads"?: number;
  "Embedded DRAM Writes"?: number;
  "Embedded DRAM Reads"?: number;
  "Memory Controller IA Requests"?: number;
  "Memory Controller GT Requests"?: number;
  "Memory Controller IO Requests"?: number;
  "Package Joules Consumed"?: number;
  "PP0 Joules Consumed"?: number;
  "PP1 Joules Consumed"?: number;
  "DRAM Joules Consumed"?: number;
  "Local Memory Request Ratio"?: number;
  "Remote Memory Request Ratio"?: number;
  "NM HitRate"?: number;
  "NM Hits"?: number;
  "NM Misses"?: number;
  "NM Miss Bw"?: number;
  [key: string]: any;              // "CStateResidency[0]" ... , "Uncore Frequency Die N"
}

export interface PcmRawUncoreBlock {
  "Object"?: string;
  "Uncore Counters"?: PcmRawUncoreCounters;
  [key: string]: any;
}

export interface PcmRawAggregateBlock {
  "Object"?: string;
  "Core Counters"?: PcmRawCoreCounters;
  "Energy Counters"?: PcmRawEnergyCounters;
  "Core Memory Bandwidth Counters"?: PcmRawCoreMemBwCounters;
  [key: string]: any;
}

// ─── Safe numeric accessor ──────────────────────────────────────────────────

/** Extract a numeric value from a loose object, returning 0 if absent/NaN.
 *  Also treats INT32_MIN-adjacent sentinel values as zero — pcm-sensor-server
 *  emits values near -2147483648 (0x80000000) for unsupported counters on
 *  some platforms (e.g. Sapphire Rapids uncore memory bandwidth). */
export function num(obj: Record<string, any> | undefined, key: string): number {
  if (!obj) return 0;
  const v = obj[key];
  if (v === undefined || v === null) return 0;
  const n = Number(v);
  if (!Number.isFinite(n)) return 0;
  // pcm-sensor-server sentinel: values near INT32_MIN (-2147483648)
  if (n < -2_000_000_000) return 0;
  return n;
}

/** Safely extract a sub-object. */
export function sub<T = Record<string, any>>(obj: any, key: string): T | undefined {
  if (!obj || typeof obj !== "object") return undefined;
  return obj[key] as T | undefined;
}

// ─── HTTP helpers ───────────────────────────────────────────────────────────

function httpGet(host: string, port: number, path: string, accept: string, timeoutMs: number): Promise<{ status: number; body: string }> {
  return new Promise((resolve, reject) => {
    const req = httpRequest(
      {
        hostname: host,
        port,
        path,
        method: "GET",
        headers: { Accept: accept },
        timeout: timeoutMs,
        // Disable connection pooling — pcm-sensor-server closes connections
        // after each response (no keep-alive), so reusing sockets causes
        // "socket hang up" errors on subsequent requests.
        agent: false,
      },
      (res: IncomingMessage) => {
        let body = "";
        res.setEncoding("utf-8");
        res.on("data", (chunk: string) => {
          body += chunk;
        });
        res.on("end", () => {
          resolve({ status: res.statusCode ?? 0, body });
        });
        res.on("error", reject);
      }
    );
    req.on("timeout", () => {
      req.destroy(new Error(`PCM request timed out after ${timeoutMs}ms`));
    });
    req.on("error", reject);
    req.end();
  });
}

// ─── PcmBridge class ────────────────────────────────────────────────────────

export class PcmBridge {
  private host: string;
  private port: number;
  private _available: boolean = false;
  private _detected: boolean = false;
  private _error: string | undefined;

  constructor(host?: string, port?: number) {
    this.host = host ?? process.env["PCM_HOST"] ?? PCM_DEFAULT_HOST;
    this.port = port ?? (process.env["PCM_PORT"] ? parseInt(process.env["PCM_PORT"], 10) : PCM_DEFAULT_PORT);
  }

  // ─── Detection ──────────────────────────────────────────────────

  /**
   * Probe pcm-sensor-server by requesting / with a short timeout.
   * We check for a 200 OK with any body that looks like valid JSON or HTML.
   * This is called once at startup (from index.ts) and cached.
   */
  async detect(): Promise<void> {
    if (this._detected) return;
    this._detected = true;

    try {
      // Request / with Accept: application/json.
      // If pcm-sensor-server is running, it returns JSON with "Sockets".
      // We use a short connect timeout to avoid blocking if server is absent.
      const { status, body } = await httpGet(
        this.host,
        this.port,
        "/",
        "application/json",
        PCM_CONNECT_TIMEOUT_MS
      );

      if (status === 200 && body.length > 0) {
        // Sanity check: try parsing the JSON to ensure it's PCM data
        try {
          const parsed = JSON.parse(body);
          if (typeof parsed === "object" && parsed !== null && ("Sockets" in parsed || "Number of sockets" in parsed)) {
            this._available = true;
            this._error = undefined;
          } else {
            this._available = false;
            this._error = `Server at ${this.host}:${this.port} responded but does not appear to be pcm-sensor-server (missing "Sockets" key)`;
          }
        } catch {
          // Might be HTML (the default page)
          // Check if body contains PCM indicator strings
          if (body.includes("PCM Sensor Server") || body.includes("pcm")) {
            this._available = true;
            this._error = undefined;
          } else {
            this._available = false;
            this._error = `Server at ${this.host}:${this.port} responded but does not appear to be pcm-sensor-server`;
          }
        }
      } else {
        this._available = false;
        this._error = `pcm-sensor-server returned status ${status}`;
      }
    } catch (err: any) {
      this._available = false;
      const code = err?.code ?? "";
      if (code === "ECONNREFUSED") {
        this._error = `pcm-sensor-server not reachable at ${this.host}:${this.port} (connection refused). Start it with: sudo pcm-sensor-server`;
      } else if (code === "ENOTFOUND") {
        this._error = `Host ${this.host} not found`;
      } else {
        this._error = `pcm-sensor-server probe failed: ${err?.message ?? err}`;
      }
    }
  }

  // ─── Auto-start ─────────────────────────────────────────────────

  private _autoStartAttempted: boolean = false;
  private _autoStartPromise: Promise<void> | null = null;

  /**
   * Ensure pcm-sensor-server is running. If not available after initial
   * detect(), attempt to spawn it once and wait for it to become ready.
   * Safe to call concurrently — deduplicates via a shared promise.
   * Works for both local (execFile) and remote (SSH) hosts.
   */
  async ensureRunning(): Promise<void> {
    if (this._available) return;

    // Only attempt auto-start once
    if (this._autoStartAttempted) return;

    if (this._autoStartPromise) {
      await this._autoStartPromise;
      return;
    }

    this._autoStartPromise = this._doAutoStart();
    try {
      await this._autoStartPromise;
    } finally {
      this._autoStartPromise = null;
    }
  }

  private _isLocalHost(): boolean {
    return this.host === PCM_DEFAULT_HOST || this.host === "localhost" || this.host === "127.0.0.1";
  }

  private async _doAutoStart(): Promise<void> {
    this._autoStartAttempted = true;

    if (this._isLocalHost()) {
      await this._autoStartLocal();
    } else {
      await this._autoStartRemote();
    }
  }

  private async _autoStartLocal(): Promise<void> {
    // Check if the binary exists
    let pcmPath: string | null = null;
    try {
      const { stdout } = await execFileAsync("which", ["pcm-sensor-server"], { timeout: 3000 });
      pcmPath = stdout.trim();
    } catch {
      this._error = "pcm-sensor-server binary not found in PATH";
      return;
    }

    if (!pcmPath) {
      this._error = "pcm-sensor-server binary not found in PATH";
      return;
    }

    // Spawn pcm-sensor-server in daemon mode
    try {
      await execFileAsync(pcmPath, ["-d", "-p", String(this.port)], { timeout: 10_000 });
    } catch {
      // -d mode may exit immediately after forking; we'll check by probing the port.
    }

    await this._pollUntilReady();
  }

  private async _autoStartRemote(): Promise<void> {
    // Check if binary exists on remote host
    const whichResult = await sshExecSafe(this.host, "which pcm-sensor-server", 5000);
    if (!whichResult?.trim()) {
      this._error = `pcm-sensor-server binary not found on ${this.host}`;
      return;
    }

    // Check if already running on the remote host
    const pgrepResult = await sshExecSafe(this.host, `pgrep -f 'pcm-sensor-server.*-p ${this.port}' || pgrep -f pcm-sensor-server`, 5000);
    if (pgrepResult?.trim()) {
      // Already running — just re-detect
      this._detected = false;
      await this.detect();
      if (this._available) return;
    }

    // Start pcm-sensor-server on remote host in daemon mode
    await sshExecSafe(this.host, `pcm-sensor-server -d -p ${this.port}`, 15_000);

    await this._pollUntilReady();
  }

  private async _pollUntilReady(): Promise<void> {
    const deadline = Date.now() + 10_000;
    const pollInterval = 500;

    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, pollInterval));

      // Reset detection state and re-probe
      this._detected = false;
      await this.detect();

      if (this._available) {
        return;
      }
    }

    this._error = `pcm-sensor-server was started but did not become ready within 10s on ${this.host}:${this.port}`;
  }

  // ─── Status ─────────────────────────────────────────────────────

  get isAvailable(): boolean {
    return this._available;
  }

  get endpoint(): string {
    return `http://${this.host}:${this.port}`;
  }

  get connectionError(): string | undefined {
    return this._error;
  }

  getStatus(): PcmConnectionStatus {
    return {
      available: this._available,
      endpoint: this.endpoint,
      error: this._error,
    };
  }

  // ─── Data fetching ──────────────────────────────────────────────

  /**
   * Fetch delta-based JSON from pcm-sensor-server.
   *
   * @param seconds Number of seconds between the two samples (1–30).
   *   - 1 = delta between the last two 1-second internal samples (fastest)
   *   - N = delta between samples N seconds apart (more stable/averaged)
   *
   * The server maintains an internal ring buffer of 30 1-second samples.
   * /persecond/1 gives the most recent 1-second delta.
   * /persecond/5 gives a 5-second averaged delta, etc.
   */
  async getJson(seconds: number = 1): Promise<PcmRawJson> {
    if (!this._available) {
      throw new Error(
        this._error ?? `pcm-sensor-server is not available at ${this.endpoint}`
      );
    }

    // Clamp seconds to valid range
    const s = Math.max(1, Math.min(30, Math.round(seconds)));
    const path = `/persecond/${s}`;

    const { status, body } = await httpGet(
      this.host,
      this.port,
      path,
      "application/json",
      PCM_READ_TIMEOUT_MS
    );

    if (status !== 200) {
      throw new Error(`pcm-sensor-server returned status ${status} for ${path}`);
    }

    const json = JSON.parse(body);
    // Validate minimal structure
    if (typeof json !== "object" || json === null) {
      throw new Error("pcm-sensor-server returned non-object JSON");
    }

    return json as PcmRawJson;
  }

  // ─── High-level structured extractors ─────────────────────────

  /**
   * Walk the Socket→Core→Thread tree and call a visitor for each thread.
   * Returns a flat array of whatever the visitor returns.
   */
  walkThreads<T>(
    json: PcmRawJson,
    visitor: (thread: PcmRawThread, core: PcmRawCore, socket: PcmRawSocket) => T | null
  ): T[] {
    const results: T[] = [];
    const sockets = json["Sockets"];
    if (!Array.isArray(sockets)) return results;

    for (const sock of sockets) {
      const cores = sock["Cores"];
      if (!Array.isArray(cores)) continue;

      for (const core of cores) {
        const threads = core["Threads"];
        if (!Array.isArray(threads)) continue;

        for (const thread of threads) {
          const item = visitor(thread, core, sock);
          if (item !== null) results.push(item);
        }
      }
    }
    return results;
  }

  /**
   * Walk sockets and call a visitor for each socket.
   */
  walkSockets<T>(json: PcmRawJson, visitor: (socket: PcmRawSocket) => T | null): T[] {
    const results: T[] = [];
    const sockets = json["Sockets"];
    if (!Array.isArray(sockets)) return results;

    for (const sock of sockets) {
      const item = visitor(sock);
      if (item !== null) results.push(item);
    }
    return results;
  }

  /**
   * Extract QPI/UPI per-socket link data.
   * The raw JSON key is "QPI/UPI Links" containing objects like
   * "QPI Counters Socket 0": { "Incoming Data Traffic On Link 0": N, ... }
   */
  extractQpiLinks(json: PcmRawJson): { socket: number; links: Record<string, any> }[] {
    const qpiBlock = json["QPI/UPI Links"];
    if (!qpiBlock || typeof qpiBlock !== "object") return [];

    const result: { socket: number; links: Record<string, any> }[] = [];

    for (const [key, value] of Object.entries(qpiBlock)) {
      // Keys like "QPI Counters Socket 0"
      const match = key.match(/Socket\s+(\d+)/i);
      if (match && typeof value === "object" && value !== null) {
        result.push({ socket: parseInt(match[1], 10), links: value as Record<string, any> });
      }
    }
    return result;
  }
}

// ─── Per-host bridge cache ──────────────────────────────────────────────────

const _bridges = new Map<string, PcmBridge>();

/**
 * Get a PcmBridge for the given host. Reuses cached instances.
 * For local use (no host), returns the default localhost bridge.
 */
export function getPcmBridgeForHost(host?: string): PcmBridge {
  const resolvedHost = host && host !== "localhost" && host !== "127.0.0.1" && host !== ""
    ? host
    : (process.env["PCM_HOST"] ?? PCM_DEFAULT_HOST);
  const key = `${resolvedHost}:${process.env["PCM_PORT"] ?? PCM_DEFAULT_PORT}`;

  let bridge = _bridges.get(key);
  if (!bridge) {
    bridge = new PcmBridge(resolvedHost);
    _bridges.set(key, bridge);
  }
  return bridge;
}

/** Backward-compatible alias — returns the localhost bridge. */
export function getPcmBridge(): PcmBridge {
  return getPcmBridgeForHost();
}
