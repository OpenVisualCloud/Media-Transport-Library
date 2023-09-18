/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_DP_SOCKET_HEAD_H_
#define _MT_LIB_DP_SOCKET_HEAD_H_

#include "../mt_main.h"

struct mt_tx_socket_entry* mt_tx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port, struct mt_txq_flow* flow);
int mt_tx_socket_put(struct mt_tx_socket_entry* entry);
static inline uint16_t mt_tx_socket_queue_id(struct mt_tx_socket_entry* entry) {
  return entry->fd;
}
uint16_t mt_tx_socket_burst(struct mt_tx_socket_entry* entry, struct rte_mbuf** tx_pkts,
                            uint16_t nb_pkts);

struct mt_rx_socket_entry* mt_rx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port, struct mt_rxq_flow* flow);
int mt_rx_socket_put(struct mt_rx_socket_entry* entry);
static inline uint16_t mt_rx_socket_queue_id(struct mt_rx_socket_entry* entry) {
  return entry->fd;
}
uint16_t mt_rx_socket_burst(struct mt_rx_socket_entry* entry, struct rte_mbuf** rx_pkts,
                            const uint16_t nb_pkts);

#endif
