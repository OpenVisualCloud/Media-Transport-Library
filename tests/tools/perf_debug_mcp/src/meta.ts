/**
 * Meta/timestamp generation for tool responses.
 */
import { hostname } from "os";
import { readFile } from "fs/promises";
import type { ResponseMeta, ModeUsed, HostInfo, ToolResponse, ResponseError } from "./types.js";

let _cpuCount: number | null = null;
let _kernel: string | null = null;
let _hostname: string | null = null;

export async function getHostInfo(): Promise<HostInfo> {
  if (!_hostname) _hostname = hostname();
  if (!_kernel) {
    try {
      _kernel = (await readFile("/proc/version", "utf-8")).split(" ").slice(0, 3).join(" ").trim();
    } catch {
      _kernel = "unknown";
    }
  }
  if (_cpuCount === null) {
    try {
      const stat = await readFile("/proc/stat", "utf-8");
      _cpuCount = stat.split("\n").filter((l) => /^cpu\d+/.test(l)).length;
    } catch {
      _cpuCount = 0;
    }
  }
  return { hostname: _hostname, kernel: _kernel, cpu_count: _cpuCount };
}

export function getMonotonicNs(): number {
  const [sec, ns] = process.hrtime();
  return sec * 1_000_000_000 + ns;
}

export async function buildMeta(mode: ModeUsed, windowMs?: number, includeHost = false): Promise<ResponseMeta> {
  const meta: ResponseMeta = {
    timestamp_wall: new Date().toISOString(),
    t_monotonic_ns: getMonotonicNs(),
    mode_used: mode,
  };
  if (windowMs !== undefined) meta.window_ms = windowMs;
  if (includeHost) meta.host = await getHostInfo();
  return meta;
}

export function okResponse<T>(data: T, meta: ResponseMeta): ToolResponse<T> {
  return { ok: true, meta, data };
}

export function errorResponse<T = unknown>(meta: ResponseMeta, code: string, message: string, hint?: string): ToolResponse<T> {
  const err: ResponseError = { code, message };
  if (hint) err.hint = hint;
  return { ok: false, meta, error: err };
}
