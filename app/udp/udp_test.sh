#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

ST_PORT_TX=0000:af:01.1
ST_PORT_RX=0000:af:01.0
ST_SIP_TX=192.168.85.60
ST_SIP_RX=192.168.85.80
ST_TX_IP=${ST_SIP_RX}
ST_RX_IP=${ST_SIP_TX}
ST_MCAST_IP=239.168.85.80
TEST_TIME_SEC=15
SESSIONS_CNT=2

#UFD cfg
MUFD_RX_CFG=ufd_server.json
MUFD_TX_CFG=ufd_client.json

TEST_BIN_PATH=../../build/app
LOG_LEVEL=notice

export KAHAWAI_CFG_PATH=../../kahawai.json

test_udp() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local udp_mode=$4

	echo "${name}: start ${tx_prog}"
	${TEST_BIN_PATH}/${tx_prog} --log_level ${LOG_LEVEL} --p_port ${ST_PORT_TX} --p_sip ${ST_SIP_TX} --p_tx_ip ${ST_TX_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
	pid_tx=$!
	echo "${name}: start ${rx_prog}"
	${TEST_BIN_PATH}/${rx_prog} --log_level ${LOG_LEVEL} --p_port ${ST_PORT_RX} --p_sip ${ST_SIP_RX} --p_rx_ip ${ST_RX_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
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

test_ufd() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local udp_mode=$4

	echo "${name}: start ${tx_prog}"
	MUFD_CFG=${MUFD_TX_CFG} ${TEST_BIN_PATH}/${tx_prog} --log_level ${LOG_LEVEL} --p_tx_ip ${ST_TX_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
	pid_tx=$!
	echo "${name}: start ${rx_prog}"
	MUFD_CFG=${MUFD_RX_CFG} ${TEST_BIN_PATH}/${rx_prog} --log_level ${LOG_LEVEL} --p_rx_ip ${ST_RX_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
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

test_ufd_mcast() {
	local name=$1
	local tx_prog=$2
	local rx_prog=$3
	local udp_mode=$4

	echo "${name}: start ${tx_prog}"
	MUFD_CFG=${MUFD_TX_CFG} ${TEST_BIN_PATH}/${tx_prog} --log_level ${LOG_LEVEL} --p_tx_ip ${ST_MCAST_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
	pid_tx=$!
	echo "${name}: start ${rx_prog}"
	MUFD_CFG=${MUFD_RX_CFG} ${TEST_BIN_PATH}/${rx_prog} --log_level ${LOG_LEVEL} --p_rx_ip ${ST_MCAST_IP} --udp_mode ${udp_mode} --sessions_cnt ${SESSIONS_CNT} &
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

# test udp
test_udp udp_default UdpClientSample UdpServerSample default
test_udp udp_transport UdpClientSample UdpServerSample transport
test_udp udp_transport_poll UdpClientSample UdpServerSample transport_poll
test_udp udp_transport_unify_poll UdpClientSample UdpServerSample transport_unify_poll

# test ufd
test_ufd ufd_default UfdClientSample UfdServerSample default
test_ufd ufd_transport UfdClientSample UfdServerSample transport
test_ufd ufd_transport_poll UfdClientSample UfdServerSample transport_poll
test_ufd ufd_transport_unify_poll UfdClientSample UfdServerSample transport_unify_poll

# test ufd mcast
test_ufd_mcast ufd_mcast UfdClientSample UfdServerSample transport_poll

echo "****** All test OK ******"

unset KAHAWAI_CFG_PATH