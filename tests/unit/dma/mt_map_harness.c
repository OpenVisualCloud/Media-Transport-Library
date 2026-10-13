/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "dma/mt_map_harness.h"

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_dma.h"
#include "mt_main.h"

struct ut_map_ctx {
  struct mtl_main_impl impl;
};

ut_map_ctx* ut_map_create(void) {
  if (ut_eal_init() < 0) return NULL;

  ut_map_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  mt_map_init(&ctx->impl);
  return ctx;
}

void ut_map_destroy(ut_map_ctx* ctx) {
  if (!ctx) return;
  mt_map_uinit(&ctx->impl);
  free(ctx);
}

int ut_map_add(ut_map_ctx* ctx, uintptr_t vaddr, size_t size, uint64_t* iova) {
  struct mt_map_item item = {
      .vaddr = (void*)vaddr,
      .size = size,
      .iova = MTL_BAD_IOVA,
  };
  int ret = mt_map_add(&ctx->impl, &item);

  if (!ret) *iova = item.iova;
  return ret;
}
