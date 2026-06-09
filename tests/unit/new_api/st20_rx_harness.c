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

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20rx_ctx {
  struct mtl_session_impl s;
  struct st_rx_video_session_impl rx_impl;
  struct video_rx_ctx vctx;
  struct st_frame_trans* frames;
  uint8_t* frame_storage;
  int framebuff_cnt;
  uint32_t enq_seq; /* advanced only on a successful enqueue */
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
  s->event_ring = NULL; /* events are best-effort; post just returns -EINVAL */
  s->event_fd = -1;
  s->inner.video_rx = rx;
  snprintf(s->name, sizeof(s->name), "ut_new_rx");

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

  rx->ops.priv = v; /* rx_ctx_from_session() reads this */

  return ctx;
}

void ut20rx_ctx_destroy(ut20rx_ctx* ctx) {
  if (!ctx) return;
  if (ctx->vctx.ready_ring) rte_ring_free(ctx->vctx.ready_ring);
  free(ctx->s.buffers);
  free(ctx->frames);
  free(ctx->frame_storage);
  free(ctx);
}

/* ── inject one frame via video_rx_notify_frame_ready ─────────────────── */

int ut20rx_inject_frame(ut20rx_ctx* ctx, enum st_frame_status status,
                        uint32_t timestamp) {
  /* Pick the frame address from the cyclic pool. enq_seq advances only on a
   * successful enqueue, so the live (undrained) window of the FIFO ready_ring
   * always maps to distinct frame_trans slots. */
  void* frame = ctx->frames[ctx->enq_seq % ctx->framebuff_cnt].addr;

  struct st20_rx_frame_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.status = status;
  meta.timestamp = timestamp;
  meta.rtp_timestamp = timestamp;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.pkts_total = 1;

  uint64_t dropped_before = ctx->s.stats.buffers_dropped;
  video_rx_notify_frame_ready(&ctx->vctx, frame, &meta);

  if (ctx->s.stats.buffers_dropped != dropped_before) {
    /* ready_ring was full — frame returned to library, counted as dropped. */
    return -ENOSPC;
  }
  ctx->enq_seq++;
  return 0;
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
