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
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_platform.h"
/* Only the filelock and lcore helpers swap this: the host lock may be held or keep its
 * old mode. */
static const char* ut_sch_flock_path = MT_FLOCK_PATH;
#undef MT_FLOCK_PATH
#define MT_FLOCK_PATH ut_sch_flock_path
/* Seams: the worker lcores mt_sch_get_lcore() walks, and a bounded flock() so a lock left
 * held fails a get instead of hanging the binary. */
static unsigned int ut_sch_next_lcore(unsigned int i, int skip_main, int wrap);
static int ut_sch_flock(int fd, int op);
#define rte_get_next_lcore ut_sch_next_lcore
#define flock ut_sch_flock
#include "mt_sch.c"
#undef flock
#undef rte_get_next_lcore

static unsigned int ut_sch_nb_lcores; /* when set, lcores 1..n stand in for EAL workers */

static unsigned int ut_sch_next_lcore(unsigned int i, int skip_main, int wrap) {
  if (!ut_sch_nb_lcores) return rte_get_next_lcore(i, skip_main, wrap);
  return i < ut_sch_nb_lcores ? i + 1 : RTE_MAX_LCORE;
}

static int ut_sch_flock(int fd, int op) {
  if (op != LOCK_EX) return flock(fd, op);

  uint64_t deadline = rte_get_tsc_cycles() + rte_get_tsc_hz();
  while (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    if (errno != EWOULDBLOCK || rte_get_tsc_cycles() > deadline) return -1;
    mt_sleep_us(10);
  }
  return 0;
}

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

struct ut_sch_lcore_main {
  struct mtl_main_impl impl;
  struct mt_lcore_shm shm;
  char dir[sizeof("/tmp/ut_sch_lcore_XXXXXX")];
  char path[PATH_MAX];
  const char* old_path;
  enum mtl_log_level old_level;
};

mtl_handle ut_sch_create_lcore_main(unsigned int nb_lcores) {
  struct ut_sch_lcore_main* m = calloc(1, sizeof(*m));
  if (!m) return NULL;

  (void)snprintf(m->dir, sizeof(m->dir), "/tmp/ut_sch_lcore_XXXXXX");
  if (!mkdtemp(m->dir)) {
    free(m);
    return NULL;
  }
  (void)snprintf(m->path, sizeof(m->path), "%s/lock", m->dir);

  struct mtl_main_impl* impl = &m->impl;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  if (mt_pthread_mutex_init(&mgr->lcore_mutex, NULL)) {
    rmdir(m->dir);
    free(m);
    return NULL;
  }
  impl->type = MT_HANDLE_MAIN;
  impl->instance_fd = -1;
  impl->user_para.flags = MTL_FLAG_NOT_BIND_NUMA;
  impl->u_info.pid = getpid();
  mgr->lcore_lock_fd = -1;
  mgr->lcore_mgr.lcore_shm = &m->shm;
  mgr->lcore_mgr.lcore_shm_id = -1;

  m->old_path = ut_sch_flock_path;
  ut_sch_flock_path = m->path;
  ut_sch_nb_lcores = nb_lcores;
  m->old_level = mt_get_log_global_level();
  mt_set_log_global_level(MTL_LOG_LEVEL_ERR);
  return impl;
}

void ut_sch_destroy_lcore_main(mtl_handle mt) {
  struct ut_sch_lcore_main* m = (struct ut_sch_lcore_main*)mt;
  if (!m) return;

  mt_set_log_global_level(m->old_level);
  ut_sch_nb_lcores = 0;
  ut_sch_flock_path = m->old_path;
  unlink(m->path);
  rmdir(m->dir);
  mt_pthread_mutex_destroy(&mt_sch_get_mgr(&m->impl)->lcore_mutex);
  free(m);
}

int ut_sch_lcore_shm_used(mtl_handle mt) {
  return ((struct ut_sch_lcore_main*)mt)->shm.used;
}

int ut_sch_lcore_cnt(mtl_handle mt) {
  return rte_atomic32_read(&((struct mtl_main_impl*)mt)->lcore_cnt);
}
