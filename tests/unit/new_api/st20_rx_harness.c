/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the NEW-API (unified session) video RX frame-count tests.
 *
 * Drives video_rx_notify_frame_ready() directly with a synthetic
 * st20_rx_frame_meta — bypasses the transport session entirely. The
 * unified-session counters under test (buffers_processed / buffers_dropped)
 * are about producer/consumer flow, not packet semantics, so transport
 * realism is not required.
 *
 * convert.derive=true so buffer_get takes the passthrough branch
 * (pub->data = frame_trans->addr) and never touches a converter, mirroring
 * how st20p_harness sets derive=true.
 */

#include <stdlib.h>
#include <string.h>

/*
 * Include the production RX .c so the static datapath/buffer helpers are
 * reachable and our hand-built session is the one they operate on. Disable
 * USDT to avoid linker references to probe semaphores.
 */
#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "new_api/mt_session_video_rx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/*
 * Stubs for the transport libmtl symbols the RX .c calls on the paths we
 * exercise. The real implementations dereference a live st20_rx handle (HW /
 * a real session); ours own a fake handle, so --allow-multiple-definition lets
 * these win.
 */

int st20_rx_put_framebuff(st20_rx_handle handle, void* frame) {
  (void)handle;
  (void)frame; /* transport-side framebuf release is a no-op for the harness */
  return 0;
}

int st20_rx_get_session_stats(st20_rx_handle handle, struct st20_rx_user_stats* stats) {
  (void)handle;
  /* io_stats_get is pure passthrough; return zeroed transport stats so the
   * test can show it is a distinct surface from the abstract counters. */
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st20_rx_reset_session_stats(st20_rx_handle handle) {
  (void)handle;
  return 0;
}

/*
 * Stub the format converter (defined in a separate TU, mt_session_video_common.c).
 * Records each invocation so the convert-vs-derive contract can be asserted, and
 * copies a byte so the destination buffer is observably written.
 */
static int ut20rx_convert_calls_g;

int video_convert_frame(struct video_convert_ctx* cvt, void* src_data,
                        mtl_iova_t src_iova, size_t src_size, void* dst_data,
                        mtl_iova_t dst_iova, size_t dst_size, bool is_tx) {
  (void)cvt;
  (void)src_iova;
  (void)dst_iova;
  (void)is_tx;
  ut20rx_convert_calls_g++;
  if (dst_data && src_data && dst_size && src_size) {
    *(uint8_t*)dst_data = *(const uint8_t*)src_data;
  }
  return 0;
}

/*
 * Stub st20_rx_create so the framebuff_cnt clamp in mtl_video_rx_session_init is
 * reachable without a NIC: it captures the clamped ops.framebuff_cnt and returns
 * NULL, making init bail out before any transport handle is dereferenced.
 */
static uint16_t ut20rx_captured_framebuff_cnt;

st20_rx_handle st20_rx_create(mtl_handle mt, struct st20_rx_ops* ops) {
  (void)mt;
  ut20rx_captured_framebuff_cnt = ops ? ops->framebuff_cnt : 0;
  return NULL;
}

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20rx_ctx {
  struct mtl_session_impl s;
  struct st_rx_video_session_impl rx_impl;
  struct video_rx_ctx vctx;
  struct mtl_main_impl impl;
  struct st_frame_trans* frames;
  uint8_t* frame_storage;
  void* app_buf_slots[64]; /* per-slot convert destinations (!derive) */
  int framebuff_cnt;
  uint32_t enq_seq; /* advanced only on a successful enqueue */
  bool user_owned;  /* user_buf ring initialized → uinit at destroy */
  bool buffers_rte; /* wrapper pool came from mtl_session_init_buffers (rte) */
};

#include "new_api/st20_rx_harness.h"

#define UT20RX_FRAME_STRIDE 64 /* per-frame sentinel storage, never read */

/* ── init ─────────────────────────────────────────────────────────────── */

int ut20rx_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut20rx_ctx* ut20rx_ctx_create(int framebuff_cnt) {
  static uint32_t ring_seq;

  ut20rx_convert_calls_g = 0;

  ut20rx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->framebuff_cnt = framebuff_cnt;

  ctx->frames = calloc(framebuff_cnt, sizeof(struct st_frame_trans));
  ctx->frame_storage = calloc(framebuff_cnt, UT20RX_FRAME_STRIDE);
  if (!ctx->frames || !ctx->frame_storage) {
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->frames[i].idx = i;
    ctx->frames[i].addr = &ctx->frame_storage[i * UT20RX_FRAME_STRIDE];
    rte_atomic32_set(&ctx->frames[i].refcnt, 0);
  }

  struct st_rx_video_session_impl* rx = &ctx->rx_impl;
  rx->idx = 0;
  rx->socket_id = rte_socket_id();
  rx->st20_frames = ctx->frames;
  rx->st20_frames_cnt = framebuff_cnt;

  struct mtl_session_impl* s = &ctx->s;
  s->vt = &mtl_video_rx_vtable;
  s->magic = MTL_SESSION_MAGIC_VIDEO_RX;
  s->type = MTL_TYPE_VIDEO;
  s->direction = MTL_SESSION_RX;
  s->socket_id = rte_socket_id();
  s->ownership = MTL_BUFFER_LIBRARY_OWNED;
  s->stopped = 0;
  s->event_fd = -1;
  s->inner.video_rx = rx;
  snprintf(s->name, sizeof(s->name), "ut_new_rx");

  ctx->impl.type = MT_HANDLE_MAIN;
  s->parent = &ctx->impl;

  s->buffer_count = framebuff_cnt;
  s->buffers = calloc(framebuff_cnt, sizeof(struct mtl_buffer_impl));
  if (!s->buffers) {
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    s->buffers[i].session = s;
    s->buffers[i].idx = i;
  }

  struct video_rx_ctx* v = &ctx->vctx;
  v->session = s;
  v->handle = (st20_rx_handle)(uintptr_t)0x1; /* non-NULL sentinel */
  v->convert.derive = true;
  v->convert.transport_frame_size = 1;

  char ring_name[RTE_RING_NAMESIZE];
  snprintf(ring_name, sizeof(ring_name), "utnrx_%u", ring_seq++);
  /* EXACT_SZ: usable capacity == framebuff_cnt → deterministic overflow. */
  v->ready_ring = rte_ring_create(ring_name, framebuff_cnt, s->socket_id,
                                  RING_F_SP_ENQ | RING_F_SC_DEQ | RING_F_EXACT_SZ);
  if (!v->ready_ring) {
    free(s->buffers);
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }

  snprintf(ring_name, sizeof(ring_name), "utnrxe_%u", ring_seq++);
  s->event_ring =
      rte_ring_create(ring_name, 64, s->socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!s->event_ring) {
    rte_ring_free(v->ready_ring);
    free(s->buffers);
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }

  rx->ops.priv = v; /* rx_ctx_from_session() reads this */

  return ctx;
}

void ut20rx_ctx_destroy(ut20rx_ctx* ctx) {
  if (!ctx) return;
  if (ctx->user_owned) mtl_session_user_buf_uinit(&ctx->s);
  if (ctx->s.event_ring) {
    void* obj = NULL;
    while (rte_ring_dequeue(ctx->s.event_ring, &obj) == 0 && obj) {
      mt_rte_free(obj);
      obj = NULL;
    }
    rte_ring_free(ctx->s.event_ring);
  }
  if (ctx->vctx.ready_ring) rte_ring_free(ctx->vctx.ready_ring);
  if (ctx->buffers_rte)
    mtl_session_buffers_uinit(&ctx->s);
  else
    free(ctx->s.buffers);
  free(ctx->frames);
  free(ctx->frame_storage);
  free(ctx);
}

/* ── inject one frame via video_rx_notify_frame_ready ─────────────────── */

int ut20rx_inject_frame(ut20rx_ctx* ctx, enum st_frame_status status,
                        uint32_t timestamp) {
  struct st20_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.status = status;
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.pkts_total = 1;
  return ut20rx_inject_meta(ctx, &meta);
}

int ut20rx_inject_meta(ut20rx_ctx* ctx, const struct st20_rx_frame_meta* meta) {
  /* Pick the frame address from the cyclic pool. enq_seq advances only on a
   * successful enqueue, so the live (undrained) window of the FIFO ready_ring
   * always maps to distinct frame_trans slots. */
  void* frame = ctx->frames[ctx->enq_seq % ctx->framebuff_cnt].addr;

  struct st20_rx_frame_meta local = *meta;
  uint64_t dropped_before = ctx->s.stats.buffers_dropped;
  video_rx_notify_frame_ready(&ctx->vctx, frame, &local);

  if (ctx->s.stats.buffers_dropped != dropped_before) {
    /* ready_ring was full — frame returned to library, counted as dropped. */
    return -ENOSPC;
  }
  ctx->enq_seq++;
  return 0;
}

void ut20rx_set_frame_user_meta(ut20rx_ctx* ctx, int idx, void* meta, size_t size) {
  ctx->frames[idx].user_meta = meta;
  ctx->frames[idx].user_meta_data_size = size;
}

void ut20rx_enable_convert(ut20rx_ctx* ctx, enum st_frame_fmt app_fmt, void* app_buf,
                           size_t app_size) {
  struct video_convert_ctx* cvt = &ctx->vctx.convert;
  cvt->derive = false;
  cvt->frame_fmt = app_fmt;
  cvt->app_frame_size = app_size;
  for (int i = 0; i < ctx->framebuff_cnt && i < 64; i++) ctx->app_buf_slots[i] = app_buf;
  cvt->app_bufs = ctx->app_buf_slots;
  cvt->app_bufs_cnt = ctx->framebuff_cnt;
}

int ut20rx_convert_calls(const ut20rx_ctx* ctx) {
  (void)ctx;
  return ut20rx_convert_calls_g;
}

int ut20rx_notify_detected(ut20rx_ctx* ctx, uint32_t width, uint32_t height,
                           enum st_fps fps, enum st20_packing packing, bool interlaced) {
  struct st20_detect_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.width = width;
  meta.height = height;
  meta.fps = fps;
  meta.packing = packing;
  meta.interlaced = interlaced;
  struct st20_detect_reply reply;
  memset(&reply, 0, sizeof(reply));
  return video_rx_notify_detected(&ctx->vctx, &meta, &reply);
}

int ut20rx_poll_event(ut20rx_ctx* ctx, mtl_event_t* event) {
  return mtl_video_rx_vtable.event_poll(&ctx->s, event, 0 /* non-blocking */);
}

int ut20rx_enable_user_owned_post(ut20rx_ctx* ctx) {
  ctx->s.ownership = MTL_BUFFER_USER_OWNED;
  ctx->vctx.user_query_ext_frame = NULL;
  int ret = mtl_session_user_buf_init(&ctx->s, ctx->framebuff_cnt);
  if (ret == 0) ctx->user_owned = true;
  return ret;
}

static int ut20rx_stub_query_ext(void* priv, struct st_ext_frame* ext,
                                 struct mtl_buffer* frame_meta) {
  (void)priv;
  (void)ext;
  (void)frame_meta;
  return 0;
}

int ut20rx_enable_user_owned_query_ext(ut20rx_ctx* ctx) {
  ctx->s.ownership = MTL_BUFFER_USER_OWNED;
  ctx->vctx.user_query_ext_frame = ut20rx_stub_query_ext;
  int ret = mtl_session_user_buf_init(&ctx->s, ctx->framebuff_cnt);
  if (ret == 0) ctx->user_owned = true;
  return ret;
}

uint32_t ut20rx_frame_count(ut20rx_ctx* ctx) {
  return mtl_session_video_frame_count(&ctx->s);
}

uint32_t ut20rx_buffer_count(ut20rx_ctx* ctx) {
  return ctx->s.buffer_count;
}

int ut20rx_init_buffers(ut20rx_ctx* ctx) {
  free(ctx->s.buffers); /* drop the libc pool ut20rx_ctx_create allocated */
  ctx->s.buffers = NULL;
  ctx->s.buffer_count = 0;
  ctx->buffers_rte = true;
  return mtl_session_init_buffers(&ctx->s);
}

int ut20rx_mem_register(ut20rx_ctx* ctx, void* addr, size_t size) {
  mtl_dma_mem_t* handle = NULL;
  return mtl_video_rx_vtable.mem_register(&ctx->s, addr, size, &handle);
}

int ut20rx_post_user_buffer(ut20rx_ctx* ctx, void* data, size_t size, void* user_ctx) {
  return mtl_video_rx_vtable.buffer_post(&ctx->s, data, size, user_ctx);
}

int ut20rx_query_ext_frame(ut20rx_ctx* ctx, struct st20_ext_frame* out,
                           struct st20_rx_frame_meta* meta) {
  return video_rx_query_ext_frame_wrapper(&ctx->vctx, out, meta);
}

uint16_t ut20rx_clamp_framebuff_cnt(uint32_t requested) {
  struct mtl_session_impl s;
  memset(&s, 0, sizeof(s));
  s.socket_id = rte_socket_id();
  s.ownership = MTL_BUFFER_LIBRARY_OWNED;
  snprintf(s.name, sizeof(s.name), "ut_clamp");

  struct mtl_main_impl impl;
  memset(&impl, 0, sizeof(impl));
  impl.type = MT_HANDLE_MAIN;

  mtl_video_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.base.name = "ut_clamp";
  cfg.base.num_buffers = requested;
  cfg.width = 1920;
  cfg.height = 1080;
  cfg.fps = ST_FPS_P59_94;
  /* derive pair so video_convert_ctx_init returns early (no converter lookup) */
  cfg.transport_fmt = ST20_FMT_YUV_422_10BIT;
  cfg.frame_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;

  ut20rx_captured_framebuff_cnt = 0;
  (void)mtl_video_rx_session_init(&s, &impl, &cfg);
  return ut20rx_captured_framebuff_cnt;
}

/* ── buffer get/put ───────────────────────────────────────────────────── */

mtl_buffer_t* ut20rx_buffer_get(ut20rx_ctx* ctx) {
  mtl_buffer_t* buf = NULL;
  int ret = mtl_video_rx_vtable.buffer_get(&ctx->s, &buf, 0 /* non-blocking */);
  if (ret != 0) return NULL;
  return buf;
}

int ut20rx_buffer_put(ut20rx_ctx* ctx, mtl_buffer_t* buf) {
  return mtl_video_rx_vtable.buffer_put(&ctx->s, buf);
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20rx_buffers_processed(const ut20rx_ctx* ctx) {
  return ctx->s.stats.buffers_processed;
}

uint64_t ut20rx_buffers_dropped(const ut20rx_ctx* ctx) {
  return ctx->s.stats.buffers_dropped;
}

uint64_t ut20rx_bytes_processed(const ut20rx_ctx* ctx) {
  return ctx->s.stats.bytes_processed;
}

/* ── vtable stats surfaces ────────────────────────────────────────────── */

int ut20rx_stats_get(ut20rx_ctx* ctx, mtl_session_stats_t* stats) {
  return mtl_video_rx_vtable.stats_get(&ctx->s, stats);
}

int ut20rx_io_stats_get(ut20rx_ctx* ctx, struct st20_rx_user_stats* stats) {
  return mtl_video_rx_vtable.io_stats_get(&ctx->s, stats, sizeof(*stats));
}

int ut20rx_reset_stats(ut20rx_ctx* ctx) {
  return mtl_video_rx_vtable.stats_reset(&ctx->s);
}
