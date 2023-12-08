#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Disable error break since we need loop all jsons
# set -e

RXTXAPP=../../build/app/RxTxApp
TEST_JSON_DIR=loop_json
TEST_TIME_SEC=100
TEST_LOOP=1
TEST_MULTI=true
TEST_REDUNDANT=true

TEST_JSON_LIST=("1080p59_1v.json" "1080p50_1v.json" "1080p29_1v.json" \
	"720p59_1v.json" "720p50_1v.json" "720p29_1v.json" \
	"4kp59_1v.json" "4kp50_1v.json" "4kp29_1v.json" \
	"unicast_1v_1a_1anc.json" "rtp_1v_1a_1anc.json")
TEST_JSON_LIST_MULTI=("unicast_4v_4a_4anc.json"	"rtp_4v_4a_4anc.json")
TEST_JSON_LIST_REDUNDANT=("unicast_redundant_1v_1a_1anc.json" "rtp_redundant_1v_1a_1anc.json" \
	"unicast_redundant_4v_4a_4anc.json" "rtp_redundant_4v_4a_4anc.json")

if [ $TEST_MULTI == "true" ]; then
	TEST_JSON_LIST=("(${TEST_JSON_LIST[@]} ${TEST_JSON_LIST_MULTI[@]})")
fi
if [ $TEST_REDUNDANT == "true" ]; then
	TEST_JSON_LIST=("(${TEST_JSON_LIST[@]} ${TEST_JSON_LIST_REDUNDANT[@]})")
fi

echo "Total ${#TEST_JSON_LIST[@]} cases, each with ${TEST_TIME_SEC}s"
for ((loop=0; loop<TEST_LOOP; loop++)); do
	cur_json_idx=0
	for json_file in "${TEST_JSON_LIST[@]}"; do
		cmd="$RXTXAPP --log_level error --test_time $TEST_TIME_SEC --config_file $TEST_JSON_DIR/$json_file --rx_timing_parser"
		echo "test with cmd: $cmd, index: $cur_json_idx, loop: $loop"
		cur_json_idx=$((cur_json_idx+1))
		$cmd
		echo "Test OK with config: $json_file"
		echo ""
	done
done

echo "All test cases finished"
