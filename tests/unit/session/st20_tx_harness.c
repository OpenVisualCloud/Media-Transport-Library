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
#include "common/ut_tx_ring.h"
#include "datapath/mt_queue.h"
#include "dev/mt_dev.h"
#include "mt_main.h"
#include "mt_rtcp.h"
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
  int lock_probe_runs;
  bool lock_probe_locked;
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
  uint32_t rl_width; /* ut_txv_set_rl_format(), 0 for 1920x1080 */
  uint32_t rl_height;
  bool rl_interlaced;
  enum st_fps rl_fps;
  struct mt_txq_entry rl_queue[MTL_SESSION_PORT_MAX];
  struct mt_tx_queue rl_txq[MTL_SESSION_PORT_MAX];
  bool rl_bps_set[MTL_SESSION_PORT_MAX];
  uint64_t rl_bps[MTL_SESSION_PORT_MAX];
  int trim_pad_tag;
  long double train_drain_end_ns;
  uint64_t slept_ns;
  /* st20_tx_queue_fatal_error(): each leg's queue [0] and its replacement [1] */
  struct mt_txq_entry recovery_queue[MTL_SESSION_PORT_MAX][2];
  struct mt_tx_queue recovery_txq[MTL_SESSION_PORT_MAX][2];
  int recovery_flush_ret[MTL_SESSION_PORT_MAX];
  int recovery_flushes[MTL_SESSION_PORT_MAX];
  int recovery_fallback_flushes[MTL_SESSION_PORT_MAX];
  int recovery_warns;
  int recovery_done_cleanups[MTL_SESSION_PORT_MAX];
  struct st_frame_trans recovery_frame;
  int fatal_events;
  int recovery_events;
  /* ut_txv_pipe_*(): the builder and the TSC transmitter over a ut_txr ring a leg */
  bool pipe_on;
  struct ut_txr* pipe_ring[MTL_SESSION_PORT_MAX];
  struct mt_txq_entry pipe_queue[MTL_SESSION_PORT_MAX];
  struct rte_mbuf* pipe_held[UT_TXV_PIPE_HOLD_MAX];
  int pipe_held_cnt;
  uint16_t pipe_next_frame;
  uint64_t pipe_step;
  uint64_t pipe_first_frame_busy;
  uint64_t pipe_frames_asked;
  uint64_t pipe_first_alloc_fail;
  uint64_t pipe_first_burst_fail;
  /* ut_txv_pipe_build(): the pkts of each leg, [freed, cnt) still out */
  struct rte_mbuf* pipe_built[MTL_SESSION_PORT_MAX][UT_TXV_PIPE_BUILT_MAX];
  int pipe_built_cnt[MTL_SESSION_PORT_MAX];
  int pipe_built_freed[MTL_SESSION_PORT_MAX];
  int pipe_flush_ret[MTL_SESSION_PORT_MAX];
  /* notify_frame_done calls when a leg's first flush started */
  bool flush_seen[MTL_SESSION_PORT_MAX];
  int done_at_flush[MTL_SESSION_PORT_MAX];
  bool recovery_owns_pools;
  uint8_t recovery_data[UT_TXV_FRAME_SIZE];
  unsigned int first_pool_n; /* of the first pool tv_mempool_init() made */
  bool ptp_unstable;         /* mt_ptp_wait_stable() times out */
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
  if (ctx->pipe_on) {
    *next_frame_idx = ctx->pipe_next_frame;
    ctx->pipe_next_frame = (ctx->pipe_next_frame + 1) % ctx->session.st20_frames_cnt;
    ctx->pipe_frames_asked++;
  } else {
    *next_frame_idx = ctx->app_frame_idx;
  }
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

static int ut_txv_notify_event(void* priv, enum st_event event, void* args) {
  struct ut_txv_ctx* ctx = priv;
  (void)args;
  if (event == ST_EVENT_FATAL_ERROR) ctx->fatal_events++;
  if (event == ST_EVENT_RECOVERY_ERROR) ctx->recovery_events++;
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

static struct ut_txv_ctx* ut_txv_recovery_ctx;

/* Records the queue request. Outside a recovery it fails it, so tv_init_hw() stops right
 * after; in one it hands out the leg's next queue at the rate asked. */
static struct mt_txq_entry* ut_txv_txq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                           struct mt_txq_flow* flow) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  ctx->txq_get_calls++;
  ctx->txq_get_port = port;
  ctx->txq_get_bytes_per_sec = flow->bytes_per_sec;
  if (ctx != ut_txv_recovery_ctx) return NULL;
  /* the leg's other entry */
  int q = ctx->session.queue[port] == &ctx->recovery_queue[port][0] ? 1 : 0;
  ctx->recovery_txq[port][q].bps = flow->bytes_per_sec;
  return &ctx->recovery_queue[port][q];
}

/* the leg a queue entry belongs to, -1 outside a recovery */
static int ut_txv_recovery_leg(struct mt_txq_entry* entry) {
  struct ut_txv_ctx* ctx = ut_txv_recovery_ctx;
  if (!ctx) return -1;
  for (int p = 0; p < MTL_SESSION_PORT_MAX; p++)
    for (int q = 0; q < 2; q++)
      if (entry == &ctx->recovery_queue[p][q]) return p;
  return -1;
}

static void ut_txv_note_flush(struct ut_txv_ctx* ctx, int leg) {
  if (ctx->flush_seen[leg]) return;
  ctx->flush_seen[leg] = true;
  ctx->done_at_flush[leg] = ctx->notify_frame_done_calls;
}

static int ut_txv_txq_recovery_flush(struct mt_txq_entry* entry, struct rte_mbuf* pad) {
  int leg = ut_txv_recovery_leg(entry);
  (void)pad;
  if (leg < 0) return 0;
  ut_txv_note_flush(ut_txv_recovery_ctx, leg);
  ut_txv_recovery_ctx->recovery_flushes[leg]++;
  return ut_txv_recovery_ctx->recovery_flush_ret[leg];
}

/* ut_txv_pipe_stop_last_pkt_unsent(): its queues hold the pkts the legs kept */
static struct ut_txv_ctx* ut_txv_stop_ctx;

/* the flush that falls back to the per-pad loop */
static int ut_txv_txq_flush(struct mt_txq_entry* entry, struct rte_mbuf* pad) {
  int leg = ut_txv_recovery_leg(entry);
  struct ut_txv_ctx* stop = ut_txv_stop_ctx;
  (void)pad;
  if (leg >= 0) ut_txv_recovery_ctx->recovery_fallback_flushes[leg]++;
  for (int i = 0; stop && i < MTL_SESSION_PORT_MAX; i++) {
    if (entry != &stop->pipe_queue[i]) continue;
    ut_txv_note_flush(stop, i);
    if (stop->pipe_flush_ret[i] < 0) return stop->pipe_flush_ret[i];
    ut_txv_pipe_free_built(stop, i, stop->pipe_built_cnt[i] - stop->pipe_built_freed[i]);
  }
  return 0;
}

static int ut_txv_txq_done_cleanup(struct mt_txq_entry* entry) {
  int leg = ut_txv_recovery_leg(entry);
  if (leg >= 0) ut_txv_recovery_ctx->recovery_done_cleanups[leg]++;
  return 0;
}

static int ut_txv_txq_fatal_error(struct mt_txq_entry* entry) {
  (void)entry;
  return 0;
}

static int ut_txv_txq_put(struct mt_txq_entry* entry) {
  (void)entry;
  return 0;
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
  uint16_t nb_desc = mt_if_nb_tx_desc(&ctx->impl, MTL_PORT_P);
  (void)entry;
  (void)timeout_ms;

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

/* reprograms the training queue: its shaper drains a pad that much faster */
static int ut_txv_txq_set_tx_bps(struct mt_txq_entry* entry, uint64_t bytes_per_sec) {
  struct ut_txv_ctx* ctx = ut_txv_train_ctx;
  if (!ctx || !entry->txq || !bytes_per_sec) return -ENOTSUP;
  ctx->train_pad_ns = ctx->train_pad_ns * entry->txq->bps / bytes_per_sec;
  entry->txq->bps = bytes_per_sec;
  return 0;
}

static void ut_txv_sleep_us(unsigned int us) {
  ut_txv_advance(ut_txv_train_ctx, (uint64_t)us * NS_PER_US);
  ut_txv_train_ctx->slept_ns += (uint64_t)us * NS_PER_US;
}

static int ut_txv_ptp_wait_stable(struct mtl_main_impl* impl, enum mtl_port port,
                                  int timeout_ms) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  (void)port;
  (void)timeout_ms;
  return ctx->ptp_unstable ? -ETIMEDOUT : 0;
}

static struct rte_mempool* ut_txv_mempool_create_by_socket(
    struct mtl_main_impl* impl, const char* name, unsigned int n, unsigned int cache_size,
    uint16_t priv_size, uint16_t element_size, int socket_id) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  if (!ctx->first_pool_n) ctx->first_pool_n = n;
  return mt_mempool_create_by_socket(impl, name, n, cache_size, priv_size, element_size,
                                     socket_id);
}

static int ut_txv_wait_tsc_stable(struct mtl_main_impl* impl) {
  (void)impl;
  return 0;
}

/* the retransmit ring tv_init_rtcp() last asked mt_rtcp_tx_create() for */
static uint16_t ut_txv_rtcp_ring;

static struct mt_rtcp_tx* ut_txv_rtcp_tx_create(struct mtl_main_impl* impl,
                                                struct mt_rtcp_tx_ops* ops) {
  (void)impl;
  ut_txv_rtcp_ring = ops->buffer_size;
  return (struct mt_rtcp_tx*)&ut_txv_rtcp_ring;
}

static void ut_txv_rtcp_tx_free(struct mt_rtcp_tx* tx) {
  (void)tx;
}

static struct mt_rxq_entry* ut_txv_rxq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                           struct mt_rxq_flow* flow) {
  (void)impl;
  (void)port;
  (void)flow;
  return (struct mt_rxq_entry*)&ut_txv_rtcp_ring;
}

static int ut_txv_rxq_put(struct mt_rxq_entry* entry) {
  (void)entry;
  return 0;
}

/* Records a create's quota and gives no scheduler, so the create stops there. */
static struct mtl_sch_impl* ut_txv_sch_get_by_socket(struct mtl_main_impl* impl,
                                                     int quota_mbs, enum mt_sch_type type,
                                                     mt_sch_mask_t mask, int socket);

/* Seams: these calls in st_tx_video_session.c hit the mocks. */
#define mt_rtcp_tx_create ut_txv_rtcp_tx_create
#define mt_rtcp_tx_free ut_txv_rtcp_tx_free
#define mt_rxq_get ut_txv_rxq_get
#define mt_rxq_put ut_txv_rxq_put
#define mt_get_tsc ut_txv_tsc_time_fn
#define mt_txq_get ut_txv_txq_get
#define mt_txq_burst_busy ut_txv_txq_burst_busy
#define mt_txq_reclaim ut_txv_txq_reclaim
#define mt_sleep_us ut_txv_sleep_us
#define mt_ptp_wait_stable ut_txv_ptp_wait_stable
#define mt_wait_tsc_stable ut_txv_wait_tsc_stable
#define mt_txq_flush ut_txv_txq_flush
#define mt_txq_recovery_flush ut_txv_txq_recovery_flush
#define mt_txq_done_cleanup ut_txv_txq_done_cleanup
#define mt_txq_fatal_error ut_txv_txq_fatal_error
#define mt_txq_put ut_txv_txq_put
#define mt_txq_set_tx_bps ut_txv_txq_set_tx_bps
#define mt_mempool_create_by_socket ut_txv_mempool_create_by_socket
#define mt_sch_get_by_socket ut_txv_sch_get_by_socket
#include "st2110/st_tx_video_session.c"
#undef mt_sch_get_by_socket
#undef mt_mempool_create_by_socket
#undef mt_txq_set_tx_bps
#undef mt_txq_put
#undef mt_txq_fatal_error
#undef mt_txq_done_cleanup
#undef mt_txq_recovery_flush
#undef mt_txq_flush
#undef mt_wait_tsc_stable
#undef mt_ptp_wait_stable
#undef mt_sleep_us
#undef mt_txq_burst_busy
#undef mt_txq_reclaim
#undef mt_txq_get
#undef mt_rxq_put
#undef mt_rxq_get
#undef mt_rtcp_tx_free
#undef mt_rtcp_tx_create
#define mt_txq_burst ut_txv_txq_burst
static struct ut_txv_ctx* ut_txv_active_burst_ctx;
static uint16_t ut_txv_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                                 uint16_t nb_pkts) {
  struct ut_txv_ctx* ctx = ut_txv_active_burst_ctx;
  if (!ctx) return 0;
  ctx->burst_calls++;
  if (ctx->pipe_on) {
    int leg = entry == &ctx->pipe_queue[MTL_SESSION_PORT_R] ? MTL_SESSION_PORT_R
                                                            : MTL_SESSION_PORT_P;
    uint16_t tx = ut_txr_xmit(ctx->pipe_ring[leg], tx_pkts, nb_pkts, ctx->mock_tsc_ns);
    if (tx < nb_pkts && !ctx->pipe_first_burst_fail)
      ctx->pipe_first_burst_fail = ctx->pipe_step;
    return tx;
  }
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
  struct mtl_sch_impl* sch = mt_sch_instance(&ctx->impl, 0); /* the gets wait on it */
  sch->parent = &ctx->impl;
  sch->qs_seen = UINT64_MAX; /* no scheduler thread runs the harness tasklets */
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.max_idx = 1;
  rte_spinlock_init(&ctx->mgr.mutex[0]);

  struct st_tx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->mgr = &ctx->mgr;
  s->idx = 0;
  s->active = true;
  ut_txv_set_frame_time(ctx, NS_PER_MS); /* round number for simple math */
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
  if (ctx->recovery_owns_pools) tv_mempool_free(&ctx->session);
  free(ctx);
}

/* ── pacing setup ─────────────────────────────────────────────────────── */

void ut_txv_set_frame_time(ut_txv_ctx* ctx, uint32_t frame_time_ns) {
  ctx->session.pacing.frame_time = frame_time_ns;
  ctx->session.fps_tm.mul = NS_PER_S;
  ctx->session.fps_tm.den = (int)frame_time_ns;
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

/* Header mempool and TX ring on a session port, as tv_mempool_init()/tv_init_hw() would.
 * Shared by every driver below that lets the production builders run. */
static int ut_txv_tx_leg_init(struct ut_txv_ctx* ctx, int port) {
  static unsigned int path_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(pool_name, sizeof(pool_name), "ut_txv_pool_%u", path_idx);
  snprintf(ring_name, sizeof(ring_name), "ut_txv_ring_%u", path_idx++);
  s->mbuf_mempool_hdr[port] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  s->ring[port] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[port] || !s->ring[port]) return -1;
  return 0;
}

static void ut_txv_tx_leg_uinit(struct ut_txv_ctx* ctx, int port) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (s->ring[port]) {
    ut_ring_drain(s->ring[port]);
    rte_ring_free(s->ring[port]);
    s->ring[port] = NULL;
  }
  rte_mempool_free(s->mbuf_mempool_hdr[port]);
  s->mbuf_mempool_hdr[port] = NULL;
}

static int ut_txv_tx_path_init(struct ut_txv_ctx* ctx) {
  return ut_txv_tx_leg_init(ctx, MTL_SESSION_PORT_P);
}

static void ut_txv_tx_path_uinit(struct ut_txv_ctx* ctx) {
  ut_txv_tx_leg_uinit(ctx, MTL_SESSION_PORT_P);
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

static int ut_txv_mark_unstamped(struct rte_mempool* pool) {
  struct rte_mbuf* mbufs[pool->size];

  if (rte_pktmbuf_alloc_bulk(pool, mbufs, pool->size) < 0) return -ENOMEM;
  for (unsigned int i = 0; i < pool->size; i++) {
    st_tx_mbuf_set_tsc(mbufs[i], UT_TXV_NO_STAMP);
    st_tx_mbuf_set_ptp(mbufs[i], UT_TXV_NO_STAMP);
  }
  rte_pktmbuf_free_bulk(mbufs, pool->size);
  return 0;
}

int ut_txv_run_frame_stamps(ut_txv_ctx* ctx, const enum st21_tx_pacing_way* ways,
                            int num_port, unsigned int bulk, int total_pkts,
                            uint64_t build_ns, struct ut_txv_stamps* stamps) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans frame = {0};
  uint8_t frame_data[UT_TXV_STAMP_PKTS * UT_TXV_FRAME_SIZE] = {0};
  int builds = (total_pkts + (int)bulk - 1) / (int)bulk;
  int pkts = builds * (int)bulk;
  int ret = -1;

  if (pkts > UT_TXV_STAMP_PKTS) return -EINVAL;
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++)
    s->pacing_way[port] = ways[port];
  for (int port = 0; port < num_port; port++) {
    if (ut_txv_tx_leg_init(ctx, port) < 0) goto out;
    if (ut_txv_mark_unstamped(s->mbuf_mempool_hdr[port]) < 0) goto out;
  }
  ut_txv_frame_setup(ctx, &frame, frame_data, total_pkts);
  s->st20_frame_size = (size_t)total_pkts * UT_TXV_FRAME_SIZE;
  s->st20_fb_size = s->st20_frame_size;
  s->ops.height = total_pkts;
  s->ops.num_port = num_port;
  s->bulk = bulk;
  ctx->app_tfmt = ST10_TIMESTAMP_FMT_TAI;
  ctx->app_timestamp = 0;

  for (int b = 0; b < builds; b++) {
    tvs_tasklet_handler(&ctx->mgr);
    ctx->mock_tsc_ns += build_ns;
  }
  if (s->st20_frame_stat != ST21_TX_STAT_WAIT_FRAME) goto out;
  for (int port = 0; port < num_port; port++) {
    if (rte_ring_count(s->ring[port]) != (unsigned int)pkts) goto out;
    for (int i = 0; i < pkts; i++) {
      struct rte_mbuf* pkt;
      if (rte_ring_sc_dequeue(s->ring[port], (void**)&pkt) < 0) goto out;
      stamps->tsc[port][i] = st_tx_mbuf_get_tsc(pkt);
      stamps->ptp[port][i] = st_tx_mbuf_get_ptp(pkt);
      rte_pktmbuf_free(pkt);
    }
  }
  ret = pkts;

out:
  for (int port = 0; port < num_port; port++) ut_txv_tx_leg_uinit(ctx, port);
  s->st20_frames = NULL;
  return ret;
}

mtl_iova_t ut_txv_frame_offset_iova(ut_txv_ctx* ctx, mtl_iova_t iova,
                                    const mtl_iova_t* page_iova, uint16_t pages,
                                    size_t offset) {
  uint8_t va[UT_TXV_IOVA_PAGES_MAX * UT_TXV_IOVA_PAGE_LEN];
  struct st_page_info page_table[UT_TXV_IOVA_PAGES_MAX];
  struct st_frame_trans frame = {
      .addr = va, .iova = iova, .page_table = page_table, .page_table_len = pages};

  if (pages > UT_TXV_IOVA_PAGES_MAX) return MTL_BAD_IOVA;
  for (uint16_t i = 0; i < pages; i++) {
    page_table[i].iova = page_iova[i];
    page_table[i].addr = va + (size_t)i * UT_TXV_IOVA_PAGE_LEN;
    page_table[i].len = UT_TXV_IOVA_PAGE_LEN;
  }
  return tv_frame_get_offset_iova(&ctx->session, &frame, offset);
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
  peer->fps_tm = ctx->session.fps_tm;
  peer->pacing.frame_time = ctx->session.pacing.frame_time;
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

int ut_txv_zc_setup(ut_txv_ctx* ctx, bool tx_reclaim, int num_port) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (ut_txv_tx_path_init(ctx) < 0) return -ENOMEM;
  ut_txv_frame_setup(ctx, &ctx->zc_frames[0], ctx->zc_data[0], 1);
  s->reclaim.enabled = tx_reclaim;
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

void ut_txv_zc_set_next_frame_due(ut_txv_ctx* ctx, uint64_t tsc_ns) {
  ctx->session.pacing.tsc_time_frame_start = tsc_ns;
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

static void ut_txv_lock_probe_record(struct ut_txv_ctx* ctx) {
  ctx->lock_probe_runs++;
  ctx->lock_probe_locked = rte_spinlock_is_locked(&ctx->mgr.mutex[0]);
}

static int ut_txv_lock_probe_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                            struct st20_tx_frame_meta* meta) {
  *next_frame_idx = 0;
  (void)meta;
  ut_txv_lock_probe_record(priv);
  return -EBUSY;
}

static int ut_txv_lock_probe_pacing(struct mtl_main_impl* impl,
                                    struct st_tx_video_session_impl* s,
                                    enum mtl_session_port s_port) {
  (void)impl;
  (void)s_port;
  ut_txv_lock_probe_record(s->ops.priv);
  return MTL_TASKLET_ALL_DONE;
}

int ut_txv_run_tasklet_lock_probe(ut_txv_ctx* ctx, enum ut_txv_tasklet tasklet,
                                  bool hold_lock, bool* locked_in_run,
                                  bool* locked_after) {
  struct st_tx_video_sessions_mgr* mgr = &ctx->mgr;
  struct st_video_transmitter_impl trs = {.parent = &ctx->impl, .mgr = mgr};
  struct st_tx_video_session_impl* peer = ut_txv_peer_create(ctx, 0, true);

  if (!peer) return -ENOMEM;
  peer->ops.get_next_frame = ut_txv_lock_probe_get_next_frame;
  peer->queue[MTL_SESSION_PORT_P] = &ctx->zc_queues[MTL_SESSION_PORT_P];
  peer->pacing_tasklet_func[MTL_SESSION_PORT_P] = ut_txv_lock_probe_pacing;
  mgr->sessions[0] = peer;
  ctx->lock_probe_runs = 0;
  ctx->lock_probe_locked = false;
  if (hold_lock) rte_spinlock_lock(&mgr->mutex[0]);

  if (tasklet == UT_TXV_TASKLET_BUILDER)
    tvs_tasklet_handler(mgr);
  else
    video_trs_tasklet_handler(&trs);

  *locked_in_run = ctx->lock_probe_locked;
  *locked_after = rte_spinlock_is_locked(&mgr->mutex[0]);
  if (*locked_after) rte_spinlock_unlock(&mgr->mutex[0]);
  mgr->sessions[0] = &ctx->session;
  ut_txv_peer_free(peer);
  return ctx->lock_probe_runs;
}

struct ut_txv_fake_sch {
  struct mtl_sch_impl* sch;
  rte_spinlock_t* lock;
  uint64_t token; /* qs_token before the call */
  atomic_bool stop;
  int waits;
  int unlocked_waits; /* begun without the session lock held */
  /* when set, the first wait finds the session migrated there meanwhile */
  struct ut_txv_ctx* migrate_ctx;
  struct st_tx_video_sessions_mgr* migrate_to;
};

#define UT_TXV_MIGRATED_IDX (3)

/* What tx_video_migrate_to() does to the session, onto slot 3 of to. */
static void ut_txv_migrate(struct ut_txv_ctx* ctx, struct st_tx_video_sessions_mgr* to) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->mgr->sessions[s->idx] = NULL;
  st_tx_video_session_migrate(to, s, UT_TXV_MIGRATED_IDX);
  to->sessions[UT_TXV_MIGRATED_IDX] = s;
  to->max_idx = UT_TXV_MIGRATED_IDX + 1;
}

/* A scheduler in a pass, which ends the pass once a wait on it began. */
static void* ut_txv_fake_sch_thread(void* arg) {
  struct ut_txv_fake_sch* f = arg;
  uint64_t seen = f->token;

  while (!atomic_load(&f->stop)) {
    uint64_t token = atomic_load(&f->sch->qs_token);
    if (token == seen) {
      rte_pause();
      continue;
    }
    f->waits += (int)(token - seen);
    if (!rte_spinlock_is_locked(f->lock)) f->unlocked_waits++;
    if (f->migrate_to) ut_txv_migrate(f->migrate_ctx, f->migrate_to);
    f->migrate_to = NULL;
    seen = token;
    atomic_store(&f->sch->qs_seen, token);
  }
  return NULL;
}

/* The stats, update and free calls act on ctx->session, the gets on mgr's slot idx. */
static int ut_txv_control_call(struct ut_txv_ctx* ctx,
                               struct st_tx_video_sessions_mgr* mgr, int idx,
                               enum ut_txv_control call) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_tx_dest_info dst = {.dip_addr[MTL_SESSION_PORT_P] = {239, 0, 0, 1}};
  struct st20_tx_user_stats stats;
  int ret;

  ctx->handle.parent = &ctx->impl;
  ctx->handle.type = MT_HANDLE_TX_VIDEO;
  ctx->handle.impl = s;
  ctx->handle.sch = mt_sch_instance(&ctx->impl, 0);
  ctx->handle.sch->tx_video_mgr.parent =
      &ctx->impl; /* a free past its guard updates it */
  /* as ut_txv_update_dst(): the source MAC then comes from inf->k_mac_addr */
  ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_NOT_DPDK_PMD;
  s->ops.num_port = 1;
  s->ops.flags |= ST20_TX_FLAG_USER_P_MAC;

  switch (call) {
    case UT_TXV_GET:
      ret = tx_video_session_get(mgr, idx) ? 0 : -EIO;
      break;
    case UT_TXV_GET_TIMEOUT:
      ret = tx_video_session_get_timeout(mgr, idx, US_PER_S) ? 0 : -EIO;
      break;
    case UT_TXV_GET_EMPTY:
      ret = tx_video_session_get_empty(mgr, idx) ? 0 : -EIO;
      break;
    case UT_TXV_STATS_GET:
      return st20_tx_get_session_stats(&ctx->handle, &stats);
    case UT_TXV_STATS_RESET:
      return st20_tx_reset_session_stats(&ctx->handle);
    case UT_TXV_UPDATE_DST:
      return st20_tx_update_destination(&ctx->handle, &dst);
    default: {
      /* rte memory, as a free that passes its guard releases the handle */
      struct st_tx_video_session_handle_impl* h =
          mt_rte_zmalloc_socket(sizeof(*h), SOCKET_ID_ANY);
      if (!h) return -ENOMEM;
      h->parent = &ctx->impl;
      h->type = MT_HANDLE_TX_VIDEO;
      h->impl = s;
      h->sch = ctx->handle.sch;
      ret = st20_tx_free(h);
      if (ret) mt_rte_free(h);
      return ret;
    }
  }
  if (!ret) tx_video_session_put(mgr, idx);
  return ret;
}

static int ut_txv_run_on_fake_sch(struct ut_txv_ctx* ctx,
                                  struct st_tx_video_sessions_mgr* mgr, int idx,
                                  enum ut_txv_control call, bool* locked_at_wait,
                                  struct st_tx_video_sessions_mgr* migrate_to) {
  struct mtl_sch_impl* sch = mt_sch_instance(mgr->parent, mgr->idx);
  struct ut_txv_fake_sch f = {
      .sch = sch, .lock = &mgr->mutex[idx], .migrate_ctx = ctx, .migrate_to = migrate_to};
  pthread_t thread;

  f.token = atomic_load(&sch->qs_token);
  atomic_store(&sch->qs_seen, f.token);
  int ret = -pthread_create(&thread, NULL, ut_txv_fake_sch_thread, &f);
  if (!ret) {
    ret = ut_txv_control_call(ctx, mgr, idx, call);
    atomic_store(&f.stop, true);
    pthread_join(thread, NULL);
  }
  atomic_store(&sch->qs_seen, UINT64_MAX);
  *locked_at_wait = !f.unlocked_waits;
  return ret < 0 ? ret : f.waits;
}

int ut_txv_run_control(ut_txv_ctx* ctx, enum ut_txv_control call, bool* locked_at_wait) {
  int idx = call == UT_TXV_GET_EMPTY ? 1 : 0;

  rte_spinlock_init(&ctx->mgr.mutex[idx]);
  return ut_txv_run_on_fake_sch(ctx, &ctx->mgr, idx, call, locked_at_wait, NULL);
}

/* A manager of scheduler 1, offline, for the session to migrate into. */
static struct st_tx_video_sessions_mgr* ut_txv_migration_target(struct ut_txv_ctx* ctx) {
  struct st_tx_video_sessions_mgr* to = calloc(1, sizeof(*to));
  struct mtl_sch_impl* sch = mt_sch_instance(&ctx->impl, 1);

  if (!to) return NULL;
  sch->parent = &ctx->impl;
  sch->qs_seen = UINT64_MAX;
  to->parent = &ctx->impl;
  to->idx = 1;
  rte_spinlock_init(&to->mutex[UT_TXV_MIGRATED_IDX]);
  return to;
}

static void ut_txv_migration_undo(struct ut_txv_ctx* ctx,
                                  struct st_tx_video_sessions_mgr* to) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->mgr = &ctx->mgr;
  s->idx = 0;
  ctx->mgr.sessions[0] = s;
  free(to);
}

int ut_txv_run_control_after_migration(ut_txv_ctx* ctx, enum ut_txv_control call,
                                       bool* locked_at_wait) {
  struct st_tx_video_sessions_mgr* to = ut_txv_migration_target(ctx);
  if (!to) return -ENOMEM;

  ut_txv_migrate(ctx, to);
  int ret =
      ut_txv_run_on_fake_sch(ctx, to, UT_TXV_MIGRATED_IDX, call, locked_at_wait, NULL);
  ut_txv_migration_undo(ctx, to);
  return ret;
}

int ut_txv_run_control_racing_migration(ut_txv_ctx* ctx, enum ut_txv_control call) {
  struct st_tx_video_sessions_mgr* to = ut_txv_migration_target(ctx);
  bool locked_at_wait;
  if (!to) return -ENOMEM;

  rte_spinlock_init(&ctx->mgr.mutex[0]);
  int ret = ut_txv_run_on_fake_sch(ctx, &ctx->mgr, 0, call, &locked_at_wait, to);
  ut_txv_migration_undo(ctx, to);
  return ret < 0 ? ret : 0;
}

struct ut_txv_migrator {
  struct ut_txv_ctx* ctx;
  struct st_tx_video_sessions_mgr* to;
  atomic_bool locked;
  atomic_bool reader_started;
};

/* As tx_video_migrate_to(): moves the session while it holds the old slot. */
static void* ut_txv_migrator_thread(void* arg) {
  struct ut_txv_migrator* m = arg;
  rte_spinlock_t* old_lock = &m->ctx->mgr.mutex[0];
  uint64_t deadline = rte_get_tsc_cycles() + rte_get_tsc_hz();

  rte_spinlock_lock(old_lock);
  atomic_store(&m->locked, true);
  while (!atomic_load(&m->reader_started) && rte_get_tsc_cycles() < deadline) rte_pause();
  /* time for the reader to block on the old slot; a later one reads the new slot */
  rte_delay_us_block(1000);
  ut_txv_migrate(m->ctx, m->to);
  rte_spinlock_unlock(old_lock);
  return NULL;
}

int ut_txv_run_control_blocked_by_migration(ut_txv_ctx* ctx, enum ut_txv_control call) {
  struct ut_txv_migrator m = {.ctx = ctx, .to = ut_txv_migration_target(ctx)};
  pthread_t thread;
  if (!m.to) return -ENOMEM;

  rte_spinlock_init(&ctx->mgr.mutex[0]);
  int ret = -pthread_create(&thread, NULL, ut_txv_migrator_thread, &m);
  if (!ret) {
    while (!atomic_load(&m.locked)) rte_pause();
    atomic_store(&m.reader_started, true);
    ret = ut_txv_control_call(ctx, &ctx->mgr, 0, call);
    pthread_join(thread, NULL);
  }
  ut_txv_migration_undo(ctx, m.to);
  return ret;
}

struct ut_txv_writer {
  struct st_tx_video_sessions_mgr* mgr;
};

static void* ut_txv_writer_thread(void* arg) {
  struct ut_txv_writer* w = arg;

  if (tx_video_session_get(w->mgr, 0)) tx_video_session_put(w->mgr, 0);
  return NULL;
}

struct ut_txv_watchdog {
  struct mtl_sch_impl* sch;
  atomic_bool done;
};

/* Ends a wait that has no timeout after 1 s, so the case fails instead of hanging. */
static void* ut_txv_watchdog_thread(void* arg) {
  struct ut_txv_watchdog* w = arg;
  uint64_t end = rte_get_tsc_cycles() + rte_get_tsc_hz();

  while (!atomic_load(&w->done) && rte_get_tsc_cycles() < end) rte_pause();
  if (!atomic_load(&w->done)) atomic_store(&w->sch->qs_seen, UINT64_MAX);
  return NULL;
}

int ut_txv_run_control_in_callback(ut_txv_ctx* ctx, enum ut_txv_control call, bool held) {
  struct st_tx_video_sessions_mgr* mgr = &ctx->mgr;
  struct mtl_sch_impl* sch = mt_sch_instance(&ctx->impl, 0);
  struct ut_txv_writer writer = {.mgr = mgr};
  struct ut_txv_watchdog w = {.sch = sch};
  pthread_t writer_thread, watchdog_thread;
  int ret = 0;

  sch->t_pid = rte_gettid(); /* the caller plays the scheduler thread, mid-pass */
  uint64_t token = atomic_load(&sch->qs_token);
  atomic_store(&sch->qs_seen, token);
  if (held) {
    ret = -pthread_create(&writer_thread, NULL, ut_txv_writer_thread, &writer);
    if (ret) goto out;
    /* the writer holds the lock once it waits for the pass */
    uint64_t deadline = rte_get_tsc_cycles() + rte_get_tsc_hz();
    while (atomic_load(&sch->qs_token) == token && rte_get_tsc_cycles() < deadline)
      rte_pause();
    if (atomic_load(&sch->qs_token) == token) ret = -ETIMEDOUT;
  }

  if (!ret) ret = -pthread_create(&watchdog_thread, NULL, ut_txv_watchdog_thread, &w);
  if (!ret) {
    ret = ut_txv_control_call(ctx, mgr, 0, call);
    atomic_store(&w.done, true);
    pthread_join(watchdog_thread, NULL);
  }
  atomic_store(&sch->qs_seen, atomic_load(&sch->qs_token)); /* the pass ends */
  if (held) pthread_join(writer_thread, NULL);
out:
  sch->t_pid = 0;
  atomic_store(&sch->qs_seen, UINT64_MAX);
  return ret;
}

bool ut_txv_get_timeout_on_endless_pass(ut_txv_ctx* ctx, int timeout_us,
                                        bool* locked_after) {
  struct st_tx_video_sessions_mgr* mgr = &ctx->mgr;
  struct ut_txv_watchdog w = {.sch = mt_sch_instance(&ctx->impl, 0)};
  pthread_t thread;
  bool got = true;

  atomic_store(&w.sch->qs_seen, atomic_load(&w.sch->qs_token));
  if (!pthread_create(&thread, NULL, ut_txv_watchdog_thread, &w)) {
    got = tx_video_session_get_timeout(mgr, 0, timeout_us) != NULL;
    atomic_store(&w.done, true);
    pthread_join(thread, NULL);
  }
  *locked_after = rte_spinlock_is_locked(&mgr->mutex[0]);
  if (*locked_after) tx_video_session_put(mgr, 0);
  atomic_store(&w.sch->qs_seen, UINT64_MAX);
  return got;
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

int ut_txv_run_launch_time(ut_txv_ctx* ctx, uint64_t target_ptp, uint64_t* launch_time,
                           bool* launch_flag) {
  static unsigned int test_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mt_interface* inf =
      mt_if(&ctx->impl, mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P));
  struct rte_mbuf* packet = NULL;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];
  uint64_t flag = 0;
  int offset, ret = -1;

  if (rte_mbuf_dyn_tx_timestamp_register(&offset, &flag) < 0) return -1;
  inf->tx_dynfield_offset = offset;
  inf->tx_launch_time_flag = flag;
  (void)snprintf(pool_name, sizeof(pool_name), "ut_txv_lt_pool_%u", test_idx);
  (void)snprintf(ring_name, sizeof(ring_name), "ut_txv_lt_ring_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !s->ring[MTL_SESSION_PORT_P]) goto out;
  packet = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  if (!packet) goto out;
  packet->pkt_len = 64;
  st_tx_mbuf_set_idx(packet, 1);
  st_tx_mbuf_set_ptp(packet, target_ptp);
  if (rte_ring_sp_enqueue(s->ring[MTL_SESSION_PORT_P], packet) < 0) goto out;
  packet = NULL;

  s->bulk = 1;
  s->ops.num_port = 1;
  s->queue[MTL_SESSION_PORT_P] = (struct mt_txq_entry*)ctx;
  s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_TSN;
  s->tx_hang_detect_time_thresh = UINT64_MAX;
  ctx->burst_calls = 0;
  ctx->burst_packets_count = 0;
  ut_txv_active_burst_ctx = ctx;
  st_video_resolve_pacing_tasklet(s, MTL_SESSION_PORT_P);
  s->pacing_tasklet_func[MTL_SESSION_PORT_P](&ctx->impl, s, MTL_SESSION_PORT_P);
  if (ctx->burst_packets_count != 1) goto out;
  *launch_time = *RTE_MBUF_DYNFIELD(ctx->burst_packets[0], offset, uint64_t*);
  *launch_flag = (ctx->burst_packets[0]->ol_flags & flag) != 0;
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
  inf->tx_dynfield_offset = 0;
  inf->tx_launch_time_flag = 0;
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

uint64_t ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                      uint64_t timestamp) {
  return tv_update_rtp_time_stamp(&ctx->session, tfmt, timestamp);
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

/* the bytes/s tv_init_hw() asks mt_txq_get() for on port, as the session alone uses it */
static uint64_t ut_txv_init_hw_bps(struct ut_txv_ctx* ctx, enum mtl_port port) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int num_port = s->ops.num_port;
  enum mtl_port port_p = s->port_maps[MTL_SESSION_PORT_P];

  s->ops.num_port = 1;
  s->port_maps[MTL_SESSION_PORT_P] = port;
  ctx->txq_get_calls = 0;
  tv_init_hw(&ctx->impl, &ctx->mgr, s);
  s->ops.num_port = num_port;
  s->port_maps[MTL_SESSION_PORT_P] = port_p;
  return ctx->txq_get_calls == 1 ? ctx->txq_get_bytes_per_sec : 0;
}

static int ut_txv_setup_rl_pacing(ut_txv_ctx* ctx, int num_port,
                                  uint16_t user_pad_interval, bool static_pad,
                                  const float* trained_pad_interval, bool wide) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st20_packet_group_info* tail = &s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL];
  uint32_t width = ctx->rl_width ? ctx->rl_width : 1920;
  uint32_t height = ctx->rl_height ? ctx->rl_height : 1080;
  uint32_t lines = ctx->rl_interlaced ? height / 2 : height;
  uint32_t extra = 0;

  if (st_get_fps_timing(ctx->rl_fps, &s->fps_tm) < 0) return -EINVAL;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.width = width;
  s->ops.height = height;
  s->ops.fps = ctx->rl_fps;
  s->ops.packing = ST20_PACKING_GPM;
  s->ops.interlaced = ctx->rl_interlaced;
  s->ops.num_port = num_port;
  s->ops.pad_interval = user_pad_interval;
  if (static_pad) s->ops.flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;
  s->ops.pacing = wide ? ST21_PACING_WIDE : ST21_PACING_NARROW;
  /* tv_init_pkt()'s GPM: 480 pixels a 1200-byte pkt, a field's lines when interlaced */
  s->st20_total_pkts = (int)((width * lines + 479) / 480);
  s->st20_pkt_len = 1200;
  s->st20_pkt_size = 1262;
  s->st20_bytes_in_line = (int)(width * 5 / 2);
  s->st20_frame_size = (size_t)s->st20_bytes_in_line * lines;
  /* as tv_init_pkt(): a frame that is not whole pkts ends in a FRAME_TAIL */
  if (s->st20_frame_size % s->st20_pkt_len) {
    tail->number = 1;
    tail->size =
        s->st20_frame_size % s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
  }
  /* the pkts crossing a line end, 480 at 1280x720 */
  for (int i = 0; i < s->st20_total_pkts; i++)
    if (tv_train_pad_type(s, i) == ST20_PKT_TYPE_EXTRA) extra++;
  s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
      s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
  s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = extra;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
      s->st20_total_pkts - extra - tail->number;
  s->st21_vrx_narrow = 8;
  s->st21_vrx_wide = 720;
  /* before the tags below, which the failed tv_init_hw() would free */
  for (int i = 0; i < num_port; i++)
    ctx->rl_txq[i].bps =
        ctx->rl_bps_set[i] ? ctx->rl_bps[i] : ut_txv_init_hw_bps(ctx, (enum mtl_port)i);
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
    if (trained_pad_interval) {
      struct mt_pacing_pad_mix mix;
      tv_train_mix(s, &mix);
      mt_pacing_train_pad_result_add(&ctx->impl, (enum mtl_port)i, tv_rl_bps(s),
                                     ctx->rl_txq[i].bps, &mix, trained_pad_interval[i]);
    }
    ctx->rl_queue[i].txq = &ctx->rl_txq[i];
    s->queue[i] = &ctx->rl_queue[i];
  }
  return 0;
}

int ut_txv_init_rl_pacing(ut_txv_ctx* ctx, int num_port, uint16_t user_pad_interval,
                          bool static_pad, const float* trained_pad_interval, bool wide) {
  int ret = ut_txv_setup_rl_pacing(ctx, num_port, user_pad_interval, static_pad,
                                   trained_pad_interval, wide);
  if (ret < 0) return ret;
  return tv_init_pacing(&ctx->impl, &ctx->session);
}

int ut_txv_init_rl_pacing_at_bulk(ut_txv_ctx* ctx, unsigned int bulk, bool train_fails) {
  static const float trained[MTL_SESSION_PORT_MAX] = {164, 164};
  struct st_tx_video_session_impl* s = &ctx->session;

  s->s_type = MT_HANDLE_TX_VIDEO;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  ctx->ptp_unstable = train_fails;
  int ret = ut_txv_setup_rl_pacing(ctx, 1, 0, false, train_fails ? NULL : trained, false);
  if (ret < 0) return ret;
  s->bulk = bulk;
  return tv_init_pacing(&ctx->impl, s);
}

unsigned int ut_txv_bulk(const ut_txv_ctx* ctx) {
  return ctx->session.bulk;
}

uint32_t ut_txv_pacing_vrx(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.vrx;
}

enum st21_tx_pacing_way ut_txv_pacing_way(const ut_txv_ctx* ctx, int port) {
  return ctx->session.pacing_way[port];
}

static struct rte_mbuf* ut_txv_train_pad(uint16_t len) {
  struct rte_mbuf* pad = rte_pktmbuf_alloc(ut_pool());
  if (pad && !rte_pktmbuf_append(pad, len)) {
    rte_pktmbuf_free(pad);
    return NULL;
  }
  return pad;
}

/* tv_train_pacing() of the session the caller laid out, against the RL queue mock: a
 * NORMAL pad drains in pad_ns, any other by its bytes. Builds a pad for each pkt type the
 * frame has, as tv_init_hw() does. */
static int ut_txv_train_rl(ut_txv_ctx* ctx, uint16_t nb_tx_desc, long double pad_ns) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  int ret = 0;

  inf->drv_info.drv_type = MT_DRV_IAVF;
  inf->nb_tx_desc = nb_tx_desc;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_RL;
  ctx->rl_txq[MTL_SESSION_PORT_P].bps = ut_txv_init_hw_bps(ctx, MTL_PORT_P);
  ctx->rl_queue[MTL_SESSION_PORT_P].txq = &ctx->rl_txq[MTL_SESSION_PORT_P];
  s->queue[MTL_SESSION_PORT_P] = &ctx->rl_queue[MTL_SESSION_PORT_P];
  for (int t = 0; t < ST20_PKT_TYPE_MAX; t++) {
    s->pad[MTL_SESSION_PORT_P][t] = NULL;
    if (!s->st20_pkt_info[t].number) continue;
    s->pad[MTL_SESSION_PORT_P][t] = ut_txv_train_pad(s->st20_pkt_info[t].size);
    if (!s->pad[MTL_SESSION_PORT_P][t]) ret = -ENOMEM;
  }
  ctx->train_pad_ns = pad_ns;
  ctx->train_normal_len = s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
  ctx->train_drain_end_ns = ctx->mock_tsc_ns;

  if (!ret) {
    ut_txv_train_ctx = ctx;
    ret = tv_train_pacing(&ctx->impl, s, MTL_SESSION_PORT_P);
    ut_txv_train_ctx = NULL;
  }
  for (int t = 0; t < ST20_PKT_TYPE_MAX; t++) {
    rte_pktmbuf_free(s->pad[MTL_SESSION_PORT_P][t]);
    s->pad[MTL_SESSION_PORT_P][t] = NULL;
  }
  s->queue[MTL_SESSION_PORT_P] = NULL;
  return ret;
}

int ut_txv_train_rl_pacing(ut_txv_ctx* ctx, uint16_t nb_tx_desc, long double pad_ns,
                           bool bpm) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st20_packet_group_info* normal = &s->st20_pkt_info[ST20_PKT_TYPE_NORMAL];
  struct st20_packet_group_info* extra = &s->st20_pkt_info[ST20_PKT_TYPE_EXTRA];
  struct st20_packet_group_info* tail = &s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL];

  if (st_get_fps_timing(ST_FPS_P59_94, &s->fps_tm) < 0) return -EINVAL;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.fps = ST_FPS_P59_94;
  s->ops.num_port = 1;
  s->st20_bytes_in_line = 4800;
  s->st20_frame_size = (size_t)4800 * 1080;
  if (bpm) { /* 1028 of the 4115 pkts cross a line end: EXTRA, 6 bytes longer; the
              * last holds the frame's last 360 bytes */
    s->ops.packing = ST20_PACKING_BPM;
    s->st20_total_pkts = 4115;
    s->st20_pkt_len = ST_VIDEO_BPM_SIZE;
    extra->number = 1028;
    tail->number = 1;
    tail->size = 360 + sizeof(struct st_rfc4175_video_hdr);
  } else { /* NORMAL pads only */
    s->ops.packing = ST20_PACKING_GPM_SL;
    s->st20_total_pkts = 4320;
    s->st20_pkts_in_line = 4;
    s->st20_pkt_len = 1200;
    extra->number = 0;
    tail->number = 0;
  }
  s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
  normal->size = s->st20_pkt_size;
  normal->number = s->st20_total_pkts - extra->number - tail->number;
  extra->size = normal->size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
  s->pacing.trs = (long double)s->fps_tm.den * NS_PER_S / s->fps_tm.mul * 1080 / 1125 /
                  s->st20_total_pkts;
  return ut_txv_train_rl(ctx, nb_tx_desc, pad_ns);
}

int ut_txv_init_frame(ut_txv_ctx* ctx, enum st20_packing packing, enum st20_fmt fmt,
                      uint32_t width, uint32_t height, bool interlaced) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st20_tx_ops* ops = &s->ops;
  uint32_t lines = interlaced ? height >> 1 : height;
  int ret;

  if (st_get_fps_timing(ST_FPS_P25, &s->fps_tm) < 0) return -EINVAL;
  s->s_type = MT_HANDLE_TX_VIDEO;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->packing = packing;
  ops->fmt = fmt;
  ops->width = width;
  ops->height = height;
  ops->interlaced = interlaced;
  ops->fps = ST_FPS_P25;
  ops->num_port = 1;
  if (st20_get_pgroup(fmt, &s->st20_pg) < 0) return -EINVAL;
  /* as mtl_init() and tv_attach() set them, a field's lines when interlaced */
  ctx->impl.pkt_udp_suggest_max_size = MTL_PKT_MAX_RTP_BYTES;
  s->st20_linesize =
      (width * s->st20_pg.size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
  s->st20_frame_size = width * lines * s->st20_pg.size / s->st20_pg.coverage;
  ret = tv_init_pkt(&ctx->impl, s, ops, NULL);
  if (ret < 0) return ret;
  s->pacing.trs = (long double)s->fps_tm.den * NS_PER_S / s->fps_tm.mul * 1080 / 1125 /
                  s->st20_total_pkts;
  return 0;
}

int ut_txv_mis_sized_pads(ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans frame = {.addr = calloc(1, s->st20_frame_size)};
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(ut_pool());
  int mis_sized = 0;

  if (frame.addr && pkt) {
    s->st20_frames = &frame;
    s->st20_frame_idx = 0;
    for (s->st20_pkt_idx = 0; s->st20_pkt_idx < s->st20_total_pkts; s->st20_pkt_idx++) {
      tv_build_st20(s, pkt);
      if (pkt->data_len != s->st20_pkt_info[tv_train_pad_type(s, s->st20_pkt_idx)].size)
        mis_sized++;
    }
    s->st20_frames = NULL;
  } else {
    mis_sized = -ENOMEM;
  }
  rte_pktmbuf_free(pkt);
  free(frame.addr);
  return mis_sized;
}

int ut_txv_train_rl_gpm_sl(ut_txv_ctx* ctx, enum st20_fmt fmt, uint32_t width,
                           uint32_t height, long double pad_trs) {
  int ret = ut_txv_init_frame(ctx, ST20_PACKING_GPM_SL, fmt, width, height, false);
  if (ret < 0) return ret;
  return ut_txv_train_rl(ctx, 512, ctx->session.pacing.trs * pad_trs);
}

long double ut_txv_gpm_sl_frame_ns(const ut_txv_ctx* ctx) {
  const struct st_tx_video_session_impl* s = &ctx->session;
  uint32_t hdr = sizeof(struct st_rfc4175_video_hdr);
  uint32_t pkts_in_line = s->st20_pkts_in_line;
  /* tv_build_st20() packs the line in pkts of st20_pkt_len, the last one the rest */
  uint32_t last = s->st20_bytes_in_line - (pkts_in_line - 1) * s->st20_pkt_len;
  uint32_t line = (pkts_in_line - 1) * (s->st20_pkt_len + hdr) + last + hdr;
  uint32_t lines = s->st20_total_pkts / pkts_in_line;
  long double pads = (long double)s->st20_total_pkts / s->pacing.pad_interval;

  /* the shaper drains a pkt by its bytes, video_burst_packet()'s pads are NORMAL */
  return ctx->train_pad_ns * ((long double)lines * line / ctx->train_normal_len + pads);
}

uint32_t ut_txv_line_tail_pkts(const ut_txv_ctx* ctx) {
  return ctx->session.st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number;
}

uint32_t ut_txv_normal_pkts(const ut_txv_ctx* ctx) {
  return ctx->session.st20_pkt_info[ST20_PKT_TYPE_NORMAL].number;
}

float ut_txv_pad_interval(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.pad_interval;
}

void ut_txv_set_rtp_level(ut_txv_ctx* ctx, bool enable) {
  ctx->session.ops.type = enable ? ST20_TYPE_RTP_LEVEL : ST20_TYPE_FRAME_LEVEL;
}

int ut_txv_pad_result_add(ut_txv_ctx* ctx, uint64_t bps, uint64_t queue_bps,
                          uint64_t bytes, uint32_t normal, uint32_t pkts,
                          float pad_interval) {
  struct mt_pacing_pad_mix mix = {.bytes = bytes, .normal = normal, .pkts = pkts};
  return mt_pacing_train_pad_result_add(&ctx->impl, MTL_PORT_P, bps, queue_bps, &mix,
                                        pad_interval);
}

int ut_txv_pad_result_search(ut_txv_ctx* ctx, uint64_t bps, uint64_t queue_bps,
                             uint64_t bytes, uint32_t normal, uint32_t pkts,
                             float* pad_interval) {
  struct mt_pacing_pad_mix mix = {.bytes = bytes, .normal = normal, .pkts = pkts};
  return mt_pacing_train_pad_result_search(&ctx->impl, MTL_PORT_P, bps, queue_bps, &mix,
                                           pad_interval);
}

int ut_txv_add_trained_bps(ut_txv_ctx* ctx, uint64_t bps) {
  return mt_pacing_train_bps_result_add(&ctx->impl, MTL_PORT_P, tv_rl_bps(&ctx->session),
                                        bps);
}

uint64_t ut_txv_train_queued_ns(const ut_txv_ctx* ctx) {
  long double left = ctx->train_drain_end_ns - ctx->mock_tsc_ns;
  return left > 0 ? (uint64_t)left : 0;
}

uint64_t ut_txv_slept_ns(const ut_txv_ctx* ctx) {
  return ctx->slept_ns;
}

uint64_t ut_txv_rl_queue_bps(const ut_txv_ctx* ctx, int port) {
  return ctx->rl_txq[port].bps;
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
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    ctx->impl.inf[i].nb_tx_desc = nb_tx_desc;
    ctx->impl.inf[i].tx_deep = nb_tx_desc == MT_DEV_TX_DESC_DEEP;
  }
  ut_txv_reclaim_session(ctx, c);
  tv_init_tx_chain(&ctx->impl, &ctx->session);
  *zero_copy = !ctx->session.tx_no_chain;
  *tx_reclaim = ctx->session.reclaim.enabled;
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

void ut_txv_set_rl_format(ut_txv_ctx* ctx, uint32_t width, uint32_t height,
                          bool interlaced, enum st_fps fps) {
  ctx->rl_width = width;
  ctx->rl_height = height;
  ctx->rl_interlaced = interlaced;
  ctx->rl_fps = fps;
}

void ut_txv_set_rl_queue_bps(ut_txv_ctx* ctx, int port, uint64_t bps) {
  ctx->rl_bps_set[port] = true;
  ctx->rl_bps[port] = bps;
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

bool ut_txv_rl_armed(const ut_txv_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].armed;
}

uint16_t ut_txv_rl_pad_size(const ut_txv_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].pad_size;
}

int64_t ut_txv_rl_trim_pads(const ut_txv_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].trim_pads;
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
  ctx->impl.inf[MTL_PORT_P].tx_deep = nb_tx_desc == MT_DEV_TX_DESC_DEEP;
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

static int ut_txv_rtcp_mempool_init(ut_txv_ctx* ctx, unsigned int* hdr_size,
                                    unsigned int* chain_size) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int ret = tv_mempool_init(&ctx->impl, &ctx->mgr, s);

  if (ret < 0) return ret;
  *hdr_size = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]->size;
  if (chain_size) *chain_size = s->mbuf_mempool_chain->size;
  return tv_mempool_free(s);
}

int ut_txv_rtcp_mempool_sizes(ut_txv_ctx* ctx, uint16_t nb_tx_desc,
                              uint16_t rtcp_buffer_size, unsigned int* hdr_size,
                              unsigned int* chain_size) {
  struct st_tx_video_session_impl* s = &ctx->session;

  ctx->impl.inf[MTL_PORT_P].nb_tx_desc = nb_tx_desc;
  ctx->impl.inf[MTL_PORT_P].tx_deep = nb_tx_desc == MT_DEV_TX_DESC_DEEP;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.num_port = 1;
  s->ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  s->ops.rtcp.buffer_size = rtcp_buffer_size;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  s->bulk = RTE_MIN(4, ST_SESSION_MAX_BULK);
  s->st20_pkt_size = 1262;
  s->socket_id = (int)rte_socket_id();
  return ut_txv_rtcp_mempool_init(ctx, hdr_size, chain_size);
}

int ut_txv_rtcp_recovery_mempool_sizes(ut_txv_ctx* ctx, uint16_t nb_tx_desc,
                                       uint16_t rtcp_buffer_size, unsigned int* hdr_size,
                                       uint16_t* rtcp_ring,
                                       unsigned int* recovery_hdr_size) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int ret = ut_txv_rtcp_mempool_sizes(ctx, nb_tx_desc, rtcp_buffer_size, hdr_size, NULL);

  if (ret < 0) return ret;
  ret = tv_init_rtcp(&ctx->impl, &ctx->mgr, s);
  if (ret < 0) return ret;
  *rtcp_ring = ut_txv_rtcp_ring;
  ret = ut_txv_rtcp_mempool_init(ctx, recovery_hdr_size, NULL);
  tv_uinit_rtcp(s);
  return ret;
}

int ut_txv_recovery_setup(ut_txv_ctx* ctx, int num_port) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->ops.num_port = num_port;
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.notify_event = ut_txv_notify_event;
  s->tx_mono_pool = true;
  s->tx_no_chain = true;
  s->active = true;
  /* a frame of 1080p GPM, for tv_rl_bps(), whose pkts are on their way */
  s->st20_pkt_size = 1262;
  s->st20_total_pkts = 4320;
  s->st20_frames = &ctx->recovery_frame;
  s->st20_frames_cnt = 1;
  rte_atomic32_set(&ctx->recovery_frame.refcnt, 1);
  for (int p = 0; p < num_port; p++) {
    s->port_maps[p] = (enum mtl_port)p;
    /* tags: the mocks only compare them */
    ctx->impl.inf[p].tx_mbuf_pool = (struct rte_mempool*)&ctx->trim_pad_tag;
    s->pad[p][ST20_PKT_TYPE_NORMAL] = (struct rte_mbuf*)&ctx->trim_pad_tag;
    for (int q = 0; q < 2; q++) ctx->recovery_queue[p][q].txq = &ctx->recovery_txq[p][q];
    s->queue[p] = &ctx->recovery_queue[p][0];
    s->last_burst_succ_time_tsc[p] = ctx->mock_tsc_ns;
  }
  return 0;
}

int ut_txv_recovery_frame_refcnt(const ut_txv_ctx* ctx) {
  return rte_atomic32_read(&ctx->recovery_frame.refcnt);
}

bool ut_txv_recovery_session_dead(const ut_txv_ctx* ctx) {
  const struct st_tx_video_session_impl* s = &ctx->session;
  return !s->active && s->port_user_stats.common.stat_unrecoverable_error;
}

int ut_txv_recovery_fatal_events(const ut_txv_ctx* ctx) {
  return ctx->fatal_events;
}

int ut_txv_recovery_recovered_events(const ut_txv_ctx* ctx) {
  return ctx->recovery_events;
}

void ut_txv_recovery_set_queue_bps(ut_txv_ctx* ctx, int leg, uint64_t bps) {
  ctx->recovery_txq[leg][0].bps = bps;
}

uint64_t ut_txv_txq_get_bps(const ut_txv_ctx* ctx) {
  return ctx->txq_get_bytes_per_sec;
}

void ut_txv_recovery_set_flush_ret(ut_txv_ctx* ctx, int leg, int ret) {
  ctx->recovery_flush_ret[leg] = ret;
}

static void ut_txv_count_recovery_warns(enum mtl_log_level level, const char* format,
                                        ...) {
  (void)format;
  if (level == MTL_LOG_LEVEL_WARNING) ut_txv_recovery_ctx->recovery_warns++;
}

int ut_txv_recover(ut_txv_ctx* ctx, int leg) {
  enum mtl_log_level level = mt_get_log_global_level();

  ut_txv_recovery_ctx = ctx;
  mt_set_log_global_level(MTL_LOG_LEVEL_WARNING);
  mtl_set_log_printer(ut_txv_count_recovery_warns);
  int ret = st20_tx_queue_fatal_error(&ctx->impl, &ctx->session, leg);
  mtl_set_log_printer(NULL);
  mt_set_log_global_level(level);
  ut_txv_recovery_ctx = NULL;
  return ret;
}

int ut_txv_recovery_warns(const ut_txv_ctx* ctx) {
  return ctx->recovery_warns;
}

int ut_txv_recovery_flushes(const ut_txv_ctx* ctx, int leg) {
  return ctx->recovery_flushes[leg];
}

int ut_txv_recovery_fallback_flushes(const ut_txv_ctx* ctx, int leg) {
  return ctx->recovery_fallback_flushes[leg];
}

int ut_txv_recovery_done_cleanups(const ut_txv_ctx* ctx, int leg) {
  return ctx->recovery_done_cleanups[leg];
}

/* ── builder and transmitter over modeled NIC rings ───────────────────── */

static struct rte_mempool* ut_txv_pipe_pool(unsigned int n, uint16_t priv_size,
                                            uint16_t data_room) {
  static unsigned int pool_idx;
  char name[RTE_MEMPOOL_NAMESIZE];

  (void)snprintf(name, sizeof(name), "ut_txv_pipe_%u", pool_idx++);
  return rte_pktmbuf_pool_create(name, n, 0, priv_size, RTE_PKTMBUF_HEADROOM + data_room,
                                 (int)rte_socket_id());
}

static int ut_txv_pipe_hold(struct ut_txv_ctx* ctx, struct rte_mempool* pool,
                            unsigned int n) {
  for (unsigned int i = 0; i < n; i++) {
    if (ctx->pipe_held_cnt >= UT_TXV_PIPE_HOLD_MAX) return -ENOSPC;
    struct rte_mbuf* m = rte_pktmbuf_alloc(pool);
    if (!m) return -ENOMEM;
    ctx->pipe_held[ctx->pipe_held_cnt++] = m;
  }
  return 0;
}

static int ut_txv_pipe_pools(struct ut_txv_ctx* ctx, const struct ut_txv_pipe_case* c) {
  struct st_tx_video_session_impl* s = &ctx->session;
  uint16_t hdr_room = s->tx_no_chain ? s->st20_pkt_size
                                     : sizeof(struct st_rfc4175_video_hdr) +
                                           sizeof(struct st20_rfc4175_extra_rtp_hdr);

  if (!c->hdr_pool[MTL_SESSION_PORT_P]) return tv_mempool_init(&ctx->impl, &ctx->mgr, s);
  for (int i = 0; i < c->num_port; i++) {
    s->mbuf_mempool_hdr[i] =
        ut_txv_pipe_pool(c->hdr_pool[i], sizeof(struct mt_muf_priv_data), hdr_room);
    if (!s->mbuf_mempool_hdr[i] ||
        ut_txv_pipe_hold(ctx, s->mbuf_mempool_hdr[i], c->hold) < 0)
      return -ENOMEM;
  }
  if (s->tx_no_chain) return 0;
  s->mbuf_mempool_chain = ut_txv_pipe_pool(c->chain_pool, 0, 0);
  if (!s->mbuf_mempool_chain || ut_txv_pipe_hold(ctx, s->mbuf_mempool_chain, c->hold) < 0)
    return -ENOMEM;
  if (!c->linesize_pad) return 0;
  s->mbuf_mempool_copy_chain = ut_txv_pipe_pool(c->copy_pool, 0, s->st20_pkt_len);
  if (!s->mbuf_mempool_copy_chain ||
      ut_txv_pipe_hold(ctx, s->mbuf_mempool_copy_chain, c->hold) < 0)
    return -ENOMEM;
  return 0;
}

int ut_txv_pipe_setup(ut_txv_ctx* ctx, const struct ut_txv_pipe_case* c) {
  static unsigned int ring_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st20_tx_ops* ops = &s->ops;
  char name[RTE_RING_NAMESIZE];

  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->fmt = ST20_FMT_YUV_422_10BIT;
  ops->packing = ST20_PACKING_BPM;
  ops->width = c->width;
  ops->height = c->height;
  ops->fps = ST_FPS_P59_94;
  ops->num_port = c->num_port;
  ops->framebuff_cnt = c->frames_cnt;
  if (st20_get_pgroup(ops->fmt, &s->st20_pg) < 0) return -EINVAL;
  if (st_get_fps_timing(ops->fps, &s->fps_tm) < 0) return -EINVAL;
  s->st20_linesize = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_frame_size = s->st20_linesize * ops->height;
  s->st20_linesize += c->linesize_pad;
  s->st20_fb_size = s->st20_linesize * ops->height;
  s->st20_frames_cnt = c->frames_cnt;
  if (tv_init_pkt(&ctx->impl, s, ops, NULL) < 0) return -EINVAL;
  s->st21_vrx_narrow = 8;
  s->bulk = RTE_MIN(4, ST_SESSION_MAX_BULK);
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  while (s->ring_count > s->st20_total_pkts) s->ring_count /= 2;
  s->tx_no_chain = c->copy;
  s->socket_id = (int)rte_socket_id();
  s->tx_hang_detect_time_thresh = UINT64_MAX;
  ctx->impl.pkt_udp_suggest_max_size = MTL_UDP_MAX_BYTES;
  for (int i = 0; i < c->num_port; i++) {
    struct mt_interface* inf = &ctx->impl.inf[i];
    inf->nb_tx_desc = c->nb_tx_desc[i];
    inf->tx_deep = !c->user_nb_tx_desc && c->nb_tx_desc[i] == MT_DEV_TX_DESC_DEEP;
    inf->drv_info.drv_type = c->iavf ? MT_DRV_IAVF : MT_DRV_DEFAULT;
    inf->ptp_get_time_fn = ut_txv_ptp_time_fn;
    s->port_maps[i] = (enum mtl_port)i;
    s->pacing_way[i] = ST21_TX_PACING_WAY_TSC;
    ctx->pipe_ring[i] = ut_txr_create((enum ut_txr_mode)c->ring_mode, c->nb_tx_desc[i],
                                      32, c->free_thresh, c->hung[i] ? UT_TXR_HUNG : 1);
    (void)snprintf(name, sizeof(name), "ut_txv_pipe_%u", ring_idx++);
    s->ring[i] =
        rte_ring_create(name, s->ring_count, s->socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!ctx->pipe_ring[i] || !s->ring[i]) return -ENOMEM;
    s->queue[i] = &ctx->pipe_queue[i];
    st_video_resolve_pacing_tasklet(s, i);
  }
  s->tx_direct_chain = tv_tx_direct_chain(&ctx->impl, s);
  if (tv_init_pacing(&ctx->impl, s) < 0) return -EINVAL;
  if (ut_txv_pipe_pools(ctx, c) < 0) return -ENOMEM;
  if (tv_alloc_frames(&ctx->impl, s) < 0) return -ENOMEM;
  ctx->pipe_on = true;
  return 0;
}

void ut_txv_pipe_hang(ut_txv_ctx* ctx, int leg) {
  ut_txr_set_drain_ns(ctx->pipe_ring[leg], UT_TXR_HUNG);
}

static void ut_txv_pipe_note(struct ut_txv_ctx* ctx) {
  int code = -ctx->session.stat_build_ret_code;

  if (!ctx->pipe_first_alloc_fail &&
      (code == STI_FRAME_PKT_ALLOC_FAIL || code == STI_FRAME_PKT_ALLOC_CHAIN_FAIL ||
       code == STI_FRAME_PKT_ALLOC_R_FAIL ||
       ctx->session.port_user_stats.stat_pkts_chain_realloc_fail))
    ctx->pipe_first_alloc_fail = ctx->pipe_step;
}

void ut_txv_pipe_run(ut_txv_ctx* ctx, uint64_t duration_ns, uint64_t step_ns) {
  struct st_tx_video_session_impl* s = &ctx->session;
  uint64_t end = ctx->mock_tsc_ns + duration_ns;

  ut_txv_active_burst_ctx = ctx;
  while (ctx->mock_tsc_ns < end && !ctx->pipe_first_alloc_fail &&
         !ctx->pipe_first_burst_fail) {
    ctx->pipe_step++;
    tvs_tasklet_handler(&ctx->mgr);
    ut_txv_pipe_note(ctx);
    if (s->stat_build_ret_code == -STI_FRAME_APP_ERR_TX_FRAME) {
      ctx->pipe_first_frame_busy = ctx->pipe_step;
      break; /* the builder errs on each later try */
    }
    for (int i = 0; i < s->ops.num_port; i++)
      s->pacing_tasklet_func[i](&ctx->impl, s, (enum mtl_session_port)i);
    ut_txv_advance(ctx, step_ns);
  }
  ut_txv_active_burst_ctx = NULL;
}

int ut_txv_pipe_teardown(ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int ret;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->ring[i]) {
      ut_ring_drain(s->ring[i]);
      rte_ring_free(s->ring[i]);
      s->ring[i] = NULL;
    }
    if (s->inflight[i][0]) rte_pktmbuf_free_bulk(s->inflight[i], s->bulk);
    s->inflight[i][0] = NULL;
    for (int j = 0; j < s->trs_inflight_num[i]; j++)
      rte_pktmbuf_free(s->trs_inflight[i][s->trs_inflight_idx[i] + j]);
    s->trs_inflight_num[i] = 0;
    ut_txr_destroy(ctx->pipe_ring[i]);
    ctx->pipe_ring[i] = NULL;
    s->queue[i] = NULL;
    ut_txv_pipe_free_built(ctx, i, ctx->pipe_built_cnt[i] - ctx->pipe_built_freed[i]);
  }
  for (int i = 0; i < ctx->pipe_held_cnt; i++) rte_pktmbuf_free(ctx->pipe_held[i]);
  ctx->pipe_held_cnt = 0;
  tv_free_frames(s);
  ret = tv_mempool_free(s);
  ctx->pipe_on = false;
  return ret;
}

uint64_t ut_txv_pipe_first_frame_busy(const ut_txv_ctx* ctx) {
  return ctx->pipe_first_frame_busy;
}

uint64_t ut_txv_pipe_frames_asked(const ut_txv_ctx* ctx) {
  return ctx->pipe_frames_asked;
}

uint64_t ut_txv_pipe_first_alloc_fail(const ut_txv_ctx* ctx) {
  return ctx->pipe_first_alloc_fail;
}

uint64_t ut_txv_pipe_first_burst_fail(const ut_txv_ctx* ctx) {
  return ctx->pipe_first_burst_fail;
}

int ut_txv_pipe_total_pkts(const ut_txv_ctx* ctx) {
  return ctx->session.st20_total_pkts;
}

unsigned int ut_txv_pipe_pool_size(const ut_txv_ctx* ctx, int pool) {
  const struct st_tx_video_session_impl* s = &ctx->session;
  struct rte_mempool* mp = pool < UT_TXV_PIPE_CHAIN    ? s->mbuf_mempool_hdr[pool]
                           : pool == UT_TXV_PIPE_CHAIN ? s->mbuf_mempool_chain
                                                       : s->mbuf_mempool_copy_chain;
  return mp ? mp->size : 0;
}

unsigned int ut_txv_pipe_mempool_n(ut_txv_ctx* ctx, int leg) {
  return tv_mempool_n(&ctx->impl, &ctx->session, leg);
}

unsigned int ut_txv_pipe_rl_mempool_n(ut_txv_ctx* ctx, int leg) {
  struct st_tx_video_session_impl* s = &ctx->session;
  enum st21_tx_pacing_way way = s->pacing_way[leg];

  s->pacing_way[leg] = ST21_TX_PACING_WAY_RL;
  unsigned int n = tv_mempool_n(&ctx->impl, s, leg);
  s->pacing_way[leg] = way;
  return n;
}

void ut_txv_set_tx_video_migrate(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->impl.user_para.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
  else
    ctx->impl.user_para.flags &= ~MTL_FLAG_TX_VIDEO_MIGRATE;
}

unsigned int ut_txv_pipe_copy_mempool_n(ut_txv_ctx* ctx, unsigned int chain_n) {
  return tv_copy_mempool_n(&ctx->session, chain_n);
}

void ut_txv_pipe_geometry(const ut_txv_ctx* ctx, uint32_t* pkt_len, uint32_t* line_bytes,
                          uint32_t* frame_size, int* total_pkts) {
  const struct st_tx_video_session_impl* s = &ctx->session;
  *pkt_len = s->st20_pkt_len;
  *line_bytes = s->st20_bytes_in_line;
  *frame_size = s->st20_frame_size;
  *total_pkts = s->st20_total_pkts;
}

/* ── direct chain ─────────────────────────────────────────────────────── */

static enum mt_driver_type ut_txv_drv_type(enum ut_txv_drv drv) {
  switch (drv) {
    case UT_TXV_DRV_ICE:
      return MT_DRV_ICE;
    case UT_TXV_DRV_IGC:
      return MT_DRV_IGC;
    case UT_TXV_DRV_I40E:
      return MT_DRV_I40E;
    case UT_TXV_DRV_MLX5:
      return MT_DRV_MLX5;
    case UT_TXV_DRV_DEFAULT:
      return MT_DRV_DEFAULT;
    default:
      return MT_DRV_IAVF;
  }
}

bool ut_txv_tx_direct_chain(ut_txv_ctx* ctx, const struct ut_txv_direct_chain_case* c) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->ops.num_port = 2;
  s->ops.type = c->rtp_level     ? ST20_TYPE_RTP_LEVEL
                : c->slice_level ? ST20_TYPE_SLICE_LEVEL
                                 : ST20_TYPE_FRAME_LEVEL;
  if (c->rtcp) s->ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  s->tx_no_chain = c->no_chain;
  s->tx_mono_pool = c->mono_pool;
  ctx->impl.iova_mode = c->iova_pa ? RTE_IOVA_PA : RTE_IOVA_VA;
  if (c->shared_txq) ctx->impl.user_para.flags |= MTL_FLAG_SHARED_TX_QUEUE;
  for (int i = 0; i < 2; i++) {
    s->port_maps[i] = (enum mtl_port)i;
    ctx->impl.inf[i].drv_info.drv_type = ut_txv_drv_type(c->drv[i]);
  }
  return tv_tx_direct_chain(&ctx->impl, s);
}

int ut_txv_attach_direct_chain(ut_txv_ctx* ctx, bool mono_pool, bool* direct) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mtl_init_params* p = &ctx->impl.user_para;
  struct st20_tx_ops ops;

  memset(&ops, 0, sizeof(ops));
  p->num_ports = 1;
  (void)snprintf(p->port[MTL_PORT_P], MTL_PORT_MAX_LEN, "ut_txv_port0");
  (void)snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "ut_txv_port0");
  if (mono_pool) p->flags |= MTL_FLAG_TX_MONO_POOL;
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  inf->drv_info.drv_type = MT_DRV_IAVF;
  inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
  inf->nb_tx_desc = MT_DEV_TX_DESC;
  ops.num_port = 1;
  ops.type = ST20_TYPE_FRAME_LEVEL;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.packing = ST20_PACKING_BPM;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.framebuff_cnt = 3;
  ops.get_next_frame = ut_txv_get_next_frame;
  ops.notify_frame_done = ut_txv_notify_frame_done;
  ops.priv = ctx;
  int ret = tv_attach(&ctx->impl, &ctx->mgr, s, &ops, MT_HANDLE_TX_VIDEO, NULL);
  *direct = s->tx_direct_chain;
  return ret;
}

unsigned int ut_txv_attach_bulk(ut_txv_ctx* ctx, const struct ut_txv_bulk_case* c) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mtl_init_params* p = &ctx->impl.user_para;
  struct st20_tx_ops ops;
  struct st22_tx_ops st22_ops;

  memset(&ops, 0, sizeof(ops));
  memset(&st22_ops, 0, sizeof(st22_ops));
  p->num_ports = c->num_port;
  ctx->impl.pkt_udp_suggest_max_size = MTL_UDP_MAX_BYTES;
  for (int i = 0; i < c->num_port; i++) {
    struct mt_interface* inf = &ctx->impl.inf[i];
    (void)snprintf(p->port[i], MTL_PORT_MAX_LEN, "ut_txv_port%d", i);
    (void)snprintf(ops.port[i], MTL_PORT_MAX_LEN, "ut_txv_port%d", i);
    inf->drv_info.drv_type = MT_DRV_IAVF;
    inf->feature |= MT_IF_FEATURE_TX_MULTI_SEGS;
    inf->tx_deep = c->deep;
    inf->nb_tx_desc = c->deep ? MT_DEV_TX_DESC_DEEP : MT_DEV_TX_DESC;
    inf->tx_pacing_way = c->way[i];
  }
  ops.num_port = c->num_port;
  ops.type = c->rtp_level     ? ST20_TYPE_RTP_LEVEL
             : c->slice_level ? ST20_TYPE_SLICE_LEVEL
                              : ST20_TYPE_FRAME_LEVEL;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.packing = ST20_PACKING_BPM;
  ops.width = 1920;
  ops.height = c->height ? c->height : 1080;
  ops.fps = ST_FPS_P59_94;
  ops.framebuff_cnt = 3;
  if (c->disable_bulk) ops.flags |= ST20_TX_FLAG_DISABLE_BULK;
  ops.get_next_frame = ut_txv_get_next_frame;
  ops.notify_frame_done = ut_txv_notify_frame_done;
  ops.rtp_frame_total_pkts = 4320;
  ops.rtp_pkt_size = 1200 + sizeof(struct st_rfc4175_video_hdr);
  ops.rtp_ring_size = 1024;
  ops.notify_rtp_done = ut_txv_notify_rtp_done;
  ops.priv = ctx;
  st22_ops.framebuff_max_size = (size_t)64 * 1024;
  (void)tv_attach(&ctx->impl, &ctx->mgr, s, &ops,
                  c->st22 ? MT_ST22_HANDLE_TX_VIDEO : MT_HANDLE_TX_VIDEO,
                  c->st22 ? &st22_ops : NULL);
  return s->bulk;
}

unsigned int ut_txv_first_pool_n(const ut_txv_ctx* ctx) {
  return ctx->first_pool_n;
}

int ut_txv_pipe_build(ut_txv_ctx* ctx, int pkts) {
  struct st_tx_video_session_impl* s = &ctx->session;
  int target = RTE_MIN(pkts, (int)s->st20_total_pkts);
  struct rte_mbuf* m;

  for (int step = 0; step < 1000 && ctx->pipe_built_cnt[MTL_SESSION_PORT_P] < target;
       step++) {
    tvs_tasklet_handler(&ctx->mgr);
    for (int i = 0; i < s->ops.num_port; i++) {
      while (rte_ring_sc_dequeue(s->ring[i], (void**)&m) == 0) {
        if (st_tx_mbuf_get_idx(m) == ST_TX_DUMMY_PKT_IDX) {
          rte_pktmbuf_free(m);
          continue;
        }
        if (ctx->pipe_built_cnt[i] >= UT_TXV_PIPE_BUILT_MAX) {
          rte_pktmbuf_free(m);
          return -ENOSPC;
        }
        ctx->pipe_built[i][ctx->pipe_built_cnt[i]++] = m;
      }
    }
  }
  return ctx->pipe_built_cnt[MTL_SESSION_PORT_P];
}

void ut_txv_pipe_payload(const ut_txv_ctx* ctx, int idx, struct ut_txv_payload* payload) {
  const struct st_frame_trans* frame = &ctx->session.st20_frames[0];
  const struct rte_mbuf* chain = ctx->pipe_built[MTL_SESSION_PORT_P][idx]->next;

  payload->extbuf = RTE_MBUF_HAS_EXTBUF(chain) && chain->shinfo == &frame->sh_info;
  payload->copy = chain->pool == ctx->session.mbuf_mempool_copy_chain;
  payload->addr_off =
      (uintptr_t)rte_pktmbuf_mtod(chain, uint8_t*) - (uintptr_t)frame->addr;
  payload->iova_off = rte_pktmbuf_iova(chain) - frame->iova;
  payload->len = chain->data_len;
}

void ut_txv_pipe_free_built(ut_txv_ctx* ctx, int leg, int n) {
  for (int i = 0; i < n && ctx->pipe_built_freed[leg] < ctx->pipe_built_cnt[leg]; i++)
    rte_pktmbuf_free(ctx->pipe_built[leg][ctx->pipe_built_freed[leg]++]);
}

void ut_txv_pipe_free_last_built(ut_txv_ctx* ctx, int leg) {
  if (ctx->pipe_built_cnt[leg] <= ctx->pipe_built_freed[leg]) return;
  rte_pktmbuf_free(ctx->pipe_built[leg][--ctx->pipe_built_cnt[leg]]);
}

void ut_txv_pipe_set_flush_ret(ut_txv_ctx* ctx, int leg, int ret) {
  ctx->pipe_flush_ret[leg] = ret;
}

int ut_txv_pipe_stop_last_pkt_unsent(ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  for (int i = 0; i < s->ops.num_port; i++) {
    if (ctx->pipe_built_cnt[i] <= ctx->pipe_built_freed[i]) return -EINVAL;
    if (rte_ring_sp_enqueue(s->ring[i], ctx->pipe_built[i][ctx->pipe_built_cnt[i] - 1]))
      return -ENOSPC;
    ctx->pipe_built_cnt[i]--;
    /* the mark tv_uinit_hw() flushes the queue with */
    s->pad[i][ST20_PKT_TYPE_NORMAL] = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[i]);
    if (!s->pad[i][ST20_PKT_TYPE_NORMAL]) return -ENOMEM;
  }
  ut_txv_stop_ctx = ctx;
  tv_uinit(s);
  ut_txv_stop_ctx = NULL;
  return 0;
}

int ut_txv_recovery_set_last_pkt_unsent(ut_txv_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_frame_trans* frame = &ctx->recovery_frame;

  /* session-owned pools on iavf legs, as tv_attach() leaves a direct chain */
  s->tx_mono_pool = false;
  s->tx_no_chain = false;
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  s->bulk = 4;
  s->socket_id = (int)rte_socket_id();
  for (int i = 0; i < s->ops.num_port; i++) {
    ctx->impl.inf[i].drv_info.drv_type = MT_DRV_IAVF;
    ctx->impl.inf[i].nb_tx_desc = MT_DEV_TX_DESC;
  }
  s->tx_direct_chain = tv_tx_direct_chain(&ctx->impl, s);
  if (!s->tx_direct_chain) return -EINVAL;
  if (tv_mempool_init(&ctx->impl, &ctx->mgr, s) < 0) return -ENOMEM;
  ctx->recovery_owns_pools = true;
  struct rte_mbuf* chain = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
  if (!chain) return -ENOMEM;

  s->st20_fb_size = UT_TXV_FRAME_SIZE;
  frame->addr = ctx->recovery_data;
  frame->iova = (rte_iova_t)(uintptr_t)frame->addr;
  frame->priv = s;
  frame->sh_info.free_cb = tv_frame_free_cb;
  frame->sh_info.fcb_opaque = frame;
  rte_mbuf_ext_refcnt_set(&frame->sh_info, 1);
  rte_pktmbuf_attach_extbuf(chain, frame->addr, frame->iova, UT_TXV_FRAME_SIZE,
                            &frame->sh_info);
  chain->data_len = chain->pkt_len = UT_TXV_FRAME_SIZE;
  for (int i = 0; i < s->ops.num_port; i++) {
    struct rte_mbuf* hdr = rte_pktmbuf_alloc(s->mbuf_mempool_hdr[i]);
    if (!hdr) return -ENOMEM;
    /* the legs share the payload, as tv_build_st20_redundant_chain() does */
    if (i) rte_mbuf_refcnt_update(chain, 1);
    hdr->next = chain;
    hdr->nb_segs = 2;
    s->trs_inflight[i][0] = hdr;
    s->trs_inflight_idx[i] = 0;
    s->trs_inflight_num[i] = 1;
  }
  return 0;
}

int ut_txv_done_at_flush(const ut_txv_ctx* ctx, int leg) {
  return ctx->flush_seen[leg] ? ctx->done_at_flush[leg] : -1;
}

static int ut_txv_sch_quota_mbs;

static struct mtl_sch_impl* ut_txv_sch_get_by_socket(struct mtl_main_impl* impl,
                                                     int quota_mbs, enum mt_sch_type type,
                                                     mt_sch_mask_t mask, int socket) {
  (void)impl;
  (void)type;
  (void)mask;
  (void)socket;
  ut_txv_sch_quota_mbs = quota_mbs;
  return NULL;
}

/* framebuff_max_size of the quota checks' ST22 sessions: a 1080p 4:2:2 10-bit frame */
#define UT_TXV_ST22_QUOTA_FRAME_SIZE (1920 * 1080 * 5 / 2)
/* the RTP fields of the quota checks' RTP sessions: a 1080p frame in 1200-byte pkts */
#define UT_TXV_QUOTA_RTP_PKTS (4320)
#define UT_TXV_QUOTA_RTP_PKT_SIZE (1200)

static struct mtl_main_impl* ut_txv_quota_impl(bool user_quota) {
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  if (!impl) return NULL;
  impl->type = MT_HANDLE_MAIN;
  impl->user_para.num_ports = 1;
  snprintf(impl->user_para.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "ut_txv");
  if (user_quota) impl->user_para.data_quota_mbs_per_sch = 100000;
  return impl;
}

static int ut_txv_quota_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  (void)priv;
  (void)next_frame_idx;
  (void)meta;
  return -EIO;
}

static int ut_txv_quota_lines_ready(void* priv, uint16_t frame_idx,
                                    struct st20_tx_slice_meta* meta) {
  (void)priv;
  (void)frame_idx;
  (void)meta;
  return -EIO;
}

static int ut_txv_quota_st22_next_frame(void* priv, uint16_t* next_frame_idx,
                                        struct st22_tx_frame_meta* meta) {
  (void)priv;
  (void)next_frame_idx;
  (void)meta;
  return -EIO;
}

static int ut_txv_quota_rtp_done(void* priv) {
  (void)priv;
  return 0;
}

int ut_txv_st20_tx_quota_mbs(enum st20_type type, bool user_quota) {
  struct mtl_main_impl* impl = ut_txv_quota_impl(user_quota);
  if (!impl) return -ENOMEM;

  struct st20_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = "ut_txv_quota";
  ops.type = type;
  ops.num_port = 1;
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "ut_txv");
  ops.dip_addr[MTL_SESSION_PORT_P][0] = 239;
  ops.dip_addr[MTL_SESSION_PORT_P][3] = 1;
  ops.udp_port[MTL_SESSION_PORT_P] = 20000;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.payload_type = 112;
  ops.framebuff_cnt = 2;
  ops.get_next_frame = ut_txv_quota_next_frame;
  ops.query_frame_lines_ready = ut_txv_quota_lines_ready;
  ops.rtp_ring_size = 1024;
  ops.rtp_frame_total_pkts = UT_TXV_QUOTA_RTP_PKTS;
  ops.rtp_pkt_size = UT_TXV_QUOTA_RTP_PKT_SIZE;
  ops.notify_rtp_done = ut_txv_quota_rtp_done;

  ut_txv_sch_quota_mbs = -EINVAL;
  st20_tx_handle handle = st20_tx_create(impl, &ops);
  free(impl);
  return handle ? -EEXIST : ut_txv_sch_quota_mbs;
}

int ut_txv_st22_tx_quota_mbs(enum st22_type type, bool user_quota) {
  struct mtl_main_impl* impl = ut_txv_quota_impl(user_quota);
  if (!impl) return -ENOMEM;

  struct st22_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = "ut_txv_quota";
  ops.type = type;
  ops.num_port = 1;
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "ut_txv");
  ops.dip_addr[MTL_SESSION_PORT_P][0] = 239;
  ops.dip_addr[MTL_SESSION_PORT_P][3] = 1;
  ops.udp_port[MTL_SESSION_PORT_P] = 20000;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.payload_type = 112;
  ops.pack_type = ST22_PACK_CODESTREAM;
  ops.framebuff_cnt = 2;
  ops.framebuff_max_size = UT_TXV_ST22_QUOTA_FRAME_SIZE;
  ops.get_next_frame = ut_txv_quota_st22_next_frame;
  ops.rtp_ring_size = 1024;
  ops.rtp_frame_total_pkts = UT_TXV_QUOTA_RTP_PKTS;
  ops.rtp_pkt_size = UT_TXV_QUOTA_RTP_PKT_SIZE;
  ops.notify_rtp_done = ut_txv_quota_rtp_done;

  ut_txv_sch_quota_mbs = -EINVAL;
  st22_tx_handle handle = st22_tx_create(impl, &ops);
  free(impl);
  return handle ? -EEXIST : ut_txv_sch_quota_mbs;
}
