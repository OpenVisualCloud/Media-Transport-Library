/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST40p (ancillary) RX pipeline-layer unit tests.
 * Drives rx_st40p_frame_ready() directly and stubs the transport calls the
 * pipeline makes: st40_rx_create(), st40_rx_put_framebuff() and the stats calls.
 */

#include <stdlib.h>
#include <string.h>

#define st40_rx_create ut40p_rx_create
#define st40_rx_get_session_stats ut40p_rx_get_session_stats
#define st40_rx_put_framebuff ut40p_rx_put_framebuff
#define st40_rx_reset_session_stats ut40p_rx_reset_session_stats

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st40_pipeline_rx.c"
#pragma GCC diagnostic pop

#undef st40_rx_create
#undef st40_rx_get_session_stats
#undef st40_rx_put_framebuff
#undef st40_rx_reset_session_stats

#include "common/ut_common.h"

static int g_put_framebuff_calls;
static void* g_put_framebuff_last_addr;
static struct st40_rx_ops g_created_ops;

st40_rx_handle ut40p_rx_create(mtl_handle mt, struct st40_rx_ops* ops) {
  (void)mt;
  g_created_ops = *ops;
  return (st40_rx_handle)(uintptr_t)0x1;
}

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

  ut40p_put_framebuff_reset_spy();

  return ctx;
}

void ut40p_ctx_destroy(ut40p_ctx* ctx) {
  if (!ctx) return;
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

int ut40p_inject_meta(ut40p_ctx* ctx, void* udw_addr, struct st40_rx_frame_meta* meta) {
  return rx_st40p_frame_ready(&ctx->pipeline, udw_addr, meta);
}

int ut40p_rx_create_transport(ut40p_ctx* ctx, uint32_t flags, struct st40_rx_ops* out) {
  struct st40p_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.port.num_port = 1;
  (void)snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", "ut40p_port");
  ops.max_udw_buff_size = 1024;
  ops.flags = flags;
  ctx->impl.user_para.num_ports = 1;
  (void)snprintf(ctx->impl.user_para.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s",
                 "ut40p_port");

  memset(&g_created_ops, 0, sizeof(g_created_ops));
  int ret = rx_st40p_create_transport(&ctx->impl, &ctx->pipeline, &ops);
  *out = g_created_ops;
  return ret;
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

int ut40p_frame_idx(const struct st40_frame_info* frame) {
  const struct st40p_rx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut40p_framebuff_cnt(const ut40p_ctx* ctx) {
  return ctx->framebuff_cnt;
}

int ut40p_frame_stat(const ut40p_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
}

uint32_t ut40p_stat_drop_frame(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_drop_frame;
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
