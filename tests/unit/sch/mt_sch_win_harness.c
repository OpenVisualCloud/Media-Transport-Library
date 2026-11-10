/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Windows C harness for the lcore shm and lock. Includes the production mt_sch.c, which
 * takes shmget(), shmctl() and flock() from the shims of lib/windows in libmtl.dll.
 */

#include "sch/mt_sch_win_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_platform.h"
static const char* ut_sch_win_flock_path = MT_FLOCK_PATH;
#undef MT_FLOCK_PATH
#define MT_FLOCK_PATH ut_sch_win_flock_path
static unsigned int ut_sch_win_next_lcore(unsigned int i, int skip_main, int wrap);
#define rte_get_next_lcore ut_sch_win_next_lcore
#include "mt_sch.c"
#undef rte_get_next_lcore

static unsigned int
    ut_sch_win_nb_lcores; /* when set, lcores 1..n stand in for EAL workers */
static int ut_sch_win_mains;
static const char* ut_sch_win_host_flock_path;
static char ut_sch_win_lock_file[MAX_PATH];

static unsigned int ut_sch_win_next_lcore(unsigned int i, int skip_main, int wrap) {
  if (!ut_sch_win_nb_lcores) return rte_get_next_lcore(i, skip_main, wrap);
  return i < ut_sch_win_nb_lcores ? i + 1 : RTE_MAX_LCORE;
}

struct ut_sch_win_main {
  struct mtl_main_impl impl;
  HANDLE exited_owner; /* kept open, so no new process can take its pid */
};

static int ut_sch_win_seams_set(unsigned int nb_lcores) {
  DWORD len = GetTempPathA(sizeof(ut_sch_win_lock_file), ut_sch_win_lock_file);
  if (!len || len >= sizeof(ut_sch_win_lock_file)) return -EIO;
  (void)snprintf(ut_sch_win_lock_file + len, sizeof(ut_sch_win_lock_file) - len,
                 "ut_sch_win_lcore.lock");
  ut_sch_win_host_flock_path = ut_sch_win_flock_path;
  ut_sch_win_flock_path = ut_sch_win_lock_file;
  ut_sch_win_nb_lcores = nb_lcores;
  return 0;
}

static void ut_sch_win_seams_clear(void) {
  ut_sch_win_nb_lcores = 0;
  ut_sch_win_flock_path = ut_sch_win_host_flock_path;
  SetFileAttributesA(ut_sch_win_lock_file, FILE_ATTRIBUTE_NORMAL);
  DeleteFileA(ut_sch_win_lock_file);
}

static void ut_sch_win_main_free(struct ut_sch_win_main* m) {
  if (m->exited_owner) CloseHandle(m->exited_owner);
  mt_pthread_mutex_destroy(&mt_sch_get_mgr(&m->impl)->lcore_mutex);
  free(m);
  if (!--ut_sch_win_mains) ut_sch_win_seams_clear();
}

mtl_handle ut_sch_win_create_main(unsigned int nb_lcores) {
  if (ut_eal_init() < 0) return NULL;
  struct ut_sch_win_main* m = calloc(1, sizeof(*m));
  if (!m) return NULL;

  struct mtl_main_impl* impl = &m->impl;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  if (mt_pthread_mutex_init(&mgr->lcore_mutex, NULL)) {
    free(m);
    return NULL;
  }
  if (!ut_sch_win_mains && ut_sch_win_seams_set(nb_lcores) < 0) {
    mt_pthread_mutex_destroy(&mgr->lcore_mutex);
    free(m);
    return NULL;
  }
  ut_sch_win_mains++;
  impl->type = MT_HANDLE_MAIN;
  impl->instance_fd = -1;
  impl->user_para.flags = MTL_FLAG_NOT_BIND_NUMA;
  mt_user_info_init(&impl->u_info);
  mgr->lcore_lock_fd = -1;

  if (sch_init_lcores(mgr) < 0) {
    ut_sch_win_main_free(m);
    return NULL;
  }
  return impl;
}

void ut_sch_win_destroy_main(mtl_handle mt) {
  struct ut_sch_win_main* m = (struct ut_sch_win_main*)mt;
  if (!m) return;

  sch_uinit_lcores(&m->impl, mt_sch_get_mgr(&m->impl));
  ut_sch_win_main_free(m);
}

int ut_sch_win_get_lcore(mtl_handle mt, unsigned int* lcore) {
  return mt_sch_get_lcore(mt, lcore, MT_LCORE_TYPE_USER, 0);
}

int ut_sch_win_orphan_lcore(mtl_handle mt, unsigned int lcore) {
  struct ut_sch_win_main* m = (struct ut_sch_win_main*)mt;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(&m->impl);
  char cmd[] = "cmd.exe /c exit 0";
  STARTUPINFOA si = {.cb = sizeof(si)};
  PROCESS_INFORMATION pi;

  if (lcore >= RTE_MAX_LCORE || !mgr->local_lcores_active[lcore] || m->exited_owner)
    return -EINVAL;
  if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si,
                      &pi))
    return -EIO;
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  m->exited_owner = pi.hProcess;

  mgr->lcore_mgr.lcore_shm->lcores_info[lcore].pid = pi.dwProcessId;
  mgr->local_lcores_active[lcore] = false;
  rte_atomic32_dec(&m->impl.lcore_cnt);
  return 0;
}

int ut_sch_win_lock_excludes(mtl_handle mt) {
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(mt);
  if (sch_filelock_lock(mgr) < 0) return -EIO;

  int fd = open(MT_FLOCK_PATH, O_RDONLY);
  int excludes = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK;
  if (fd >= 0) close(fd);
  if (sch_filelock_unlock(mgr) < 0 || fd < 0) return -EIO;
  return excludes;
}
