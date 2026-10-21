/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "dev/mt_dev.h"
#include "mt_main.h"
#include "st2110/st_tx_video_session.h"

#define UT_TRS_MAX_TSC_SCRIPT 64
#define UT_TRS_NIC_FIFO 8192
#define UT_TRS_MAX_SEND_HISTORY 8
#define UT_TRS_PAD_SIZE 1262 /* the NORMAL pad of 1080p59 GPM */
#define UT_TRS_MAX_BULK 16
#define UT_TRS_MAX_FRAMES 4
#define UT_TRS_WIRE_MAX 16384
#define UT_TRS_SENT_MAX 128
#define UT_TRS_FRAME_TRS 1000 /* ns */
#define UT_TRS_FRAME_STEPS 1000000

/* pads accepted on a port ahead of its first real packet */
struct ut_trs_train {
  long double drained_tsc; /* when the shaper finishes the pads */
  long double full_tsc;    /* when it last started them from a full bucket */
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
  bool stall_after_doorbell;

  uint32_t pad_send_count;
  uint32_t real_send_count;
  uint32_t burst_call_count;
  uint64_t last_pad_send_tsc;
  uint64_t last_real_send_tsc;
  uint32_t send_history[UT_TRS_MAX_SEND_HISTORY];
  uint32_t send_history_count;
  struct ut_trs_train train[MTL_SESSION_PORT_MAX];
  long double shaper_bucket_ns[MTL_SESSION_PORT_MAX]; /* 0: the plan's bucket_ns */
  long double timeslot_ns[MTL_SESSION_PORT_MAX];      /* 0: a fluid shaper */
  long double timeslot_phase_ns[MTL_SESSION_PORT_MAX];
  bool burst_force_fail;
  uint16_t burst_accept_limit;
  uint32_t refcnt_since_tsc;
  uint32_t max_refcnt_before_burst;

  /* stream mode: a NIC TX queue of nic_desc descriptors, each pkt takes nb_segs of
   * them until the shaper finishes it, one pkt per trs */
  uint16_t nic_desc;
  uint32_t nic_used;
  long double nic_done[UT_TRS_NIC_FIFO]; /* when each queued pkt leaves the wire */
  uint16_t nic_segs[UT_TRS_NIC_FIFO];
  uint32_t nic_head, nic_tail;
  long double nic_last_done;
  long double nic_first_done;
  uint64_t nic_pkts;
  long double nic_max_late;
  uint16_t stream_segs;
  uint32_t stream_idx;
  struct rte_mempool* stream_pool;

  /* frames mode: the pkts of ut_trs_run_frames(), freed once a burst returned, and what
   * every burst took, in order */
  struct rte_mempool* frame_pool;
  struct rte_mbuf* sent[UT_TRS_SENT_MAX];
  uint32_t sent_count;
  uint64_t frame_target[UT_TRS_MAX_FRAMES];
  int feed_frame;
  int feed_next; /* < 0: the lead pkts before frame 0 */
  uint32_t burst_fail_every;
  uint32_t wire[UT_TRS_WIRE_MAX];
  uint64_t wire_tsc[UT_TRS_WIRE_MAX];
  uint32_t wire_count;
};

#include "session/st_video_transmitter_harness.h"

static uint64_t ut_trs_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_trs_ctx* ctx = (struct ut_trs_ctx*)impl; /* impl is ctx's first member */
  int len = ctx->tsc_script_len;
  int pos = ctx->tsc_script_pos++;
  ctx->refcnt_since_tsc = 0;
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

static void ut_trs_burst_stall(struct ut_trs_ctx* ctx, bool after_doorbell) {
  if (!ctx->stall_ns || ctx->burst_call_count != ctx->stall_call) return;
  if (ctx->stall_after_doorbell != after_doorbell) return;
  ctx->tsc_skew += ctx->stall_ns;
  ctx->last_tsc += ctx->stall_ns;
}

static uint16_t ut_trs_nic_burst(struct ut_trs_ctx* ctx, struct rte_mbuf** tx_pkts,
                                 uint16_t nb_pkts) {
  long double now = ctx->last_tsc;
  long double trs = ctx->session.pacing.trs;
  uint16_t tx;

  while (ctx->nic_head != ctx->nic_tail &&
         ctx->nic_done[ctx->nic_head % UT_TRS_NIC_FIFO] <= now) {
    ctx->nic_used -= ctx->nic_segs[ctx->nic_head % UT_TRS_NIC_FIFO];
    ctx->nic_head++;
  }
  for (tx = 0; tx < nb_pkts; tx++) {
    struct rte_mbuf* m = tx_pkts[tx];
    /* stream pkts are single mbufs standing for chained ones */
    uint16_t segs = m->pool == ctx->stream_pool ? ctx->stream_segs : m->nb_segs;
    if (ctx->nic_used + segs > ctx->nic_desc) break;
    long double done = RTE_MAX(ctx->nic_last_done, now) + trs;
    if (!ctx->nic_pkts) ctx->nic_first_done = done;
    /* the slot of the nic_pkts-th pkt of a stream that never pauses */
    long double late = done - (ctx->nic_first_done + ctx->nic_pkts * trs);
    ctx->nic_max_late = RTE_MAX(ctx->nic_max_late, late);
    ctx->nic_last_done = done;
    ctx->nic_pkts++;
    ctx->nic_done[ctx->nic_tail % UT_TRS_NIC_FIFO] = done;
    ctx->nic_segs[ctx->nic_tail % UT_TRS_NIC_FIFO] = segs;
    ctx->nic_tail++;
    ctx->nic_used += segs;
    rte_pktmbuf_free(m);
  }
  return tx;
}

static uint16_t ut_trs_txq_burst_mock(struct mt_txq_entry* entry,
                                      struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  struct ut_trs_ctx* ctx = ut_trs_active_ctx;
  if (!ctx) return nb_pkts;
  ctx->burst_call_count++;
  ctx->max_refcnt_before_burst =
      RTE_MAX(ctx->max_refcnt_before_burst, ctx->refcnt_since_tsc);
  if (ctx->nic_desc) return ut_trs_nic_burst(ctx, tx_pkts, nb_pkts);
  ut_trs_burst_stall(ctx, false);
  if (ctx->burst_force_fail) return 0;
  if (ctx->burst_fail_every && !(ctx->burst_call_count % ctx->burst_fail_every)) return 0;

  int port = (entry == (struct mt_txq_entry*)&ctx->queue_tag[MTL_SESSION_PORT_R])
                 ? MTL_SESSION_PORT_R
                 : MTL_SESSION_PORT_P;
  struct ut_trs_train* train = &ctx->train[port];
  if (ctx->burst_accept_limit && nb_pkts > ctx->burst_accept_limit)
    nb_pkts = ctx->burst_accept_limit;
  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    bool frame_pkt = ctx->frame_pool && m->pool == ctx->frame_pool;
    bool pad = m->pool != ut_trs_priv_pool() && !frame_pkt; /* pads come from ut_pool() */
    if (ctx->wire_count < UT_TRS_WIRE_MAX) {
      ctx->wire[ctx->wire_count] = pad ? UT_TRS_WIRE_PAD : st_tx_mbuf_get_idx(m);
      ctx->wire_tsc[ctx->wire_count++] = ctx->last_tsc;
    }
    if (frame_pkt && ctx->sent_count < UT_TRS_SENT_MAX) ctx->sent[ctx->sent_count++] = m;
    if (pad) {
      ctx->pad_send_count++;
      ctx->last_pad_send_tsc = ctx->last_tsc;
      if (!train->closed) {
        /* an idle shaper refills its bucket: at most bucket_ns ahead of arrival */
        long double bucket = ctx->shaper_bucket_ns[port]
                                 ? ctx->shaper_bucket_ns[port]
                                 : ctx->session.rl_plan[port].bucket_ns;
        long double start = ctx->last_tsc - bucket;
        if (!train->pads || train->drained_tsc < start) {
          train->drained_tsc = start;
          train->full_tsc = ctx->last_tsc;
        }
        /* a pad drains by its bytes */
        train->drained_tsc += ctx->session.rl_plan[port].pad_ns * m->pkt_len /
                              ctx->session.st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
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
  ut_trs_burst_stall(ctx, true);
  return nb_pkts;
}

static uint16_t ut_trs_refcnt_update_mock(struct rte_mbuf* m, int16_t value) {
  if (ut_trs_active_ctx) ut_trs_active_ctx->refcnt_since_tsc++;
  return rte_mbuf_refcnt_update(m, value);
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
#define rte_mbuf_refcnt_update ut_trs_refcnt_update_mock
#include "st2110/st_video_transmitter.c"
#undef rte_mbuf_refcnt_update
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

static struct rte_mbuf* ut_trs_pad(uint16_t len) {
  struct rte_mbuf* pad = rte_pktmbuf_alloc(ut_pool());
  if (pad && !rte_pktmbuf_append(pad, len)) {
    rte_pktmbuf_free(pad);
    return NULL;
  }
  return pad;
}

static void ut_trs_free_trim_pads(struct st_tx_video_session_impl* s) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX; j++) {
      rte_pktmbuf_free(s->trim_pad[i][j]);
      s->trim_pad[i][j] = NULL;
    }
  }
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

  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = UT_TRS_PAD_SIZE;
  ctx->pad_mbuf = ut_trs_pad(UT_TRS_PAD_SIZE);
  if (!ctx->pad_mbuf) goto fail;
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] = ctx->pad_mbuf;
  s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL] = ut_trs_pad(UT_TRS_PAD_SIZE);
  if (!s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]) goto fail;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct rte_mbuf** trim_pad = s->trim_pad[i];

    trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_COARSE] =
        ut_trs_pad(UT_TRS_PAD_SIZE - ST_TX_VIDEO_RL_TRIM_COARSE);
    trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_FINE] =
        ut_trs_pad(UT_TRS_PAD_SIZE - ST_TX_VIDEO_RL_TRIM_FINE);
    if (!trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_COARSE] ||
        !trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_FINE])
      goto fail;
  }

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
  ut_trs_free_trim_pads(s);
  if (s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL])
    rte_pktmbuf_free(s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]);
  if (ctx->pad_mbuf) rte_pktmbuf_free(ctx->pad_mbuf);
  free(ctx);
  return NULL;
}

void ut_trs_destroy(ut_trs_ctx* ctx) {
  if (!ctx) return;
  if (ut_trs_active_ctx == ctx) ut_trs_active_ctx = NULL;
  /* stream and frames modes leave their own pkts in inflight, never the ctx mbufs */
  bool stream = ctx->stream_pool || ctx->frame_pool;
  bool owns_inflight = !stream && ctx->session.trs_inflight_num[MTL_SESSION_PORT_P] > 0;
  bool owns_inflight2 = !stream && ctx->session.trs_inflight_num2[MTL_SESSION_PORT_P] > 0;
  bool owns_redundant = !stream && ctx->session.trs_inflight_num[MTL_SESSION_PORT_R] > 0;
  st_tx_video_transmitter_state_cleanup(&ctx->session);
  if (owns_inflight) ctx->inflight_mbuf = NULL;
  if (owns_inflight2) ctx->inflight2_mbuf = NULL;
  if (owns_redundant) ctx->redundant_inflight_mbuf = NULL;
  rte_pktmbuf_free(ctx->session.pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL]);
  ut_trs_free_trim_pads(&ctx->session);
  if (ctx->pad_mbuf) rte_pktmbuf_free(ctx->pad_mbuf);
  if (ctx->inflight_mbuf) rte_pktmbuf_free(ctx->inflight_mbuf);
  if (ctx->inflight2_mbuf) rte_pktmbuf_free(ctx->inflight2_mbuf);
  if (ctx->redundant_inflight_mbuf) rte_pktmbuf_free(ctx->redundant_inflight_mbuf);
  if (ctx->session.ring[MTL_SESSION_PORT_P]) {
    /* only stream and frames modes leave pkts of their own in the ring */
    if (stream) mt_ring_dequeue_clean(ctx->session.ring[MTL_SESSION_PORT_P]);
    rte_ring_free(ctx->session.ring[MTL_SESSION_PORT_P]);
  }
  if (ctx->stream_pool) rte_mempool_free(ctx->stream_pool);
  if (ctx->frame_pool) {
    rte_pktmbuf_free_bulk(ctx->sent, ctx->sent_count);
    rte_mempool_free(ctx->frame_pool);
  }
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
  struct st_tx_video_rl_plan* plan = &ctx->session.rl_plan[port];

  plan->pad_ns = drain_ns;
  plan->pad_size = ctx->session.st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
  plan->trim_pads = st_tx_video_rl_trim_pads(plan->pad_size);
  plan->armed = drain_ns != 0;
}

void ut_trs_set_credit(ut_trs_ctx* ctx, int port, long double credit_ns) {
  ctx->session.rl_plan[port].bucket_ns = credit_ns;
}

void ut_trs_set_shaper_bucket(ut_trs_ctx* ctx, int port, long double bucket_ns) {
  ctx->shaper_bucket_ns[port] = bucket_ns;
}

void ut_trs_set_shaper_timeslot(ut_trs_ctx* ctx, int port, long double slot_ns,
                                long double phase_ns) {
  ctx->timeslot_ns[port] = slot_ns;
  ctx->timeslot_phase_ns[port] = phase_ns;
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

bool ut_trs_pre_arm(ut_trs_ctx* ctx) {
  return ut_trs_pre_arm_port(ctx, MTL_SESSION_PORT_P);
}

bool ut_trs_pre_arm_port(ut_trs_ctx* ctx, int port) {
  ut_trs_active_ctx = ctx;
  bool armed = video_trs_rl_pre_arm(&ctx->impl, &ctx->session, port);
  ut_trs_active_ctx = NULL;
  return armed;
}

long double ut_trs_modeled_launch_tsc(const ut_trs_ctx* ctx, int port) {
  const struct ut_trs_train* train = &ctx->train[port];
  /* no earlier than packet 0 is queued, at best after the last tsc read */
  uint64_t queued = train->closed ? train->real_tsc : ctx->last_tsc;
  long double launch = train->drained_tsc;
  long double slot = ctx->timeslot_ns[port];

  /* the first tick whose tokens cover the pads past the bucket, ticks a slot apart from
   * phase_ns before the full start */
  if (slot && train->pads)
    launch = train->full_tsc - ctx->timeslot_phase_ns[port] +
             ceill((train->drained_tsc - train->full_tsc) / slot) * slot;
  return RTE_MAX(launch, (long double)queued);
}

long double ut_trs_fine_trim_ns(const ut_trs_ctx* ctx, int port) {
  return ctx->session.rl_plan[port].pad_ns * ST_TX_VIDEO_RL_TRIM_FINE / UT_TRS_PAD_SIZE;
}

uint16_t ut_trs_trim_pad_refcnt(const ut_trs_ctx* ctx, int j) {
  return rte_mbuf_refcnt_read(ctx->session.trim_pad[MTL_SESSION_PORT_P][j]);
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

void ut_trs_set_burst_stall(ut_trs_ctx* ctx, uint32_t call, uint64_t stall_ns,
                            bool after_doorbell) {
  ctx->stall_call = call;
  ctx->stall_ns = stall_ns;
  ctx->stall_after_doorbell = after_doorbell;
}

uint32_t ut_trs_burst_call_count(const ut_trs_ctx* ctx) {
  return ctx->burst_call_count;
}

uint32_t ut_trs_max_refcnt_updates_before_burst(const ut_trs_ctx* ctx) {
  return ctx->max_refcnt_before_burst;
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

uint16_t ut_trs_deep_nb_tx_desc(void) {
  return MT_DEV_TX_DESC_DEEP;
}

int ut_trs_setup_stream(ut_trs_ctx* ctx, long double trs_ns, uint16_t nic_desc,
                        uint16_t segs_per_pkt) {
  static unsigned int pool_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char name[RTE_MEMPOOL_NAMESIZE];

  (void)snprintf(name, sizeof(name), "ut_trs_stream_%u", pool_idx);
  ctx->stream_pool =
      rte_pktmbuf_pool_create(name, 2048, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
  (void)snprintf(name, sizeof(name), "ut_trs_stream_ring_%u", pool_idx++);
  struct rte_ring* ring = ut_ring_create(name, 1024);
  if (!ctx->stream_pool || !ring) return -ENOMEM;
  rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  s->ring[MTL_SESSION_PORT_P] = ring;

  s->bulk = ST_SESSION_MAX_BULK;
  s->pacing.trs = trs_ns;
  /* no in-frame pads, < 1 % of the wire */
  s->pacing.pad_interval = 1e9f;
  ctx->nic_desc = nic_desc;
  ctx->stream_segs = segs_per_pkt;
  ctx->stream_idx = 1;
  ctx->tsc_script_len = 0;
  return 0;
}

void ut_trs_stream(ut_trs_ctx* ctx, uint64_t duration_ns, uint64_t call_ns) {
  struct rte_ring* ring = ctx->session.ring[MTL_SESSION_PORT_P];
  uint64_t end = ctx->last_tsc + duration_ns;

  ut_trs_active_ctx = ctx;
  while (ctx->last_tsc < end) {
    /* the builder keeps the ring full of mid-frame pkts */
    while (rte_ring_free_count(ring)) {
      struct rte_mbuf* m = rte_pktmbuf_alloc(ctx->stream_pool);
      if (!m) break;
      st_tx_mbuf_set_idx(m, ctx->stream_idx++);
      rte_ring_sp_enqueue(ring, m);
    }
    ctx->last_tsc += call_ns;
    video_trs_rl_tasklet(&ctx->impl, &ctx->session, MTL_SESSION_PORT_P);
  }
  ut_trs_active_ctx = NULL;
}

void ut_trs_stall_stream(ut_trs_ctx* ctx, uint64_t stall_ns) {
  ctx->last_tsc += stall_ns;
}

long double ut_trs_stream_max_late_ns(const ut_trs_ctx* ctx) {
  return ctx->nic_max_late;
}

uint32_t ut_trs_stream_nic_desc_used(const ut_trs_ctx* ctx) {
  return ctx->nic_used;
}

/* the next bulk, as tv_tasklet_frame() builds it: dummies past a frame's end, a target
 * on pkt 0 only */
static int ut_trs_feed_bulk(ut_trs_ctx* ctx, const struct ut_trs_frames_case* c) {
  void* pkts[UT_TRS_MAX_BULK];

  if (rte_pktmbuf_alloc_bulk(ctx->frame_pool, (struct rte_mbuf**)pkts, c->bulk) < 0)
    return -ENOMEM;
  for (unsigned int i = 0; i < c->bulk; i++) {
    int idx = ctx->feed_next < 0 ? ctx->feed_next + c->total_pkts : ctx->feed_next;
    st_tx_mbuf_set_priv(pkts[i], NULL);
    st_tx_mbuf_set_idx(pkts[i],
                       idx < c->total_pkts ? (uint32_t)idx : ST_TX_DUMMY_PKT_IDX);
    if (!ctx->feed_next) {
      int f = ctx->feed_frame;
      ctx->frame_target[f] = c->hold_ns ? ctx->last_tsc + c->hold_ns : 1;
      st_tx_mbuf_set_tsc(pkts[i], ctx->frame_target[f]);
    }
    if (ctx->feed_next < c->total_pkts) ctx->feed_next++;
  }
  if (ctx->feed_next >= c->total_pkts) {
    ctx->feed_frame++;
    ctx->feed_next = 0;
  }
  if (!rte_ring_sp_enqueue_bulk(ctx->session.ring[MTL_SESSION_PORT_P], pkts, c->bulk,
                                NULL)) {
    rte_pktmbuf_free_bulk((struct rte_mbuf**)pkts, c->bulk);
    return -ENOSPC;
  }
  return 0;
}

static bool ut_trs_frames_idle(const struct st_tx_video_session_impl* s) {
  return rte_ring_empty(s->ring[MTL_SESSION_PORT_P]) &&
         !s->trs_inflight_num[MTL_SESSION_PORT_P] &&
         !s->trs_inflight_num2[MTL_SESSION_PORT_P] &&
         !s->trs_pad_inflight_num[MTL_SESSION_PORT_P];
}

int ut_trs_run_frames(ut_trs_ctx* ctx, const struct ut_trs_frames_case* c) {
  static unsigned int frames_idx;
  struct st_tx_video_session_impl* s = &ctx->session;
  char name[RTE_MEMPOOL_NAMESIZE];
  int ret = -ETIMEDOUT;

  if (!c->bulk || c->bulk > UT_TRS_MAX_BULK || c->bulk > RTE_DIM(s->trs_inflight[0]) ||
      c->lead >= c->bulk || c->frames > UT_TRS_MAX_FRAMES || ctx->frame_pool ||
      ctx->stream_pool)
    return -EINVAL;
  (void)snprintf(name, sizeof(name), "ut_trs_frames_%u", frames_idx);
  ctx->frame_pool =
      rte_pktmbuf_pool_create(name, 512, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
  (void)snprintf(name, sizeof(name), "ut_trs_frames_ring_%u", frames_idx++);
  struct rte_ring* ring = ut_ring_create(name, 64);
  if (!ctx->frame_pool || !ring) {
    if (ring) rte_ring_free(ring);
    return -ENOMEM;
  }
  rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  s->ring[MTL_SESSION_PORT_P] = ring;

  s->bulk = c->bulk;
  s->pacing.pad_interval = c->pad_interval;
  s->pacing.trs = UT_TRS_FRAME_TRS;
  /* no warm-up pads: packet 0 waits for its target in WAIT_WARMUP */
  s->pacing.warm_pkts = 0;
  ctx->burst_accept_limit = c->accept_limit;
  ctx->burst_fail_every = c->fail_every;
  ctx->tsc_script_len = 0;
  ctx->feed_frame = 0;
  ctx->feed_next = -(int)c->lead;

  ut_trs_active_ctx = ctx;
  for (uint32_t step = 0; step < UT_TRS_FRAME_STEPS; step++) {
    while (ctx->feed_frame < c->frames && rte_ring_free_count(ring) >= c->bulk) {
      ret = ut_trs_feed_bulk(ctx, c);
      if (ret < 0) goto out;
    }
    if (ctx->feed_frame == c->frames && ut_trs_frames_idle(s)) {
      ret = 0;
      break;
    }
    ret = -ETIMEDOUT;
    video_trs_rl_tasklet(&ctx->impl, s, MTL_SESSION_PORT_P);
    rte_pktmbuf_free_bulk(ctx->sent, ctx->sent_count);
    ctx->sent_count = 0;
  }
out:
  ut_trs_active_ctx = NULL;
  return ret;
}

uint32_t ut_trs_wire_count(const ut_trs_ctx* ctx) {
  return ctx->wire_count;
}

uint32_t ut_trs_wire(const ut_trs_ctx* ctx, uint32_t pos) {
  return pos < ctx->wire_count ? ctx->wire[pos] : UINT32_MAX;
}

uint64_t ut_trs_wire_tsc(const ut_trs_ctx* ctx, uint32_t pos) {
  return pos < ctx->wire_count ? ctx->wire_tsc[pos] : 0;
}

uint64_t ut_trs_frame_target(const ut_trs_ctx* ctx, int frame) {
  return frame < UT_TRS_MAX_FRAMES ? ctx->frame_target[frame] : 0;
}

int ut_trs_stat_pkts_burst_dummy(const ut_trs_ctx* ctx) {
  return ctx->session.stat_pkts_burst_dummy;
}

unsigned int ut_trs_frame_pkts_in_use(const ut_trs_ctx* ctx) {
  return ctx->frame_pool ? rte_mempool_in_use_count(ctx->frame_pool) : 0;
}
