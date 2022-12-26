/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SQ_HEAD_H_
#define _MT_LIB_SQ_HEAD_H_

#include "mt_main.h"

int mt_sq_init(struct mtl_main_impl* impl);
int mt_sq_uinit(struct mtl_main_impl* impl);

struct mt_sq_entry* mt_sq_get(struct mtl_main_impl* impl, enum mtl_port port,
                              struct mt_sq_flow* flow);
static inline uint16_t mt_sq_queue_id(struct mt_sq_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_sq_rx_burst(struct mt_sq_entry* entry, uint16_t nb_pkts);
int mt_sq_put(struct mt_sq_entry* entry);

#endif
