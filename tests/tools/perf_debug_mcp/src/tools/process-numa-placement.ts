/**
 * process_numa_placement(target_pid) — NUMA placement for a process.
 */
import type { ToolResponse, ProcessNumaPlacementData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readFile } from "fs/promises";
import { parseCpuList } from "../utils/proc-reader.js";
import { readFileHost, listEntriesHost, isLocalHost } from "../remote/host-utils.js";

export async function processNumaPlacement(params: {
  target_pid: number;
  host?: string;
}): Promise<ToolResponse<ProcessNumaPlacementData>> {
  const { target_pid } = params;
  const host = params.host;
  const meta = await buildMeta("fallback");

  try {
    let content: string;
    try {
      content = isLocalHost(host)
        ? await readFile(`/proc/${target_pid}/status`, "utf-8")
        : await readFileHost(host, `/proc/${target_pid}/status`);
    } catch {
      return errorResponse(meta, "PROCESS_NOT_FOUND", `PID ${target_pid} not found or not readable`);
    }

    let cpusAllowed: number[] = [];
    let memsAllowed: number[] = [];
    const notes: string[] = [];

    for (const line of content.split("\n")) {
      if (line.startsWith("Cpus_allowed_list:")) {
        cpusAllowed = parseCpuList(line.split(":")[1].trim());
      } else if (line.startsWith("Mems_allowed_list:")) {
        memsAllowed = parseCpuList(line.split(":")[1].trim());
      }
    }

    // Add notes about NUMA alignment
    if (memsAllowed.length === 1 && cpusAllowed.length > 0) {
      notes.push(`Process is confined to NUMA node ${memsAllowed[0]}`);
    } else if (memsAllowed.length > 1) {
      notes.push(`Process can allocate memory from NUMA nodes: ${memsAllowed.join(",")}`);
    }

    // Check if CPUs span multiple NUMA nodes
    try {
      const { readdir } = await import("fs/promises");
      const allNodeEntries = isLocalHost(host)
        ? await readdir("/sys/devices/system/node")
        : await listEntriesHost(host, "/sys/devices/system/node");
      const nodeList = allNodeEntries.filter((e: string) => /^node\d+$/.test(e));
      const cpuToNode = new Map<number, number>();
      for (const nodeDir of nodeList) {
        const nodeId = parseInt(nodeDir.replace("node", ""), 10);
        const cpulistContent = isLocalHost(host)
          ? await readFile(`/sys/devices/system/node/${nodeDir}/cpulist`, "utf-8").catch(() => "")
          : await readFileHost(host, `/sys/devices/system/node/${nodeDir}/cpulist`).catch(() => "");
        const nodeCpus = parseCpuList(cpulistContent.trim());
        for (const c of nodeCpus) cpuToNode.set(c, nodeId);
      }

      const usedNodes = new Set<number>();
      for (const c of cpusAllowed) {
        const node = cpuToNode.get(c);
        if (node !== undefined) usedNodes.add(node);
      }

      if (usedNodes.size > 1) {
        notes.push(
          `WARNING: Allowed CPUs span ${usedNodes.size} NUMA nodes (${[...usedNodes].sort().join(",")}) — ` +
          "cross-node memory access may add latency"
        );
      }
    } catch { /* ignore NUMA check errors */ }

    const data: ProcessNumaPlacementData = {
      pid: target_pid,
      cpus_allowed_list: cpusAllowed,
      mems_allowed_list: memsAllowed,
      notes,
    };

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "NUMA_PLACEMENT_ERROR", `Failed: ${err}`);
  }
}
