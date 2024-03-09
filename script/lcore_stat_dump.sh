#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

if [ -n "$1" ];  then
  cpu=$1
else
  echo "Usage: sudo ./script/lcore_stat_dump.sh 92"
  exit 0
fi

# default 10s
time_s=10
if [ -n "$2" ];  then
  time_s=$2
fi

out_file=lcore_status_cpu${cpu}.log

echo "Collecting sched and irq events on cpu: ${cpu}, time ${time_s}s"

# disable tracing
echo 0 > /sys/kernel/debug/tracing/tracing_on
# flush trace
echo > /sys/kernel/debug/tracing/trace
# enable sched
echo 1 > /sys/kernel/debug/tracing/events/sched/sched_switch/enable
# enable irq
echo 1 > /sys/kernel/debug/tracing/events/irq_vectors/enable
# enable
echo 1 > /sys/kernel/debug/tracing/tracing_on

# sleep
sleep "${time_s}"

# disable trace and save the log
echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/per_cpu/cpu"${cpu}"/trace > "${out_file}"
echo "Collected to file: ${out_file}"