#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

# json cfg
MUPL_TX_CFG=.github/workflows/upl_tx.json
MUPL_TX_SIP=192.168.89.80 # define in MUPL_TX_CFG
MUPL_RX_CFG=.github/workflows/upl_rx.json
MUPL_RX_SIP=192.168.89.81 # define in MUPL_RX_CFG

MTL_LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so
TEST_BIN_PATH=build/app

TEST_TIME_SEC=120
SESSIONS_CNT=2

test_upl() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local tx_ip=$4
	local rx_ip=$5

	echo "${name}: start ${tx_prog} ${tx_ip}"
	LD_PRELOAD="${MTL_LD_PRELOAD}" MUFD_CFG="${MUPL_TX_CFG}" "${TEST_BIN_PATH}"/"${tx_prog}" --p_sip "${MUPL_TX_SIP}" --p_tx_ip "${tx_ip}" --sessions_cnt "${SESSIONS_CNT}" &
	pid_tx=$!
	echo "${name}: start ${rx_prog} ${rx_ip}"
	LD_PRELOAD="${MTL_LD_PRELOAD}" MUFD_CFG="${MUPL_RX_CFG}" "${TEST_BIN_PATH}"/"${rx_prog}" --p_sip "${MUPL_RX_SIP}" --p_rx_ip "${rx_ip}" --sessions_cnt "${SESSIONS_CNT}" &
	pid_rx=$!
	echo "${name}: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep ${TEST_TIME_SEC}

	echo "${name}: wait all thread ending"
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	wait ${pid_tx}
	echo "${name}: ${tx_prog} exit succ"
	wait ${pid_rx}
	echo "${name}: ${rx_prog} exit succ"
	echo "${name}: ****** Done ******"
	echo ""
}

# test upl
test_upl upl_default UsocketClientSample UsocketServerSample "${MUPL_RX_SIP}" "${MUPL_TX_SIP}"
# test upl multicast
# test_upl upl_default UsocketClientSample UsocketServerSample 239.168.89.88 239.168.89.88

echo "****** All test OK ******"