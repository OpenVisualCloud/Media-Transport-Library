#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

SCRIPT_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
REPOSITORY_DIR="$(readlink -f "${SCRIPT_DIR}/../..")"

# shellcheck source="script/common.sh"
. "${REPOSITORY_DIR}/script/common.sh"

print_logo_anim
TEST_TIME_SEC="${TEST_TIME_SEC:-15}"
TEST_BIN_PATH="${REPOSITORY_DIR}/build/app"
LOG_LEVEL=notice

name=RxSt20RedundantSample

"${TEST_BIN_PATH}/TxSt20PipelineSample" >/dev/null --log_level "${LOG_LEVEL}" --p_port 0000:af:01.2 --r_port 0000:af:01.3 --p_sip 192.168.77.2 --r_sip 192.168.77.3 --p_tx_ip 239.168.77.20 --r_tx_ip 239.168.77.21 2>&1 &
pid_tx=$!
"${TEST_BIN_PATH}/RxSt20RedundantSample" --log_level "${LOG_LEVEL}" --p_port 0000:af:01.0 --r_port 0000:af:01.1 --p_sip 192.168.77.11 --r_sip 192.168.77.12 --p_rx_ip 239.168.77.20 --r_rx_ip 239.168.77.21 &
pid_rx=$!

log_info "${name}: pid_tx ${pid_tx}, pid_rx ${pid_rx}, wait ${TEST_TIME_SEC}s"
sleep "${TEST_TIME_SEC}"
kill -SIGINT "${pid_tx}"
kill -SIGINT "${pid_rx}"

log_info "${name}: wait all thread ending"
wait
log_success "${name}: ****** Done ******"
log_info ""
