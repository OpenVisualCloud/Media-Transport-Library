/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST22p (compressed video) RX pipeline-layer concurrency unit
 * tests. Mirrors st30p_harness in spirit: drives rx_st22p_frame_ready()
 * directly in the derive path and stubs the transport-side libmtl symbol that
 * put_frame references.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st22_pipeline_rx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/* libmtl stub: put_frame returns the codestream buffer to the transport. */

int st22_rx_put_framebuff(st22_rx_handle handle, void* frame) {
  (void)handle;
  (void)frame;
  return 0;
}

struct ut22p_ctx {
  struct mtl_main_impl impl;
  struct st22p_rx_ctx pipeline;
  struct st22p_rx_frame* framebuffs;
  int framebuff_cnt;
};

#include "pipeline/st22p_harness.h"

int ut22p_init(void) {
  return ut_eal_init();
}

ut22p_ctx* ut22p_ctx_create(int framebuff_cnt) {
  ut22p_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;
  ctx->impl.type = MT_HANDLE_MAIN;

  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st22p_rx_frame));
  if (!ctx->framebuffs) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->framebuffs[i].stat = ST22P_RX_FRAME_FREE;
    ctx->framebuffs[i].idx = i;
    /* derive frame_ready() copies dst = src, so the user frame (&dst) inherits
     * src.priv; seed src.priv so put_frame() recovers the framebuf. */
    ctx->framebuffs[i].src.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
  }

  struct st22p_rx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST22_HANDLE_PIPELINE_RX;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuffs = ctx->framebuffs;
  p->ready = true;
  p->derive = true; /* output_fmt == transport_fmt: frame_ready -> DECODED */
  p->ext_frame = false;
  p->transport = (st22_rx_handle)(uintptr_t)0x1;
  p->block_get = false;

  return ctx;
}

void ut22p_ctx_destroy(ut22p_ctx* ctx) {
  if (!ctx) return;
  free(ctx->framebuffs);
  free(ctx);
}

int ut22p_framebuff_cnt(const ut22p_ctx* ctx) {
  return ctx->framebuff_cnt;
}

int ut22p_inject_frame(ut22p_ctx* ctx, enum st_frame_status status, uint32_t timestamp) {
  struct st22_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.frame_total_size = 1;
  meta.status = status;

  static uint8_t dummy_frame_storage;
  return rx_st22p_frame_ready(&ctx->pipeline, &dummy_frame_storage, &meta);
}

struct st_frame* ut22p_get_frame(ut22p_ctx* ctx) {
  return st22p_rx_get_frame(&ctx->pipeline);
}

int ut22p_put_frame(ut22p_ctx* ctx, struct st_frame* frame) {
  return st22p_rx_put_frame(&ctx->pipeline, frame);
}

int ut22p_frame_idx(const struct st_frame* frame) {
  const struct st22p_rx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut22p_frame_stat(const ut22p_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}
