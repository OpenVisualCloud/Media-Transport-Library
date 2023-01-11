/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_shared_queue.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "mt_util.h"

static inline struct mt_rsq_impl* rsq_ctx_get(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->rsq[port];
}

static int rsq_stat_dump(void* priv) {
  struct mt_rsq_impl* rsq = priv;
  struct mt_rsq_queue* s;

  for (uint16_t q = 0; q < rsq->max_rsq_queues; q++) {
    s = &rsq->rsq_queues[q];
    if (s->stat_pkts_recv) {
      info("%s(%d,%u), entries %d, pkt recv %d deliver %d\n", __func__, rsq->port, q,
           rte_atomic32_read(&s->entry_cnt), s->stat_pkts_recv, s->stat_pkts_deliver);
      s->stat_pkts_recv = 0;
      s->stat_pkts_deliver = 0;
    }
  }

  return 0;
}

static int rsq_entry_free(struct mt_rsq_entry* entry) {
  struct mt_rsq_impl* rsqm = entry->parnet;

  if (entry->flow_rsp) {
    mt_dev_free_rx_flow(rsqm->parnet, rsqm->port, entry->flow_rsp);
    entry->flow_rsp = NULL;
  }
  mt_rte_free(entry);
  return 0;
}

static int rsq_uinit(struct mt_rsq_impl* rsq) {
  struct mt_rsq_queue* rsq_queue;
  struct mt_rsq_entry* entry;

  if (rsq->rsq_queues) {
    for (uint16_t q = 0; q < rsq->max_rsq_queues; q++) {
      rsq_queue = &rsq->rsq_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&rsq_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry->flow.priv);
        MT_TAILQ_REMOVE(&rsq_queue->head, entry, next);
        rsq_entry_free(entry);
      }
      mt_pthread_mutex_destroy(&rsq_queue->mutex);
    }
    mt_rte_free(rsq->rsq_queues);
    rsq->rsq_queues = NULL;
  }

  mt_stat_unregister(rsq->parnet, rsq_stat_dump, rsq);

  return 0;
}

static int rsq_init(struct mtl_main_impl* impl, struct mt_rsq_impl* rsq) {
  enum mtl_port port = rsq->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_rsq_queue* rsq_queue;

  rsq->rsq_queues =
      mt_rte_zmalloc_socket(sizeof(*rsq->rsq_queues) * rsq->max_rsq_queues, soc_id);
  if (!rsq->rsq_queues) {
    err("%s(%d), rsq_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < rsq->max_rsq_queues; q++) {
    rsq_queue = &rsq->rsq_queues[q];
    rsq_queue->queue_id = q;
    rsq_queue->port_id = mt_port_id(impl, port);
    rte_atomic32_set(&rsq_queue->entry_cnt, 0);
    mt_pthread_mutex_init(&rsq_queue->mutex, NULL);
    MT_TAILQ_INIT(&rsq_queue->head);
  }

  int ret = mt_stat_register(impl, rsq_stat_dump, rsq);
  if (ret < 0) {
    rsq_uinit(rsq);
    return ret;
  }

  return 0;
}

static uint32_t rsq_flow_hash(struct mt_rx_flow* flow) {
  struct rte_ipv4_tuple tuple;
  uint32_t len;

  if (flow->sys_queue) return 0;

  len = RTE_THASH_V4_L4_LEN;
  tuple.src_addr = RTE_IPV4(flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2],
                            flow->dip_addr[3]);
  tuple.dst_addr = RTE_IPV4(flow->sip_addr[0], flow->sip_addr[1], flow->sip_addr[2],
                            flow->sip_addr[3]);
  tuple.sport = flow->dst_port;
  tuple.dport = flow->dst_port;
  return mt_dev_softrss((uint32_t*)&tuple, len);
}

struct mt_rsq_entry* mt_rsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rx_flow* flow) {
  if (!mt_shared_queue(impl, port)) {
    err("%s(%d), shared queue not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_rsq_impl* rsqm = rsq_ctx_get(impl, port);
  uint32_t hash = rsq_flow_hash(flow);
  uint16_t q = (hash % RTE_ETH_RETA_GROUP_SIZE) % rsqm->max_rsq_queues;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[q];
  struct mt_rsq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%u), entry malloc fail\n", __func__, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->parnet = rsqm;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));
  entry->dst_port_net = htons(flow->dst_port);

  if (!flow->sys_queue) {
    entry->flow_rsp = mt_dev_create_rx_flow(impl, port, q, flow);
    if (!entry->flow_rsp) {
      err("%s(%u), create flow fail\n", __func__, q);
      return NULL;
    }
  }

  mt_pthread_mutex_lock(&rsq_queue->mutex);
  /* todo: insert rsq entry by rbtree? */
  MT_TAILQ_INSERT_HEAD(&rsq_queue->head, entry, next);
  rte_atomic32_inc(&rsq_queue->entry_cnt);
  mt_pthread_mutex_unlock(&rsq_queue->mutex);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u\n", __func__, port, q, ip[0], ip[1],
       ip[2], ip[3], flow->dst_port, hash);
  return entry;
}

int mt_rsq_put(struct mt_rsq_entry* entry) {
  struct mt_rsq_impl* rsqm = entry->parnet;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[entry->queue_id];

  mt_pthread_mutex_lock(&rsq_queue->mutex);
  MT_TAILQ_REMOVE(&rsq_queue->head, entry, next);
  rte_atomic32_dec(&rsq_queue->entry_cnt);
  mt_pthread_mutex_unlock(&rsq_queue->mutex);

  rsq_entry_free(entry);
  return 0;
}

uint16_t mt_rsq_burst(struct mt_rsq_entry* entry, uint16_t nb_pkts) {
  struct mt_rsq_impl* rsqm = entry->parnet;
  uint16_t q = entry->queue_id;
  struct mt_rsq_queue* rsq_queue = &rsqm->rsq_queues[q];
  struct rte_mbuf* pkts[nb_pkts];
  uint16_t rx;
  struct mt_rsq_entry* rsq_entry;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  uint8_t* sip;
  struct rte_udp_hdr* udp;

  mt_pthread_mutex_lock(&rsq_queue->mutex);
  rx = rte_eth_rx_burst(rsq_queue->port_id, q, pkts, nb_pkts);
  if (rx) dbg("%s(%u), rx pkts %u\n", __func__, q, rx);
  rsq_queue->stat_pkts_recv += rx;
  for (uint16_t i = 0; i < rx; i++) {
    hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    ipv4 = &hdr->ipv4;
    sip = (uint8_t*)&ipv4->src_addr;
    udp = &hdr->udp;
    dbg("%s(%u), pkt %u ip %u.%u.%u.%u, port dst %u src %u\n", __func__, q, i, sip[0],
        sip[1], sip[2], sip[3], ntohs(udp->dst_port), ntohs(udp->src_port));
    MT_TAILQ_FOREACH(rsq_entry, &rsq_queue->head, next) {
      /* check if this is the matched pkt or sys entry */
      if (!memcmp(sip, rsq_entry->flow.dip_addr, MTL_IP_ADDR_LEN) &&
          (rsq_entry->dst_port_net == udp->dst_port)) {
        rsq_entry->flow.cb(rsq_entry->flow.priv, &pkts[i], 1);
        rsq_queue->stat_pkts_deliver++;
        break;
      }
      if (rsq_entry->flow.sys_queue) { /* sys flow is always in last pos */
        rsq_entry->flow.cb(rsq_entry->flow.priv, &pkts[i], 1);
        rsq_queue->stat_pkts_deliver++;
        break;
      }
    }
  }
  mt_pthread_mutex_unlock(&rsq_queue->mutex);

  rte_pktmbuf_free_bulk(&pkts[0], rx);

  return rx;
}

int mt_rsq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_shared_queue(impl, i)) continue;
    impl->rsq[i] = mt_rte_zmalloc_socket(sizeof(*impl->rsq[i]), mt_socket_id(impl, i));
    if (!impl->rsq[i]) {
      err("%s(%d), rsq malloc fail\n", __func__, i);
      mt_rsq_uinit(impl);
      return -ENOMEM;
    }
    impl->rsq[i]->parnet = impl;
    impl->rsq[i]->port = i;
    impl->rsq[i]->max_rsq_queues = mt_if(impl, i)->max_rx_queues;
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

static int tsq_stat_dump(void* priv) {
  struct mt_tsq_impl* tsq = priv;
  struct mt_tsq_queue* s;

  for (uint16_t q = 0; q < tsq->max_tsq_queues; q++) {
    s = &tsq->tsq_queues[q];
    if (s->stat_pkts_send) {
      info("%s(%d,%u), entries %d, pkt send %d\n", __func__, tsq->port, q,
           rte_atomic32_read(&s->entry_cnt), s->stat_pkts_send);
      s->stat_pkts_send = 0;
    }
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
    for (uint16_t q = 0; q < tsq->max_tsq_queues; q++) {
      tsq_queue = &tsq->tsq_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&tsq_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry);
        MT_TAILQ_REMOVE(&tsq_queue->head, entry, next);
        tsq_entry_free(entry);
      }
      mt_pthread_mutex_destroy(&tsq_queue->mutex);
      mt_pthread_mutex_destroy(&tsq_queue->tx_mutex);
    }
    mt_rte_free(tsq->tsq_queues);
    tsq->tsq_queues = NULL;
  }

  mt_stat_unregister(tsq->parnet, tsq_stat_dump, tsq);

  return 0;
}

static int tsq_init(struct mtl_main_impl* impl, struct mt_tsq_impl* tsq) {
  enum mtl_port port = tsq->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_tsq_queue* tsq_queue;

  tsq->tsq_queues =
      mt_rte_zmalloc_socket(sizeof(*tsq->tsq_queues) * tsq->max_tsq_queues, soc_id);
  if (!tsq->tsq_queues) {
    err("%s(%d), tsq_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < tsq->max_tsq_queues; q++) {
    tsq_queue = &tsq->tsq_queues[q];
    tsq_queue->queue_id = q;
    tsq_queue->port_id = mt_port_id(impl, port);
    rte_atomic32_set(&tsq_queue->entry_cnt, 0);
    mt_pthread_mutex_init(&tsq_queue->mutex, NULL);
    mt_pthread_mutex_init(&tsq_queue->tx_mutex, NULL);
    MT_TAILQ_INIT(&tsq_queue->head);
  }

  int ret = mt_stat_register(impl, tsq_stat_dump, tsq);
  if (ret < 0) {
    tsq_uinit(tsq);
    return ret;
  }

  return 0;
}

static uint32_t tsq_flow_hash(struct mt_tsq_flow* flow) {
  struct rte_ipv4_tuple tuple;
  uint32_t len;

  if (flow->sys_queue) return 0;

  len = RTE_THASH_V4_L4_LEN;
  tuple.src_addr = RTE_IPV4(flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2],
                            flow->dip_addr[3]);
  tuple.dst_addr = tuple.src_addr;
  tuple.sport = flow->dst_port;
  tuple.dport = flow->dst_port;
  return mt_dev_softrss((uint32_t*)&tuple, len);
}

struct mt_tsq_entry* mt_tsq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_tsq_flow* flow) {
  if (!mt_shared_queue(impl, port)) {
    err("%s(%d), shared queue not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_tsq_impl* tsqm = tsq_ctx_get(impl, port);
  uint32_t hash = tsq_flow_hash(flow);
  uint16_t q = (hash % RTE_ETH_RETA_GROUP_SIZE) % tsqm->max_tsq_queues;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[q];
  struct mt_tsq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%u), entry malloc fail\n", __func__, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->parnet = tsqm;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  mt_pthread_mutex_lock(&tsq_queue->mutex);
  MT_TAILQ_INSERT_HEAD(&tsq_queue->head, entry, next);
  rte_atomic32_inc(&tsq_queue->entry_cnt);
  mt_pthread_mutex_unlock(&tsq_queue->mutex);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u\n", __func__, port, q, ip[0], ip[1],
       ip[2], ip[3], flow->dst_port, hash);
  return entry;
}

int mt_tsq_put(struct mt_tsq_entry* entry) {
  struct mt_tsq_impl* tsqm = entry->parnet;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];

  mt_pthread_mutex_lock(&tsq_queue->mutex);
  MT_TAILQ_REMOVE(&tsq_queue->head, entry, next);
  rte_atomic32_dec(&tsq_queue->entry_cnt);
  mt_pthread_mutex_unlock(&tsq_queue->mutex);

  tsq_entry_free(entry);
  return 0;
}

uint16_t mt_tsq_burst(struct mt_tsq_entry* entry, struct rte_mbuf** tx_pkts,
                      uint16_t nb_pkts) {
  struct mt_tsq_impl* tsqm = entry->parnet;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];
  uint16_t tx;

  mt_pthread_mutex_lock(&tsq_queue->tx_mutex);
  tx = rte_eth_tx_burst(tsq_queue->port_id, tsq_queue->queue_id, tx_pkts, nb_pkts);
  tsq_queue->stat_pkts_send += tx;
  mt_pthread_mutex_unlock(&tsq_queue->tx_mutex);

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
  struct mt_tsq_impl* tsqm = entry->parnet;
  enum mtl_port port = tsqm->port;
  uint16_t queue_id = entry->queue_id;

  int burst_pkts = mt_if_nb_tx_burst(impl, port);
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

int mt_tsq_set_bps(struct mtl_main_impl* impl, struct mt_tsq_entry* entry,
                   uint64_t bytes_per_sec) {
  struct mt_tsq_impl* tsqm = entry->parnet;
  enum mtl_port port = tsqm->port;
  struct mt_tsq_queue* tsq_queue = &tsqm->tsq_queues[entry->queue_id];
  uint16_t q = entry->queue_id;

  mt_pthread_mutex_lock(&tsq_queue->tx_mutex);
  mt_dev_set_tx_bps(impl, port, q, bytes_per_sec);
  mt_pthread_mutex_unlock(&tsq_queue->tx_mutex);

  return 0;
}

int mt_tsq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_shared_queue(impl, i)) continue;
    impl->tsq[i] = mt_rte_zmalloc_socket(sizeof(*impl->tsq[i]), mt_socket_id(impl, i));
    if (!impl->tsq[i]) {
      err("%s(%d), tsq malloc fail\n", __func__, i);
      mt_tsq_uinit(impl);
      return -ENOMEM;
    }
    impl->tsq[i]->parnet = impl;
    impl->tsq[i]->port = i;
    impl->tsq[i]->max_tsq_queues = mt_if(impl, i)->max_tx_queues;
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
