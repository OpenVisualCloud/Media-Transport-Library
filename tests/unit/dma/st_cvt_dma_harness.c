/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#define _GNU_SOURCE /* RTLD_NEXT, before any system header */
#include "dma/st_cvt_dma_harness.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_util.h"

static bool ut_ctx_fail;
static int ut_ctx_failed;

/* Preempts libmtl's: fails inside ut_cvt_dma_le10_to_be10(), else calls libmtl's. */
struct mt_cvt_dma_ctx* mt_cvt_dma_ctx_init(int fifo_size, int soc_id, int type_num) {
  if (ut_ctx_fail) {
    ut_ctx_failed++;
    return NULL;
  }

  struct mt_cvt_dma_ctx* (*real)(int, int, int) = dlsym(RTLD_NEXT, "mt_cvt_dma_ctx_init");
  if (!real) abort();
  return real(fifo_size, soc_id, type_num);
}

int ut_cvt_dma_le10_to_be10(struct st20_rfc4175_422_10_pg2_le* pg_le,
                            struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
                            uint32_t h, enum mtl_simd_level level) {
  if (ut_eal_init() < 0) return -EIO;

  struct mt_dma_dev dev = {.soc_id = (int)rte_socket_id()};
  struct mtl_dma_lender_dev lender = {.parent = &dev};

  ut_ctx_failed = 0;
  ut_ctx_fail = true;
  int ret =
      st20_rfc4175_422le10_to_422be10_simd_dma(&lender, pg_le, 0, pg_be, w, h, level);
  ut_ctx_fail = false;
  return ret;
}

int ut_cvt_dma_failed_ctx_inits(void) {
  return ut_ctx_failed;
}
