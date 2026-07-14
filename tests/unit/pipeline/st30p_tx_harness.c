/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST30p (audio) TX pipeline-layer concurrency unit tests.
 * Includes the production translation unit so the file-local transport
 * callbacks (tx_st30p_next_frame / tx_st30p_frame_done) are reachable, and
 * stubs the libmtl transport symbols the TU references but the test never
 * calls (create_transport is bypassed).
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "st2110/pipeline/st30_pipeline_tx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/* libmtl stubs: only referenced via create/free/update paths we never run. */

int st30_tx_get_session_stats(st30_tx_handle handle, struct st30_tx_user_stats* stats) {
  (void)handle;
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st30_tx_reset_session_stats(st30_tx_handle handle) {
  (void)handle;
  return 0;
}

struct ut30p_tx_ctx {
  struct mtl_main_impl impl;
  struct st30p_tx_ctx pipeline;
  struct st30p_tx_frame* framebuffs;
  int framebuff_cnt;
};

#include "pipeline/st30p_tx_harness.h"

int ut30p_tx_init(void) {
  return ut_eal_init();
}

ut30p_tx_ctx* ut30p_tx_ctx_create(int framebuff_cnt) {
  ut30p_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st30p_tx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST30P_TX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* mirrors production init: put_frame() recovers framebuf via frame->priv */
    ctx->framebuffs[i].frame.priv = &ctx->framebuffs[i];
  }

  struct st30p_tx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST30_HANDLE_PIPELINE_TX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->transport = (st30_tx_handle)(uintptr_t)0x1;
  p->block_get = false;
  p->usdt_dump_fd = -1; /* keep usdt_dump_close() a no-op (avoid close(0)) */
  p->frames_per_sec = 1000;
  /* ops.flags left 0: no DROP_WHEN_LATE / USER_PACING, so tx_st30p_if_frame_late
   * returns immediately and next_frame never touches the (absent) transport. */

  return ctx;
}

void ut30p_tx_ctx_destroy(ut30p_tx_ctx* ctx) {
  if (!ctx) return;
  free(ctx->framebuffs);
  free(ctx);
}

int ut30p_tx_framebuff_cnt(const ut30p_tx_ctx* ctx) {
  return ctx->framebuff_cnt;
}

struct st30_frame* ut30p_tx_get_frame(ut30p_tx_ctx* ctx) {
  return st30p_tx_get_frame(&ctx->pipeline);
}

int ut30p_tx_put_frame(ut30p_tx_ctx* ctx, struct st30_frame* frame) {
  return st30p_tx_put_frame(&ctx->pipeline, frame);
}

int ut30p_tx_next_frame(ut30p_tx_ctx* ctx, uint16_t* idx) {
  struct st30_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st30p_next_frame(&ctx->pipeline, idx, &meta);
}

int ut30p_tx_frame_done(ut30p_tx_ctx* ctx, uint16_t idx) {
  struct st30_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st30p_frame_done(&ctx->pipeline, idx, &meta);
}

void ut30p_tx_set_frame_ready(ut30p_tx_ctx* ctx, int idx) {
  __atomic_store_n(&ctx->framebuffs[idx].stat, ST30P_TX_FRAME_READY, __ATOMIC_RELEASE);
}

int ut30p_tx_frame_idx(const struct st30_frame* frame) {
  const struct st30p_tx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut30p_tx_all_free(const ut30p_tx_ctx* ctx) {
  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->framebuffs[i].stat != ST30P_TX_FRAME_FREE) return 0;
  }
  return 1;
}

int ut30p_tx_frame_stat(const ut30p_tx_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}

uint64_t ut30p_tx_stat_frames_sent(const ut30p_tx_ctx* ctx) {
  return ctx->pipeline.stat_frames_sent;
}
