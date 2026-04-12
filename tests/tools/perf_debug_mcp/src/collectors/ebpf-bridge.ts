/**
 * eBPF bridge — interface to the native eBPF collector.
 * The native helper binary (cpu-debug-ebpf) is a Rust binary that loads
 * CO-RE eBPF programs and outputs JSON snapshots to stdout.
 *
 * This bridge manages spawning the collector, sending commands, and
 * reading results.
 */
import { spawn, ChildProcess } from "child_process";
import { existsSync } from "fs";
import { resolve, dirname } from "path";
import { fileURLToPath } from "url";
import type { EbpfSnapshot } from "../types.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Search paths for the native helper binary
const HELPER_SEARCH_PATHS = [
  resolve(__dirname, "../../native/target/release/cpu-debug-ebpf"),
  resolve(__dirname, "../../native/target/debug/cpu-debug-ebpf"),
  "/usr/local/bin/cpu-debug-ebpf",
  "/usr/bin/cpu-debug-ebpf",
];

export class EbpfBridge {
  private helperPath: string | null = null;
  private available: boolean = false;
  private enabled: boolean = false;
  private helperProcess: ChildProcess | null = null;
  private missingCaps: string[] = [];

  /**
   * Detect whether the eBPF helper is available and can run.
   */
  async detect(): Promise<void> {
    // Find the helper binary
    for (const p of HELPER_SEARCH_PATHS) {
      if (existsSync(p)) {
        this.helperPath = p;
        break;
      }
    }

    if (!this.helperPath) {
      this.available = false;
      this.missingCaps.push("native helper binary not found");
      return;
    }

    // Try to run a capability check
    try {
      const result = await this.runCommand("capabilities");
      if (result && result.ebpf_available) {
        this.available = true;
        this.enabled = true;
      } else {
        this.available = false;
        if (result?.missing_caps) {
          this.missingCaps = result.missing_caps;
        }
      }
    } catch (err) {
      this.available = false;
      this.missingCaps.push(`helper execution failed: ${err}`);
    }
  }

  get isAvailable(): boolean {
    return this.available;
  }

  get isEnabled(): boolean {
    return this.enabled;
  }

  get missingCapabilities(): string[] {
    return this.missingCaps;
  }

  /**
   * Get a scheduling snapshot from the eBPF collector.
   */
  async getSchedSnapshot(windowMs: number): Promise<EbpfSnapshot | null> {
    if (!this.enabled || !this.helperPath) return null;
    try {
      return await this.runCommand("sched_snapshot", { window_ms: windowMs });
    } catch {
      return null;
    }
  }

  /**
   * Run a command against the native helper and parse JSON output.
   */
  private runCommand(command: string, args?: Record<string, unknown>): Promise<any> {
    return new Promise((resolve, reject) => {
      if (!this.helperPath) {
        reject(new Error("No helper binary"));
        return;
      }

      const cmdArgs = [command];
      if (args) {
        cmdArgs.push(JSON.stringify(args));
      }

      const proc = spawn(this.helperPath, cmdArgs, {
        stdio: ["pipe", "pipe", "pipe"],
        timeout: 10_000,
      });

      let stdout = "";
      let stderr = "";

      proc.stdout?.on("data", (data: Buffer) => {
        stdout += data.toString();
      });
      proc.stderr?.on("data", (data: Buffer) => {
        stderr += data.toString();
      });

      proc.on("error", (err) => {
        reject(err);
      });

      proc.on("close", (code) => {
        if (code === 0 && stdout) {
          try {
            resolve(JSON.parse(stdout));
          } catch {
            reject(new Error(`Invalid JSON from helper: ${stdout.substring(0, 200)}`));
          }
        } else {
          reject(new Error(`Helper exited with code ${code}: ${stderr.substring(0, 200)}`));
        }
      });
    });
  }

  /**
   * Clean up.
   */
  shutdown(): void {
    if (this.helperProcess) {
      this.helperProcess.kill();
      this.helperProcess = null;
    }
  }
}

/** Singleton instance */
let _bridge: EbpfBridge | null = null;

export function getEbpfBridge(): EbpfBridge {
  if (!_bridge) {
    _bridge = new EbpfBridge();
  }
  return _bridge;
}
