/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mt_rdma.h"

#include <stdlib.h>

static enum mtl_rdma_log_level rdma_log_level = MTL_RDMA_LOG_LEVEL_INFO;

void mt_rdma_set_log_level(enum mtl_rdma_log_level level) {
  rdma_log_level = level;
}
enum mtl_rdma_log_level mt_rdma_get_log_level(void) {
  return rdma_log_level;
}

mtl_rdma_handle mtl_rdma_init(struct mtl_rdma_init_params* p) {
  struct mt_rdma_impl* impl = NULL;
  impl = calloc(1, sizeof(*impl));
  if (!impl) {
    err("%s, failed to allocate memory for mtl_rdma_impl\n", __func__);
    return NULL;
  }
  impl->init = 1;
  mt_rdma_set_log_level(p->log_level);
  return impl;
}

int mtl_rdma_uinit(mtl_rdma_handle mrh) {
  struct mt_rdma_impl* impl = (struct mt_rdma_impl*)mrh;
  free(impl);
  return 0;
}