/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"
#include "st2110/st_tx_video_session.h"

#define UT_TRS_MAX_TSC_SCRIPT 64
#define UT_TRS_MAX_SEND_HISTORY 8

/* pads accepted on a port ahead of its first real packet */
struct ut_trs_train {
  long double drained_tsc; /* when the shaper finishes the pads */
  uint64_t real_tsc;       /* when the first real packet was queued */
  uint32_t pads;
  bool closed;
};

struct ut_trs_ctx {
  struct mtl_main_impl impl;
  struct st_tx_video_session_impl session;
  int queue_tag[MTL_SESSION_PORT_MAX]; /* s->queue[] identity for the burst mock */
  struct rte_mbuf* pad_mbuf;
  struct rte_mbuf* inflight_mbuf;
  struct rte_mbuf* inflight2_mbuf;
  struct rte_mbuf* redundant_inflight_mbuf;

  uint64_t tsc_script[UT_TRS_MAX_TSC_SCRIPT];
  int tsc_script_len;
  int tsc_script_pos;
  uint64_t last_tsc;
  uint64_t tsc_skew; /* added to the script, the stalls so far */
  uint32_t stall_call;
  uint64_t stall_ns;

  uint32_t pad_send_count;
  uint32_t real_send_count;
  uint32_t burst_call_count;
  uint64_t last_pad_send_tsc;
  uint64_t last_real_send_tsc;
  uint32_t send_history[UT_TRS_MAX_SEND_HISTORY];
  uint32_t send_history_count;
  struct ut_trs_train train[MTL_SESSION_PORT_MAX];
  bool burst_force_fail;
  uint16_t burst_accept_limit;
};

#include "session/st_video_transmitter_harness.h"

static uint64_t ut_trs_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_trs_ctx* ctx = (struct ut_trs_ctx*)impl; /* impl is ctx's first member */
  int len = ctx->tsc_script_len;
  int pos = ctx->tsc_script_pos++;
  if (!len) return ++ctx->last_tsc;
  if (pos < len) {
    ctx->last_tsc = ctx->tsc_script[pos] + ctx->tsc_skew;
  } else {
    /* Keep exhausted scripts monotonic. */
    uint64_t step = (len >= 2) ? ctx->tsc_script[len - 1] - ctx->tsc_script[len - 2] : 0;
    if (!step) step = 1;
    ctx->last_tsc = ctx->tsc_script[len - 1] + step * (pos - len + 1) + ctx->tsc_skew;
  }
  return ctx->last_tsc;
}

static uint64_t ut_trs_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  struct ut_trs_ctx* ctx = (struct ut_trs_ctx*)impl;
  MTL_MAY_UNUSED(port);
  return ctx->last_tsc;
}

/* mt_txq_burst() has no private context argument. */
static struct ut_trs_ctx* ut_trs_active_ctx;

static struct rte_mempool* ut_trs_priv_pool(void);

static uint16_t ut_trs_txq_burst_mock(struct mt_txq_entry* entry,
                                      struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  struct ut_trs_ctx* ctx = ut_trs_active_ctx;
  if (!ctx) return nb_pkts;
  ctx->burst_call_count++;
  if (ctx->stall_ns && ctx->burst_call_count == ctx->stall_call) {
    /* the doorbell rings stall_ns after the last tsc read */
    ctx->tsc_skew += ctx->stall_ns;
    ctx->last_tsc += ctx->stall_ns;
  }
  if (ctx->burst_force_fail) return 0;

  int port = (entry == (struct mt_txq_entry*)&ctx->queue_tag[MTL_SESSION_PORT_R])
                 ? MTL_SESSION_PORT_R
                 : MTL_SESSION_PORT_P;
  struct ut_trs_train* train = &ctx->train[port];
  if (ctx->burst_accept_limit && nb_pkts > ctx->burst_accept_limit)
    nb_pkts = ctx->burst_accept_limit;
  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    if (m->pool != ut_trs_priv_pool()) { /* pads come from ut_pool() */
      ctx->pad_send_count++;
      ctx->last_pad_send_tsc = ctx->last_tsc;
      if (!train->closed) {
        /* an idle shaper refills its bucket: at most rl_credit ahead of arrival */
        long double start = ctx->last_tsc - ctx->session.pacing.rl_credit[port];
        if (!train->pads || train->drained_tsc < start) train->drained_tsc = start;
        train->drained_tsc += ctx->session.pacing.rl_drain[port];
        train->pads++;
      }
      if (rte_mbuf_refcnt_read(m) > 1) rte_pktmbuf_free(m);
    } else {
      if (ctx->send_history_count < UT_TRS_MAX_SEND_HISTORY)
        ctx->send_history[ctx->send_history_count++] = st_tx_mbuf_get_idx(m);
      ctx->real_send_count++;
      ctx->last_real_send_tsc = ctx->last_tsc;
      if (!train->closed) train->real_tsc = ctx->last_tsc;
      train->closed = true;
    }
  }
  return nb_pkts;
}

/* st20_tx_harness.c compiles st_video_transmitter.c too, and UnitTest links both
 * objects, so the three external functions of the file would be defined twice.
 * Give this copy private names: this harness drives only the static tasklets. */
#define st_video_resolve_pacing_tasklet ut_trs_resolve_pacing_tasklet
#define st_video_transmitter_init ut_trs_transmitter_init
#define st_video_transmitter_uinit ut_trs_transmitter_uinit
#define mt_get_tsc ut_trs_tsc_time_fn
#define mt_get_ptp_time ut_trs_ptp_time_fn
#define mt_txq_burst ut_trs_txq_burst_mock
#include "st2110/st_video_transmitter.c"
#undef mt_get_tsc
#undef mt_get_ptp_time
#undef mt_txq_burst
#undef st_video_resolve_pacing_tasklet
#undef st_video_transmitter_init
#undef st_video_transmitter_uinit

int ut_trs_init(void) {
  return ut_eal_init();
}

static struct rte_mempool* ut_trs_priv_pool(void) {
  static struct rte_mempool* pool;
  if (!pool) {
    pool = rte_pktmbuf_pool_create("ut_trs_priv_pool", 64, 0,
                                   sizeof(struct mt_muf_priv_data),
                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  }
  return pool;
}

ut_trs_ctx* ut_trs_create(void) {
  struct rte_mempool* priv_pool;
  ut_trs_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;

  struct st_tx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->idx = 0;
  s->bulk = 1;
  s->tx_hang_detect_time_thresh = NS_PER_S;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++)
    s->queue[i] = (struct mt_txq_entry*)&ctx->queue_tag[i];

  ctx->pad_mbuf = rte_pktmbuf_alloc(ut_pool());
  if (!ctx->pad_mbuf) goto fail;
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] = ctx->pad_mbuf;
  s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL] = rte_pktmbuf_alloc(ut_pool());
  if (!s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]) goto fail;

  priv_pool = ut_trs_priv_pool();
  if (!priv_pool) goto fail;
  ctx->inflight_mbuf = rte_pktmbuf_alloc(priv_pool);
  if (!ctx->inflight_mbuf) goto fail;
  st_tx_mbuf_set_idx(ctx->inflight_mbuf, 1);
  ctx->inflight2_mbuf = rte_pktmbuf_alloc(priv_pool);
  if (!ctx->inflight2_mbuf) goto fail;
  st_tx_mbuf_set_idx(ctx->inflight2_mbuf, 1);
  ctx->redundant_inflight_mbuf = rte_pktmbuf_alloc(priv_pool);
  if (!ctx->redundant_inflight_mbuf) goto fail;
  st_tx_mbuf_set_idx(ctx->redundant_inflight_mbuf, 1);

  s->ring[MTL_SESSION_PORT_P] = ut_ring_create("ut_trs_ring", 4);
  if (!s->ring[MTL_SESSION_PORT_P]) goto fail;

  return ctx;

fail:
  if (s->ring[MTL_SESSION_PORT_P]) rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  if (ctx->redundant_inflight_mbuf) rte_pktmbuf_free(ctx->redundant_inflight_mbuf);
  if (ctx->inflight2_mbuf) rte_pktmbuf_free(ctx->inflight2_mbuf);
  if (ctx->inflight_mbuf) rte_pktmbuf_free(ctx->inflight_mbuf);
  if (s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL])
    rte_pktmbuf_free(s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]);
  if (ctx->pad_mbuf) rte_pktmbuf_free(ctx->pad_mbuf);
  free(ctx);
  return NULL;
}

void ut_trs_destroy(ut_trs_ctx* ctx) {
  if (!ctx) return;
  if (ut_trs_active_ctx == ctx) ut_trs_active_ctx = NULL;
  bool owns_inflight = ctx->session.trs_inflight_num[MTL_SESSION_PORT_P] > 0;
  bool owns_inflight2 = ctx->session.trs_inflight_num2[MTL_SESSION_PORT_P] > 0;
  bool owns_redundant = ctx->session.trs_inflight_num[MTL_SESSION_PORT_R] > 0;
  st_tx_video_transmitter_state_cleanup(&ctx->session);
  if (owns_inflight) ctx->inflight_mbuf = NULL;
  if (owns_inflight2) ctx->inflight2_mbuf = NULL;
  if (owns_redundant) ctx->redundant_inflight_mbuf = NULL;
  rte_pktmbuf_free(ctx->session.pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]);
  if (ctx->pad_mbuf) rte_pktmbuf_free(ctx->pad_mbuf);
  if (ctx->inflight_mbuf) rte_pktmbuf_free(ctx->inflight_mbuf);
  if (ctx->inflight2_mbuf) rte_pktmbuf_free(ctx->inflight2_mbuf);
  if (ctx->redundant_inflight_mbuf) rte_pktmbuf_free(ctx->redundant_inflight_mbuf);
  if (ctx->session.ring[MTL_SESSION_PORT_P])
    rte_ring_free(ctx->session.ring[MTL_SESSION_PORT_P]);
  free(ctx);
}

void ut_trs_set_trs(ut_trs_ctx* ctx, long double trs_ns) {
  ctx->session.pacing.trs = trs_ns;
}

void ut_trs_set_warm_pkts_cap(ut_trs_ctx* ctx, uint32_t warm_pkts_cap) {
  ctx->session.pacing.warm_pkts = warm_pkts_cap;
}

void ut_trs_set_target_tsc(ut_trs_ctx* ctx, uint64_t target_tsc) {
  ctx->session.trs_target_tsc[MTL_SESSION_PORT_P] = target_tsc;
}

void ut_trs_set_target_tsc_port(ut_trs_ctx* ctx, int port, uint64_t target_tsc) {
  ctx->session.trs_target_tsc[port] = target_tsc;
}

void ut_trs_set_drain(ut_trs_ctx* ctx, int port, long double drain_ns) {
  ctx->session.pacing.rl_drain[port] = drain_ns;
}

void ut_trs_set_credit(ut_trs_ctx* ctx, int port, long double credit_ns) {
  ctx->session.pacing.rl_credit[port] = credit_ns;
}

void ut_trs_set_mock_tsc_script(ut_trs_ctx* ctx, const uint64_t* values, int count) {
  if (count > UT_TRS_MAX_TSC_SCRIPT) count = UT_TRS_MAX_TSC_SCRIPT;
  memcpy(ctx->tsc_script, values, count * sizeof(*values));
  ctx->tsc_script_len = count;
  ctx->tsc_script_pos = 0;
  ctx->tsc_skew = 0;
}

void ut_trs_warm_up(ut_trs_ctx* ctx) {
  ut_trs_active_ctx = ctx;
  video_trs_rl_warm_up(&ctx->impl, &ctx->session, MTL_SESSION_PORT_P);
  ut_trs_active_ctx = NULL;
}

void ut_trs_pre_arm(ut_trs_ctx* ctx) {
  ut_trs_pre_arm_port(ctx, MTL_SESSION_PORT_P);
}

void ut_trs_pre_arm_port(ut_trs_ctx* ctx, int port) {
  ut_trs_active_ctx = ctx;
  video_trs_rl_pre_arm(&ctx->impl, &ctx->session, port);
  ut_trs_active_ctx = NULL;
}

long double ut_trs_modeled_launch_tsc(const ut_trs_ctx* ctx, int port) {
  const struct ut_trs_train* train = &ctx->train[port];
  /* no earlier than packet 0 is queued, at best after the last tsc read */
  uint64_t queued = train->closed ? train->real_tsc : ctx->last_tsc;

  return RTE_MAX(train->drained_tsc, (long double)queued);
}

uint32_t ut_trs_train_pads(const ut_trs_ctx* ctx, int port) {
  return ctx->train[port].pads;
}

void ut_trs_clear_train(ut_trs_ctx* ctx) {
  memset(ctx->train, 0, sizeof(ctx->train));
}

uint32_t ut_trs_pad_send_count(const ut_trs_ctx* ctx) {
  return ctx->pad_send_count;
}

uint64_t ut_trs_last_tsc(const ut_trs_ctx* ctx) {
  return ctx->last_tsc;
}

uint64_t ut_trs_get_mock_tsc(ut_trs_ctx* ctx) {
  return ut_trs_tsc_time_fn(&ctx->impl);
}

uint64_t ut_trs_stat_troffset_mismatch(const ut_trs_ctx* ctx) {
  return ctx->session.port_user_stats.stat_trans_troffset_mismatch;
}

uint64_t ut_trs_stat_recalculate_warmup(const ut_trs_ctx* ctx) {
  return ctx->session.port_user_stats.stat_trans_recalculate_warmup;
}

void ut_trs_set_burst_force_fail(ut_trs_ctx* ctx, bool fail) {
  ctx->burst_force_fail = fail;
}

void ut_trs_set_burst_accept_limit(ut_trs_ctx* ctx, uint16_t limit) {
  ctx->burst_accept_limit = limit;
}

void ut_trs_set_burst_stall(ut_trs_ctx* ctx, uint32_t call, uint64_t stall_ns) {
  ctx->stall_call = call;
  ctx->stall_ns = stall_ns;
}

uint32_t ut_trs_burst_call_count(const ut_trs_ctx* ctx) {
  return ctx->burst_call_count;
}

uint32_t ut_trs_real_send_count(const ut_trs_ctx* ctx) {
  return ctx->real_send_count;
}

uint32_t ut_trs_sent_pkt_idx(const ut_trs_ctx* ctx, uint32_t pos) {
  if (pos >= ctx->send_history_count) return UINT32_MAX;
  return ctx->send_history[pos];
}

uint64_t ut_trs_last_pad_send_tsc(const ut_trs_ctx* ctx) {
  return ctx->last_pad_send_tsc;
}

uint64_t ut_trs_last_real_send_tsc(const ut_trs_ctx* ctx) {
  return ctx->last_real_send_tsc;
}

uint16_t ut_trs_pad_refcnt(const ut_trs_ctx* ctx) {
  return rte_mbuf_refcnt_read(ctx->pad_mbuf);
}

unsigned int ut_trs_pad_inflight_num(const ut_trs_ctx* ctx) {
  return ctx->session.trs_pad_inflight_num[MTL_SESSION_PORT_P];
}

unsigned int ut_trs_inflight_num(const ut_trs_ctx* ctx) {
  return ctx->session.trs_inflight_num[MTL_SESSION_PORT_P];
}

uint64_t ut_trs_target_tsc(const ut_trs_ctx* ctx) {
  return ctx->session.trs_target_tsc[MTL_SESSION_PORT_P];
}

int ut_trs_rl_state(const ut_trs_ctx* ctx) {
  return ctx->session.rl_state[MTL_SESSION_PORT_P];
}

bool ut_trs_recovery_pending(const ut_trs_ctx* ctx) {
  return ctx->session.tx_queue_recovery_pending[MTL_SESSION_PORT_P];
}

int ut_trs_rl_state_port(const ut_trs_ctx* ctx, int port) {
  return ctx->session.rl_state[port];
}

void ut_trs_set_rl_state_port(ut_trs_ctx* ctx, int port, int state) {
  ctx->session.rl_state[port] = state;
}

bool ut_trs_recovery_pending_port(const ut_trs_ctx* ctx, int port) {
  return ctx->session.tx_queue_recovery_pending[port];
}

void ut_trs_set_recovery_pending_port(ut_trs_ctx* ctx, int port, bool pending) {
  ctx->session.tx_queue_recovery_pending[port] = pending;
}

bool ut_trs_process_recovery_cleanup(ut_trs_ctx* ctx, int port) {
  if (!ctx->session.tx_queue_recovery_pending[port]) return false;

  ctx->session.tx_queue_recovery_pending[port] = false;
  ut_trs_call_recovery_cleanup(ctx);
  return true;
}

void ut_trs_call_recovery_cleanup(ut_trs_ctx* ctx) {
  bool owns_inflight = ctx->session.trs_inflight_num[MTL_SESSION_PORT_P] > 0;
  bool owns_inflight2 = ctx->session.trs_inflight_num2[MTL_SESSION_PORT_P] > 0;
  bool owns_redundant = ctx->session.trs_inflight_num[MTL_SESSION_PORT_R] > 0;

  st_tx_video_transmitter_state_cleanup(&ctx->session);
  if (owns_inflight) ctx->inflight_mbuf = NULL;
  if (owns_inflight2) ctx->inflight2_mbuf = NULL;
  if (owns_redundant) ctx->redundant_inflight_mbuf = NULL;
}

void ut_trs_set_pad_inflight_num(ut_trs_ctx* ctx, unsigned int n) {
  for (unsigned int i = 0; i < n; i++) rte_mbuf_refcnt_update(ctx->pad_mbuf, 1);
  ctx->session.trs_pad_inflight_num[MTL_SESSION_PORT_P] = n;
}

void ut_trs_set_inflight_num(ut_trs_ctx* ctx, unsigned int n) {
  struct st_tx_video_session_impl* s = &ctx->session;
  s->trs_inflight_num[MTL_SESSION_PORT_P] = n;
  s->trs_inflight_idx[MTL_SESSION_PORT_P] = 0;
  for (unsigned int i = 0; i < n; i++)
    s->trs_inflight[MTL_SESSION_PORT_P][i] = ctx->inflight_mbuf;
}

void ut_trs_set_inflight_num2(ut_trs_ctx* ctx, unsigned int n) {
  struct st_tx_video_session_impl* s = &ctx->session;
  s->trs_inflight_num2[MTL_SESSION_PORT_P] = n;
  s->trs_inflight_idx2[MTL_SESSION_PORT_P] = 0;
  for (unsigned int i = 0; i < n; i++)
    s->trs_inflight2[MTL_SESSION_PORT_P][i] = ctx->inflight_mbuf;
}

static bool ut_trs_alloc_cleanup_mbufs(ut_trs_ctx* ctx) {
  struct rte_mempool* pool = ut_trs_priv_pool();

  if (!ctx->inflight_mbuf) {
    ctx->inflight_mbuf = rte_pktmbuf_alloc(pool);
    if (!ctx->inflight_mbuf) return false;
    st_tx_mbuf_set_idx(ctx->inflight_mbuf, 1);
  }
  if (!ctx->inflight2_mbuf) {
    ctx->inflight2_mbuf = rte_pktmbuf_alloc(pool);
    if (!ctx->inflight2_mbuf) return false;
    st_tx_mbuf_set_idx(ctx->inflight2_mbuf, 1);
  }
  if (!ctx->redundant_inflight_mbuf) {
    ctx->redundant_inflight_mbuf = rte_pktmbuf_alloc(pool);
    if (!ctx->redundant_inflight_mbuf) return false;
    st_tx_mbuf_set_idx(ctx->redundant_inflight_mbuf, 1);
  }
  return true;
}

bool ut_trs_prepare_cleanup_state(ut_trs_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (!ut_trs_alloc_cleanup_mbufs(ctx)) return false;

  s->trs_inflight[MTL_SESSION_PORT_P][0] = ctx->inflight_mbuf;
  s->trs_inflight_num[MTL_SESSION_PORT_P] = 1;
  s->trs_inflight_idx[MTL_SESSION_PORT_P] = 0;
  s->trs_inflight2[MTL_SESSION_PORT_P][0] = ctx->inflight2_mbuf;
  s->trs_inflight_num2[MTL_SESSION_PORT_P] = 1;
  s->trs_inflight_idx2[MTL_SESSION_PORT_P] = 0;
  rte_mbuf_refcnt_update(ctx->pad_mbuf, 2);
  s->trs_pad_inflight_num[MTL_SESSION_PORT_P] = 2;
  s->trs_target_tsc[MTL_SESSION_PORT_P] = 10000;
  s->rl_state[MTL_SESSION_PORT_P] = ST_TX_VIDEO_RL_STATE_WAIT_TARGET;
  return true;
}

bool ut_trs_prepare_redundant_cleanup_state(ut_trs_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  if (!ut_trs_prepare_cleanup_state(ctx)) return false;
  s->trs_inflight[MTL_SESSION_PORT_R][0] = ctx->redundant_inflight_mbuf;
  s->trs_inflight_num[MTL_SESSION_PORT_R] = 1;
  s->trs_inflight_idx[MTL_SESSION_PORT_R] = 0;
  s->rl_state[MTL_SESSION_PORT_R] = ST_TX_VIDEO_RL_STATE_WAIT_TARGET;
  s->tx_queue_recovery_pending[MTL_SESSION_PORT_R] = true;
  return true;
}

void ut_trs_cleanup_state(ut_trs_ctx* ctx) {
  bool owns_inflight = ctx->session.trs_inflight_num[MTL_SESSION_PORT_P] > 0;
  bool owns_inflight2 = ctx->session.trs_inflight_num2[MTL_SESSION_PORT_P] > 0;

  st_tx_video_transmitter_state_cleanup(&ctx->session);
  if (owns_inflight) ctx->inflight_mbuf = NULL;
  if (owns_inflight2) ctx->inflight2_mbuf = NULL;
}

unsigned int ut_trs_inflight_num2(const ut_trs_ctx* ctx) {
  return ctx->session.trs_inflight_num2[MTL_SESSION_PORT_P];
}

unsigned int ut_trs_inflight_idx(const ut_trs_ctx* ctx) {
  return ctx->session.trs_inflight_idx[MTL_SESSION_PORT_P];
}

unsigned int ut_trs_inflight_idx2(const ut_trs_ctx* ctx) {
  return ctx->session.trs_inflight_idx2[MTL_SESSION_PORT_P];
}

unsigned int ut_trs_priv_pool_avail(void) {
  return rte_mempool_avail_count(ut_trs_priv_pool());
}

void ut_trs_set_last_burst_succ_tsc(ut_trs_ctx* ctx, uint64_t tsc) {
  ctx->session.last_burst_succ_time_tsc[MTL_SESSION_PORT_P] = tsc;
}

uint64_t ut_trs_get_last_burst_succ_tsc(const ut_trs_ctx* ctx) {
  return ctx->session.last_burst_succ_time_tsc[MTL_SESSION_PORT_P];
}

void ut_trs_set_hang_detect_thresh_ns(ut_trs_ctx* ctx, uint64_t thresh_ns) {
  ctx->session.tx_hang_detect_time_thresh = thresh_ns;
}

int ut_trs_get_stat_trs_ret_code(const ut_trs_ctx* ctx) {
  return ctx->session.stat_trs_ret_code[MTL_SESSION_PORT_P];
}

int ut_trs_get_stat_pkts_burst(const ut_trs_ctx* ctx) {
  return ctx->session.stat_pkts_burst;
}

uint16_t ut_trs_call_burst_pad(ut_trs_ctx* ctx) {
  struct st_tx_video_session_impl* s = &ctx->session;

  ut_trs_active_ctx = ctx;
  rte_mbuf_refcnt_update(s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL], 1);
  uint16_t tx = video_trs_burst_pad(&ctx->impl, s, MTL_SESSION_PORT_P,
                                    &s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL], 1);
  if (tx < 1) s->trs_pad_inflight_num[MTL_SESSION_PORT_P]++;
  ut_trs_active_ctx = NULL;
  return tx;
}

int ut_trs_call_rl_tasklet(ut_trs_ctx* ctx) {
  ut_trs_active_ctx = ctx;
  int ret = video_trs_rl_tasklet(&ctx->impl, &ctx->session, MTL_SESSION_PORT_P);
  ut_trs_active_ctx = NULL;
  return ret;
}

void ut_trs_enqueue_ring_pkt(ut_trs_ctx* ctx) {
  void* obj = ctx->inflight_mbuf;
  rte_ring_sp_enqueue_bulk(ctx->session.ring[MTL_SESSION_PORT_P], &obj, 1, NULL);
}

void ut_trs_enqueue_first_pkt(ut_trs_ctx* ctx, uint64_t target_tsc) {
  st_tx_mbuf_set_idx(ctx->inflight_mbuf, 0);
  st_tx_mbuf_set_tsc(ctx->inflight_mbuf, target_tsc);
  ut_trs_enqueue_ring_pkt(ctx);
}

void ut_trs_enqueue_frame_boundary(ut_trs_ctx* ctx, uint64_t target_tsc) {
  struct st_tx_video_session_impl* s = &ctx->session;
  void* pkts[2] = {ctx->inflight_mbuf, ctx->inflight2_mbuf};

  s->bulk = 2;
  st_tx_mbuf_set_idx(ctx->inflight_mbuf, 1);
  st_tx_mbuf_set_idx(ctx->inflight2_mbuf, 0);
  st_tx_mbuf_set_tsc(ctx->inflight2_mbuf, target_tsc);
  rte_ring_sp_enqueue_bulk(s->ring[MTL_SESSION_PORT_P], pkts, 2, NULL);
}
