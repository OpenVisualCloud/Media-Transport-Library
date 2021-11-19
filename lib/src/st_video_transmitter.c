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
                                     struct st_video_transmitter_impl* trs,
                                     struct st_tx_video_session_impl* s,
                                     enum st_session_port s_port) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  uint64_t cur_tsc = st_get_tsc(impl);
  int32_t warm_pkts = pacing->warm_pkts;
  struct rte_mbuf* pads[warm_pkts];
  int32_t delta_pkts;
  unsigned int tx;

  delta_pkts = (cur_tsc - target_tsc) / pacing->trs;
  warm_pkts -= delta_pkts;
  if (warm_pkts < 0) {
    dbg("%s(%d), mismatch timing with %d\n", __func__, s->idx, warm_pkts);
    s->st20_troffset_mismatch++;
    return 0;
  }

  for (int i = 0; i < warm_pkts; i++) pads[i] = s->pad[s_port];
  rte_mbuf_refcnt_update(s->pad[s_port], warm_pkts);

  dbg("%s(%d), send warm_pkts %d\n", __func__, s->idx, warm_pkts);
  tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pads[0], warm_pkts);
  if (tx < warm_pkts) s->trs_pad_inflight_num[s_port] += (warm_pkts - tx);

  return 0;
}

static void video_burst_packet(struct st_tx_video_session_impl* s,
                               enum st_session_port s_port, struct rte_mbuf** pkts,
                               int bulk, bool use_two) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  int tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pkts[0], bulk);
  s->st20_stat_pkts_burst += tx;
  int pkt_idx = st_mbuf_get_idx(pkts[0]);
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
  if (fmodf(pkt_idx + 1, pacing->pad_interval) < bulk) {
    rte_mbuf_refcnt_update(s->pad[s_port], 1);
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &s->pad[s_port], 1);
    if (tx < 1) s->trs_pad_inflight_num[s_port]++;
  }
}

static int video_trs_rl_tasklet(struct st_main_impl* impl,
                                struct st_video_transmitter_impl* trs,
                                struct st_tx_video_session_impl* s,
                                enum st_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx;
  unsigned int n, tx;
  uint64_t pkt_idx;

  /* check if any inflight pkts in transmitter inflight 2 */
  if (s->trs_inflight_num2[s_port] > 0) {
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->trs_inflight2[s_port][s->trs_inflight_idx2[s_port]],
                          s->trs_inflight_num2[s_port]);
    s->trs_inflight_num2[s_port] -= tx;
    s->trs_inflight_idx2[s_port] += tx;
    s->st20_stat_pkts_burst += tx;
    return 0;
  }

  /* check if it's pending on the first pkt */
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    uint64_t cur_tsc = st_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta < NS_PER_S)) {
        return 0;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    video_trs_session_warm_up(impl, trs, s, s_port);
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any padding inflight pkts in transmitter */
  if (s->trs_pad_inflight_num[s_port] > 0) {
    dbg("%s(%d), inflight padding pkts %d\n", __func__, idx,
        s->trs_pad_inflight_num[s_port]);
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &s->pad[s_port], 1);
    s->trs_pad_inflight_num[s_port] -= tx;
    return 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port],
                          &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                          s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    s->st20_stat_pkts_burst += tx;
    return 0;
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) return 0;

  int valid_bulk = bulk;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_mbuf_get_idx(pkts[i]);
    if (pkt_idx == 0) {
      valid_bulk = i;
      break;
    }
  }
  dbg("%s(%d), pkt_idx %" PRIu64 " ts %" PRIu64 "\n", __func__, idx, pkt_idx,
      st_mbuf_get_time_stamp(pkts[0]));

  if (unlikely(!pkt_idx)) {
    uint64_t cur_tsc = st_get_tsc(impl);
    if (valid_bulk != 0) {
      video_burst_packet(s, s_port, pkts, valid_bulk, true);
    }
    uint64_t target_tsc = st_mbuf_get_time_stamp(pkts[valid_bulk]);
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
        return 0;
      } else {
        err("%s(%d), invalid tsc for first pkt cur %" PRIu64 " target %" PRIu64 "\n",
            __func__, idx, cur_tsc, target_tsc);
      }
    } else {
      video_trs_session_warm_up(impl, trs, s, s_port);
    }
  }
  int pos = (valid_bulk == bulk) ? 0 : valid_bulk;

  video_burst_packet(s, s_port, &pkts[pos], bulk - pos, false);

  return 0;
}

static int video_trs_tsc_tasklet(struct st_main_impl* impl,
                                 struct st_video_transmitter_impl* trs,
                                 struct st_tx_video_session_impl* s,
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
        return 0;
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
    return 0;
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[bulk];
  n = rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) return 0;

  cur_tsc = st_get_tsc(impl);
  target_tsc = st_mbuf_get_time_stamp(pkts[0]);
  if (cur_tsc < target_tsc) {
    unsigned int i;
    uint64_t delta = target_tsc - cur_tsc;

    if (likely(delta < NS_PER_S)) {
      s->trs_target_tsc[s_port] = target_tsc;
      /* save it on inflight */
      s->trs_inflight_num[s_port] = bulk;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < bulk; i++) s->trs_inflight[s_port][i] = pkts[i];
      return 0;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  tx = rte_eth_tx_burst(s->port_id[s_port], s->queue_id[s_port], &pkts[0], bulk);
  s->st20_stat_pkts_burst += tx;
  if (tx < bulk) {
    unsigned int i;
    unsigned int remaining = bulk - tx;

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
  int i, s_port;

  for (i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    if (!mgr->active[i]) continue;
    s = &mgr->sessions[i];
    for (s_port = 0; s_port < s->ops.num_port; s_port++) {
      if (ST21_TX_PACING_WAY_RL == impl->tx_pacing_way)
        video_trs_rl_tasklet(impl, trs, s, s_port);
      else
        video_trs_tsc_tasklet(impl, trs, s, s_port);
    }
  }

  return 0;
}

int st_video_transmitter_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs) {
  int ret, idx = sch->idx;
  struct st_sch_tasklet_ops ops;

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

  trs->parnet = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs) {
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
