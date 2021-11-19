/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
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
