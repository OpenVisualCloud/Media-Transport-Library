/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST20p (video) pipeline-layer TX unit tests.
 *
 * Includes the production pipeline TX .c so the static tx_st20p_next_frame /
 * tx_st20p_frame_done / tx_st20p_if_frame_late are reachable and the public
 * st20p_tx_* entry points see our hand-built st20p_tx_ctx. derive=true so
 * put_frame goes straight to CONVERTED and the converter is never invoked.
 * Disable USDT to avoid linker references to probe semaphores.
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

/*
 * Stubs for the transport libmtl symbols the pipeline TX calls on the paths we
 * exercise (get_session_stats / reset). Our transport handle is a fake, so
 * --allow-multiple-definition lets ours win.
 */

int st20_tx_get_session_stats(st20_tx_handle handle, struct st20_tx_user_stats* stats) {
  (void)handle;
  /* Return zeroed transport stats; the pipeline overlays its own frame-level
   * counters on top, which is exactly what we want to test. */
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st20_tx_reset_session_stats(st20_tx_handle handle) {
  (void)handle;
  return 0;
}

/* ── opaque context ───────────────────────────────────────────────────── */

#define UT20PTX_USER_META_SIZE 64

struct ut20ptx_ctx {
  struct mtl_main_impl impl;
  struct st20p_tx_ctx pipeline;
  struct st20p_tx_frame* framebuffs;
  int framebuff_cnt;
  uint32_t late_cnt;
};

#include "pipeline/st20p_tx_harness.h"

/* ── ptp stub (returns the test-controlled wall clock) ─────────────────── */

static uint64_t ut20ptx_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  return impl->ptp_usync;
}

/* ── notify_frame_late callback (counts drop-when-late events) ──────────── */

static int ut20ptx_notify_late(void* priv, uint64_t epoch_skipped) {
  (void)epoch_skipped;
  ut20ptx_ctx* ctx = priv;
  ctx->late_cnt++;
  return 0;
}

/* ── init ─────────────────────────────────────────────────────────────── */

int ut20ptx_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut20ptx_ctx* ut20ptx_ctx_create(int framebuff_cnt) {
  ut20ptx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->framebuff_cnt = framebuff_cnt;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.ptp_usync = 0;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->impl.inf[i].parent = &ctx->impl;
    ctx->impl.inf[i].port = i;
    ctx->impl.inf[i].ptp_get_time_fn = ut20ptx_ptp_time;
  }

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st20p_tx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST20P_TX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* derive=true: get/put operate on dst; put reads frame->priv to recover
     * the owning framebuff. */
    ctx->framebuffs[i].src.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].user_meta = calloc(1, UT20PTX_USER_META_SIZE);
    ctx->framebuffs[i].user_meta_buffer_size = UT20PTX_USER_META_SIZE;
    if (!ctx->framebuffs[i].user_meta) {
      for (int j = 0; j < i; j++) free(ctx->framebuffs[j].user_meta);
      free(ctx->framebuffs);
      free(ctx);
      return NULL;
    }
  }

  struct st20p_tx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST20_HANDLE_PIPELINE_TX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->derive = true;
  p->ready = true;
  p->block_get = false;
  p->transport = (st20_tx_handle)(uintptr_t)0x1;
  p->ops.fps = ST_FPS_P59_94;
  p->ops.notify_frame_late = ut20ptx_notify_late;
  p->ops.priv = ctx;

  if (pthread_mutex_init(&p->lock, NULL) != 0) {
    for (int i = 0; i < framebuff_cnt; i++) free(ctx->framebuffs[i].user_meta);
    free(ctx->framebuffs);
    free(ctx);
    return NULL;
  }

  return ctx;
}

void ut20ptx_ctx_destroy(ut20ptx_ctx* ctx) {
  if (!ctx) return;
  pthread_mutex_destroy(&ctx->pipeline.lock);
  for (int i = 0; i < ctx->framebuff_cnt; i++) free(ctx->framebuffs[i].user_meta);
  free(ctx->framebuffs);
  free(ctx);
}

/* ── controls ─────────────────────────────────────────────────────────── */

void ut20ptx_set_drop_when_late(ut20ptx_ctx* ctx, bool on) {
  if (on)
    ctx->pipeline.ops.flags |= ST20P_TX_FLAG_DROP_WHEN_LATE;
  else
    ctx->pipeline.ops.flags &= ~(uint32_t)ST20P_TX_FLAG_DROP_WHEN_LATE;
}

void ut20ptx_set_user_pacing(ut20ptx_ctx* ctx, bool on) {
  if (on)
    ctx->pipeline.ops.flags |= ST20P_TX_FLAG_USER_PACING;
  else
    ctx->pipeline.ops.flags &= ~(uint32_t)ST20P_TX_FLAG_USER_PACING;
}

void ut20ptx_set_fps(ut20ptx_ctx* ctx, enum st_fps fps) {
  ctx->pipeline.ops.fps = fps;
}

void ut20ptx_set_ptp_now(ut20ptx_ctx* ctx, uint64_t ns) {
  ctx->impl.ptp_usync = ns;
}

/* ── frame state machine drivers ──────────────────────────────────────── */

struct st_frame* ut20ptx_get_frame(ut20ptx_ctx* ctx) {
  return st20p_tx_get_frame(&ctx->pipeline);
}

int ut20ptx_put_frame(ut20ptx_ctx* ctx, struct st_frame* frame) {
  return st20p_tx_put_frame(&ctx->pipeline, frame);
}

int ut20ptx_next_frame(ut20ptx_ctx* ctx, uint16_t* idx, struct st20_tx_frame_meta* meta) {
  struct st20_tx_frame_meta local;
  memset(&local, 0, sizeof(local));
  uint16_t local_idx = 0;
  int ret = tx_st20p_next_frame(&ctx->pipeline, &local_idx, &local);
  if (ret == 0) {
    if (idx) *idx = local_idx;
    if (meta) *meta = local;
  }
  return ret;
}

int ut20ptx_frame_done(ut20ptx_ctx* ctx, uint16_t idx) {
  struct st20_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return tx_st20p_frame_done(&ctx->pipeline, idx, &meta);
}

/* ── inspection ───────────────────────────────────────────────────────── */

int ut20ptx_frame_stat(ut20ptx_ctx* ctx, uint16_t idx) {
  return (int)ctx->framebuffs[idx].stat;
}

uint32_t ut20ptx_notify_late_cnt(const ut20ptx_ctx* ctx) {
  return ctx->late_cnt;
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20ptx_stat_frames_dropped(const ut20ptx_ctx* ctx) {
  return ctx->pipeline.stat_frames_dropped;
}

uint64_t ut20ptx_stat_frames_sent(const ut20ptx_ctx* ctx) {
  return ctx->pipeline.stat_frames_sent;
}

/* ── public-API wrappers ──────────────────────────────────────────────── */

int ut20ptx_get_session_stats(ut20ptx_ctx* ctx, struct st20_tx_user_stats* stats) {
  return st20p_tx_get_session_stats(&ctx->pipeline, stats);
}

int ut20ptx_reset_session_stats(ut20ptx_ctx* ctx) {
  return st20p_tx_reset_session_stats(&ctx->pipeline);
}
