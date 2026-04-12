/**
 * mtl_hugepage_usage — Report hugepage allocation on a target host.
 *
 * Reads /proc/meminfo for hugepage stats and optionally counts DPDK
 * hugepage files in the hugepage mount directory.
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - hugepage_dir: DPDK hugepage directory (default /dev/hugepages)
 */
import type { ToolResponse } from "../types.js";
import type { MtlHugepageUsageData, HugepageSizeInfo } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function mtlHugepageUsage(params: {
  host?: string;
  hugepage_dir?: string;
}): Promise<ToolResponse<MtlHugepageUsageData>> {
  const host = params.host ?? "localhost";
  const hugepageDir = params.hugepage_dir ?? "/dev/hugepages";

  const meta = await buildMeta("fallback");

  try {
    // Read /proc/meminfo for hugepage info
    const meminfo = await sshExecSafe(host, "grep -i huge /proc/meminfo 2>/dev/null");
    if (!meminfo) {
      return errorResponse(
        meta,
        "HUGEPAGE_READ_ERROR",
        "Cannot read /proc/meminfo",
        "Ensure the host is accessible",
      );
    }

    const sizes: HugepageSizeInfo[] = [];

    // Parse standard 2MB hugepage lines
    const totalMatch = meminfo.match(/HugePages_Total:\s*(\d+)/);
    const freeMatch = meminfo.match(/HugePages_Free:\s*(\d+)/);
    const rsvdMatch = meminfo.match(/HugePages_Rsvd:\s*(\d+)/);
    const surpMatch = meminfo.match(/HugePages_Surp:\s*(\d+)/);
    const sizeMatch = meminfo.match(/Hugepagesize:\s*(\d+)\s*kB/);

    if (totalMatch && sizeMatch) {
      const total = parseInt(totalMatch[1], 10);
      const free = freeMatch ? parseInt(freeMatch[1], 10) : 0;
      const reserved = rsvdMatch ? parseInt(rsvdMatch[1], 10) : 0;
      const surplus = surpMatch ? parseInt(surpMatch[1], 10) : 0;
      const pageSizeKb = parseInt(sizeMatch[1], 10);

      sizes.push({
        page_size_kb: pageSizeKb,
        total,
        free,
        reserved,
        surplus,
        in_use: total - free,
        total_bytes: total * pageSizeKb * 1024,
      });
    }

    // Also check /sys/kernel/mm/hugepages/ for all sizes (1GB pages, etc.)
    const sysHuge = await sshExecSafe(
      host,
      `for d in /sys/kernel/mm/hugepages/hugepages-*/; do
        [ -d "$d" ] || continue
        size_kb=$(basename "$d" | sed 's/hugepages-//;s/kB//')
        total=$(cat "$d/nr_hugepages" 2>/dev/null || echo 0)
        free=$(cat "$d/free_hugepages" 2>/dev/null || echo 0)
        rsvd=$(cat "$d/resv_hugepages" 2>/dev/null || echo 0)
        surp=$(cat "$d/surplus_hugepages" 2>/dev/null || echo 0)
        echo "$size_kb $total $free $rsvd $surp"
      done`,
    );

    if (sysHuge) {
      // Clear the sizes array (sysfs is more comprehensive) unless already populated
      const sysEntries: HugepageSizeInfo[] = [];
      for (const line of sysHuge.trim().split("\n")) {
        if (!line.trim()) continue;
        const parts = line.trim().split(/\s+/);
        if (parts.length >= 5) {
          const pageSizeKb = parseInt(parts[0], 10);
          const total = parseInt(parts[1], 10);
          const free = parseInt(parts[2], 10);
          const reserved = parseInt(parts[3], 10);
          const surplus = parseInt(parts[4], 10);

          if (total > 0 || free > 0) {
            sysEntries.push({
              page_size_kb: pageSizeKb,
              total,
              free,
              reserved,
              surplus,
              in_use: total - free,
              total_bytes: total * pageSizeKb * 1024,
            });
          }
        }
      }
      if (sysEntries.length > 0) {
        sizes.length = 0;
        sizes.push(...sysEntries);
      }
    }

    // Count DPDK hugepage files
    const fileCount = await sshExecSafe(
      host,
      `ls -1 ${hugepageDir}/ 2>/dev/null | wc -l`,
    );
    const dpdkHugeFiles = fileCount ? parseInt(fileCount.trim(), 10) || 0 : 0;

    return okResponse<MtlHugepageUsageData>(
      {
        sizes,
        dpdk_hugepage_files: dpdkHugeFiles,
        dpdk_hugepage_dir: hugepageDir,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "HUGEPAGE_ERROR",
      `Failed to collect hugepage stats: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
