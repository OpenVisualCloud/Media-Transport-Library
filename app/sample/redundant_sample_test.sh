#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

TEST_TIME_SEC=15

TEST_BIN_PATH=../../build/app
LOG_LEVEL=notice

name=RxSt20RedundantSample

${TEST_BIN_PATH}/TxSt20PipelineSample > /dev/null --log_level ${LOG_LEVEL} --p_port 0000:af:01.2 --r_port 0000:af:01.3 --p_sip 192.168.77.2 --r_sip 192.168.77.3 --p_tx_ip 239.168.77.20 --r_tx_ip 239.168.77.21 2>&1 &
pid_tx=$!
${TEST_BIN_PATH}/RxSt20RedundantSample --log_level ${LOG_LEVEL} --p_port 0000:af:01.0 --r_port 0000:af:01.1 --p_sip 192.168.77.11 --r_sip 192.168.77.12 --p_rx_ip 239.168.77.20 --r_rx_ip 239.168.77.21 &
pid_rx=$!

echo "${name}: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
sleep ${TEST_TIME_SEC}
kill -SIGINT ${pid_tx}
kill -SIGINT ${pid_rx}

echo "${name}: wait all thread ending"
wait
echo "${name}: ****** Done ******"
echo ""