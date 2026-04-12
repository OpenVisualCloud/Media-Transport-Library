/**
 * mtl_instance_processes — Discover MTL processes, their threads, CPU affinity,
 * and per-thread CPU usage on a target host.
 *
 * Scans for processes matching MTL naming conventions:
 *   - MtlManager
 *   - mtl_* (MTL applications)
 *   - Processes with threads named mtl_sch_*, dpdk-worker*, dpdk-telemet*
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - process_regex: custom regex to match process names (default covers MTL patterns)
 */
import type { ToolResponse } from "../types.js";
import type {
  MtlInstanceProcessesData,
  MtlProcessInfo,
  MtlThreadInfo,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function mtlInstanceProcesses(params: {
  host?: string;
  process_regex?: string;
}): Promise<ToolResponse<MtlInstanceProcessesData>> {
  const host = params.host ?? "localhost";
  // Default: find MtlManager + any process that has mtl_ or dpdk- threads
  const processRegex = params.process_regex ?? "MtlManager|mtl_|mxl_|dpdk-";

  const meta = await buildMeta("fallback");

  try {
    // Step 1: Find candidate PIDs by scanning thread names AND cmdline args.
    // Uses fast bulk ps commands (~0.3s) instead of /proc iteration (~18s+).
    //
    //   Pass A: `ps -eLo pid,comm` — thread names matching MTL/DPDK/MXL patterns
    //   Pass B: `ps -eo pid,args` — cmdline args matching MTL/DPDK/MXL flags
    //   Pass C: DPDK telemetry socket ownership via lsof
    const cmdlinePatterns = "--mxl-domain|--flow-json|--p_sip|--dma_dev|--file-prefix|--socket-mem|dpdk_telemetry|imtl|libmtl|kahawai|st2110";
    const scanScript = `
# Pass A: fast thread-name scan via ps (single fork, ~0.15s on 4700+ threads)
PIDS_A=$(ps -eLo pid,comm --no-headers 2>/dev/null | grep -E '${processRegex}' | awk '{print $1}' | sort -un)

# Pass B: fast cmdline scan via ps (catches MXL-only processes with no MTL threads)
PIDS_B=$(ps -eo pid,args --no-headers 2>/dev/null | grep -E -- '${cmdlinePatterns}' | grep -v grep | awk '{print $1}' | sort -un)

# Pass C: DPDK telemetry sockets
PIDS_C=""
for sock in /var/run/dpdk/*/dpdk_telemetry.* /run/dpdk/*/dpdk_telemetry.*; do
  [ -S "$sock" ] 2>/dev/null || continue
  spid=$(lsof -t "$sock" 2>/dev/null | head -1)
  [ -z "$spid" ] && continue
  PIDS_C="$PIDS_C $spid"
done

# Output unique PIDs
{ echo "$PIDS_A"; echo "$PIDS_B"; for p in $PIDS_C; do echo "$p"; done; } | grep -v "^$" | sort -un
`;

    const pidOutput = await sshExecSafe(host, scanScript, 30_000);
    if (!pidOutput || !pidOutput.trim()) {
      return okResponse<MtlInstanceProcessesData>(
        {
          processes: [],
          process_count: 0,
          manager: { pid: null, running: false },
        },
        meta,
      );
    }

    const pids = pidOutput.trim().split("\n").map((p) => parseInt(p.trim(), 10)).filter((p) => !isNaN(p));

    // Step 2: For each PID, collect process + thread details
    const processes: MtlProcessInfo[] = [];
    let managerPid: number | null = null;
    let managerRunning = false;

    for (const pid of pids) {
      // Get main process info
      const infoScript = `
comm=$(cat /proc/${pid}/comm 2>/dev/null)
cmdline=$(cat /proc/${pid}/cmdline 2>/dev/null | tr '\\0' ' ')
status_lines=$(cat /proc/${pid}/status 2>/dev/null)
stat_line=$(cat /proc/${pid}/stat 2>/dev/null)

echo "COMM:$comm"
echo "CMDLINE:$cmdline"

# Extract RSS and VSize from status
echo "$status_lines" | grep -E '^(VmRSS|VmSize|Cpus_allowed_list):' | while read line; do
  echo "STATUS:$line"
done

# Extract start time from stat (field 22)
start_time=$(echo "$stat_line" | awk '{print $22}')
echo "START_TIME:$start_time"

# Enumerate threads
for tid_dir in /proc/${pid}/task/[0-9]*/; do
  tid=\${tid_dir#/proc/${pid}/task/}
  tid=\${tid%/}
  tcomm=$(cat /proc/${pid}/task/$tid/comm 2>/dev/null)
  tstatus=$(cat /proc/${pid}/task/$tid/status 2>/dev/null)
  tstat=$(cat /proc/${pid}/task/$tid/stat 2>/dev/null)

  cpus_allowed=$(echo "$tstatus" | grep 'Cpus_allowed_list:' | awk '{print $2}')
  vol_cs=$(echo "$tstatus" | grep 'voluntary_ctxt_switches:' | awk '{print $2}')
  nonvol_cs=$(echo "$tstatus" | grep 'nonvoluntary_ctxt_switches:' | awk '{print $2}')
  state=$(echo "$tstatus" | grep '^State:' | awk '{print $2}')

  # utime and stime are fields 14 and 15 in /proc/pid/task/tid/stat
  utime=$(echo "$tstat" | awk '{print $14}')
  stime=$(echo "$tstat" | awk '{print $15}')

  echo "THREAD:$tid|$tcomm|$cpus_allowed|$utime|$stime|$vol_cs|$nonvol_cs|$state"
done
`;
      const infoOutput = await sshExecSafe(host, infoScript, 10_000);
      if (!infoOutput) continue;

      let comm = "";
      let cmdline = "";
      let rssKb = 0;
      let vsizeKb = 0;
      let cpusAllowedList = "";
      const threads: MtlThreadInfo[] = [];

      for (const line of infoOutput.split("\n")) {
        if (line.startsWith("COMM:")) {
          comm = line.slice(5).trim();
        } else if (line.startsWith("CMDLINE:")) {
          cmdline = line.slice(8).trim();
        } else if (line.startsWith("STATUS:VmRSS:")) {
          rssKb = parseInt(line.replace(/[^0-9]/g, ""), 10) || 0;
        } else if (line.startsWith("STATUS:VmSize:")) {
          vsizeKb = parseInt(line.replace(/[^0-9]/g, ""), 10) || 0;
        } else if (line.startsWith("STATUS:Cpus_allowed_list:")) {
          cpusAllowedList = line.split(":").pop()?.trim() ?? "";
        } else if (line.startsWith("THREAD:")) {
          const parts = line.slice(7).split("|");
          if (parts.length >= 8) {
            const cpuList = parseCpuListSimple(parts[2]);
            threads.push({
              tid: parseInt(parts[0], 10) || 0,
              name: parts[1] || "",
              cpu_affinity: cpuList,
              utime_ticks: parseInt(parts[3], 10) || 0,
              stime_ticks: parseInt(parts[4], 10) || 0,
              voluntary_ctx_switches: parseInt(parts[5], 10) || 0,
              nonvoluntary_ctx_switches: parseInt(parts[6], 10) || 0,
              state: parts[7] || "?",
            });
          }
        }
      }

      // Check if this is MtlManager
      if (comm === "MtlManager") {
        managerPid = pid;
        managerRunning = true;
      }

      processes.push({
        pid,
        comm,
        cmdline,
        cpu_affinity: parseCpuListSimple(cpusAllowedList),
        threads: threads.sort((a, b) => a.tid - b.tid),
        thread_count: threads.length,
        rss_kb: rssKb,
        vsize_kb: vsizeKb,
        start_time: null,  // Would need to convert from jiffies; not critical
      });
    }

    return okResponse<MtlInstanceProcessesData>(
      {
        processes: processes.sort((a, b) => a.pid - b.pid),
        process_count: processes.length,
        manager: { pid: managerPid, running: managerRunning },
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_PROCESS_ERROR",
      `Failed to scan MTL processes: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure SSH access to the target host is configured",
    );
  }
}

/**
 * Simple CPU list parser (subset of the full parser in proc-reader.ts).
 * Handles "0-3,5,7-9" format.
 */
function parseCpuListSimple(list: string): number[] {
  const cpus: number[] = [];
  const trimmed = (list ?? "").trim();
  if (!trimmed) return cpus;
  for (const part of trimmed.split(",")) {
    const p = part.trim();
    if (!p) continue;
    const range = p.split("-");
    if (range.length === 2) {
      const start = parseInt(range[0], 10);
      const end = parseInt(range[1], 10);
      if (!isNaN(start) && !isNaN(end)) {
        for (let i = start; i <= end; i++) cpus.push(i);
      }
    } else {
      const val = parseInt(p, 10);
      if (!isNaN(val)) cpus.push(val);
    }
  }
  return cpus.sort((a, b) => a - b);
}
