/**
 * numasched — Track cross-NUMA task migrations via bpftrace.
 *
 * Traces sched_migrate_task events and counts cross-NUMA-node migrations
 * per task.  Cross-NUMA migrations are expensive because the migrated task
 * loses cache locality and must access memory from a remote node.
 *
 * Complementary to process_numa_placement (which shows current placement)
 * and pcm_numa_traffic (which shows bandwidth).  This shows migration *events*.
 *
 * Source: Custom bpftrace script (sched:sched_migrate_task tracepoint).
 * Requires: bpftrace, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, NumaSchedData, NumaSchedMigration } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function numaSched(params: {
  host?: string;
  duration_sec?: number;
}): Promise<ToolResponse<NumaSchedData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;

  const meta = await buildMeta("fallback", duration * 1000);

  // Check for bpftrace
  const bpfCheck = await sshExecSafe(host, "command -v bpftrace 2>/dev/null", 5_000);
  if (!bpfCheck || !bpfCheck.trim()) {
    return errorResponse(
      meta,
      "BPFTRACE_MISSING",
      "bpftrace not found on target host",
      "Install bpftrace: apt-get install bpftrace",
    );
  }

  // Read NUMA topology to map CPUs to NUMA nodes
  // Then trace sched_migrate_task and count cross-NUMA migrations
  const script = [
    `'tracepoint:sched:sched_migrate_task`,
    `{`,
    `  @mig[comm, args->pid, args->orig_cpu, args->dest_cpu] = count();`,
    `}`,
    ``,
    `interval:s:${duration}`,
    `{`,
    `  exit();`,
    `}'`,
  ].join("\\n");

  // bpftrace inline script: trace migrations for duration seconds
  // We use a simpler approach: print orig_cpu -> dest_cpu, then post-process with NUMA map
  const cmd = [
    `NUMA_MAP=$(`,
    `for node_dir in /sys/devices/system/node/node*/cpulist; do`,
    `  NODE=$(basename $(dirname "$node_dir") | sed 's/node//');`,
    `  for cpu in $(cat "$node_dir" | tr ',' '\\n'); do`,
    `    if echo "$cpu" | grep -q '-'; then`,
    `      START=$(echo "$cpu" | cut -d- -f1);`,
    `      END=$(echo "$cpu" | cut -d- -f2);`,
    `      for c in $(seq $START $END); do echo "$c:$NODE"; done;`,
    `    else echo "$cpu:$NODE"; fi;`,
    `  done;`,
    `done)`,
    `&& bpftrace -e 'tracepoint:sched:sched_migrate_task { printf("%s %d %d %d\\n", comm, args->pid, args->orig_cpu, args->dest_cpu); } interval:s:${duration} { exit(); }' 2>/dev/null`,
    `| while read COMM PID ORIG DEST; do`,
    `  ORIG_NODE=$(echo "$NUMA_MAP" | grep "^$ORIG:" | cut -d: -f2);`,
    `  DEST_NODE=$(echo "$NUMA_MAP" | grep "^$DEST:" | cut -d: -f2);`,
    `  if [ -n "$ORIG_NODE" ] && [ -n "$DEST_NODE" ] && [ "$ORIG_NODE" != "$DEST_NODE" ]; then`,
    `    echo "CROSS $COMM $PID $ORIG_NODE $DEST_NODE";`,
    `  fi;`,
    `done | sort | uniq -c | sort -rn`,
  ].join(" ");

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);

  const migrations: NumaSchedMigration[] = [];

  if (output && output.trim()) {
    const lines = output.split("\n");
    for (const line of lines) {
      const trimmed = line.trim();
      // Format: "  5 CROSS taskname 1234 0 1"
      const match = trimmed.match(/^(\d+)\s+CROSS\s+(\S+)\s+(\d+)\s+(\d+)\s+(\d+)/);
      if (match) {
        migrations.push({
          count: parseInt(match[1], 10),
          task: match[2],
          pid: parseInt(match[3], 10),
          src_node: parseInt(match[4], 10),
          dst_node: parseInt(match[5], 10),
        });
      }
    }
  }

  const totalCrossNuma = migrations.reduce((s, m) => s + m.count, 0);

  const warnings: string[] = [];
  if (totalCrossNuma === 0) {
    warnings.push("No cross-NUMA migrations detected — NUMA affinity is stable");
  }
  if (totalCrossNuma > 100) {
    warnings.push(
      `${totalCrossNuma} cross-NUMA migrations in ${duration}s — consider pinning tasks with cpuset/taskset`,
    );
  }

  return okResponse<NumaSchedData>({
    duration_sec: duration,
    migrations,
    total_cross_numa: totalCrossNuma,
    warnings,
  }, meta);
}
