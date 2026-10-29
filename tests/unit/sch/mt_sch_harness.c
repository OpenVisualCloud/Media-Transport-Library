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
#include <rte_pause.h>

#include "common/ut_common.h"
#include "mt_main.h"
#include "mt_platform.h"
/* Only the filelock and lcore helpers swap this: the host lock may be held or keep its
 * old mode. */
static const char* ut_sch_flock_path = MT_FLOCK_PATH;
#undef MT_FLOCK_PATH
#define MT_FLOCK_PATH ut_sch_flock_path
/* Seams: each poll of mt_sch_quiescent_wait(), spinning or sleeping, and the scheduler's
 * short sleeps. */
static void ut_sch_qs_pause(void);
static void ut_sch_qs_sleep_us(unsigned int us);
static void ut_sch_sleep_ms(unsigned int ms);
/* Seams: the worker lcores mt_sch_get_lcore() walks, and a bounded flock() so a lock left
 * held fails a get instead of hanging the binary. */
static unsigned int ut_sch_next_lcore(unsigned int i, int skip_main, int wrap);
static int ut_sch_flock(int fd, int op);
#define rte_pause ut_sch_qs_pause
#define mt_sleep_us ut_sch_qs_sleep_us
#define mt_sleep_ms ut_sch_sleep_ms
#define rte_get_next_lcore ut_sch_next_lcore
#define flock ut_sch_flock
#include "mt_sch.c"
#undef flock
#undef rte_get_next_lcore
#undef mt_sleep_ms
#undef mt_sleep_us
#undef rte_pause

static atomic_int ut_sch_qs_polls;
/* when set, a poll marks it offline, so a wait that should not poll cannot hang */
static struct mtl_sch_impl* ut_sch_qs_unblock;

static void ut_sch_qs_poll(void) {
  atomic_fetch_add(&ut_sch_qs_polls, 1);
  if (ut_sch_qs_unblock) sch_qs_offline(ut_sch_qs_unblock);
}

static void ut_sch_qs_pause(void) {
  ut_sch_qs_poll();
  rte_pause();
}

static void ut_sch_qs_sleep_us(unsigned int us) {
  ut_sch_qs_poll();
  mt_sleep_us(us);
}

static struct mtl_sch_impl* ut_sch_qs_sleeper; /* whose own sleeps are counted */
static atomic_int ut_sch_qs_sleeps;
static atomic_int ut_sch_qs_online_sleeps;

static void ut_sch_sleep_ms(unsigned int ms) {
  struct mtl_sch_impl* sch = ut_sch_qs_sleeper;

  if (sch && rte_gettid() == sch->t_pid) {
    if (atomic_load(&sch->qs_seen) != UINT64_MAX)
      atomic_fetch_add(&ut_sch_qs_online_sleeps, 1);
    atomic_fetch_add(&ut_sch_qs_sleeps, 1);
  }
  mt_sleep_ms(ms);
}

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

  snprintf(m->dir, sizeof(m->dir), "/tmp/ut_sch_lcore_XXXXXX");
  if (!mkdtemp(m->dir)) {
    free(m);
    return NULL;
  }
  snprintf(m->path, sizeof(m->path), "%s/lock", m->dir);

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

static struct ut_sch_mempolicy_ctx* ut_sch_qs_ctx_create(int (*handler)(void* priv),
                                                         void* priv) {
  int mode;
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_mempolicy_ctx_create(&mode);
  if (!ctx) return NULL;

  ctx->tasklet.ops.start = NULL;
  ctx->tasklet.ops.handler = handler;
  ctx->tasklet.ops.priv = priv;
  return ctx;
}

/* Spins below are bounded by 1 s, so a scheduler that stops running fails a case. */
static uint64_t ut_sch_qs_deadline(void) {
  return rte_get_tsc_cycles() + rte_get_tsc_hz();
}

static bool ut_sch_qs_before(uint64_t deadline) {
  return rte_get_tsc_cycles() < deadline;
}

static int ut_sch_qs_wait_unblocked(struct mtl_sch_impl* sch) {
  atomic_store(&ut_sch_qs_polls, 0);
  ut_sch_qs_unblock = sch;
  mt_sch_quiescent_wait(sch, 0);
  ut_sch_qs_unblock = NULL;
  return atomic_load(&ut_sch_qs_polls);
}

int ut_sch_qs_wait_after_stop(int* polls) {
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_qs_ctx_create(ut_sch_idle_handler, NULL);
  if (!ctx) return -ENOMEM;

  int ret = sch_start(&ctx->sch);
  if (!ret) ret = sch_stop(&ctx->sch);
  if (!ret) *polls = ut_sch_qs_wait_unblocked(&ctx->sch);
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}

struct ut_sch_qs_self {
  struct mtl_sch_impl* sch;
  int polls;
  atomic_bool done;
};

static int ut_sch_qs_self_handler(void* priv) {
  struct ut_sch_qs_self* self = priv;

  if (!atomic_load(&self->done)) {
    self->polls = ut_sch_qs_wait_unblocked(self->sch);
    atomic_store(&self->done, true);
  }
  return MTL_TASKLET_ALL_DONE;
}

int ut_sch_qs_wait_on_sch_thread(int* polls) {
  struct ut_sch_qs_self self = {0};
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_qs_ctx_create(ut_sch_qs_self_handler, &self);
  if (!ctx) return -ENOMEM;

  self.sch = &ctx->sch;
  int ret = sch_start(&ctx->sch);
  if (!ret) {
    uint64_t deadline = ut_sch_qs_deadline();
    while (!atomic_load(&self.done) && ut_sch_qs_before(deadline)) rte_pause();
    if (!atomic_load(&self.done)) ret = -ETIMEDOUT;
    int stop = sch_stop(&ctx->sch);
    if (!ret) ret = stop;
  }
  *polls = self.polls;
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}

struct ut_sch_qs_waiter {
  struct mtl_sch_impl* sch;
  atomic_bool done;
};

static void* ut_sch_qs_waiter_thread(void* arg) {
  struct ut_sch_qs_waiter* waiter = arg;

  mt_sch_quiescent_wait(waiter->sch, 0);
  atomic_store(&waiter->done, true);
  return NULL;
}

/* polls a correct wait cannot make between the end of the pass and its return */
#define UT_SCH_QS_POLLS_AFTER_END (10000)

int ut_sch_qs_wait_across_pass(enum ut_sch_qs_pass_end end, bool* returned_in_pass,
                               bool* returned_after_end) {
  struct mtl_sch_impl* sch = calloc(1, sizeof(*sch));
  struct ut_sch_qs_waiter waiter = {.sch = sch};
  pthread_t thread;
  if (!sch) return -ENOMEM;

  sch_qs_online(sch);
  atomic_store(&ut_sch_qs_polls, 0);
  int ret = -pthread_create(&thread, NULL, ut_sch_qs_waiter_thread, &waiter);
  if (!ret) {
    uint64_t deadline = ut_sch_qs_deadline();
    while (!atomic_load(&ut_sch_qs_polls) && !atomic_load(&waiter.done) &&
           ut_sch_qs_before(deadline))
      rte_pause();
    if (!atomic_load(&ut_sch_qs_polls) && !atomic_load(&waiter.done)) ret = -ETIMEDOUT;
    *returned_in_pass = atomic_load(&waiter.done);
    if (end == UT_SCH_QS_END_OF_PASS)
      sch_qs_pass_end(sch);
    else
      sch_qs_offline(sch);
    int polls_at_end = atomic_load(&ut_sch_qs_polls);
    while (!atomic_load(&waiter.done) &&
           atomic_load(&ut_sch_qs_polls) - polls_at_end < UT_SCH_QS_POLLS_AFTER_END)
      rte_pause();
    *returned_after_end = atomic_load(&waiter.done);
    sch_qs_offline(sch); /* lets a broken wait return */
    int join = -pthread_join(thread, NULL);
    if (!ret) ret = join;
  }
  free(sch);
  return ret;
}

int ut_sch_qs_sleeps_offline(int* sleeps, int* online_sleeps) {
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_qs_ctx_create(ut_sch_idle_handler, NULL);
  if (!ctx) return -ENOMEM;

  ctx->sch.allow_sleep = true;
  /* a sleep below the threshold is the mt_sleep_ms(0) yield */
  ctx->impl.var_para.sch_zero_sleep_threshold_us = 1;
  atomic_store(&ut_sch_qs_sleeps, 0);
  atomic_store(&ut_sch_qs_online_sleeps, 0);
  ut_sch_qs_sleeper = &ctx->sch;
  int ret = sch_start(&ctx->sch);
  if (!ret) {
    uint64_t deadline = ut_sch_qs_deadline();
    while (atomic_load(&ut_sch_qs_sleeps) < 3 && ut_sch_qs_before(deadline)) rte_pause();
    int stop = sch_stop(&ctx->sch);
    ret = atomic_load(&ut_sch_qs_sleeps) < 3 ? -ETIMEDOUT : stop;
  }
  ut_sch_qs_sleeper = NULL;
  *sleeps = atomic_load(&ut_sch_qs_sleeps);
  *online_sleeps = atomic_load(&ut_sch_qs_online_sleeps);
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}

enum ut_sch_qs_step {
  UT_SCH_QS_HOLD,  /* hold the next pass until a wait began */
  UT_SCH_QS_HELD,  /* holding it */
  UT_SCH_QS_CHECK, /* in the pass after: did the held one's end satisfy the wait */
  UT_SCH_QS_DONE,
};

struct ut_sch_qs_pass {
  struct mtl_sch_impl* sch;
  atomic_int step;
  bool published;
};

static int ut_sch_qs_pass_handler(void* priv) {
  struct ut_sch_qs_pass* pass = priv;
  struct mtl_sch_impl* sch = pass->sch;
  int step = atomic_load(&pass->step);

  if (step == UT_SCH_QS_HOLD) {
    uint64_t token = atomic_load(&sch->qs_token);
    atomic_store(&pass->step, UT_SCH_QS_HELD);
    while (atomic_load(&sch->qs_token) == token) rte_pause();
    atomic_store(&pass->step, UT_SCH_QS_CHECK);
  } else if (step == UT_SCH_QS_CHECK) {
    pass->published = atomic_load(&sch->qs_seen) >= atomic_load(&sch->qs_token);
    if (!pass->published) sch_qs_offline(sch); /* release the waiter */
    atomic_store(&pass->step, UT_SCH_QS_DONE);
  }
  return MTL_TASKLET_ALL_DONE;
}

int ut_sch_qs_pass_end_publishes(bool* published) {
  struct ut_sch_qs_pass pass = {0};
  struct ut_sch_mempolicy_ctx* ctx = ut_sch_qs_ctx_create(ut_sch_qs_pass_handler, &pass);
  if (!ctx) return -ENOMEM;

  pass.sch = &ctx->sch;
  int ret = sch_start(&ctx->sch);
  if (!ret) {
    uint64_t deadline = ut_sch_qs_deadline();
    while (atomic_load(&pass.step) != UT_SCH_QS_HELD && ut_sch_qs_before(deadline))
      rte_pause();
    if (atomic_load(&pass.step) == UT_SCH_QS_HELD)
      mt_sch_quiescent_wait(&ctx->sch, US_PER_S);
    deadline = ut_sch_qs_deadline();
    while (atomic_load(&pass.step) != UT_SCH_QS_DONE && ut_sch_qs_before(deadline))
      rte_pause();
    if (atomic_load(&pass.step) != UT_SCH_QS_DONE) {
      ret = -ETIMEDOUT;
      atomic_fetch_add(&ctx->sch.qs_token, 1); /* releases a held pass */
    }
    int stop = sch_stop(&ctx->sch);
    if (!ret) ret = stop;
  }
  *published = pass.published;
  ut_sch_mempolicy_ctx_destroy(ctx);
  return ret;
}
