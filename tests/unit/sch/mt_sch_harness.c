/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the scheduler unit tests. Includes the production mt_sch.c so the static
 * scheduler start/stop and thread entries are reachable; its non-static symbols preempt
 * libmtl's for the whole UnitTest binary.
 */

#include "sch/mt_sch_harness.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_sch.c"

int ut_sch_init(void) {
  return ut_eal_init();
}

mtl_handle ut_sch_create_main(void) {
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  if (!impl) return NULL;

  impl->type = MT_HANDLE_MAIN;
  return impl;
}

void ut_sch_destroy_main(mtl_handle mt) {
  free(mt);
}

unsigned int ut_sch_max_lcore(void) {
  return RTE_MAX_LCORE;
}

int ut_sch_max_sch_num(void) {
  return MT_MAX_SCH_NUM;
}

/* Both out-of-range sch indices must land on poisoned bytes of this allocation rather
 * than on the allocator's, so the test measures the missing bound check. */
struct ut_sch_probe_main {
  struct mtl_sch_impl before;
  struct mtl_main_impl impl;
  struct mtl_sch_impl after;
};

mtl_handle ut_sch_create_probe_main(void) {
  struct ut_sch_probe_main* probe = malloc(sizeof(*probe));
  if (!probe) return NULL;

  memset(probe, 0xFF, sizeof(*probe));
  struct mtl_main_impl* impl = &probe->impl;
  impl->type = MT_HANDLE_MAIN;

  struct mtl_sch_impl* sch = mt_sch_instance(impl, MT_MAX_SCH_NUM - 1);
  memset(sch, 0, sizeof(*sch));
  sch->idx = MT_MAX_SCH_NUM - 1;
  rte_atomic32_set(&sch->active, 1);
  return impl;
}

void ut_sch_destroy_probe_main(mtl_handle mt) {
  if (!mt) return;
  free((char*)mt - offsetof(struct ut_sch_probe_main, impl));
}

struct ut_sch_mempolicy_ctx {
  struct mtl_main_impl impl;
  struct mtl_sch_impl sch;
  struct mt_sch_tasklet_impl tasklet;
  struct mt_sch_tasklet_impl* tasklets[1];
  int exit_mode;
};

static int ut_sch_record_mempolicy(void* priv) {
  int* mode = priv;
  if (get_mempolicy(mode, NULL, 0, NULL, 0) < 0) *mode = -errno;
  return 0;
}

static int ut_sch_idle_handler(void* priv) {
  MTL_MAY_UNUSED(priv);
  return MTL_TASKLET_ALL_DONE;
}

static struct ut_sch_mempolicy_ctx* ut_sch_mempolicy_ctx_create(int* mode) {
  struct ut_sch_mempolicy_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->tasklet.ops.start = ut_sch_record_mempolicy;
  ctx->tasklet.ops.handler = ut_sch_idle_handler;
  ctx->tasklet.ops.priv = mode;
  ctx->tasklets[0] = &ctx->tasklet;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->sch.parent = &ctx->impl;
  ctx->sch.tasklet = ctx->tasklets;
  ctx->sch.max_tasklet_idx = 1;
  ctx->sch.run_in_thread = true;
  if (mt_pthread_mutex_init(&ctx->sch.mutex, NULL)) {
    free(ctx);
    return NULL;
  }
  *mode = -1;
  ctx->exit_mode = -1;
  return ctx;
}

static void ut_sch_mempolicy_ctx_destroy(struct ut_sch_mempolicy_ctx* ctx) {
  mt_pthread_mutex_destroy(&ctx->sch.mutex);
  free(ctx);
}

int ut_sch_thread_mode_mempolicy(int* mode) {
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_mempolicy_ctx_create(mode);
  if (!ctx) return -ENOMEM;

  int ret = sch_start(&ctx->sch);
  if (!ret) ret = sch_stop(&ctx->sch);
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}

static void* ut_sch_lcore_entry(void* arg) {
  struct ut_sch_mempolicy_ctx* ctx = arg;
  sch_tasklet_lcore(&ctx->sch);
  ut_sch_record_mempolicy(&ctx->exit_mode);
  return NULL;
}

int ut_sch_lcore_entry_mempolicy(int* mode, int* exit_mode) {
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_mempolicy_ctx_create(mode);
  pthread_t thread;
  if (!ctx) return -ENOMEM;

  rte_atomic32_set(&ctx->sch.request_stop, 1);
  int ret = -pthread_create(&thread, NULL, ut_sch_lcore_entry, ctx);
  if (!ret) ret = -pthread_join(thread, NULL);
  *exit_mode = ctx->exit_mode;
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}
