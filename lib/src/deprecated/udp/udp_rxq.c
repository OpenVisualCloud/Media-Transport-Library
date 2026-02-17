/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "udp_rxq.h"

#include "../../dev/mt_dev.h"
#include "../../mt_log.h"
#include "../../mt_stat.h"
#include "udp_main.h"

/* queue implementation */

static inline void urq_mgr_lock(struct mudp_rxq_mgr* mgr) {
  mt_pthread_mutex_lock(&mgr->mutex);
}

static inline void urq_mgr_unlock(struct mudp_rxq_mgr* mgr) {
  mt_pthread_mutex_unlock(&mgr->mutex);
}

static inline void urq_lock(struct mur_queue* q) {
  mt_pthread_mutex_lock(&q->mutex);
}

/* return true if try lock succ */
static inline bool urq_try_lock(struct mur_queue* q) {
  int ret = mt_pthread_mutex_try_lock(&q->mutex);
  return ret == 0 ? true : false;
}

static inline void urq_unlock(struct mur_queue* q) {
  mt_pthread_mutex_unlock(&q->mutex);
}

static uint16_t urq_rx_handle(struct mur_queue* q, struct rte_mbuf** pkts,
                              uint16_t nb_pkts) {
  uint16_t idx = q->rxq_id;
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
      warn("%s(%u), not udp pkt %u\n", __func__, idx, ipv4->next_proto_id);
      // mt_mbuf_dump(q->port, i, "urq", pkts[i]);
    }
  }

  if (!valid_mbuf_cnt) return 0;

  int clients = q->clients;

  if (clients < 1) { /* should never happen */
    err("%s(%u), no clients %d attached\n", __func__, idx, clients);
    rte_pktmbuf_free_bulk(&valid_mbuf[0], valid_mbuf_cnt);
    return 0;
  }

  /* enqueue the valid mbuf */
  if (clients == 1) {
    struct mur_client* c = MT_TAILQ_FIRST(&q->client_head);
    c->stat_pkt_rx += valid_mbuf_cnt;
    n = rte_ring_sp_enqueue_bulk(c->ring, (void**)&valid_mbuf[0], valid_mbuf_cnt, NULL);
    if (!n) {
      dbg("%s(%d), %u pkts enqueue fail\n", __func__, idx, valid_mbuf_cnt);
      rte_pktmbuf_free_bulk(&valid_mbuf[0], valid_mbuf_cnt);
      c->stat_pkt_rx_enq_fail += valid_mbuf_cnt;
    }

    return n;
  }

  urq_lock(q);
  struct mur_client* cs[clients];
  cs[0] = MT_TAILQ_FIRST(&q->client_head);
  for (int i = 1; i < clients; i++) {
    cs[i] = MT_TAILQ_NEXT(cs[i - 1], next);
  }

  int last_c_idx = -1;
  int c_pkts_nb = 0;
  struct rte_mbuf* c_pkts[valid_mbuf_cnt];

  for (uint16_t i = 0; i < valid_mbuf_cnt; i++) {
    struct rte_mbuf* mbuf = valid_mbuf[i];
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(mbuf, struct mt_udp_hdr*);
    /* hash with ip and port of both src and dst */
    uint32_t tuple[3];
    rte_memcpy(tuple, &hdr->ipv4.src_addr, sizeof(tuple));
    uint32_t hash = mt_softrss(tuple, 3);
    int c_idx = hash % clients;

    if (c_idx != last_c_idx) {
      if (c_pkts_nb) { /* push last client */
        struct mur_client* c = cs[last_c_idx];
        c->stat_pkt_rx += c_pkts_nb;
        unsigned int e =
            rte_ring_sp_enqueue_bulk(c->ring, (void**)&c_pkts[0], c_pkts_nb, NULL);
        if (0 == e) { /* enqueue fail */
          rte_pktmbuf_free_bulk(c_pkts, c_pkts_nb);
          c->stat_pkt_rx_enq_fail += c_pkts_nb;
        }
      }
      last_c_idx = c_idx;
      c_pkts_nb = 0;
    }
    c_pkts[c_pkts_nb++] = mbuf;
  }
  if (c_pkts_nb) { /* push last client */
    struct mur_client* c = cs[last_c_idx];
    c->stat_pkt_rx += c_pkts_nb;
    unsigned int e =
        rte_ring_sp_enqueue_bulk(c->ring, (void**)&c_pkts[0], c_pkts_nb, NULL);
    if (0 == e) { /* enqueue fail */
      rte_pktmbuf_free_bulk(c_pkts, c_pkts_nb);
      c->stat_pkt_rx_enq_fail += c_pkts_nb;
    }
  }

  urq_unlock(q);

  /* now with the shared case */
  return n;
}

static uint16_t urq_rx(struct mur_queue* q) {
  uint16_t rx_burst = q->rx_burst_pkts;
  struct rte_mbuf* pkts[rx_burst];

  if (!urq_try_lock(q)) return 0;
  uint16_t rx = mt_rxq_burst(q->rxq, pkts, rx_burst);
  urq_unlock(q);

  uint16_t n = urq_rx_handle(q, pkts, rx);
  rte_pktmbuf_free_bulk(&pkts[0], rx);

  return n;
}

static int urq_mgr_add(struct mudp_rxq_mgr* mgr, struct mur_queue* q) {
  MT_TAILQ_INSERT_TAIL(&mgr->head, q, next);
  return 0;
}

static int urq_mgr_del(struct mudp_rxq_mgr* mgr, struct mur_queue* q) {
  struct mur_queue *item, *tmp_item;

  for (item = MT_TAILQ_FIRST(&mgr->head); item != NULL; item = tmp_item) {
    tmp_item = MT_TAILQ_NEXT(item, next);
    if (item == q) {
      /* found the matched item, remove it */
      MT_TAILQ_REMOVE(&mgr->head, item, next);
      dbg("%s, succ, q %p\n", __func__, q);
      return 0;
    }
  }

  warn("%s(%d), q %p not found\n", __func__, mgr->port, q);
  return -EIO;
}

static struct mur_queue* urq_mgr_search(struct mudp_rxq_mgr* mgr, uint16_t dst_port) {
  struct mur_queue* q;
  struct mur_queue* ret = NULL;

  MT_TAILQ_FOREACH(q, &mgr->head, next) {
    if (q->dst_port == dst_port) {
      info("%s(%d), dst_port %u already on q %p, reuse_port %d\n", __func__, mgr->port,
           dst_port, q, q->reuse_port);
      ret = q;
    }
  }

  return ret;
}

static int urq_put(struct mur_queue* q) {
  struct mtl_main_impl* impl = q->parent;

  if (!mt_atomic32_dec_and_test(&q->refcnt)) {
    info("%s(%d,%u), refcnt %d\n", __func__, q->port, q->dst_port,
         mt_atomic32_read(&q->refcnt));
    return 0;
  }

  info("%s(%d,%u), refcnt zero now\n", __func__, q->port, q->dst_port);
  struct mudp_rxq_mgr* mgr = impl->mudp_rxq_mgr[q->port];
  urq_mgr_lock(mgr);

  /* check if any not removed client */
  struct mur_client* c;
  while ((c = MT_TAILQ_FIRST(&q->client_head))) {
    warn("%s(%d,%u), %p not removed\n", __func__, q->port, q->dst_port, c);
    MT_TAILQ_REMOVE(&q->client_head, c, next);
  }

  urq_mgr_del(mgr, q);
  if (q->rxq) {
    mt_rxq_put(q->rxq);
    q->rxq = NULL;
  }

  urq_mgr_unlock(mgr);

  mt_pthread_mutex_destroy(&mgr->mutex);
  mt_rte_free(q);
  return 0;
}

static struct mur_queue* urq_get(struct mudp_rxq_mgr* mgr,
                                 struct mur_client_create* create, int* idx) {
  struct mtl_main_impl* impl = create->impl;
  enum mtl_port port = create->port;
  uint16_t dst_port = create->dst_port;
  int ret;
  struct mur_queue* q = NULL;

  urq_mgr_lock(mgr); /* get the lock */

  /* first search if the udp port is used already */
  q = urq_mgr_search(mgr, dst_port);
  if (q) {
    if (!q->reuse_port || !create->reuse_port) {
      err("%s(%d,%u), already used\n", __func__, port, dst_port);
      q = NULL;
      goto out_unlock_fail;
    }

    /* reuse queue */
    q->client_idx++;
    *idx = q->client_idx;
    mt_atomic32_inc(&q->refcnt);
    urq_mgr_unlock(mgr);
    info("%s(%d,%u), reuse cnt %d for q %p\n", __func__, port, dst_port,
         mt_atomic32_read(&q->refcnt), q);
    return q;
  }

  /* create a new one */
  q = mt_rte_zmalloc_socket(sizeof(*q), mt_socket_id(impl, port));
  if (!q) {
    err("%s(%d,%u), queue malloc fail\n", __func__, port, dst_port);
    goto out_unlock_fail;
  }
  q->parent = impl;
  q->port = port;
  q->dst_port = dst_port;
  q->reuse_port = create->reuse_port;
  q->rx_burst_pkts = 128;
  MT_TAILQ_INIT(&q->client_head);
  mt_pthread_mutex_init(&q->mutex, NULL);
  mt_atomic32_inc(&q->refcnt);

  /* create flow */
  struct mt_rxq_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.flags = MT_RXQ_FLOW_F_NO_IP;
  flow.dst_port = dst_port;
  q->rxq = mt_rxq_get(impl, port, &flow);
  if (!q->rxq) {
    /* wa for e810 pf mode since it doesn't support MT_RXQ_FLOW_F_NO_IP */
    warn("%s(%d,%u), get rxq fail with no ip flow, try cni queue\n", __func__, port,
         dst_port);
    flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;
    q->rxq = mt_rxq_get(impl, port, &flow);
    if (!q->rxq) {
      err("%s(%d,%u), get rxq fail with CNI also\n", __func__, port, dst_port);
      goto out_unlock_fail;
    }
    /* start mtl sch with CNI tasklet mode */
    if (!mt_started(impl)) {
      mtl_start(impl);
    }
  }
  q->rxq_id = mt_rxq_queue_id(q->rxq);

  ret = urq_mgr_add(mgr, q);
  if (ret < 0) {
    err("%s(%d,%u), urq mgr add fail %d\n", __func__, port, dst_port, ret);
    goto out_unlock_fail;
  }

  *idx = 0;
  urq_mgr_unlock(mgr);

  info("%s(%d,%u), new q %p\n", __func__, port, dst_port, q);
  return q;

out_unlock_fail:
  urq_mgr_unlock(mgr);
  if (q) urq_put(q);
  return NULL;
}

static int urq_add_client(struct mur_queue* q, struct mur_client* c) {
  urq_lock(q);
  MT_TAILQ_INSERT_TAIL(&q->client_head, c, next);
  q->clients++;
  urq_unlock(q);
  info("%s(%d,%u), %p added\n", __func__, q->port, q->dst_port, c);
  return 0;
}

static int urq_del_client(struct mur_queue* q, struct mur_client* c) {
  struct mur_client *item, *tmp_item;

  urq_lock(q);
  for (item = MT_TAILQ_FIRST(&q->client_head); item != NULL; item = tmp_item) {
    tmp_item = MT_TAILQ_NEXT(item, next);
    if (item == c) {
      /* found the matched item, remove it */
      MT_TAILQ_REMOVE(&q->client_head, item, next);
      q->clients--;
      urq_unlock(q);
      info("%s(%d,%u), %p removed\n", __func__, q->port, q->dst_port, c);
      return 0;
    }
  }
  urq_unlock(q);

  warn("%s(%d,%u), c %p not found\n", __func__, q->port, q->dst_port, c);
  return -EIO;
}

/* client implementation */

static inline bool urc_lcore_mode(struct mur_client* c) {
  return c->lcore_tasklet ? true : false;
}

static void urc_lcore_wakeup(struct mur_client* c) {
  mt_pthread_mutex_lock(&c->lcore_wake_mutex);
  mt_pthread_cond_signal(&c->lcore_wake_cond);
  mt_pthread_mutex_unlock(&c->lcore_wake_mutex);
}

static int urc_tasklet_handler(void* priv) {
  struct mur_client* c = priv;
  struct mtl_main_impl* impl = c->parent;

  if (c->q) urq_rx(c->q);

  unsigned int count = rte_ring_count(c->ring);
  if (count > 0) {
    uint64_t tsc = mt_get_tsc(impl);
    int us = (tsc - c->wake_tsc_last) / NS_PER_US;
    if ((count > c->wake_thresh_count) || (us > c->wake_timeout_us)) {
      urc_lcore_wakeup(c);
      c->wake_tsc_last = tsc;
    }
  }
  return 0;
}

static int urc_init_tasklet(struct mtl_main_impl* impl, struct mur_client* c) {
  if (!mt_user_udp_lcore(impl, c->port)) return 0;

  struct mtl_tasklet_ops ops;
  char name[32];
  snprintf(name, 32, "%sP%dDP%dQ%uC%d", MT_UDP_RXQ_PREFIX, c->port, c->dst_port,
           c->q->rxq_id, c->idx);

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = c;
  ops.name = name;
  ops.handler = urc_tasklet_handler;

  c->lcore_tasklet = mtl_sch_register_tasklet(impl->main_sch, &ops);
  if (!c->lcore_tasklet) {
    err("%s, register lcore tasklet fail\n", __func__);
    MUDP_ERR_RET(EIO);
  }
  /* start mtl to start the sch */
  mtl_start(impl);
  return 0;
}

struct mur_client* mur_client_get(struct mur_client_create* create) {
  struct mtl_main_impl* impl = create->impl;
  enum mtl_port port = create->port;
  uint16_t dst_port = create->dst_port;
  struct mudp_rxq_mgr* mgr = impl->mudp_rxq_mgr[port];
  int ret;
  int idx;

  struct mur_queue* q = urq_get(mgr, create, &idx);
  if (!q) {
    err("%s(%d,%u), get queue fail\n", __func__, port, dst_port);
    return NULL;
  }

  struct mur_client* c = mt_rte_zmalloc_socket(sizeof(*c), mt_socket_id(impl, port));
  if (!c) {
    err("%s(%d,%u), client malloc fail\n", __func__, port, dst_port);
    urq_put(q);
    return NULL;
  }

  c->q = q;
  c->idx = idx;
  c->parent = impl;
  c->port = port;
  c->dst_port = dst_port;

  /* lcore related */
  mt_pthread_mutex_init(&c->lcore_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&c->lcore_wake_cond);
  c->wake_thresh_count = create->wake_thresh_count;
  c->wake_timeout_us = create->wake_timeout_us;
  c->wake_tsc_last = mt_get_tsc(impl);

  char ring_name[64];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, sizeof(ring_name), "%sP%dDP%dQ%uC%d", MT_UDP_RXQ_PREFIX, port,
           dst_port, q->rxq_id, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = create->ring_count;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%u), rx ring create fail\n", __func__, port, dst_port);
    mur_client_put(c);
    return NULL;
  }
  c->ring = ring;

  ret = urc_init_tasklet(impl, c);
  if (ret < 0) {
    err("%s(%d,%u), init tasklet fail %d\n", __func__, port, dst_port, ret);
    mur_client_put(c);
    return NULL;
  }

  /* enable q with client */
  ret = urq_add_client(q, c);
  if (ret < 0) {
    err("%s(%d,%u), urq add client %d\n", __func__, port, dst_port, ret);
    mur_client_put(c);
    return NULL;
  }

  info("%s(%d,%u), count %d\n", __func__, port, dst_port, count);
  return c;
}

int mur_client_put(struct mur_client* c) {
  urc_lcore_wakeup(c); /* wake up any pending wait */

  if (c->lcore_tasklet) {
    mtl_sch_unregister_tasklet(c->lcore_tasklet);
    c->lcore_tasklet = NULL;
  }
  if (c->q) {
    urq_del_client(c->q, c);
    urq_put(c->q);
    c->q = NULL;
  }
  if (c->ring) {
    mt_ring_dequeue_clean(c->ring);
    rte_ring_free(c->ring);
    c->ring = NULL;
  }

  /* lcore related */
  mt_pthread_mutex_destroy(&c->lcore_wake_mutex);
  mt_pthread_cond_destroy(&c->lcore_wake_cond);

  mt_rte_free(c);

  return 0;
}

int mur_client_dump(struct mur_client* c) {
  enum mtl_port port = c->port;
  uint16_t dst_port = c->dst_port;
  int idx = c->idx;

  if (c->stat_pkt_rx) {
    notice("%s(%d,%u,%d), pkt rx %u\n", __func__, port, dst_port, idx, c->stat_pkt_rx);
    c->stat_pkt_rx = 0;
  }
  if (c->stat_pkt_rx_enq_fail) {
    warn("%s(%d,%u,%d), pkt rx %u enqueue fail\n", __func__, port, dst_port, idx,
         c->stat_pkt_rx_enq_fail);
    c->stat_pkt_rx_enq_fail = 0;
  }
  if (c->stat_timedwait) {
    notice("%s(%d,%u,%d), timedwait %u timeout %u\n", __func__, port, dst_port, idx,
           c->stat_timedwait, c->stat_timedwait_timeout);
    c->stat_timedwait = 0;
    c->stat_timedwait_timeout = 0;
  }

  return 0;
}

uint16_t mur_client_rx(struct mur_client* c) {
  if (urc_lcore_mode(c))
    return 0;
  else
    return urq_rx(c->q);
}

int mur_client_timedwait(struct mur_client* c, unsigned int timedwait_us,
                         unsigned int poll_sleep_us) {
  if (!urc_lcore_mode(c)) {
    if (poll_sleep_us) {
      dbg("%s(%d), sleep %u us\n", __func__, c->idx, poll_sleep_us);
      mt_sleep_us(poll_sleep_us);
    }
    return 0; /* return directly if not lcore mode */
  }

  int ret;

  c->stat_timedwait++;
  mt_pthread_mutex_lock(&c->lcore_wake_mutex);
  ret = mt_pthread_cond_timedwait_ns(&c->lcore_wake_cond, &c->lcore_wake_mutex,
                                     (uint64_t)timedwait_us * NS_PER_US);
  dbg("%s(%u), timedwait ret %d\n", __func__, c->dst_port, ret);
  mt_pthread_mutex_unlock(&c->lcore_wake_mutex);

  if (ret == ETIMEDOUT) c->stat_timedwait_timeout++;
  return ret;
}

int mudp_rxq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);

  for (int i = 0; i < num_ports; i++) {
    struct mudp_rxq_mgr* mgr = mt_rte_zmalloc_socket(sizeof(*mgr), socket);
    if (!mgr) {
      err("%s(%d), mgr malloc fail\n", __func__, i);
      mudp_rxq_uinit(impl);
      return -ENOMEM;
    }

    mgr->parent = impl;
    mgr->port = i;
    mt_pthread_mutex_init(&mgr->mutex, NULL);
    MT_TAILQ_INIT(&mgr->head);

    impl->mudp_rxq_mgr[i] = mgr;
  }

  return 0;
}

int mudp_rxq_uinit(struct mtl_main_impl* impl) {
  struct mudp_rxq_mgr* mgr;
  struct mur_queue* q;

  for (int i = 0; i < MTL_PORT_MAX; i++) {
    mgr = impl->mudp_rxq_mgr[i];
    if (!mgr) continue;

    /* check if any not unregister */
    while ((q = MT_TAILQ_FIRST(&mgr->head))) {
      warn("%s(%d), %p(%u) not removed\n", __func__, i, q, q->dst_port);
      MT_TAILQ_REMOVE(&mgr->head, q, next);
      urq_put(q);
    }

    mt_pthread_mutex_destroy(&mgr->mutex);
    mt_rte_free(mgr);
    impl->mudp_rxq_mgr[i] = NULL;
  }
  return 0;
}
