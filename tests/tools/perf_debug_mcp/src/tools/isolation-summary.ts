/**
 * isolation_summary() — Summarize CPU isolation expectations vs reality.
 */
import type { ToolResponse, IsolationSummaryData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getIsolatedCpus, parseKernelCmdline } from "../collectors/sysfs.js";
import { readProcInterrupts } from "../collectors/interrupts.js";
import { buildTaskTable } from "../collectors/proc-pid-stat.js";
import { isKernelThread } from "../utils/helpers.js";
import { parseCpuList, readFileTrimmed } from "../utils/proc-reader.js";

export async function isolationSummary(params?: {
  host?: string;
}): Promise<ToolResponse<IsolationSummaryData>> {
  const host = params?.host;
  const meta = await buildMeta("fallback");

  try {
    const cmdlineFlags = await parseKernelCmdline(host);

    // Parse each flag
    const isolatedFromSysfs = await getIsolatedCpus(host);

    const isolcpusStr = cmdlineFlags["isolcpus"] ?? "";
    let isolcpusParsed: number[] = [];
    if (isolcpusStr) {
      // isolcpus can have flags like "managed_irq,domain,..." before the CPU list
      const parts = isolcpusStr.split(",");
      // Find the first part that looks like a CPU range
      let cpuPart = "";
      for (const p of parts) {
        if (/^\d/.test(p) || p.includes("-")) {
          cpuPart = cpuPart ? `${cpuPart},${p}` : p;
        }
      }
      if (cpuPart) isolcpusParsed = parseCpuList(cpuPart);
    }

    const nohzFullStr = cmdlineFlags["nohz_full"] ?? "";
    const nohzFullCpus = nohzFullStr ? parseCpuList(nohzFullStr) : [];

    const rcuNocbsStr = cmdlineFlags["rcu_nocbs"] ?? "";
    const rcuNocbsCpus = rcuNocbsStr ? parseCpuList(rcuNocbsStr) : [];

    const isolatedCpus = isolatedFromSysfs.length > 0 ? isolatedFromSysfs : isolcpusParsed;

    // Generate warnings
    const warnings: string[] = [];

    if (isolatedCpus.length === 0) {
      warnings.push("No isolated CPUs detected (no isolcpus= or sysfs isolated list)");
    }

    // Check for IRQs pinned to isolated CPUs
    if (isolatedCpus.length > 0) {
      const interrupts = await readProcInterrupts(host);
      const isolatedSet = new Set(isolatedCpus);

      for (const line of interrupts.lines) {
        // Check if any isolated CPU has significant IRQ counts
        for (const cpu of isolatedCpus) {
          if (cpu < line.per_cpu_counts.length && line.per_cpu_counts[cpu] > 100) {
            warnings.push(
              `IRQ ${line.irq} (${line.device_name}) has ${line.per_cpu_counts[cpu]} counts on isolated CPU ${cpu}`
            );
          }
        }
      }

      // Check for kernel threads observed on isolated CPUs
      const tasks = await buildTaskTable({ limit: 10000, host });
      const kthreadsOnIsolated: string[] = [];

      for (const t of tasks) {
        if (!isKernelThread(t.ppid, t.flags, t.vsize)) continue;
        if (!isolatedSet.has(t.processor)) continue;
        if (t.state !== "R" && t.state !== "S") continue;

        kthreadsOnIsolated.push(`${t.comm} (pid=${t.pid}) last seen on isolated CPU ${t.processor}`);
      }

      if (kthreadsOnIsolated.length > 0) {
        warnings.push(`${kthreadsOnIsolated.length} kernel thread(s) observed on isolated CPUs`);
        // Add first few details
        for (const detail of kthreadsOnIsolated.slice(0, 5)) {
          warnings.push(`  - ${detail}`);
        }
      }

      // Check nohz_full alignment
      if (nohzFullCpus.length > 0) {
        const nohzSet = new Set(nohzFullCpus);
        const misaligned = isolatedCpus.filter((c) => !nohzSet.has(c));
        if (misaligned.length > 0) {
          warnings.push(
            `CPUs ${misaligned.join(",")} are in isolcpus but not in nohz_full — they still get timer ticks`
          );
        }
      } else if (isolatedCpus.length > 0) {
        warnings.push("nohz_full is not set — isolated CPUs still receive periodic timer ticks");
      }

      // Check rcu_nocbs alignment
      if (rcuNocbsCpus.length > 0) {
        const rcuSet = new Set(rcuNocbsCpus);
        const misaligned = isolatedCpus.filter((c) => !rcuSet.has(c));
        if (misaligned.length > 0) {
          warnings.push(
            `CPUs ${misaligned.join(",")} are in isolcpus but not in rcu_nocbs — RCU callbacks may run there`
          );
        }
      } else if (isolatedCpus.length > 0) {
        warnings.push("rcu_nocbs is not set — RCU callbacks may execute on isolated CPUs");
      }
    }

    const data: IsolationSummaryData = {
      cmdline_flags: cmdlineFlags,
      isolated_cpus: isolatedCpus,
      nohz_full_cpus: nohzFullCpus,
      rcu_nocbs_cpus: rcuNocbsCpus,
      warnings,
    };

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "ISOLATION_SUMMARY_ERROR", `Failed: ${err}`);
  }
}
