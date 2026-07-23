/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST20p (video) TX pipeline-layer concurrency unit tests.
 * Includes the production translation unit so the file-local transport
 * callbacks (tx_st20p_next_frame / tx_st20p_frame_done) are reachable, and
 * hand-initialises the ctx in the derive path so create_transport is bypassed.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "st2110/pipeline/st20_pipeline_tx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

struct ut20p_tx_ctx {
  struct mtl_main_impl impl;
  struct st20p_tx_ctx pipeline;
  struct st20p_tx_frame* framebuffs;
  int framebuff_cnt;
  bool blocking;
};

#include "pipeline/st20p_tx_harness.h"

int ut20p_tx_init(void) {
  return ut_eal_init();
}

ut20p_tx_ctx* ut20p_tx_ctx_create(int framebuff_cnt) {
  ut20p_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st20p_tx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST20P_TX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* derive path: tx_st20p_user_frame() returns &dst, so put_frame() recovers
     * the framebuf via dst.priv. */
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
    /* convert_frame.priv lets ut20p_tx_convert_frame_idx() recover the slot
     * index from a st20_convert_frame_meta pointer. */
    ctx->framebuffs[i].convert_frame.src = &ctx->framebuffs[i].src;
    ctx->framebuffs[i].convert_frame.dst = &ctx->framebuffs[i].dst;
    ctx->framebuffs[i].convert_frame.priv = &ctx->framebuffs[i];
  }

  struct st20p_tx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST20_HANDLE_PIPELINE_TX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->derive = true; /* input_fmt == transport_fmt: put_frame -> CONVERTED directly */
  p->internal_converter = NULL;
  p->convert_impl = NULL;
  p->transport = (st20_tx_handle)(uintptr_t)0x1;
  p->block_get = false;
  /* ops.flags left 0: no DROP_WHEN_LATE / USER_PACING, so tx_st20p_if_frame_late
   * returns immediately and next_frame never touches the (absent) transport. */

  return ctx;
}

void ut20p_tx_ctx_destroy(ut20p_tx_ctx* ctx) {
  if (!ctx) return;
  if (ctx->blocking) {
    mt_pthread_mutex_destroy(&ctx->pipeline.block_wake_mutex);
    mt_pthread_cond_destroy(&ctx->pipeline.block_wake_cond);
  }
  free(ctx->framebuffs);
  free(ctx);
}

void ut20p_tx_ctx_enable_blocking(ut20p_tx_ctx* ctx, uint64_t timeout_ns) {
  struct st20p_tx_ctx* p = &ctx->pipeline;
  mt_pthread_mutex_init(&p->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&p->block_wake_cond);
  p->block_timeout_ns = timeout_ns;
  p->wake_on_destroy = (void (*)(void*))tx_st20p_block_wake;
  p->block_get = true;
  ctx->blocking = true;
}

void ut20p_tx_wake_block(ut20p_tx_ctx* ctx) {
  st20p_tx_wake_block(&ctx->pipeline);
}

int ut20p_tx_framebuff_cnt(const ut20p_tx_ctx* ctx) {
  return ctx->framebuff_cnt;
}

struct st_frame* ut20p_tx_get_frame(ut20p_tx_ctx* ctx) {
  return st20p_tx_get_frame(&ctx->pipeline);
}

int ut20p_tx_put_frame(ut20p_tx_ctx* ctx, struct st_frame* frame) {
  return st20p_tx_put_frame(&ctx->pipeline, frame);
}

int ut20p_tx_put_frame_abort(ut20p_tx_ctx* ctx, struct st_frame* frame) {
  return st20p_tx_put_frame_abort(&ctx->pipeline, frame);
}

int ut20p_tx_next_frame(ut20p_tx_ctx* ctx, uint16_t* idx) {
  struct st20_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st20p_next_frame(&ctx->pipeline, idx, &meta);
}

int ut20p_tx_frame_done(ut20p_tx_ctx* ctx, uint16_t idx) {
  struct st20_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st20p_frame_done(&ctx->pipeline, idx, &meta);
}

void ut20p_tx_ctx_set_manual_release(ut20p_tx_ctx* ctx) {
  ctx->pipeline.ops.flags |= ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE;
}

static int ut20p_tx_stub_convert(struct st_frame* src, struct st_frame* dst) {
  MTL_MAY_UNUSED(src);
  MTL_MAY_UNUSED(dst);
  return 0;
}

static struct st_frame_converter ut20p_tx_stub_converter = {
    .convert_func = ut20p_tx_stub_convert,
};

void ut20p_tx_ctx_set_internal_converter(ut20p_tx_ctx* ctx) {
  struct st20p_tx_ctx* p = &ctx->pipeline;
  const enum st_frame_fmt fmt = ST_FRAME_FMT_UYVY;
  const uint32_t width = 64, height = 2;

  p->derive = false;
  p->internal_converter = &ut20p_tx_stub_converter;
  p->ops.flags |= ST20P_TX_FLAG_EXT_FRAME;

  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    struct st20p_tx_frame* fb = &ctx->framebuffs[i];
    fb->src.priv = fb;
    fb->src.fmt = fmt;
    fb->src.width = width;
    fb->src.height = height;
    fb->src.interlaced = false;
  }
}

int ut20p_tx_put_ext_frame(ut20p_tx_ctx* ctx, struct st_frame* frame,
                           struct st_ext_frame* ext_frame) {
  return st20p_tx_put_ext_frame(&ctx->pipeline, frame, ext_frame);
}

int ut20p_tx_notify_ext_frame_free(ut20p_tx_ctx* ctx, uint16_t idx) {
  struct st_frame* frame = tx_st20p_user_frame(&ctx->pipeline, &ctx->framebuffs[idx]);
  return st20p_tx_notify_ext_frame_free(&ctx->pipeline, frame);
}

void ut20p_tx_set_notify_frame_done(ut20p_tx_ctx* ctx,
                                    int (*cb)(void* priv, struct st_frame* frame),
                                    void* priv) {
  ctx->pipeline.ops.notify_frame_done = cb;
  ctx->pipeline.ops.priv = priv;
}

void ut20p_tx_set_frame_ready(ut20p_tx_ctx* ctx, int idx) {
  __atomic_store_n(&ctx->framebuffs[idx].stat, ST20P_TX_FRAME_READY, __ATOMIC_RELEASE);
}

struct st20_convert_frame_meta* ut20p_tx_convert_get_frame(ut20p_tx_ctx* ctx) {
  return tx_st20p_convert_get_frame(&ctx->pipeline);
}

int ut20p_tx_convert_put_frame(ut20p_tx_ctx* ctx, struct st20_convert_frame_meta* frame,
                               int result) {
  return tx_st20p_convert_put_frame(&ctx->pipeline, frame, result);
}

int ut20p_tx_convert_frame_idx(const struct st20_convert_frame_meta* meta) {
  const struct st20p_tx_frame* framebuff = meta->priv;
  return framebuff->idx;
}

int ut20p_tx_frame_idx(const struct st_frame* frame) {
  const struct st20p_tx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut20p_tx_all_free(const ut20p_tx_ctx* ctx) {
  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->framebuffs[i].stat != ST20P_TX_FRAME_FREE) return 0;
  }
  return 1;
}

int ut20p_tx_frame_stat(const ut20p_tx_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}

uint64_t ut20p_tx_stat_frames_sent(const ut20p_tx_ctx* ctx) {
  return ctx->pipeline.stat_frames_sent;
}
