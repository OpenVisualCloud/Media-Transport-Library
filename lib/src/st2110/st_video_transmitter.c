/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_video_transmitter.h"

#include <math.h>
#include <stdlib.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_ptp.h"
#include "../mt_rtcp.h"
#include "st_err.h"
#include "st_tx_video_session.h"

static int video_trs_tasklet_start(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int video_trs_tasklet_stop(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static uint16_t video_trs_burst_fail(struct mtl_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
                                     enum mtl_session_port s_port, uint16_t nb_pkts) {
  uint64_t cur_tsc = mt_get_tsc(impl);
  uint64_t fail_duration = cur_tsc - s->last_burst_succ_time_tsc[s_port];

  if (fail_duration > s->tx_hang_detect_time_thresh) {
    err("%s(%d,%d), hang duration %" PRIu64 " ms\n", __func__, s->idx, s_port,
        fail_duration / NS_PER_MS);
    st20_tx_queue_fatal_error(impl, s, s_port);
    s->last_burst_succ_time_tsc[s_port] = cur_tsc;
    return nb_pkts; /* skip current pkts */
  }

  return 0;
}

static uint16_t video_trs_burst_pad(struct mtl_main_impl* impl,
                                    struct st_tx_video_session_impl* s,
                                    enum mtl_session_port s_port,
                                    struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  uint16_t tx = mt_txq_burst(s->queue[s_port], tx_pkts, nb_pkts);
  if (!tx) return video_trs_burst_fail(impl, s, s_port, nb_pkts);
  return tx;
}

static inline void video_trs_save_inflight(struct st_tx_video_session_impl* s,
                                           enum mtl_session_port s_port,
                                           struct rte_mbuf** pkts, unsigned int nb_pkts,
                                           bool use_two) {
  unsigned int i;

  if (!use_two) {
    s->trs_inflight_num[s_port] = nb_pkts;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < nb_pkts; i++) s->trs_inflight[s_port][i] = pkts[i];
  } else {
    s->trs_inflight_num2[s_port] = nb_pkts;
    s->trs_inflight_idx2[s_port] = 0;
    s->trs_inflight_cnt2[s_port]++;
    for (i = 0; i < nb_pkts; i++) s->trs_inflight2[s_port][i] = pkts[i];
  }
}

static inline void video_trs_tsn_mark_launch_time(struct st_tx_video_session_impl* s,
                                                  struct mt_interface* inf,
                                                  struct rte_mbuf** pkts,
                                                  unsigned int nb_pkts,
                                                  uint64_t cur_ptp_for_lt) {
  for (unsigned int i = 0; i < nb_pkts; i++) {
    uint64_t target_ptp = st_tx_mbuf_get_ptp(pkts[i]);

    pkts[i]->ol_flags |= inf->tx_launch_time_flag;
    *RTE_MBUF_DYNFIELD(pkts[i], inf->tx_dynfield_offset, uint64_t*) = target_ptp;
    if (target_ptp > cur_ptp_for_lt)
      s->stat_lt_future_pkts++;
    else
      s->stat_lt_past_pkts++;
  }
}

static void video_trs_tsn_mode_diag(struct st_tx_video_session_impl* s,
                                    enum mtl_session_port s_port,
                                    struct st_frame_trans* frame, uint64_t frame_epoch,
                                    uint32_t frame_rtp, bool have_prev_boundary,
                                    int64_t delta_ptp, int64_t delta_tsc,
                                    int64_t boundary_ptp, int64_t submit_boundary_ptp,
                                    int64_t submit_boundary_tsc, int32_t boundary_rtp) {
  int64_t sync_to_dequeue_ptp;
  int64_t enqueue_to_dequeue_ptp = 0;
  int64_t dequeue_headroom_ptp;
  int64_t prev_last_delta_ptp = 0;
  int64_t prev_last_delta_tsc = 0;
  bool have_prev_last_delta = s->stat_tsn_prev_last_delta_valid[s_port];
  bool prev_lagging = s->stat_tsn_mode_lagging[s_port];
  bool lagging = prev_lagging;

  if (!frame->tsn_debug.first_dequeue_valid[s_port]) return;

  sync_to_dequeue_ptp = (int64_t)frame->tsn_debug.first_dequeue_ptp[s_port] -
                        (int64_t)frame->tsn_debug.sync_ptp;
  if (frame->tsn_debug.first_enqueue_valid[s_port]) {
    enqueue_to_dequeue_ptp = (int64_t)frame->tsn_debug.first_dequeue_ptp[s_port] -
                             (int64_t)frame->tsn_debug.first_enqueue_ptp[s_port];
  }
  dequeue_headroom_ptp = frame->tsn_debug.sync_time_to_tx_ns - sync_to_dequeue_ptp;

  if (have_prev_last_delta) {
    prev_last_delta_ptp = s->stat_tsn_prev_last_delta_ptp[s_port];
    prev_last_delta_tsc = s->stat_tsn_prev_last_delta_tsc[s_port];
  }

  if (!s->stat_tsn_mode_diag_init[s_port]) {
    s->stat_tsn_mode_min_deq_headroom_ns[s_port] = dequeue_headroom_ptp;
    s->stat_tsn_mode_min_submit_headroom_ns[s_port] = delta_ptp;
    s->stat_tsn_mode_min_submit_gap_ns[s_port] = submit_boundary_ptp;
    s->stat_tsn_mode_min_prev_last_delta_ns[s_port] = prev_last_delta_ptp;
    s->stat_tsn_mode_diag_init[s_port] = true;
  } else {
    if (dequeue_headroom_ptp < s->stat_tsn_mode_min_deq_headroom_ns[s_port])
      s->stat_tsn_mode_min_deq_headroom_ns[s_port] = dequeue_headroom_ptp;
    if (delta_ptp < s->stat_tsn_mode_min_submit_headroom_ns[s_port])
      s->stat_tsn_mode_min_submit_headroom_ns[s_port] = delta_ptp;
    if (submit_boundary_ptp < s->stat_tsn_mode_min_submit_gap_ns[s_port])
      s->stat_tsn_mode_min_submit_gap_ns[s_port] = submit_boundary_ptp;
    if (prev_last_delta_ptp < s->stat_tsn_mode_min_prev_last_delta_ns[s_port])
      s->stat_tsn_mode_min_prev_last_delta_ns[s_port] = prev_last_delta_ptp;
  }

  s->stat_tsn_mode_samples[s_port]++;

  if (prev_lagging) {
    bool recovered =
        (dequeue_headroom_ptp > ST_TSN_MODE_EXIT_DEQ_HEADROOM_NS) &&
        (!have_prev_boundary || (submit_boundary_ptp > ST_TSN_MODE_EXIT_SUBMIT_GAP_NS)) &&
        (!have_prev_last_delta || (prev_last_delta_ptp > ST_TSN_MODE_EXIT_LAST_LATE_NS));
    if (recovered) lagging = false;
  } else {
    bool enter_lag =
        (dequeue_headroom_ptp < ST_TSN_MODE_ENTER_DEQ_HEADROOM_NS) ||
        (have_prev_boundary && (submit_boundary_ptp < ST_TSN_MODE_ENTER_SUBMIT_GAP_NS)) ||
        (have_prev_last_delta && (prev_last_delta_ptp < ST_TSN_MODE_ENTER_LAST_LATE_NS));
    if (enter_lag) lagging = true;
  }

  if (lagging) s->stat_tsn_mode_lag_samples[s_port]++;
  if (lagging == prev_lagging) return;

  if (lagging)
    s->stat_tsn_mode_entries[s_port]++;
  else
    s->stat_tsn_mode_recoveries[s_port]++;
  s->stat_tsn_mode_lagging[s_port] = lagging;

  if (s->stat_tsn_mode_logs[s_port] >= ST_TSN_MODE_DIAG_MAX) return;

  notice("%s(%d,%d), TSN MODE[%u]: state=%s prev=%s frame_idx=%d epoch=%" PRIu64
         " frame_rtp=%u sync_time_to_tx_ns=%" PRId64 " sync_to_deq_ptp_ns=%" PRId64
         " enqueue_to_deq_ptp_ns=%" PRId64 " deq_headroom_ptp_ns=%" PRId64
         " submit_headroom_ptp_ns=%" PRId64 " submit_headroom_tsc_ns=%" PRId64
         " prev_last_delta_ptp_ns=%" PRId64 " prev_last_delta_tsc_ns=%" PRId64
         " submit_gap_ptp_ns=%" PRId64 " submit_gap_tsc_ns=%" PRId64
         " target_gap_ptp_ns=%" PRId64 " gap_rtp=%d ring_count=%u inflight=%u\n",
         __func__, s->idx, s_port, s->stat_tsn_mode_logs[s_port],
         lagging ? "lagging" : "healthy", prev_lagging ? "lagging" : "healthy",
         frame->idx, frame_epoch, frame_rtp, frame->tsn_debug.sync_time_to_tx_ns,
         sync_to_dequeue_ptp, enqueue_to_dequeue_ptp, dequeue_headroom_ptp, delta_ptp,
         delta_tsc, prev_last_delta_ptp, prev_last_delta_tsc, submit_boundary_ptp,
         submit_boundary_tsc, boundary_ptp, boundary_rtp, rte_ring_count(s->ring[s_port]),
         s->trs_inflight_num[s_port]);
  s->stat_tsn_mode_logs[s_port]++;
}

static inline void video_trs_tsn_note_inflight_peak(struct st_tx_video_session_impl* s,
                                                    enum mtl_session_port s_port) {
  if (s->trs_inflight_num[s_port] > s->stat_tsn_tx_inflight_peak[s_port])
    s->stat_tsn_tx_inflight_peak[s_port] = s->trs_inflight_num[s_port];
  if (s->trs_inflight_num2[s_port] > s->stat_tsn_tx_inflight2_peak[s_port])
    s->stat_tsn_tx_inflight2_peak[s_port] = s->trs_inflight_num2[s_port];
}

static void video_trs_tsn_stall_diag(struct mtl_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
                                     enum mtl_session_port s_port, const char* phase,
                                     struct rte_ring* ring, struct rte_mbuf** pkts,
                                     unsigned int requested, unsigned int sent) {
  struct st_frame_trans* frame;
  uint64_t cur_tsc;
  uint64_t cur_ptp;
  uint64_t frame_epoch = 0;
  uint32_t frame_rtp = 0;
  uint32_t pkt_idx;
  int64_t delta_ptp;
  int64_t delta_tsc;
  int frame_idx = -1;

  if (!pkts || (s->stat_tsn_tx_stall_logs[s_port] >= ST_TSN_TX_STALL_LOG_MAX)) return;

  frame = st_tx_mbuf_get_priv(pkts[0]);
  pkt_idx = st_tx_mbuf_get_idx(pkts[0]);
  cur_tsc = mt_get_tsc(impl);
  cur_ptp = mt_get_ptp_time(impl, mt_port_logic2phy(s->port_maps, s_port));
  delta_ptp = (int64_t)st_tx_mbuf_get_ptp(pkts[0]) - (int64_t)cur_ptp;
  delta_tsc = (int64_t)st_tx_mbuf_get_tsc(pkts[0]) - (int64_t)cur_tsc;

  if (frame) {
    frame_idx = frame->idx;
    if (s->st22_info) {
      frame_epoch = frame->tx_st22_meta.epoch;
      frame_rtp = frame->tx_st22_meta.rtp_timestamp;
    } else {
      frame_epoch = frame->tv_meta.epoch;
      frame_rtp = frame->tv_meta.rtp_timestamp;
    }
  }

  notice("%s(%d,%d), TSN TX STALL[%u]: phase=%s frame_idx=%d epoch=%" PRIu64
         " frame_rtp=%u pkt_idx=%u req=%u sent=%u remain=%u ring_count=%u"
         " inflight=%u inflight2=%u queue=%u delta_ptp_ns=%" PRId64
         " delta_tsc_ns=%" PRId64 "\n",
         __func__, s->idx, s_port, s->stat_tsn_tx_stall_logs[s_port], phase, frame_idx,
         frame_epoch, frame_rtp, pkt_idx, requested, sent, requested - sent,
         rte_ring_count(ring), s->trs_inflight_num[s_port], s->trs_inflight_num2[s_port],
         mt_txq_queue_id(s->queue[s_port]), delta_ptp, delta_tsc);
  s->stat_tsn_tx_stall_logs[s_port]++;
}

/* for normal pkts, pad should call the video_trs_burst_pad */
static uint16_t video_trs_burst(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port, struct rte_mbuf** tx_pkts,
                                uint16_t nb_pkts) {
  if (s->rtcp_tx[s_port]) mt_mbuf_refcnt_inc_bulk(tx_pkts, nb_pkts);
  uint16_t tx = mt_txq_burst(s->queue[s_port], tx_pkts, nb_pkts);
  s->stat_pkts_burst += tx;
  if (!tx) {
    if (s->rtcp_tx[s_port]) rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
    return video_trs_burst_fail(impl, s, s_port, nb_pkts);
  }

  if (s->rtcp_tx[s_port]) {
    mt_rtcp_tx_buffer_rtp_packets(s->rtcp_tx[s_port], tx_pkts, tx);
    rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
  }

  int pkt_idx = st_tx_mbuf_get_idx(tx_pkts[0]);
  if (0 == pkt_idx) {
    struct st_frame_trans* frame = st_tx_mbuf_get_priv(tx_pkts[0]);
    if ((s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_TSN) && frame) {
      uint64_t target_ptp = st_tx_mbuf_get_ptp(tx_pkts[0]);
      uint64_t cur_ptp = mt_get_ptp_time(impl, mt_port_logic2phy(s->port_maps, s_port));
      int64_t delta_ptp = (int64_t)target_ptp - (int64_t)cur_ptp;

      if (frame->tsn_debug.first_dequeue_valid[s_port]) {
        uint64_t cur_tsc = mt_get_tsc(impl);
        int64_t delta_tsc = (int64_t)st_tx_mbuf_get_tsc(tx_pkts[0]) - (int64_t)cur_tsc;
        int64_t boundary_ptp = 0, submit_boundary_ptp = 0, submit_boundary_tsc = 0;
        int32_t boundary_rtp = 0;
        bool have_prev_boundary = s->stat_tsn_prev_last_valid[s_port];
        uint64_t frame_epoch;
        uint32_t frame_rtp;
        bool stable_window;

        if (s->st22_info) {
          frame_epoch = frame->tx_st22_meta.epoch;
          frame_rtp = frame->tx_st22_meta.rtp_timestamp;
        } else {
          frame_epoch = frame->tv_meta.epoch;
          frame_rtp = frame->tv_meta.rtp_timestamp;
        }

        if (have_prev_boundary) {
          boundary_ptp =
              (int64_t)target_ptp - (int64_t)s->stat_tsn_prev_last_target_ptp[s_port];
          submit_boundary_ptp =
              (int64_t)cur_ptp - (int64_t)s->stat_tsn_prev_last_submit_ptp[s_port];
          submit_boundary_tsc =
              (int64_t)cur_tsc - (int64_t)s->stat_tsn_prev_last_submit_tsc[s_port];
          boundary_rtp =
              (int32_t)frame_rtp - (int32_t)s->stat_tsn_prev_last_frame_rtp[s_port];
        }

        if (s->stat_tsn_trace_anchor_ptp == 0) s->stat_tsn_trace_anchor_ptp = cur_ptp;
        stable_window =
            (cur_ptp >= (s->stat_tsn_trace_anchor_ptp + ST_TSN_STABLE_TRACE_DELAY_NS));

        if (stable_window) {
          video_trs_tsn_mode_diag(s, s_port, frame, frame_epoch, frame_rtp,
                                  have_prev_boundary, delta_ptp, delta_tsc, boundary_ptp,
                                  submit_boundary_ptp, submit_boundary_tsc, boundary_rtp);
        }
      }
    }
    if (frame) st20_frame_tx_start(impl, s, s_port, frame);
  }

  for (uint16_t i = 0; i < tx; i++) {
    uint32_t burst_pkt_idx = st_tx_mbuf_get_idx(tx_pkts[i]);
    if ((s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_TSN) &&
        (burst_pkt_idx == (uint32_t)(s->st20_total_pkts - 1))) {
      s->stat_tsn_prev_last_target_tsc[s_port] = st_tx_mbuf_get_tsc(tx_pkts[i]);
      s->stat_tsn_prev_last_target_ptp[s_port] = st_tx_mbuf_get_ptp(tx_pkts[i]);
      s->stat_tsn_prev_last_submit_tsc[s_port] = mt_get_tsc(impl);
      s->stat_tsn_prev_last_submit_ptp[s_port] =
          mt_get_ptp_time(impl, mt_port_logic2phy(s->port_maps, s_port));
      struct st_frame_trans* frame = st_tx_mbuf_get_priv(tx_pkts[i]);
      if (frame) {
        if (s->st22_info) {
          s->stat_tsn_prev_last_frame_rtp[s_port] = frame->tx_st22_meta.rtp_timestamp;
          s->stat_tsn_prev_last_frame_epoch[s_port] = frame->tx_st22_meta.epoch;
        } else {
          s->stat_tsn_prev_last_frame_rtp[s_port] = frame->tv_meta.rtp_timestamp;
          s->stat_tsn_prev_last_frame_epoch[s_port] = frame->tv_meta.epoch;
        }
      }
      s->stat_tsn_prev_last_valid[s_port] = true;

      int64_t last_delta_ptp =
          (int64_t)st_tx_mbuf_get_ptp(tx_pkts[i]) -
          (int64_t)mt_get_ptp_time(impl, mt_port_logic2phy(s->port_maps, s_port));
      int64_t last_delta_tsc =
          (int64_t)st_tx_mbuf_get_tsc(tx_pkts[i]) - (int64_t)mt_get_tsc(impl);
      s->stat_tsn_prev_last_delta_ptp[s_port] = last_delta_ptp;
      s->stat_tsn_prev_last_delta_tsc[s_port] = last_delta_tsc;
      s->stat_tsn_prev_last_delta_valid[s_port] = true;
    }
    s->port_user_stats.common.port[s_port].bytes += tx_pkts[i]->pkt_len;
    s->port_user_stats.common.port[s_port].packets++;
  }

  s->last_burst_succ_time_tsc[s_port] = mt_get_tsc(impl);
  return tx;
}

/* warm start for the first packet */
static void video_trs_rl_warm_up(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  uint64_t cur_tsc;
  int64_t warm_pkts;
  struct rte_mbuf* pads[1];
  int64_t delta_pkts;
  unsigned int tx;

  if (!target_tsc) {
    err("%s(%d), target_tsc is zero\n", __func__, s->idx);
    return;
  }
  cur_tsc = mt_get_tsc(impl);

  /* Calculate warm packets needed (pacing->trs - 1 added to ceil the result) */
  warm_pkts = (target_tsc - cur_tsc + pacing->trs - 1) / pacing->trs;

  if (warm_pkts < 0 || warm_pkts > pacing->warm_pkts) {
    dbg("%s(%d), mismatch timing with %ld\n", __func__, s->idx, warm_pkts);
    s->port_user_stats.stat_trans_troffset_mismatch++;
    return;
  }

  pads[0] = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  for (int i = 0; i < warm_pkts; i++) {
    rte_mbuf_refcnt_update(pads[0], 1);
    tx = video_trs_burst_pad(impl, s, s_port, &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    if (tx < 1) s->trs_pad_inflight_num[s_port]++;

    /* re-calculate the delta */
    cur_tsc = mt_get_tsc(impl);
    delta_pkts = (target_tsc - cur_tsc + pacing->trs - 1) / pacing->trs;
    if (delta_pkts < warm_pkts - (i + 1)) {
      warm_pkts = delta_pkts;
      s->port_user_stats.stat_trans_recalculate_warmup++;
      dbg("%s(%d), mismatch delta_pkts %ld at %d\n", __func__, s->idx, delta_pkts, i);
    }
  }

  return;
}

static int video_burst_packet(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port, struct rte_mbuf** pkts,
                              int bulk, bool use_two) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  int tx = video_trs_burst(impl, s, s_port, &pkts[0], bulk);
  int pkt_idx = st_tx_mbuf_get_idx(pkts[0]);

  if (tx < bulk) {
    unsigned int i;
    unsigned int remaining = bulk - tx;

    if (!use_two) {
      s->trs_inflight_num[s_port] = remaining;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
    } else {
      s->trs_inflight_num2[s_port] = remaining;
      s->trs_inflight_idx2[s_port] = 0;
      s->trs_inflight_cnt2[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight2[s_port][i] = pkts[tx + i];
    }
  }

  /* check if it need insert padding packet */
  if (fmodf(pkt_idx + 1 + pacing->pad_interval / 2, pacing->pad_interval) < bulk) {
    rte_mbuf_refcnt_update(s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    tx = video_trs_burst_pad(impl, s, s_port, &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    if (tx < 1) s->trs_pad_inflight_num[s_port]++;
  }

  return 0;
}

static int _video_trs_rl_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port, int* ret_status) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx;
  unsigned int n, tx;
  uint32_t pkt_idx = 0;

  /* check if any inflight pkts in transmitter inflight 2 */
  if (s->trs_inflight_num2[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight2[s_port][s->trs_inflight_idx2[s_port]],
                         s->trs_inflight_num2[s_port]);
    s->trs_inflight_num2[s_port] -= tx;
    s->trs_inflight_idx2[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT2_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* check if it's pending on the first pkt */
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    target_tsc -= s->pacing.warm_pkts * s->pacing.trs; /* Start warmup earlier */
    uint64_t cur_tsc = mt_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        *ret_status = -STI_RLTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
        *ret_status = -STI_RLTRS_TARGET_TSC_NOT_REACH;
        return MTL_TASKLET_ALL_DONE;
      }
    }
    video_trs_rl_warm_up(impl, s, s_port);
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any padding inflight pkts in transmitter */
  if (s->trs_pad_inflight_num[s_port] > 0) {
    dbg("%s(%d), inflight padding pkts %d\n", __func__, idx,
        s->trs_pad_inflight_num[s_port]);
    tx = video_trs_burst_pad(impl, s, s_port, &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    s->trs_pad_inflight_num[s_port] -= tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    *ret_status = -STI_RLTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  int valid_bulk = bulk;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if ((pkt_idx == 0) || (pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
      valid_bulk = i;
      break; /* break if it's the first pkt of frame or it's the start of dummy */
    }
  }
  dbg("%s(%d), pkt_idx %u valid_bulk %d ts %" PRIu64 "\n", __func__, idx, pkt_idx,
      valid_bulk, st_tx_mbuf_get_tsc(pkts[0]));

  /* builder always build bulk pkts per enqueue, pkts after dummy are all dummy */
  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    video_burst_packet(impl, s, s_port, pkts, valid_bulk, false);
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    dbg("%s(%d), pkt_idx %" PRIu64 " ts %" PRIu64 "\n", __func__, idx, (uint64_t)pkt_idx,
        st_tx_mbuf_get_tsc(pkts[0]));
    *ret_status = -STI_RLTRS_BURST_HAS_DUMMY;
    return MTL_TASKLET_HAS_PENDING;
  }

  if (unlikely(!pkt_idx)) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    if (valid_bulk != 0) {
      video_burst_packet(impl, s, s_port, pkts, valid_bulk, true);
    }
    uint64_t target_tsc = st_tx_mbuf_get_tsc(pkts[valid_bulk]);
    uint64_t target_ptp = st_tx_mbuf_get_ptp(pkts[valid_bulk]);
    dbg("%s(%d), first pkt, ts cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
        cur_tsc, target_tsc);
    if (likely(cur_tsc < target_tsc || s->trs_inflight_num2[s_port])) {
      unsigned int i;
      uint64_t delta = target_tsc - cur_tsc;

      if (likely(delta < NS_PER_S || s->trs_inflight_num2[s_port])) {
        s->trs_target_tsc[s_port] = target_tsc;
        s->trs_target_ptp[s_port] = target_ptp;
        /* save it on inflight */
        s->trs_inflight_num[s_port] = bulk - valid_bulk;
        s->trs_inflight_idx[s_port] = 0;
        s->trs_inflight_cnt[s_port]++;
        for (i = 0; i < bulk - valid_bulk; i++)
          s->trs_inflight[s_port][i] = pkts[i + valid_bulk];
        *ret_status = -STI_RLTRS_1ST_PKT_TSC;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc for first pkt cur %" PRIu64 " target %" PRIu64 "\n",
            __func__, idx, cur_tsc, target_tsc);
      }
    } else {
      s->trs_target_tsc[s_port] = target_tsc;
      video_trs_rl_warm_up(impl, s, s_port);
      s->trs_target_tsc[s_port] = 0;
    }
  }

  int pos = (valid_bulk == bulk) ? 0 : valid_bulk;

  video_burst_packet(impl, s, s_port, &pkts[pos], bulk - pos, false);

  *ret_status = 1;
  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_rl_tasklet(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port) {
  int pending = MTL_TASKLET_ALL_DONE;
  int ret_status = 0;

  pending += _video_trs_rl_tasklet(impl, s, s_port, &ret_status);
  /*
   * Try to burst pkts again for the performance, in this way nic tx get double
   * bulk since tx pkt is in critical path
   */
  if (ret_status > 0) {
    ret_status = 0;
    pending += _video_trs_rl_tasklet(impl, s, s_port, &ret_status);
  }
  s->stat_trs_ret_code[s_port] = ret_status;
  return pending;
}

static int video_trs_tsc_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  if (s->pacing_way[s_port] == ST21_TX_PACING_WAY_BE) bulk = 1;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx, tx;
  unsigned int n;
  uint64_t target_tsc, cur_tsc;

  /* check if it's pending on the tsc */
  target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    cur_tsc = mt_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx = 0;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  if (s->pacing_way[s_port] != ST21_TX_PACING_WAY_BE || pkt_idx == 0) {
    cur_tsc = mt_get_tsc(impl);
    target_tsc = st_tx_mbuf_get_tsc(pkts[0]);
    if (cur_tsc < target_tsc) {
      unsigned int i;
      uint64_t delta = target_tsc - cur_tsc;

      if (likely(delta < NS_PER_S)) {
        s->trs_target_tsc[s_port] = target_tsc;
        /* save it on inflight */
        s->trs_inflight_num[s_port] = valid_bulk;
        s->trs_inflight_idx[s_port] = 0;
        s->trs_inflight_cnt[s_port]++;
        for (i = 0; i < valid_bulk; i++) s->trs_inflight[s_port][i] = pkts[i];
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
  }

  tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_launch_time_tasklet(struct mtl_main_impl* impl,
                                         struct st_tx_video_session_impl* s,
                                         enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int tx = 0;
  unsigned int n;
  uint64_t target_tsc = 0, cur_tsc = 0;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);

  /* TSC gate removed — submit immediately, let HW schedule via launch time */

  if (s->trs_inflight_num[s_port] > 0) {
    s->stat_tsn_tx_retry_inflight[s_port]++;
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      s->stat_tsn_tx_calls++;
      s->stat_tsn_tx_bulks_sum++;
      if (s->stat_tsn_tx_bulks_max < 1) s->stat_tsn_tx_bulks_max = 1;
      if (s->trs_inflight_num[s_port] > 0) {
        s->stat_tsn_tx_partial++;
        s->stat_tsn_tx_saved_partial_pkts[s_port] += s->trs_inflight_num[s_port];
        video_trs_tsn_stall_diag(impl, s, s_port, "inflight-partial", ring,
                                 &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                                 s->trs_inflight_num[s_port] + tx, tx);
        video_trs_tsn_note_inflight_peak(s, s_port);
      }
      return MTL_TASKLET_HAS_PENDING;
    } else {
      video_trs_tsn_stall_diag(impl, s, s_port, "inflight-zero", ring,
                               &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                               s->trs_inflight_num[s_port], 0);
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (rte_ring_count(ring) > s->stat_tsn_tx_ring_peak)
    s->stat_tsn_tx_ring_peak = rte_ring_count(ring);

  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_tsn_tx_dequeue_empty[s_port]++;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  int valid_bulk = bulk;
  uint32_t pkt_idx = 0;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  if (valid_bulk > 0) {
    uint64_t cur_ptp_for_lt = mt_get_ptp_time(impl, MTL_PORT_P);
    video_trs_tsn_mark_launch_time(s, inf, &pkts[0], valid_bulk, cur_ptp_for_lt);

    cur_tsc = mt_get_tsc(impl);
    target_tsc = st_tx_mbuf_get_tsc(pkts[0]);
    if ((s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_TSN) &&
        (st_tx_mbuf_get_idx(pkts[0]) == 0)) {
      struct st_frame_trans* frame = st_tx_mbuf_get_priv(pkts[0]);

      if (frame && !frame->tsn_debug.first_dequeue_valid[s_port]) {
        frame->tsn_debug.first_dequeue_tsc[s_port] = cur_tsc;
        frame->tsn_debug.first_dequeue_ptp[s_port] = cur_ptp_for_lt;
        frame->tsn_debug.first_dequeue_valid[s_port] = true;

        if (s->stat_tsn_startup_dequeue_logs[s_port] < ST_TSN_STARTUP_TRACE_FRAMES) {
          int64_t enqueue_to_dequeue_ptp = 0;
          int64_t enqueue_to_dequeue_tsc = 0;

          if (frame->tsn_debug.first_enqueue_valid[s_port]) {
            enqueue_to_dequeue_ptp = (int64_t)cur_ptp_for_lt -
                                     (int64_t)frame->tsn_debug.first_enqueue_ptp[s_port];
            enqueue_to_dequeue_tsc =
                (int64_t)cur_tsc - (int64_t)frame->tsn_debug.first_enqueue_tsc[s_port];
          }

          notice("%s(%d,%d), TSN STARTUP DEQ[%u]: frame_idx=%d epoch=%" PRIu64
                 " sync_to_deq_ptp_ns=%" PRId64 " sync_to_deq_tsc_ns=%" PRId64
                 " enqueue_to_deq_ptp_ns=%" PRId64 " enqueue_to_deq_tsc_ns=%" PRId64
                 " delta_ptp_ns=%" PRId64 " delta_tsc_ns=%" PRId64 " ring_count=%u\n",
                 __func__, s->idx, s_port, s->stat_tsn_startup_dequeue_logs[s_port],
                 frame->idx, frame->tv_meta.epoch,
                 (int64_t)cur_ptp_for_lt - (int64_t)frame->tsn_debug.sync_ptp,
                 (int64_t)cur_tsc - (int64_t)frame->tsn_debug.sync_tsc,
                 enqueue_to_dequeue_ptp, enqueue_to_dequeue_tsc,
                 (int64_t)st_tx_mbuf_get_ptp(pkts[0]) - (int64_t)cur_ptp_for_lt,
                 (int64_t)target_tsc - (int64_t)cur_tsc, rte_ring_count(ring));
          s->stat_tsn_startup_dequeue_logs[s_port]++;
        } else if ((cur_ptp_for_lt >=
                    (s->stat_tsn_trace_anchor_ptp + ST_TSN_STABLE_TRACE_DELAY_NS)) &&
                   (s->stat_tsn_stable_dequeue_logs[s_port] <
                    ST_TSN_STARTUP_TRACE_FRAMES)) {
          int64_t enqueue_to_dequeue_ptp = 0;
          int64_t enqueue_to_dequeue_tsc = 0;

          if (frame->tsn_debug.first_enqueue_valid[s_port]) {
            enqueue_to_dequeue_ptp = (int64_t)cur_ptp_for_lt -
                                     (int64_t)frame->tsn_debug.first_enqueue_ptp[s_port];
            enqueue_to_dequeue_tsc =
                (int64_t)cur_tsc - (int64_t)frame->tsn_debug.first_enqueue_tsc[s_port];
          }

          notice("%s(%d,%d), TSN STABLE DEQ[%u]: frame_idx=%d epoch=%" PRIu64
                 " sync_to_deq_ptp_ns=%" PRId64 " sync_to_deq_tsc_ns=%" PRId64
                 " enqueue_to_deq_ptp_ns=%" PRId64 " enqueue_to_deq_tsc_ns=%" PRId64
                 " delta_ptp_ns=%" PRId64 " delta_tsc_ns=%" PRId64 " ring_count=%u\n",
                 __func__, s->idx, s_port, s->stat_tsn_stable_dequeue_logs[s_port],
                 frame->idx, frame->tv_meta.epoch,
                 (int64_t)cur_ptp_for_lt - (int64_t)frame->tsn_debug.sync_ptp,
                 (int64_t)cur_tsc - (int64_t)frame->tsn_debug.sync_tsc,
                 enqueue_to_dequeue_ptp, enqueue_to_dequeue_tsc,
                 (int64_t)st_tx_mbuf_get_ptp(pkts[0]) - (int64_t)cur_ptp_for_lt,
                 (int64_t)target_tsc - (int64_t)cur_tsc, rte_ring_count(ring));
          s->stat_tsn_stable_dequeue_logs[s_port]++;
        }
      }
    }
    /* No TSC gate — submit immediately, let HW schedule via launch time */
    tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);
    s->stat_tsn_tx_calls++;
    s->stat_tsn_tx_bulks_sum++;
    if (s->stat_tsn_tx_bulks_max < 1) s->stat_tsn_tx_bulks_max = 1;

    if (tx < valid_bulk) {
      unsigned int remaining = valid_bulk - tx;

      video_trs_save_inflight(s, s_port, &pkts[tx], remaining, false);
      video_trs_tsn_note_inflight_peak(s, s_port);
      s->stat_tsn_tx_partial++;
      s->stat_tsn_tx_saved_partial_pkts[s_port] += remaining;
      video_trs_tsn_stall_diag(impl, s, s_port, "fresh-partial", ring, &pkts[0],
                               valid_bulk, tx);
    }
  }

  /* Inner drain loop: keep submitting while NIC accepts and ring has data.
   * This avoids returning to the scheduler (and re-running all other tasklets)
   * between every 32-pkt batch. The loop stops when:
   *   - NIC ring full (partial burst saved as inflight for next tasklet call)
   *   - Software ring empty
   *   - Budget exhausted (cap iterations to avoid starving other tasklets) */
  unsigned int drain_budget = 128; /* max batches per tasklet invocation */
  while (drain_budget > 0 && s->trs_inflight_num[s_port] == 0) {
    drain_budget--;
    n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
    if (n == 0) break;

    valid_bulk = bulk;
    pkt_idx = 0;
    for (int i = 0; i < (int)bulk; i++) {
      pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
      if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
        valid_bulk = i;
        break;
      }
    }
    if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
      rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
      s->stat_pkts_burst_dummy += bulk - valid_bulk;
    }
    if (valid_bulk > 0) {
      uint64_t lt_ptp = mt_get_ptp_time(impl, MTL_PORT_P);
      video_trs_tsn_mark_launch_time(s, inf, &pkts[0], valid_bulk, lt_ptp);
      tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);
      s->stat_tsn_tx_calls++;
      s->stat_tsn_tx_bulks_sum++;
      if (tx < valid_bulk) {
        unsigned int remaining = valid_bulk - tx;
        video_trs_save_inflight(s, s_port, &pkts[tx], remaining, false);
        video_trs_tsn_note_inflight_peak(s, s_port);
        s->stat_tsn_tx_partial++;
        s->stat_tsn_tx_saved_partial_pkts[s_port] += remaining;
        break; /* NIC ring full, stop draining */
      }
    }
    if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) break; /* frame boundary */
  }

  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_ptp_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx, tx;
  unsigned int n;
  uint64_t target_ptp, cur_ptp;

  /* check if it's pending on the tsc */
  target_ptp = s->trs_target_tsc[s_port];
  if (target_ptp) {
    cur_ptp = mt_get_ptp_time(impl, MTL_PORT_P);
    if (cur_ptp < target_ptp) {
      uint64_t delta = target_ptp - cur_ptp;
      if (likely(delta < NS_PER_S)) {
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_ptp, target_ptp);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx = 0;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  cur_ptp = mt_get_ptp_time(impl, MTL_PORT_P);
  target_ptp = st_tx_mbuf_get_ptp(pkts[0]);
  if (cur_ptp < target_ptp) {
    unsigned int i;
    uint64_t delta = target_ptp - cur_ptp;

    if (likely(delta < NS_PER_S)) {
      s->trs_target_tsc[s_port] = target_ptp;
      /* save it on inflight */
      s->trs_inflight_num[s_port] = valid_bulk;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < valid_bulk; i++) s->trs_inflight[s_port][i] = pkts[i];
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
      return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                              : MTL_TASKLET_ALL_DONE;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_ptp, target_ptp);
    }
  }

  tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_tasklet_handler(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  struct mtl_main_impl* impl = trs->parent;
  struct st_tx_video_sessions_mgr* mgr = trs->mgr;
  struct st_tx_video_session_impl* s;
  int sidx, s_port;
  int pending = MTL_TASKLET_ALL_DONE;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    for (s_port = 0; s_port < s->ops.num_port; s_port++) {
      if (!s->queue[s_port]) continue;
      pending += s->pacing_tasklet_func[s_port](impl, s, s_port);
    }
    tx_video_session_put(mgr, sidx);
  }

  return pending;
}

int st_video_resolve_pacing_tasklet(struct st_tx_video_session_impl* s,
                                    enum mtl_session_port port) {
  int idx = s->idx;

  switch (s->pacing_way[port]) {
    case ST21_TX_PACING_WAY_RL:
      s->pacing_tasklet_func[port] = video_trs_rl_tasklet;
      break;
    case ST21_TX_PACING_WAY_TSN:
      s->pacing_tasklet_func[port] = video_trs_launch_time_tasklet;
      break;
    case ST21_TX_PACING_WAY_TSC:
    case ST21_TX_PACING_WAY_BE:
    case ST21_TX_PACING_WAY_TSC_NARROW:
      s->pacing_tasklet_func[port] = video_trs_tsc_tasklet;
      break;
    case ST21_TX_PACING_WAY_PTP:
      s->pacing_tasklet_func[port] = video_trs_ptp_tasklet;
      break;
    default:
      err("%s(%d), unknow pacing %d\n", __func__, idx, s->pacing_way[port]);
      return -EIO;
  }
  return 0;
}

int st_video_transmitter_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  trs->parent = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "video_transmitter";
  ops.start = video_trs_tasklet_start;
  ops.stop = video_trs_tasklet_stop;
  ops.handler = video_trs_tasklet_handler;

  trs->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!trs->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs) {
  int idx = trs->idx;

  if (trs->tasklet) {
    mtl_sch_unregister_tasklet(trs->tasklet);
    trs->tasklet = NULL;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
