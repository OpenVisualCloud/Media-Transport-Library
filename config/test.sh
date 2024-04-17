#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

set -e

ST_PORT_TX=0000:af:01.1
ST_PORT_RX=0000:af:01.0
TX_SEC=20
RX_SEC=15

RXTXAPP=./build/app/RxTxApp
LOG_LEVEL=error #notice

test_tx_rx_config() {
    $RXTXAPP --config_file "$1" --p_port $ST_PORT_TX --test_time $TX_SEC --log_level $LOG_LEVEL &
    pid_tx=$!
    $RXTXAPP --config_file "$2" --p_port $ST_PORT_RX --test_time $RX_SEC --log_level $LOG_LEVEL
    echo "wait $1 finish"
    wait ${pid_tx}
    echo "pass with tx: $1, rx: $2"
    echo ""
}

test_tx_rx_config config/tx_1v.json config/rx_1v.json
test_tx_rx_config config/tx_1v_1a_1anc.json config/rx_1v_1a_1anc.json
test_tx_rx_config config/tx_2v2dest_1a_1anc.json config/rx_2v2dest_1a_1anc.json
# redundant
test_tx_rx_config config/redundant_tx_1v_1a_1anc.json config/redundant_rx_1v_1a_1anc.json
test_tx_rx_config config/redundant_tx_2v_1a_1anc.json config/redundant_rx_2v_1a_1anc.json

echo "****** All test OK ******"
