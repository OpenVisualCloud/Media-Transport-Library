#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

ST_PORT_TX=0000:af:01.1
ST_PORT_RX=0000:af:01.0
TEST_TIME_SEC=30

TEST_ST20=true
TEST_ST20P=true
TEST_ST22P=true
TEST_ST20_SLICE=true
TEST_ST20_RTP=true

export KAHAWAI_CFG_PATH=../../kahawai.json

if [ $TEST_ST20 == "true" ]; then
	echo "St20: start tx"
	ST_PORT_P=${ST_PORT_TX} ../../build/app/TxVideoSample > /dev/null 2>&1 &
	pid_tx=$!
	echo "St20: start rx"
	ST_PORT_P=${ST_PORT_RX} ../../build/app/RxVideoSample &
	pid_rx=$!
	echo "St20: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	echo "St20: wait all thread ending"
	wait
fi

if [ $TEST_ST20P == "true" ]; then
	echo "St20p: start tx"
	ST_PORT_P=${ST_PORT_TX} ../../build/app/TxSt20PipelineSample > /dev/null 2>&1 &
	pid_tx=$!
	echo "St20p: start rx"
	ST_PORT_P=${ST_PORT_RX} ../../build/app/RxSt20PipelineSample &
	pid_rx=$!
	echo "St20p: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	echo "St20p: wait all thread ending"
wait
fi

if [ $TEST_ST22P == "true" ]; then
	echo "St22p: start tx"
	ST_PORT_P=${ST_PORT_TX} ../../build/app/TxSt22PipelineSample > /dev/null 2>&1 &
	pid_tx=$!
	echo "St22p: start rx"
	ST_PORT_P=${ST_PORT_RX} ../../build/app/RxSt22PipelineSample &
	pid_rx=$!
	echo "St22p: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	echo "St22p: wait all thread ending"
wait
fi

if [ $TEST_ST20_SLICE == "true" ]; then
	echo "St20_slice: start tx"
	ST_PORT_P=${ST_PORT_TX} ../../build/app/TxSliceVideoSample > /dev/null 2>&1 &
	pid_tx=$!
	echo "St20_slice: start rx"
	ST_PORT_P=${ST_PORT_RX} ../../build/app/RxSliceVideoSample &
	pid_rx=$!
	echo "St20_slice: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	echo "St20_slice: wait all thread ending"
	wait
fi

if [ $TEST_ST20_RTP == "true" ]; then
	echo "St20_rtp: start tx"
	ST_PORT_P=${ST_PORT_TX} ../../build/app/TxRtpVideoSample > /dev/null 2>&1 &
	pid_tx=$!
	echo "St20_rtp: start rx"
	ST_PORT_P=${ST_PORT_RX} ../../build/app/RxRtpVideoSample &
	pid_rx=$!
	echo "St20_rtp: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	echo "St20_rtp: wait all thread ending"
	wait
fi

unset KAHAWAI_CFG_PATH