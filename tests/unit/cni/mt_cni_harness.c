/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Includes the production mt_cni.c so the static cni_rx_handle() is callable.
 */

#include "cni/mt_cni_harness.h"

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_cni.c"

struct ut_cni_ctx {
  struct mtl_main_impl impl;
  struct mt_cni_entry cni;
};

ut_cni_ctx* ut_cni_create(void) {
  if (ut_eal_init() < 0) return NULL;
  ut_cni_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->cni.impl = &ctx->impl;
  ctx->cni.port = MTL_PORT_P;
  MT_TAILQ_INIT(&ctx->cni.csq_queues);
  rte_spinlock_init(&ctx->cni.csq_lock);
  MT_TAILQ_INIT(&ctx->cni.udp_detect);
  return ctx;
}

void ut_cni_destroy(ut_cni_ctx* ctx) {
  free(ctx);
}

int ut_cni_rx(ut_cni_ctx* ctx, const uint8_t* frame, size_t len) {
  uint8_t* data = ut_guarded_copy(frame, len);
  struct rte_mbuf m;
  memset(&m, 0, sizeof(m));
  m.buf_addr = data;
  m.data_len = len;
  m.pkt_len = len;
  m.nb_segs = 1;
  int ret = cni_rx_handle(&ctx->cni, &m);
  ut_guarded_free(data, len);
  return ret;
}
