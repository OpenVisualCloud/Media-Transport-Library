#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

# set -e

TX_IF=enp175s0f0np0
RX_IF=enp175s0f1np1
TX_IP=192.168.99.101/24
RX_IP=192.168.99.102/24

sudo ip netns delete rdma0
sudo ip netns delete rdma1

sudo ip netns add rdma0
sudo ip netns add rdma1
# sudo ip netns list

sudo ip link set ${TX_IF} netns rdma0
sudo ip link set ${RX_IF} netns rdma1

sudo ip netns exec rdma1 ip link list

sudo ip netns exec rdma0 ip addr add ${TX_IP} dev ${TX_IF}
sudo ip netns exec rdma1 ip addr add ${RX_IP} dev ${RX_IF}

sudo ip netns exec rdma0 ip link set dev ${TX_IF} up
sudo ip netns exec rdma1 ip link set dev ${RX_IF} up

sudo ip netns exec rdma0 ifconfig ${TX_IF} mtu 2100
sudo ip netns exec rdma1 ifconfig ${RX_IF} mtu 2100

# display the ns info
sudo ip netns exec rdma0 ifconfig ${TX_IF}
sudo ip netns exec rdma1 ifconfig ${RX_IF}
