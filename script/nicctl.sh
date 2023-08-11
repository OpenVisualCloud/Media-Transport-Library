#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

if [ $# -lt 2 ]; then
    echo "Usage: "
    echo "    $0 <command> <bb:dd:ff.x> [args]"
    echo "Commands:"
    echo "   bind_pmd                 bind driver to DPDK PMD driver"
    echo "   bind_kernel              bind driver to kernel driver"
    echo "   create_vf                create VF and bind to VFIO"
    echo "   create_dcf_vf            create DCF VF and bind to VFIO"
    echo "   disable_vf               Disable VF"
    echo "   status                   List the DPDK port status"
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
    echo "$numvfs" > /sys/bus/pci/devices/"$bdf"/sriov_numvfs

    #enable trust
    ip link set "$port" vf 0 trust on

    # Start to bind to DCF VFIO
    for ((i=0;i<"$numvfs";i++)); do
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
    echo 0 > /sys/bus/pci/devices/"$bdf"/sriov_numvfs
}

create_vf() {
    local numvfs=$1

    # Enable VFs
    echo "$numvfs" > /sys/bus/pci/devices/"$bdf"/sriov_numvfs
    # wait VF driver
    bifurcated_driver=0
    kernel_drv=$(dpdk-devbind.py -s | grep "$bdf.*drv" | sed -e s/.*drv=//g | awk '{print $1;}')
    # check if mellanox driver is loaded, NVIDIA/Mellanox PMD uses bifurcated driver
    if [[ $kernel_drv == *"mlx"* ]]; then
	bifurcated_driver=1
    fi
    sleep 2

    # Start to bind to VFIO
    for ((i=0;i<numvfs;i++)); do
        vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
        vfport=$(readlink "$vfpath" | awk -F/ '{print $NF;}')
        vfif=$(dpdk-devbind.py -s | grep "$vfport.*if" | sed -e s/.*if=//g | awk '{print $1;}')
        if [ -n "$vfif" ]; then
            ip link set "$vfif" down
        fi
        #enable trust
        #ip link set $port vf $i trust on
	if [ $bifurcated_driver -eq 0 ]; then
            if dpdk-devbind.py -b vfio-pci "$vfport"; then
                echo "Bind $vfport($vfif) to vfio-pci success"
            fi
	else
	    echo "PMD uses bifurcated driver, No need to bind the $vfport($vfif) to vfio-pci"
	fi

    done
}

cmdlist=("bind_kernel" "create_vf" "disable_vf" "bind_pmd" "create_dcf_vf" "status")

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

bdf=$2
bdf_stat=$(dpdk-devbind.py -s | { grep "$bdf" || true; })
if [ -z "$bdf_stat" ]; then
   echo "$bdf not found in this platform"
   exit 1
fi
echo "$bdf_stat"

port=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
if [ "$cmd" == "bind_kernel" ]; then
    if [ -z "$port" ]; then
        bind_kernel
        port=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
        echo "Bind bdf: $bdf to kernel $port succ"
    else
        echo "bdf: $bdf to kernel $port already"
    fi
    exit 0
fi

iommu_check

if [ "$cmd" == "bind_pmd" ]; then
    modprobe vfio-pci
    if [ -n "$port" ]; then
        ip link set "$port" down
    fi
    dpdk-devbind.py -b vfio-pci "$bdf"
    echo "Bind bdf: $bdf to vfio-pci succ"
    exit 0
fi

# suppose bind kernel should be called

if [ -z "$port" ]; then
    bind_kernel
    port=$(dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}')
    echo "Bind bdf: $bdf to kernel $port succ"
fi

if [ "$cmd" == "disable_vf" ]; then
    disable_vf
    echo "Disable vf bdf: $bdf $port succ"
fi

if [ "$cmd" == "create_dcf_vf" ]; then
    if [ -z "$ice" ]; then
        echo "only CVL device is allowed"
        exit 1
    fi

    if [ -n "$3" ]; then
        numvfs=$(($3+0))
    else
        #default VF number
        numvfs=6
    fi
    disable_vf
    create_dcf_vf $numvfs
    echo "Create dcf vf bdf: $bdf $port succ"
fi

if [ "$cmd" == "create_vf" ]; then
    if [ -n "$3" ]; then
        numvfs=$(($3+0))
    else
        #default VF number
        numvfs=6
    fi
    modprobe vfio-pci
    disable_vf
    create_vf $numvfs
    echo "Create VFs on PF bdf: $bdf $port succ"
fi

