/**
 * bpftrace-bridge.ts — Singleton bridge for running bpftrace USDT commands.
 *
 * Provides:
 *   1. detect() — checks bpftrace availability and USDT probe support
 *   2. runScript() — executes a bpftrace one-liner/script against a PID with timeout
 *   3. listProbes() — lists all USDT probes in a shared object
 *
 * Architecture:
 *   - Uses `bpftrace -e '...' -p PID` for per-process USDT tracing
 *   - BPFTRACE_MAX_STRLEN=200 by default (bpftrace v0.20.x max; truncates long strings)
 *   - Timeout-based: runs bpftrace for N seconds then kills it
 *   - Output is line-oriented text that callers parse
 *   - Requires root (USDT probes need CAP_SYS_ADMIN or CAP_BPF+CAP_PERFMON)
 */
import { execFile, spawn, ChildProcess } from "child_process";
import { promisify } from "util";
import { sshExecSafe } from "../utils/ssh-exec.js";

const execFileAsync = promisify(execFile);

// Default libmtl.so path (system-wide install)
const DEFAULT_LIBMTL_PATHS = [
  "/usr/local/lib/x86_64-linux-gnu/libmtl.so",
  "/usr/local/lib/libmtl.so",
  "/usr/lib/x86_64-linux-gnu/libmtl.so",
  "/usr/lib/libmtl.so",
];

export interface BpftraceResult {
  stdout: string;
  stderr: string;
  exitCode: number;
  timedOut: boolean;
}

export interface UsdtProbeInfo {
  provider: string;
  probe: string;
  full_name: string;
}

class BpftraceBridge {
  private _available = false;
  private _version = "";
  private _libmtlPath: string | null = null;
  private _probeCount = 0;
  private _missingCaps: string[] = [];

  get isAvailable(): boolean { return this._available; }
  get version(): string { return this._version; }
  get libmtlPath(): string | null { return this._libmtlPath; }
  get probeCount(): number { return this._probeCount; }
  get missingCapabilities(): string[] { return this._missingCaps; }

  /**
   * Detect bpftrace availability and USDT probe support.
   * Called once at startup from index.ts.
   */
  async detect(): Promise<void> {
    this._missingCaps = [];

    // Check bpftrace binary
    try {
      const { stdout } = await execFileAsync("bpftrace", ["--version"], { timeout: 5000 });
      const m = stdout.match(/bpftrace\s+v?([\d.]+)/i);
      if (m) this._version = m[1];
    } catch {
      this._missingCaps.push("bpftrace not found");
      return;
    }

    // Find libmtl.so
    for (const p of DEFAULT_LIBMTL_PATHS) {
      try {
        const check = await sshExecSafe("localhost", `test -f ${p} && echo yes`);
        if (check?.trim() === "yes") {
          this._libmtlPath = p;
          break;
        }
      } catch { /* try next */ }
    }

    // Also check ldconfig
    if (!this._libmtlPath) {
      try {
        const ldOut = await sshExecSafe("localhost", "ldconfig -p 2>/dev/null | grep libmtl.so | head -1");
        if (ldOut) {
          const m = ldOut.match(/=>\s*(\S+)/);
          if (m) this._libmtlPath = m[1];
        }
      } catch { /* ignore */ }
    }

    if (!this._libmtlPath) {
      this._missingCaps.push("libmtl.so not found (MTL not installed with USDT support)");
      // bpftrace itself is still available for non-MTL USDT
    }

    // Count probes if libmtl found
    if (this._libmtlPath) {
      try {
        const { stdout } = await execFileAsync("bpftrace", ["-l", `usdt:${this._libmtlPath}:*`], { timeout: 10000 });
        this._probeCount = stdout.trim().split("\n").filter(l => l.startsWith("usdt:")).length;
      } catch {
        this._missingCaps.push("bpftrace -l failed (may need root)");
      }
    }

    // Check if we can actually attach (needs root)
    try {
      const uid = await sshExecSafe("localhost", "id -u");
      if (uid?.trim() !== "0") {
        this._missingCaps.push("bpftrace requires root (current uid=" + uid?.trim() + ")");
      }
    } catch { /* ignore */ }

    this._available = true;
  }

  /**
   * List all USDT probes available in libmtl.so.
   */
  async listProbes(): Promise<UsdtProbeInfo[]> {
    if (!this._libmtlPath) return [];

    try {
      const { stdout } = await execFileAsync("bpftrace", ["-l", `usdt:${this._libmtlPath}:*`], { timeout: 10000 });
      return stdout.trim().split("\n")
        .filter(l => l.startsWith("usdt:"))
        .map(l => {
          // usdt:/path/libmtl.so:provider:probe
          const parts = l.split(":");
          const provider = parts[parts.length - 2];
          const probe = parts[parts.length - 1];
          return { provider, probe, full_name: `${provider}:${probe}` };
        });
    } catch {
      return [];
    }
  }

  /**
   * Run a bpftrace script against a specific PID with a timeout.
   *
   * @param script    bpftrace script body (e.g. 'usdt::sys:log_msg { printf(...) }')
   * @param pid       target process PID
   * @param timeoutMs max runtime in milliseconds (bpftrace is killed after this)
   * @param env       additional environment variables (e.g. BPFTRACE_MAX_STRLEN=200)
   * @returns         stdout, stderr, exit code, and whether it timed out
   */
  async runScript(
    script: string,
    pid: number,
    timeoutMs: number = 12000,
    env?: Record<string, string>,
  ): Promise<BpftraceResult> {
    return new Promise((resolve) => {
      const procEnv = {
        ...process.env,
        BPFTRACE_MAX_STRLEN: "200",
        ...env,
      };

      const child: ChildProcess = spawn(
        "bpftrace",
        ["-e", script, "-p", String(pid)],
        {
          env: procEnv,
          stdio: ["ignore", "pipe", "pipe"],
        },
      );

      let stdout = "";
      let stderr = "";
      let timedOut = false;
      let finished = false;

      child.stdout?.on("data", (data: Buffer) => { stdout += data.toString(); });
      child.stderr?.on("data", (data: Buffer) => { stderr += data.toString(); });

      const timer = setTimeout(() => {
        timedOut = true;
        // Send SIGINT first (bpftrace prints final maps on SIGINT)
        child.kill("SIGINT");
        // Hard kill after 2s if still alive
        setTimeout(() => {
          if (!finished) child.kill("SIGKILL");
        }, 2000);
      }, timeoutMs);

      child.on("close", (code) => {
        finished = true;
        clearTimeout(timer);
        resolve({ stdout, stderr, exitCode: code ?? -1, timedOut });
      });

      child.on("error", (err) => {
        finished = true;
        clearTimeout(timer);
        resolve({ stdout, stderr: err.message, exitCode: -1, timedOut: false });
      });
    });
  }

  /**
   * Run a bpftrace script against ALL processes matching a name pattern.
   * Discovers PIDs first, then runs the script against each.
   * Returns combined results.
   */
  async runScriptForProcessName(
    script: string,
    processName: string,
    timeoutMs: number = 12000,
    env?: Record<string, string>,
  ): Promise<{ pid: number; name: string; result: BpftraceResult }[]> {
    // Find PIDs matching the process name
    const pidOutput = await sshExecSafe("localhost",
      `pgrep -a '${processName}' 2>/dev/null || true`);
    if (!pidOutput?.trim()) return [];

    const results: { pid: number; name: string; result: BpftraceResult }[] = [];
    const lines = pidOutput.trim().split("\n");

    for (const line of lines) {
      const m = line.match(/^(\d+)\s+(.+)/);
      if (!m) continue;
      const pid = parseInt(m[1], 10);
      const name = m[2].split(/\s+/)[0]; // first token of cmdline

      const result = await this.runScript(script, pid, timeoutMs, env);
      results.push({ pid, name, result });
    }

    return results;
  }
}

// ─── Singleton ──────────────────────────────────────────────────────────────

let _instance: BpftraceBridge | null = null;

export function getBpftraceBridge(): BpftraceBridge {
  if (!_instance) _instance = new BpftraceBridge();
  return _instance;
}
