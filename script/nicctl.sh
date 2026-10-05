#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} <command> <bb:dd:ff.x> [args]

Bind a NIC port to the DPDK PMD or to the kernel driver, create and remove
VFs, and list the NIC ports. The list commands write the data to stdout and
need no root. The other commands need root. Status messages go to stderr.

REQUIRED PACKAGES (Debian/Ubuntu):
	dpdk (dpdk-devbind.py, from script/build_dpdk.sh) iproute2 kmod

OPTIONS:
	-h, --help		Show this help message

Commands:
	bind_pmd <bdf>			Bind driver to DPDK PMD driver
	bind_kernel <bdf>		Bind driver to kernel driver
	create_vf <bdf> [n]		Create VFs, bind to VFIO, and bring PF UP
	create_kvf <bdf> [n]		Create VFs and bind to kernel driver
	create_tvf <bdf> [n]		Create trusted VFs, bind to VFIO, and bring PF UP
	disable_vf <bdf>		Disable VF
	list all			List all NIC devices and the brief
	list up				List all NIC devices and the brief with UP status
	list <bdf>			List VFs of the specified PF

	The default VF count [n] is 6.

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Find dpdk-devbind.py in this tree first
				(default: <repo>/.local_install)

EXAMPLES:
	${script_name} list all
	sudo ${script_name} create_vf 0000:af:00.0 4
	sudo ${script_name} bind_pmd 0000:af:00.0
EOF
}

# Prints the value of field $2 (for example "if" or "drv") of the
# dpdk-devbind.py status line that matches $1. Prints nothing if no line has it.
devbind_field() {
	{ dpdk-devbind.py -s | grep "$1.*$2" | sed -e "s/.*$2=//g" | awk '{print $1;}'; } || true
}

iommu_check() {
	local iommu_groups_dir="/sys/kernel/iommu_groups/" iommu_groups_count
	iommu_groups_count=$(find "$iommu_groups_dir" -maxdepth 1 -mindepth 1 -type d | wc -l)
	if [ "$iommu_groups_count" == "0" ]; then
		log_warning "no iommu_groups in $iommu_groups_dir"
		log_warning "IOMMU is not enabled on this setup, please check kernel command line and BIOS settings"
	fi
}

bind_kernel() {
	local kernel_drv
	kernel_drv=$({ dpdk-devbind.py -s | grep "$bdf" | sed -e s/.*unused=//g | awk '{print $1;}'; } || true)
	if [ -n "$kernel_drv" ]; then
		dpdk-devbind.py -b "$kernel_drv" "$bdf"
	else
		log_warning "No kernel drv found for $bdf"
	fi
}

disable_vf() {
	echo 0 >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
}

create_vf() {
	local numvfs=$1 trusted="${2:-}" bifurcated_driver=0 kernel_drv i vfpath vfport vfif

	# Enable VFs
	echo "$numvfs" >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
	# wait VF driver
	kernel_drv=$(devbind_field "$bdf" drv)
	# check if mellanox driver is loaded, NVIDIA/Mellanox PMD uses bifurcated driver
	if [[ $kernel_drv == *"mlx"* ]]; then
		bifurcated_driver=1
	fi
	sleep 2

	# Start to bind to VFIO
	for ((i = 0; i < numvfs; i++)); do
		vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
		vfport=$({ readlink "$vfpath" | awk -F/ '{print $NF;}'; } || true)
		vfif=$(devbind_field "$vfport" if)
		if [ -n "$vfif" ]; then
			ip link set "$vfif" down
		fi
		if [ "$trusted" == "trusted" ]; then
			# enable trust
			ip link set "$inf" vf $i trust on
		fi
		if [ $bifurcated_driver -eq 0 ]; then
			if dpdk-devbind.py -b vfio-pci "$vfport"; then
				log_success "Bind $vfport($vfif) to vfio-pci success"
			fi
		else
			log_info "PMD uses bifurcated driver, No need to bind the $vfport($vfif) to vfio-pci"
		fi
	done
}

create_kvf() {
	local numvfs=$1 i vfpath vfport vfif
	# Enable VFs
	echo "$numvfs" >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
	for ((i = 0; i < numvfs; i++)); do
		vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
		vfport=$({ readlink "$vfpath" | awk -F/ '{print $NF;}'; } || true)
		vfif=$(devbind_field "$vfport" if)
		log_success "Bind $vfport($vfif) to kernel success"
	done
}

# Data on stdout: one VF BDF a line, or "No VFs found for <bdf>".
# tests/acceptance/common/nicctl.py reads both from stdout.
list_vf() {
	local pci_device_path="/sys/bus/pci/devices/$1/" vf_names vf vfport
	if [ ! -d "$pci_device_path" ]; then
		log_error "PCI device $1 does not exist."
		exit 1
	fi
	vf_names=$(find "$pci_device_path" -name "virtfn*" -exec basename {} \; | sort || true)
	if [ -z "$vf_names" ]; then
		echo "No VFs found for $1"
		return
	fi
	for vf in $vf_names; do
		vfport=$(basename "$(readlink "$pci_device_path/$vf")")
		printf "%s\n" "$vfport"
	done
}

# Data on stdout: a table, read by .github/scripts/gtest.sh and
# .github/scripts/ci/bind-test-ports.sh. Do not change the columns.
list() {
	local filter="${1:-}" id_counter=0 pci_bdf driver numa_node iommu_group interface_name
	trap '' PIPE
	printf "%-4s\t%-12s\t%-12s\t%-4s\t%-6s\t%-10s\n" "ID" "PCI BDF" "Driver" "NUMA" "IOMMU" "IF Name"

	for pci_bdf in $(dpdk-devbind.py -s | awk '/^Network devices/ {show=1; next} /^$/ {show=0} show && /drv=/ {print $1}'); do

		driver=$(basename "$(readlink /sys/bus/pci/devices/"${pci_bdf}"/driver)" 2>/dev/null || echo "N/A")

		numa_node=$(cat /sys/bus/pci/devices/"${pci_bdf}"/numa_node 2>/dev/null || echo "N/A")

		iommu_group=$(basename "$(readlink /sys/bus/pci/devices/"${pci_bdf}"/iommu_group)" 2>/dev/null || echo "N/A")

		interface_name=$(basename /sys/bus/pci/devices/"${pci_bdf}"/net/* 2>/dev/null || echo "N/A")

		if [[ $filter == "up" ]]; then
			ip link show "$interface_name" 2>/dev/null | grep -q "state UP" || continue
		fi

		printf "%-4s\t%-12s\t%-12s\t%-4s\t%-6s\t%-10s\n" \
			"$id_counter" "$pci_bdf" "$driver" "$numa_node" "$iommu_group" "$interface_name" 2>/dev/null || break

		id_counter=$((id_counter + 1))
	done
}

# Prints the VF count: $1, or 6 when $1 is empty.
vf_count() {
	if [ -z "${1:-}" ]; then
		# default VF number
		echo 6
	elif [[ $1 =~ ^[0-9]+$ ]]; then
		echo $((10#$1))
	else
		log_error "The VF count must be a number, not '$1'."
		exit 1
	fi
}

main() {
	local cmd="${1:-}" numvfs local_dpdk_bin

	# If local DPDK install is set or just exists then use that
	local_dpdk_bin="${MTL_INSTALL_PREFIX:-${REPO_DIR}/.local_install}/dpdk/bin"
	if [ -d "$local_dpdk_bin" ]; then
		PATH="${local_dpdk_bin}:${PATH}"
	fi

	case "$cmd" in
	-h | --help)
		show_help
		exit 0
		;;
	esac

	if [ $# -lt 2 ]; then
		log_error "A command and a PCI address (or all, up) are necessary."
		show_help >&2
		exit 2
	fi

	case "$cmd" in
	bind_kernel | create_vf | create_kvf | create_tvf | disable_vf | bind_pmd | list) ;;
	*)
		log_error "Command $1 not found"
		show_help >&2
		exit 1
		;;
	esac

	if [ "$cmd" == "list" ]; then
		if [ "$2" == "all" ]; then
			require_commands dpdk-devbind.py:dpdk || exit 1
			list
			exit 0
		elif [[ "$2" == "up" ]]; then
			require_commands dpdk-devbind.py:dpdk ip:iproute2 || exit 1
			list "up"
			exit 0
		else
			list_vf "$2"
			exit 0
		fi
	fi

	require_root "${script_name} ${cmd}" || exit 1
	require_commands dpdk-devbind.py:dpdk ip:iproute2 modprobe:kmod || exit 1

	bdf=$2
	numvfs=0
	case "$cmd" in
	create_*) numvfs=$(vf_count "${3:-}") ;;
	esac
	bdf_stat=$({ dpdk-devbind.py -s | { grep "$bdf" || true; }; } || true)
	if [ -z "$bdf_stat" ]; then
		log_error "$bdf not found in this platform"
		exit 1
	fi
	log_info "$bdf_stat"

	inf=$(devbind_field "$bdf" if)
	if [ "$cmd" == "bind_kernel" ]; then
		if [ -z "$inf" ]; then
			bind_kernel
			inf=$(devbind_field "$bdf" if)
			log_success "Bind bdf: $bdf to kernel $inf succ"
		else
			log_info "bdf: $bdf to kernel $inf already"
		fi
		exit 0
	fi

	iommu_check

	if [ "$cmd" == "bind_pmd" ]; then
		modprobe vfio-pci
		if [ -n "$inf" ]; then
			ip link set "$inf" down
		fi
		dpdk-devbind.py -b vfio-pci "$bdf"
		log_success "Bind bdf: $bdf to vfio-pci succ"
		exit 0
	fi

	# suppose bind kernel should be called for following commands
	if [ -z "$inf" ]; then
		bind_kernel
		inf=$(devbind_field "$bdf" if)
		log_success "Bind bdf: $bdf to kernel $inf succ"
	fi

	if [ "$cmd" == "disable_vf" ]; then
		disable_vf
		log_success "Disable vf bdf: $bdf $inf succ"
	fi

	if [ "$cmd" == "create_vf" ]; then
		modprobe vfio-pci
		disable_vf
		create_vf "$numvfs"
		# Ensure PF is UP so the VF admin queue works for DPDK init
		if [ -n "$inf" ]; then
			ip link set "$inf" up
		fi
		log_success "Create $numvfs VFs on PF bdf: $bdf $inf succ"
	fi

	if [ "$cmd" == "create_tvf" ]; then
		modprobe vfio-pci
		disable_vf
		create_vf "$numvfs" trusted
		# Ensure PF is UP so the VF admin queue works for DPDK init
		if [ -n "$inf" ]; then
			ip link set "$inf" up
		fi
		log_success "Create trusted $numvfs VFs on PF bdf: $bdf $inf succ"
	fi

	if [ "$cmd" == "create_kvf" ]; then
		disable_vf
		create_kvf "$numvfs"
		log_success "Create kernel VFs on PF bdf: $bdf $inf succ"
	fi
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
