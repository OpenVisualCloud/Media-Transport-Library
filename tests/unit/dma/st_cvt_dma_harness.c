/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#define _GNU_SOURCE /* RTLD_NEXT, before any system header */
#include "dma/st_cvt_dma_harness.h"

#include <dlfcn.h>
#include <rte_dev.h>
#include <rte_dmadev.h>
#include <stdbool.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_log.h"
#include "mt_util.h"

/* A vdev name binds to the driver whose name it starts with. */
#define UT_CVT_DMA_SKELETON "dma_skeleton_ut_cvt"

struct ut_cvt_dma_skeleton {
  struct mt_dma_dev dev;
};

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

ut_cvt_dma_skeleton* ut_cvt_dma_skeleton_create(void) {
  if (ut_eal_init() < 0) return NULL;
  /* The skeleton dereferences each IOVA it is given. */
  if (rte_eal_iova_mode() != RTE_IOVA_VA) {
    err("%s, IOVA mode is not VA\n", __func__);
    return NULL;
  }
  int ret = rte_eal_hotplug_add("vdev", UT_CVT_DMA_SKELETON, "");
  if (ret < 0) {
    err("%s, add %s fail %d\n", __func__, UT_CVT_DMA_SKELETON, ret);
    return NULL;
  }

  struct rte_dma_conf conf = {.nb_vchans = 1};
  struct rte_dma_vchan_conf vconf = {.direction = RTE_DMA_DIR_MEM_TO_MEM, .nb_desc = 32};
  int16_t dev_id = (int16_t)rte_dma_get_dev_id_by_name(UT_CVT_DMA_SKELETON);
  ut_cvt_dma_skeleton* skel = calloc(1, sizeof(*skel));
  const char* step = NULL;
  if (dev_id < 0)
    step = "get the dev id";
  else if (!skel)
    step = "calloc";
  else if (rte_dma_configure(dev_id, &conf) < 0)
    step = "configure";
  else if (rte_dma_vchan_setup(dev_id, 0, &vconf) < 0)
    step = "vchan setup";
  else if (rte_dma_start(dev_id) < 0)
    step = "start";
  if (step) {
    err("%s, %s fail\n", __func__, step);
    free(skel);
    rte_eal_hotplug_remove("vdev", UT_CVT_DMA_SKELETON);
    return NULL;
  }

  skel->dev.dev_id = dev_id;
  skel->dev.soc_id = (int)rte_socket_id();
  skel->dev.lenders[0].parent = &skel->dev;
  return skel;
}

int ut_cvt_dma_skeleton_destroy(ut_cvt_dma_skeleton* skel) {
  if (!skel) return 0;
  int stop = rte_dma_stop(skel->dev.dev_id);
  int remove = rte_eal_hotplug_remove("vdev", UT_CVT_DMA_SKELETON);
  free(skel);
  return stop < 0 ? stop : remove;
}

mtl_udma_handle ut_cvt_dma_skeleton_handle(ut_cvt_dma_skeleton* skel) {
  return &skel->dev.lenders[0];
}

uint64_t ut_cvt_dma_skeleton_completed(ut_cvt_dma_skeleton* skel) {
  struct rte_dma_stats stats;
  if (rte_dma_stats_get(skel->dev.dev_id, 0, &stats) < 0) return UINT64_MAX;
  return stats.completed;
}
