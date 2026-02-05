#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

if [ $# -lt 2 ]; then
	echo "Usage: "
	echo "    $0 <command> <bb:dd:ff.x> [args]"
	echo "Commands:"
	echo "   bind_pmd                 Bind driver to DPDK PMD driver"
	echo "   bind_kernel              Bind driver to kernel driver"
	echo "   create_vf                Create VFs and bind to VFIO"
	echo "   create_kvf               Create VFs and bind to kernel driver"
	echo "   create_tvf               Create trusted VFs and bind to VFIO"
	echo "   create_dcf_vf            Create DCF VFs and bind to VFIO"
	echo "   disable_vf               Disable VF"
	echo "   list all                 List all NIC devices and the brief"
	echo "   list up                  List all NIC devices and the brief with UP status"
	echo "   list <bb:dd:ff.x>        List VFs of the specified PF"
	exit 0
fi

iommu_check() {
	iommu_groups_dir="/sys/kernel/iommu_groups/"
	iommu_groups_count=$(find "$iommu_groups_dir" -maxdepth 1 -mindepth 1 -type d | wc -l)
	if [ "$iommu_groups_count" == "0" ]; then
		echo "Warn: no iommu_groups in $iommu_groups_dir"
		echo "Warn: IOMMU is not enabled on this setup, please check kernel command line and BIOS settings"
	fi
}

create_dcf_vf() {
	local numvfs=$1
	# Hard code

	# Enable VFs
	echo "$numvfs" >/sys/bus/pci/devices/"$bdf"/sriov_numvfs

	#enable trust
	ip link set "$inf" vf 0 trust on

	# Start to bind to DCF VFIO
	for ((i = 0; i < "$numvfs"; i++)); do
		vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
		vf=$(readlink "$vfpath" | awk -F/ '{print $NF;}')
		if [ "$i" -ne 1 ]; then
			if dpdk-devbind.py -b vfio-pci "$vf"; then
				echo "Bind $vf to dcf vfio success"
			fi
		fi
	done
}

bind_kernel() {
	kernel_drv=$(dpdk-devbind.py -s | grep "$bdf" | sed -e s/.*unused=//g | awk '{print $1;}')
	if [ -n "$kernel_drv" ]; then
		dpdk-devbind.py -b "$kernel_drv" "$bdf"
	else
		echo "No kernel drv found for $bdf"
	fi
}

disable_vf() {
	echo 0 >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
}

create_vf() {
	local numvfs=$1

	# Enable VFs
	echo "$numvfs" >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
	# wait VF driver
	bifurcated_driver=0
	kernel_drv=$(dpdk-devbind.py -s | grep "$bdf.*drv" | sed -e s/.*drv=//g | awk '{print $1;}')
	# check if mellanox driver is loaded, NVIDIA/Mellanox PMD uses bifurcated driver
	if [[ $kernel_drv == *"mlx"* ]]; then
		bifurcated_driver=1
	fi
	sleep 2

	# Start to bind to VFIO
	for ((i = 0; i < numvfs; i++)); do
		vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
		vfport=$(readlink "$vfpath" | awk -F/ '{print $NF;}')
		vfif=$(dpdk-devbind.py -s | grep "$vfport.*if" | sed -e s/.*if=//g | awk '{print $1;}')
		if [ -n "$vfif" ]; then
			ip link set "$vfif" down
		fi
		if [ "$2" == "trusted" ]; then
			# enable trust
			ip link set "$inf" vf $i trust on
		fi
		if [ $bifurcated_driver -eq 0 ]; then
			if dpdk-devbind.py -b vfio-pci "$vfport"; then
				echo "Bind $vfport($vfif) to vfio-pci success"
			fi
		else
			echo "PMD uses bifurcated driver, No need to bind the $vfport($vfif) to vfio-pci"
		fi
	done
}

create_kvf() {
	local numvfs=$1
	# Enable VFs
	echo "$numvfs" >/sys/bus/pci/devices/"$bdf"/sriov_numvfs
	for ((i = 0; i < numvfs; i++)); do
		vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
		vfport=$(readlink "$vfpath" | awk -F/ '{print $NF;}')
		vfif=$(dpdk-devbind.py -s | grep "$vfport.*if" | sed -e s/.*if=//g | awk '{print $1;}')
		echo "Bind $vfport($vfif) to kernel success"
	done
}

list_vf() {
	pci_device_path="/sys/bus/pci/devices/$1/"
	if [ ! -d "$pci_device_path" ]; then
		echo "PCI device $1 does not exist."
		exit 1
	fi
	vf_names=$(find "$pci_device_path" -name "virtfn*" -exec basename {} \; | sort)
	if [ -z "$vf_names" ]; then
		echo "No VFs found for $1"
		return
	fi
	for vf in $vf_names; do
		vfport=$(basename "$(readlink "$pci_device_path/$vf")")
		printf "%s\n" "$vfport"
	done
}

list() {
	trap '' PIPE
	printf "%-4s\t%-12s\t%-12s\t%-4s\t%-6s\t%-10s\n" "ID" "PCI BDF" "Driver" "NUMA" "IOMMU" "IF Name"

	id_counter=0

	for pci_bdf in $(dpdk-devbind.py -s | awk '/^Network devices/ {show=1; next} /^$/ {show=0} show && /drv=/ {print $1}'); do

		driver=$(basename "$(readlink /sys/bus/pci/devices/"${pci_bdf}"/driver)" 2>/dev/null || echo "N/A")

		numa_node=$(cat /sys/bus/pci/devices/"${pci_bdf}"/numa_node 2>/dev/null || echo "N/A")

		iommu_group=$(basename "$(readlink /sys/bus/pci/devices/"${pci_bdf}"/iommu_group)" 2>/dev/null || echo "N/A")

		interface_name=$(basename /sys/bus/pci/devices/"${pci_bdf}"/net/* 2>/dev/null || echo "N/A")

		if [[ $1 == "up" ]]; then
			ip link show "$interface_name" 2>/dev/null | grep -q "state UP" || continue
		fi

		printf "%-4s\t%-12s\t%-12s\t%-4s\t%-6s\t%-10s\n" \
			"$id_counter" "$pci_bdf" "$driver" "$numa_node" "$iommu_group" "$interface_name" 2>/dev/null || break

		id_counter=$((id_counter + 1))
	done
}

cmdlist=("bind_kernel" "create_vf" "create_kvf" "create_tvf" "disable_vf" "bind_pmd" "create_dcf_vf" "list")

for c in "${cmdlist[@]}"; do
	if [ "$c" == "$1" ]; then
		cmd=$c
		break
	fi
done

if [ -z "$cmd" ]; then
	echo "Command $1 not found"
	exit 1
fi

if [ "$cmd" == "list" ]; then
	if [ "$2" == "all" ]; then
		list
		exit 0
	elif [[ "$2" == "up" ]]; then
		list "up"
		exit 0
	else
		list_vf "$2"
		exit 0
	fi
fi

bdf=$2
bdf_stat=$(dpdk-devbind.py -s | { grep "$bdf" || true; })
if [ -z "$bdf_stat" ]; then
	echo "$bdf not found in this platform"
	exit 1
fi
echo "$bdf_stat"

inf=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
if [ "$cmd" == "bind_kernel" ]; then
	if [ -z "$inf" ]; then
		bind_kernel
		inf=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
		echo "Bind bdf: $bdf to kernel $inf succ"
	else
		echo "bdf: $bdf to kernel $inf already"
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
	echo "Bind bdf: $bdf to vfio-pci succ"
	exit 0
fi

# suppose bind kernel should be called for following commands
if [ -z "$inf" ]; then
	bind_kernel
	inf=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
	echo "Bind bdf: $bdf to kernel $inf succ"
fi

if [ "$cmd" == "disable_vf" ]; then
	disable_vf
	echo "Disable vf bdf: $bdf $inf succ"
fi

if [ "$cmd" == "create_dcf_vf" ]; then
	if [ -z "$ice" ]; then
		echo "only CVL device is allowed"
		exit 1
	fi

	if [ -n "$3" ]; then
		numvfs=$(($3 + 0))
	else
		#default VF number
		numvfs=6
	fi
	disable_vf
	create_dcf_vf $numvfs
	echo "Create dcf vf bdf: $bdf $inf succ"
fi

if [ "$cmd" == "create_vf" ]; then
	if [ -n "$3" ]; then
		numvfs=$(($3 + 0))
	else
		# default VF number
		numvfs=6
	fi
	modprobe vfio-pci
	disable_vf
	create_vf $numvfs
	echo "Create $numvfs VFs on PF bdf: $bdf $inf succ"
fi

if [ "$cmd" == "create_tvf" ]; then
	if [ -n "$3" ]; then
		numvfs=$(($3 + 0))
	else
		# default VF number
		numvfs=6
	fi
	modprobe vfio-pci
	disable_vf
	create_vf $numvfs trusted
	echo "Create trusted $numvfs VFs on PF bdf: $bdf $inf succ"
fi

if [ "$cmd" == "create_kvf" ]; then
	if [ -n "$3" ]; then
		numvfs=$(($3 + 0))
	else
		# default VF number
		numvfs=6
	fi
	disable_vf
	create_kvf $numvfs
	echo "Create kernel VFs on PF bdf: $bdf $inf succ"
fi
