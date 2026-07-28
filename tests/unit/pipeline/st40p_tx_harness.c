/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST40p (ancillary) TX pipeline-layer concurrency unit tests.
 * Includes the production translation unit so the file-local transport
 * callbacks (tx_st40p_next_frame / tx_st40p_frame_done) are reachable, and
 * hand-initialises the ctx so create_transport is bypassed.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "st2110/pipeline/st40_pipeline_tx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

struct ut40p_tx_ctx {
  struct mtl_main_impl impl;
  struct st40p_tx_ctx pipeline;
  struct st40p_tx_frame* framebuffs;
  struct st40_frame* anc_frames; /* put_frame writes anc_frame->meta_num/data_size */
  int framebuff_cnt;
};

#include "pipeline/st40p_tx_harness.h"

int ut40p_tx_init(void) {
  return ut_eal_init();
}

ut40p_tx_ctx* ut40p_tx_ctx_create(int framebuff_cnt) {
  ut40p_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st40p_tx_frame));
  ctx->anc_frames = calloc(framebuff_cnt, sizeof(struct st40_frame));
  if (!ctx->framebuffs || !ctx->anc_frames) {
    free(ctx->anc_frames);
    free(ctx->framebuffs);
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST40P_TX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* mirrors production init: put_frame() recovers framebuf via frame_info->priv */
    ctx->framebuffs[i].frame_info.priv = &ctx->framebuffs[i];
    /* put_frame() writes meta_num/data_size into the assigned anc frame */
    ctx->framebuffs[i].anc_frame = &ctx->anc_frames[i];
  }

  struct st40p_tx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST40_HANDLE_PIPELINE_TX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->transport = (st40_tx_handle)(uintptr_t)0x1;
  p->block_get = false;
  p->frames_per_sec = 1000;
  /* ops.flags left 0: no DROP_WHEN_LATE / USER_PACING, so tx_st40p_if_frame_late
   * returns immediately and next_frame never touches the (absent) transport. */

  return ctx;
}

void ut40p_tx_ctx_destroy(ut40p_tx_ctx* ctx) {
  if (!ctx) return;
  free(ctx->anc_frames);
  free(ctx->framebuffs);
  free(ctx);
}

int ut40p_tx_framebuff_cnt(const ut40p_tx_ctx* ctx) {
  return ctx->framebuff_cnt;
}

struct st40_frame_info* ut40p_tx_get_frame(ut40p_tx_ctx* ctx) {
  return st40p_tx_get_frame(&ctx->pipeline);
}

int ut40p_tx_put_frame(ut40p_tx_ctx* ctx, struct st40_frame_info* frame) {
  return st40p_tx_put_frame(&ctx->pipeline, frame);
}

int ut40p_tx_next_frame(ut40p_tx_ctx* ctx, uint16_t* idx) {
  struct st40_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st40p_next_frame(&ctx->pipeline, idx, &meta);
}

int ut40p_tx_frame_done(ut40p_tx_ctx* ctx, uint16_t idx) {
  struct st40_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st40p_frame_done(&ctx->pipeline, idx, &meta);
}

void ut40p_tx_set_frame_ready(ut40p_tx_ctx* ctx, int idx) {
  __atomic_store_n(&ctx->framebuffs[idx].stat, ST40P_TX_FRAME_READY, __ATOMIC_RELEASE);
}

int ut40p_tx_frame_idx(const struct st40_frame_info* frame) {
  const struct st40p_tx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut40p_tx_all_free(const ut40p_tx_ctx* ctx) {
  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->framebuffs[i].stat != ST40P_TX_FRAME_FREE) return 0;
  }
  return 1;
}

int ut40p_tx_frame_stat(const ut40p_tx_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}

uint64_t ut40p_tx_stat_frames_sent(const ut40p_tx_ctx* ctx) {
  return ctx->pipeline.stat_frames_sent;
}
