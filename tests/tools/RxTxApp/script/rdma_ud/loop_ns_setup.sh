#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

# set -e

TX_IF=enp175s0f0np0
RX_IF=enp175s0f1np1
TX_IP=192.168.99.101/24
RX_IP=192.168.99.102/24

sudo ip addr add ${TX_IP} dev ${TX_IF}
sudo ip addr add ${RX_IP} dev ${RX_IF}

sudo ip link set dev ${TX_IF} up
sudo ip link set dev ${RX_IF} up

sudo ifconfig ${TX_IF} mtu 2100
sudo ifconfig ${RX_IF} mtu 2100

# allow local arp
sudo sysctl net.ipv4.conf.${TX_IF}.accept_local=1
sudo sysctl net.ipv4.conf.${RX_IF}.accept_local=1

# display the ns info
ifconfig ${TX_IF}
ifconfig ${RX_IF}
