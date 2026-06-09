/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the NEW-API (unified session) video TX tests.
 *
 * Includes the production TX .c so the static datapath/buffer helpers and the
 * get_next_frame / frame_done callbacks are reachable and operate on our
 * hand-built session. convert.derive=true so buffer_get/put take the
 * passthrough branch (pub->data = frame_trans->addr) and never touch a
 * converter, mirroring how st20_rx_harness sets derive=true.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "new_api/mt_session_video_tx.c"
#pragma GCC diagnostic pop

#include "common/ut_common.h"

/*
 * Stubs for the transport libmtl symbols the TX .c calls on the paths we
 * exercise. The real implementations dereference a live st20_tx handle; ours
 * owns a fake handle, so --allow-multiple-definition lets these win.
 */

int st20_tx_set_ext_frame(st20_tx_handle handle, uint16_t idx,
                          struct st20_ext_frame* ext_frame) {
  (void)handle;
  (void)idx;
  (void)ext_frame;
  return 0;
}

int st20_tx_get_session_stats(st20_tx_handle handle, struct st20_tx_user_stats* stats) {
  (void)handle;
  /* io_stats_get is pure passthrough; return zeroed transport stats so the
   * test can show it is a distinct surface from the abstract counters. */
  if (stats) memset(stats, 0, sizeof(*stats));
  return 0;
}

int st20_tx_reset_session_stats(st20_tx_handle handle) {
  (void)handle;
  return 0;
}

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20tx_ctx {
  struct mtl_session_impl s;
  struct st_tx_video_session_impl tx_impl;
  struct video_tx_ctx vctx;
  struct mtl_main_impl impl;
  struct st_frame_trans* frames;
  uint8_t* frame_storage;
  int framebuff_cnt;
};

#include "new_api/st20_tx_harness.h"

#define UT20TX_FRAME_STRIDE 64 /* per-frame sentinel storage */

/* ── ptp stub (returns the test-controlled wall clock) ─────────────────── */

static uint64_t ut20tx_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  return impl->ptp_usync;
}

/* ── init ─────────────────────────────────────────────────────────────── */

int ut20tx_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut20tx_ctx* ut20tx_ctx_create(int framebuff_cnt) {
  static uint32_t ring_seq;

  ut20tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->framebuff_cnt = framebuff_cnt;

  ctx->frames = calloc(framebuff_cnt, sizeof(struct st_frame_trans));
  ctx->frame_storage = calloc(framebuff_cnt, UT20TX_FRAME_STRIDE);
  if (!ctx->frames || !ctx->frame_storage) {
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < framebuff_cnt; i++) {
    ctx->frames[i].idx = i;
    ctx->frames[i].addr = &ctx->frame_storage[i * UT20TX_FRAME_STRIDE];
    rte_atomic32_set(&ctx->frames[i].refcnt, 0);
  }

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.ptp_usync = 0;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->impl.inf[i].parent = &ctx->impl;
    ctx->impl.inf[i].port = i;
    ctx->impl.inf[i].ptp_get_time_fn = ut20tx_ptp_time;
  }

  struct st_tx_video_session_impl* tx = &ctx->tx_impl;
  tx->idx = 0;
  tx->socket_id = rte_socket_id();
  tx->st20_frames = ctx->frames;
  tx->st20_frames_cnt = framebuff_cnt;

  struct mtl_session_impl* s = &ctx->s;
  s->vt = &mtl_video_tx_vtable;
  s->magic = MTL_SESSION_MAGIC_VIDEO_TX;
  s->type = MTL_TYPE_VIDEO;
  s->direction = MTL_SESSION_TX;
  s->parent = &ctx->impl;
  s->socket_id = rte_socket_id();
  s->ownership = MTL_BUFFER_LIBRARY_OWNED;
  s->flags = 0;
  s->stopped = 0;
  s->event_fd = -1;
  s->inner.video_tx = tx;
  snprintf(s->name, sizeof(s->name), "ut_new_tx");

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

  char ring_name[RTE_RING_NAMESIZE];
  snprintf(ring_name, sizeof(ring_name), "utntx_%u", ring_seq++);
  s->event_ring =
      rte_ring_create(ring_name, 64, s->socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!s->event_ring) {
    free(s->buffers);
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }

  struct video_tx_ctx* v = &ctx->vctx;
  v->session = s;
  v->handle = (st20_tx_handle)(uintptr_t)0x1; /* non-NULL sentinel */
  v->convert.derive = true;
  v->convert.transport_fmt = ST20_FMT_YUV_422_10BIT;
  v->convert.transport_frame_size = UT20TX_FRAME_STRIDE;
  v->convert.width = 1920;
  v->convert.height = 1080;
  v->frame_state = calloc(framebuff_cnt, sizeof(enum tx_frame_state));
  if (!v->frame_state) {
    rte_ring_free(s->event_ring);
    free(s->buffers);
    free(ctx->frames);
    free(ctx->frame_storage);
    free(ctx);
    return NULL;
  }
  v->frame_cnt = framebuff_cnt;
  for (int i = 0; i < framebuff_cnt; i++) v->frame_state[i] = TX_FRAME_FREE;
  v->fps = ST_FPS_P59_94;
  v->drop_when_late = false;

  tx->ops.priv = v; /* tx_ctx_from_session() reads this */

  return ctx;
}

void ut20tx_ctx_destroy(ut20tx_ctx* ctx) {
  if (!ctx) return;
  mtl_session_user_buf_uinit(&ctx->s);
  if (ctx->s.event_ring) rte_ring_free(ctx->s.event_ring);
  free(ctx->vctx.frame_state);
  free(ctx->s.buffers);
  free(ctx->frames);
  free(ctx->frame_storage);
  free(ctx);
}

/* ── controls ─────────────────────────────────────────────────────────── */

void ut20tx_set_drop_when_late(ut20tx_ctx* ctx, bool on) {
  ctx->vctx.drop_when_late = on;
}

void ut20tx_set_user_pacing(ut20tx_ctx* ctx, bool on) {
  if (on)
    ctx->s.flags |= MTL_SESSION_FLAG_USER_PACING;
  else
    ctx->s.flags &= ~(uint32_t)MTL_SESSION_FLAG_USER_PACING;
}

void ut20tx_set_fps(ut20tx_ctx* ctx, enum st_fps fps) {
  ctx->vctx.fps = fps;
}

void ut20tx_set_ptp_now(ut20tx_ctx* ctx, uint64_t ns) {
  ctx->impl.ptp_usync = ns;
}

int ut20tx_set_user_owned(ut20tx_ctx* ctx) {
  ctx->s.ownership = MTL_BUFFER_USER_OWNED;
  return mtl_session_user_buf_init(&ctx->s, ctx->framebuff_cnt);
}

int ut20tx_post_user_buffer(ut20tx_ctx* ctx, void* data, void* user_ctx) {
  return mtl_session_user_buf_enqueue(&ctx->s, data, 0, UT20TX_FRAME_STRIDE, user_ctx);
}

void ut20tx_frame_set_timestamp(ut20tx_ctx* ctx, uint16_t idx, uint64_t tai_ns) {
  ctx->frames[idx].tv_meta.timestamp = tai_ns;
  ctx->frames[idx].tv_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
}

void* ut20tx_user_buf_ctx(ut20tx_ctx* ctx, uint16_t idx) {
  return ctx->s.user_buf_ctx ? ctx->s.user_buf_ctx[idx] : NULL;
}

/* ── state machine drivers ────────────────────────────────────────────── */

mtl_buffer_t* ut20tx_buffer_get(ut20tx_ctx* ctx) {
  mtl_buffer_t* buf = NULL;
  int ret = mtl_video_tx_vtable.buffer_get(&ctx->s, &buf, 0 /* non-blocking */);
  if (ret != 0) return NULL;
  return buf;
}

void ut20tx_buffer_set_timestamp(mtl_buffer_t* buf, uint64_t tai_ns) {
  buf->timestamp = tai_ns;
  buf->tfmt = ST10_TIMESTAMP_FMT_TAI;
}

void ut20tx_buffer_set_user_meta(mtl_buffer_t* buf, void* meta, size_t size) {
  buf->user_meta = meta;
  buf->user_meta_size = size;
}

int ut20tx_buffer_put(ut20tx_ctx* ctx, mtl_buffer_t* buf) {
  return mtl_video_tx_vtable.buffer_put(&ctx->s, buf);
}

int ut20tx_get_next_frame(ut20tx_ctx* ctx, uint16_t* idx) {
  struct st20_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  uint16_t local = 0;
  int ret = video_tx_get_next_frame(&ctx->vctx, &local, &meta);
  if (ret == 0 && idx) *idx = local;
  return ret;
}

int ut20tx_frame_done(ut20tx_ctx* ctx, uint16_t idx) {
  struct st20_tx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  return video_tx_notify_frame_done(&ctx->vctx, idx, &meta);
}

/* ── inspection ───────────────────────────────────────────────────────── */

int ut20tx_frame_state(ut20tx_ctx* ctx, uint16_t idx) {
  return (int)ctx->vctx.frame_state[idx];
}

const void* ut20tx_frame_user_meta(ut20tx_ctx* ctx, uint16_t idx) {
  return ctx->frames[idx].tv_meta.user_meta;
}

void* ut20tx_frame_addr(ut20tx_ctx* ctx, uint16_t idx) {
  return ctx->frames[idx].addr;
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20tx_buffers_processed(const ut20tx_ctx* ctx) {
  return ctx->s.stats.buffers_processed;
}

uint64_t ut20tx_buffers_dropped(const ut20tx_ctx* ctx) {
  return ctx->s.stats.buffers_dropped;
}

uint64_t ut20tx_bytes_processed(const ut20tx_ctx* ctx) {
  return ctx->s.stats.bytes_processed;
}

/* ── vtable stats surfaces ────────────────────────────────────────────── */

int ut20tx_stats_get(ut20tx_ctx* ctx, mtl_session_stats_t* stats) {
  return mtl_video_tx_vtable.stats_get(&ctx->s, stats);
}

int ut20tx_io_stats_get(ut20tx_ctx* ctx, struct st20_tx_user_stats* stats) {
  return mtl_video_tx_vtable.io_stats_get(&ctx->s, stats, sizeof(*stats));
}

int ut20tx_reset_stats(ut20tx_ctx* ctx) {
  return mtl_video_tx_vtable.stats_reset(&ctx->s);
}

/* ── event drain ──────────────────────────────────────────────────────── */

int ut20tx_poll_event(ut20tx_ctx* ctx, mtl_event_t* ev) {
  return mtl_video_tx_vtable.event_poll(&ctx->s, ev, 0 /* non-blocking */);
}
