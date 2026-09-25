/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST22p (compressed video) TX pipeline-layer concurrency unit
 * tests. Includes the production translation unit so the file-local transport
 * callbacks (tx_st22p_next_frame / tx_st22p_frame_done) are reachable, and
 * hand-initialises the ctx in the derive path so create_transport and the
 * encoder plugin are bypassed.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "st2110/pipeline/st22_pipeline_tx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

struct ut22p_tx_ctx {
  struct mtl_main_impl impl;
  struct st22p_tx_ctx pipeline;
  struct st22p_tx_frame* framebuffs;
  int framebuff_cnt;
  uint64_t mock_ptp_ns;
  bool encode_blocking;
  bool blocking;
};

#include "pipeline/st22p_tx_harness.h"

static uint64_t ut22p_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  struct ut22p_tx_ctx* ctx = (struct ut22p_tx_ctx*)impl; /* impl is ctx's first member */
  return ctx->mock_ptp_ns;
}

int ut22p_tx_init(void) {
  return ut_eal_init();
}

ut22p_tx_ctx* ut22p_tx_ctx_create(int framebuff_cnt) {
  ut22p_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut22p_ptp_time_fn;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st22p_tx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST22P_TX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* derive path: tx_st22p_user_frame() returns &dst, so put_frame() recovers
     * the framebuf via dst.priv. */
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
  }

  struct st22p_tx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST22_HANDLE_PIPELINE_TX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->derive = true; /* input_fmt == transport_fmt: put_frame -> ENCODED directly */
  p->ext_frame = false;
  p->encode_impl = NULL; /* derive skips the encoder notify path */
  p->transport = (st22_tx_handle)(uintptr_t)0x1;
  p->block_get = false;
  p->encode_block_get = false;
  /* ops.fps 0 is ST_FPS_P59_94, so cache the period create would cache for it */
  ut22p_tx_set_fps(ctx, ST_FPS_P59_94);
  /* ops.flags left 0: no DROP_WHEN_LATE / USER_PACING, so tx_st22p_if_frame_late
   * returns immediately and next_frame never touches the (absent) transport. */

  return ctx;
}

void ut22p_tx_ctx_destroy(ut22p_tx_ctx* ctx) {
  if (!ctx) return;
  if (ctx->blocking) {
    mt_pthread_mutex_destroy(&ctx->pipeline.block_wake_mutex);
    mt_pthread_cond_destroy(&ctx->pipeline.block_wake_cond);
  }
  if (ctx->encode_blocking) {
    mt_pthread_mutex_destroy(&ctx->pipeline.encode_block_wake_mutex);
    mt_pthread_cond_destroy(&ctx->pipeline.encode_block_wake_cond);
  }
  free(ctx->framebuffs);
  free(ctx);
}

void ut22p_tx_ctx_enable_blocking(ut22p_tx_ctx* ctx, uint64_t timeout_ns) {
  struct st22p_tx_ctx* p = &ctx->pipeline;
  mt_pthread_mutex_init(&p->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&p->block_wake_cond);
  p->block_timeout_ns = timeout_ns;
  p->block_get = true;
  ctx->blocking = true;
}

void ut22p_tx_ctx_enable_encode_blocking(ut22p_tx_ctx* ctx, uint64_t timeout_ns) {
  struct st22p_tx_ctx* p = &ctx->pipeline;
  mt_pthread_mutex_init(&p->encode_block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&p->encode_block_wake_cond);
  p->encode_block_timeout_ns = timeout_ns;
  p->encode_block_get = true;
  ctx->encode_blocking = true;
}

void ut22p_tx_encode_wake_block(ut22p_tx_ctx* ctx) {
  tx_st22p_encode_wake_block(&ctx->pipeline);
}

int ut22p_tx_encode_get_frame(ut22p_tx_ctx* ctx) {
  return tx_st22p_encode_get_frame(&ctx->pipeline) ? 0 : -EBUSY;
}

int ut22p_tx_framebuff_cnt(const ut22p_tx_ctx* ctx) {
  return ctx->framebuff_cnt;
}

void ut22p_tx_set_ptp_ns(ut22p_tx_ctx* ctx, uint64_t ns) {
  ctx->mock_ptp_ns = ns;
}

void ut22p_tx_set_flags(ut22p_tx_ctx* ctx, uint32_t flags) {
  ctx->pipeline.ops.flags |= flags;
}

void ut22p_tx_set_fps(ut22p_tx_ctx* ctx, enum st_fps fps) {
  ctx->pipeline.ops.fps = fps;
  /* mirror the period st22p_tx_create() caches for ops.fps. Zero it on an
   * out-of-table fps rather than leaving the previous value: a future caller
   * that passes one then fails the window assertions instead of passing on a
   * stale period. */
  if (st_frame_period_ns(fps, &ctx->pipeline.frame_period_ns) < 0)
    ctx->pipeline.frame_period_ns = 0;
}

void ut22p_tx_set_fps_mismatch(ut22p_tx_ctx* ctx, enum st_fps cached_fps,
                               enum st_fps ops_fps) {
  ut22p_tx_set_fps(ctx, cached_fps);
  ctx->pipeline.ops.fps = ops_fps;
}

void ut22p_tx_set_notify_frame_done(ut22p_tx_ctx* ctx,
                                    int (*cb)(void* priv, struct st_frame* frame),
                                    void* priv) {
  ctx->pipeline.ops.notify_frame_done = cb;
  ctx->pipeline.ops.priv = priv;
}

void ut22p_tx_set_notify_frame_late(ut22p_tx_ctx* ctx,
                                    int (*cb)(void* priv, uint64_t epoch_skipped),
                                    void* priv) {
  ctx->pipeline.ops.notify_frame_late = cb;
  ctx->pipeline.ops.priv = priv;
}

struct st_frame* ut22p_tx_get_frame(ut22p_tx_ctx* ctx) {
  return st22p_tx_get_frame(&ctx->pipeline);
}

int ut22p_tx_put_frame(ut22p_tx_ctx* ctx, struct st_frame* frame) {
  return st22p_tx_put_frame(&ctx->pipeline, frame);
}

int ut22p_tx_next_frame(ut22p_tx_ctx* ctx, uint16_t* idx) {
  struct st22_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st22p_next_frame(&ctx->pipeline, idx, &meta);
}

int ut22p_tx_frame_done(ut22p_tx_ctx* ctx, uint16_t idx) {
  struct st22_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st22p_frame_done(&ctx->pipeline, idx, &meta);
}

int ut22p_tx_frame_idx(const struct st_frame* frame) {
  const struct st22p_tx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut22p_tx_all_free(const ut22p_tx_ctx* ctx) {
  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->framebuffs[i].stat != ST22P_TX_FRAME_FREE) return 0;
  }
  return 1;
}

int ut22p_tx_frame_stat(const ut22p_tx_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}
