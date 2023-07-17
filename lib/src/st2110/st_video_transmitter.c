/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_video_transmitter.h"

#include <math.h>

#include "../mt_log.h"
#include "../mt_queue.h"
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

/* warm start for the first packet */
static int video_trs_rl_warm_up(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  uint64_t cur_tsc, pre_tsc;
  int32_t warm_pkts = pacing->warm_pkts;
  struct rte_mbuf* pads[1];
  int32_t delta_pkts;
  unsigned int tx;

  cur_tsc = mt_get_tsc(impl);
  delta_pkts = (cur_tsc - target_tsc) / pacing->trs;
  pre_tsc = cur_tsc;
  warm_pkts -= delta_pkts;
  if (warm_pkts < 0) {
    dbg("%s(%d), mismatch timing with %d\n", __func__, s->idx, warm_pkts);
    s->stat_trans_troffset_mismatch++;
    return 0;
  }

  dbg("%s(%d), send warm_pkts %d\n", __func__, s->idx, warm_pkts);
  pads[0] = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  for (int i = 0; i < warm_pkts; i++) {
    rte_mbuf_refcnt_update(pads[0], 1);
    tx = mt_txq_burst(s->queue[s_port], &pads[0], 1);
    if (tx < 1) {
      dbg("%s(%d), warm_pkts fail at %d\n", __func__, s->idx, i);
      s->trs_pad_inflight_num[s_port] += (warm_pkts - i);
      return 0;
    }
    /* re-calculate the delta */
    cur_tsc = mt_get_tsc(impl);
    delta_pkts = (cur_tsc - pre_tsc) / pacing->trs;
    pre_tsc = cur_tsc;
    if (delta_pkts > i) {
      warm_pkts -= (delta_pkts - i);
      dbg("%s(%d), mismatch delta_pkts %d at %d\n", __func__, s->idx, delta_pkts, i);
    }
  }

  return 0;
}

static int video_burst_packet(struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port, struct rte_mbuf** pkts,
                              int bulk, bool use_two) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  int tx = mt_txq_burst(s->queue[s_port], &pkts[0], bulk);
  int pkt_idx = st_tx_mbuf_get_idx(pkts[0]);

  if (tx && s->rtcp_tx[s_port]) {
    mt_rtcp_tx_buffer_rtp_packets(s->rtcp_tx[s_port], pkts, tx);
  }

  s->stat_pkts_burst += tx;
  s->pri_nic_burst_cnt++;
  if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
    rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
    s->pri_nic_burst_cnt = 0;
    rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
    s->pri_nic_inflight_cnt = 0;
  }
  if (tx < bulk) {
    unsigned int i;
    unsigned int remaining = bulk - tx;

    s->pri_nic_inflight_cnt++;

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
    tx = mt_txq_burst(s->queue[s_port], &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
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
    tx = mt_txq_burst(s->queue[s_port],
                      &s->trs_inflight2[s_port][s->trs_inflight_idx2[s_port]],
                      s->trs_inflight_num2[s_port]);
    s->trs_inflight_num2[s_port] -= tx;
    s->trs_inflight_idx2[s_port] += tx;
    s->stat_pkts_burst += tx;
    if (tx > 0) {
      return MT_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT2_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* check if it's pending on the first pkt */
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        *ret_status = -STI_RLTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    video_trs_rl_warm_up(impl, s, s_port);
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any padding inflight pkts in transmitter */
  if (s->trs_pad_inflight_num[s_port] > 0) {
    dbg("%s(%d), inflight padding pkts %d\n", __func__, idx,
        s->trs_pad_inflight_num[s_port]);
    tx = mt_txq_burst(s->queue[s_port], &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    s->trs_pad_inflight_num[s_port] -= tx;
    if (tx > 0) {
      return MT_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = mt_txq_burst(s->queue[s_port],
                      &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                      s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->stat_pkts_burst += tx;
    if (tx > 0) {
      return MT_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    *ret_status = -STI_RLTRS_DEQUEUE_FAIL;
    return MT_TASKLET_ALL_DONE;
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
    video_burst_packet(s, s_port, pkts, valid_bulk, false);
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    dbg("%s(%d), pkt_idx %" PRIu64 " ts %" PRIu64 "\n", __func__, idx, pkt_idx,
        st_tx_mbuf_get_tsc(pkts[0]));
    *ret_status = -STI_RLTRS_BURST_HAS_DUMMY;
    return MT_TASKLET_HAS_PENDING;
  }

  if (unlikely(!pkt_idx)) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    if (valid_bulk != 0) {
      video_burst_packet(s, s_port, pkts, valid_bulk, true);
    }
    uint64_t target_tsc = st_tx_mbuf_get_tsc(pkts[valid_bulk]);
    dbg("%s(%d), first pkt, ts cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
        cur_tsc, target_tsc);
    if (likely(cur_tsc < target_tsc || s->trs_inflight_num2[s_port])) {
      unsigned int i;
      uint64_t delta = target_tsc - cur_tsc;

      if (likely(delta < NS_PER_S || s->trs_inflight_num2[s_port])) {
        s->trs_target_tsc[s_port] = target_tsc;
        /* save it on inflight */
        s->trs_inflight_num[s_port] = bulk - valid_bulk;
        s->trs_inflight_idx[s_port] = 0;
        s->trs_inflight_cnt[s_port]++;
        for (i = 0; i < bulk - valid_bulk; i++)
          s->trs_inflight[s_port][i] = pkts[i + valid_bulk];
        *ret_status = -STI_RLTRS_1ST_PKT_TSC;
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc for first pkt cur %" PRIu64 " target %" PRIu64 "\n",
            __func__, idx, cur_tsc, target_tsc);
      }
    } else {
      video_trs_rl_warm_up(impl, s, s_port);
    }
  }

  int pos = (valid_bulk == bulk) ? 0 : valid_bulk;

  video_burst_packet(s, s_port, &pkts[pos], bulk - pos, false);

  *ret_status = 1;
  return MT_TASKLET_HAS_PENDING;
}

static int video_trs_rl_tasklet(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port) {
  int pending = MT_TASKLET_ALL_DONE;
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
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = mt_txq_burst(s->queue[s_port],
                      &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                      s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->stat_pkts_burst += tx;
    if (tx > 0) {
      return MT_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MT_TASKLET_ALL_DONE;
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

  s->pri_nic_burst_cnt++;
  if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
    dbg("%s, pri_nic_burst_cnt %d pri_nic_inflight_cnt %d\n", __func__,
        s->pri_nic_burst_cnt, s->pri_nic_inflight_cnt);
    rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
    s->pri_nic_burst_cnt = 0;
    rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
    s->pri_nic_inflight_cnt = 0;
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
        s->pri_nic_inflight_cnt++;
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
  }

  tx = mt_txq_burst(s->queue[s_port], &pkts[0], valid_bulk);
  s->stat_pkts_burst += tx;

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MT_TASKLET_HAS_PENDING;
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
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_ptp, target_ptp);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = mt_txq_burst(s->queue[s_port],
                      &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                      s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->stat_pkts_burst += tx;
    if (tx > 0) {
      return MT_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MT_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx;
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

  s->pri_nic_burst_cnt++;
  if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
    dbg("%s, pri_nic_burst_cnt %d pri_nic_inflight_cnt %d\n", __func__,
        s->pri_nic_burst_cnt, s->pri_nic_inflight_cnt);
    rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
    s->pri_nic_burst_cnt = 0;
    rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
    s->pri_nic_inflight_cnt = 0;
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
      s->pri_nic_inflight_cnt++;
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
      return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                              : MT_TASKLET_ALL_DONE;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_ptp, target_ptp);
    }
  }

  tx = mt_txq_burst(s->queue[s_port], &pkts[0], valid_bulk);
  s->stat_pkts_burst += tx;

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MT_TASKLET_HAS_PENDING;
}

static int video_trs_tasklet_handler(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  struct mtl_main_impl* impl = trs->parent;
  struct st_tx_video_sessions_mgr* mgr = trs->mgr;
  struct st_tx_video_session_impl* s;
  int sidx, s_port;
  int pending = MT_TASKLET_ALL_DONE;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    for (s_port = 0; s_port < s->ops.num_port; s_port++) {
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

int st_video_transmitter_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs) {
  int idx = sch->idx;
  struct mt_sch_tasklet_ops ops;

  trs->parent = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "video_transmitter";
  ops.start = video_trs_tasklet_start;
  ops.stop = video_trs_tasklet_stop;
  ops.handler = video_trs_tasklet_handler;

  trs->tasklet = mt_sch_register_tasklet(sch, &ops);
  if (!trs->tasklet) {
    err("%s(%d), mt_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs) {
  int idx = trs->idx;

  if (trs->tasklet) {
    mt_sch_unregister_tasklet(trs->tasklet);
    trs->tasklet = NULL;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
