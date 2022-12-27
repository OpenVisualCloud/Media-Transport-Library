/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_RSS_HEAD_H_
#define _MT_LIB_RSS_HEAD_H_

#include "mt_main.h"

int mt_rss_init(struct mtl_main_impl* impl);
int mt_rss_uinit(struct mtl_main_impl* impl);

struct mt_rss_entry* mt_rss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rss_flow* flow);
static inline uint16_t mt_rss_queue_id(struct mt_rss_entry* entry) {
  return entry->queue_id;
}
uint16_t mt_rss_burst(struct mt_rss_entry* entry, uint16_t nb_pkts);
int mt_rss_put(struct mt_rss_entry* entry);

#endif
