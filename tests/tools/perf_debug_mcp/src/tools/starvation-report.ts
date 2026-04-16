/**
 * starvation_report(target, window_ms, solo_cpu_hint, mode, thresholds)
 * High-level analysis tool for process/thread starvation.
 */
import type {
  ToolResponse, StarvationReportData, StarvationFlag, StarvationTarget,
  StarvationThreadReport, RunningTask, ModeUsed,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { buildTaskTable, listTids } from "../collectors/proc-pid-stat.js";
import { readTaskAffinity, isAllowedOnCpu } from "../collectors/affinity.js";
import { getSchedulerInfo } from "../collectors/scheduler.js";
import { readProcStat, computeCpuDeltas } from "../collectors/proc-stat.js";
import { readProcSoftirqs, computeSoftirqDeltas } from "../collectors/softirqs.js";
import { readProcInterrupts, computeInterruptDeltas } from "../collectors/interrupts.js";
import { readCgroupCpuLimits } from "../collectors/cgroup.js";
import { isKernelThread, stateToString, round2 } from "../utils/helpers.js";
import { sleep } from "../utils/proc-reader.js";

interface StarvationThresholds {
  runqueue_latency_high_ns?: number;
  co_runner_count_threshold?: number;
  softirq_pct_threshold?: number;
  irq_pct_threshold?: number;
  preemption_rate_threshold?: number;
}

export async function starvationReport(params: {
  target?: { pid?: number; tid?: number; comm_regex?: string };
  target_pid?: number;
  target_tid?: number;
  target_comm_regex?: string;
  window_ms?: number;
  solo_cpu_hint?: number | null;
  mode?: ModeUsed;
  thresholds?: StarvationThresholds;
  host?: string;
}): Promise<ToolResponse<StarvationReportData>> {
  const windowMs = params.window_ms ?? 2000;
  const requestedMode = params.mode ?? "auto";
  const host = params.host;
  const thresholds = params.thresholds ?? {};
  const coRunnerThreshold = thresholds.co_runner_count_threshold ?? 2;
  const softirqPctThreshold = thresholds.softirq_pct_threshold ?? 5;
  const irqPctThreshold = thresholds.irq_pct_threshold ?? 5;

  let modeUsed: ModeUsed = "fallback";
  const meta = await buildMeta(modeUsed, windowMs);

  try {
    // ── Step 1: Resolve target ──────────────────────────────────────
    // Support both nested target object and flat params (flat takes priority if both provided)
    const effectiveTarget = {
      pid: params.target_pid ?? params.target?.pid,
      tid: params.target_tid ?? params.target?.tid,
      comm_regex: params.target_comm_regex ?? params.target?.comm_regex,
    };

    const targets: StarvationTarget[] = [];
    const allTasks = await buildTaskTable({ host });

    if (effectiveTarget.tid !== undefined) {
      const t = allTasks.find((tt) => tt.pid === effectiveTarget.tid);
      if (t) targets.push({ pid: t.pid, tid: t.pid, comm: t.comm });
    } else if (effectiveTarget.pid !== undefined) {
      const tids = await listTids(effectiveTarget.pid, host);
      for (const tid of tids) {
        const t = allTasks.find((tt) => tt.pid === tid);
        if (t) targets.push({ pid: effectiveTarget.pid, tid, comm: t.comm });
      }
    } else if (effectiveTarget.comm_regex !== undefined) {
      const regex = new RegExp(effectiveTarget.comm_regex, "i");
      for (const t of allTasks) {
        if (regex.test(t.comm)) {
          targets.push({ pid: t.pid, tid: t.pid, comm: t.comm });
        }
      }
    }

    if (targets.length === 0) {
      return errorResponse(meta, "TARGET_NOT_FOUND", "No tasks matched the target specification",
        "Check PID/TID exists or comm_regex matches a running process");
    }

    // ── Step 2: Affinity/cpuset summary ─────────────────────────────
    const firstTarget = targets[0];
    const affinity = await readTaskAffinity(firstTarget.tid, firstTarget.tid, host);
    const cpusAllowed = affinity?.cpus_allowed ?? [];
    const isSingleCpuPinned = cpusAllowed.length === 1;
    const soloCpu = params.solo_cpu_hint ?? (isSingleCpuPinned ? cpusAllowed[0] : null);

    // ── Step 3: Windowed observation ────────────────────────────────
    const HZ = 100;

    // Take before snapshots
    const statBefore = await readProcStat(host);
    const softirqBefore = await readProcSoftirqs(host);
    const irqBefore = await readProcInterrupts(host);
    const tasksBefore = await buildTaskTable({ host });

    const beforeMap = new Map<number, { utime: number; stime: number; processor: number }>();
    for (const t of tasksBefore) {
      beforeMap.set(t.pid, { utime: t.utime, stime: t.stime, processor: t.processor });
    }

    // Wait for observation window
    await sleep(windowMs);

    // Take after snapshots
    const statAfter = await readProcStat(host);
    const softirqAfter = await readProcSoftirqs(host);
    const irqAfter = await readProcInterrupts(host);
    const tasksAfter = await buildTaskTable({ host });

    modeUsed = "fallback";
    meta.mode_used = modeUsed;

    // ── Step 4: Compute runtime per thread ──────────────────────────
    const runtimePerThread: StarvationThreadReport[] = [];
    const targetTids = new Set(targets.map((t) => t.tid));
    const windowNs = windowMs * 1_000_000;

    for (const t of tasksAfter) {
      if (!targetTids.has(t.pid)) continue;

      const prev = beforeMap.get(t.pid);
      let runtimeNs = 0;
      let switches = 0;

      if (prev) {
        const utimeDelta = Math.max(0, t.utime - prev.utime);
        const stimeDelta = Math.max(0, t.stime - prev.stime);
        runtimeNs = Math.round(((utimeDelta + stimeDelta) / HZ) * 1e9);
      }

      runtimePerThread.push({
        tid: t.pid,
        comm: t.comm,
        runtime_ns: runtimeNs,
        runtime_pct: round2((runtimeNs / windowNs) * 100),
        cpu_observed: [t.processor],
        switches,
      });
    }

    // ── Step 5: Find interferers on the solo CPU ────────────────────
    const topInterferers: RunningTask[] = [];

    if (soloCpu !== null) {
      for (const t of tasksAfter) {
        if (t.processor !== soloCpu) continue;
        if (targetTids.has(t.pid)) continue;

        const prev = beforeMap.get(t.pid);
        let runtimeNs = 0;
        if (prev) {
          const delta = Math.max(0, (t.utime - prev.utime) + (t.stime - prev.stime));
          runtimeNs = Math.round((delta / HZ) * 1e9);
        }

        if (runtimeNs > 0 || t.state === "R") {
          const schedInfo = getSchedulerInfo(t);
          topInterferers.push({
            pid: t.pid,
            tid: t.pid,
            comm: t.comm,
            runtime_ns: runtimeNs > 0 ? runtimeNs : undefined,
            runtime_pct: runtimeNs > 0 ? round2((runtimeNs / windowNs) * 100) : undefined,
            last_seen_ms_ago: 0,
            policy: schedInfo.policy,
            state: stateToString(t.state),
          });
        }
      }

      // Sort interferers by runtime desc
      topInterferers.sort((a, b) => (b.runtime_ns ?? 0) - (a.runtime_ns ?? 0));
    }

    // ── Step 6: Compute starvation flags ────────────────────────────
    const flags: StarvationFlag[] = [];

    // Check co-runners
    if (soloCpu !== null && topInterferers.length >= coRunnerThreshold) {
      flags.push("UNEXPECTED_CO_RUNNERS_ON_SOLO_CPU");
    }

    // Check softirq pressure
    const softirqDeltas = computeSoftirqDeltas(softirqBefore, softirqAfter);
    if (soloCpu !== null) {
      const cpuSoftirq = softirqDeltas.per_cpu.find((c) => c.cpu === soloCpu);
      if (cpuSoftirq && cpuSoftirq.total_delta > 0) {
        // Rough estimate: if > threshold percentage of events, flag it
        const totalAllCpus = softirqDeltas.per_cpu.reduce((s, c) => s + c.total_delta, 0);
        const pct = totalAllCpus > 0 ? (cpuSoftirq.total_delta / totalAllCpus) * 100 : 0;
        if (pct > softirqPctThreshold || softirqDeltas.hot_cpus.includes(soloCpu)) {
          flags.push("SOFTIRQ_PRESSURE");
        }
      }
    }

    // Check IRQ pressure
    const irqDeltas = computeInterruptDeltas(irqBefore, irqAfter);
    if (soloCpu !== null) {
      const cpuIrq = irqDeltas.per_cpu_totals.find((c) => c.cpu === soloCpu);
      if (cpuIrq && cpuIrq.irq_total_delta > 0) {
        const totalAllCpus = irqDeltas.per_cpu_totals.reduce((s, c) => s + c.irq_total_delta, 0);
        const pct = totalAllCpus > 0 ? (cpuIrq.irq_total_delta / totalAllCpus) * 100 : 0;
        if (pct > irqPctThreshold) {
          flags.push("IRQ_PRESSURE");
        }
      }
    }

    // Check CPU utilization for solo CPU — if target didn't get enough time
    const cpuDeltas = computeCpuDeltas(statBefore, statAfter);
    if (soloCpu !== null) {
      const cpuLoad = cpuDeltas.find((c) => c.cpu === soloCpu);
      if (cpuLoad && cpuLoad.irq_pct + cpuLoad.softirq_pct > irqPctThreshold) {
        if (!flags.includes("IRQ_PRESSURE") && cpuLoad.irq_pct > irqPctThreshold) {
          flags.push("IRQ_PRESSURE");
        }
        if (!flags.includes("SOFTIRQ_PRESSURE") && cpuLoad.softirq_pct > softirqPctThreshold) {
          flags.push("SOFTIRQ_PRESSURE");
        }
      }
    }

    // Check for preemption — many interferers suggest preemption
    if (topInterferers.length > 5) {
      flags.push("FREQUENT_PREEMPTION");
    }

    // Check cgroup throttling
    let cgroupThrottled = false;
    try {
      const cgLimits = await readCgroupCpuLimits(firstTarget.pid, host);
      if (cgLimits) {
        if (cgLimits.nr_throttled_delta && cgLimits.nr_throttled_delta > 0) {
          flags.push("CPU_THROTTLING_DETECTED");
          cgroupThrottled = true;
        }
      }
    } catch { /* ignore */ }

    // Check RT throttling
    try {
      const { readFile } = await import("fs/promises");
      const rtPeriod = await readFile("/proc/sys/kernel/sched_rt_period_us", "utf-8").catch(() => null);
      const rtRuntime = await readFile("/proc/sys/kernel/sched_rt_runtime_us", "utf-8").catch(() => null);
      if (rtPeriod && rtRuntime) {
        const period = parseInt(rtPeriod.trim(), 10);
        const runtime = parseInt(rtRuntime.trim(), 10);
        // RT throttling is off when runtime == -1
        if (runtime !== -1 && runtime < period) {
          // Check if target is RT
          const targetTask = tasksAfter.find((t) => t.pid === firstTarget.tid);
          if (targetTask && (targetTask.policy === 1 || targetTask.policy === 2)) {
            // SCHED_FIFO or SCHED_RR + RT throttling enabled
            flags.push("RT_THROTTLING_DETECTED");
          }
        }
      }
    } catch { /* ignore */ }

    // Runtime starvation heuristic: if target got < 50% of window on a solo CPU
    for (const thr of runtimePerThread) {
      if (isSingleCpuPinned && thr.runtime_pct !== undefined && thr.runtime_pct < 50) {
        if (!flags.includes("RUNQUEUE_LATENCY_HIGH")) {
          flags.push("RUNQUEUE_LATENCY_HIGH");
        }
      }
    }

    // ── Step 7: Generate recommendations ────────────────────────────
    const recommendations: string[] = [];

    if (flags.includes("UNEXPECTED_CO_RUNNERS_ON_SOLO_CPU")) {
      recommendations.push(
        `${topInterferers.length} co-runners found on CPU ${soloCpu}. ` +
        "Use 'allowed_on_core' to identify all tasks allowed on this CPU. " +
        "Consider tighter affinity or isolcpus= boot parameter."
      );
    }
    if (flags.includes("SOFTIRQ_PRESSURE")) {
      recommendations.push(
        `Softirq activity detected on CPU ${soloCpu}. ` +
        "Use 'softirq_snapshot' to identify which softirq vectors are hot. " +
        "Consider moving NIC IRQs away from this CPU."
      );
    }
    if (flags.includes("IRQ_PRESSURE")) {
      recommendations.push(
        `IRQ activity detected on CPU ${soloCpu}. ` +
        "Use 'irq_distribution' and 'irq_affinity' to identify and relocate IRQs."
      );
    }
    if (flags.includes("FREQUENT_PREEMPTION")) {
      recommendations.push(
        "High preemption frequency detected. " +
        "Use 'running_on_core' with a shorter window to identify rapid context switches."
      );
    }
    if (flags.includes("CPU_THROTTLING_DETECTED")) {
      recommendations.push(
        "cgroup CPU throttling is active. " +
        "Use 'cgroup_cpu_limits' to inspect cpu.max and throttle counters."
      );
    }
    if (flags.includes("RT_THROTTLING_DETECTED")) {
      recommendations.push(
        "RT group scheduling throttling is enabled (/proc/sys/kernel/sched_rt_runtime_us). " +
        "This limits RT tasks to a fraction of each period."
      );
    }
    if (flags.includes("RUNQUEUE_LATENCY_HIGH")) {
      recommendations.push(
        "Target thread received less than 50% of available CPU on its pinned core. " +
        "Investigate co-runners and IRQ/softirq interference."
      );
    }
    if (flags.length === 0) {
      recommendations.push(
        "No obvious starvation signals detected in this window. " +
        "Consider re-running with a longer window_ms or using eBPF mode for more accurate measurement."
      );
    }

    const data: StarvationReportData = {
      target_resolution: targets.slice(0, 50),
      affinity_summary: {
        cpus_allowed: cpusAllowed,
        is_single_cpu_pinned: isSingleCpuPinned,
        cpuset_cpus: affinity?.cpuset_cpus,
      },
      runtime_per_thread: runtimePerThread,
      top_interferers: topInterferers.slice(0, 20),
      starvation_flags: flags,
      recommended_next_steps: recommendations,
    };

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "STARVATION_REPORT_ERROR", `Failed: ${err}`);
  }
}
