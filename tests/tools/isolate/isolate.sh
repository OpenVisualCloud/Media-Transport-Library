#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Runs one command at nice -20 inside an exclusive cgroup v2 cpuset partition
# ("isolated") that exists only for the lifetime of that command, and moves IRQs
# and vmstat off its CPUs until exit. If the partition cannot be created, try
# mode warns once and runs the command unconfined. See README.md.
# The command sees the achieved level as MTL_CPU_ISOLATION=exclusive|none.
#
# Usage: isolate.sh [--] <command...>
#        isolate.sh --sweep   restore and remove what dead wrappers left behind
#
#   MTL_ISOLATE        off (default) | try | require
#   MTL_ISOLATE_PORTS  comma-separated PCI BDFs, the MTL primary port first; each
#                      port's NUMA node adds CPUs to the partition and to cpuset.mems

CG_ROOT=/sys/fs/cgroup
CG_PREFIX=mtl-isolate-
CORES=4
HOUSEKEEPING_CORES=2
STATE_PREFIX=/run/mtl-isolate-
LOCK=/run/mtl-isolate.lock
STAT_INTERVAL=/proc/sys/vm/stat_interval
DEFAULT_AFFINITY=/proc/irq/default_smp_affinity

mode=${MTL_ISOLATE:-off}
cg=$CG_ROOT/$CG_PREFIX$$
restore_file=$STATE_PREFIX$$.restore
lock_fd=""
why=""

log() { echo "isolate.sh: $*" >&2; }

put() { printf '%s\n' "$2" 2>/dev/null >"$1"; }

# Like put, but keeps the kernel's error in why.
put_or_why() {
	local err
	err=$({ printf '%s\n' "$2" >"$1"; } 2>&1) && return 0
	why="$1: ${err##*: }"
	return 1
}

port_node() {
	local node
	node=$(cat "/sys/bus/pci/devices/$1/numa_node" 2>/dev/null) || return 1
	# -1 means no NUMA affinity, which is node 0 on a single-node host.
	[ "$node" -lt 0 ] 2>/dev/null && [ ! -d /sys/devices/system/node/node1 ] && node=0
	[ "$node" -ge 0 ] 2>/dev/null && echo "$node"
}

# Hex affinity mask $1 (comma-separated 32-bit groups) with the CPUs of list $2 cleared.
mask_without() {
	local -a groups
	local list range lo hi c i
	IFS=, read -ra groups <<<"$1"
	IFS=, read -ra list <<<"$2"
	for range in "${list[@]}"; do
		lo=${range%-*} hi=${range#*-}
		for ((c = lo; c <= hi; c++)); do
			i=$((${#groups[@]} - 1 - c / 32))
			[ "$i" -ge 0 ] && groups[i]=$(printf '%08x' $((0x${groups[i]} & ~(1 << (c % 32)) & 0xffffffff)))
		done
	done
	local IFS=,
	echo "${groups[*]}"
}

# CORES highest physical cores of node $1 with their SMT siblings, skipping the first HOUSEKEEPING_CORES.
auto_cpus() {
	local -a cores
	mapfile -t cores < <(LC_ALL=C sort -t, -k1,1n -u /sys/devices/system/node/node"$1"/cpu[0-9]*/topology/thread_siblings_list)
	[ "${#cores[@]}" -ge $((HOUSEKEEPING_CORES + CORES)) ] || return 1
	local IFS=,
	echo "${cores[*]: -CORES}"
}

wait_empty() {
	local i
	for ((i = 0; i < 50; i++)); do
		grep -q '^populated 0' "$1/cgroup.events" && return 0
		sleep 0.1
	done
	return 1
}

# Terminates everything in cgroup $1, dissolves its partition and removes it.
teardown() {
	[ -d "$1" ] || return 0
	if ! grep -q '^populated 0' "$1/cgroup.events"; then
		xargs -r kill -TERM <"$1/cgroup.procs" 2>/dev/null
		wait_empty "$1" || { put "$1/cgroup.kill" 1 && wait_empty "$1"; }
	fi
	put "$1/cpuset.cpus.partition" member
	rmdir "$1" 2>/dev/null || log "WARNING: could not remove $1"
}

# "path original new" for each IRQ whose affinity keeps a CPU outside $1 once $1 is removed.
irq_moves() {
	grep -H . /proc/irq/*/smp_affinity_list 2>/dev/null | awk -F: -v cpus="$1" '
	function expand(list, set, n, p, r, i, c, m) {
		n = split(list, p, ",")
		for (i = 1; i <= n; i++) {
			if (split(p[i], r, "-") < 2) r[2] = r[1]
			for (c = r[1] + 0; c <= r[2] + 0; c++) set[c] = 1
			if (c > m) m = c
		}
		return m
	}
	BEGIN { expand(cpus, t) }
	{
		split("", s)
		n = expand($2, s)
		hit = 0
		out = ""
		start = -1
		for (c = 0; c <= n; c++) {
			if ((c in s) && (c in t)) hit = 1
			keep = (c in s) && !(c in t)
			if (keep && start < 0) start = c
			if (keep || start < 0) continue
			out = out (out == "" ? "" : ",") start (c - 1 > start ? "-" (c - 1) : "")
			start = -1
		}
		if (hit && out != "") print $1, $2, out
	}'
}

# Saves the originals to restore_file first, so every exit path can restore them.
isolate_housekeeping() {
	local moves old defmask path new total moved=0
	old=$(cat "$STAT_INTERVAL")
	moves=$(irq_moves "$1")
	# IRQs allocated later (VF MSI-X vectors at port open) start from the default mask.
	defmask=$(cat "$DEFAULT_AFFINITY")
	if ! { echo "$STAT_INTERVAL $old" && echo "$DEFAULT_AFFINITY $defmask" &&
		awk 'NF { print $1, $2 }' <<<"$moves"; } 2>/dev/null >"$restore_file"; then
		rm -f "$restore_file"
		log "WARNING: cannot write $restore_file; not moving IRQs or vmstat"
		return
	fi
	put "$STAT_INTERVAL" 120
	put "$DEFAULT_AFFINITY" "$(mask_without "$defmask" "$1")"
	while read -r path _ new; do
		[ -n "$path" ] && put "$path" "$new" && moved=$((moved + 1))
	done <<<"$moves"
	total=$(($(wc -l <"$restore_file") - 2))
	log "moved $moved of $total IRQs off $1, vm.stat_interval $old -> 120; originals in $restore_file"
}

restore_housekeeping() {
	local path value total restored=0
	[ -e "$1" ] || return 0
	total=$(wc -l <"$1")
	while read -r path value; do
		put "$path" "$value" && restored=$((restored + 1))
	done <"$1"
	rm -f "$1"
	log "restored $restored of $total saved settings (vm.stat_interval, IRQ affinities) from $1"
}

release_lock() {
	[ -z "$lock_fd" ] || exec {lock_fd}>&-
	lock_fd=""
}

take_lock() {
	command -v flock >/dev/null || { why="flock not found" && return 1; }
	{ exec {lock_fd}>>"$LOCK"; } 2>/dev/null || { why="cannot open $LOCK" && return 1; }
	flock -w 10 "$lock_fd" || { why="$LOCK held by another isolate.sh for 10 s" && return 1; }
}

# Every live wrapper with a cgroup or saved settings holds LOCK, so under it all of them are stale.
sweep_stale() {
	local dir file
	for dir in "$CG_ROOT/$CG_PREFIX"*; do
		[ ! -d "$dir" ] || { log "removing stale $dir" && teardown "$dir"; }
	done
	for file in "$STATE_PREFIX"*.restore; do
		restore_housekeeping "$file"
	done
}

# Takes LOCK and creates the exclusive partition cg, or sets why and fails.
setup() {
	local cpus="" mems="" node bdf state pick first_cpu="" first_node
	local -a ports
	grep -qw cpuset "$CG_ROOT/cgroup.subtree_control" 2>/dev/null ||
		{ why="no cgroup v2 cpuset controller at $CG_ROOT" && return 1; }
	take_lock || return 1
	sweep_stale
	IFS=, read -ra ports <<<"${MTL_ISOLATE_PORTS:-}"
	[ "${#ports[@]}" -gt 0 ] || { why="MTL_ISOLATE_PORTS is empty" && return 1; }
	for bdf in "${ports[@]}"; do
		node=$(port_node "$bdf") ||
			{ why="cannot resolve the NUMA node of port '$bdf' (MTL_ISOLATE_PORTS)" && return 1; }
		[[ ",$mems," == *",$node,"* ]] || mems=${mems:+$mems,}$node
	done
	for node in ${mems//,/ }; do
		pick=$(auto_cpus "$node") || { why="fewer than $((HOUSEKEEPING_CORES + CORES)) cores on NUMA node $node" && return 1; }
		cpus=${cpus:+$cpus,}$pick
		if [ -z "$first_cpu" ] || [ "${pick%%[,-]*}" -lt "$first_cpu" ]; then
			first_cpu=${pick%%[,-]*} first_node=$node
		fi
	done
	# EAL's main lcore is the partition's first CPU, and mtl_init binds the main thread to port P's node.
	[ "$first_node" = "${mems%%,*}" ] ||
		{ why="port ${ports[0]} is on node ${mems%%,*}, but the partition's first CPU $first_cpu is on node $first_node" && return 1; }
	why=$(mkdir "$cg" 2>&1) || return 1
	put_or_why "$cg/cpuset.cpus" "$cpus" || return 1
	put_or_why "$cg/cpuset.mems" "$mems" || return 1
	put "$cg/cgroup.pressure" 0
	put "$cg/cpuset.cpus.exclusive" "$cpus" # absent before Linux 6.7
	put_or_why "$cg/cpuset.cpus.partition" isolated || return 1
	state=$(cat "$cg/cpuset.cpus.partition")
	why="partition $state"
	[ "$state" = isolated ]
}

cleanup() {
	teardown "$cg"
	restore_housekeeping "$restore_file"
}

# nice, not SCHED_FIFO/RR: busy-polling lcores would hit RT throttling (~50 ms/s) or starve per-CPU kthreads.
prio=(nice -n -20)

unconfined() {
	release_lock
	if [ "$mode" = require ]; then
		log "ERROR: exclusive CPU isolation unavailable ($why); MTL_ISOLATE=require, not running the command"
		exit 1
	fi
	log "WARNING: exclusive CPU isolation unavailable ($why); running unconfined${prio[*]:+ at nice -20}"
	export MTL_CPU_ISOLATION=none
	exec "${prio[@]}" "$@"
}

# Started detached by the wrapper $1, so a kill of its session or process group spares it.
watch() {
	cg=$CG_ROOT/$CG_PREFIX$1
	restore_file=$STATE_PREFIX$1.restore
	# PIPE too: its stderr may be a pipe from a session that is already gone.
	trap '' HUP INT TERM PIPE
	while kill -0 "$1" 2>/dev/null; do sleep 0.2; done
	[ ! -d "$cg" ] || log "wrapper $1 died, removing $cg"
	cleanup
	exit 0
}

sweep() {
	[ "$(id -u)" -eq 0 ] || { log "ERROR: --sweep needs root" && exit 1; }
	take_lock || { log "ERROR: $why" && exit 1; }
	sweep_stale
	! compgen -G "$CG_ROOT/$CG_PREFIX*" >/dev/null || { log "ERROR: could not remove every $CG_ROOT/$CG_PREFIX*" && exit 1; }
	exit 0
}

case "${1:-}" in
--watch) watch "$2" ;;
--sweep) sweep ;;
--) shift ;;
esac
[ "$#" -gt 0 ] || { echo "usage: isolate.sh [--] <command...> | --sweep" >&2 && exit 2; }
case "$mode" in
off) MTL_CPU_ISOLATION=none exec "$@" ;;
try | require) ;;
*) log "invalid MTL_ISOLATE=$mode (off|try|require)" && exit 2 ;;
esac
if [ "$(id -u)" -ne 0 ]; then
	why="not root"
	prio=()
	unconfined "$@"
fi

trap cleanup EXIT
trap 'exit 129' HUP
trap 'cleanup; trap - INT EXIT; kill -INT $$' INT
trap 'exit 143' TERM
if ! setup; then
	cleanup
	unconfined "$@"
fi
log "exclusive CPU isolation of $(cat "$cg/cpuset.cpus.effective") mems $(cat "$cg/cpuset.mems") at nice -20 in $cg"
export MTL_CPU_ISOLATION=exclusive

# Cleans up if this wrapper is SIGKILLed; inherits LOCK and holds it until then.
setsid -f bash "${BASH_SOURCE[0]}" --watch $$ </dev/null ||
	log "WARNING: no watcher; a SIGKILL of this wrapper leaves cleanup to the next one"
cpus=$(cat "$cg/cpuset.cpus.effective")
isolate_housekeeping "$cpus"
# A subshell that execs: non-interactive bash starts a plain `cmd &` with SIGINT and SIGQUIT ignored.
(
	release_lock
	if ! printf '%s\n' "$BASHPID" 2>/dev/null >"$cg/cgroup.procs"; then
		why="could not move into $cg"
		cleanup
		unconfined "$@"
	fi
	# No load balancing here, so every unpinned thread stays where it starts: on EAL's main lcore, which runs no scheduler.
	{ taskset -pc "${cpus%%[,-]*}" "$BASHPID" && taskset -pc "$cpus" "$BASHPID"; } >/dev/null ||
		log "WARNING: could not start on CPU ${cpus%%[,-]*}"
	exec "${prio[@]}" "$@"
) &
wait $!
