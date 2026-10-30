/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the scheduler unit tests. Includes the production mt_sch.c so the static
 * scheduler start/stop and thread entries are reachable; its non-static symbols preempt
 * libmtl's for the whole UnitTest binary.
 */

#define _GNU_SOURCE /* CPU_SETSIZE needs it before any libc header */
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
static const char* ut_sch_shm_path = "/dev/null";
#define ftok(path, id) ftok(ut_sch_shm_path, id)
#include "mt_sch.c"
#undef ftok

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

static char ut_sch_shm_file[PATH_MAX];
static void* ut_sch_shm_creator;
static struct mt_lcore_mgr ut_sch_shm_mgr = {.lcore_shm_id = -1};

/* A key that is not the host's and has no segment yet, so teardown removes only ours. */
int ut_sch_shm_private_begin(void) {
  key_t host_key = ftok("/dev/null", 21);

  for (int try = 0; try < 16; try++) {
    snprintf(ut_sch_shm_file, sizeof(ut_sch_shm_file), "/tmp/ut_sch_shm_XXXXXX");
    int fd = mkstemp(ut_sch_shm_file);
    if (fd < 0) return -errno;
    close(fd);
    key_t key = ftok(ut_sch_shm_file, 21);
    if (key != -1 && key != host_key && shmget(key, 0, 0) < 0 && errno == ENOENT) {
      ut_sch_shm_path = ut_sch_shm_file;
      return 0;
    }
    unlink(ut_sch_shm_file);
  }
  return -EEXIST;
}

void ut_sch_shm_private_end(void) {
  if (ut_sch_shm_mgr.lcore_shm) sch_lcore_shm_uinit(&ut_sch_shm_mgr);
  if (ut_sch_shm_creator) shmdt(ut_sch_shm_creator);
  ut_sch_shm_creator = NULL;
  if (ut_sch_shm_path == ut_sch_shm_file) {
    int id = shmget(ftok(ut_sch_shm_path, 21), 0, 0666);
    if (id >= 0) shmctl(id, IPC_RMID, NULL);
    unlink(ut_sch_shm_file);
  }
  ut_sch_shm_path = "/dev/null";
}

long ut_sch_shm_table_size(unsigned int entries) {
  return (long)(offsetof(struct mt_lcore_shm, lcores_info) +
                entries * sizeof(struct mt_lcore_shm_entry));
}

long ut_sch_shm_size(void) {
  int id = shmget(ftok(ut_sch_shm_path, 21), 0, 0666);
  if (id < 0) return -errno;
  struct shmid_ds stat;
  if (shmctl(id, IPC_STAT, &stat) < 0) return -errno;
  return (long)stat.shm_segsz;
}

int ut_sch_shm_create(long bytes, bool attached) {
  int id = shmget(ftok(ut_sch_shm_path, 21), bytes, 0666 | IPC_CREAT | IPC_EXCL);
  if (id < 0) return -errno;
  void* shm = shmat(id, NULL, 0);
  if (shm == (void*)-1) return -errno;
  memset(shm, 0, bytes);
  if (attached)
    ut_sch_shm_creator = shm;
  else
    shmdt(shm);
  return 0;
}

int ut_sch_shm_mark_active(unsigned int lcore, int pid) {
  int id = shmget(ftok(ut_sch_shm_path, 21), 0, 0666);
  if (id < 0) return -errno;
  struct shmid_ds stat;
  if (shmctl(id, IPC_STAT, &stat) < 0) return -errno;
  if ((long)stat.shm_segsz < ut_sch_shm_table_size(lcore + 1)) return -ERANGE;
  struct mt_lcore_shm* shm = shmat(id, NULL, 0);
  if (shm == (void*)-1) return -errno;
  shm->lcores_info[lcore].active = true;
  shm->lcores_info[lcore].pid = pid;
  shm->used++;
  shmdt(shm);
  return 0;
}

long ut_sch_shm_join(bool first_user) {
  int ret = sch_lcore_shm_init(&ut_sch_shm_mgr, first_user);
  if (ret < 0) return ret;
  struct shmid_ds stat;
  if (shmctl(ut_sch_shm_mgr.lcore_shm_id, IPC_STAT, &stat) < 0) return -errno;
  return (long)stat.shm_segsz;
}
