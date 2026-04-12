/**
 * runqueue_snapshot() — per-CPU runnable count, load averages, schedstat.
 */
import type { ToolResponse, RunqueueSnapshotData, RunqueueGlobal, RunqueuePerCpu } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readFile } from "fs/promises";
import { readFileTrimmed, isReadable } from "../utils/proc-reader.js";
import { readFileHost, readFileTrimmedHost, isReadableHost, isLocalHost } from "../remote/host-utils.js";

export async function runqueueSnapshot(params?: {
  host?: string;
}): Promise<ToolResponse<RunqueueSnapshotData>> {
  const host = params?.host;
  const meta = await buildMeta("fallback");

  try {
    // Read /proc/loadavg
    const loadavgStr = isLocalHost(host)
      ? await readFileTrimmed("/proc/loadavg")
      : await readFileTrimmedHost(host, "/proc/loadavg");
    let global: RunqueueGlobal = {
      loadavg_1: 0,
      loadavg_5: 0,
      loadavg_15: 0,
      runnable: 0,
      running: 0,
    };

    if (loadavgStr) {
      const parts = loadavgStr.split(/\s+/);
      global.loadavg_1 = parseFloat(parts[0]) || 0;
      global.loadavg_5 = parseFloat(parts[1]) || 0;
      global.loadavg_15 = parseFloat(parts[2]) || 0;

      // Field 4 is "runnable/total"
      const runnableParts = (parts[3] ?? "0/0").split("/");
      global.runnable = parseInt(runnableParts[0], 10) || 0;
      global.running = global.runnable; // Closest proxy
    }

    // Try /proc/schedstat for per-CPU data
    let perCpu: RunqueuePerCpu[] | undefined;

    if (isLocalHost(host) ? await isReadable("/proc/schedstat") : await isReadableHost(host, "/proc/schedstat")) {
      try {
        const content = isLocalHost(host)
          ? await readFile("/proc/schedstat", "utf-8")
          : await readFileHost(host, "/proc/schedstat");
        perCpu = [];

        for (const line of content.split("\n")) {
          // Format: cpu<N> <9 fields>
          // Fields: yld_count, sched_count, sched_goidle, ttwu_count, ttwu_local,
          //         rq_sched_info.run_delay, rq_sched_info.pcount, ...
          const match = line.match(/^cpu(\d+)\s+(.+)/);
          if (!match) continue;

          const cpuId = parseInt(match[1], 10);
          const fields = match[2].trim().split(/\s+/).map(Number);

          // fields[0-2] are the scheduling stats
          // sched_count = context switches for this CPU
          const schedCount = fields[1] ?? 0;

          perCpu.push({
            cpu: cpuId,
            context_switch_rate: schedCount, // raw count, consumer can diff
          });
        }
      } catch { /* ignore schedstat errors */ }
    }

    const data: RunqueueSnapshotData = { global };
    if (perCpu) data.per_cpu = perCpu;

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "RUNQUEUE_SNAPSHOT_ERROR", `Failed: ${err}`);
  }
}
