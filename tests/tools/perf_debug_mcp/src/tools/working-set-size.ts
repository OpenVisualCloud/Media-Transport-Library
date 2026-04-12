/**
 * working_set_size — Estimate process Working Set Size via page table bits.
 *
 * Uses the Linux clear_refs/smaps mechanism: clears the Referenced bit on all
 * pages, waits for a measurement window, then reads back which pages were
 * accessed.  The sum of Referenced pages is the WSS for that window.
 *
 * Critical for capacity planning: if WSS > LLC size, the process will suffer
 * cache misses.  If WSS > NUMA node memory, cross-socket access will hurt.
 *
 * WARNING: Writing to clear_refs briefly impacts TLB performance for the
 * target process.  Use with care on latency-sensitive production workloads.
 *
 * Source: /proc/<pid>/clear_refs + /proc/<pid>/smaps (Brendan Gregg wss.pl).
 * Requires: root (to read/write /proc/<pid>/ for other processes).
 */
import type { ToolResponse, WorkingSetSizeData, WssRegion } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function workingSetSize(params: {
  host?: string;
  pid: number;
  window_sec?: number;
}): Promise<ToolResponse<WorkingSetSizeData>> {
  const host = params.host ?? "localhost";
  const pid = params.pid;
  const windowSec = params.window_sec ?? 1;
  const meta = await buildMeta("fallback", windowSec * 1000);

  // Validate PID exists
  const commResult = await sshExecSafe(host, `cat /proc/${pid}/comm 2>/dev/null`);
  if (!commResult || !commResult.trim()) {
    return errorResponse(meta, "WSS_PID_NOT_FOUND", `Process ${pid} does not exist`);
  }
  const comm = commResult.trim();

  // Get RSS before clearing (from smaps_rollup for efficiency)
  const rssResult = await sshExecSafe(host, `cat /proc/${pid}/smaps_rollup 2>/dev/null`);
  const rssKb = extractSmapsField(rssResult ?? "", "Rss");

  // Clear referenced bits, wait, then read smaps Referenced
  // We use a single command to minimize SSH round trips
  const cmd = [
    // Step 1: Clear referenced bits (write 1 to clear_refs)
    `echo 1 > /proc/${pid}/clear_refs`,
    // Step 2: Wait for measurement window
    `sleep ${windowSec}`,
    // Step 3: Read smaps and extract Referenced + region info
    `awk '` +
      `/^[0-9a-f]/ { region=$0 }` +
      `/^Referenced:/ { ref+=$2; if($2>0) print region, "REF=" $2 }` +
      `END { print "TOTAL_REF=" ref }` +
    `' /proc/${pid}/smaps 2>/dev/null`,
  ].join(" && ");

  const timeoutMs = (windowSec + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output) {
    return errorResponse(
      meta,
      "WSS_MEASURE_FAILED",
      `Failed to measure WSS for PID ${pid}`,
      "Ensure root access and process is still running. /proc/<pid>/clear_refs requires CAP_SYS_PTRACE.",
    );
  }

  try {
    const lines = output.trim().split("\n");
    let totalRefKb = 0;
    const regions: WssRegion[] = [];

    for (const line of lines) {
      const totalMatch = line.match(/TOTAL_REF=(\d+)/);
      if (totalMatch) {
        totalRefKb = parseInt(totalMatch[1], 10);
        continue;
      }

      // Region lines: "7f1234-7f5678 r-xp 00000000 fc:00 1234  /lib/x86_64-linux-gnu/libc.so.6 REF=1234"
      const refMatch = line.match(/REF=(\d+)/);
      if (refMatch) {
        const refKb = parseInt(refMatch[1], 10);
        // Extract region name (last field before REF=)
        const parts = line.split(/\s+/);
        const refIdx = parts.findIndex(p => p.startsWith("REF="));
        const name = refIdx > 5 ? parts.slice(5, refIdx).join(" ") || parts[0] : parts[0];
        regions.push({ name: name || "anonymous", wss_kb: refKb, rss_kb: 0 });
      }
    }

    const pageSize = 4; // 4 KB default
    const wssPages = Math.round(totalRefKb / pageSize);
    const wssMb = totalRefKb / 1024;
    const rssMb = rssKb / 1024;

    // Sort regions by WSS descending
    regions.sort((a, b) => b.wss_kb - a.wss_kb);
    // Keep top 20 regions
    const topRegions = regions.slice(0, 20);

    const warnings: string[] = [];
    const wssToRss = rssKb > 0 ? totalRefKb / rssKb : 0;

    if (wssToRss < 0.1 && rssKb > 100_000) {
      warnings.push(`WSS is only ${(wssToRss * 100).toFixed(1)}% of RSS — process has a lot of cold memory (candidate for memory compaction)`);
    }
    if (wssMb > 30) {
      warnings.push(`WSS ${wssMb.toFixed(1)}MB may exceed L3 cache — expect LLC misses`);
    }
    if (wssMb > 1000) {
      warnings.push(`WSS ${wssMb.toFixed(0)}MB is very large — verify NUMA-local memory allocation`);
    }
    if (windowSec < 1) {
      warnings.push("Short measurement window may underestimate WSS — consider window_sec >= 1");
    }

    return okResponse<WorkingSetSizeData>({
      pid,
      comm,
      window_sec: windowSec,
      wss_pages: wssPages,
      wss_kb: totalRefKb,
      wss_mb: round2(wssMb),
      rss_kb: rssKb,
      rss_mb: round2(rssMb),
      wss_to_rss_ratio: round2(wssToRss),
      page_size_kb: pageSize,
      regions: topRegions,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(meta, "WSS_PARSE_ERROR", `Failed to parse WSS data: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function extractSmapsField(text: string, field: string): number {
  const m = text.match(new RegExp(`${field}:\\s+(\\d+)`));
  return m ? parseInt(m[1], 10) : 0;
}

function round2(n: number): number { return Math.round(n * 100) / 100; }
