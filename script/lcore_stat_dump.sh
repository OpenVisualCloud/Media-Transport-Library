#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

TRACING_DIR="/sys/kernel/debug/tracing"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] CPU [SECONDS]

Collect the scheduler switch and IRQ vector trace events of one CPU with
ftrace. The script records for SECONDS seconds (default: 10), then writes
the trace of the CPU to lcore_status_cpu<CPU>.log in the current directory.
Run it as root. debugfs must be mounted on /sys/kernel/debug.

OPTIONS:
	-h		Show this help message

EXAMPLES:
	sudo ${script_name} 92		# Trace CPU 92 for 10 seconds
	sudo ${script_name} 92 30	# Trace CPU 92 for 30 seconds
EOF
}

main() {
	local opt cpu time_s out_file

	while getopts ":h" opt; do
		case $opt in
		h)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown option: -${OPTARG}"
			show_help
			exit 1
			;;
		esac
	done
	shift $((OPTIND - 1))
	if [ -z "${1:-}" ]; then
		log_error "Please specify the CPU"
		show_help
		exit 1
	fi
	cpu="$1"
	# default 10s
	time_s="${2:-10}"
	[ "$#" -le 2 ] || {
		log_error "Unexpected argument: $3"
		show_help
		exit 1
	}
	if [[ ! "$cpu" =~ ^[0-9]+$ ]]; then
		log_error "CPU must be a CPU number: ${cpu}"
		exit 1
	fi
	# The same forms that sleep accepts: 30, 1.5, 2m.
	if [[ ! "$time_s" =~ ^[0-9]+([.][0-9]+)?[smhd]?$ ]]; then
		log_error "SECONDS must be a time for sleep, such as 30 or 2m: ${time_s}"
		exit 1
	fi

	require_root || exit 1
	if [ ! -d "${TRACING_DIR}/per_cpu/cpu${cpu}" ]; then
		log_error "Cannot find the trace of CPU ${cpu} in ${TRACING_DIR}. Mount debugfs, or use a CPU that is online."
		exit 1
	fi

	out_file="lcore_status_cpu${cpu}.log"

	log_info "Collecting sched and irq events on cpu: ${cpu}, time ${time_s}s"

	# disable tracing
	echo 0 >"${TRACING_DIR}/tracing_on"
	# flush trace
	echo >"${TRACING_DIR}/trace"
	# enable sched
	echo 1 >"${TRACING_DIR}/events/sched/sched_switch/enable"
	# enable irq
	echo 1 >"${TRACING_DIR}/events/irq_vectors/enable"
	# enable
	echo 1 >"${TRACING_DIR}/tracing_on"

	# sleep
	sleep "${time_s}"

	# disable trace and save the log
	echo 0 >"${TRACING_DIR}/tracing_on"
	cat "${TRACING_DIR}/per_cpu/cpu${cpu}/trace" >"${out_file}"
	log_success "Collected to file: ${out_file}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
