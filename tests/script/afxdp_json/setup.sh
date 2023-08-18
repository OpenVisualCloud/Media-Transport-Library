#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

AFXDP_PORT_0=enp175s0f0np0
AFXDP_PORT_1=enp175s0f1np1

echo "Config  ${AFXDP_PORT_0}"
sudo nmcli dev set ${AFXDP_PORT_0} managed no
sudo ifconfig ${AFXDP_PORT_0} 192.168.108.101/24
echo 2 | sudo tee /sys/class/net/${AFXDP_PORT_0}/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/${AFXDP_PORT_0}/gro_flush_timeout
echo "Config  ${AFXDP_PORT_1}"
sudo nmcli dev set ${AFXDP_PORT_1} managed no
sudo ifconfig ${AFXDP_PORT_1} 192.168.108.102/24
echo 2 | sudo tee /sys/class/net/${AFXDP_PORT_1}/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/${AFXDP_PORT_1}/gro_flush_timeout

echo "Disable rp_filter"
sudo sysctl -w net.ipv4.conf.all.rp_filter=0