/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_video_transmitter.h"

#include <math.h>

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
    if (frame) st20_frame_tx_start(impl, s, s_port, frame);
  }

  for (uint16_t i = 0; i < tx; i++) {
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
  uint64_t i;
  uint64_t target_ptp;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);

  /* DEBUG: TSN HW launch time rides the NIC PHC directly, so it paces correctly
   * whether or not PTP is disciplined to an external master. The software TSC
   * fallback (taken when PTP is not "locked") is removed here so that every
   * burst goes through the HW launch-time path under test. Final state: TSN
   * always runs with --ptp. */

// #ifdef MTL_TXPP_PROBE
//   /* TXPP probe P-call: tasklet entry cadence. If the tasklet itself is not
//    * scheduled for ~one frame_time, the launch stamps of the queued packets go
//    * stale before they reach the HW. max_gap = worst entry-to-entry interval in
//    * the window; ring at entry shows how full the SW ring is when we are (re)run.
//    * Logged ~every 10ms, forced whenever a single inter-entry gap exceeds 1ms. */
//   {
//     static uint64_t prev_entry_tsc[MTL_SESSION_PORT_MAX];
//     static uint64_t win_max_gap[MTL_SESSION_PORT_MAX];
//     static uint64_t win_calls[MTL_SESSION_PORT_MAX];
//     static uint64_t probe_next_tsc;
//     uint64_t entry_tsc = mt_get_tsc(impl);
//     uint64_t gap = prev_entry_tsc[s_port] ? (entry_tsc - prev_entry_tsc[s_port]) : 0;
//     prev_entry_tsc[s_port] = entry_tsc;
//     if (gap > win_max_gap[s_port]) win_max_gap[s_port] = gap;
//     win_calls[s_port]++;
//     if (gap > 1000000 || entry_tsc >= probe_next_tsc) {
//       info("%s(%d), TXPP P-call: gap=%" PRIu64 " ns max_gap=%" PRIu64
//            " ns calls=%" PRIu64 " ring=%u inflight=%u%s\n",
//            __func__, s->idx, gap, win_max_gap[s_port], win_calls[s_port],
//            rte_ring_count(s->ring[s_port]), s->trs_inflight_num[s_port],
//            gap > 1000000 ? " STARVED" : "");
//       probe_next_tsc = entry_tsc + rte_get_tsc_hz() / 100;
//       win_max_gap[s_port] = 0;
//       win_calls[s_port] = 0;
//     }
//   }
// #endif

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
  uint32_t pkt_idx;
  for (i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
  }

  if (valid_bulk > 0) {
    for (i = 0; i < valid_bulk; i++) {
      target_ptp = st_tx_mbuf_get_ptp(pkts[i]);
      /* Put tx timestamp into transmit descriptor */
      pkts[i]->ol_flags |= inf->tx_launch_time_flag;
      *RTE_MBUF_DYNFIELD(pkts[i], inf->tx_dynfield_offset, uint64_t*) = target_ptp;
    }

// #ifdef MTL_TXPP_PROBE
//     /* TXPP probe P-trs: the launch timestamps as they actually reach the NIC.
//      * lead = first descriptor's launch time vs the live PHC at submit time
//      * (must stay >= 0; negative = past-dated -> HW flushes at wire rate). To
//      * catch every past-date we read the PHC on each bulk and keep the worst
//      * (min) lead plus a negative-lead count, then emit a summary ~every 10ms.
//      * regress = bulks whose head landed before the previous bulk's tail (a
//      * non-monotonic launch grid). */
//     {
//       static uint64_t probe_next_tsc;
//       static uint64_t prev_tail_ptp[MTL_SESSION_PORT_MAX];
//       static uint64_t regress_cnt[MTL_SESSION_PORT_MAX];
//       static int64_t regress_max[MTL_SESSION_PORT_MAX];
//       static int64_t win_min_lead[MTL_SESSION_PORT_MAX];
//       static uint64_t win_neg_cnt[MTL_SESSION_PORT_MAX];
//       static uint64_t win_bulks[MTL_SESSION_PORT_MAX];
//       static int prev_pastdate[MTL_SESSION_PORT_MAX];
//       static uint64_t prev_submit_tsc[MTL_SESSION_PORT_MAX];
//       uint64_t head_ptp = st_tx_mbuf_get_ptp(pkts[0]);
//       uint64_t tail_ptp = st_tx_mbuf_get_ptp(pkts[valid_bulk - 1]);
//       uint64_t now_tsc = mt_get_tsc(impl);
//       struct timespec phc;
//       int64_t lead = 0;
//       if (rte_eth_timesync_read_time(mt_port_id(impl, port), &phc) == 0) {
//         uint64_t phc_now = (uint64_t)phc.tv_sec * NS_PER_S + (uint64_t)phc.tv_nsec;
//         lead = (int64_t)(head_ptp - phc_now);
//         if (win_bulks[s_port] == 0 || lead < win_min_lead[s_port])
//           win_min_lead[s_port] = lead;
//         if (lead < 0) win_neg_cnt[s_port]++;
//         /* edge trigger: capture the exact bulk where lead crosses zero in
//          * either direction. gap_since = wall time since the previous submit
//          * (a large value = the tx tasklet was starved). ring/inflight show
//          * where the backlog sits when the past-date window opens. */
//         int now_pastdate = (lead < 0);
//         if (now_pastdate != prev_pastdate[s_port]) {
//           int64_t gap_since =
//               prev_submit_tsc[s_port] ? (int64_t)(now_tsc - prev_submit_tsc[s_port]) : 0;
//           info("%s(%d), TXPP P-edge: %s lead=%" PRId64 " ns gap_since_submit=%" PRId64
//                " ns ring=%u inflight=%u\n",
//                __func__, s->idx, now_pastdate ? "ENTER-PASTDATE" : "EXIT-PASTDATE", lead,
//                gap_since, rte_ring_count(ring), s->trs_inflight_num[s_port]);
//           prev_pastdate[s_port] = now_pastdate;
//         }
//       }
//       prev_submit_tsc[s_port] = now_tsc;
//       win_bulks[s_port]++;
//       if (prev_tail_ptp[s_port] && head_ptp < prev_tail_ptp[s_port]) {
//         int64_t regress = (int64_t)(prev_tail_ptp[s_port] - head_ptp);
//         regress_cnt[s_port]++;
//         if (regress > regress_max[s_port]) regress_max[s_port] = regress;
//       }
//       prev_tail_ptp[s_port] = tail_ptp;
//       if (now_tsc >= probe_next_tsc) {
//         info("%s(%d), TXPP P-trs: head_ptp=%" PRIu64 " lead=%" PRId64
//              " ns min_lead=%" PRId64 " ns neg=%" PRIu64 "/%" PRIu64
//              " regress_cnt=%" PRIu64 " regress_max=%" PRId64 " ns%s\n",
//              __func__, s->idx, head_ptp, lead, win_min_lead[s_port], win_neg_cnt[s_port],
//              win_bulks[s_port], regress_cnt[s_port], regress_max[s_port],
//              win_neg_cnt[s_port] ? " PASTDATE" : "");
//         probe_next_tsc = now_tsc + rte_get_tsc_hz() / 100; /* ~10ms */
//         regress_max[s_port] = 0;
//         win_min_lead[s_port] = 0;
//         win_neg_cnt[s_port] = 0;
//         win_bulks[s_port] = 0;
//       }
//     }
// #endif

    tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

    if (tx < valid_bulk) {
      unsigned int remaining = valid_bulk - tx;

      s->trs_inflight_num[s_port] = remaining;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
    return MTL_TASKLET_ALL_DONE;
  } else {
    return MTL_TASKLET_HAS_PENDING;
  }
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
