/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_dev.h"
#include "mt_rss.h"
#include "mt_shared_queue.h"
#include "mt_shared_rss.h"

#ifndef _MT_LIB_QUEUE_HEAD_H_
#define _MT_LIB_QUEUE_HEAD_H_

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

#endif
