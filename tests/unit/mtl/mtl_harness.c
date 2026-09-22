/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtl/mtl_harness.h"

#include <stdlib.h>

#include "mt_main.h"

struct ut_mtl_ctx {
  struct mtl_main_impl impl;
};

ut_mtl_ctx* ut_mtl_create_ctx(int num_ports) {
  ut_mtl_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.user_para.num_ports = num_ports;
  return ctx;
}

void ut_mtl_destroy_ctx(ut_mtl_ctx* ctx) {
  free(ctx);
}

mtl_handle ut_mtl_handle(ut_mtl_ctx* ctx) {
  return &ctx->impl;
}

void ut_mtl_set_hw_rx_timestamp(ut_mtl_ctx* ctx, enum mtl_port port, bool active) {
  if (active)
    ctx->impl.inf[port].feature |= MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;
  else
    ctx->impl.inf[port].feature &= ~MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;
}

void ut_mtl_set_invalid_handle_type(ut_mtl_ctx* ctx) {
  ctx->impl.type = 0;
}
