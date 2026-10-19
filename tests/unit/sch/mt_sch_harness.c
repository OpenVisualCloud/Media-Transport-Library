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
#include "mt_main.h"
#include "mt_platform.h"
#include "mt_usdt.h"
/* the fake clock once a test starts it */
static uint64_t ut_sch_tsc(struct mtl_main_impl* impl) {
  return ut_clock_on() ? ut_clock_tick() : mt_get_tsc(impl);
}
#define mt_get_tsc ut_sch_tsc
/* Only the filelock helper swaps this: the host lock may be held or keep its old mode. */
static const char* ut_sch_flock_path = MT_FLOCK_PATH;
#undef MT_FLOCK_PATH
#define MT_FLOCK_PATH ut_sch_flock_path
/* The semaphore of sys:tasklet_time_sample, which a probe attach raises. */
static unsigned short ut_sch_time_sample_semaphore;
#undef MT_USDT_TASKLET_TIME_SAMPLE_ENABLED
#define MT_USDT_TASKLET_TIME_SAMPLE_ENABLED() (ut_sch_time_sample_semaphore != 0)
/* sys:tasklet_stat, always attached: what it last handed a collector. */
static struct ut_sch_tasklet_stat ut_sch_tasklet_stat_last;
#undef MT_USDT_SYS_TASKLET_STAT_ENABLED
#define MT_USDT_SYS_TASKLET_STAT_ENABLED() (1)
#undef MT_USDT_SYS_TASKLET_STAT
#define MT_USDT_SYS_TASKLET_STAT(sch_idx, name, cnt, sum, work_cnt, work_sum) \
  ut_sch_tasklet_stat_last = (struct ut_sch_tasklet_stat) {                   \
    sch_idx, name, cnt, sum, work_cnt, work_sum                               \
  }
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

struct ut_sch_stats_ctx {
  struct mtl_main_impl impl; /* first, the handle is its address */
  struct mt_sch_tasklet_impl filler;
  struct mt_sch_tasklet_impl* slots[3];
  mtl_tasklet_handle tasklet;
  mtl_tasklet_handle idle;
  unsigned int work_every;
  uint64_t loops;
  uint64_t calls;
  uint64_t stop_at;
};

static int ut_sch_filler_handler(void* priv) {
  struct ut_sch_stats_ctx* ctx = priv;

  ut_clock_advance(UT_SCH_FILLER_NS);
  if (++ctx->loops == 4 * ctx->stop_at)
    rte_atomic32_set(&ctx->filler.sch->request_stop, 1);
  return MTL_TASKLET_ALL_DONE;
}

static int ut_sch_stats_handler(void* priv) {
  struct ut_sch_stats_ctx* ctx = priv;
  uint64_t call = ++ctx->calls;
  bool work = ctx->work_every && call % ctx->work_every == 0;

  ut_clock_advance(work ? UT_SCH_WORK_NS : UT_SCH_IDLE_NS);
  if (call == ctx->stop_at) rte_atomic32_set(&ctx->filler.sch->request_stop, 1);
  return work ? MTL_TASKLET_HAS_PENDING : MTL_TASKLET_ALL_DONE;
}

mtl_handle ut_sch_stats_main_create(uint64_t flags) {
  struct ut_sch_stats_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  struct mtl_main_impl* impl = &ctx->impl;
  impl->type = MT_HANDLE_MAIN;
  impl->user_para.flags = flags;
  struct mtl_sch_impl* sch = mt_sch_instance(impl, UT_SCH_STATS_SCH);
  sch->idx = UT_SCH_STATS_SCH;
  sch->parent = impl;
  sch->tasklet = ctx->slots;
  sch->nb_tasklets = RTE_DIM(ctx->slots);
  mt_stat_u64_init(&sch->stat_time);

  ctx->filler.ops =
      (struct mtl_tasklet_ops){.priv = ctx, .handler = ut_sch_filler_handler};
  (void)snprintf(ctx->filler.name, sizeof(ctx->filler.name), "ut_filler");
  ctx->filler.sch = sch;
  mt_stat_u64_init(&ctx->filler.stat_time);
  ctx->slots[0] = &ctx->filler;
  sch->max_tasklet_idx = 1;

  /* a decoy for sch 0, so a lookup that ignores the index reads zeros instead of NULL */
  if (sch_tasklet_stats_alloc(0, sch->nb_tasklets) < 0 ||
      sch_tasklet_stats_alloc(UT_SCH_STATS_SCH, sch->nb_tasklets) < 0) {
    sch_tasklet_stats_free(0);
    free(ctx);
    return NULL;
  }
  if (mt_pthread_mutex_init(&sch->mutex, NULL)) {
    sch_tasklet_stats_free(UT_SCH_STATS_SCH);
    sch_tasklet_stats_free(0);
    free(ctx);
    return NULL;
  }
  rte_atomic32_set(&sch->active, 1);
  ut_sch_tasklet_stat_last = (struct ut_sch_tasklet_stat){0};
  return impl;
}

void ut_sch_stats_main_destroy(mtl_handle mt) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  if (!ctx) return;
  if (ctx->tasklet) mtl_sch_unregister_tasklet(ctx->tasklet);
  if (ctx->idle) mtl_sch_unregister_tasklet(ctx->idle);
  mt_pthread_mutex_destroy(&mt_sch_instance(&ctx->impl, UT_SCH_STATS_SCH)->mutex);
  sch_tasklet_stats_free(UT_SCH_STATS_SCH);
  sch_tasklet_stats_free(0);
  free(ctx);
}

int ut_sch_stats_register(mtl_handle mt, unsigned int work_every) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  struct mtl_tasklet_ops ops = {
      .name = "ut_tasklet", .priv = ctx, .handler = ut_sch_stats_handler};

  if (ctx->tasklet) {
    int ret = mtl_sch_unregister_tasklet(ctx->tasklet);
    if (ret < 0) return ret;
  }
  ctx->work_every = work_every;
  ctx->tasklet = mtl_sch_register_tasklet(mt_sch_instance(mt, UT_SCH_STATS_SCH), &ops);
  return ctx->tasklet ? 0 : -ENOMEM;
}

int ut_sch_stats_register_idle(mtl_handle mt) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  struct mtl_tasklet_ops ops = {.name = "ut_idle", .handler = ut_sch_idle_handler};

  ctx->idle = mtl_sch_register_tasklet(mt_sch_instance(mt, UT_SCH_STATS_SCH), &ops);
  return ctx->idle ? 0 : -ENOMEM;
}

int ut_sch_stats_run(mtl_handle mt, uint64_t calls) {
  struct ut_sch_stats_ctx* ctx = (struct ut_sch_stats_ctx*)mt;
  struct mtl_sch_impl* sch = mt_sch_instance(mt, UT_SCH_STATS_SCH);

  ctx->stop_at = calls;
  ut_clock_start(0, 0);
  int ret = -pthread_create(&sch->tid, NULL, sch_tasklet_thread, sch);
  if (!ret) ret = -pthread_join(sch->tid, NULL);
  ut_clock_stop();
  if (!ret && ctx->calls != calls) ret = -EIO;
  return ret;
}

int ut_sch_stats_dump(mtl_handle mt) {
  return sch_stat(mt_sch_instance(mt, UT_SCH_STATS_SCH));
}

uint32_t ut_sch_time_sample_gap(uint32_t* state) {
  return sch_time_sample_gap(state);
}

struct ut_sch_tasklet_stat ut_sch_stats_counter(int slot) {
  struct mt_sch_tasklet_stat* ts = &g_sch_tasklet_stats[UT_SCH_STATS_SCH].slots[slot];

  return (struct ut_sch_tasklet_stat){.cnt = ts->cnt,
                                      .sum = ts->sum_ns,
                                      .work_cnt = ts->work_cnt,
                                      .work_sum = ts->work_sum_ns};
}

struct ut_sch_tasklet_stat ut_sch_stats_probe(void) {
  return ut_sch_tasklet_stat_last;
}

void ut_sch_stats_sample(bool on) {
  if (on)
    ut_sch_time_sample_semaphore++;
  else
    ut_sch_time_sample_semaphore--;
}

uint64_t ut_sch_stats_dump_loop_cnt(mtl_handle mt) {
  return mt_sch_instance(mt, UT_SCH_STATS_SCH)->stat_time.cnt;
}

uint64_t ut_sch_stats_dump_tasklet_cnt(mtl_handle mt) {
  return ((struct ut_sch_stats_ctx*)mt)->tasklet->stat_time.cnt;
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
