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
Usage: ${script_name} [OPTIONS]

Collect the system status of this host for a setup issue report. The script
makes the directory mtl_system_status_<YYYYmmddHHMMSS> in the current
directory, writes kernel_log.txt and system_info.txt into it, and packs it
into mtl_system_status_<YYYYmmddHHMMSS>.tar. Some commands need root, so the
script uses sudo for them.

REQUIRED PACKAGES (Debian/Ubuntu):
	ethtool iproute2 pciutils tar util-linux
	dpdk (dpdk-devbind.py, optional, from script/build_dpdk.sh)

OPTIONS:
	-h		Show this help message

EXAMPLES:
	${script_name}		# Make the report in the current directory
EOF
}

# Writes $1 to stdout and adds it to the report.
section() {
	echo "$1" | tee -a "$SYS_INFO_OUT"
}

# Adds the stdout of the command to the report. A command that fails does not
# stop the report: the report must show as much of the host as it can.
collect() {
	"$@" | tee -a "$SYS_INFO_OUT" >/dev/null || log_warning "Command failed: $*"
}

blank_line() {
	echo "" | tee -a "$SYS_INFO_OUT" >/dev/null
}

main() {
	local opt iface ethernet_lines pci_address

	while getopts "h" opt; do
		case $opt in
		h)
			show_help
			exit 0
			;;
		*)
			show_help
			exit 1
			;;
		esac
	done
	shift $((OPTIND - 1))
	[ "$#" -eq 0 ] || {
		log_error "Unexpected argument: $1"
		show_help
		exit 1
	}

	require_commands dmesg:util-linux lscpu:util-linux ip:iproute2 \
		ethtool:ethtool lspci:pciutils tar:tar || exit 1
	command_exists dpdk-devbind.py ||
		log_warning "dpdk-devbind.py is not in PATH. The report has no dpdk-devbind status."

	REPORT_DIR="mtl_system_status_$(date +%Y%m%d%H%M%S)"
	log_info "Create dir: $REPORT_DIR for the report"
	mkdir "$REPORT_DIR"

	log_info "Collect dmesg:"
	as_root dmesg | tee "$REPORT_DIR"/kernel_log.txt >/dev/null ||
		log_warning "Command failed: dmesg"

	SYS_INFO_OUT=$REPORT_DIR/system_info.txt
	: >"$SYS_INFO_OUT"

	section "Collect system info:"
	collect uname -a
	blank_line

	section "Collect kernel cmdline:"
	collect as_root cat /proc/cmdline
	blank_line

	section "Collect HugePages info:"
	collect grep -i "HugePages" /proc/meminfo
	blank_line

	section "Collect status of dpdk-devbind:"
	if command_exists dpdk-devbind.py; then
		collect dpdk-devbind.py -s
	fi
	blank_line

	section "Collect iommu_groups:"
	collect find /sys/kernel/iommu_groups/ -maxdepth 1 -mindepth 1 -type d
	blank_line

	section "Collect cpu info:"
	collect lscpu
	blank_line

	log_info "Collect ethernet interface info:"
	for iface in $(ip -o link show | awk -F': ' '{print $2}'); do
		if [[ "$iface" == "lo" || "$iface" == docker* ]]; then
			continue
		fi

		section "Collect ethtool info for $iface"
		collect as_root ethtool "$iface"
		collect ethtool -i "$iface"
		blank_line
	done

	log_info "Collect ethernet lspci info:"
	ethernet_lines=$({ lspci | grep -i ethernet; } || true)
	for pci_address in $(echo "$ethernet_lines" | awk '{print $1}'); do
		section "Collect lspci info for $pci_address"
		collect as_root lspci -vv -s "$pci_address"
		blank_line
	done

	log_info "Create $REPORT_DIR.tar"
	tar -cvf "$REPORT_DIR".tar "$REPORT_DIR"
	log_success "All finished, share $REPORT_DIR.tar for the setup issues report."
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
