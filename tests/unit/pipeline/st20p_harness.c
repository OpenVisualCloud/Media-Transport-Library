/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST20p (video) pipeline-layer unit tests.
 *
 * Drives rx_st20p_frame_ready() directly with a synthetic
 * st20_rx_frame_meta — bypasses the transport session entirely.
 * The pipeline counters under test (stat_frames_received / _dropped /
 * _corrupted) are about producer/consumer flow, not packet semantics,
 * so transport realism is not required.
 *
 * The pipeline is configured with derive=true so that the converter
 * path is bypassed and frame_ready takes the simple branch:
 *   framebuff->dst = framebuff->src; stat = ST20P_RX_FRAME_CONVERTED;
 * This is the path get_frame() consumes ST20P_RX_FRAME_CONVERTED from.
 */

#include <stdlib.h>
#include <string.h>

/*
 * Include the production pipeline .c so static rx_st20p_frame_ready()
 * is reachable and the public st20p_rx_* entry points see our private
 * struct.  Disable USDT to avoid linker references to probe semaphores.
 */
#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st20_pipeline_rx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/*
 * Stubs for libmtl symbols called by the pipeline that would otherwise
 * resolve to the real library (which requires HW/hugepages or a real
 * transport session).  --allow-multiple-definition lets ours win.
 */

int st20_rx_put_framebuff(st20_rx_handle handle, void* frame) {
  (void)handle;
  (void)frame; /* transport-side framebuf release is a no-op for the harness */
  return 0;
}

int st20_rx_get_session_stats(st20_rx_handle handle, struct st20_rx_user_stats* stats) {
  (void)handle;
  /* Return zeroed transport stats; the pipeline overlays its own
   * frame-level counters on top, which is exactly what we want to test. */
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st20_rx_reset_session_stats(st20_rx_handle handle) {
  (void)handle;
  return 0;
}

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20p_ctx {
  struct mtl_main_impl impl;
  struct st20p_rx_ctx pipeline;
  struct st20p_rx_frame* framebuffs;
  int framebuff_cnt;
};

#include "pipeline/st20p_harness.h"

/* ── init ─────────────────────────────────────────────────────────────── */

int ut20p_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut20p_ctx* ut20p_ctx_create(int framebuff_cnt) {
  ut20p_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st20p_rx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST20P_RX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* mirrors production init: put_frame() reads frame->priv to recover
     * the owning st20p_rx_frame.  With derive=true frame_ready does
     * `dst = src`, so setting src.priv is sufficient. */
    ctx->framebuffs[i].src.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
  }

  struct st20p_rx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST20_HANDLE_PIPELINE_RX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  /* derive=true: skip converter; frame_ready stores src and marks
   * ST20P_RX_FRAME_CONVERTED, which get_frame() consumes directly. */
  p->derive = true;
  p->ready = true;
  /* transport handle: must be non-NULL for put_frame / overlay paths,
   * but our stubs ignore it. */
  p->transport = (st20_rx_handle)(uintptr_t)0x1;

  if (pthread_mutex_init(&p->lock, NULL) != 0) {
    free(ctx->framebuffs);
    free(ctx);
    return NULL;
  }

  return ctx;
}

void ut20p_ctx_destroy(ut20p_ctx* ctx) {
  if (!ctx) return;
  pthread_mutex_destroy(&ctx->pipeline.lock);
  free(ctx->framebuffs);
  free(ctx);
}

/* ── inject one frame via rx_st20p_frame_ready ────────────────────────── */

int ut20p_inject_frame(ut20p_ctx* ctx, enum st_frame_status status, uint32_t timestamp) {
  /* Stack-allocated synthetic meta — frame_ready copies what it needs
   * into the framebuf.  The frame addr only needs to be a stable
   * non-NULL pointer; with derive=true it is never dereferenced by the
   * pipeline before delivery, and our st20_rx_put_framebuff stub
   * ignores it on release. */
  struct st20_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.status = status;
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.pkts_total = 1;

  static uint8_t dummy_frame_storage; /* address-stable sentinel */
  return rx_st20p_frame_ready(&ctx->pipeline, &dummy_frame_storage, &meta);
}

/* ── frame get/put ────────────────────────────────────────────────────── */

struct st_frame* ut20p_get_frame(ut20p_ctx* ctx) {
  return st20p_rx_get_frame(&ctx->pipeline);
}

int ut20p_put_frame(ut20p_ctx* ctx, struct st_frame* frame) {
  return st20p_rx_put_frame(&ctx->pipeline, frame);
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20p_stat_frames_received(const ut20p_ctx* ctx) {
  return ctx->pipeline.stat_frames_received;
}

uint64_t ut20p_stat_frames_dropped(const ut20p_ctx* ctx) {
  return ctx->pipeline.stat_frames_dropped;
}

uint64_t ut20p_stat_frames_corrupted(const ut20p_ctx* ctx) {
  return ctx->pipeline.stat_frames_corrupted;
}

uint32_t ut20p_stat_busy(const ut20p_ctx* ctx) {
  return rte_atomic32_read(&ctx->pipeline.stat_busy);
}

/* ── public-API wrappers ─────────────────────────────────────────────── */

int ut20p_get_session_stats(ut20p_ctx* ctx, struct st20_rx_user_stats* stats) {
  return st20p_rx_get_session_stats(&ctx->pipeline, stats);
}

int ut20p_reset_session_stats(ut20p_ctx* ctx) {
  return st20p_rx_reset_session_stats(&ctx->pipeline);
}
