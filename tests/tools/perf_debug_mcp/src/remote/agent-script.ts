/**
 * Embedded bash agent script for remote data collection.
 *
 * Deployed to /opt/.cpu-debug-agent/agent.sh on target hosts.
 * Handles batch /proc and /sys reads efficiently in a single SSH session.
 */

export const AGENT_VERSION = "1";

export const AGENT_INSTALL_DIR = "/opt/.cpu-debug-agent";
export const AGENT_PATH = `${AGENT_INSTALL_DIR}/agent.sh`;

export const AGENT_SCRIPT = `#!/bin/bash
# cpu-debug-agent v${AGENT_VERSION} — deployed by cpu-debug-mcp
set -uo pipefail

VERSION="${AGENT_VERSION}"
D="===CPU_DBG_AGENT==="

case "\${1:-}" in
  version)
    echo "\$VERSION"
    ;;

  tasks)
    # Scan /proc task table.
    # Usage: agent.sh tasks [--limit N] [--pids p1,p2,...] [--runnable]
    shift
    limit=100000
    pids=""
    runnable=0
    while [[ \$# -gt 0 ]]; do
      case "\$1" in
        --limit) limit="\$2"; shift 2 ;;
        --pids) pids="\$2"; shift 2 ;;
        --runnable) runnable=1; shift ;;
        *) shift ;;
      esac
    done

    count=0
    echo "\${D}TASKS"

    if [[ -n "\$pids" ]]; then
      IFS=',' read -ra pid_arr <<< "\$pids"
      for pid in "\${pid_arr[@]}"; do
        [[ \$count -ge \$limit ]] && break
        for tid_dir in /proc/"\$pid"/task/[0-9]*; do
          [[ \$count -ge \$limit ]] && break
          stat=\$(cat "\$tid_dir/stat" 2>/dev/null) || continue
          if [[ \$runnable -eq 1 ]]; then
            state=\$(echo "\$stat" | sed 's/.*) //' | cut -d' ' -f1)
            [[ "\$state" != "R" ]] && continue
          fi
          echo "\$stat"
          ((count++)) || true
        done
      done
    else
      for pid_dir in /proc/[0-9]*; do
        [[ \$count -ge \$limit ]] && break
        for tid_dir in "\$pid_dir"/task/[0-9]*; do
          [[ \$count -ge \$limit ]] && break
          stat=\$(cat "\$tid_dir/stat" 2>/dev/null) || continue
          if [[ \$runnable -eq 1 ]]; then
            state=\$(echo "\$stat" | sed 's/.*) //' | cut -d' ' -f1)
            [[ "\$state" != "R" ]] && continue
          fi
          echo "\$stat"
          ((count++)) || true
        done
      done
    fi
    echo "\${D}END"
    ;;

  affinities)
    # Batch affinity reads.
    # Usage: agent.sh affinities <tid1> <tid2> ...
    shift
    echo "\${D}AFFINITIES"
    for tid in "\$@"; do
      status=""
      if [[ -f "/proc/\$tid/status" ]]; then
        status="/proc/\$tid/status"
      elif [[ -f "/proc/\$tid/task/\$tid/status" ]]; then
        status="/proc/\$tid/task/\$tid/status"
      fi
      if [[ -n "\$status" ]]; then
        echo "\${D}TID \$tid"
        grep -E '^Cpus_allowed' "\$status" 2>/dev/null || true
        cgroup_line=\$(grep '^0::' "/proc/\$tid/cgroup" 2>/dev/null | head -1) || true
        if [[ -n "\$cgroup_line" ]]; then
          cg_path="\${cgroup_line#0::}"
          cpuset=\$(cat "/sys/fs/cgroup\${cg_path}/cpuset.cpus.effective" 2>/dev/null || \\
                   cat "/sys/fs/cgroup\${cg_path}/cpuset.cpus" 2>/dev/null || echo "")
          [[ -n "\$cpuset" ]] && echo "Cpuset_cpus: \$cpuset"
        fi
      fi
    done
    echo "\${D}END"
    ;;

  sysfs)
    # Full sysfs topology collection.
    echo "\${D}SYSFS"
    echo "online=\$(cat /sys/devices/system/cpu/online 2>/dev/null)"
    echo "offline=\$(cat /sys/devices/system/cpu/offline 2>/dev/null)"
    echo "isolated=\$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
    echo "cmdline=\$(cat /proc/cmdline 2>/dev/null)"

    echo "\${D}FREQ"
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
      n="\${cpu_dir##*/cpu}"
      cur=\$(cat "\$cpu_dir/cpufreq/scaling_cur_freq" 2>/dev/null || echo "")
      min=\$(cat "\$cpu_dir/cpufreq/scaling_min_freq" 2>/dev/null || echo "")
      max=\$(cat "\$cpu_dir/cpufreq/scaling_max_freq" 2>/dev/null || echo "")
      gov=\$(cat "\$cpu_dir/cpufreq/scaling_governor" 2>/dev/null || echo "")
      thr=\$(cat "\$cpu_dir/thermal_throttle/core_throttle_count" 2>/dev/null || echo "")
      echo "\$n \$cur \$min \$max \$gov \$thr"
    done

    echo "\${D}NUMA"
    for node_dir in /sys/devices/system/node/node[0-9]*; do
      n="\${node_dir##*/node}"
      cpus=\$(cat "\$node_dir/cpulist" 2>/dev/null || echo "")
      dist=\$(cat "\$node_dir/distance" 2>/dev/null || echo "")
      echo "\$n \$cpus \$dist"
    done
    echo "\${D}END"
    ;;

  *)
    echo "Usage: \$0 {version|tasks|affinities|sysfs}" >&2
    exit 1
    ;;
esac
`;
