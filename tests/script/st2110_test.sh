#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

if [ -n "$1" ];  then
  P_PORT=$1
else
  # default
  P_PORT=0000:af:01.0
fi
if [ -n "$2" ];  then
  R_PORT=$1
else
  # default
  R_PORT=0000:af:01.1
fi
if [ -n "$3" ];  then
  DMA_PORT=$3
else
  # default
  DMA_PORT=0000:80:04.0
fi

echo "P_PORT: $P_PORT, R_PORT: $R_PORT, DMA_PORT: $DMA_PORT"

echo "Test with st2110"
./build/tests/KahawaiTest --auto_start_stop --p_port "$P_PORT" --r_port "$R_PORT" --dma_dev "$DMA_PORT"
echo "Test OK"
echo ""

echo "Test with st2110 RSS l4 udp"
#./build/tests/KahawaiTest --auto_start_stop --p_port "$P_PORT" --r_port "$R_PORT" --dma_dev "$DMA_PORT" --rss_mode l4_dst_port_only --gtest_filter="Main.*:St20p.*"
./build/tests/KahawaiTest --auto_start_stop --p_port "$P_PORT" --r_port "$R_PORT" --dma_dev "$DMA_PORT" --rss_mode l4_dst_port_only
echo "Test OK"
echo ""

echo "All done"

