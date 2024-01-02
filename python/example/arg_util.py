# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation
# argparse utils

import argparse


def parse_args(is_tx):
    parser = argparse.ArgumentParser(description="Argument util for MTL example")
    if is_tx:
        p_port_default = "0000:af:01.1"
    else:
        p_port_default = "0000:af:01.0"
    parser.add_argument(
        "--p_port", type=str, default=p_port_default, help="primary port name"
    )
    if is_tx:
        p_sip_default = "192.168.108.101"
    else:
        p_sip_default = "192.168.108.102"
    parser.add_argument(
        "--p_sip", type=str, default=p_sip_default, help="primary local IP address"
    )
    # p_tx_ip
    parser.add_argument(
        "--p_tx_ip",
        type=str,
        default="239.168.85.20",
        help="primary TX dest IP address",
    )
    # p_rx_ip
    parser.add_argument(
        "--p_rx_ip",
        type=str,
        default="239.168.85.20",
        help="primary RX source IP address",
    )
    return parser.parse_args()
