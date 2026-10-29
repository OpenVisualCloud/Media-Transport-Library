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
 * By default the pipeline is configured with derive=true so that the
 * converter path is bypassed and frame_ready takes the simple branch:
 *   framebuff->dst = framebuff->src; stat = ST20P_RX_FRAME_CONVERTED;
 * After ut20p_ctx_set_internal_converter() it leaves frames READY instead.
 */

#include <stdlib.h>
#include <string.h>

/*
 * Include the production pipeline .c so static rx_st20p_frame_ready()
 * is reachable and the public st20p_rx_* entry points see our private
 * struct.  Disable USDT to avoid linker references to probe semaphores.
 *
 * The pipeline calls transport-layer st20_rx_* entry points that are
 * also defined for real by st_rx_video_session.c (pulled into this same
 * binary via session/st20_harness.c). Redirect them to distinctly named
 * local stubs at compile time so the two definitions can never collide
 * as global symbols — relying on the linker's multiple-definition
 * resolution order is fragile and previously picked the real,
 * HW-backed symbol here, which dereferenced our fake transport handle
 * and crashed.
 */
#define st20_rx_put_framebuff ut20p_stub_put_framebuff
#define st20_rx_get_session_stats ut20p_stub_get_session_stats
#define st20_rx_reset_session_stats ut20p_stub_reset_session_stats
#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st20_pipeline_rx.c"
#pragma GCC diagnostic pop
#undef st20_rx_put_framebuff
#undef st20_rx_get_session_stats
#undef st20_rx_reset_session_stats

#undef st20_rx_get_session_stats
#undef st20_rx_put_framebuff
#undef st20_rx_reset_session_stats

#include "common/ut_common.h"
/*
 * Stubs for libmtl symbols called by the pipeline that would otherwise
 * resolve to the real library (which requires HW/hugepages or a real
 * transport session). Named distinctly from the real st20_rx_* symbols
 * and wired in via the #define redirection above.
 */

int ut20p_stub_put_framebuff(st20_rx_handle handle, void* frame) {
  (void)handle;
  (void)frame; /* transport-side framebuf release is a no-op for the harness */
  return 0;
}

int ut20p_stub_get_session_stats(st20_rx_handle handle,
                                 struct st20_rx_user_stats* stats) {
  (void)handle;
  /* Return zeroed transport stats; the pipeline overlays its own
   * frame-level counters on top, which is exactly what we want to test. */
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int ut20p_stub_reset_session_stats(st20_rx_handle handle) {
  (void)handle;
  return 0;
}

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20p_ctx {
  struct mtl_main_impl impl;
  struct st20p_rx_ctx pipeline;
  struct st20p_rx_frame* framebuffs;
  int framebuff_cnt;
  bool blocking;
  struct st_frame_converter converter;
  uint8_t* frames_mem;
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
     * the owning st20p_rx_frame. */
    ctx->framebuffs[i].src.priv = &ctx->framebuffs[i];
    ctx->framebuffs[i].dst.priv = &ctx->framebuffs[i];
    /* convert_frame.priv lets ut20p_convert_frame_idx() recover the
     * owning slot index from a st20_convert_frame_meta pointer. */
    ctx->framebuffs[i].convert_frame.src = &ctx->framebuffs[i].src;
    ctx->framebuffs[i].convert_frame.dst = &ctx->framebuffs[i].dst;
    ctx->framebuffs[i].convert_frame.priv = &ctx->framebuffs[i];
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

  return ctx;
}

void ut20p_ctx_destroy(ut20p_ctx* ctx) {
  if (!ctx) return;
  if (ctx->blocking) {
    mt_pthread_mutex_destroy(&ctx->pipeline.block_wake_mutex);
    mt_pthread_cond_destroy(&ctx->pipeline.block_wake_cond);
  }
  if (ctx->pipeline.nt_scratch) mt_rte_free(ctx->pipeline.nt_scratch);
  free(ctx->frames_mem);
  free(ctx->framebuffs);
  free(ctx);
}

void ut20p_ctx_enable_blocking(ut20p_ctx* ctx, uint64_t timeout_ns) {
  struct st20p_rx_ctx* p = &ctx->pipeline;
  mt_pthread_mutex_init(&p->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&p->block_wake_cond);
  p->block_timeout_ns = timeout_ns;
  p->block_get = true;
  ctx->blocking = true;
}

void ut20p_wake_block(ut20p_ctx* ctx) {
  st20p_rx_wake_block(&ctx->pipeline);
}

void ut20p_notify_frame_available(ut20p_ctx* ctx) {
  rx_st20p_notify_frame_available(&ctx->pipeline);
}

void ut20p_begin_destroy(ut20p_ctx* ctx) {
  struct st20p_rx_ctx* p = &ctx->pipeline;
  mt_handle_begin_destroy(&p->lc_destroying, &p->type, MT_ST20_HANDLE_PIPELINE_RX);
  rx_st20p_block_wake(p);
}

static void (*ut20p_convert_hook)(void* arg);
static void* ut20p_convert_hook_arg;

void ut20p_set_convert_hook(void (*hook)(void* arg), void* arg) {
  ut20p_convert_hook_arg = arg;
  ut20p_convert_hook = hook;
}

void ut20p_lock_block_mutex(ut20p_ctx* ctx) {
  mt_pthread_mutex_lock(&ctx->pipeline.block_wake_mutex);
}

void ut20p_unlock_block_mutex(ut20p_ctx* ctx) {
  mt_pthread_mutex_unlock(&ctx->pipeline.block_wake_mutex);
}

void ut20p_post_wake_locked(ut20p_ctx* ctx) {
  ctx->pipeline.block_wake_pending = true;
  mt_pthread_cond_broadcast(&ctx->pipeline.block_wake_cond);
}

static int ut20p_stub_convert(struct st_frame* src, struct st_frame* dst) {
  MTL_MAY_UNUSED(src);
  MTL_MAY_UNUSED(dst);
  if (ut20p_convert_hook) ut20p_convert_hook(ut20p_convert_hook_arg);
  return 0;
}

static struct st_frame_converter ut20p_stub_converter = {
    .convert_func = ut20p_stub_convert,
};

void ut20p_ctx_set_internal_converter(ut20p_ctx* ctx) {
  ctx->pipeline.derive = false;
  ctx->pipeline.internal_converter = &ut20p_stub_converter;
}

int ut20p_ctx_set_converting_frames(ut20p_ctx* ctx, enum st_frame_fmt output_fmt,
                                    uint32_t width, uint32_t height, uint32_t flags) {
  struct st20p_rx_ctx* p = &ctx->pipeline;
  const enum st_frame_fmt t_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  size_t src_size = RTE_ALIGN_CEIL(st_frame_size(t_fmt, width, height, false), 64);
  size_t dst_size = RTE_ALIGN_CEIL(st_frame_size(output_fmt, width, height, false), 64);
  int ret;

  p->ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  p->ops.output_fmt = output_fmt;
  p->ops.width = width;
  p->ops.height = height;
  p->ops.flags = flags;
  p->derive = false;
  ret = st_frame_get_converter(t_fmt, output_fmt, &ctx->converter);
  if (ret < 0) return ret;
  p->internal_converter = &ctx->converter;

  ctx->frames_mem = aligned_alloc(64, (src_size + dst_size) * ctx->framebuff_cnt);
  if (!ctx->frames_mem) return -ENOMEM;
  memset(ctx->frames_mem, 0, (src_size + dst_size) * ctx->framebuff_cnt);
  for (int i = 0; i < ctx->framebuff_cnt; i++) {
    struct st20p_rx_frame* fb = &ctx->framebuffs[i];
    uint8_t* mem = ctx->frames_mem + (src_size + dst_size) * i;
    fb->src.fmt = t_fmt;
    fb->dst.fmt = output_fmt;
    fb->src.width = fb->dst.width = width;
    fb->src.height = fb->dst.height = height;
    st_frame_init_plane_single_src(&fb->src, mem, 0);
    st_frame_init_plane_single_src(&fb->dst, mem + src_size, 0);
  }
  return rx_st20p_init_nt_scratch(p, &p->ops);
}

size_t ut20p_nt_scratch_size(const ut20p_ctx* ctx) {
  return ctx->pipeline.nt_scratch ? ctx->pipeline.nt_scratch_size : 0;
}

void* ut20p_nt_scratch(ut20p_ctx* ctx) {
  return ctx->pipeline.nt_scratch;
}

bool ut20p_nt_scratch_held(const ut20p_ctx* ctx) {
  return atomic_load(&ctx->pipeline.nt_scratch_busy) != 0;
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
void ut20p_set_frame_ready(ut20p_ctx* ctx, int idx) {
  atomic_store_explicit(&ctx->framebuffs[idx].stat, ST20P_RX_FRAME_READY,
                        memory_order_release);
}

void ut20p_set_frame_converted(ut20p_ctx* ctx, int idx) {
  atomic_store_explicit(&ctx->framebuffs[idx].stat, ST20P_RX_FRAME_CONVERTED,
                        memory_order_release);
}

struct st20_convert_frame_meta* ut20p_convert_get_frame(ut20p_ctx* ctx) {
  return rx_st20p_convert_get_frame(&ctx->pipeline);
}

int ut20p_convert_put_frame(ut20p_ctx* ctx, struct st20_convert_frame_meta* frame,
                            int result) {
  return rx_st20p_convert_put_frame(&ctx->pipeline, frame, result);
}

int ut20p_convert_frame_idx(const struct st20_convert_frame_meta* meta) {
  const struct st20p_rx_frame* framebuff = meta->priv;
  return framebuff->idx;
}
/* ── concurrency-test helpers ─────────────────────────────────────────── */

int ut20p_frame_idx(const struct st_frame* frame) {
  const struct st20p_rx_frame* framebuff = frame->priv;
  return framebuff->idx;
}

int ut20p_framebuff_cnt(const ut20p_ctx* ctx) {
  return ctx->framebuff_cnt;
}

int ut20p_frame_stat(const ut20p_ctx* ctx, int i) {
  return (int)ctx->framebuffs[i].stat;
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

st20p_rx_handle ut20p_handle(ut20p_ctx* ctx) {
  return &ctx->pipeline;
}

void ut20p_force_destroying(ut20p_ctx* ctx) {
  atomic_store_explicit(&ctx->pipeline.lc_destroying, 1, memory_order_release);
}
