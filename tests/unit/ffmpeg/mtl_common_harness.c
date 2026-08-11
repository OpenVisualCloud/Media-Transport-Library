/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdarg.h>
#include <string.h>

#include "ffmpeg/ffmpeg_test_compat.h"
#define MTL_FFMPEG_UNIT_TEST
#include "../../../ecosystem/ffmpeg_plugin/mtl_common.c"
#include "ffmpeg/mtl_common_harness.h"

static struct mtl_init_params ut_init_params;
static int ut_init_calls;
static int ut_uninit_calls;
static mtl_handle ut_handle = (mtl_handle)(uintptr_t)1;

void av_log(void* avcl, int level, const char* fmt, ...) {
  (void)avcl;
  (void)level;
  (void)fmt;
}

mtl_handle mtl_init(struct mtl_init_params* p) {
  ut_init_params = *p;
  ut_init_calls++;
  return ut_handle;
}

int mtl_uninit(mtl_handle handle) {
  if (handle == ut_handle) ut_uninit_calls++;
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
  g_mtl_ptp_enable = 0;
  g_mtl_ptp_pi = 0;
  g_mtl_ptp_unicast = 0;
  memset(&ut_init_params, 0, sizeof(ut_init_params));
  ut_init_calls = 0;
  ut_uninit_calls = 0;
}

mtl_handle ut_ffmpeg_get(const struct StDevArgs* args, int* idx) {
  return mtl_dev_get(NULL, args, idx);
}

int ut_ffmpeg_put(mtl_handle handle) {
  return mtl_instance_put(NULL, handle);
}

const struct mtl_init_params* ut_ffmpeg_last_init_params(void) {
  return &ut_init_params;
}

int ut_ffmpeg_init_calls(void) {
  return ut_init_calls;
}

int ut_ffmpeg_uninit_calls(void) {
  return ut_uninit_calls;
}