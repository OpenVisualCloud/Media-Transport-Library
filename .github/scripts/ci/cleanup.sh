#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

echo 'Killing any running DPDK or MTL related processes...'
for process in gtest.sh KahawaiTest ffmpeg RxTxApp; do
	sudo killall -SIGKILL "$process" || true
done
echo 'Cleaning up supporting processes...'
for process in pytest MtlManager phc2sys ptp4l netsniff-ng; do
	sudo killall -SIGINT "$process" || true
done
sleep 2
