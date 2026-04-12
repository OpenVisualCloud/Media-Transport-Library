/**
 * throttling_summary(window_ms) — Thermal throttle indicators.
 */
import type { ToolResponse, ThrottlingSummaryData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readThermalThrottleCount, getOnlineCpus } from "../collectors/sysfs.js";
import { readCgroupCpuLimits } from "../collectors/cgroup.js";
import { readFileTrimmed, isReadable } from "../utils/proc-reader.js";
import { readFileTrimmedHost, isLocalHost } from "../remote/host-utils.js";

export async function throttlingSummary(params: {
  window_ms?: number;
  host?: string;
}): Promise<ToolResponse<ThrottlingSummaryData>> {
  const host = params.host;
  const meta = await buildMeta("fallback");

  // Helper: read a file trimmed, local or remote
  const readTr = (path: string) =>
    isLocalHost(host) ? readFileTrimmed(path) : readFileTrimmedHost(host, path);

  try {
    const notes: string[] = [];
    let thermalCounts: Record<string, number> | undefined;

    // Try to read thermal throttle counts
    const cpus = await getOnlineCpus(host);
    const counts: Record<string, number> = {};
    let anyThrotle = false;

    for (const cpu of cpus) {
      const count = await readThermalThrottleCount(cpu, host);
      if (count !== null) {
        counts[`cpu${cpu}`] = count;
        if (count > 0) anyThrotle = true;
      }
    }

    if (Object.keys(counts).length > 0) {
      thermalCounts = counts;
      if (anyThrotle) {
        notes.push("Thermal throttling events detected on some CPUs (cumulative counts since boot)");
      } else {
        notes.push("No thermal throttle events detected (counters are zero)");
      }
    } else {
      notes.push("Thermal throttle counters not available on this platform");
    }

    // Check for intel_pstate
    const pstateStatus = await readTr("/sys/devices/system/cpu/intel_pstate/status");
    if (pstateStatus) {
      notes.push(`intel_pstate driver: status=${pstateStatus}`);
      const noTurbo = await readTr("/sys/devices/system/cpu/intel_pstate/no_turbo");
      if (noTurbo) {
        notes.push(`intel_pstate: no_turbo=${noTurbo}`);
        if (noTurbo === "1") {
          notes.push("WARNING: Turbo boost is disabled — may limit max frequency");
        }
      }
    }

    // Check for amd_pstate
    const amdPstateStatus = await readTr("/sys/devices/system/cpu/amd_pstate/status");
    if (amdPstateStatus) {
      notes.push(`amd_pstate driver: status=${amdPstateStatus}`);
    }

    // Check for CPU frequency scaling limits
    if (cpus.length > 0) {
      const exampleCpu = cpus[0];
      const governor = await readTr(
        `/sys/devices/system/cpu/cpu${exampleCpu}/cpufreq/scaling_governor`
      );
      if (governor) {
        notes.push(`Frequency governor: ${governor}`);
        if (governor === "powersave") {
          notes.push("WARNING: powersave governor is active — CPU may not reach max frequency under load");
        }
      }
    }

    // Check RT throttling
    const rtPeriod = await readTr("/proc/sys/kernel/sched_rt_period_us");
    const rtRuntime = await readTr("/proc/sys/kernel/sched_rt_runtime_us");
    if (rtPeriod && rtRuntime) {
      const period = parseInt(rtPeriod, 10);
      const runtime = parseInt(rtRuntime, 10);
      if (runtime === -1) {
        notes.push("RT group scheduling: unlimited (sched_rt_runtime_us = -1)");
      } else {
        notes.push(
          `RT group scheduling: ${runtime}us / ${period}us (${Math.round((runtime / period) * 100)}% ceiling)`
        );
      }
    }

    // Note about dmesg
    notes.push(
      "Note: dmesg-based thermal/throttle detection is not used by default (noisy/invasive). " +
      "Consult dmesg manually if thermal throttling is suspected."
    );

    const data: ThrottlingSummaryData = { notes };
    if (thermalCounts && Object.keys(thermalCounts).length > 0) {
      data.thermal_throttle_count = thermalCounts;
    }

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "THROTTLING_SUMMARY_ERROR", `Failed: ${err}`);
  }
}
