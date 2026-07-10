/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-20 (video) TX epoch/pacing math unit tests.
 *
 * Includes the production st_tx_video_session.c directly so the file-local
 * static functions (calc_frame_count_since_epoch, tv_sync_pacing,
 * validate_user_timestamp, transmission_start_time, ...) become visible in
 * this translation unit. Non-static symbols duplicate those in libmtl;
 * --allow-multiple-definition resolves this. USDT is disabled to avoid
 * probe-semaphore link references.
 *
 * mt_get_tsc() is mocked with a preprocessor seam instead of any production
 * source change: mt_main.h is included first, under its real name, so the
 * genuine mt_get_tsc() compiles normally; its include guard then makes the
 * copy pulled in transitively by st_tx_video_session.c a no-op, so the
 * `#define mt_get_tsc ...` below only rewrites the call sites written inside
 * st_tx_video_session.c itself. mt_get_ptp_time() needs no such seam -- it
 * already dispatches through the pre-existing, production `ptp_get_time_fn`
 * function-pointer field on `struct mt_interface`.
 *
 * Only pacing math is exercised here -- no mbuf pool, queue, or packet I/O
 * is set up, since neither target function touches packets.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"
#include "st2110/st_tx_video_session.h"

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut_txv_ctx {
  struct mtl_main_impl impl;
  struct st_tx_video_sessions_mgr mgr;
  struct st_tx_video_session_impl session;
  uint64_t mock_ptp_ns;
  uint64_t mock_tsc_ns;
  enum st10_timestamp_fmt app_tfmt;
  uint64_t app_timestamp;
  int get_next_frame_calls;
  int notify_frame_done_calls;
  uint16_t notify_frame_done_idx;
  struct st20_tx_frame_meta notify_frame_done_meta;
  enum st21_tx_frame_status frame_status;
  int frame_refcnt;
  int notify_late_calls;
  uint64_t notify_late_last_delta;
  int burst_calls;
  struct rte_mbuf* burst_packets[8];
  unsigned int burst_packets_count;
};

#include "session/st20_tx_harness.h"

/* ── mocked time sources ──────────────────────────────────────────────── */

static uint64_t ut_txv_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl; /* impl is ctx's first member */
  return ctx->mock_ptp_ns;
}

static uint64_t ut_txv_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  return ctx->mock_tsc_ns;
}

static int ut_txv_notify_frame_late(void* priv, uint64_t epoch_skipped) {
  struct ut_txv_ctx* ctx = priv;
  ctx->notify_late_calls++;
  ctx->notify_late_last_delta = epoch_skipped;
  return 0;
}

static int ut_txv_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st20_tx_frame_meta* meta) {
  struct ut_txv_ctx* ctx = priv;
  ctx->get_next_frame_calls++;
  *next_frame_idx = 0;
  meta->tfmt = ctx->app_tfmt;
  meta->timestamp = ctx->app_timestamp;
  return 0;
}

static int ut_txv_notify_frame_done(void* priv, uint16_t frame_idx,
                                    struct st20_tx_frame_meta* meta) {
  struct ut_txv_ctx* ctx = priv;
  ctx->notify_frame_done_calls++;
  ctx->notify_frame_done_idx = frame_idx;
  ctx->notify_frame_done_meta = *meta;
  return 0;
}

/* Seam: from here on, calls to mt_get_tsc() written inside st_tx_video_session.c
 * resolve to our mock instead. The real mt_get_tsc() compiled above (via the
 * explicit mt_main.h include) is unaffected. */
#define mt_get_tsc ut_txv_tsc_time_fn
#include "st2110/st_tx_video_session.c"
#define mt_txq_burst ut_txv_txq_burst
static struct ut_txv_ctx* ut_txv_active_burst_ctx;
static uint16_t ut_txv_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                                 uint16_t nb_pkts) {
  (void)entry;
  struct ut_txv_ctx* ctx = ut_txv_active_burst_ctx;
  if (!ctx) return 0;
  ctx->burst_calls++;
  for (uint16_t i = 0; i < nb_pkts; i++) {
    if (ctx->burst_packets_count < RTE_DIM(ctx->burst_packets))
      ctx->burst_packets[ctx->burst_packets_count++] = tx_pkts[i];
  }
  return nb_pkts;
}
#include "st2110/st_video_transmitter.c"
#undef mt_txq_burst
#undef mt_get_tsc

/* ── init (delegates to common) ───────────────────────────────────────── */

int ut_txv_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut_txv_ctx* ut_txv_create(void) {
  ut_txv_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut_txv_ptp_time_fn;
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.max_idx = 1;
  rte_spinlock_init(&ctx->mgr.mutex[0]);

  struct st_tx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->mgr = &ctx->mgr;
  s->idx = 0;
  s->active = true;
  s->pacing.frame_time = 1000000.0L; /* 1ms, round number for simple math */
  s->pacing.max_onward_epochs = 3;
  s->fps_tm.sampling_clock_rate = ST10_VIDEO_SAMPLING_RATE_90K;
  s->ops.get_next_frame = ut_txv_get_next_frame;
  s->ops.notify_frame_done = ut_txv_notify_frame_done;
  s->ops.notify_frame_late = ut_txv_notify_frame_late;
  s->ops.priv = ctx;
  ctx->mgr.sessions[0] = s;

  return ctx;
}

void ut_txv_destroy(ut_txv_ctx* ctx) {
  free(ctx);
}

/* ── pacing setup ─────────────────────────────────────────────────────── */

void ut_txv_set_frame_time(ut_txv_ctx* ctx, long double frame_time_ns) {
  ctx->session.pacing.frame_time = frame_time_ns;
}

void ut_txv_set_max_onward_epochs(ut_txv_ctx* ctx, uint32_t max_onward_epochs) {
  ctx->session.pacing.max_onward_epochs = max_onward_epochs;
}

void ut_txv_set_cur_epochs(ut_txv_ctx* ctx, uint64_t cur_epochs) {
  ctx->session.pacing.cur_epochs = cur_epochs;
}

void ut_txv_set_tr_offset(ut_txv_ctx* ctx, long double tr_offset_ns) {
  ctx->session.pacing.tr_offset = tr_offset_ns;
}

void ut_txv_set_vrx(ut_txv_ctx* ctx, uint32_t vrx) {
  ctx->session.pacing.vrx = vrx;
}

void ut_txv_set_trs(ut_txv_ctx* ctx, long double trs_ns) {
  ctx->session.pacing.trs = trs_ns;
}

void ut_txv_set_sampling_clock_rate(ut_txv_ctx* ctx, uint32_t sampling_rate) {
  ctx->session.fps_tm.sampling_clock_rate = sampling_rate;
}

void ut_txv_set_ptp_time_cursor(ut_txv_ctx* ctx, uint64_t tai_ns) {
  ctx->session.pacing.ptp_time_cursor = tai_ns;
}

void ut_txv_set_exact_user_pacing(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_EXACT_USER_PACING;
}

void ut_txv_set_user_pacing(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_USER_PACING;
}

void ut_txv_set_mock_ptp_time(ut_txv_ctx* ctx, uint64_t ptp_ns) {
  ctx->mock_ptp_ns = ptp_ns;
}

void ut_txv_set_mock_tsc_time(ut_txv_ctx* ctx, uint64_t tsc_ns) {
  ctx->mock_tsc_ns = tsc_ns;
}

/* ── code under test ──────────────────────────────────────────────────── */

uint64_t ut_txv_calc_frame_count_since_epoch(ut_txv_ctx* ctx, uint64_t cur_tai,
                                             uint64_t required_tai) {
  return calc_frame_count_since_epoch(&ctx->session, cur_tai, required_tai);
}

int ut_txv_sync_pacing(ut_txv_ctx* ctx, uint64_t required_tai) {
  return tv_sync_pacing(&ctx->impl, &ctx->session, required_tai);
}

uint64_t ut_txv_pacing_required_tai(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp) {
  return tv_pacing_required_tai(&ctx->session, tfmt, timestamp);
}

int ut_txv_run_frame_tasklet(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc,
                             uint64_t* packet_ptp) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans frame = {0};
  struct rte_mbuf* packet = NULL;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];
  uint8_t frame_data[4] = {0};
  int ret = -1;

  snprintf(pool_name, sizeof(pool_name), "ut_txv_pool_%u", test_idx);
  snprintf(ring_name, sizeof(ring_name), "ut_txv_ring_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !s->ring[MTL_SESSION_PORT_P]) goto out;

  frame.addr = frame_data;
  frame.idx = 0;
  frame.priv = s;
  s->st20_frames = &frame;
  s->st20_frames_cnt = 1;
  s->st20_frame_size = sizeof(frame_data);
  s->st20_fb_size = sizeof(frame_data);
  s->st20_linesize = sizeof(frame_data);
  s->st20_bytes_in_line = sizeof(frame_data);
  s->st20_pkts_in_line = 1;
  s->st20_pkt_len = sizeof(frame_data);
  s->st20_total_pkts = 1;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  s->st20_pg.size = sizeof(frame_data);
  s->st20_pg.coverage = 2;
  s->bulk = 1;
  s->tx_no_chain = true;
  s->ops.num_port = 1;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.packing = ST20_PACKING_GPM_SL;
  s->ops.width = 2;
  s->ops.height = 1;
  ctx->app_tfmt = tfmt;
  ctx->app_timestamp = timestamp;
  ctx->get_next_frame_calls = 0;
  ctx->notify_frame_done_calls = 0;

  tvs_tasklet_handler(&ctx->mgr);
  ctx->frame_status = s->st20_frame_stat;
  ctx->frame_refcnt = rte_atomic32_read(&frame.refcnt);
  if (rte_ring_sc_dequeue(s->ring[MTL_SESSION_PORT_P], (void**)&packet) < 0) goto out;
  *packet_tsc = st_tx_mbuf_get_tsc(packet);
  *packet_ptp = st_tx_mbuf_get_ptp(packet);
  ret = 0;

out:
  rte_pktmbuf_free(packet);
  ut_ring_drain(s->ring[MTL_SESSION_PORT_P]);
  rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  s->ring[MTL_SESSION_PORT_P] = NULL;
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  s->st20_frames = NULL;
  return ret;
}

int ut_txv_run_transmitter_boundary(ut_txv_ctx* ctx, enum ut_txv_pacing_way way,
                                    uint64_t delta_ns, int* bursts_before_target,
                                    int* bursts_at_target) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct rte_mbuf* packet = NULL;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];
  const uint64_t now = NS_PER_MS / 2;
  int ret = -1;

  snprintf(pool_name, sizeof(pool_name), "ut_txv_trs_pool_%u", test_idx);
  snprintf(ring_name, sizeof(ring_name), "ut_txv_trs_ring_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !s->ring[MTL_SESSION_PORT_P]) goto out;
  packet = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  if (!packet) goto out;
  packet->pkt_len = 64;
  st_tx_mbuf_set_idx(packet, 0);
  st_tx_mbuf_set_tsc(packet, now + delta_ns);
  st_tx_mbuf_set_ptp(packet, now + delta_ns);
  if (rte_ring_sp_enqueue(s->ring[MTL_SESSION_PORT_P], packet) < 0) goto out;
  packet = NULL;

  s->bulk = 1;
  s->ops.num_port = 1;
  s->queue[MTL_SESSION_PORT_P] = (struct mt_txq_entry*)ctx;
  s->pacing_way[MTL_SESSION_PORT_P] = (enum st21_tx_pacing_way)way;
  s->pacing.warm_pkts = 0;
  s->pacing.trs = NS_PER_US;
  s->last_burst_succ_time_tsc[MTL_SESSION_PORT_P] = now;
  s->tx_hang_detect_time_thresh = UINT64_MAX;
  ctx->mock_tsc_ns = now;
  ctx->mock_ptp_ns = now;
  ctx->burst_calls = 0;
  ctx->burst_packets_count = 0;
  ut_txv_active_burst_ctx = ctx;
  st_video_resolve_pacing_tasklet(s, MTL_SESSION_PORT_P);
  s->pacing_tasklet_func[MTL_SESSION_PORT_P](&ctx->impl, s, MTL_SESSION_PORT_P);
  *bursts_before_target = ctx->burst_calls;

  ctx->mock_tsc_ns = now + delta_ns;
  ctx->mock_ptp_ns = now + delta_ns;
  s->pacing_tasklet_func[MTL_SESSION_PORT_P](&ctx->impl, s, MTL_SESSION_PORT_P);
  *bursts_at_target = ctx->burst_calls;
  ret = 0;

out:
  ut_txv_active_burst_ctx = NULL;
  rte_pktmbuf_free(packet);
  for (unsigned int i = 0; i < ctx->burst_packets_count; i++)
    rte_pktmbuf_free(ctx->burst_packets[i]);
  ctx->burst_packets_count = 0;
  if (s->ring[MTL_SESSION_PORT_P]) {
    ut_ring_drain(s->ring[MTL_SESSION_PORT_P]);
    rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  }
  rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  s->ring[MTL_SESSION_PORT_P] = NULL;
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  s->queue[MTL_SESSION_PORT_P] = NULL;
  s->trs_target_tsc[MTL_SESSION_PORT_P] = 0;
  s->trs_inflight_num[MTL_SESSION_PORT_P] = 0;
  return ret;
}

void ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp) {
  tv_update_rtp_time_stamp(&ctx->session, tfmt, timestamp);
}

/* ── accessors ─────────────────────────────────────────────────────────── */

uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.cur_epochs;
}

long double ut_txv_tsc_time_cursor(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.tsc_time_cursor;
}

long double ut_txv_ptp_time_cursor(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.ptp_time_cursor;
}

uint64_t ut_txv_tsc_time_frame_start(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.tsc_time_frame_start;
}

uint64_t ut_txv_stat_epoch_onward(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_onward;
}

uint64_t ut_txv_stat_epoch_drop(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_drop;
}

uint64_t ut_txv_stat_error_user_timestamp(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_error_user_timestamp;
}

uint64_t ut_txv_stat_epoch_mismatch(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_mismatch;
}

int ut_txv_notify_late_calls(const ut_txv_ctx* ctx) {
  return ctx->notify_late_calls;
}

uint64_t ut_txv_notify_late_last_delta(const ut_txv_ctx* ctx) {
  return ctx->notify_late_last_delta;
}

int ut_txv_get_next_frame_calls(const ut_txv_ctx* ctx) {
  return ctx->get_next_frame_calls;
}

int ut_txv_notify_frame_done_calls(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_calls;
}

uint16_t ut_txv_notify_frame_done_idx(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_idx;
}

uint64_t ut_txv_notify_frame_done_timestamp(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_meta.timestamp;
}

uint64_t ut_txv_notify_frame_done_epoch(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_meta.epoch;
}

bool ut_txv_frame_is_waiting(const ut_txv_ctx* ctx) {
  return ctx->frame_status == ST21_TX_STAT_WAIT_FRAME;
}

int ut_txv_frame_refcnt(const ut_txv_ctx* ctx) {
  return ctx->frame_refcnt;
}

uint64_t ut_txv_stat_port_build(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].build;
}

uint64_t ut_txv_stat_port_frames(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].frames;
}

uint64_t ut_txv_stat_exceed_frame_time(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_exceed_frame_time;
}

uint32_t ut_txv_rtp_time_stamp(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.rtp_time_stamp;
}
