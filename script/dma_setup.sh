#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

# The Intel devices of the DPDK dmadev drivers: drivers/dma/ioat (CBDMA)
# and drivers/dma/idxd (DSA).
IOAT_IDS="6f20 6f21 6f22 6f23 6f24 6f25 6f26 6f27 6f2e 6f2f 2021 0b00"
IDXD_IDS="0b25 11fb 1212"
SYSFS_PCI_DEVICES="/sys/bus/pci/devices"

show_help() {
	cat <<EOF
Usage: ${script_name} <command> [port...]

Bind the Intel DMA devices (CBDMA or DSA) to vfio-pci, and find the DMA
devices for a NIC port. MTL uses a DMA device only on the NUMA node of the
port, so both commands select the DMA devices by the NUMA node of the port.
A port is a PCI address (0000:af:00.0, also a VF) or an interface name.

REQUIRED PACKAGES (Debian/Ubuntu):
	dpdk (dpdk-devbind.py, from script/build_dpdk.sh) kmod

OPTIONS:
	-h, --help	Show this help message

Commands:
	bind [port...]	Bind DMA devices on the NUMA node of each port to
			vfio-pci. Without a port, bind them on each NUMA node.
	get <port...>	Print the DMA devices on vfio-pci on the NUMA node of
			each port, in the format of the --dma_dev argument.
	reset [port...]	Unbind each DMA device on the NUMA node of each port
			from vfio-pci, then bind as "bind" does. A device that
			a process holds stays bound. Without a port, reset
			each NUMA node.

ENVIRONMENT:
	DMA_CHANNELS		DMA devices to bind on one NUMA node (default: 2)
	MTL_INSTALL_PREFIX	The MTL prefix: find dpdk-devbind.py in its dpdk
				sibling first (default: <repo>/.local_install/mtl)

EXAMPLES:
	sudo ${script_name} bind 0000:af:00.0
	sudo ${script_name} bind
	sudo ${script_name} reset
	RxTxApp --dma_dev \$(${script_name} get 0000:af:01.0) ...
EOF
}

# Prints "<bdf> <numa node> <driver> <ioat|idxd>" for each Intel DMA device.
# The driver is "-" if the device has no driver.
dma_devices() {
	local dev vendor id kind driver numa
	for dev in "${SYSFS_PCI_DEVICES}"/*; do
		read -r vendor <"${dev}/vendor"
		[ "${vendor}" = 0x8086 ] || continue
		read -r id <"${dev}/device"
		id=${id#0x}
		if [[ " ${IOAT_IDS} " == *" ${id} "* ]]; then
			kind=ioat
		elif [[ " ${IDXD_IDS} " == *" ${id} "* ]]; then
			kind=idxd
		else
			continue
		fi
		driver=-
		[ ! -e "${dev}/driver" ] || driver=$(basename "$(readlink -f "${dev}/driver")")
		read -r numa <"${dev}/numa_node"
		echo "$(basename "${dev}") ${numa} ${driver} ${kind}"
	done
}

# Prints the DMA devices on NUMA node $1: first the devices on vfio-pci, then
# the devices with no driver, then the devices on a kernel driver.
node_devices() {
	dma_devices | awk -v numa="$1" '$2 == numa {
		print ($3 == "vfio-pci" ? 0 : ($3 == "-" ? 1 : 2)), $0
	}' | sort -s -n -k1,1 | cut -d' ' -f2-
}

# Prints the NUMA node of port $1.
port_numa() {
	local dev="${SYSFS_PCI_DEVICES}/$1"
	if [ -e "/sys/class/net/$1/device" ]; then
		dev="/sys/class/net/$1/device"
	elif [ ! -e "${dev}" ]; then
		dev="${SYSFS_PCI_DEVICES}/0000:$1"
	fi
	if [ ! -e "${dev}/numa_node" ]; then
		log_error "$1 is not a PCI address or an interface name."
		return 1
	fi
	cat "${dev}/numa_node"
}

# Prints each NUMA node of the ports $@ one time. Without a port, prints each
# NUMA node that has a DMA device.
numa_nodes() {
	local port
	if [ $# -eq 0 ]; then
		dma_devices | awk '{print $2}' | sort -un
		return
	fi
	for port in "$@"; do
		port_numa "${port}" || return 1
	done | sort -un
}

# vfio-pci does not bind Intel DSA while its denylist is on. The kernel reads
# disable_denylist only when it loads the module (doc/dma.md).
load_vfio_pci() {
	local kind=$1 denylist=/sys/module/vfio_pci/parameters/disable_denylist
	if [ -d /sys/module/vfio_pci ]; then
		[ "${kind}" = idxd ] && [ "$(cat "${denylist}" 2>/dev/null)" = N ] || return 0
		if compgen -G "/sys/bus/pci/drivers/vfio-pci/*:*:*.*" >/dev/null; then
			log_error "vfio-pci has its denylist on, so it cannot bind Intel DSA."
			log_error "A reload of vfio-pci turns the denylist off, but vfio-pci holds devices now."
			log_error "Unbind these devices, or set the option and reboot:"
			log_error "  echo 'options vfio-pci disable_denylist=1' | sudo tee /etc/modprobe.d/vfio-pci.conf"
			return 1
		fi
		log_info "Reload vfio-pci with disable_denylist=1 for Intel DSA."
		modprobe -r vfio-pci
	fi
	modprobe vfio-pci disable_denylist=1
}

# Binds DMA_CHANNELS DMA devices on NUMA node $1 to vfio-pci.
bind_node() {
	local numa=$1 bdf driver kind dev bound=()
	while read -r bdf _ driver kind; do
		if [ "${driver}" != vfio-pci ]; then
			load_vfio_pci "${kind}"
			dpdk-devbind.py -b vfio-pci "${bdf}"
			# dpdk-devbind.py exits 0 also when the kernel refuses the bind.
			dev="${SYSFS_PCI_DEVICES}/${bdf}"
			if [ "$(basename "$(readlink -f "${dev}/driver")")" != vfio-pci ]; then
				[ "$(cat "${dev}/driver_override" 2>/dev/null)" != vfio-pci ] || echo >"${dev}/driver_override"
				log_error "DMA ${bdf} is not on vfio-pci after dpdk-devbind.py -b. Look at dmesg."
				return 1
			fi
		fi
		bound+=("${bdf}")
	done < <(node_devices "${numa}" | head -n "${DMA_CHANNELS}")

	if [ "${#bound[@]}" -lt "${DMA_CHANNELS}" ]; then
		log_warning "NUMA node ${numa} has ${#bound[@]} of ${DMA_CHANNELS} DMA devices."
	fi
	if [ "${#bound[@]}" -gt 0 ]; then
		log_success "NUMA node ${numa}: DMA ${bound[*]} on vfio-pci"
	fi
}

# Returns 0 if a process holds the VFIO group of DMA device $1 open.
dma_in_use() {
	local group
	group=$(basename "$(readlink -f "${SYSFS_PCI_DEVICES}/$1/iommu_group")")
	find /proc/[0-9]*/fd -lname "/dev/vfio/${group}" -print -quit 2>/dev/null | grep -q .
}

# Unbinds each DMA device on NUMA node $1 from vfio-pci, except a device in use
# and a DSA device that the denylist of vfio-pci would not let bind again.
unbind_node() {
	local numa=$1 bdf driver kind
	while read -r bdf _ driver kind; do
		[ "${driver}" = vfio-pci ] || continue
		if dma_in_use "${bdf}"; then
			log_warning "DMA ${bdf} is in use by a process. It stays on vfio-pci."
			continue
		fi
		if [ "${kind}" = idxd ] && [ "$(cat /sys/module/vfio_pci/parameters/disable_denylist 2>/dev/null)" = N ]; then
			log_warning "DMA ${bdf} stays on vfio-pci: the denylist of vfio-pci would stop a new bind."
			continue
		fi
		dpdk-devbind.py -u "${bdf}"
	done < <(node_devices "${numa}")
}

# Prints the DMA devices on vfio-pci on the NUMA nodes $@, separated by commas.
get_devices() {
	local numa list=()
	for numa in "$@"; do
		mapfile -t -O "${#list[@]}" list < <(node_devices "${numa}" | awk '$3 == "vfio-pci" {print $1}')
	done
	if [ "${#list[@]}" -eq 0 ]; then
		log_error "No DMA device on vfio-pci on NUMA node $*. Run: sudo ${script_name} bind <port>"
		return 1
	fi
	local IFS=,
	echo "${list[*]}"
}

main() {
	local cmd="${1:-}" nodes=() numa out local_dpdk_bin

	case "${cmd}" in
	-h | --help)
		show_help
		return 0
		;;
	bind | get | reset) shift ;;
	*)
		log_error "Unknown command: ${cmd}"
		show_help >&2
		return 2
		;;
	esac

	if [ "${cmd}" = get ] && [ $# -eq 0 ]; then
		log_error "get needs a port."
		return 2
	fi
	out=$(numa_nodes "$@") || return 1
	[ -z "${out}" ] || mapfile -t nodes <<<"${out}"

	if [ "${cmd}" = get ]; then
		get_devices "${nodes[@]}"
		return
	fi

	require_root "${script_name} ${cmd}" || return 1
	local_dpdk_bin="$(dirname -- "${MTL_INSTALL_PREFIX:-${REPO_DIR}/.local_install/mtl}")/dpdk/bin"
	if [ -d "${local_dpdk_bin}" ]; then
		PATH="${local_dpdk_bin}:${PATH}"
	fi
	require_commands dpdk-devbind.py:dpdk modprobe:kmod || return 1
	: "${DMA_CHANNELS:=2}"

	if [ "${#nodes[@]}" -eq 0 ]; then
		log_warning "This platform shows no Intel DMA device. Enable CBDMA or DSA in the BIOS."
		return 0
	fi
	for numa in "${nodes[@]}"; do
		[ "${cmd}" != reset ] || unbind_node "${numa}"
		bind_node "${numa}"
	done
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
