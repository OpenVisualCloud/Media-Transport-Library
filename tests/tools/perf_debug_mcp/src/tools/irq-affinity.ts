/**
 * irq_affinity(irq_or_regex, include_effective)
 * Show configured and effective affinity for IRQs.
 */
import type { ToolResponse, IrqAffinityData, IrqAffinityEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readFileTrimmed, parseCpuList, listEntries } from "../utils/proc-reader.js";
import { readFileTrimmedHost, listEntriesHost, isLocalHost } from "../remote/host-utils.js";

export async function irqAffinity(params: {
  irq_or_regex: string;
  include_effective?: boolean;
  host?: string;
}): Promise<ToolResponse<IrqAffinityData>> {
  const includeEffective = params.include_effective ?? true;
  const host = params.host;
  const meta = await buildMeta("fallback");

  // Helper: read a file trimmed, local or remote
  const readTr = (path: string) =>
    isLocalHost(host) ? readFileTrimmed(path) : readFileTrimmedHost(host, path);
  const listEnt = (path: string) =>
    isLocalHost(host) ? listEntries(path) : listEntriesHost(host, path);

  try {
    // List all IRQs in /proc/irq/
    const irqDirs = await listEnt("/proc/irq");
    const irqNumbers = irqDirs.filter((d) => /^\d+$/.test(d));

    // Determine if irq_or_regex is a number or a regex
    const isExact = /^\d+$/.test(params.irq_or_regex);
    let regex: RegExp | null = null;
    if (!isExact) {
      try {
        regex = new RegExp(params.irq_or_regex, "i");
      } catch {
        return errorResponse(meta, "INVALID_REGEX", `Invalid regex: ${params.irq_or_regex}`);
      }
    }

    const irqs: IrqAffinityEntry[] = [];

    for (const irqNum of irqNumbers) {
      const base = `/proc/irq/${irqNum}`;

      // Read action files to get IRQ name
      const actions = await readTr(`${base}/actions`);
      const irqName = actions ?? undefined;

      // Filter by IRQ number or name regex
      if (isExact) {
        if (irqNum !== params.irq_or_regex) continue;
      } else if (regex) {
        if (!regex.test(irqNum) && !(irqName && regex.test(irqName))) continue;
      }

      // Read configured affinity
      const affinityListStr = await readTr(`${base}/smp_affinity_list`);
      const affinityList = affinityListStr ? parseCpuList(affinityListStr) : [];

      const entry: IrqAffinityEntry = {
        irq: irqNum,
        name: irqName,
        affinity_list: affinityList,
      };

      // Read effective affinity if requested
      if (includeEffective) {
        const effListStr = await readTr(`${base}/effective_affinity_list`);
        if (effListStr) {
          entry.effective_affinity_list = parseCpuList(effListStr);
        }
      }

      irqs.push(entry);
    }

    // Sort by IRQ number
    irqs.sort((a, b) => parseInt(a.irq, 10) - parseInt(b.irq, 10));

    return okResponse({ irqs }, meta);
  } catch (err) {
    return errorResponse(meta, "IRQ_AFFINITY_ERROR", `Failed: ${err}`);
  }
}
