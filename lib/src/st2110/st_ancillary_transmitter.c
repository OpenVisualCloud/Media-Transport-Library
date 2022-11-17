/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_ancillary_transmitter.h"

#include "../mt_log.h"
#include "st_err.h"
#include "st_tx_ancillary_session.h"

static int st_ancillary_trs_tasklet_start(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  int idx = trs->idx;
  struct st_tx_ancillary_sessions_mgr* mgr = trs->mgr;

  rte_atomic32_set(&mgr->transmitter_started, 1);

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int st_ancillary_trs_tasklet_stop(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  struct mtl_main_impl* impl = trs->parnet;
  struct st_tx_ancillary_sessions_mgr* mgr = trs->mgr;
  int idx = trs->idx, port;

  rte_atomic32_set(&mgr->transmitter_started, 0);

  for (port = 0; port < st_num_ports(impl); port++) {
    /* flush all the pkts in the tx ring desc */
    st_dev_flush_tx_queue(impl, port, mgr->queue_id[port], st_get_pad(impl, port));
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
static int st_ancillary_trs_session_tasklet(struct mtl_main_impl* impl,
                                            struct st_ancillary_transmitter_impl* trs,
                                            struct st_tx_ancillary_sessions_mgr* mgr,
                                            enum mtl_port port) {
  struct rte_ring* ring = mgr->ring[port];
  int ret;
  uint16_t n;
  struct rte_mbuf* pkt;

  /* check if any inflight pkts in transmitter */
  pkt = trs->inflight[port];
  if (pkt) {
    n = rte_eth_tx_burst(mgr->port_id[port], mgr->queue_id[port], &pkt, 1);
    if (n >= 1) {
      trs->inflight[port] = NULL;
    } else {
      mgr->stat_trs_ret_code[port] = -STI_TSCTRS_BURST_INFILGHT_FAIL;
      return ST_TASKLET_HAS_PENDING;
    }
    mgr->st40_stat_pkts_burst += n;
  }

  /* try to dequeue */
  for (int i = 0; i < mgr->max_idx; i++) {
    /* try to dequeue */
    ret = rte_ring_sc_dequeue(ring, (void**)&pkt);
    if (ret < 0) {
      mgr->stat_trs_ret_code[port] = -STI_TSCTRS_DEQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE; /* all done */
    }

    n = rte_eth_tx_burst(mgr->port_id[port], mgr->queue_id[port], &pkt, 1);
    mgr->st40_stat_pkts_burst += n;
    if (n < 1) {
      trs->inflight[port] = pkt;
      trs->inflight_cnt[port]++;
      mgr->stat_trs_ret_code[port] = -STI_TSCTRS_BURST_INFILGHT_FAIL;
      return ST_TASKLET_HAS_PENDING;
    }
  }

  mgr->stat_trs_ret_code[port] = 0;
  return ST_TASKLET_HAS_PENDING; /* may has pending pkt in the ring */
}

static int st_ancillary_trs_tasklet_handler(void* priv) {
  struct st_ancillary_transmitter_impl* trs = priv;
  struct mtl_main_impl* impl = trs->parnet;
  struct st_tx_ancillary_sessions_mgr* mgr = trs->mgr;
  int port;
  int pending = ST_TASKLET_ALL_DONE;

  for (port = 0; port < st_num_ports(impl); port++) {
    pending += st_ancillary_trs_session_tasklet(impl, trs, mgr, port);
  }

  return pending;
}

int st_ancillary_transmitter_init(struct mtl_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_tx_ancillary_sessions_mgr* mgr,
                                  struct st_ancillary_transmitter_impl* trs) {
  int idx = sch->idx;
  struct st_sch_tasklet_ops ops;

  trs->parnet = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  rte_atomic32_set(&mgr->transmitter_started, 0);

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "ancillary_transmitter";
  ops.start = st_ancillary_trs_tasklet_start;
  ops.stop = st_ancillary_trs_tasklet_stop;
  ops.handler = st_ancillary_trs_tasklet_handler;

  trs->tasklet = st_sch_register_tasklet(sch, &ops);
  if (!trs->tasklet) {
    err("%s(%d), st_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_ancillary_transmitter_uinit(struct st_ancillary_transmitter_impl* trs) {
  int idx = trs->idx;

  if (trs->tasklet) {
    st_sch_unregister_tasklet(trs->tasklet);
    trs->tasklet = NULL;
  }

  info("%s(%d), succ, inflight %d:%d\n", __func__, idx, trs->inflight_cnt[MTL_PORT_P],
       trs->inflight_cnt[MTL_PORT_R]);
  return 0;
}
