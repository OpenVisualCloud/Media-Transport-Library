/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST22p (compressed video) TX pipeline-layer concurrency unit
 * tests. Includes the production translation unit so the file-local transport
 * callbacks (tx_st22p_next_frame / tx_st22p_frame_done) are reachable, and
 * hand-initialises the ctx in the derive path so create_transport and the
 * encoder plugin are bypassed.
 */

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
};

#include "pipeline/st22p_tx_harness.h"

int ut22p_tx_init(void) {
  return ut_eal_init();
}

ut22p_tx_ctx* ut22p_tx_ctx_create(int framebuff_cnt) {
  ut22p_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

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
  /* ops.flags left 0: no DROP_WHEN_LATE / USER_PACING, so tx_st22p_if_frame_late
   * returns immediately and next_frame never touches the (absent) transport. */

  return ctx;
}

void ut22p_tx_ctx_destroy(ut22p_tx_ctx* ctx) {
  if (!ctx) return;
  free(ctx->framebuffs);
  free(ctx);
}

int ut22p_tx_framebuff_cnt(const ut22p_tx_ctx* ctx) {
  return ctx->framebuff_cnt;
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
