# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging

logger = logging.getLogger(__name__)

rx: list[str] = []
rx_multicast: list[str] = []
tx: list[str] = []

# Redundant (secondary port) pools — uses session_id + 1 as subnet
tx_r: list[str] = []
rx_r: list[str] = []


def init(session_id: int, pool_size: int = 8) -> None:
    if pool_size > 128:
        logger.warning(f"Pool size was too big ({pool_size} > 128). Set to 128.")
        pool_size = 128
    for i in range(pool_size):
        host_octet = i + 1
        rx.append(f"192.168.{session_id}.{host_octet}")
        # The second octet must stay non-zero. An IPv4 multicast MAC carries
        # only the low 23 bits of the group, so 239.0.0.x would share
        # 01:00:5e:00:00:x with the 224.0.0.x link-local control groups --
        # 239.0.0.1 aliases all-hosts and is then flooded to every VF on the
        # NIC, landing on the CNI queue of ports that never joined it.
        rx_multicast.append(f"239.1.{session_id}.{host_octet}")
        tx.append(f"192.168.{session_id}.{pool_size + i}")

        # Secondary port for ST2022-7 redundant sessions
        r_subnet = session_id + 1
        tx_r.append(f"192.168.{r_subnet}.{pool_size + i}")
        rx_r.append(f"192.168.{r_subnet}.{host_octet}")
