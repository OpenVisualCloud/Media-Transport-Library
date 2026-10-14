#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] INTERFACE

Delete all ethtool n-tuple (RX classification) filters of a network
interface. The script reads the filter list with "ethtool -n" and deletes
each filter with "ethtool -N INTERFACE delete RULE". Run it as root.

REQUIRED PACKAGES (Debian/Ubuntu):
	ethtool

OPTIONS:
	-h		Show this help message

EXAMPLES:
	sudo ${script_name} enp175s0f0	# Delete all filters of enp175s0f0
EOF
}

main() {
	local opt if_name filter_list rule rules=()

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
	if [ "$#" -lt 1 ]; then
		log_error "Please specify the network interface"
		show_help
		exit 1
	fi
	[ "$#" -eq 1 ] || {
		log_error "Unexpected argument: $2"
		show_help
		exit 1
	}
	if_name="$1"

	require_commands ethtool:ethtool || exit 1
	require_root || exit 1
	if [ ! -e "/sys/class/net/${if_name}" ]; then
		log_error "Network interface not found: ${if_name}"
		exit 1
	fi

	if ! filter_list="$(ethtool -n "$if_name")"; then
		log_error "Cannot read the n-tuple filters of ${if_name}"
		exit 1
	fi

	log_info "Start to delete all filters for $if_name"
	mapfile -t rules < <(awk '/Filter:/ {print $2}' <<<"$filter_list")
	for rule in ${rules[@]+"${rules[@]}"}; do
		log_info "Delete filter $rule"
		ethtool -N "$if_name" delete "$rule"
	done
	log_success "All filters deleted for $if_name"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
