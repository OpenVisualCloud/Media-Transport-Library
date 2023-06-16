/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_dev.h"
#include "mt_rss.h"
#include "mt_shared_queue.h"
#include "mt_shared_rss.h"

#ifndef _MT_LIB_QUEUE_HEAD_H_
#define _MT_LIB_QUEUE_HEAD_H_

struct mt_txq_entry {
  uint16_t queue_id;
};

struct mt_rxq_entry {
  struct mtl_main_impl* parent;
  uint16_t queue_id;
  struct mt_rx_queue* rxq;
  struct mt_rsq_entry* rsq;
  struct mt_rss_entry* rss;
  struct mt_srss_entry* srss;
};

struct mt_rxq_entry* mt_rxq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow);
static inline uint16_t mt_rxq_queue_id(struct mt_rxq_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                      const uint16_t nb_pkts);
int mt_rxq_put(struct mt_rxq_entry* entry);

#if 0
struct mt_txq_entry* mt_txq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_txq_flow* flow);
static inline uint16_t mt_txq_queue_id(struct mt_txq_entry* entry) {
  return entry->queue_id;
}
static inline struct rte_mempool* mt_txq_mempool(struct mt_txq_entry* entry) {
  return entry->tx_pool;
}
int mt_txq_set_bps(struct mtl_main_impl* impl, struct mt_txq_entry* entry,
                   uint64_t bytes_per_sec);
uint16_t mt_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts);
uint16_t mt_txq_burst_busy(struct mtl_main_impl* impl, struct mt_txq_entry* entry,
                           struct rte_mbuf** tx_pkts, uint16_t nb_pkts, int timeout_ms);
int mt_txq_flush(struct mtl_main_impl* impl, struct mt_txq_entry* entry,
                 struct rte_mbuf* pad);
int mt_txq_put(struct mt_txq_entry* entry);
#endif

#endif
