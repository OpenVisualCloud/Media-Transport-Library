/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_shared_queue.h"

#include "../dev/mt_af_xdp.h"
#include "../dev/mt_dev.h"
#include "../mt_flow.h"
#include "../mt_log.h"
#include "../mt_socket.h"
#include "../mt_stat.h"
#include "../mt_util.h"

#define MT_SQ_RING_PREFIX "SQ_"
#define MT_SQ_BURST_SIZE (128)

static inline struct mt_rsq_impl* rsq_ctx_get(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->rsq[port];
}

static inline void rsq_lock(struct mt_rsq_queue* s) {
  rte_spinlock_lock(&s->mutex);
}

/* return true if try lock succ */
static inline bool rsq_try_lock(struct mt_rsq_queue* s) {
  int ret = rte_spinlock_trylock(&s->mutex);
  return ret ? true : false;
}

static inline void rsq_unlock(struct mt_rsq_queue* s) {
  rte_spinlock_unlock(&s->mutex);
}

static int rsq_stat_dump(void* priv) {
  struct mt_rsq_impl* rsq = priv;
  enum mtl_port port = rsq->port;
  struct mt_rsq_queue* s;
  struct mt_rsq_entry* entry;
  int idx;

  for (uint16_t q = 0; q < rsq->nb_rsq_queues; q++) {
    s = &rsq->rsq_queues[q];
    if (!rsq_try_lock(s)) continue;
    if (s->stat_pkts_recv) {
      notice("%s(%d,%u), entries %d, pkt recv %d deliver %d\n", __func__, port, q,
             mt_atomic32_read(&s->entry_cnt), s->stat_pkts_recv, s->stat_pkts_deliver);
      s->stat_pkts_recv = 0;
      s->stat_pkts_deliver = 0;

      MT_TAILQ_FOREACH(entry, &s->head, next) {
        idx = entry->idx;
        notice("%s(%d,%u,%d), enqueue %u dequeue %u\n", __func__, port, q, idx,
               entry->stat_enqueue_cnt, entry->stat_dequeue_cnt);
        entry->stat_enqueue_cnt = 0;
        entry->stat_dequeue_cnt = 0;
        if (entry->stat_enqueue_fail_cnt) {
          warn("%s(%d,%u,%d), enqueue fail %u\n", __func__, port, q, idx,
               entry->stat_enqueue_fail_cnt);
          entry->stat_enqueue_fail_cnt = 0;
        }
      }
    }
    rsq_unlock(s);
  }

  return 0;
}

static int rsq_entry_free(struct mt_rsq_entry* entry) {
  struct mt_rsq_impl* rsqm = entry->parent;

  if (entry->flow_rsp) {
    mt_rx_flow_free(rsqm->parent, rsqm->port, entry->flow_rsp);
    entry->flow_rsp = NULL;
  }
  if (entry->ring) {
    mt_ring_dequeue_clean(entry->ring);
    rte_ring_free(entry->ring);
  }
  if (entry->mcast_fd > 0) close(entry->mcast_fd);

  entry->mcast_fd = -1;

  info("%s(%d), succ on q %u idx %d\n", __func__, rsqm->port, entry->queue_id,
       entry->idx);
  mt_rte_free(entry);
  return 0;
}

static int rsq_uinit(struct mt_rsq_impl* rsq) {
  struct mt_rsq_queue* rsq_queue;
  struct mt_rsq_entry* entry;

  if (rsq->rsq_queues) {
    for (uint16_t q = 0; q < rsq->nb_rsq_queues; q++) {
      rsq_queue = &rsq->rsq_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&rsq_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry);
        MT_TAILQ_REMOVE(&rsq_queue->head, entry, next);
        rsq_entry_free(entry);
      }

      if (rsq_queue->xdp) {
        mt_rx_xdp_put(rsq_queue->xdp);
        rsq_queue->xdp = NULL;
      }
    }
    mt_rte_free(rsq->rsq_queues);
    rsq->rsq_queues = NULL;
  }

  mt_stat_unregister(rsq->parent, rsq_stat_dump, rsq);

  return 0;
}

static int rsq_init(struct mtl_main_impl* impl, struct mt_rsq_impl* rsq) {
  enum mtl_port port = rsq->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_rsq_queue* rsq_queue;

  rsq->rsq_queues =
      mt_rte_zmalloc_socket(sizeof(*rsq->rsq_queues) * rsq->nb_rsq_queues, soc_id);
  if (!rsq->rsq_queues) {
    err("%s(%d), rsq_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < rsq->nb_rsq_queues; q++) {
    rsq_queue = &rsq->rsq_queues[q];
    rsq_queue->queue_id = q;
    rsq_queue->port_id = mt_port_id(impl, port);
    mt_atomic32_set(&rsq_queue->entry_cnt, 0);
    rte_spinlock_init(&rsq_queue->mutex);
    MT_TAILQ_INIT(&rsq_queue->head);
  }

  int ret = mt_stat_register(impl, rsq_stat_dump, rsq, "rsq");
  if (ret < 0) {
    rsq_uinit(rsq);
    return ret;
  }

  return 0;
}

static uint32_t rsq_flow_hash(struct mt_rxq_flow* flow) {
  struct rte_ipv4_tuple tuple;
  uint32_t len;

  if (flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE) return 0;

  len = RTE_THASH_V4_L4_LEN;
  tuple.src_addr = RTE_IPV4(flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2],
                            flow->dip_addr[3]);
  tuple.dst_addr = RTE_IPV4(flow->sip_addr[0], flow->sip_addr[1], flow->sip_addr[2],
                            flow->sip_addr[3]);
  tuple.dport = flow->dst_port;
  tuple.sport = tuple.dport;
  return mt_softrss((uint32_t*)&tuple, len);
}

struct mt_rsq_entry* mt_rsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow) {
  if (!mt_user_shared_rxq(impl, port)) {
    err("%s(%d), shared queue not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_rsq_impl* rsqm = rsq_ctx_get(impl, port);
  uint32_t hash = rsq_flow_hash(flow);
  uint16_t q = (hash % RTE_ETH_RETA_GROUP_SIZE) % rsqm->nb_rsq_queues;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[q];
  int idx = rsq_queue->entry_idx;
  struct mt_rsq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%u), entry malloc fail\n", __func__, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->idx = idx;
  entry->parent = rsqm;
  entry->mcast_fd = -1;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  if (rsqm->queue_mode == MT_QUEUE_MODE_XDP) {
    rsq_lock(rsq_queue);
    if (!rsq_queue->xdp) {
      /* get a 1:1 mapped queue */
      struct mt_rx_xdp_get_args args;
      memset(&args, 0, sizeof(args));
      args.queue_match = true;
      args.queue_id = q;
      args.skip_flow = true;
      args.skip_udp_port_check = true;
      rsq_queue->xdp = mt_rx_xdp_get(impl, port, flow, &args);
      if (!rsq_queue->xdp) {
        err("%s(%d:%u), xdp queue get fail\n", __func__, port, q);
        rsq_unlock(rsq_queue);
        mt_rte_free(entry);
        return NULL;
      }
    }
    rsq_unlock(rsq_queue);
  }

  if (!(flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE)) {
    entry->flow_rsp = mt_rx_flow_create(impl, port, q, flow);
    if (!entry->flow_rsp) {
      err("%s(%u), create flow fail\n", __func__, q);
      rsq_entry_free(entry);
      return NULL;
    }
  }

  /* ring create */
  char ring_name[32];
  snprintf(ring_name, 32, "%sP%d_Q%u_%d", MT_SQ_RING_PREFIX, port, q, idx);
  entry->ring = rte_ring_create(ring_name, 512, mt_socket_id(impl, MTL_PORT_P),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!entry->ring) {
    err("%s(%d,%d), ring %s create fail\n", __func__, port, idx, ring_name);
    rsq_entry_free(entry);
    return NULL;
  }

  if (mt_pmd_is_dpdk_af_packet(impl, port) && mt_is_multicast_ip(flow->dip_addr)) {
    /* join multicast group, will drop automatically when socket fd closed */
    entry->mcast_fd = mt_socket_get_multicast_fd(impl, port, flow);
    if (entry->mcast_fd < 0) {
      err("%s(%d,%d), get multicast socket fd fail %d\n", __func__, port, idx,
          entry->mcast_fd);
      rsq_entry_free(entry);
      return NULL;
    }
  }

  rsq_lock(rsq_queue);
  MT_TAILQ_INSERT_HEAD(&rsq_queue->head, entry, next);
  mt_atomic32_inc(&rsq_queue->entry_cnt);
  rsq_queue->entry_idx++;
  if (flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE) rsq_queue->cni_entry = entry;
  rsq_unlock(rsq_queue);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u, on %d\n", __func__, port, q, ip[0],
       ip[1], ip[2], ip[3], flow->dst_port, hash, idx);
  return entry;
}

int mt_rsq_put(struct mt_rsq_entry* entry) {
  struct mt_rsq_impl* rsqm = entry->parent;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[entry->queue_id];

  rsq_lock(rsq_queue);
  MT_TAILQ_REMOVE(&rsq_queue->head, entry, next);
  mt_atomic32_dec(&rsq_queue->entry_cnt);
  rsq_unlock(rsq_queue);

  rsq_entry_free(entry);
  return 0;
}

static inline void rsq_entry_pkts_enqueue(struct mt_rsq_entry* entry,
                                          struct rte_mbuf** pkts,
                                          const uint16_t nb_pkts) {
  /* use bulk version */
  unsigned int n = rte_ring_sp_enqueue_bulk(entry->ring, (void**)pkts, nb_pkts, NULL);
  entry->stat_enqueue_cnt += n;
  if (n == 0) {
    rte_pktmbuf_free_bulk(pkts, nb_pkts);
    entry->stat_enqueue_fail_cnt += nb_pkts;
  }
}

#define UPDATE_ENTRY()                                                           \
  do {                                                                           \
    if (matched_pkts_nb)                                                         \
      rsq_entry_pkts_enqueue(last_rsq_entry, &matched_pkts[0], matched_pkts_nb); \
    last_rsq_entry = rsq_entry;                                                  \
    matched_pkts_nb = 0;                                                         \
  } while (0)

static int rsq_rx(struct mt_rsq_queue* rsq_queue) {
  uint16_t q = rsq_queue->queue_id;
  struct rte_mbuf* pkts[MT_SQ_BURST_SIZE];
  struct rte_mbuf* matched_pkts[MT_SQ_BURST_SIZE];
  uint16_t rx;
  struct mt_rsq_entry* rsq_entry = NULL;
  struct mt_rsq_entry* last_rsq_entry = NULL;
  uint16_t matched_pkts_nb = 0;
  struct mt_udp_hdr* hdr;

  if (rsq_queue->xdp)
    rx = mt_rx_xdp_burst(rsq_queue->xdp, pkts, MT_SQ_BURST_SIZE);
  else
    rx = rte_eth_rx_burst(rsq_queue->port_id, q, pkts, MT_SQ_BURST_SIZE);
  if (rx) dbg("%s(%u), rx pkts %u\n", __func__, q, rx);
  rsq_queue->stat_pkts_recv += rx;

  for (uint16_t i = 0; i < rx; i++) {
    rsq_entry = NULL;

    hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    dbg("%s, pkt %u ip %u, port dst %u src %u\n", __func__, q, i,
        (unsigned int)ntohs(hdr->udp.dst_port), (unsigned int)ntohs(hdr->udp.src_port));

    MT_TAILQ_FOREACH(rsq_entry, &rsq_queue->head, next) {
      bool matched = mt_udp_matched(&rsq_entry->flow, hdr);
      if (matched) {
        if (rsq_entry != last_rsq_entry) UPDATE_ENTRY();
        matched_pkts[matched_pkts_nb++] = pkts[i];
        break;
      }
    }
    if (!rsq_entry) { /* no match, redirect to cni */
      UPDATE_ENTRY();
      if (rsq_queue->cni_entry) rsq_entry_pkts_enqueue(rsq_queue->cni_entry, &pkts[i], 1);
    }
  }
  if (matched_pkts_nb)
    rsq_entry_pkts_enqueue(last_rsq_entry, &matched_pkts[0], matched_pkts_nb);

  return rx;
}

uint16_t mt_rsq_burst(struct mt_rsq_entry* entry, struct rte_mbuf** rx_pkts,
                      uint16_t nb_pkts) {
  struct mt_rsq_impl* rsqm = entry->parent;
  uint16_t q = entry->queue_id;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[q];

  if (!rsq_try_lock(rsq_queue)) return 0;
  rsq_rx(rsq_queue);
  rsq_unlock(rsq_queue);
  uint16_t n = rte_ring_sc_dequeue_burst(entry->ring, (void**)rx_pkts, nb_pkts, NULL);
  entry->stat_dequeue_cnt += n;

  return n;
}

int mt_rsq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_user_shared_rxq(impl, i)) continue;
    impl->rsq[i] = mt_rte_zmalloc_socket(sizeof(*impl->rsq[i]), mt_socket_id(impl, i));
    if (!impl->rsq[i]) {
      err("%s(%d), rsq malloc fail\n", __func__, i);
      mt_rsq_uinit(impl);
      return -ENOMEM;
    }
    impl->rsq[i]->parent = impl;
    impl->rsq[i]->port = i;
    impl->rsq[i]->nb_rsq_queues = mt_if(impl, i)->nb_rx_q;
    impl->rsq[i]->queue_mode =
        mt_pmd_is_native_af_xdp(impl, i) ? MT_QUEUE_MODE_XDP : MT_QUEUE_MODE_DPDK;
    ret = rsq_init(impl, impl->rsq[i]);
    if (ret < 0) {
      err("%s(%d), rsq init fail\n", __func__, i);
      mt_rsq_uinit(impl);
      return ret;
    }
    info("%s(%d), succ with shared queue mode\n", __func__, i);
  }

  return 0;
}

int mt_rsq_uinit(struct mtl_main_impl* impl) {
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (impl->rsq[i]) {
      rsq_uinit(impl->rsq[i]);
      mt_rte_free(impl->rsq[i]);
      impl->rsq[i] = NULL;
    }
  }

  return 0;
}

static inline struct mt_tsq_impl* tsq_ctx_get(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->tsq[port];
}

static inline void tsq_lock(struct mt_tsq_queue* s) {
  mt_pthread_mutex_lock(&s->mutex);
}

/* return true if try lock succ */
static inline bool tsq_try_lock(struct mt_tsq_queue* s) {
  int ret = mt_pthread_mutex_try_lock(&s->mutex);
  return ret == 0 ? true : false;
}

static inline void tsq_unlock(struct mt_tsq_queue* s) {
  mt_pthread_mutex_unlock(&s->mutex);
}

static int tsq_stat_dump(void* priv) {
  struct mt_tsq_impl* tsq = priv;
  struct mt_tsq_queue* s;

  for (uint16_t q = 0; q < tsq->nb_tsq_queues; q++) {
    s = &tsq->tsq_queues[q];
    if (!tsq_try_lock(s)) continue;
    if (s->stat_pkts_send) {
      notice("%s(%d,%u), entries %d, pkt send %d\n", __func__, tsq->port, q,
             mt_atomic32_read(&s->entry_cnt), s->stat_pkts_send);
      s->stat_pkts_send = 0;
    }
    tsq_unlock(s);
  }

  return 0;
}

static int tsq_entry_free(struct mt_tsq_entry* entry) {
  mt_rte_free(entry);
  return 0;
}

static int tsq_uinit(struct mt_tsq_impl* tsq) {
  struct mt_tsq_queue* tsq_queue;
  struct mt_tsq_entry* entry;

  if (tsq->tsq_queues) {
    for (uint16_t q = 0; q < tsq->nb_tsq_queues; q++) {
      tsq_queue = &tsq->tsq_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&tsq_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry);
        MT_TAILQ_REMOVE(&tsq_queue->head, entry, next);
        tsq_entry_free(entry);
      }
      if (tsq_queue->tx_pool) {
        mt_mempool_free(tsq_queue->tx_pool);
        tsq_queue->tx_pool = NULL;
      }
      if (tsq_queue->xdp) {
        mt_tx_xdp_put(tsq_queue->xdp);
        tsq_queue->xdp = NULL;
      }
      mt_pthread_mutex_destroy(&tsq_queue->mutex);
    }
    mt_rte_free(tsq->tsq_queues);
    tsq->tsq_queues = NULL;
  }

  mt_stat_unregister(tsq->parent, tsq_stat_dump, tsq);

  return 0;
}

static int tsq_init(struct mtl_main_impl* impl, struct mt_tsq_impl* tsq) {
  enum mtl_port port = tsq->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_tsq_queue* tsq_queue;

  tsq->tsq_queues =
      mt_rte_zmalloc_socket(sizeof(*tsq->tsq_queues) * tsq->nb_tsq_queues, soc_id);
  if (!tsq->tsq_queues) {
    err("%s(%d), tsq_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < tsq->nb_tsq_queues; q++) {
    tsq_queue = &tsq->tsq_queues[q];
    tsq_queue->queue_id = q;
    tsq_queue->port_id = mt_port_id(impl, port);
    mt_atomic32_set(&tsq_queue->entry_cnt, 0);
    mt_pthread_mutex_init(&tsq_queue->mutex, NULL);
    MT_TAILQ_INIT(&tsq_queue->head);
  }

  int ret = mt_stat_register(impl, tsq_stat_dump, tsq, "tsq");
  if (ret < 0) {
    tsq_uinit(tsq);
    return ret;
  }

  return 0;
}

static uint32_t tsq_flow_hash(struct mt_txq_flow* flow) {
  struct rte_ipv4_tuple tuple;
  uint32_t len;

  if (flow->flags & MT_TXQ_FLOW_F_SYS_QUEUE) return 0;

  len = RTE_THASH_V4_L4_LEN;
  tuple.src_addr = RTE_IPV4(flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2],
                            flow->dip_addr[3]);
  tuple.dst_addr = tuple.src_addr;
  tuple.sport = flow->dst_port;
  tuple.dport = flow->dst_port;
  return mt_softrss((uint32_t*)&tuple, len);
}

struct mt_tsq_entry* mt_tsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_txq_flow* flow) {
  if (!mt_user_shared_txq(impl, port)) {
    err("%s(%d), shared queue not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_tsq_impl* tsqm = tsq_ctx_get(impl, port);
  uint32_t hash = tsq_flow_hash(flow);
  uint16_t q = 0;
  /* queue zero is reserved for system queue */
  if (!(flow->flags & MT_TXQ_FLOW_F_SYS_QUEUE)) {
    q = (hash % RTE_ETH_RETA_GROUP_SIZE) % (tsqm->nb_tsq_queues - 1) + 1;
  }
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[q];

  if (tsq_queue->fatal_error) {
    /* try to find one valid queue */
    uint16_t q_b = rand() % (tsqm->nb_tsq_queues - 1) + 1;
    tsq_queue = &tsqm->tsq_queues[q];

    if (tsq_queue->fatal_error) { /* loop to find a valid queue*/
      for (q_b = 1; q_b < tsqm->nb_tsq_queues; q_b++) {
        tsq_queue = &tsqm->tsq_queues[q_b];
        if (!tsq_queue->fatal_error) break;
      }
    }

    if (tsq_queue->fatal_error) {
      err("%s(%d), all queues are in fatal error stat\n", __func__, port);
      return NULL;
    }

    warn("%s(%d), q %u is fatal error, use %u instead\n", __func__, port, q, q_b);
    q = q_b;
    tsq_queue = &tsqm->tsq_queues[q];
  }
  struct mt_tsq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d:%u), entry malloc fail\n", __func__, port, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->parent = tsqm;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  tsq_lock(tsq_queue);
  if (!tsq_queue->tx_pool) {
    char pool_name[32];
    snprintf(pool_name, 32, "TSQ_P%dQ%u", port, q);
    struct rte_mempool* pool =
        mt_mempool_create(impl, port, pool_name, mt_if_nb_tx_desc(impl, port) + 512,
                          MT_MBUF_CACHE_SIZE, 0, MTL_MTU_MAX_BYTES);
    if (!pool) {
      err("%s(%d:%u), mempool create fail\n", __func__, port, q);
      tsq_unlock(tsq_queue);
      mt_rte_free(entry);
      return NULL;
    }
    tsq_queue->tx_pool = pool;
  }
  if (tsqm->queue_mode == MT_QUEUE_MODE_XDP) {
    if (!tsq_queue->xdp) {
      /* get a 1:1 mapped queue */
      struct mt_tx_xdp_get_args args;
      memset(&args, 0, sizeof(args));
      args.queue_match = true;
      args.queue_id = q;
      tsq_queue->xdp = mt_tx_xdp_get(impl, port, flow, &args);
      if (!tsq_queue->xdp) {
        err("%s(%d:%u), xdp queue get fail\n", __func__, port, q);
        tsq_unlock(tsq_queue);
        mt_rte_free(entry);
        return NULL;
      }
    }
  }

  MT_TAILQ_INSERT_HEAD(&tsq_queue->head, entry, next);
  mt_atomic32_inc(&tsq_queue->entry_cnt);
  tsq_unlock(tsq_queue);

  entry->tx_pool = tsq_queue->tx_pool;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u\n", __func__, port, q, ip[0], ip[1],
       ip[2], ip[3], flow->dst_port, hash);
  return entry;
}

int mt_tsq_put(struct mt_tsq_entry* entry) {
  struct mt_tsq_impl* tsqm = entry->parent;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];

  tsq_lock(tsq_queue);
  MT_TAILQ_REMOVE(&tsq_queue->head, entry, next);
  mt_atomic32_dec(&tsq_queue->entry_cnt);
  tsq_unlock(tsq_queue);

  tsq_entry_free(entry);
  return 0;
}

int mt_tsq_fatal_error(struct mt_tsq_entry* entry) {
  struct mt_tsq_impl* tsqm = entry->parent;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];

  tsq_lock(tsq_queue);
  tsq_queue->fatal_error = true;
  tsq_unlock(tsq_queue);

  err("%s(%d), q %d masked as fatal error\n", __func__, tsqm->port, tsq_queue->queue_id);
  return 0;
}

int mt_tsq_done_cleanup(struct mt_tsq_entry* entry) {
  struct mt_tsq_impl* tsqm = entry->parent;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];

  tsq_lock(tsq_queue);
  rte_eth_tx_done_cleanup(tsq_queue->port_id, tsq_queue->queue_id, 0);
  tsq_unlock(tsq_queue);

  return 0;
}

uint16_t mt_tsq_burst(struct mt_tsq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts) {
  struct mt_tsq_impl* tsqm = entry->parent;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];
  uint16_t tx;

  rte_spinlock_lock(&tsq_queue->tx_mutex);
  if (tsq_queue->xdp)
    tx = mt_tx_xdp_burst(tsq_queue->xdp, tx_pkts, nb_pkts);
  else
    tx = rte_eth_tx_burst(tsq_queue->port_id, tsq_queue->queue_id, tx_pkts, nb_pkts);
  tsq_queue->stat_pkts_send += tx;
  rte_spinlock_unlock(&tsq_queue->tx_mutex);

  return tx;
}

uint16_t mt_tsq_burst_busy(struct mtl_main_impl* impl, struct mt_tsq_entry* entry,
                           struct rte_mbuf** tx_pkts, uint16_t nb_pkts, int timeout_ms) {
  uint16_t sent = 0;
  uint64_t start_ts = mt_get_tsc(impl);

  /* Send this vector with busy looping */
  while (sent < nb_pkts) {
    if (timeout_ms > 0) {
      int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
      if (ms > timeout_ms) {
        warn("%s(%u), fail as timeout to %d ms\n", __func__, mt_tsq_queue_id(entry),
             timeout_ms);
        return sent;
      }
    }
    sent += mt_tsq_burst(entry, &tx_pkts[sent], nb_pkts - sent);
  }

  return sent;
}

int mt_tsq_flush(struct mtl_main_impl* impl, struct mt_tsq_entry* entry,
                 struct rte_mbuf* pad) {
  struct mt_tsq_impl* tsqm = entry->parent;
  enum mtl_port port = tsqm->port;
  uint16_t queue_id = entry->queue_id;

  /* use double to make sure all the fifo are burst out to clean all mbufs in the pool */
  int burst_pkts = mt_if_nb_tx_burst(impl, port) * 2;
  struct rte_mbuf* pads[1];
  pads[0] = pad;

  info("%s(%d), queue %u burst_pkts %d\n", __func__, port, queue_id, burst_pkts);
  for (int i = 0; i < burst_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    mt_tsq_burst_busy(impl, entry, &pads[0], 1, 10);
  }
  dbg("%s, end\n", __func__);
  return 0;
}

int mt_tsq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_user_shared_txq(impl, i)) continue;
    impl->tsq[i] = mt_rte_zmalloc_socket(sizeof(*impl->tsq[i]), mt_socket_id(impl, i));
    if (!impl->tsq[i]) {
      err("%s(%d), tsq malloc fail\n", __func__, i);
      mt_tsq_uinit(impl);
      return -ENOMEM;
    }
    impl->tsq[i]->parent = impl;
    impl->tsq[i]->port = i;
    impl->tsq[i]->nb_tsq_queues = mt_if(impl, i)->nb_tx_q;
    impl->tsq[i]->queue_mode =
        mt_pmd_is_native_af_xdp(impl, i) ? MT_QUEUE_MODE_XDP : MT_QUEUE_MODE_DPDK;
    ret = tsq_init(impl, impl->tsq[i]);
    if (ret < 0) {
      err("%s(%d), tsq init fail\n", __func__, i);
      mt_tsq_uinit(impl);
      return ret;
    }
    info("%s(%d), succ with shared queue mode\n", __func__, i);
  }

  return 0;
}

int mt_tsq_uinit(struct mtl_main_impl* impl) {
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (impl->tsq[i]) {
      tsq_uinit(impl->tsq[i]);
      mt_rte_free(impl->tsq[i]);
      impl->tsq[i] = NULL;
    }
  }

  return 0;
}
