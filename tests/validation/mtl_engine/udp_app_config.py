# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

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
