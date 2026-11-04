/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "session/st_rx_mgr_queue_harness.h"

#include <stdlib.h>
#include <string.h>

#include "common/ut_common.h"
#include "datapath/mt_queue.h"
#include "st2110/st_rx_mgr_queue.h"

#define UT_MQ_MAX_PKTS (128)

static struct {
  struct st_rx_mgr_queue q;
  struct mt_rxq_entry entry;
  _Atomic uint32_t map[UINT16_MAX + 1];
  struct rte_mbuf* pending[UT_MQ_MAX_PKTS];
  uint16_t pending_n;
  uint32_t seq;
  int64_t busy_key;
  int busy_times;
  uint32_t got[UT_MQ_MAX_PKTS];
  uint32_t got_seq[UT_MQ_MAX_PKTS];
  int got_n;
} ut_mq;

/* zeroed: a regression that reaches the device fails the call, not the process */
static struct mtl_main_impl* ut_mq_impl;

static uint16_t ut_mq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** pkts,
                            const uint16_t nb_pkts) {
  uint16_t n = RTE_MIN(nb_pkts, ut_mq.pending_n);

  (void)entry;
  memcpy(pkts, ut_mq.pending, n * sizeof(*pkts));
  memmove(ut_mq.pending, ut_mq.pending + n, (ut_mq.pending_n - n) * sizeof(*pkts));
  ut_mq.pending_n -= n;
  return n;
}

static int ut_mq_dispatch(void* priv, enum mtl_port port, uint32_t key,
                          struct rte_mbuf* pkt) {
  (void)priv;
  (void)port;
  if ((int64_t)key == ut_mq.busy_key && ut_mq.busy_times) {
    if (ut_mq.busy_times > 0) ut_mq.busy_times--;
    return -EBUSY;
  }
  if (ut_mq.got_n < UT_MQ_MAX_PKTS) {
    ut_mq.got[ut_mq.got_n] = key;
    memcpy(&ut_mq.got_seq[ut_mq.got_n],
           rte_pktmbuf_mtod_offset(pkt, uint8_t*, sizeof(struct mt_udp_hdr)),
           sizeof(uint32_t));
    ut_mq.got_n++;
  }
  return 0;
}

int ut_mq_init(void) {
  int ret = ut_eal_init();

  if (ret < 0) return ret;
  if (!ut_mq_impl) ut_mq_impl = calloc(1, sizeof(*ut_mq_impl));
  return ut_mq_impl ? 0 : -ENOMEM;
}

void ut_mq_reset(void) {
  memset(&ut_mq, 0, sizeof(ut_mq));
  ut_mq.busy_key = -1;
  ut_mq.entry.burst = ut_mq_burst;
  ut_mq.q.rxq[MTL_PORT_P] = &ut_mq.entry;
  ut_mq.q.map[MTL_PORT_P] = ut_mq.map;
}

void ut_mq_teardown(void) {
  for (uint16_t i = 0; i < ut_mq.pending_n; i++) rte_pktmbuf_free(ut_mq.pending[i]);
  ut_mq.pending_n = 0;
  /* the fake queue and map are not from the device: only the kept packets to free */
  ut_mq.q.rxq[MTL_PORT_P] = NULL;
  ut_mq.q.map[MTL_PORT_P] = NULL;
  st_rx_mgr_queue_uinit(&ut_mq.q);
}

void ut_mq_map(uint16_t udp_port, uint32_t key) {
  ut_mq.map[udp_port] = key + 1;
}

static int ut_mq_feed_len(uint16_t udp_port, uint16_t len) {
  struct rte_mbuf* m;
  uint8_t* data;

  if (ut_mq.pending_n >= UT_MQ_MAX_PKTS) return -ENOSPC;
  m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return -ENOMEM;
  data = (uint8_t*)rte_pktmbuf_append(m, len);
  if (!data) {
    rte_pktmbuf_free(m);
    return -ENOMEM;
  }
  memset(data, 0, len);
  if (len >= sizeof(struct mt_udp_hdr) + sizeof(uint32_t)) {
    struct mt_udp_hdr* hdr = (struct mt_udp_hdr*)data;
    uint32_t seq = ut_mq.seq++;

    hdr->udp.dst_port = htons(udp_port);
    memcpy(data + sizeof(*hdr), &seq, sizeof(seq));
  }
  ut_mq.pending[ut_mq.pending_n++] = m;
  return 0;
}

int ut_mq_feed(uint16_t udp_port) {
  return ut_mq_feed_len(udp_port, sizeof(struct mt_udp_hdr) + 16);
}

int ut_mq_feed_short(void) {
  return ut_mq_feed_len(0, sizeof(struct mt_udp_hdr) - 1);
}

void ut_mq_set_busy(int64_t key, int times) {
  ut_mq.busy_key = key;
  ut_mq.busy_times = times;
}

int ut_mq_tasklet(uint16_t burst) {
  return st_rx_mgr_queue_tasklet(&ut_mq.q, MTL_PORT_P, burst, ut_mq_dispatch, NULL);
}

int ut_mq_got_n(void) {
  return ut_mq.got_n;
}

uint32_t ut_mq_got(int i) {
  return ut_mq.got[i];
}

uint32_t ut_mq_got_seq(int i) {
  return ut_mq.got_seq[i];
}

int ut_mq_pending(void) {
  return ut_mq.pending_n;
}

uint16_t ut_mq_deferred(void) {
  return ut_mq.q.defer_n[MTL_PORT_P];
}

uint64_t ut_mq_dropped(void) {
  return ut_mq.q.stat_defer_drop;
}

unsigned int ut_mq_mbufs_in_use(void) {
  return rte_mempool_in_use_count(ut_pool());
}

int ut_mq_add(uint16_t udp_port, uint32_t key) {
  struct mt_rxq_flow flow;
  struct mt_rx_flow_rsp* rsp = NULL;

  memset(&flow, 0, sizeof(flow));
  flow.dst_port = udp_port;
  /* a taken map entry stops the add before the flow rule; on a free entry the flow rule
   * fails, because the impl of the harness has no port */
  return st_rx_mgr_queue_add(ut_mq_impl, &ut_mq.q, MTL_PORT_P, &flow, key, &rsp);
}

uint32_t ut_mq_map_get(uint16_t udp_port) {
  return ut_mq.map[udp_port];
}
