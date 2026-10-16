/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the scheduler unit tests. Includes the production mt_sch.c so the static
 * scheduler start/stop and thread entries are reachable; its non-static symbols preempt
 * libmtl's for the whole UnitTest binary.
 */

#include "sch/mt_sch_harness.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_platform.h"
/* Only the filelock helper swaps this: the host lock may be held or keep its old mode. */
static const char* ut_sch_flock_path = MT_FLOCK_PATH;
#undef MT_FLOCK_PATH
#define MT_FLOCK_PATH ut_sch_flock_path
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

/* Two more slots than the API reports: one empty, one that overflows tasklets[]. */
#define UT_SCH_STATS_SLOTS (MTL_SCH_MAX_TASKLETS + 2)

struct ut_sch_stats_ctx {
  struct mtl_main_impl impl; /* first, the handle is its address */
  struct mt_sch_tasklet_impl tasklet;
  struct mt_sch_tasklet_impl* tasklets[UT_SCH_STATS_SLOTS];
  struct mt_sch_tasklet_impl idle[UT_SCH_STATS_SLOTS];
  unsigned int spin_us;
  unsigned int work_every;
  _Atomic uint64_t calls;
  mtl_tasklet_handle registered;
};

static int ut_sch_spin_handler(void* priv) {
  struct ut_sch_stats_ctx* ctx = priv;
  uint64_t call = atomic_load_explicit(&ctx->calls, memory_order_relaxed) + 1;
  bool work = ctx->work_every && call % ctx->work_every == 0;

  if (work || !ctx->work_every) {
    uint64_t end = mt_get_tsc(&ctx->impl) + (uint64_t)ctx->spin_us * NS_PER_US;
    while (mt_get_tsc(&ctx->impl) < end) {
    }
  }
  atomic_store_explicit(&ctx->calls, call, memory_order_relaxed);
  return work ? MTL_TASKLET_HAS_PENDING : MTL_TASKLET_ALL_DONE;
}

mtl_handle ut_sch_stats_main_create(uint64_t flags, unsigned int spin_us,
                                    unsigned int work_every) {
  struct ut_sch_stats_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  struct mtl_main_impl* impl = &ctx->impl;
  impl->type = MT_HANDLE_MAIN;
  impl->user_para.flags = flags;
  impl->tsc_hz = rte_get_tsc_hz();
  ctx->spin_us = spin_us;
  ctx->work_every = work_every;

  ctx->tasklet.ops.handler = ut_sch_spin_handler;
  ctx->tasklet.ops.priv = ctx;
  snprintf(ctx->tasklet.name, sizeof(ctx->tasklet.name), "%s", UT_SCH_SPIN_TASKLET_NAME);
  ctx->tasklet.idx = 1;
  mt_stat_u64_init(&ctx->tasklet.stat_time);
  ctx->tasklets[1] = &ctx->tasklet;

  struct mtl_sch_impl* sch = mt_sch_instance(impl, 0);
  snprintf(sch->name, sizeof(sch->name), "ut_sch");
  sch->parent = impl;
  ctx->tasklet.sch = sch;
  sch->tasklet = ctx->tasklets;
  sch->nb_tasklets = UT_SCH_STATS_SLOTS;
  sch->max_tasklet_idx = 2;
  sch->run_in_thread = true;
  mt_stat_u64_init(&sch->stat_time);
  if (sch_user_stats_alloc(0, UT_SCH_STATS_SLOTS) < 0) {
    free(ctx);
    return NULL;
  }
  if (mt_pthread_mutex_init(&sch->mutex, NULL)) {
    sch_user_stats_free(0);
    free(ctx);
    return NULL;
  }
  rte_atomic32_set(&sch->active, 1);
  return impl;
}

void ut_sch_stats_main_destroy(mtl_handle mt) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  if (!ctx) return;
  if (ctx->registered) mtl_sch_unregister_tasklet(ctx->registered);
  mt_pthread_mutex_destroy(&mt_sch_instance(&ctx->impl, 0)->mutex);
  sch_user_stats_free(0);
  free(ctx);
}

int ut_sch_stats_add_idle_tasklets(mtl_handle mt, int count) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  struct mtl_sch_impl* sch = mt_sch_instance(&ctx->impl, 0);

  for (int slot = sch->max_tasklet_idx; count > 0; slot++, count--) {
    if (slot >= UT_SCH_STATS_SLOTS) return -ENOSPC;
    struct mt_sch_tasklet_impl* tasklet = &ctx->idle[slot];
    tasklet->ops.handler = ut_sch_idle_handler;
    snprintf(tasklet->name, sizeof(tasklet->name), "ut_idle_%d", slot);
    tasklet->sch = sch;
    tasklet->idx = slot;
    mt_stat_u64_init(&tasklet->stat_time);
    ctx->tasklets[slot] = tasklet;
    sch->max_tasklet_idx = slot + 1;
  }
  return 0;
}

static int ut_sch_busy_handler(void* priv) {
  MTL_MAY_UNUSED(priv);
  return MTL_TASKLET_HAS_PENDING;
}

int ut_sch_stats_register_busy(mtl_handle mt) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  struct mtl_tasklet_ops ops = {.name = "ut_registered", .handler = ut_sch_busy_handler};

  if (ctx->registered) {
    int ret = mtl_sch_unregister_tasklet(ctx->registered);
    if (ret < 0) return ret;
  }
  ctx->registered = mtl_sch_register_tasklet(mt_sch_instance(mt, 0), &ops);
  return ctx->registered ? 0 : -ENOMEM;
}

void ut_sch_stats_drop_tasklets(mtl_handle mt) {
  mt_sch_instance(mt, 0)->tasklet = NULL;
}

int ut_sch_stats_start(mtl_handle mt) {
  return sch_start(mt_sch_instance(mt, 0));
}

int ut_sch_stats_stop(mtl_handle mt) {
  return sch_stop(mt_sch_instance(mt, 0));
}

int ut_sch_stats_wait_calls(mtl_handle mt, uint64_t calls) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;

  for (int i = 0; i < 2000; i++) {
    if (atomic_load_explicit(&ctx->calls, memory_order_relaxed) >= calls) return 0;
    mt_sleep_ms(1);
  }
  return -ETIMEDOUT;
}

int ut_sch_stats_dump(mtl_handle mt) {
  return sch_stat(mt_sch_instance(mt, 0));
}

uint64_t ut_sch_stats_calls(mtl_handle mt) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;

  return atomic_load_explicit(&ctx->calls, memory_order_relaxed);
}

uint64_t ut_sch_stats_dump_loop_cnt(mtl_handle mt) {
  return mt_sch_instance(mt, 0)->stat_time.cnt;
}

uint64_t ut_sch_stats_dump_tasklet_cnt(mtl_handle mt) {
  return ((struct ut_sch_stats_ctx*)mt)->tasklet.stat_time.cnt;
}

int ut_sch_filelock_create_mode(unsigned int* mode) {
  char dir[] = "/tmp/ut_sch_flock_XXXXXX";
  if (!mkdtemp(dir)) return -errno;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/lock", dir);

  int ret = -ENOMEM;
  struct mt_sch_mgr* mgr = calloc(1, sizeof(*mgr));
  if (mgr) {
    mgr->lcore_lock_fd = -1;
    const char* old_path = ut_sch_flock_path;
    ut_sch_flock_path = path;
    mode_t old_mask = umask(0);
    ret = sch_filelock_lock(mgr);
    umask(old_mask);
    ut_sch_flock_path = old_path;
  }
  if (!ret) {
    struct stat st;
    if (fstat(mgr->lcore_lock_fd, &st) < 0)
      ret = -errno;
    else
      *mode = st.st_mode & 07777;
    if (sch_filelock_unlock(mgr) < 0 && !ret) ret = -EIO;
  }
  unlink(path);
  free(mgr);
  rmdir(dir);
  return ret;
}
