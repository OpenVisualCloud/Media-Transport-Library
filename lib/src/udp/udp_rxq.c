/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "udp_rxq.h"

#include "../mt_log.h"
#include "../mt_stat.h"
#include "udp_main.h"

static inline bool udp_rxq_lcore_mode(struct mudp_rxq* q) {
  return q->lcore_tasklet ? true : false;
}

static void udp_queue_wakeup(struct mudp_rxq* q) {
  mt_pthread_mutex_lock(&q->lcore_wake_mutex);
  mt_pthread_cond_signal(&q->lcore_wake_cond);
  mt_pthread_mutex_unlock(&q->lcore_wake_mutex);
}

static uint16_t udp_rx_handle(struct mudp_rxq* q, struct rte_mbuf** pkts,
                              uint16_t nb_pkts) {
  int idx = q->rxq_id;
  struct rte_mbuf* valid_mbuf[nb_pkts];
  uint16_t valid_mbuf_cnt = 0;
  uint16_t n = 0;

  /* check if valid udp pkt */
  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;

    if (ipv4->next_proto_id == IPPROTO_UDP) {
      valid_mbuf[valid_mbuf_cnt] = pkts[i];
      valid_mbuf_cnt++;
      rte_mbuf_refcnt_update(pkts[i], 1);
    } else { /* invalid pkt */
      warn("%s(%d), not udp pkt %u\n", __func__, idx, ipv4->next_proto_id);
    }
  }

  /* enqueue the valid mbuf */
  if (valid_mbuf_cnt) {
    if (q->rx_ring) {
      n = rte_ring_sp_enqueue_bulk(q->rx_ring, (void**)&valid_mbuf[0], valid_mbuf_cnt,
                                   NULL);
    }
    if (!n) {
      dbg("%s(%d), %u pkts enqueue fail\n", __func__, idx, valid_mbuf_cnt);
      rte_pktmbuf_free_bulk(&valid_mbuf[0], valid_mbuf_cnt);
      q->stat_pkt_rx_enq_fail += valid_mbuf_cnt;
    }
  }

  return n;
}

static int udp_rsq_mbuf_cb(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct mudp_rxq* q = priv;
  udp_rx_handle(q, mbuf, nb);
  return 0;
}

static uint16_t udp_rxq_rx(struct mudp_rxq* q) {
  uint16_t rx_burst = q->rx_burst_pkts;
  struct rte_mbuf* pkts[rx_burst];

  if (q->rsq) return mt_rsq_burst(q->rsq, rx_burst);
  if (q->rss) return mt_rss_burst(q->rss, rx_burst);

  if (!q->rxq) return 0;
  uint16_t rx = mt_dev_rx_burst(q->rxq, pkts, rx_burst);
  if (!rx) return 0; /* no pkt */
  uint16_t n = udp_rx_handle(q, pkts, rx);
  rte_pktmbuf_free_bulk(&pkts[0], rx);
  return n;
}

static int udp_tasklet_handler(void* priv) {
  struct mudp_rxq* q = priv;
  struct mtl_main_impl* impl = q->parent;

  udp_rxq_rx(q);

  unsigned int count = rte_ring_count(q->rx_ring);
  if (count > 0) {
    uint64_t tsc = mt_get_tsc(impl);
    int us = (tsc - q->wake_tsc_last) / NS_PER_US;
    if ((count > q->wake_thresh_count) || (us > q->wake_timeout_us)) {
      udp_queue_wakeup(q);
      q->wake_tsc_last = tsc;
    }
  }
  return 0;
}

static int udp_init_tasklet(struct mtl_main_impl* impl, struct mudp_rxq* q) {
  if (!mt_udp_lcore(impl, q->port)) return 0;

  struct mt_sch_tasklet_ops ops;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = q;
  ops.name = q->name;
  ops.handler = udp_tasklet_handler;

  q->lcore_tasklet = mt_sch_register_tasklet(impl->main_sch, &ops);
  if (!q->lcore_tasklet) {
    err("%s, register lcore tasklet fail\n", __func__);
    MUDP_ERR_RET(EIO);
  }
  /* start mtl to start the sch */
  mtl_start(impl);
  return 0;
}

struct mudp_rxq* mudp_get_rxq(struct mudp_rxq_create* create) {
  struct mtl_main_impl* impl = create->impl;
  enum mtl_port port = create->port;
  uint16_t dst_port = create->dst_port;

  struct mudp_rxq* q = mt_rte_zmalloc_socket(sizeof(*q), mt_socket_id(impl, port));
  if (!q) {
    err("%s(%d,%u), entry malloc fail\n", __func__, port, dst_port);
    return NULL;
  }

  q->parent = impl;
  q->port = port;
  q->dst_port = dst_port;
  q->rx_burst_pkts = 128;
  snprintf(q->name, sizeof(q->name), "mudp_%d_%u", port, dst_port);

  /* lcore related */
  mt_pthread_mutex_init(&q->lcore_wake_mutex, NULL);
#if MT_THREAD_TIMEDWAIT_CLOCK_ID != CLOCK_REALTIME
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, MT_THREAD_TIMEDWAIT_CLOCK_ID);
  mt_pthread_cond_init(&q->lcore_wake_cond, &attr);
#else
  mt_pthread_cond_init(&q->lcore_wake_cond, NULL);
#endif
  q->wake_thresh_count = create->wake_thresh_count;
  q->wake_timeout_us = create->wake_timeout_us;
  q->wake_tsc_last = mt_get_tsc(impl);

  /* create flow */
  uint16_t queue_id;
  struct mt_rx_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.no_ip_flow = true;
  flow.dst_port = dst_port;
  flow.priv = q;
  flow.cb = udp_rsq_mbuf_cb; /* for rss and rsq */

  if (mt_has_rss(impl, port)) {
    q->rss = mt_rss_get(impl, port, &flow);
    if (!q->rss) {
      err("%s(%d,%u), get rss fail\n", __func__, port, dst_port);
      mudp_put_rxq(q);
      return NULL;
    }
    queue_id = mt_rss_queue_id(q->rss);
  } else if (mt_shared_queue(impl, port)) {
    q->rsq = mt_rsq_get(impl, port, &flow);
    if (!q->rsq) {
      err("%s(%d,%u), get rsq fail\n", __func__, port, dst_port);
      mudp_put_rxq(q);
      return NULL;
    }
    queue_id = mt_rsq_queue_id(q->rsq);
  } else {
    q->rxq = mt_dev_get_rx_queue(impl, port, &flow);
    if (!q->rxq) {
      err("%s(%d,%u), get rx queue fail\n", __func__, port, dst_port);
      mudp_put_rxq(q);
      return NULL;
    }
    queue_id = mt_dev_rx_queue_id(q->rxq);
  }
  q->rxq_id = queue_id;

  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, 32, "MUDP%d-RX-P%d-Q%u", port, dst_port, queue_id);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = create->ring_count;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%u), rx ring create fail\n", __func__, port, dst_port);
    mudp_put_rxq(q);
    return NULL;
  }
  q->rx_ring = ring;

  int ret = udp_init_tasklet(impl, q);
  if (ret < 0) {
    err("%s(%d,%u), init tasklet fail %d\n", __func__, port, dst_port, ret);
    mudp_put_rxq(q);
    return NULL;
  }

  info("%s(%d,%u), count %d\n", __func__, port, dst_port, count);
  return q;
}

int mudp_put_rxq(struct mudp_rxq* q) {
  udp_queue_wakeup(q); /* wake up any pending wait */

  if (q->lcore_tasklet) {
    mt_sch_unregister_tasklet(q->lcore_tasklet);
    q->lcore_tasklet = NULL;
  }
  if (q->rxq) {
    mt_dev_put_rx_queue(q->parent, q->rxq);
    q->rxq = NULL;
  }
  if (q->rsq) {
    mt_rsq_put(q->rsq);
    q->rsq = NULL;
  }
  if (q->rss) {
    mt_rss_put(q->rss);
    q->rss = NULL;
  }
  if (q->rx_ring) {
    mt_ring_dequeue_clean(q->rx_ring);
    rte_ring_free(q->rx_ring);
    q->rx_ring = NULL;
  }

  /* lcore related */
  mt_pthread_mutex_destroy(&q->lcore_wake_mutex);
  mt_pthread_cond_destroy(&q->lcore_wake_cond);

  mt_rte_free(q);

  return 0;
}

int mudp_rxq_dump(struct mudp_rxq* q) {
  enum mtl_port port = q->port;
  uint16_t dst_port = q->dst_port;

  if (q->stat_pkt_rx_enq_fail) {
    warn("%s(%d,%u), pkt rx %u enqueue fail\n", __func__, port, dst_port,
         q->stat_pkt_rx_enq_fail);
    q->stat_pkt_rx_enq_fail = 0;
  }
  if (q->stat_timedwait) {
    notice("%s(%d,%u), timedwait %u timeout %u\n", __func__, port, dst_port,
           q->stat_timedwait, q->stat_timedwait_timeout);
    q->stat_timedwait = 0;
    q->stat_timedwait_timeout = 0;
  }

  return 0;
}

uint16_t mudp_rxq_rx(struct mudp_rxq* q) {
  if (udp_rxq_lcore_mode(q))
    return 0;
  else
    return udp_rxq_rx(q);
}

int mudp_rxq_timedwait_lcore(struct mudp_rxq* q, unsigned int us) {
  if (!udp_rxq_lcore_mode(q)) return 0; /* return directly if not lcore mode */

  int ret;

  q->stat_timedwait++;
  mt_pthread_mutex_lock(&q->lcore_wake_mutex);

  struct timespec time;
  clock_gettime(MT_THREAD_TIMEDWAIT_CLOCK_ID, &time);
  uint64_t ns = mt_timespec_to_ns(&time);
  ns += us * NS_PER_US;
  mt_ns_to_timespec(ns, &time);
  ret = mt_pthread_cond_timedwait(&q->lcore_wake_cond, &q->lcore_wake_mutex, &time);
  dbg("%s(%u), timedwait ret %d\n", __func__, q->dst_port, ret);
  mt_pthread_mutex_unlock(&q->lcore_wake_mutex);

  if (ret == ETIMEDOUT) q->stat_timedwait_timeout++;
  return ret;
}

char* mudp_rxq_mode(struct mudp_rxq* q) {
  if (q->rsq) return "shared";
  if (q->rss) return "rss";
  return "dedicated";
}
