/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST40p (ancillary) pipeline-layer unit tests.
 * Mirrors st30p_harness in spirit: drives rx_st40p_frame_ready() directly
 * and stubs the transport-side st40_rx_put_framebuff() release call.
 */

#include <stdlib.h>
#include <string.h>

#define st40_rx_get_session_stats ut40p_rx_get_session_stats
#define st40_rx_put_framebuff ut40p_rx_put_framebuff
#define st40_rx_reset_session_stats ut40p_rx_reset_session_stats

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st40_pipeline_rx.c"
#pragma GCC diagnostic pop

#undef st40_rx_get_session_stats
#undef st40_rx_put_framebuff
#undef st40_rx_reset_session_stats

#include "common/ut_common.h"

static int g_put_framebuff_calls;
static void* g_put_framebuff_last_addr;

int ut40p_rx_put_framebuff(st40_rx_handle handle, void* frame) {
  (void)handle;
  g_put_framebuff_calls++;
  g_put_framebuff_last_addr = frame;
  return 0;
}

int ut40p_put_framebuff_call_count(void) {
  return g_put_framebuff_calls;
}

void* ut40p_put_framebuff_last_addr(void) {
  return g_put_framebuff_last_addr;
}

void ut40p_put_framebuff_reset_spy(void) {
  g_put_framebuff_calls = 0;
  g_put_framebuff_last_addr = NULL;
}

int ut40p_rx_get_session_stats(st40_rx_handle handle, struct st40_rx_user_stats* stats) {
  (void)handle;
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int ut40p_rx_reset_session_stats(st40_rx_handle handle) {
  (void)handle;
  return 0;
}

struct ut40p_ctx {
  struct mtl_main_impl impl;
  struct st40p_rx_ctx pipeline;
  struct st40p_rx_frame* framebuffs;
  int framebuff_cnt;
};

#include "pipeline/st40p_harness.h"

int ut40p_init(void) {
  return ut_eal_init();
}

ut40p_ctx* ut40p_ctx_create(int framebuff_cnt) {
  ut40p_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st40p_rx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST40P_RX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    ctx->framebuffs[i].frame_info.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].frame_info.meta = ctx->framebuffs[i].meta;
  }

  struct st40p_rx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST40_HANDLE_PIPELINE_RX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->transport = (st40_rx_handle)(uintptr_t)0x1;

  if (mt_pthread_mutex_init(&p->lock, NULL) != 0) {
    free(ctx->framebuffs);
    free(ctx);
    return NULL;
  }

  ut40p_put_framebuff_reset_spy();

  return ctx;
}

void ut40p_ctx_destroy(ut40p_ctx* ctx) {
  if (!ctx) return;
  pthread_mutex_destroy(&ctx->pipeline.lock);
  free(ctx->framebuffs);
  free(ctx);
}

int ut40p_inject_frame(ut40p_ctx* ctx, void* udw_addr, enum st_frame_status status,
                       bool seq_discont, uint32_t timestamp) {
  struct st40_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.status = status;
  meta.seq_discont = seq_discont;
  meta.meta_num = 0;
  meta.meta = NULL;

  return rx_st40p_frame_ready(&ctx->pipeline, udw_addr, &meta);
}

struct st40_frame_info* ut40p_get_frame(ut40p_ctx* ctx) {
  return st40p_rx_get_frame(&ctx->pipeline);
}

int ut40p_put_frame(ut40p_ctx* ctx, struct st40_frame_info* frame) {
  return st40p_rx_put_frame(&ctx->pipeline, frame);
}

int ut40p_put_frame_abort(ut40p_ctx* ctx, struct st40_frame_info* frame) {
  return st40p_rx_put_frame_abort(&ctx->pipeline, frame);
}

uint64_t ut40p_stat_frames_received(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_frames_received;
}

uint64_t ut40p_stat_frames_dropped(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_frames_dropped;
}

uint64_t ut40p_stat_frames_corrupted(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_frames_corrupted;
}

uint32_t ut40p_stat_busy(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_busy;
}

int ut40p_get_session_stats(ut40p_ctx* ctx, struct st40_rx_user_stats* stats) {
  return st40p_rx_get_session_stats(&ctx->pipeline, stats);
}

int ut40p_reset_session_stats(ut40p_ctx* ctx) {
  return st40p_rx_reset_session_stats(&ctx->pipeline);
}
