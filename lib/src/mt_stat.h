/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_STAT_HEAD_H_
#define _MT_LIB_STAT_HEAD_H_

#include "mt_main.h"

int mt_stat_init(struct mtl_main_impl* impl);
int mt_stat_uinit(struct mtl_main_impl* impl);
int mt_stat_dump(struct mtl_main_impl* impl);

int mt_stat_register(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv);
int mt_stat_unregister(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv);

#endif
