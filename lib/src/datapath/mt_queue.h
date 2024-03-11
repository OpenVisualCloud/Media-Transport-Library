/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "../mt_main.h"

#ifndef _MT_LIB_DP_QUEUE_HEAD_H_
#define _MT_LIB_DP_QUEUE_HEAD_H_

int mt_dp_queue_init(struct mtl_main_impl* impl);
int mt_dp_queue_uinit(struct mtl_main_impl* impl);

struct mt_rxq_entry {
  struct mtl_main_impl* parent;
  uint16_t queue_id;
  struct mt_rx_queue* rxq;
  struct mt_rsq_entry* rsq;
  struct mt_srss_entry* srss;
  struct mt_csq_entry* csq;
  struct mt_rx_socket_entry* rx_socket_q;
  struct mt_rx_xdp_entry* rx_xdp_q;

  uint16_t (*burst)(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                    const uint16_t nb_pkts);
};

struct mt_rxq_entry* mt_rxq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow);
static inline uint16_t mt_rxq_queue_id(struct mt_rxq_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                      const uint16_t nb_pkts);
int mt_rxq_put(struct mt_rxq_entry* entry);

struct mt_txq_entry {
  struct mtl_main_impl* parent;
  uint16_t queue_id;
  struct mt_tx_queue* txq;
  struct mt_tsq_entry* tsq;
  struct mt_tx_socket_entry* tx_socket_q;
  struct mt_tx_xdp_entry* tx_xdp_q;

  uint16_t (*burst)(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                    uint16_t nb_pkts);
};

struct mt_txq_entry* mt_txq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_txq_flow* flow);
static inline uint16_t mt_txq_queue_id(struct mt_txq_entry* entry) {
  return entry->queue_id;
}
static inline struct rte_mempool* mt_txq_mempool(struct mt_txq_entry* entry) {
  if (entry->tsq)
    return entry->tsq->tx_pool;
  else
    return NULL; /* only for shared queue */
}
uint16_t mt_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts);
uint16_t mt_txq_burst_busy(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                           uint16_t nb_pkts, int timeout_ms);
int mt_txq_flush(struct mt_txq_entry* entry, struct rte_mbuf* pad);
int mt_txq_put(struct mt_txq_entry* entry);
int mt_txq_fatal_error(struct mt_txq_entry* entry);
int mt_txq_done_cleanup(struct mt_txq_entry* entry);
int mt_txq_set_tx_bps(struct mt_txq_entry* entry, uint64_t bytes_per_sec);

uint16_t mt_sys_queue_tx_burst(struct mtl_main_impl* impl, enum mtl_port port,
                               struct rte_mbuf** tx_pkts, uint16_t nb_pkts);

#endif
