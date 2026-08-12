/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "ffmpeg/mtl_common_harness.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

static int ut_ffmpeg_lifecycle_mutex_lock(pthread_mutex_t* mutex);

#define MTL_FFMPEG_UNIT_TEST
#define pthread_mutex_lock ut_ffmpeg_lifecycle_mutex_lock
#include "../../../ecosystem/ffmpeg_plugin/mtl_common.c"
#undef pthread_mutex_lock

static struct mtl_init_params ut_init_params;
static int ut_init_calls;
static int ut_uninit_calls;
static int ut_lifecycle_lock_calls;
static mtl_handle ut_handle = (mtl_handle)(uintptr_t)1;
static pthread_mutex_t ut_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ut_init_cond = PTHREAD_COND_INITIALIZER;
static bool ut_block_init;
static bool ut_release_init;

static int ut_ffmpeg_lifecycle_mutex_lock(pthread_mutex_t* mutex) {
  pthread_mutex_lock(&ut_init_mutex);
  ut_lifecycle_lock_calls++;
  pthread_cond_broadcast(&ut_init_cond);
  pthread_mutex_unlock(&ut_init_mutex);
  return pthread_mutex_lock(mutex);
}

void av_log(void* avcl, int level, const char* fmt, ...) {
  (void)avcl;
  (void)level;
  (void)fmt;
}

mtl_handle mtl_init(struct mtl_init_params* p) {
  pthread_mutex_lock(&ut_init_mutex);
  ut_init_params = *p;
  ut_init_calls++;
  pthread_cond_broadcast(&ut_init_cond);
  while (ut_block_init && !ut_release_init && (ut_init_calls == 1))
    pthread_cond_wait(&ut_init_cond, &ut_init_mutex);
  pthread_mutex_unlock(&ut_init_mutex);
  return ut_handle;
}

int mtl_uninit(mtl_handle handle) {
  pthread_mutex_lock(&ut_init_mutex);
  if (handle == ut_handle) ut_uninit_calls++;
  pthread_mutex_unlock(&ut_init_mutex);
  return 0;
}

enum mtl_pmd_type mtl_pmd_by_port_name(const char* port) {
  (void)port;
  return MTL_PMD_DPDK_USER;
}

enum st_fps st_frame_rate_to_st_fps(double fps) {
  (void)fps;
  return ST_FPS_MAX;
}

void ut_ffmpeg_reset(void) {
  g_mtl_shared_handle = NULL;
  g_mtl_ref_cnt = 0;
  memset(&g_mtl_shared_params, 0, sizeof(g_mtl_shared_params));
  pthread_mutex_lock(&ut_init_mutex);
  memset(&ut_init_params, 0, sizeof(ut_init_params));
  ut_init_calls = 0;
  ut_uninit_calls = 0;
  ut_lifecycle_lock_calls = 0;
  ut_block_init = false;
  ut_release_init = false;
  pthread_mutex_unlock(&ut_init_mutex);
}

mtl_handle ut_ffmpeg_get(const struct StDevArgs* args, int* idx) {
  return mtl_dev_get(NULL, args, idx);
}

int ut_ffmpeg_put(mtl_handle handle) {
  return mtl_instance_put(NULL, handle);
}

void ut_ffmpeg_block_first_init(void) {
  pthread_mutex_lock(&ut_init_mutex);
  ut_block_init = true;
  pthread_mutex_unlock(&ut_init_mutex);
}

bool ut_ffmpeg_wait_for_init_calls(int count) {
  struct timespec timeout;
  bool reached;

  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec++;
  pthread_mutex_lock(&ut_init_mutex);
  while ((ut_init_calls < count) && !ut_release_init) {
    if (pthread_cond_timedwait(&ut_init_cond, &ut_init_mutex, &timeout) == ETIMEDOUT)
      break;
  }
  reached = ut_init_calls >= count;
  pthread_mutex_unlock(&ut_init_mutex);
  return reached;
}

bool ut_ffmpeg_wait_for_lifecycle_lock_calls(int count) {
  struct timespec timeout;
  bool reached;

  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec++;
  pthread_mutex_lock(&ut_init_mutex);
  while (ut_lifecycle_lock_calls < count) {
    if (pthread_cond_timedwait(&ut_init_cond, &ut_init_mutex, &timeout) == ETIMEDOUT)
      break;
  }
  reached = ut_lifecycle_lock_calls >= count;
  pthread_mutex_unlock(&ut_init_mutex);
  return reached;
}

void ut_ffmpeg_release_init(void) {
  pthread_mutex_lock(&ut_init_mutex);
  ut_release_init = true;
  pthread_cond_broadcast(&ut_init_cond);
  pthread_mutex_unlock(&ut_init_mutex);
}

const struct mtl_init_params* ut_ffmpeg_last_init_params(void) {
  return &ut_init_params;
}

int ut_ffmpeg_init_calls(void) {
  int calls;

  pthread_mutex_lock(&ut_init_mutex);
  calls = ut_init_calls;
  pthread_mutex_unlock(&ut_init_mutex);
  return calls;
}

int ut_ffmpeg_uninit_calls(void) {
  int calls;

  pthread_mutex_lock(&ut_init_mutex);
  calls = ut_uninit_calls;
  pthread_mutex_unlock(&ut_init_mutex);
  return calls;
}