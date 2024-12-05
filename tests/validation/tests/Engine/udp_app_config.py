# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.

config_client = {
    "interfaces": [
        {
            "port": "",
            "ip": "",
            "netmask": "",
        },
    ],
    "nb_udp_sockets": "64",
    "nb_nic_queues": "8",
    "nic_shared_tx_queues": "false",
    "nic_shared_rx_queues": "false",
    "udp_lcore": "false",
    "rss": "false",
    "nic_queue_rate_limit_g": "2",
    "log_level": "info",
    "rx_ring_count": "1024",
    "rx_poll_sleep_us": "0",
    "wake_thresh_count": "32",
    "wake_timeout_us": "1000",
}

config_server = {
    "interfaces": [
        {
            "port": "",
            "ip": "",
            "netmask": "",
        },
    ],
    "nb_udp_sockets": "64",
    "nb_nic_queues": "8",
    "nic_shared_tx_queues": "false",
    "nic_shared_rx_queues": "false",
    "udp_lcore": "false",
    "rss": "false",
    "nic_queue_rate_limit_g": "2",
    "log_level": "info",
    "rx_ring_count": "1024",
    "rx_poll_sleep_us": "0",
    "wake_thresh_count": "32",
    "wake_timeout_us": "1000",
}

config_librist_send = {
    "interfaces": [
        {
            "port": "",
            "ip": "",
        },
    ],
    "nb_udp_sockets": "64",
    "nb_nic_queues": "64",
    "nic_queue_rate_limit_g": 10,
    "nic_shared_queues": "false",
    "udp_lcore": "false",
    "rx_poll_sleep_us": "10",
    "rx_ring_count": "2048",
    "wake_thresh_count": "32",
}

config_librist_receive = {
    "interfaces": [
        {
            "port": "",
            "ip": "",
        },
    ],
    "nb_udp_sockets": "64",
    "nb_nic_queues": "64",
    "nic_queue_rate_limit_g": 10,
    "nic_shared_queues": "false",
    "udp_lcore": "false",
    "rx_poll_sleep_us": "1",
    "rx_ring_count": "4096",
    "wake_thresh_count": "32",
}
