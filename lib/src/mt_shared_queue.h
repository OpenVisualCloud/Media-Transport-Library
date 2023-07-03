/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SQ_HEAD_H_
#define _MT_LIB_SQ_HEAD_H_

#include "mt_main.h"

int mt_rsq_init(struct mtl_main_impl* impl);
int mt_rsq_uinit(struct mtl_main_impl* impl);

struct mt_rsq_entry* mt_rsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow);
static inline uint16_t mt_rsq_queue_id(struct mt_rsq_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_rsq_burst(struct mt_rsq_entry* entry, struct rte_mbuf** rx_pkts,
                      uint16_t nb_pkts);
int mt_rsq_put(struct mt_rsq_entry* entry);

int mt_tsq_init(struct mtl_main_impl* impl);
int mt_tsq_uinit(struct mtl_main_impl* impl);

struct mt_tsq_entry* mt_tsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_txq_flow* flow);
static inline uint16_t mt_tsq_queue_id(struct mt_tsq_entry* entry) {
  return entry->queue_id;
}
static inline struct rte_mempool* mt_tsq_mempool(struct mt_tsq_entry* entry) {
  return entry->tx_pool;
}
uint16_t mt_tsq_burst(struct mt_tsq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts);
uint16_t mt_tsq_burst_busy(struct mtl_main_impl* impl, struct mt_tsq_entry* entry,
                           struct rte_mbuf** tx_pkts, uint16_t nb_pkts, int timeout_ms);
int mt_tsq_flush(struct mtl_main_impl* impl, struct mt_tsq_entry* entry,
                 struct rte_mbuf* pad);
int mt_tsq_put(struct mt_tsq_entry* entry);

#endif
