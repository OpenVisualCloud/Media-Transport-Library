/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-30 (audio) TX pacing/timestamp unit tests.
 *
 * Includes the production st_tx_audio_session.c directly so
 * tx_audio_session_tasklet_frame() and the file-local statics it calls
 * (tx_audio_session_sync_pacing, tx_audio_pacing_required_tai,
 * tx_audio_pacing_update_rtp_time_stamp, ...) become visible in this
 * translation unit. Non-static symbols duplicate those in libmtl; this
 * object's definition preempts the shared library's. USDT is disabled to
 * avoid probe-semaphore link references.
 *
 * mt_get_tsc() is mocked with a preprocessor seam instead of any production
 * source change -- see st20_tx_harness.c for the rationale. mt_get_ptp_time()
 * needs no such seam; it dispatches through the pre-existing production
 * ptp_get_time_fn function-pointer field on struct mt_interface.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"
#include "st2110/st_tx_audio_session.h"

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut_txa30_ctx {
  struct mtl_main_impl impl;
  struct st_tx_audio_sessions_mgr mgr;
  struct st_tx_audio_session_impl session;
  struct st_frame_trans frame;
  uint8_t frame_data[64];
  uint64_t mock_ptp_ns;
  uint64_t mock_tsc_ns;
  enum st10_timestamp_fmt app_tfmt;
  uint64_t app_timestamp;
  int get_next_frame_calls;
  int notify_frame_done_calls;
  struct st30_tx_frame_meta notify_frame_done_meta;
  uint32_t wire_rtp_timestamps[64];
  int wire_rtp_count;
};

#include "session/st30_tx_harness.h"

/* ── mocked time sources ──────────────────────────────────────────────── */

static uint64_t ut_txa30_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  struct ut_txa30_ctx* ctx = (struct ut_txa30_ctx*)impl; /* impl is ctx's first member */
  return ctx->mock_ptp_ns;
}

static uint64_t ut_txa30_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_txa30_ctx* ctx = (struct ut_txa30_ctx*)impl;
  return ctx->mock_tsc_ns;
}

static int ut_txa30_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st30_tx_frame_meta* meta) {
  struct ut_txa30_ctx* ctx = priv;
  ctx->get_next_frame_calls++;
  *next_frame_idx = 0;
  meta->tfmt = ctx->app_tfmt;
  meta->timestamp = ctx->app_timestamp;
  return 0;
}

static int ut_txa30_notify_frame_done(void* priv, uint16_t frame_idx,
                                      struct st30_tx_frame_meta* meta) {
  struct ut_txa30_ctx* ctx = priv;
  (void)frame_idx;
  ctx->notify_frame_done_calls++;
  ctx->notify_frame_done_meta = *meta;
  return 0;
}

/* Seam: from here on, calls to mt_get_tsc() written inside st_tx_audio_session.c
 * resolve to our mock instead. st30_tx_get_session_stats()/st30_tx_reset_session_stats()
 * are renamed away too: pipeline/st30p_tx_harness.c already defines its own stub
 * under those exact public names, and this file's #include of the full production
 * .c would otherwise redefine them a second time in the same UnitTest binary. */
#define mt_get_tsc ut_txa30_tsc_time_fn
#define st30_tx_get_session_stats ut_txa30_unused_get_session_stats
#define st30_tx_reset_session_stats ut_txa30_unused_reset_session_stats
#include "st2110/st_tx_audio_session.c"
#undef st30_tx_reset_session_stats
#undef st30_tx_get_session_stats
#undef mt_get_tsc

/* ── init (delegates to common) ───────────────────────────────────────── */

int ut_txa30_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut_txa30_ctx* ut_txa30_create(void) {
  ut_txa30_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut_txa30_ptp_time_fn;
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.max_idx = 1;
  rte_spinlock_init(&ctx->mgr.mutex[0]);

  struct st_tx_audio_session_impl* s = &ctx->session;
  s->idx = 0;
  s->mgr = &ctx->mgr;
  s->active = true;
  s->usdt_dump_fd =
      -1; /* 0 is a live fd (stdin); never let the dump-close path touch it */
  s->pacing.trs = NS_PER_MS;           /* 1ms ptime, round number for simple math */
  s->pacing.pkt_time_sampling = 48.0L; /* 48 samples/packet @ 48kHz/1ms */
  s->pacing.max_onward_epochs = 3;
  s->pacing.max_late_epochs = 100;
  s->ops.sampling = ST30_SAMPLING_48K;
  s->ops.ptime = ST30_PTIME_1MS;
  s->ops.channel = 1;
  s->ops.fmt = ST30_FMT_PCM16;
  s->ops.get_next_frame = ut_txa30_get_next_frame;
  s->ops.notify_frame_done = ut_txa30_notify_frame_done;
  s->ops.priv = ctx;
  ctx->mgr.sessions[0] = s;

  return ctx;
}

void ut_txa30_destroy(ut_txa30_ctx* ctx) {
  free(ctx);
}

/* ── pacing setup ─────────────────────────────────────────────────────── */

void ut_txa30_set_user_pacing(ut_txa30_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST30_TX_FLAG_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST30_TX_FLAG_USER_PACING;
}

void ut_txa30_set_user_timestamp(ut_txa30_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST30_TX_FLAG_USER_TIMESTAMP;
  else
    ctx->session.ops.flags &= ~ST30_TX_FLAG_USER_TIMESTAMP;
}

void ut_txa30_set_rtp_timestamp_delta_us(ut_txa30_ctx* ctx, int32_t delta_us) {
  ctx->session.ops.rtp_timestamp_delta_us = delta_us;
}

void ut_txa30_set_mock_ptp_time(ut_txa30_ctx* ctx, uint64_t ptp_ns) {
  ctx->mock_ptp_ns = ptp_ns;
}

void ut_txa30_set_mock_tsc_time(ut_txa30_ctx* ctx, uint64_t tsc_ns) {
  ctx->mock_tsc_ns = tsc_ns;
}

/* ── code under test ──────────────────────────────────────────────────── */

static uint32_t ut_txa30_read_rtp_timestamp(struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st_rfc3550_rtp_hdr* rtp =
      (struct st_rfc3550_rtp_hdr*)((uint8_t*)udp + sizeof(struct rte_udp_hdr));
  return ntohl(rtp->tmstamp);
}

int ut_txa30_run_frame_tasklet(ut_txa30_ctx* ctx, enum st10_timestamp_fmt tfmt,
                               uint64_t timestamp, int total_pkts) {
  static unsigned int test_idx;
  struct st_tx_audio_session_impl* s = &ctx->session;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  int ret = -1;

  if (total_pkts <= 0 || (size_t)total_pkts > sizeof(ctx->frame_data)) return -EINVAL;

  snprintf(pool_name, sizeof(pool_name), "ut_txa30_pool_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  s->trans_ring[MTL_SESSION_PORT_P] = mt_u64_fifo_init(32, rte_socket_id());
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !s->trans_ring[MTL_SESSION_PORT_P])
    goto out;
  s->trans_ring_thresh = 32;

  memset(&ctx->frame, 0, sizeof(ctx->frame));
  memset(ctx->frame_data, 0, sizeof(ctx->frame_data));
  ctx->frame.addr = ctx->frame_data;
  ctx->frame.idx = 0;
  ctx->frame.priv = s;
  s->st30_frames = &ctx->frame;
  s->st30_frames_cnt = 1;
  s->pkt_len = 1;
  s->st30_total_pkts = total_pkts;
  s->st30_pkt_idx = 0;
  s->st30_frame_idx = 0;
  s->st30_frame_stat = ST30_TX_STAT_WAIT_FRAME;
  s->calculate_time_cursor = true;
  s->tx_no_chain = true;
  s->ops.num_port = 1;
  s->ops.type = ST30_TYPE_FRAME_LEVEL;
  ctx->app_tfmt = tfmt;
  ctx->app_timestamp = timestamp;
  ctx->get_next_frame_calls = 0;
  ctx->notify_frame_done_calls = 0;
  ctx->wire_rtp_count = 0;
  memset(&ctx->notify_frame_done_meta, 0, sizeof(ctx->notify_frame_done_meta));

  for (int i = 0; i < total_pkts; i++) {
    tx_audio_session_tasklet_frame(&ctx->impl, s);

    struct rte_mbuf* pkt = NULL;
    if (mt_u64_fifo_get(s->trans_ring[MTL_SESSION_PORT_P], (uint64_t*)&pkt) < 0) goto out;
    ctx->wire_rtp_timestamps[ctx->wire_rtp_count++] = ut_txa30_read_rtp_timestamp(pkt);
    rte_pktmbuf_free(pkt);
  }
  ret = 0;

out:
  if (s->trans_ring[MTL_SESSION_PORT_P]) {
    struct rte_mbuf* leftover;
    while (mt_u64_fifo_get(s->trans_ring[MTL_SESSION_PORT_P], (uint64_t*)&leftover) == 0)
      rte_pktmbuf_free(leftover);
    mt_u64_fifo_uinit(s->trans_ring[MTL_SESSION_PORT_P]);
    s->trans_ring[MTL_SESSION_PORT_P] = NULL;
  }
  rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  s->st30_frames = NULL;
  return ret;
}

/* ── accessors ─────────────────────────────────────────────────────────── */

uint64_t ut_txa30_stat_error_user_timestamp(const ut_txa30_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_error_user_timestamp;
}

int ut_txa30_notify_frame_done_calls(const ut_txa30_ctx* ctx) {
  return ctx->notify_frame_done_calls;
}

uint64_t ut_txa30_notify_frame_done_timestamp(const ut_txa30_ctx* ctx) {
  return ctx->notify_frame_done_meta.timestamp;
}

uint32_t ut_txa30_notify_frame_done_rtp_timestamp(const ut_txa30_ctx* ctx) {
  return ctx->notify_frame_done_meta.rtp_timestamp;
}

int ut_txa30_wire_rtp_timestamp_count(const ut_txa30_ctx* ctx) {
  return ctx->wire_rtp_count;
}

uint32_t ut_txa30_wire_rtp_timestamp(const ut_txa30_ctx* ctx, int i) {
  return ctx->wire_rtp_timestamps[i];
}
