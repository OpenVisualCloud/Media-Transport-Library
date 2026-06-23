/**
 * nic_irq_queue_map(iface)
 * Explain NIC queue → IRQ mapping and CPU steering hints.
 */
import type { ToolResponse, NicIrqQueueMapData, NicQueueEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readFileTrimmed, parseCpuList, listEntries, isReadable } from "../utils/proc-reader.js";
import { readFileTrimmedHost, listEntriesHost, isReadableHost, isLocalHost } from "../remote/host-utils.js";
import { readProcInterrupts } from "../collectors/interrupts.js";

export async function nicIrqQueueMap(params: {
  iface: string;
  host?: string;
}): Promise<ToolResponse<NicIrqQueueMapData>> {
  const { iface } = params;
  const host = params.host;
  const meta = await buildMeta("fallback");

  // Helpers
  const readTr = (path: string) =>
    isLocalHost(host) ? readFileTrimmed(path) : readFileTrimmedHost(host, path);
  const listEnt = (path: string) =>
    isLocalHost(host) ? listEntries(path) : listEntriesHost(host, path);
  const isRd = (path: string) =>
    isLocalHost(host) ? isReadable(path) : isReadableHost(host, path);

  try {
    const sysBase = `/sys/class/net/${iface}`;
    if (!(await isRd(sysBase))) {
      return errorResponse(meta, "IFACE_NOT_FOUND", `Network interface '${iface}' not found in sysfs`);
    }

    const queues: NicQueueEntry[] = [];

    // Read MSI IRQ numbers from /sys/class/net/<iface>/device/msi_irqs/
    const msiIrqPath = `${sysBase}/device/msi_irqs`;
    const msiIrqs = await listEnt(msiIrqPath);

    // Read interrupt names to map IRQ numbers → names
    const interrupts = await readProcInterrupts(host);
    const irqNameMap = new Map<string, string>();
    for (const line of interrupts.lines) {
      irqNameMap.set(line.irq, line.device_name);
    }

    // Read RX queues
    const rxQueues = await listEnt(`${sysBase}/queues`);
    const rxQueueNames = rxQueues.filter((q) => q.startsWith("rx-")).sort();
    const txQueueNames = rxQueues.filter((q) => q.startsWith("tx-")).sort();

    // For each RX queue, try to find its IRQ
    for (const qName of rxQueueNames) {
      const entry: NicQueueEntry = {
        queue: qName,
        direction: "rx",
      };

      // Try to find IRQ for this queue via interrupt name matching
      // Many drivers name IRQs like "<iface>-rx-0"
      const qNum = qName.replace("rx-", "");
      for (const [irqNum, irqName] of irqNameMap.entries()) {
        if (irqName.includes(iface) &&
            (irqName.includes(`rx-${qNum}`) || irqName.includes(`TxRx-${qNum}`) ||
             irqName.includes(`-${qNum}`))) {
          entry.irq = irqNum;
          entry.irq_name = irqName;
          // Read IRQ affinity
          const affStr = await readTr(`/proc/irq/${irqNum}/smp_affinity_list`);
          if (affStr) entry.affinity_list = parseCpuList(affStr);
          break;
        }
      }

      queues.push(entry);
    }

    // For each TX queue
    for (const qName of txQueueNames) {
      const entry: NicQueueEntry = {
        queue: qName,
        direction: "tx",
      };

      const qNum = qName.replace("tx-", "");
      for (const [irqNum, irqName] of irqNameMap.entries()) {
        if (irqName.includes(iface) &&
            (irqName.includes(`tx-${qNum}`) || irqName.includes(`TxRx-${qNum}`))) {
          entry.irq = irqNum;
          entry.irq_name = irqName;
          const affStr = await readTr(`/proc/irq/${irqNum}/smp_affinity_list`);
          if (affStr) entry.affinity_list = parseCpuList(affStr);
          break;
        }
      }

      queues.push(entry);
    }

    // If we found MSI IRQs but couldn't match to queues, list them as combined
    if (queues.length === 0 && msiIrqs.length > 0) {
      for (const irqNum of msiIrqs.sort()) {
        const irqName = irqNameMap.get(irqNum);
        const entry: NicQueueEntry = {
          queue: `msi-${irqNum}`,
          direction: "combined",
          irq: irqNum,
          irq_name: irqName,
        };
        const affStr = await readTr(`/proc/irq/${irqNum}/smp_affinity_list`);
        if (affStr) entry.affinity_list = parseCpuList(affStr);
        queues.push(entry);
      }
    }

    return okResponse({ iface, queues }, meta);
  } catch (err) {
    return errorResponse(meta, "NIC_IRQ_MAP_ERROR", `Failed: ${err}`);
  }
}
