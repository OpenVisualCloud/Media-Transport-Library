#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Records what runs on MTL lcores besides the MTL scheduler threads: sched_switch, hard and soft
# irqs and the irq vectors (timer, IPIs, TLB shootdowns), in a private ftrace instance, then sums
# it up per CPU with lcore_stat_report.py.

set -e -o pipefail

usage() {
	cat <<'EOF'
Usage: sudo ./script/lcore_stat_dump.sh <cpus|auto> [time_s] [--out DIR] [--clock CLOCK]
       ./script/lcore_stat_dump.sh <cpus|auto> --isolation
  cpus           one CPU (92), a list (65-72,80), or auto: the CPUs the running mtl_sch_*
                 threads are pinned to
  time_s         whole seconds, default 10
  --out DIR      all output in DIR; without it lcore_status_cpu<N>.log goes to the current
                 directory and the rest to a new lcore_stat_<time> directory there
  --clock CLOCK  ftrace trace_clock: mono (default), mono_raw, boot, tai, local, global or
                 perf. meta.env holds the offset of mono, mono_raw, boot and tai to
                 CLOCK_REALTIME, for joins with other captures
  --isolation    only report how isolated the CPUs are (isolcpus, nohz_full, rcu_nocbs,
                 cpuset partitions, IRQ affinity, numa balancing, SMT siblings); reads only
Writes lcore_status_cpu<N>.log (the raw trace of each CPU), timeline.csv, summary.txt,
meta.env and mtl_threads.txt.
EOF
}

cpu_spec=""
time_s=10
out_dir=""
clock=mono
isolation=0
time_set=0
while [ $# -gt 0 ]; do
	case $1 in
	--out | --clock)
		[ $# -ge 2 ] || { echo "$1 needs a value" && exit 2; }
		if [ "$1" = --out ]; then out_dir=$2; else clock=$2; fi
		shift
		;;
	--isolation) isolation=1 ;;
	-h | --help) usage && exit 0 ;;
	*)
		if [ -z "$cpu_spec" ]; then
			cpu_spec=$1
		elif [ "$time_set" = 0 ] && [[ $1 =~ ^[0-9]+$ ]]; then
			time_s=$1
			time_set=1
		else
			usage && exit 2
		fi
		;;
	esac
	shift
done
[ -n "$cpu_spec" ] || { usage && exit 0; }
case $clock in mono | mono_raw | boot | tai | local | global | perf) ;; *) usage && exit 2 ;; esac

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# the mtl_sch_* threads, as "tid comm cpus"
mtl_threads=$(for t in /proc/[0-9]*/task/[0-9]*; do
	{ read -r c <"$t/comm"; } 2>/dev/null || continue
	[[ $c == mtl_sch_* ]] || continue
	echo "${t##*/} $c $(awk '/^Cpus_allowed_list/ {print $2}' "$t/status" 2>/dev/null)"
done)
if [ "$cpu_spec" = auto ]; then
	# lcore mode pins each thread to one CPU; thread mode leaves them floating
	cpu_spec=$(awk '$3 ~ /^[0-9]+$/ {print $3}' <<<"$mtl_threads" | paste -sd,)
	[ -n "$cpu_spec" ] || { echo "auto: no mtl_sch_* thread is pinned to one CPU, give the CPUs" && exit 1; }
fi
[[ $cpu_spec =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] || { usage && exit 2; }
if [ "$isolation" = 1 ]; then
	exec python3 -I "$script_dir/lcore_stat_report.py" --isolation "$cpu_spec"
fi

# 65-72,80 -> 65 66 ... 72 80, each once
mapfile -t cpus < <(
	IFS=, read -ra parts <<<"$cpu_spec"
	for p in "${parts[@]}"; do seq "${p%-*}" "${p#*-}"; done | sort -nu
)

log_dir=${out_dir:-.}
out_dir=${out_dir:-lcore_stat_$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$out_dir" "$log_dir"
log_dir=$(cd "$log_dir" && pwd)
echo "$mtl_threads" >"$out_dir/mtl_threads.txt"

# tracing_cpumask: 32-bit hex words, the highest first
words=()
for c in "${cpus[@]}"; do words[c / 32]=$((${words[c / 32]:-0} | 1 << (c % 32))); done
mask=
for ((w = ($(nproc --all) - 1) / 32; w >= 0; w--)); do
	mask+=$(printf '%08x' "${words[w]:-0}")
	[ "$w" -eq 0 ] || mask+=,
done

tracefs=/sys/kernel/tracing
[ -d "$tracefs/instances" ] || tracefs=/sys/kernel/debug/tracing
inst=$tracefs/instances/mtl_lcore_$$
created=0

# removing the instance also disables its events
stop() {
	if [ "$created" = 1 ]; then
		created=0
		rmdir "$inst"
	fi
}
trap stop EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir "$inst"
created=1
echo 0 >"$inst/tracing_on"
echo "$clock" >"$inst/trace_clock"
echo "$mask" >"$inst/tracing_cpumask"
# nothing reads while it records, so no reader wakeups on the lcores: the traced CPUs get a
# buffer for the whole run (a busy lcore writes ~100 KB/s), the others the minimum
echo 64 >"$inst/buffer_size_kb"
for c in "${cpus[@]}"; do
	echo $((time_s < 16 ? 4096 : time_s * 256)) >"$inst/per_cpu/cpu$c/buffer_size_kb"
done
echo 1 >"$inst/events/sched/sched_switch/enable"
for e in irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry \
	irq/softirq_exit ipi/ipi_entry ipi/ipi_exit tlb/tlb_flush \
	"$inst"/events/irq_vectors/*_entry "$inst"/events/irq_vectors/*_exit; do
	e=${e#"$inst/events/"}
	if [ -e "$inst/events/$e/enable" ]; then echo 1 >"$inst/events/$e/enable" 2>/dev/null || true; fi
done

# the clock's offset to CLOCK_REALTIME, for joins with captures
offset=$(python3 -I -c 'import sys, time
c = {"mono": time.CLOCK_MONOTONIC, "mono_raw": time.CLOCK_MONOTONIC_RAW,
     "boot": time.CLOCK_BOOTTIME, "tai": time.CLOCK_TAI}.get(sys.argv[1])
print(time.clock_gettime_ns(time.CLOCK_REALTIME) - time.clock_gettime_ns(c) if c else "")' "$clock")
cat >"$out_dir/meta.env" <<EOF
cpus="${cpus[*]}"
time_s=$time_s
clock=$clock
realtime_minus_clock_ns=$offset
log_dir=$log_dir
EOF

echo "Collecting sched and irq events on cpu: ${cpus[*]}, time ${time_s}s"
echo 1 >"$inst/tracing_on"
sleep "$time_s"
echo 0 >"$inst/tracing_on"

for c in "${cpus[@]}"; do
	cat "$inst/per_cpu/cpu$c/trace" >"$log_dir/lcore_status_cpu$c.log"
	lost=$(awk '/^overrun:/ {print $2}' "$inst/per_cpu/cpu$c/stats")
	[ "$lost" = 0 ] || echo "cpu $c: the buffer overran, $lost events lost"
done
stop
echo "Collected to dir: $out_dir"
[ "$log_dir" -ef "$out_dir" ] || echo "Per-CPU logs in: $log_dir"
python3 -I "$script_dir/lcore_stat_report.py" "$out_dir" | tee "$out_dir/summary.txt"
