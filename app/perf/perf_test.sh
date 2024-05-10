#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

ST_PORT=0000:af:01.0
DMA_PORT=0000:80:04.0
ST_SIP=192.168.89.89

TEST_BIN_PATH=../../build/app
LOG_LEVEL=error
TEST_FRAMES=120
TEST_FB_CNT=3

perf_func() {
	local perf_prog=$1
	echo "Start to run: ${perf_prog}"
	"${TEST_BIN_PATH}"/"${perf_prog}" --log_level "${LOG_LEVEL}" --p_port "${ST_PORT}" --p_sip "${ST_SIP}" --dma_port "${DMA_PORT}" --perf_frames "${TEST_FRAMES}" --perf_fb_cnt "${TEST_FB_CNT}"
	echo ""
}

perf_func PerfRfc4175422be10ToP10Le
perf_func PerfP10LeToRfc4175422be10
perf_func PerfRfc4175422be10ToLe
perf_func PerfRfc4175422le10ToBe
perf_func PerfRfc4175422be10ToLe8
perf_func PerfRfc4175422be10ToV210
perf_func PerfV210ToRfc4175422be10
perf_func PerfRfc4175422be10ToY210
perf_func PerfY210ToRfc4175422be10
perf_func PerfRfc4175422be12ToLe
perf_func PerfRfc4175422be12ToP12Le
perf_func PerfRfc4175422be10ToP8
perf_func PerfDma

echo "****** All Perf test OK ******"
