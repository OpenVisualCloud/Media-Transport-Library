/**
 * context_switch_rate(window_ms) — Global and per-CPU switch rates.
 */
import type { ToolResponse, ContextSwitchRateData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readProcStatCtxt } from "../collectors/proc-stat.js";
import { sleep, isReadable } from "../utils/proc-reader.js";
import { readFile } from "fs/promises";
import { readFileHost, isReadableHost, isLocalHost } from "../remote/host-utils.js";

export async function contextSwitchRate(params: {
  window_ms?: number;
  host?: string;
}): Promise<ToolResponse<ContextSwitchRateData>> {
  const windowMs = params.window_ms ?? 1000;
  const host = params.host;
  const meta = await buildMeta("fallback", windowMs);

  try {
    // Global ctxt from /proc/stat
    const ctxtBefore = await readProcStatCtxt(host);

    // Per-CPU from /proc/schedstat (if available)
    let perCpuBefore: Map<number, number> | null = null;
    const schedstatReadable = isLocalHost(host)
      ? await isReadable("/proc/schedstat")
      : await isReadableHost(host, "/proc/schedstat");
    if (schedstatReadable) {
      perCpuBefore = await readSchedstatSwitches(host);
    }

    await sleep(windowMs);

    const ctxtAfter = await readProcStatCtxt(host);

    const globalDelta = Math.max(0, ctxtAfter - ctxtBefore);
    const globalPerSec = Math.round((globalDelta / windowMs) * 1000);

    const data: ContextSwitchRateData = {
      global_ctxt_per_s: globalPerSec,
    };

    if (perCpuBefore) {
      const perCpuAfter = await readSchedstatSwitches(host);
      if (perCpuAfter) {
        const perCpu: { cpu: number; ctxt_per_s: number }[] = [];
        for (const [cpu, beforeCount] of perCpuBefore.entries()) {
          const afterCount = perCpuAfter.get(cpu) ?? beforeCount;
          const delta = Math.max(0, afterCount - beforeCount);
          perCpu.push({
            cpu,
            ctxt_per_s: Math.round((delta / windowMs) * 1000),
          });
        }
        perCpu.sort((a, b) => a.cpu - b.cpu);
        data.per_cpu = perCpu;
      }
    }

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "CONTEXT_SWITCH_ERROR", `Failed: ${err}`);
  }
}

/**
 * Read per-CPU context switch counts from /proc/schedstat.
 */
async function readSchedstatSwitches(host?: string): Promise<Map<number, number> | null> {
  try {
    const content = isLocalHost(host)
      ? await readFile("/proc/schedstat", "utf-8")
      : await readFileHost(host, "/proc/schedstat");
    const map = new Map<number, number>();

    for (const line of content.split("\n")) {
      const match = line.match(/^cpu(\d+)\s+(.+)/);
      if (!match) continue;
      const cpuId = parseInt(match[1], 10);
      const fields = match[2].trim().split(/\s+/).map(Number);
      // fields[1] is sched_count (context switches)
      map.set(cpuId, fields[1] ?? 0);
    }

    return map;
  } catch {
    return null;
  }
}
