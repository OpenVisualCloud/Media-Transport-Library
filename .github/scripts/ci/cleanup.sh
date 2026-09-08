#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

echo 'Killing any running DPDK or MTL related processes...'
# gst-launch-1.0_ is the wrapper the acceptance suite invokes; both spellings
# have to be killed or a hung GStreamer pipeline outlives the job and its NIC
# queues and hugepages are still held when the next one starts.
for process in gtest.sh KahawaiTest ffmpeg gst-launch-1.0 gst-launch-1.0_ RxTxApp; do
	sudo killall -SIGKILL "$process" || true
done
echo 'Cleaning up supporting processes...'
for process in pytest MtlManager phc2sys ptp4l netsniff-ng; do
	sudo killall -SIGINT "$process" || true
done
sleep 2
