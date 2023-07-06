#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Disable error break since we need loop all jsons
# set -e

RXTXAPP=../../build/app/RxTxApp
TEST_JSON_DIR=.
TEST_TIME_SEC=100
#TEST_TIME_SEC=20
TEST_LOOP=1

export KAHAWAI_CFG_PATH=../../kahawai.json
echo "TEST_TIME_SEC: ${TEST_TIME_SEC}s"
for ((loop=0; loop<TEST_LOOP; loop++)); do
	cur_json_idx=0
	for json_file in loop_json/*.json; do
		cmd="$RXTXAPP --log_level error --test_time $TEST_TIME_SEC --config_file $TEST_JSON_DIR/$json_file"
		echo "test with cmd: $cmd, index: $cur_json_idx, loop: $loop"
		cur_json_idx=$((cur_json_idx+1))
		$cmd
		echo "Test OK with config: $json_file"
		echo ""
	done
done

unset KAHAWAI_CFG_PATH

echo "All test cases finished"
