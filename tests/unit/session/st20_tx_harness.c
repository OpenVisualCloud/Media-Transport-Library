/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-20 (video) TX session unit tests.
 *
 * Includes the production st_tx_video_session.c and st_video_transmitter.c
 * directly so their file-local static functions become visible in this
 * translation unit. Non-static symbols duplicate those in libmtl; this
 * object's definition preempts the shared library's. USDT is disabled to avoid
 * probe-semaphore link references.
 *
 * mt_get_tsc(), mt_txq_get(), mt_txq_burst() and, for tv_train_pacing(),
 * mt_txq_burst_busy(), mt_sleep_us(), mt_ptp_wait_stable() and
 * mt_wait_tsc_stable() are mocked with preprocessor
 * seams instead of any production source change: mt_main.h and
 * datapath/mt_queue.h are included first, under their real names, so their
 * include guards turn the copies the production files include into no-ops and
 * each `#define` below only rewrites call sites inside those files.
 * mt_get_ptp_time() needs no seam -- it dispatches through the production
 * `ptp_get_time_fn` field of `struct mt_interface`.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_queue.h"
#include "mt_main.h"
#include "session/st20_tx_harness.h"
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
  bool app_busy;
  int peer_get_next_frame_calls;
  int notify_frame_done_calls;
  uint16_t app_frame_idx;   /* the frame ut_txv_get_next_frame() hands over */
  uint32_t nic_sent_frames; /* zero copy: frames mt_txq_reclaim() frees */
  int reclaim_calls;
  struct mt_txq_entry zc_queues[MTL_SESSION_PORT_MAX];
  int leg_reclaims[MTL_SESSION_PORT_MAX];
  struct st_frame_trans zc_frames[UT_TXV_ZC_FRAMES];
  uint8_t zc_data[UT_TXV_ZC_FRAMES][UT_TXV_FRAME_SIZE];
  uint16_t notify_frame_done_idx;
  struct st20_tx_frame_meta notify_frame_done_meta;
  enum st21_tx_frame_status frame_status;
  int frame_refcnt;
  int notify_late_calls;
  uint64_t notify_late_last_delta;
  int burst_calls;
  struct rte_mbuf* burst_packets[8];
  unsigned int burst_packets_count;
  struct rte_mbuf* held_hdr_mbuf;
  char hdr_pool_name[RTE_MEMPOOL_NAMESIZE];
  /* Owned by the ctx (not heap-allocated, unlike production's mt_rte_zmalloc())
   * so ut_txv_run_st22_next_frame_step() can point s->st22_info at it and
   * reclaim it for free when the ctx itself is destroyed. */
  struct st22_tx_video_info st22_info;
  struct st_tx_video_session_handle_impl handle;
  struct st_frame_trans ext_frame;
  bool ext_frame_ready;
  void* rearm_buf;
  int rearm_ret;
  void* done_framebuffer;
  void* rearmed_framebuffer;
  bool build_on_done;
  unsigned int built_on_done;
  bool done_meta_kept;
  int txq_get_calls;
  enum mtl_port txq_get_port;
  uint64_t txq_get_bytes_per_sec;
  /* tv_train_pacing(): an RL queue of nb_tx_desc pads, which the shaper drains in
   * train_pad_ns per NORMAL pad of train_normal_len bytes, by their bytes */
  long double train_pad_ns;
  uint16_t train_normal_len;
  bool no_trim_pads; /* ut_txv_init_rl_pacing() leaves s->trim_pad empty */
  int trim_pad_tag;
  long double train_drain_end_ns;
  uint64_t slept_ns;
};

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
  if (ctx->app_busy) return -EBUSY;
  *next_frame_idx = ctx->app_frame_idx;
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

/* tv_tasklet_rtp() calls this unconditionally once it has an app packet. */
static int ut_txv_notify_rtp_done(void* priv) {
  (void)priv;
  return 0;
}

/* ST22 (compressed video) counterpart of ut_txv_get_next_frame() above: same
 * app-supplied tfmt/timestamp, but through st22_tx_video_info's own callback
 * signature so ut_txv_run_st22_next_frame_step() can drive tv_tasklet_st22(). */
static int ut_txv_st22_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                      struct st22_tx_frame_meta* meta) {
  struct ut_txv_ctx* ctx = priv;
  *next_frame_idx = 0;
  meta->tfmt = ctx->app_tfmt;
  meta->timestamp = ctx->app_timestamp;
  return 0;
}

/* Records the queue request and fails it, so tv_init_hw() stops right after. */
static struct mt_txq_entry* ut_txv_txq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                           struct mt_txq_flow* flow) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  ctx->txq_get_calls++;
  ctx->txq_get_port = port;
  ctx->txq_get_bytes_per_sec = flow->bytes_per_sec;
  return NULL;
}

static struct ut_txv_ctx* ut_txv_train_ctx;

/* frees the pkts of the frames marked sent, the last of each through its free callback */
static struct ut_txv_ctx* ut_txv_reclaim_ctx;
static void tv_frame_free_cb(void* addr, void* opaque);
static int ut_txv_txq_reclaim(struct mt_txq_entry* entry) {
  struct ut_txv_ctx* ctx = ut_txv_reclaim_ctx;
  if (!ctx) return 0;
  ctx->reclaim_calls++;
  for (int p = 0; p < MTL_SESSION_PORT_MAX; p++)
    if (entry == &ctx->zc_queues[p]) ctx->leg_reclaims[p]++;
  for (int i = 0; i < UT_TXV_ZC_FRAMES; i++) {
    struct st_frame_trans* frame = &ctx->zc_frames[i];
    if ((ctx->nic_sent_frames & (1u << i)) && rte_atomic32_read(&frame->refcnt) == 1)
      tv_frame_free_cb(frame->addr, frame);
  }
  return 0;
}

static void ut_txv_advance(struct ut_txv_ctx* ctx, uint64_t ns) {
  ctx->mock_tsc_ns += ns;
  ctx->mock_ptp_ns += ns;
}

static uint16_t ut_txv_txq_burst_busy(struct mt_txq_entry* entry,
                                      struct rte_mbuf** tx_pkts, uint16_t nb_pkts,
                                      int timeout_ms) {
  struct ut_txv_ctx* ctx = ut_txv_train_ctx;
  (void)entry;
  (void)timeout_ms;
  if (!ctx) { /* a NIC that takes everything */
    rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
    return nb_pkts;
  }
  uint16_t nb_desc = mt_if_nb_tx_desc(&ctx->impl, MTL_PORT_P);

  for (uint16_t i = 0; i < nb_pkts; i++) {
    /* the busy burst spins until the shaper frees a descriptor */
    long double full = ctx->train_drain_end_ns - (nb_desc - 1) * ctx->train_pad_ns;
    if (full > ctx->mock_tsc_ns)
      ut_txv_advance(ctx, (uint64_t)ceill(full - ctx->mock_tsc_ns));
    long double start = RTE_MAX(ctx->train_drain_end_ns, (long double)ctx->mock_tsc_ns);
    ctx->train_drain_end_ns =
        start + ctx->train_pad_ns * tx_pkts[i]->pkt_len / ctx->train_normal_len;
    if (rte_mbuf_refcnt_read(tx_pkts[i]) > 1) rte_mbuf_refcnt_update(tx_pkts[i], -1);
  }
  return nb_pkts;
}

static void ut_txv_sleep_us(unsigned int us) {
  ut_txv_advance(ut_txv_train_ctx, (uint64_t)us * NS_PER_US);
  ut_txv_train_ctx->slept_ns += (uint64_t)us * NS_PER_US;
}

static int ut_txv_ptp_wait_stable(struct mtl_main_impl* impl, enum mtl_port port,
                                  int timeout_ms) {
  (void)impl;
  (void)port;
  (void)timeout_ms;
  return 0;
}

static int ut_txv_wait_tsc_stable(struct mtl_main_impl* impl) {
  (void)impl;
  return 0;
}

/* Seams: these calls in st_tx_video_session.c hit the mocks. */
#define mt_get_tsc ut_txv_tsc_time_fn
#define mt_txq_get ut_txv_txq_get
#define mt_txq_burst_busy ut_txv_txq_burst_busy
#define mt_txq_reclaim ut_txv_txq_reclaim
#define mt_sleep_us ut_txv_sleep_us
#define mt_ptp_wait_stable ut_txv_ptp_wait_stable
#define mt_wait_tsc_stable ut_txv_wait_tsc_stable
#include "st2110/st_tx_video_session.c"
#undef mt_wait_tsc_stable
#undef mt_ptp_wait_stable
#undef mt_sleep_us
#undef mt_txq_burst_busy
#undef mt_txq_reclaim
#undef mt_txq_get
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

static void ut_txv_tx_path_uinit(struct ut_txv_ctx* ctx);

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
  if (!ctx) return;

  if (ctx->ext_frame_ready) {
    ut_txv_tx_path_uinit(ctx);
    ctx->session.st20_frames = NULL;
  }
  ut_txv_release_hdr_mbuf(ctx);
  /* by name, so a pool the session only borrowed and cleared is still reclaimed */
  if (ctx->hdr_pool_name[0]) {
    struct rte_mempool* pool = rte_mempool_lookup(ctx->hdr_pool_name);
    if (pool) rte_mempool_free(pool);
    ctx->session.mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  }
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

void ut_txv_set_warm_pkts(ut_txv_ctx* ctx, uint32_t warm_pkts) {
  ctx->session.pacing.warm_pkts = warm_pkts;
}

void ut_txv_set_interlaced(ut_txv_ctx* ctx, bool enable) {
  ctx->session.ops.interlaced = enable;
}

void ut_txv_set_st22_rtp_level(ut_txv_ctx* ctx, bool enable) {
  ctx->session.s_type = enable ? MT_ST22_HANDLE_TX_VIDEO : MT_HANDLE_TX_VIDEO;
}

void ut_txv_set_second_field(ut_txv_ctx* ctx, bool second_field) {
  ctx->session.second_field = second_field;
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

void ut_txv_set_user_timestamp(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_USER_TIMESTAMP;
}

void ut_txv_set_rtp_timestamp_epoch(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
}

void ut_txv_set_rtp_timestamp_delta_us(ut_txv_ctx* ctx, int32_t delta_us) {
  ctx->session.ops.rtp_timestamp_delta_us = delta_us;
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
  return tv_sync_pacing(&ctx->impl, &ctx->session, required_tai, /*second_field=*/false);
}

uint64_t ut_txv_pacing_required_tai(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp) {
  return tv_pacing_required_tai(&ctx->session, tfmt, timestamp);
}

/* Header mempool and TX ring on port P, as tv_mempool_init()/tv_init_hw() would.
 * Shared by every driver below that lets the production builders run. */
static int ut_txv_tx_path_init(struct ut_txv_ctx* ctx) {
  static unsigned int path_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(pool_name, sizeof(pool_name), "ut_txv_pool_%u", path_idx);
  snprintf(ring_name, sizeof(ring_name), "ut_txv_ring_%u", path_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !s->ring[MTL_SESSION_PORT_P]) return -1;
  return 0;
}

static void ut_txv_tx_path_uinit(struct ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (s->ring[MTL_SESSION_PORT_P]) {
    ut_ring_drain(s->ring[MTL_SESSION_PORT_P]);
    rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
    s->ring[MTL_SESSION_PORT_P] = NULL;
  }
  rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
}

/* One frame of total_pkts 4-byte packets, sent one per bulk, with no chain mbufs. */
static void ut_txv_frame_setup(struct ut_txv_ctx* ctx, struct st_frame_trans* frame,
                               uint8_t* data, int total_pkts) {
  struct st_tx_video_session_impl* s = &ctx->session;

  frame->addr = data;
  frame->idx = 0;
  frame->priv = s;
  s->st20_frames = frame;
  s->st20_frames_cnt = 1;
  s->st20_frame_size = UT_TXV_FRAME_SIZE;
  s->st20_fb_size = UT_TXV_FRAME_SIZE;
  s->st20_linesize = UT_TXV_FRAME_SIZE;
  s->st20_bytes_in_line = UT_TXV_FRAME_SIZE;
  s->st20_pkts_in_line = 1;
  s->st20_pkt_len = UT_TXV_FRAME_SIZE;
  s->st20_total_pkts = total_pkts;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  s->st20_pg.size = UT_TXV_FRAME_SIZE;
  s->st20_pg.coverage = 2;
  s->bulk = 1;
  s->tx_no_chain = true;
  s->ops.num_port = 1;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.packing = ST20_PACKING_GPM_SL;
  s->ops.width = 2;
  s->ops.height = 1;
}

int ut_txv_run_frame_tasklet(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc,
                             uint64_t* packet_ptp) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans frame = {0};
  struct rte_mbuf* packet = NULL;
  uint8_t frame_data[UT_TXV_FRAME_SIZE] = {0};
  int ret = -1;

  if (ut_txv_tx_path_init(ctx) < 0) goto out;

  ut_txv_frame_setup(ctx, &frame, frame_data, 1);
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
  ut_txv_tx_path_uinit(ctx);
  s->st20_frames = NULL;
  return ret;
}

/* Queues one app packet for tv_tasklet_rtp() on a one-packet RTP-level frame. */
static int ut_txv_rtp_setup(struct ut_txv_ctx* ctx, bool second_field, bool tx_no_chain) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(ring_name, sizeof(ring_name), "ut_txv_app_ring_%u", test_idx++);
  s->packet_ring = ut_ring_create(ring_name, 32);
  if (!s->packet_ring) return -1;

  struct rte_mbuf* app_pkt = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  if (!app_pkt) return -1;
  /* st20_tx_put_mbuf() leaves room for MTL's own mt_udp_hdr in the no-chain case; in
   * the chain case MTL prepends a header mbuf, so the app packet starts at the RTP
   * header */
  size_t hdr_offset = tx_no_chain ? sizeof(struct mt_udp_hdr) : 0;
  size_t app_pkt_len = hdr_offset + sizeof(struct st20_rfc4175_rtp_hdr);
  uint8_t* app_data = rte_pktmbuf_mtod(app_pkt, uint8_t*);
  memset(app_data, 0, app_pkt_len);
  app_pkt->data_len = app_pkt_len;
  app_pkt->pkt_len = app_pkt_len;
  struct st20_rfc4175_rtp_hdr* rfc4175 =
      (struct st20_rfc4175_rtp_hdr*)(app_data + hdr_offset);
  rfc4175->row_number = htons(second_field ? ST20_SECOND_FIELD : 0);
  /* the builders only treat a packet as the start of a new frame, and so only read
   * the parity, when its RTP timestamp differs from the last one sent */
  rfc4175->base.tmstamp = s->st20_rtp_time + 1;
  if (rte_ring_sp_enqueue(s->packet_ring, app_pkt) < 0) {
    rte_pktmbuf_free(app_pkt);
    return -1;
  }

  s->bulk = 4; /* production's default */
  s->tx_no_chain = tx_no_chain;
  s->ops.num_port = 1;
  s->ops.type = ST20_TYPE_RTP_LEVEL;
  s->ops.notify_rtp_done = ut_txv_notify_rtp_done;
  /* a one-packet frame, so tv_tasklet_rtp() consumes the single app packet queued
   * above and pads the rest of the burst with dummies */
  s->st20_total_pkts = 1;
  s->st20_pkt_idx = 0;
  return 0;
}

static void ut_txv_rtp_teardown(struct ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (!s->packet_ring) return;
  ut_ring_drain(s->packet_ring);
  rte_ring_free(s->packet_ring);
  s->packet_ring = NULL;
}

/* Drives the RTP-level builders (tv_build_rtp() when tx_no_chain, otherwise
 * tv_build_rtp_chain()) with one app packet whose st20_rfc4175_rtp_hdr names the
 * given field parity, so the F-bit extraction feeding tv_sync_pacing() runs for
 * real. Reports the epoch slot the session settled on. */
int ut_txv_run_rtp_tasklet(ut_txv_ctx* ctx, bool second_field, bool tx_no_chain,
                           uint64_t* epoch) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct rte_mbuf* built_pkt = NULL;
  int ret = -1;

  if (ut_txv_tx_path_init(ctx) < 0) goto out;
  if (ut_txv_rtp_setup(ctx, second_field, tx_no_chain) < 0) goto out;

  tvs_tasklet_handler(&ctx->mgr);
  /* a built packet on the TX ring is the proof the builder actually ran */
  if (rte_ring_sc_dequeue(s->ring[MTL_SESSION_PORT_P], (void**)&built_pkt) < 0) goto out;
  *epoch = s->pacing.cur_epochs;
  ret = 0;

out:
  rte_pktmbuf_free(built_pkt);
  ut_txv_rtp_teardown(ctx);
  ut_txv_tx_path_uinit(ctx);
  return ret;
}

static int ut_txv_st22_frame_setup(struct ut_txv_ctx* ctx, struct st_frame_trans* frame,
                                   enum st10_timestamp_fmt tfmt, uint64_t timestamp) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(ring_name, sizeof(ring_name), "ut_txv_st22_ring_%u", test_idx++);
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->ring[MTL_SESSION_PORT_P]) return -1;

  frame->idx = 0;
  frame->priv = s;
  s->st20_frames = frame;
  s->st20_frames_cnt = 1;
  /* codestream_size == pkt_len makes tv_tasklet_st22() compute exactly one
   * total packet; its "not > allowed" / "not zero" size checks both pass. */
  s->st20_pkt_len = 4;
  s->st22_codestream_size = 4;
  /* Required for tv_tasklet_st22() to enter its "start a new frame" branch at
   * all -- ut_txv_ctx is calloc'd, and ST21_TX_STAT_WAIT_FRAME is not the
   * zero value of enum st21_tx_frame_status (ST21_TX_STAT_UNKNOWN is). */
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  s->ops.num_port = 1;
  memset(&ctx->st22_info, 0, sizeof(ctx->st22_info));
  ctx->st22_info.get_next_frame = ut_txv_st22_get_next_frame;
  s->st22_info = &ctx->st22_info;
  ctx->app_tfmt = tfmt;
  ctx->app_timestamp = timestamp;
  return 0;
}

static void ut_txv_st22_frame_teardown(struct ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->st22_info = NULL;
  ut_ring_drain(s->ring[MTL_SESSION_PORT_P]);
  rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  s->ring[MTL_SESSION_PORT_P] = NULL;
  s->st20_frames = NULL;
}

/* Drives exactly as much of tv_tasklet_st22() as the frame->tx_st22_meta.timestamp
 * / .rtp_timestamp assignment needs: tvs_tasklet_handler() dispatches to
 * tv_tasklet_st22() purely because s->st22_info is non-NULL (no ops.type check),
 * and that function returns MTL_TASKLET_HAS_PENDING right after the metadata
 * assignment, before touching any mempool or building a packet -- so the only
 * prerequisite here is a non-full ring for its rte_ring_full() guard. */
int ut_txv_run_st22_next_frame_step(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp, uint64_t* frame_timestamp,
                                    uint32_t* frame_rtp_timestamp) {
  struct st_frame_trans frame = {0};
  int ret = -1;

  if (ut_txv_st22_frame_setup(ctx, &frame, tfmt, timestamp) < 0) goto out;

  tvs_tasklet_handler(&ctx->mgr);
  *frame_timestamp = frame.tx_st22_meta.timestamp;
  *frame_rtp_timestamp = frame.tx_st22_meta.rtp_timestamp;
  ret = 0;

out:
  ut_txv_st22_frame_teardown(ctx);
  return ret;
}

static int ut_txv_peer_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                      struct st20_tx_frame_meta* meta) {
  struct ut_txv_ctx* ctx = priv;
  (void)next_frame_idx;
  (void)meta;
  ctx->peer_get_next_frame_calls++;
  return -EBUSY;
}

static struct st_tx_video_session_impl* ut_txv_peer_create(struct ut_txv_ctx* ctx,
                                                           int idx, bool active) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* peer = calloc(1, sizeof(*peer));
  char ring_name[RTE_RING_NAMESIZE];

  if (!peer) return NULL;
  snprintf(ring_name, sizeof(ring_name), "ut_txv_peer_ring_%u", test_idx++);
  peer->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!peer->ring[MTL_SESSION_PORT_P]) {
    free(peer);
    return NULL;
  }
  peer->impl = &ctx->impl;
  peer->mgr = &ctx->mgr;
  peer->idx = idx;
  peer->active = active;
  peer->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  peer->ops.type = ST20_TYPE_FRAME_LEVEL;
  peer->ops.num_port = 1;
  peer->ops.get_next_frame = ut_txv_peer_get_next_frame;
  peer->ops.priv = ctx;
  return peer;
}

static void ut_txv_peer_free(struct st_tx_video_session_impl* peer) {
  if (!peer) return;
  rte_ring_free(peer->ring[MTL_SESSION_PORT_P]);
  free(peer);
}

int ut_txv_run_tasklet_slots(ut_txv_ctx* ctx, enum ut_txv_work work,
                             const enum ut_txv_slot* slots, int nb_slots) {
  struct st_tx_video_sessions_mgr* mgr = &ctx->mgr;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans frame = {0};
  uint8_t frame_data[UT_TXV_FRAME_SIZE] = {0};
  int ret = -1;

  if (nb_slots > ST_SCH_MAX_TX_VIDEO_SESSIONS) return -1;
  if (work == UT_TXV_WORK_ST22) {
    if (ut_txv_st22_frame_setup(ctx, &frame, ST10_TIMESTAMP_FMT_TAI, 0) < 0) goto out;
  } else {
    if (ut_txv_tx_path_init(ctx) < 0) goto out;
    if (work == UT_TXV_WORK_RTP) {
      if (ut_txv_rtp_setup(ctx, false, true) < 0) goto out;
    } else {
      /* two packets, so the one built leaves the frame unfinished */
      ut_txv_frame_setup(ctx, &frame, frame_data, 2);
      ctx->app_busy = (work == UT_TXV_WORK_NONE);
    }
  }

  mgr->sessions[0] = NULL;
  for (int i = 0; i < nb_slots; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
    if (slots[i] == UT_TXV_SLOT_SELF) {
      s->idx = i;
      mgr->sessions[i] = s;
    } else if (slots[i] != UT_TXV_SLOT_EMPTY) {
      mgr->sessions[i] = ut_txv_peer_create(ctx, i, slots[i] == UT_TXV_SLOT_IDLE);
      if (!mgr->sessions[i]) goto out;
    }
  }
  mgr->max_idx = nb_slots;
  ctx->peer_get_next_frame_calls = 0;
  ret = tvs_tasklet_handler(mgr);

out:
  for (int i = 0; i < nb_slots; i++) {
    if (mgr->sessions[i] != s) ut_txv_peer_free(mgr->sessions[i]);
    mgr->sessions[i] = NULL;
  }
  s->idx = 0;
  mgr->sessions[0] = s;
  mgr->max_idx = 1;
  ctx->app_busy = false;
  if (work == UT_TXV_WORK_ST22) {
    ut_txv_st22_frame_teardown(ctx);
  } else {
    ut_txv_rtp_teardown(ctx);
    ut_txv_tx_path_uinit(ctx);
    s->st20_frames = NULL;
  }
  return ret;
}

uint64_t ut_txv_run_rtp_pkt0_after_frame_session(ut_txv_ctx* ctx) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct rte_mbuf* packet = NULL;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];
  static uint8_t user_meta[8];
  const uint64_t now = NS_PER_MS / 2;

  (void)snprintf(pool_name, sizeof(pool_name), "ut_txv_stale_pool_%u", test_idx);
  (void)snprintf(ring_name, sizeof(ring_name), "ut_txv_stale_ring_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] && s->ring[MTL_SESSION_PORT_P])
    packet = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  if (packet) {
    /* the mbuf a frame-level session last built, as from the MTL_FLAG_TX_MONO_POOL pool:
     * its priv still points at that session's frame, which had user meta */
    ctx->ext_frame.user_meta = user_meta;
    ctx->ext_frame.user_meta_data_size = sizeof(user_meta);
    st_tx_mbuf_set_priv(packet, &ctx->ext_frame);
    /* tv_tasklet_rtp() builds it: index and time, no priv */
    packet->pkt_len = 64;
    st_tx_mbuf_set_idx(packet, 0);
    st_tx_mbuf_set_tsc(packet, now);
    st_tx_mbuf_set_ptp(packet, now);
    rte_ring_sp_enqueue(s->ring[MTL_SESSION_PORT_P], packet);

    ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_NO_SYS_TX_QUEUE;
    s->ops.type = ST20_TYPE_RTP_LEVEL;
    s->bulk = 1;
    s->ops.num_port = 1;
    s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
    s->queue[MTL_SESSION_PORT_P] = (struct mt_txq_entry*)ctx;
    s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_TSC;
    s->pacing.trs = NS_PER_US;
    s->last_burst_succ_time_tsc[MTL_SESSION_PORT_P] = now;
    s->tx_hang_detect_time_thresh = UINT64_MAX;
    ctx->mock_tsc_ns = now;
    ctx->mock_ptp_ns = now;
    ctx->burst_packets_count = 0;
    ut_txv_active_burst_ctx = ctx;
    st_video_resolve_pacing_tasklet(s, MTL_SESSION_PORT_P);
    s->pacing_tasklet_func[MTL_SESSION_PORT_P](&ctx->impl, s, MTL_SESSION_PORT_P);
    ut_txv_active_burst_ctx = NULL;
  }

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
  return s->port_user_stats.stat_user_meta_pkt_cnt;
}

int ut_txv_zc_setup(ut_txv_ctx* ctx, bool tx_reclaim, int num_port) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (ut_txv_tx_path_init(ctx) < 0) return -ENOMEM;
  ut_txv_frame_setup(ctx, &ctx->zc_frames[0], ctx->zc_data[0], 1);
  s->tx_reclaim = tx_reclaim;
  /* the R leg reclaims too; frames are built on the P leg alone */
  s->ops.num_port = num_port;
  for (int p = 0; p < num_port; p++) {
    s->queue[p] = &ctx->zc_queues[p];
    s->last_burst_succ_time_tsc[p] = ctx->mock_tsc_ns;
  }
  s->tx_hang_detect_time_thresh = UINT64_MAX;
  for (int i = 1; i < UT_TXV_ZC_FRAMES; i++) {
    ctx->zc_frames[i].addr = ctx->zc_data[i];
    ctx->zc_frames[i].idx = i;
    ctx->zc_frames[i].priv = s;
  }
  s->st20_frames_cnt = UT_TXV_ZC_FRAMES;
  ctx->get_next_frame_calls = 0;
  ctx->notify_frame_done_calls = 0;
  return 0;
}

void ut_txv_zc_teardown(ut_txv_ctx* ctx) {
  ut_txv_tx_path_uinit(ctx);
  ctx->session.st20_frames = NULL;
  for (int p = 0; p < MTL_SESSION_PORT_MAX; p++) ctx->session.queue[p] = NULL;
}

void ut_txv_zc_set_nic_refs(ut_txv_ctx* ctx, int frame, int refs) {
  rte_atomic32_set(&ctx->zc_frames[frame].refcnt, refs);
}

void ut_txv_zc_nic_sent(ut_txv_ctx* ctx, int frame) {
  ctx->nic_sent_frames |= 1u << frame;
}

void ut_txv_zc_set_app(ut_txv_ctx* ctx, int frame, bool busy) {
  ctx->app_frame_idx = frame;
  ctx->app_busy = busy;
}

void ut_txv_zc_set_last_built(ut_txv_ctx* ctx, int frame) {
  ctx->session.st20_frame_idx = frame;
}

bool ut_txv_zc_frames_out(ut_txv_ctx* ctx) {
  ut_txv_reclaim_ctx = ctx;
  bool out = tv_frames_out(&ctx->impl, &ctx->session);
  ut_txv_reclaim_ctx = NULL;
  return out;
}

void ut_txv_zc_run(ut_txv_ctx* ctx) {
  ut_txv_reclaim_ctx = ctx;
  tvs_tasklet_handler(&ctx->mgr);
  ut_txv_reclaim_ctx = NULL;
}

int ut_txv_zc_reclaim_calls(const ut_txv_ctx* ctx) {
  return ctx->reclaim_calls;
}

int ut_txv_zc_leg_reclaims(const ut_txv_ctx* ctx, int leg) {
  return ctx->leg_reclaims[leg];
}

void ut_txv_zc_set_hang_ns(ut_txv_ctx* ctx, uint64_t hang_ns) {
  ctx->session.tx_hang_detect_time_thresh = hang_ns;
}

void ut_txv_zc_leg_burst_now(ut_txv_ctx* ctx, int leg) {
  ctx->session.last_burst_succ_time_tsc[leg] = ctx->mock_tsc_ns;
}

bool ut_txv_zc_take_recovery(ut_txv_ctx* ctx, int leg) {
  bool pending = ctx->session.tx_queue_recovery_pending[leg];
  ctx->session.tx_queue_recovery_pending[leg] = false;
  return pending;
}

unsigned int ut_txv_zc_built_pkts(const ut_txv_ctx* ctx) {
  return rte_ring_count(ctx->session.ring[MTL_SESSION_PORT_P]);
}

uint64_t ut_txv_reclaim_interval_ns(void) {
  return ST_TX_VIDEO_RECLAIM_NS;
}

int ut_txv_peer_get_next_frame_calls(const ut_txv_ctx* ctx) {
  return ctx->peer_get_next_frame_calls;
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

int ut_txv_run_init_hw_rl_lookup(ut_txv_ctx* ctx, enum mtl_port phy_port,
                                 uint64_t trained_bps, enum mtl_port* queue_port,
                                 uint64_t* queue_bps) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->ops.num_port = 1;
  s->port_maps[MTL_SESSION_PORT_P] = phy_port;
  s->st20_pkt_size = 1200;
  s->st20_total_pkts = 100;
  s->fps_tm.mul = 60;
  s->fps_tm.den = 1;
  if (mt_pacing_train_bps_result_add(&ctx->impl, phy_port, tv_rl_bps(s), trained_bps) < 0)
    return -1;

  ctx->txq_get_calls = 0;
  tv_init_hw(&ctx->impl, &ctx->mgr, s);
  if (ctx->txq_get_calls != 1) return -1;
  *queue_port = ctx->txq_get_port;
  *queue_bps = ctx->txq_get_bytes_per_sec;
  return 0;
}

void ut_txv_set_random_src_port(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->impl.user_para.flags |= MTL_FLAG_RANDOM_SRC_PORT;
  else
    ctx->impl.user_para.flags &= ~MTL_FLAG_RANDOM_SRC_PORT;
}

int ut_txv_update_dst(ut_txv_ctx* ctx, uint16_t udp_src_port, uint16_t created_src_port,
                      uint16_t new_udp_port, uint16_t* hdr_src_port,
                      uint16_t* hdr_dst_port) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_tx_dest_info dst = {.dip_addr[MTL_SESSION_PORT_P] = {239, 0, 0, 1},
                                .udp_port[MTL_SESSION_PORT_P] = new_udp_port};

  /* the source MAC then comes from inf->k_mac_addr instead of an ethdev */
  ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_NOT_DPDK_PMD;
  s->ops.num_port = 1;
  s->ops.flags |= ST20_TX_FLAG_USER_P_MAC;
  s->ops.udp_src_port[MTL_SESSION_PORT_P] = udp_src_port;
  s->st20_src_port[MTL_SESSION_PORT_P] = created_src_port;
  if (tv_update_dst(&ctx->impl, s, &dst) < 0) return -1;
  *hdr_src_port = ntohs(s->s_hdr[MTL_SESSION_PORT_P].udp.src_port);
  *hdr_dst_port = ntohs(s->s_hdr[MTL_SESSION_PORT_P].udp.dst_port);
  return 0;
}

void ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp) {
  tv_update_rtp_time_stamp(&ctx->session, tfmt, timestamp);
}

int ut_txv_init_pacing(ut_txv_ctx* ctx, uint32_t height, bool interlaced,
                       enum st_fps fps) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (st_get_fps_timing(fps, &s->fps_tm) < 0) return -EINVAL;
  s->ops.height = height;
  s->ops.fps = fps;
  s->ops.interlaced = interlaced;
  s->ops.num_port = 1;
  /* TSC pacing skips tv_train_pacing(), so no NIC queue is needed, and
   * st_video_resolve_pacing_tasklet() is then only a function-pointer switch */
  s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_TSC;
  /* only used as a divisor for pacing->trs / pad_interval */
  s->st20_total_pkts = 1;
  return tv_init_pacing(&ctx->impl, s);
}

int ut_txv_init_rl_pacing(ut_txv_ctx* ctx, int num_port, uint16_t user_pad_interval,
                          bool static_pad, const float* trained_pad_interval, bool wide) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (st_get_fps_timing(ST_FPS_P59_94, &s->fps_tm) < 0) return -EINVAL;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.fps = ST_FPS_P59_94;
  s->ops.packing = ST20_PACKING_GPM;
  s->ops.interlaced = false;
  s->ops.num_port = num_port;
  s->ops.pad_interval = user_pad_interval;
  if (static_pad) s->ops.flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;
  s->ops.pacing = wide ? ST21_PACING_WIDE : ST21_PACING_NARROW;
  s->st20_total_pkts = 4320;
  s->st20_pkt_len = 1200;
  s->st20_pkt_size = 1262;
  /* 4 pkts a 4800-byte line, the last EXTRA as tv_train_pacing() types it */
  s->st20_bytes_in_line = 4800;
  s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
      s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
  s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = s->ops.height;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = s->st20_total_pkts - s->ops.height;
  s->st21_vrx_narrow = 8;
  s->st21_vrx_wide = 720;
  for (int i = 0; i < num_port; i++) {
    struct mt_interface* inf = &ctx->impl.inf[i];
    /* tags tv_init_pacing() only checks are there, as tv_init_hw() builds them */
    for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX && !ctx->no_trim_pads; j++)
      s->trim_pad[i][j] = (struct rte_mbuf*)&ctx->trim_pad_tag;
    if (inf->drv_info.drv_type != MT_DRV_ICE) inf->drv_info.drv_type = MT_DRV_IAVF;
    inf->nb_tx_desc = 512;
    s->port_maps[i] = (enum mtl_port)i;
    s->pacing_way[i] = ST21_TX_PACING_WAY_RL;
    /* a pre-train result keeps tv_train_pacing() off the NIC */
    if (trained_pad_interval)
      mt_pacing_train_pad_result_add(&ctx->impl, (enum mtl_port)i, tv_rl_bps(s),
                                     trained_pad_interval[i]);
  }
  return tv_init_pacing(&ctx->impl, s);
}

static struct rte_mbuf* ut_txv_train_pad(uint16_t len) {
  struct rte_mbuf* pad = rte_pktmbuf_alloc(ut_pool());
  if (pad && !rte_pktmbuf_append(pad, len)) {
    rte_pktmbuf_free(pad);
    return NULL;
  }
  return pad;
}

int ut_txv_train_rl_pacing(ut_txv_ctx* ctx, uint16_t nb_tx_desc, long double pad_ns,
                           bool bpm) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  struct st20_packet_group_info* normal = &s->st20_pkt_info[ST20_PKT_TYPE_NORMAL];
  struct st20_packet_group_info* extra = &s->st20_pkt_info[ST20_PKT_TYPE_EXTRA];
  int ret = -ENOMEM;

  if (st_get_fps_timing(ST_FPS_P59_94, &s->fps_tm) < 0) return -EINVAL;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.fps = ST_FPS_P59_94;
  s->ops.num_port = 1;
  s->st20_bytes_in_line = 4800;
  if (bpm) { /* 1028 of the 4115 pkts cross a line end: EXTRA, 6 bytes longer */
    s->ops.packing = ST20_PACKING_BPM;
    s->st20_total_pkts = 4115;
    s->st20_pkt_len = ST_VIDEO_BPM_SIZE;
    extra->number = 1028;
  } else { /* NORMAL pads only */
    s->ops.packing = ST20_PACKING_GPM_SL;
    s->st20_total_pkts = 4320;
    s->st20_pkt_len = 1200;
    extra->number = 0;
  }
  s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
  normal->size = s->st20_pkt_size;
  normal->number = s->st20_total_pkts - extra->number;
  extra->size = normal->size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
  s->pacing.trs = (long double)s->fps_tm.den * NS_PER_S / s->fps_tm.mul * 1080 / 1125 /
                  s->st20_total_pkts;
  inf->drv_info.drv_type = MT_DRV_IAVF;
  inf->nb_tx_desc = nb_tx_desc;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_RL;
  s->queue[MTL_SESSION_PORT_P] = (struct mt_txq_entry*)&ctx->train_drain_end_ns; /* tag */
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] = ut_txv_train_pad(normal->size);
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_EXTRA] = ut_txv_train_pad(extra->size);
  ctx->train_pad_ns = pad_ns;
  ctx->train_normal_len = normal->size;
  ctx->train_drain_end_ns = ctx->mock_tsc_ns;

  if (s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] &&
      s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_EXTRA]) {
    ut_txv_train_ctx = ctx;
    ret = tv_train_pacing(&ctx->impl, s, MTL_SESSION_PORT_P);
    ut_txv_train_ctx = NULL;
  }
  for (int t = ST20_PKT_TYPE_NORMAL; t <= ST20_PKT_TYPE_EXTRA; t++) {
    rte_pktmbuf_free(s->pad[MTL_SESSION_PORT_P][t]);
    s->pad[MTL_SESSION_PORT_P][t] = NULL;
  }
  s->queue[MTL_SESSION_PORT_P] = NULL;
  return ret;
}

float ut_txv_pad_interval(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.pad_interval;
}

uint64_t ut_txv_train_queued_ns(const ut_txv_ctx* ctx) {
  long double left = ctx->train_drain_end_ns - ctx->mock_tsc_ns;
  return left > 0 ? (uint64_t)left : 0;
}

uint64_t ut_txv_slept_ns(const ut_txv_ctx* ctx) {
  return ctx->slept_ns;
}

static void ut_txv_reclaim_session(ut_txv_ctx* ctx, const struct ut_txv_reclaim_case* c) {
  struct st_tx_video_session_impl* s = &ctx->session;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    ctx->impl.inf[i].tx_reclaim_rs_thresh = c->port_rs_thresh[i];
    s->port_maps[i] = (enum mtl_port)i;
    s->eth_has_chain[i] = !c->no_chain;
  }
  if (c->shared_txq)
    ctx->impl.user_para.flags |= MTL_FLAG_SHARED_TX_QUEUE;
  else
    ctx->impl.user_para.flags &= ~MTL_FLAG_SHARED_TX_QUEUE;
  s->ops.type = c->rtp_level ? ST20_TYPE_RTP_LEVEL : ST20_TYPE_FRAME_LEVEL;
  s->ops.num_port = c->num_port;
  if (c->rtcp)
    s->ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  else
    s->ops.flags &= ~ST20_TX_FLAG_ENABLE_RTCP;
  s->st20_total_pkts = c->total_pkts;
  s->st20_frames_cnt = c->frames_cnt;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
}

bool ut_txv_tx_reclaims(ut_txv_ctx* ctx, const struct ut_txv_reclaim_case* c) {
  ut_txv_reclaim_session(ctx, c);
  return tv_tx_reclaims(&ctx->impl, &ctx->session);
}

void ut_txv_init_tx_chain(ut_txv_ctx* ctx, const struct ut_txv_reclaim_case* c,
                          uint16_t nb_tx_desc, bool* zero_copy, bool* tx_reclaim) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) ctx->impl.inf[i].nb_tx_desc = nb_tx_desc;
  ut_txv_reclaim_session(ctx, c);
  tv_init_tx_chain(&ctx->impl, &ctx->session);
  *zero_copy = !ctx->session.tx_no_chain;
  *tx_reclaim = ctx->session.tx_reclaim;
}

void ut_txv_set_no_trim_pads(ut_txv_ctx* ctx) {
  ctx->no_trim_pads = true;
}

int ut_txv_init_trim_pads(ut_txv_ctx* ctx, enum st21_tx_pacing_way way,
                          uint32_t normal_size, uint32_t lens[2]) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int ret;

  /* mt_build_pad() takes the kernel mac of a non-DPDK port, no ethdev needed */
  ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_NOT_DPDK_PMD;
  s->pacing_way[MTL_SESSION_PORT_P] = way;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = normal_size;
  ret = tv_init_trim_pads(&ctx->impl, s, MTL_SESSION_PORT_P, MTL_PORT_P, ut_pool());
  for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX; j++) {
    struct rte_mbuf* pad = s->trim_pad[MTL_SESSION_PORT_P][j];
    lens[j] = pad ? pad->pkt_len : 0;
    rte_pktmbuf_free(pad);
    s->trim_pad[MTL_SESSION_PORT_P][j] = NULL;
  }
  return ret;
}

void ut_txv_set_ice_pf(ut_txv_ctx* ctx, int port, uint32_t rl_burst_size) {
  ctx->impl.inf[port].drv_info.drv_type = MT_DRV_ICE;
  ctx->impl.user_para.port_params[port].rl_burst_size = rl_burst_size;
}

long double ut_txv_pacing_trs(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.trs;
}

uint32_t ut_txv_pacing_warm_pkts(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.warm_pkts;
}

long double ut_txv_rl_drain(const ut_txv_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].pad_ns;
}

long double ut_txv_rl_credit(const ut_txv_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].bucket_ns;
}

uint64_t ut_txv_round_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate) {
  return st_tai_round_to_media_clk_ns(tai_ns, sampling_rate);
}

int ut_txv_install_hdr_mempool(ut_txv_ctx* ctx) {
  static unsigned int pool_idx;

  snprintf(ctx->hdr_pool_name, sizeof(ctx->hdr_pool_name), "ut_txv_hdr_pool_%u",
           pool_idx++);
  ctx->session.mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(ctx->hdr_pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  return ctx->session.mbuf_mempool_hdr[MTL_SESSION_PORT_P] ? 0 : -ENOMEM;
}

void ut_txv_set_tx_mono_pool(ut_txv_ctx* ctx, bool enable) {
  ctx->session.tx_mono_pool = enable;
}

void ut_txv_set_hdr_mempool_reuse_rx(ut_txv_ctx* ctx, bool enable) {
  ctx->session.mbuf_mempool_reuse_rx[MTL_SESSION_PORT_P] = enable;
}

bool ut_txv_hdr_mempool_alive(const ut_txv_ctx* ctx) {
  return rte_mempool_lookup(ctx->hdr_pool_name) != NULL;
}

int ut_txv_hold_hdr_mbuf(ut_txv_ctx* ctx) {
  if (ctx->held_hdr_mbuf) return -EEXIST;
  ctx->held_hdr_mbuf =
      rte_pktmbuf_alloc(ctx->session.mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  return ctx->held_hdr_mbuf ? 0 : -ENOMEM;
}

void ut_txv_release_hdr_mbuf(ut_txv_ctx* ctx) {
  if (!ctx->held_hdr_mbuf) return;
  rte_pktmbuf_free(ctx->held_hdr_mbuf);
  ctx->held_hdr_mbuf = NULL;
}

int ut_txv_mempool_free(ut_txv_ctx* ctx) {
  return tv_mempool_free(&ctx->session);
}

bool ut_txv_hdr_mempool_installed(const ut_txv_ctx* ctx) {
  return ctx->session.mbuf_mempool_hdr[MTL_SESSION_PORT_P] != NULL;
}

/* ── ext-frame completion (tv_frame_free_cb) ──────────────────────────── */

static int ut_txv_ext_notify_frame_done(void* priv, uint16_t frame_idx,
                                        struct st20_tx_frame_meta* meta) {
  struct ut_txv_ctx* ctx = priv;
  struct st_tx_video_session_impl* s = &ctx->session;

  ut_txv_notify_frame_done(priv, frame_idx, meta);
  ctx->done_framebuffer = st20_tx_get_framebuffer(&ctx->handle, frame_idx);
  if (ctx->rearm_buf) {
    ctx->rearm_ret = ut_txv_set_ext_frame(ctx, frame_idx, ctx->rearm_buf);
    ctx->rearmed_framebuffer = st20_tx_get_framebuffer(&ctx->handle, frame_idx);
  }
  if (ctx->build_on_done) {
    ctx->build_on_done = false;
    tvs_tasklet_handler(&ctx->mgr);
    ctx->built_on_done = rte_ring_count(s->ring[MTL_SESSION_PORT_P]);
    const struct st20_tx_frame_meta* seen = &ctx->notify_frame_done_meta;
    ctx->done_meta_kept = meta->timestamp == seen->timestamp &&
                          meta->epoch == seen->epoch &&
                          meta->rtp_timestamp == seen->rtp_timestamp;
  }
  return 0;
}

int ut_txv_ext_frame_setup(ut_txv_ctx* ctx, void* buf) {
  struct st_tx_video_session_impl* s = &ctx->session;

  ctx->ext_frame_ready = true;
  if (ut_txv_tx_path_init(ctx) < 0) return -1;

  ctx->handle.parent = &ctx->impl;
  ctx->handle.type = MT_HANDLE_TX_VIDEO;
  ctx->handle.impl = s;
  s->st20_handle = &ctx->handle;
  ctx->ext_frame.priv = s;
  ctx->ext_frame.flags = ST_FT_FLAG_EXT;
  s->st20_frames = &ctx->ext_frame;
  s->st20_frames_cnt = 1;
  s->st20_frame_size = UT_TXV_FRAME_SIZE;
  s->st20_fb_size = UT_TXV_FRAME_SIZE;
  s->st20_linesize = UT_TXV_FRAME_SIZE;
  s->st20_bytes_in_line = UT_TXV_FRAME_SIZE;
  s->st20_pkts_in_line = 1;
  s->st20_pkt_len = UT_TXV_FRAME_SIZE;
  /* two packets with bulk 1, so a build started from notify_frame_done stays in flight */
  s->st20_total_pkts = 2;
  s->st20_pg.size = UT_TXV_FRAME_SIZE;
  s->st20_pg.coverage = 2;
  s->bulk = 1;
  s->tx_no_chain = true;
  s->ops.num_port = 1;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.packing = ST20_PACKING_GPM_SL;
  s->ops.width = 2;
  s->ops.height = 1;
  s->ops.notify_frame_done = ut_txv_ext_notify_frame_done;

  if (ut_txv_set_ext_frame(ctx, 0, buf) < 0) return -1;
  /* built and handed to the transmitter; the builder waits for the next frame */
  rte_atomic32_set(&ctx->ext_frame.refcnt, 1);
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  return 0;
}

int ut_txv_set_ext_frame(ut_txv_ctx* ctx, uint16_t idx, void* buf) {
  struct st20_ext_frame ext = {
      .buf_addr = buf,
      .buf_iova = (rte_iova_t)(uintptr_t)buf,
      .buf_len = UT_TXV_FRAME_SIZE,
  };
  return st20_tx_set_ext_frame(&ctx->handle, idx, &ext);
}

void ut_txv_ext_frame_complete(ut_txv_ctx* ctx) {
  struct st_frame_trans* frame = &ctx->ext_frame;
  tv_frame_free_cb(frame->addr, frame);
}

void ut_txv_set_rearm_on_done(ut_txv_ctx* ctx, void* buf) {
  ctx->rearm_buf = buf;
}

void ut_txv_set_build_on_done(ut_txv_ctx* ctx) {
  ctx->build_on_done = true;
}

int ut_txv_rearm_ret(const ut_txv_ctx* ctx) {
  return ctx->rearm_ret;
}

void* ut_txv_done_framebuffer(const ut_txv_ctx* ctx) {
  return ctx->done_framebuffer;
}

void* ut_txv_rearmed_framebuffer(const ut_txv_ctx* ctx) {
  return ctx->rearmed_framebuffer;
}

unsigned int ut_txv_built_on_done(const ut_txv_ctx* ctx) {
  return ctx->built_on_done;
}

bool ut_txv_done_meta_kept(const ut_txv_ctx* ctx) {
  return ctx->done_meta_kept;
}

void* ut_txv_framebuffer(const ut_txv_ctx* ctx) {
  return st20_tx_get_framebuffer((st20_tx_handle)&ctx->handle, 0);
}

mtl_iova_t ut_txv_ext_frame_iova(const ut_txv_ctx* ctx) {
  return ctx->ext_frame.iova;
}

/* ── accessors ─────────────────────────────────────────────────────────── */

uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.cur_epochs;
}

long double ut_txv_pacing_tr_offset(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.tr_offset;
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

bool ut_txv_notify_frame_done_second_field(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_meta.second_field;
}

uint32_t ut_txv_notify_frame_done_rtp_timestamp(const ut_txv_ctx* ctx) {
  return ctx->notify_frame_done_meta.rtp_timestamp;
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

/* ── periodic stats dump ──────────────────────────────────────────────── */

static struct ut_txv_ctx* ut_txv_stat_ctx;
static int ut_txv_stat_lines;
static int ut_txv_stat_locked_lines;
static char ut_txv_stat_first_line[256];

static void ut_txv_stat_log_printer(enum mtl_log_level level, const char* format, ...) {
  va_list args;

  (void)level;
  if (rte_spinlock_is_locked(&ut_txv_stat_ctx->mgr.mutex[0])) ut_txv_stat_locked_lines++;
  if (!ut_txv_stat_lines++) {
    va_start(args, format);
    vsnprintf(ut_txv_stat_first_line, sizeof(ut_txv_stat_first_line), format, args);
    va_end(args);
  }
}

int ut_txv_run_sessions_stat(ut_txv_ctx* ctx, int* locked_lines) {
  ut_txv_stat_ctx = ctx;
  ut_txv_stat_lines = 0;
  ut_txv_stat_locked_lines = 0;
  ut_txv_stat_first_line[0] = '\0';
  mtl_set_log_printer(ut_txv_stat_log_printer);
  tv_sessions_stat(&ctx->mgr);
  mtl_set_log_printer(NULL);
  ut_txv_stat_ctx = NULL;
  *locked_lines = ut_txv_stat_locked_lines;
  return ut_txv_stat_lines;
}

const char* ut_txv_stat_first_log_line(void) {
  return ut_txv_stat_first_line;
}

void ut_txv_set_stat_port_frames(ut_txv_ctx* ctx, uint64_t frames) {
  ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].frames = frames;
}

uint64_t ut_txv_stat_snapshot_port_frames(const ut_txv_ctx* ctx) {
  return ctx->session.stat_snapshot.common.port[MTL_SESSION_PORT_P].frames;
}

bool ut_txv_pkts_capable_chain(ut_txv_ctx* ctx, uint16_t nb_tx_desc, int total_pkts,
                               uint16_t frames_cnt, bool rtcp) {
  struct st_tx_video_session_impl* s = &ctx->session;

  ctx->impl.inf[MTL_PORT_P].nb_tx_desc = nb_tx_desc;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.num_port = 1;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->st20_total_pkts = total_pkts;
  s->st20_frames_cnt = frames_cnt;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  if (rtcp)
    s->ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  else
    s->ops.flags &= ~ST20_TX_FLAG_ENABLE_RTCP;
  return tv_pkts_capable_chain(&ctx->impl, s);
}

int ut_txv_rtcp_mempool_sizes(ut_txv_ctx* ctx, uint16_t nb_tx_desc,
                              uint16_t rtcp_buffer_size, unsigned int* hdr_size,
                              unsigned int* chain_size) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int ret;

  ctx->impl.inf[MTL_PORT_P].nb_tx_desc = nb_tx_desc;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.num_port = 1;
  s->ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  s->ops.rtcp.buffer_size = rtcp_buffer_size;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  s->st20_pkt_size = 1262;
  s->socket_id = (int)rte_socket_id();
  ret = tv_mempool_init(&ctx->impl, &ctx->mgr, s);
  if (ret < 0) return ret;
  *hdr_size = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]->size;
  *chain_size = s->mbuf_mempool_chain->size;
  return tv_mempool_free(s);
}
