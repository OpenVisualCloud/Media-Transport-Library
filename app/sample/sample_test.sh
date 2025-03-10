#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022-2025 Intel Corporation

SCRIPT_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
REPOSITORY_DIR="$(readlink -f "${SCRIPT_DIR}/../..")"

# shellcheck source="script/common.sh"
. "${REPOSITORY_DIR}/script/common.sh"

ST_PORT_TX="${ST_PORT_TX:-'0000:af:01.1'}"
ST_PORT_RX="${ST_PORT_RX:-'0000:af:01.0'}"
ST_SIP_TX="${ST_SIP_TX:-'192.168.87.80'}"
ST_SIP_RX="${ST_SIP_RX:-'192.168.87.81'}"
ST_TX_IP="${ST_TX_IP:-'239.168.87.20'}"
ST_RX_IP="${ST_RX_IP:-'239.168.87.20'}"
TEST_TIME_SEC="${TEST_TIME_SEC:-15}"

TEST_BIN_PATH="${TEST_BIN_PATH:-"${REPOSITORY_DIR}/build/app"}"
LOG_LEVEL="${LOG_LEVEL:-notice}"

test_tx_and_rx() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local width=1920
	if [ -n "$4" ]; then
		width=$4
	fi
	local height=1080
	if [ -n "$5" ]; then
		height=$5
	fi
	log_info "${name}: start ${tx_prog}, width:${width} height:${height}"
	"${TEST_BIN_PATH}"/"${tx_prog}" --log_level "${LOG_LEVEL}" --p_port "${ST_PORT_TX}" --p_sip "${ST_SIP_TX}" --p_tx_ip "${ST_TX_IP}" --width "${width}" --height "${height}" &
	#${TEST_BIN_PATH}/${tx_prog} > /dev/null --log_level ${LOG_LEVEL} --p_port ${ST_PORT_TX} --p_sip ${ST_SIP_TX} --p_tx_ip ${ST_TX_IP} --width ${width} --height ${height} 2>&1 &
	pid_tx=$!
	log_info "${name}: start ${rx_prog}"
	"${TEST_BIN_PATH}"/"${rx_prog}" --log_level "${LOG_LEVEL}" --p_port "${ST_PORT_RX}" --p_sip "${ST_SIP_RX}" --p_rx_ip "${ST_RX_IP}" --width "${width}" --height "${height}" &
	pid_rx=$!
	log_info "${name}: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
	sleep "${TEST_TIME_SEC}"

	log_info "${name}: wait all thread ending"
	kill -SIGINT ${pid_tx}
	kill -SIGINT ${pid_rx}
	wait ${pid_tx}
	log_info "${name}: ${tx_prog} exit"
	wait ${pid_rx}
	log_info "${name}: ${rx_prog} exit"
	log_success "${name}: ****** Done ******"
	log_info ""
}

# Allow sourcing of the script.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	set -e -o pipefail
	export KAHAWAI_CFG_PATH="${REPOSITORY_DIR}/kahawai.json"

	print_logo_anim
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

	log_success "****** All test OK ******"
	unset KAHAWAI_CFG_PATH
fi
