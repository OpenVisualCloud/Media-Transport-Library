/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "st_video_transmitter.h"

#include <math.h>

#include "st_err.h"
#include "st_log.h"
#include "st_sch.h"
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
static int video_trs_session_warm_up(struct st_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
                                     enum st_session_port s_port) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  uint64_t cur_tsc, pre_tsc;
  int32_t warm_pkts = pacing->warm_pkts;
  struct rte_mbuf* pads[1];
  int32_t delta_pkts;
  unsigned int tx;

  cur_tsc = st_get_tsc(impl);
  delta_pkts = (cur_tsc - target_tsc) / pacing->trs;
  pre_tsc = cur_tsc;
  warm_pkts -= delta_pkts;
  if (warm_pkts < 0) {
    dbg("%s(%d), mismatch timing with %d\n", __func__, s->idx, warm_pkts);
    s->st20_troffset_mismatch++;
    return 0;
  }

  dbg("%s(%d), send warm_pkts %d\n", __func__, s->idx, warm_pkts);
  pads[0] = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  for (int i = 0; i < warm_pkts; i++) {
    rte_mbuf_refcnt_update(pads[0], 1);
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pads[0], 1);
    if (tx < 1) {
      dbg("%s(%d), warm_pkts fail at %d\n", __func__, s->idx, i);
      s->trs_pad_inflight_num[s_port] += (warm_pkts - i);
      return 0;
    }
    /* re-calculate the delta */
    cur_tsc = st_get_tsc(impl);
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
                              enum st_session_port s_port, struct rte_mbuf** pkts,
                              int bulk, bool use_two) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  int tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pkts[0], bulk);
  int pkt_idx = st_tx_mbuf_get_idx(pkts[0]);
  bool update_nic_burst = false;

  s->st20_stat_pkts_burst += tx;
  s->pri_nic_burst_cnt++;
  if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
    update_nic_burst = true;
  }
  if (update_nic_burst) {
    rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
    s->pri_nic_burst_cnt = 0;
  }
  if (tx < bulk) {
    unsigned int i;
    unsigned int remaining = bulk - tx;

    s->pri_nic_inflight_cnt++;
    if (update_nic_burst) {
      rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
      s->pri_nic_inflight_cnt = 0;
    }

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
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    if (tx < 1) s->trs_pad_inflight_num[s_port]++;
  }

  return 0;
}

static int video_trs_rl_tasklet(struct st_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum st_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx;
  unsigned int n, tx;
  uint32_t pkt_idx = 0;

  /* check if any inflight pkts in transmitter inflight 2 */
  if (s->trs_inflight_num2[s_port] > 0) {
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->trs_inflight2[s_port][s->trs_inflight_idx2[s_port]],
                          s->trs_inflight_num2[s_port]);
    s->trs_inflight_num2[s_port] -= tx;
    s->trs_inflight_idx2[s_port] += tx;
    s->st20_stat_pkts_burst += tx;
    return (tx > 0) ? 0 : -STI_RLTRS_BURST_INFILGHT2_FAIL;
  }

  /* check if it's pending on the first pkt */
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    uint64_t cur_tsc = st_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        return -STI_RLTRS_TARGET_TSC_NOT_REACH;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    video_trs_session_warm_up(impl, s, s_port);
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any padding inflight pkts in transmitter */
  if (s->trs_pad_inflight_num[s_port] > 0) {
    dbg("%s(%d), inflight padding pkts %d\n", __func__, idx,
        s->trs_pad_inflight_num[s_port]);
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    s->trs_pad_inflight_num[s_port] -= tx;
    return (tx > 0) ? 0 : -STI_RLTRS_BURST_PAD_INFILGHT_FAIL;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                          s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->st20_stat_pkts_burst += tx;
    return (tx > 0) ? 0 : -STI_RLTRS_BURST_INFILGHT_FAIL;
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) return -STI_RLTRS_DEQUEUE_FAIL;

  int valid_bulk = bulk;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == 0 || pkt_idx >= s->st20_total_pkts) {
      valid_bulk = i;
      break;
    }
  }
  dbg("%s(%d), pkt_idx %" PRIu64 " ts %" PRIu64 "\n", __func__, idx, pkt_idx,
      st_tx_mbuf_get_time_stamp(pkts[0]));

  if (unlikely(pkt_idx >= s->st20_total_pkts)) {
    video_burst_packet(s, s_port, pkts, valid_bulk, false);
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->st20_stat_pkts_burst_dummy += bulk - valid_bulk;
    dbg("%s(%d), pkt_idx %" PRIu64 " ts %" PRIu64 "\n", __func__, idx, pkt_idx,
        st_tx_mbuf_get_time_stamp(pkts[0]));
    return -STI_RLTRS_BURST_HAS_DUMMY;
  }

  if (unlikely(!pkt_idx)) {
    uint64_t cur_tsc = st_get_tsc(impl);
    if (valid_bulk != 0) {
      video_burst_packet(s, s_port, pkts, valid_bulk, true);
    }
    uint64_t target_tsc = st_tx_mbuf_get_time_stamp(pkts[valid_bulk]);
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
        return -STI_RLTRS_1ST_PKT_TSC;
      } else {
        err("%s(%d), invalid tsc for first pkt cur %" PRIu64 " target %" PRIu64 "\n",
            __func__, idx, cur_tsc, target_tsc);
      }
    } else {
      video_trs_session_warm_up(impl, s, s_port);
    }
  }

  int pos = (valid_bulk == bulk) ? 0 : valid_bulk;

  video_burst_packet(s, s_port, &pkts[pos], bulk - pos, false);

  return 1;
}

int video_trs_tsc_tasklet(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                          enum st_session_port s_port) {
  unsigned int bulk = 1; /* only one packet now for tsc */
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx;
  unsigned int n, tx;
  uint64_t target_tsc, cur_tsc;

  /* check if it's pending on the tsc */
  target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    cur_tsc = st_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        return -STI_TSCTRS_TARGET_TSC_NOT_REACH;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                          s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->st20_stat_pkts_burst += tx;
    return -STI_TSCTRS_BURST_INFILGHT_FAIL;
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) return -STI_TSCTRS_DEQUEUE_FAIL;

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx >= s->st20_total_pkts) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx >= s->st20_total_pkts)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->st20_stat_pkts_burst_dummy += bulk - valid_bulk;
    return -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  cur_tsc = st_get_tsc(impl);
  target_tsc = st_tx_mbuf_get_time_stamp(pkts[0]);
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
      return -STI_TSCTRS_TARGET_TSC_NOT_REACH;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pkts[0], valid_bulk);
  s->st20_stat_pkts_burst += tx;
  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return 0;
}

static int video_trs_tasklet_handler(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  struct st_main_impl* impl = trs->parnet;
  struct st_tx_video_sessions_mgr* mgr = trs->mgr;
  struct st_tx_video_session_impl* s;
  int sidx, s_port, ret;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    for (s_port = 0; s_port < s->ops.num_port; s_port++) {
      if (ST21_TX_PACING_WAY_TSC != impl->tx_pacing_way) {
        ret = video_trs_rl_tasklet(impl, s, s_port);
        /*
         * Try to burst pkts again for the performance, in this way nic tx get double
         * bulk since rte_eth_tx_burst is the cirtial path
         */
        if (ret > 0) video_trs_rl_tasklet(impl, s, s_port);
      } else {
        ret = video_trs_tsc_tasklet(impl, s, s_port);
      }
      s->stat_trs_ret_code[s_port] = ret;
    }
    tx_video_session_put(mgr, sidx);
  }

  return 0;
}

int st_video_transmitter_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs) {
  int ret, idx = sch->idx;
  struct st_sch_tasklet_ops ops;

  trs->parnet = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "video_transmitter";
  ops.start = video_trs_tasklet_start;
  ops.stop = video_trs_tasklet_stop;
  ops.handler = video_trs_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    info("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs) {
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
