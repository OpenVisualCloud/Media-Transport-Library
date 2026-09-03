#!/usr/bin/env bash

# Shared CPU-topology reasoning for the installer and the checks.
#
# With SMT enabled, reservedSystemCPUs has to cover *whole* physical cores. The
# static CPU Manager with full-pcpus-only hands out cores, not threads, so the
# free sibling of a reserved thread is stranded: strict-cpu-reservation keeps the
# workload off the reserved thread, and full-pcpus-only refuses to hand out the
# core because one of its threads is not free. Half-reserving four threads on a
# 128-core machine therefore costs four whole cores, silently.

# Expand a Linux CPU list ("0-1,60-61") into one CPU id per line.
lab_expand_cpu_spec() {
  local spec="${1:-}" part start end cpu
  local -a parts=()
  IFS=',' read -r -a parts <<<"$spec"
  for part in "${parts[@]}"; do
    [[ -n "$part" ]] || continue
    if [[ "$part" == *-* ]]; then
      start="${part%%-*}"; end="${part##*-}"
      for ((cpu = start; cpu <= end; cpu++)); do printf '%s\n' "$cpu"; done
    else
      printf '%s\n' "$part"
    fi
  done
}

# Print every core id that a reserved-CPU list covers only partly.
#   $1  reserved CPU list, e.g. "$LAB_RESERVED_CPUS"
#   $2  output of `lscpu -p=CPU,CORE` from the worker
lab_partially_reserved_cores() {
  local reserved="${1:-}" topology="${2:-}"
  printf '%s\n' "$topology" | grep -v '^#' | awk -F, -v reserved="$reserved" '
    BEGIN {
      count = split(reserved, items, ",")
      for (i = 1; i <= count; i++) {
        if (split(items[i], range, "-") == 2) {
          for (cpu = range[1]; cpu <= range[2]; cpu++) is_reserved[cpu] = 1
        } else if (items[i] != "") {
          is_reserved[items[i]] = 1
        }
      }
    }
    NF >= 2 { threads[$2]++; if ($1 in is_reserved) reserved_threads[$2]++ }
    END {
      for (core in threads)
        if (reserved_threads[core] > 0 && reserved_threads[core] < threads[core])
          print core
    }
  ' | sort -n
}
