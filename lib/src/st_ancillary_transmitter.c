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

#include "st_ancillary_transmitter.h"

#include "st_dev.h"
#include "st_log.h"
#include "st_sch.h"
#include "st_tx_ancillary_session.h"
#include "st_util.h"

static int st_ancillary_trs_tasklet_start(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int st_ancillary_trs_tasklet_stop(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  struct st_main_impl* impl = trs->parnet;
  struct st_tx_ancillary_sessions_mgr* mgr = trs->mgr;
  int idx = trs->idx, port;

  for (port = 0; port < st_num_ports(impl); port++) {
    /* flush all the pkts in the tx ring desc */
    st_dev_flush_tx_queue(impl, port, mgr->queue_id[port]);
    st_ring_dequeue_clean(mgr->ring[port]);
    info("%s(%d), port %d, remaining entries %d\n", __func__, idx, port,
         rte_ring_count(mgr->ring[port]));

    if (trs->inflight[port]) {
      rte_pktmbuf_free(trs->inflight[port]);
      trs->inflight[port] = NULL;
    }
  }
  mgr->st40_stat_pkts_burst = 0;

  return 0;
}

/* pacing handled by session itself */
static int st_ancillary_trs_session_tasklet(struct st_main_impl* impl,
                                            struct st_ancillary_transmitter_impl* trs,
                                            struct st_tx_ancillary_sessions_mgr* mgr,
                                            enum st_port port) {
  struct rte_ring* ring = mgr->ring[port];
  int ret;
  uint16_t n;
  struct rte_mbuf* pkt;

  /* check if any inflight pkts in transmitter */
  pkt = trs->inflight[port];
  if (pkt) {
    n = rte_eth_tx_burst(mgr->port_id[port], mgr->queue_id[port], &pkt, 1);
    if (n >= 1) trs->inflight[port] = NULL;
    mgr->st40_stat_pkts_burst += n;
    return 0;
  }

  /* try to dequeue */
  ret = rte_ring_sc_dequeue(ring, (void**)&pkt);
  if (ret < 0) return 0;

  n = rte_eth_tx_burst(mgr->port_id[port], mgr->queue_id[port], &pkt, 1);
  mgr->st40_stat_pkts_burst += n;
  if (n < 1) {
    trs->inflight[port] = pkt;
    trs->inflight_cnt[port]++;
  }

  return 0;
}

static int st_ancillary_trs_tasklet_handler(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  struct st_main_impl* impl = trs->parnet;
  struct st_tx_ancillary_sessions_mgr* mgr = trs->mgr;
  int port;

  for (port = 0; port < st_num_ports(impl); port++) {
    st_ancillary_trs_session_tasklet(impl, trs, mgr, port);
  }

  return 0;
}

int st_ancillary_transmitter_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_tx_ancillary_sessions_mgr* mgr,
                                  struct st_ancillary_transmitter_impl* trs) {
  int ret, idx = sch->idx;
  struct st_sch_tasklet_ops ops;

  trs->parnet = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "ancillary_transmitter";
  ops.start = st_ancillary_trs_tasklet_start;
  ops.stop = st_ancillary_trs_tasklet_stop;
  ops.handler = st_ancillary_trs_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    info("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_ancillary_transmitter_uinit(struct st_ancillary_transmitter_impl* trs) {
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
