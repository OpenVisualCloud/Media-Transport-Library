/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_SHARED_RSS_HEAD_H_
#define _MT_LIB_SHARED_RSS_HEAD_H_

#include "../mt_main.h"

int mt_srss_init(struct mtl_main_impl *impl);

int mt_srss_uinit(struct mtl_main_impl *impl);

struct mt_srss_entry *mt_srss_get(struct mtl_main_impl *impl,
                                  enum mtl_port port, struct mt_rxq_flow *flow);
static inline uint16_t mt_srss_queue_id(struct mt_srss_entry *entry) {
  return entry->idx; /* use dummy id*/
}
static inline uint16_t mt_srss_burst(struct mt_srss_entry *entry,
                                     struct rte_mbuf **rx_pkts,
                                     const uint16_t nb_pkts) {
  uint16_t n =
      rte_ring_sc_dequeue_burst(entry->ring, (void **)rx_pkts, nb_pkts, NULL);
  entry->stat_dequeue_cnt += n;
  return n;
}
int mt_srss_put(struct mt_srss_entry *entry);

#endif