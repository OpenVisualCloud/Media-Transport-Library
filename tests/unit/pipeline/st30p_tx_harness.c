/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST30p (audio) TX pipeline-layer unit tests.
 * Includes the production translation unit so the file-local transport
 * callbacks (tx_st30p_next_frame / tx_st30p_frame_done) are reachable, and
 * stubs the libmtl transport symbols the TU references. st30_tx_create is
 * redirected to a recorder that fails, so st30p_tx_create() runs up to its
 * transport step and no further.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define st30_tx_create ut30p_tx_st30_tx_create
#include "st2110/pipeline/st30_pipeline_tx.c"
#undef st30_tx_create
#pragma GCC diagnostic pop
#undef st30_tx_create

#include "common/ut_common.h"

/* libmtl stubs: only referenced via stats paths we never run. */

int st30_tx_get_session_stats(st30_tx_handle handle, struct st30_tx_user_stats* stats) {
  (void)handle;
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st30_tx_reset_session_stats(st30_tx_handle handle) {
  (void)handle;
  return 0;
}

static int ut30p_tx_transport_ctx_socket;
static struct st30_tx_ops ut30p_tx_transport_ops;

/* Records the transport ops and the socket st30p_tx_create() placed its ctx on, then
 * fails the create. */
st30_tx_handle ut30p_tx_st30_tx_create(mtl_handle mt, struct st30_tx_ops* ops) {
  (void)mt;
  ut30p_tx_transport_ops = *ops;
  ut30p_tx_transport_ctx_socket = ((struct st30p_tx_ctx*)ops->priv)->socket_id;
  return NULL;
}

struct ut30p_tx_ctx {
  struct mtl_main_impl impl;
  struct st30p_tx_ctx pipeline;
  struct st30p_tx_frame* framebuffs;
  int framebuff_cnt;
  bool blocking;
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
  if (ctx->blocking) {
    mt_pthread_mutex_destroy(&ctx->pipeline.block_wake_mutex);
    mt_pthread_cond_destroy(&ctx->pipeline.block_wake_cond);
  }
  free(ctx->framebuffs);
  free(ctx);
}

void ut30p_tx_ctx_enable_blocking(ut30p_tx_ctx* ctx, uint64_t timeout_ns) {
  struct st30p_tx_ctx* p = &ctx->pipeline;
  mt_pthread_mutex_init(&p->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&p->block_wake_cond);
  p->block_timeout_ns = timeout_ns;
  p->wake_on_destroy = (void (*)(void*))tx_st30p_block_wake;
  p->block_get = true;
  ctx->blocking = true;
}

void ut30p_tx_wake_block(ut30p_tx_ctx* ctx) {
  st30p_tx_wake_block(&ctx->pipeline);
}

void ut30p_tx_frame_free_wake(ut30p_tx_ctx* ctx) {
  tx_st30p_notify_frame_available(&ctx->pipeline);
}

void ut30p_tx_force_destroying(ut30p_tx_ctx* ctx) {
  atomic_store_explicit(&ctx->pipeline.lc_destroying, 1, memory_order_release);
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

int ut30p_tx_put_frame_abort(ut30p_tx_ctx* ctx, struct st30_frame* frame) {
  return st30p_tx_put_frame_abort(&ctx->pipeline, frame);
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

int ut30p_tx_create_ctx_socket(int nic_socket, uint32_t flags, int socket_id) {
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  if (!impl) return INT_MIN;
  impl->type = MT_HANDLE_MAIN;
  impl->user_para.num_ports = 1;
  snprintf(impl->user_para.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "ut30p_tx_port");
  impl->inf[MTL_PORT_P].socket_id = nic_socket;

  struct st30p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = "ut30p_tx_create";
  ops.port.num_port = 1;
  snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "ut30p_tx_port");
  ops.fmt = ST30_FMT_PCM16;
  ops.channel = 1;
  ops.sampling = ST30_SAMPLING_48K;
  ops.ptime = ST30_PTIME_1MS;
  ops.framebuff_cnt = 1;
  ops.framebuff_size = 96;
  ops.flags = flags;
  ops.socket_id = socket_id;

  ut30p_tx_transport_ctx_socket = INT_MIN;
  st30p_tx_create(impl, &ops);
  free(impl);
  return ut30p_tx_transport_ctx_socket;
}

void ut30p_tx_set_notify_frame_late(ut30p_tx_ctx* ctx,
                                    int (*cb)(void* priv, uint64_t epoch_skipped),
                                    void* priv) {
  ctx->pipeline.ops.notify_frame_late = cb;
  ctx->pipeline.ops.priv = priv;
}

int ut30p_tx_transport_report_late(ut30p_tx_ctx* ctx, uint64_t epoch_skipped) {
  memset(&ut30p_tx_transport_ops, 0, sizeof(ut30p_tx_transport_ops));
  tx_st30p_create_transport(&ctx->impl, &ctx->pipeline, &ctx->pipeline.ops);
  if (!ut30p_tx_transport_ops.notify_frame_late) return -ENOENT;
  return ut30p_tx_transport_ops.notify_frame_late(ut30p_tx_transport_ops.priv,
                                                  epoch_skipped);
}

st30p_tx_handle ut30p_tx_handle(ut30p_tx_ctx* ctx) {
  return &ctx->pipeline;
}
