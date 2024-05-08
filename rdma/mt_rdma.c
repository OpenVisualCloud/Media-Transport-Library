/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mt_rdma.h"

#include <stdlib.h>

mtl_rdma_handle mtl_rdma_init(struct mtl_rdma_init_params* p) {
  struct mt_rdma_impl* impl = NULL;
  impl = calloc(1, sizeof(*impl));
  if (!impl) {
    return NULL;
  }
  impl->init = 1;
  return impl;
}

int mtl_rdma_uinit(mtl_rdma_handle mrh) {
  struct mt_rdma_impl* impl = (struct mt_rdma_impl*)mrh;
  free(impl);
  return 0;
}