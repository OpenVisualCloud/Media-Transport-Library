/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_CNI_HEAD_H_
#define _ST_LIB_CNI_HEAD_H_

#include "st_main.h"

#define ST_CNI_RX_BURST_SIZE (32)

int st_cni_init(struct st_main_impl* impl);
int st_cni_uinit(struct st_main_impl* impl);
int st_cni_start(struct st_main_impl* impl);
int st_cni_stop(struct st_main_impl* impl);

static inline struct st_cni_impl* st_get_cni(struct st_main_impl* impl) {
  return &impl->cni;
}

void st_cni_stat(struct st_main_impl* impl);

#endif
