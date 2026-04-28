/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST30p (audio) pipeline-layer unit tests.
 * Mirrors st20p_harness in spirit: drives rx_st30p_frame_ready() directly
 * and stubs the transport-side libmtl symbols that put_frame / overlay
 * paths reference.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st30_pipeline_rx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/* libmtl stubs */

int st30_rx_put_framebuff(st30_rx_handle handle, void* frame) {
  (void)handle;
  (void)frame;
  return 0;
}

int st30_rx_get_session_stats(st30_rx_handle handle, struct st30_rx_user_stats* stats) {
  (void)handle;
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st30_rx_reset_session_stats(st30_rx_handle handle) {
  (void)handle;
  return 0;
}

struct ut30p_ctx {
  struct mtl_main_impl impl;
  struct st30p_rx_ctx pipeline;
  struct st30p_rx_frame* framebuffs;
  int framebuff_cnt;
};

#include "pipeline/st30p_harness.h"

int ut30p_init(void) {
  return ut_eal_init();
}

ut30p_ctx* ut30p_ctx_create(int framebuff_cnt) {
  ut30p_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st30p_rx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST30P_RX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* mirrors production init: put_frame() recovers framebuf via frame->priv */
    ctx->framebuffs[i].frame.priv = &ctx->framebuffs[i];
  }

  struct st30p_rx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST30_HANDLE_PIPELINE_RX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->transport = (st30_rx_handle)(uintptr_t)0x1;

  if (pthread_mutex_init(&p->lock, NULL) != 0) {
    free(ctx->framebuffs);
    free(ctx);
    return NULL;
  }

  return ctx;
}

void ut30p_ctx_destroy(ut30p_ctx* ctx) {
  if (!ctx) return;
  pthread_mutex_destroy(&ctx->pipeline.lock);
  free(ctx->framebuffs);
  free(ctx);
}

int ut30p_inject_frame(ut30p_ctx* ctx, uint32_t timestamp) {
  struct st30_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.frame_recv_size = 1;

  static uint8_t dummy_frame_storage;
  return rx_st30p_frame_ready(&ctx->pipeline, &dummy_frame_storage, &meta);
}

struct st30_frame* ut30p_get_frame(ut30p_ctx* ctx) {
  return st30p_rx_get_frame(&ctx->pipeline);
}

int ut30p_put_frame(ut30p_ctx* ctx, struct st30_frame* frame) {
  return st30p_rx_put_frame(&ctx->pipeline, frame);
}

uint64_t ut30p_stat_frames_received(const ut30p_ctx* ctx) {
  return ctx->pipeline.stat_frames_received;
}

uint64_t ut30p_stat_frames_dropped(const ut30p_ctx* ctx) {
  return ctx->pipeline.stat_frames_dropped;
}

uint32_t ut30p_stat_busy(const ut30p_ctx* ctx) {
  return ctx->pipeline.stat_busy;
}

int ut30p_get_session_stats(ut30p_ctx* ctx, struct st30_rx_user_stats* stats) {
  return st30p_rx_get_session_stats(&ctx->pipeline, stats);
}

int ut30p_reset_session_stats(ut30p_ctx* ctx) {
  return st30p_rx_reset_session_stats(&ctx->pipeline);
}
