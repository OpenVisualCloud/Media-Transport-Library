/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_CNI_HEAD_H_
#define _MT_LIB_CNI_HEAD_H_

#include "mt_main.h"

#define ST_CNI_RX_BURST_SIZE (32)

int mt_cni_init(struct mtl_main_impl *impl);
int mt_cni_uinit(struct mtl_main_impl *impl);
int mt_cni_start(struct mtl_main_impl *impl);
int mt_cni_stop(struct mtl_main_impl *impl);

static inline struct mt_cni_impl *mt_get_cni(struct mtl_main_impl *impl) {
  return &impl->cni;
}

struct mt_csq_entry *mt_csq_get(struct mtl_main_impl *impl, enum mtl_port port,
                                struct mt_rxq_flow *flow);
static inline uint16_t mt_csq_queue_id(struct mt_csq_entry *entry) {
  return entry->idx;
}
uint16_t mt_csq_burst(struct mt_csq_entry *entry, struct rte_mbuf **rx_pkts,
                      uint16_t nb_pkts);
int mt_csq_put(struct mt_csq_entry *entry);

#endif
