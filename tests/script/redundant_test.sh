#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

TEST_TIME_SEC_TX=50
TEST_TIME_SEC_RX=60

echo "Redundant: start tx"
../../build/app/RxTxApp --config_file redundant_json/tx.json --log_level error --test_time ${TEST_TIME_SEC_TX} &
echo "Redundant: start rx"
TEST_TIME_SEC=${TEST_TIME_SEC_RX} ../../build/app/RxSt20RedundantSample

echo "Redundant: wait all thread ending"
wait