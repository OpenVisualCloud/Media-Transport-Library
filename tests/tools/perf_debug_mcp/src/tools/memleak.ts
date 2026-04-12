/**
 * memleak — Trace outstanding memory allocations not yet freed.
 *
 * Tracks kernel (kmalloc/get_free_pages) or user-space (malloc/calloc)
 * allocations and reports those that haven't been freed after the trace
 * window, with stack traces showing where they were allocated.
 *
 * Useful for finding memory leaks that cause gradual memory pressure,
 * which eventually triggers OOM kills or heavy swapping (causing CPU stalls).
 *
 * Source: BCC `memleak`.
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, MemleakData, MemleakAllocation } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd } from "../utils/bcc-parser.js";

export async function memleak(params: {
  host?: string;
  duration_sec?: number;
  pid?: number;
  top_n?: number;
}): Promise<ToolResponse<MemleakData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const pid = params.pid;
  const topN = params.top_n ?? 10;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("memleak");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "MEMLEAK_MISSING",
      "memleak (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = [`-T`, String(topN)];
  if (pid !== undefined) flags.push(`-p ${pid}`);
  // interval=duration count=1 to get one snapshot after duration seconds
  const cmd = `${binary} ${flags.join(" ")} ${duration} 1 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "MEMLEAK_NO_OUTPUT",
      "memleak produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Verify target process is allocating memory.",
    );
  }

  try {
    const allocations: MemleakAllocation[] = [];
    const lines = output.split("\n");

    // Parse blocks like:
    //   12816 bytes in 18 allocations from stack
    //     0xffffffff...  func+0xNN [module]
    //     ...
    let current: MemleakAllocation | null = null;

    for (const line of lines) {
      const trimmed = line.trim();

      // Header line: "12816 bytes in 18 allocations from stack"
      const headerMatch = trimmed.match(/^(\d+)\s+bytes?\s+in\s+(\d+)\s+allocations?\s+from\s+stack/);
      if (headerMatch) {
        if (current) allocations.push(current);
        current = {
          bytes: parseInt(headerMatch[1], 10),
          allocations: parseInt(headerMatch[2], 10),
          stack: [],
        };
        continue;
      }

      // Stack frame line: "0xffffffff8305fc34  kmem_cache_alloc_lru+0x264 [kernel]"
      if (current && trimmed.match(/^0x[0-9a-f]+\s+/)) {
        const funcMatch = trimmed.match(/^0x[0-9a-f]+\s+(.+)/);
        if (funcMatch) {
          current.stack.push(funcMatch[1].trim());
        }
      }
    }
    if (current) allocations.push(current);

    // Sort by bytes descending
    allocations.sort((a, b) => b.bytes - a.bytes);

    const totalBytes = allocations.reduce((s, a) => s + a.bytes, 0);
    const totalAllocs = allocations.reduce((s, a) => s + a.allocations, 0);

    const warnings: string[] = [];
    if (totalBytes > 100 * 1024 * 1024) {
      warnings.push(
        `${(totalBytes / (1024 * 1024)).toFixed(1)} MB outstanding — significant memory leak`,
      );
    }

    return okResponse<MemleakData>({
      duration_sec: duration,
      pid_filter: pid,
      top_n: topN,
      outstanding: allocations,
      total_bytes: totalBytes,
      total_allocations: totalAllocs,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "MEMLEAK_PARSE_ERROR",
      `Failed to parse memleak output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
