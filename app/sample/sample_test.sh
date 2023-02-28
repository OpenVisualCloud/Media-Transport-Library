#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

ST_PORT_TX=0000:af:01.1
ST_PORT_RX=0000:af:01.0
ST_SIP_TX=192.168.87.80
ST_SIP_RX=192.168.87.81
ST_TX_IP=239.168.87.20
ST_RX_IP=239.168.87.20
TEST_TIME_SEC=15

TEST_BIN_PATH=../../build/app
LOG_LEVEL=notice

export KAHAWAI_CFG_PATH=../../kahawai.json

test_tx_and_rx() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local width=1920
	if [ -n "$4" ];  then
	  width=$4
	fi
	local height=1080
	if [ -n "$5" ];  then
	  height=$5
	fi
	echo "${name}: start ${tx_prog}, width:${width} height:${height}"
	"${TEST_BIN_PATH}"/"${tx_prog}" --log_level "${LOG_LEVEL}" --p_port "${ST_PORT_TX}" --p_sip "${ST_SIP_TX}" --p_tx_ip "${ST_TX_IP}" --width "${width}" --height "${height}" &
	#${TEST_BIN_PATH}/${tx_prog} > /dev/null --log_level ${LOG_LEVEL} --p_port ${ST_PORT_TX} --p_sip ${ST_SIP_TX} --p_tx_ip ${ST_TX_IP} --width ${width} --height ${height} 2>&1 &
	pid_tx=$!
	echo "${name}: start ${rx_prog}"
	"${TEST_BIN_PATH}"/"${rx_prog}" --log_level "${LOG_LEVEL}" --p_port "${ST_PORT_RX}" --p_sip "${ST_SIP_RX}" --p_rx_ip "${ST_RX_IP}" --width "${width}" --height "${height}" &
	pid_rx=$!
	echo "${name}: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}

	echo "${name}: wait all thread ending"
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	wait ${pid_tx}
	echo "${name}: ${tx_prog} exit"
	wait ${pid_rx}
	echo "${name}: ${rx_prog} exit"
	echo "${name}: ****** Done ******"
	echo ""
}

# test video
test_tx_and_rx st20 TxVideoSample RxVideoSample
# test st20p
test_tx_and_rx st20p TxSt20PipelineSample RxSt20PipelineSample
# test st22p
test_tx_and_rx st22p TxSt22PipelineSample RxSt22PipelineSample

# test rtp video
test_tx_and_rx st20_rtp TxRtpVideoSample RxRtpVideoSample
# test slice video
test_tx_and_rx st20_slice TxSliceVideoSample RxSliceVideoSample
# test st22
test_tx_and_rx st22 TxSt22VideoSample RxSt22VideoSample

# test RxSt20TxSt20Fwd
test_tx_and_rx RxSt20TxSt20Fwd TxSt20PipelineSample RxSt20TxSt20Fwd
# test RxSt20pTxSt20pFwd
test_tx_and_rx RxSt20pTxSt20pFwd TxSt20PipelineSample RxSt20pTxSt20pFwd
# test RxSt20pTxSt22pFwd
test_tx_and_rx RxSt20pTxSt22pFwd TxSt20PipelineSample RxSt20pTxSt22pFwd

# test TxVideoSplitSample
test_tx_and_rx TxVideoSplitSample TxVideoSplitSample RxSt20PipelineSample
# test RxSt20TxSt20SplitFwd, not enable now
test_tx_and_rx RxSt20TxSt20SplitFwd TxSt20PipelineSample RxSt20TxSt20SplitFwd 3840 2160

echo "****** All test OK ******"

unset KAHAWAI_CFG_PATH