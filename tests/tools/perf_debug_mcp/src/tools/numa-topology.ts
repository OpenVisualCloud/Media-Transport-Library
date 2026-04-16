/**
 * numa_topology() — NUMA nodes, CPUs per node, distance matrix.
 */
import type { ToolResponse, NumaTopologyData, NumaNode } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getNumaNodeCpus, getNumaDistances } from "../collectors/sysfs.js";
import { listEntries, readFileInt } from "../utils/proc-reader.js";
import { listEntriesHost, readFileHost, isLocalHost } from "../remote/host-utils.js";

export async function numaTopology(params?: {
  host?: string;
}): Promise<ToolResponse<NumaTopologyData>> {
  const host = params?.host;
  const meta = await buildMeta("fallback");

  try {
    const nodeEntries = isLocalHost(host)
      ? await listEntries("/sys/devices/system/node")
      : await listEntriesHost(host, "/sys/devices/system/node");
    const nodeNums = nodeEntries
      .filter((e) => /^node\d+$/.test(e))
      .map((e) => parseInt(e.replace("node", ""), 10))
      .sort((a, b) => a - b);

    if (nodeNums.length === 0) {
      return errorResponse(meta, "NUMA_NOT_AVAILABLE", "No NUMA topology found in sysfs");
    }

    const nodes: NumaNode[] = [];
    for (const nodeId of nodeNums) {
      const cpus = await getNumaNodeCpus(nodeId, host);
      const node: NumaNode = { node: nodeId, cpus };

      // Try to read memory info
      try {
        const { readFile } = await import("fs/promises");
        const meminfo = isLocalHost(host)
          ? await readFile(`/sys/devices/system/node/node${nodeId}/meminfo`, "utf-8")
          : await readFileHost(host, `/sys/devices/system/node/node${nodeId}/meminfo`);
        const totalMatch = meminfo.match(/MemTotal:\s+(\d+)\s+kB/);
        if (totalMatch) {
          node.memory_mb = Math.round(parseInt(totalMatch[1], 10) / 1024);
        }
      } catch { /* ignore */ }

      nodes.push(node);
    }

    const distances = await getNumaDistances(host);

    const data: NumaTopologyData = { nodes };
    if (distances) data.distances = distances;

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "NUMA_TOPOLOGY_ERROR", `Failed: ${err}`);
  }
}
