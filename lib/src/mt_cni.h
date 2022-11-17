/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_CNI_HEAD_H_
#define _MT_LIB_CNI_HEAD_H_

#include "mt_main.h"

#define ST_CNI_RX_BURST_SIZE (32)

int st_cni_init(struct mtl_main_impl* impl);
int st_cni_uinit(struct mtl_main_impl* impl);
int st_cni_start(struct mtl_main_impl* impl);
int st_cni_stop(struct mtl_main_impl* impl);

static inline struct st_cni_impl* st_get_cni(struct mtl_main_impl* impl) {
  return &impl->cni;
}

void st_cni_stat(struct mtl_main_impl* impl);

#endif
