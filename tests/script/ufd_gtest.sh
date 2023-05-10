#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

if [ -n "$1" ];  then
  P_PORT=$1
else
  P_PORT=0000:af:01.0
fi
if [ -n "$2" ];  then
  R_PORT=$2
else
  R_PORT=0000:af:01.1
fi

echo "P_PORT: $P_PORT, R_PORT: $R_PORT"

echo "Test with dedicated queue mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT"
echo "Test OK"
echo ""

echo "Test with dedicated queue and lcore mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT" --udp_lcore
echo "Test OK"
echo ""

echo "Test with shared queue mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT" --queue_mode shared
echo "Test OK"
echo ""

echo "Test with shared queue and lcore mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT" --queue_mode shared --udp_lcore
echo "Test OK"
echo ""

echo "Test with rss mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT" --queue_mode shared --rss_mode l4_dst_port_only
echo "Test OK"
echo ""

echo "Test with rss and lcore mode"
./build/tests/KahawaiUfdTest --p_port "$P_PORT" --r_port "$R_PORT" --queue_mode shared --udp_lcore --rss_mode l4_dst_port_only
echo "Test OK"
echo ""

echo "All done"

