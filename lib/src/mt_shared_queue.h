/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SQ_HEAD_H_
#define _MT_LIB_SQ_HEAD_H_

#include "mt_main.h"

int mt_rsq_init(struct mtl_main_impl* impl);
int mt_rsq_uinit(struct mtl_main_impl* impl);

struct mt_rsq_entry* mt_rsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rx_flow* flow);
static inline uint16_t mt_rsq_queue_id(struct mt_rsq_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_rsq_burst(struct mt_rsq_entry* entry, uint16_t nb_pkts);
int mt_rsq_put(struct mt_rsq_entry* entry);

#endif
