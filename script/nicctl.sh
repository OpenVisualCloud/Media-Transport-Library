#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

if [ $# -lt 2 ]; then
    echo "Usage: "
    echo "    $0 <command> <bb:dd:ff.x> [args]"
    echo "Commands:"
    echo "   bind_pmd                 bind driver to DPDK PMD driver"
    echo "   bind_kernel              bind driver to kernel driver"
    echo "   create_vf                create VF and bind to VFIO"
    echo "   create_dcf_vf            create DCF VF and bind to VFIO"
    echo "   disable_vf               Disable VF"
    exit 0
fi

create_dcf_vf() {
    local numvfs=$1
    # Hard code

    # Enable VFs
    echo $numvfs > /sys/bus/pci/devices/$bdf/sriov_numvfs

    #enable trust
    ip link set $port vf 0 trust on

    # Start to bind to DCF VFIO
    for ((i=0;i<$numvfs;i++)); do
        vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
        vf=`readlink $vfpath | awk -F/ '{print $NF;}'`
        if [ $i -ne 1 ]; then
            dpdk-devbind.py -b vfio-pci $vf
            if [ $? -eq 0 ]; then
                echo "Bind $vf to dcf vfio success"
            fi
        fi
    done
}

bind_kernel() {
    if [ -n "$ice" ]; then
        dpdk-devbind.py -b ice $bdf
    fi
    if [ -n "$i40e" ]; then
        dpdk-devbind.py -b i40e $bdf
    fi
}

disable_vf() {
    echo 0 > /sys/bus/pci/devices/$bdf/sriov_numvfs
}

create_vf() {
    local numvfs=$1

    # Enable VFs
    echo $numvfs > /sys/bus/pci/devices/$bdf/sriov_numvfs

    # Start to bind to VFIO
    for ((i=0;i<$numvfs;i++)); do
        vfpath="/sys/bus/pci/devices/$bdf/virtfn$i"
        vf=`readlink $vfpath | awk -F/ '{print $NF;}'`
        #enable trust
        #ip link set $port vf $i trust on
        dpdk-devbind.py -b vfio-pci $vf
        if [ $? -eq 0 ]; then
            echo "Bind $vf to vfio success"
        fi
    done
}

cmdlist=("bind_kernel" "create_vf" "disable_vf" "bind_pmd" "create_dcf_vf")

for c in ${cmdlist[@]}; do
   if [ $c == $1 ]; then
       cmd=$c
       break
   fi
done

if [ -z "$cmd" ]; then
    echo "Command $1 not found"
    exit 1
fi

bdf=$2
ice=`dpdk-devbind.py -s | grep $bdf | grep ice`
i40e=`dpdk-devbind.py -s | grep $bdf | grep i40e`
if [ -z "$ice" ] && [ -z "$i40e" ]; then
    echo "$bdf is not ice(CVL) or i40e(FLV)"
    exit 1
fi

port=`dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}'`
if [ $cmd == "bind_kernel" ]; then
    if [ -z "$port" ]; then
        bind_kernel
    fi
    exit 0
fi

if [ $cmd == "bind_pmd" ]; then
    modprobe vfio-pci
    if [ -n "$port" ]; then
        ip link set $port down
    fi
    dpdk-devbind.py -b vfio-pci $bdf
    exit 0
fi

# suppose bind kernel should be called

if [ -z "$port" ]; then
    bind_kernel
    port=`dpdk-devbind.py -s | grep "$bdf.*if" | sed -e s/.*if=//g | awk '{print $1;}'`
fi

if [ $cmd == "disable_vf" ]; then
    disable_vf
fi


if [ $cmd == "create_dcf_vf" ]; then
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
fi

if [ $cmd == "create_vf" ]; then
    if [ -n "$3" ]; then
        numvfs=$(($3+0))
    else
        #default VF number
        numvfs=6
    fi
    modprobe vfio-pci
    disable_vf
    create_vf $numvfs
fi

