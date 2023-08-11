#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

REPORT_DIR="mtl_system_status_$(date +%Y%m%d%H%M%S)"
echo "Create dir: $REPORT_DIR for the report"
mkdir "$REPORT_DIR"

echo "Collect dmesg:"
sudo dmesg | tee "$REPORT_DIR"/kernel_log.txt > /dev/null

echo "Collect kernel cmdline:" | tee "$REPORT_DIR"/system_info.txt
sudo cat /proc/cmdline | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null

echo "Collect HugePages info:" | tee -a "$REPORT_DIR"/system_info.txt
grep -i "HugePages" /proc/meminfo | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null

echo "Collect status of dpdk-devbind:" | tee -a "$REPORT_DIR"/system_info.txt
dpdk-devbind.py -s | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null

echo "Collect iommu_groups:" | tee -a "$REPORT_DIR"/system_info.txt
find /sys/kernel/iommu_groups/ -maxdepth 1 -mindepth 1 -type d | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null

echo "Collect ethernet interface info:"
for iface in $(ip -o link show | awk -F': ' '{print $2}')
do
    if [[ "$iface" == "lo" || "$iface" == docker* ]]; then
        continue
    fi

    echo "Collect ethtool info for $iface" | tee -a "$REPORT_DIR"/system_info.txt
    sudo ethtool "$iface" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
    ethtool -i "$iface" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
    echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
done

echo "Collect ethernet lspci info:"
ethernet_lines=$(lspci | grep -i ethernet)
for pci_address in $(echo "$ethernet_lines" | awk '{print $1}')
do
    echo "Collect lspci info for $pci_address" | tee -a "$REPORT_DIR"/system_info.txt
    sudo lspci -vv -s "$pci_address" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
    echo "" | tee -a "$REPORT_DIR"/system_info.txt > /dev/null
done

echo "Create $REPORT_DIR.tar"
tar -cvf "$REPORT_DIR".tar "$REPORT_DIR"
echo "All finished, share $REPORT_DIR.tar for the setup issues report."
