/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_STAT_HEAD_H_
#define _MT_LIB_STAT_HEAD_H_

#include "mt_main.h"

int mt_stat_init(struct mtl_main_impl *impl);
int mt_stat_uinit(struct mtl_main_impl *impl);

int mt_stat_register(struct mtl_main_impl *impl, mt_stat_cb_t cb, void *priv, char *name);
int mt_stat_unregister(struct mtl_main_impl *impl, mt_stat_cb_t cb, void *priv);

static inline uint64_t mt_stat_dump_period_us(struct mtl_main_impl *impl) {
  return impl->stat_mgr.dump_period_us;
}

static inline double mt_stat_dump_period_s(struct mtl_main_impl *impl) {
  return (double)mt_stat_dump_period_us(impl) / US_PER_S;
}

#endif
