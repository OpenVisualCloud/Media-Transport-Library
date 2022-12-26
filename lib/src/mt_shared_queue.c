/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_shared_queue.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_util.h"

static inline struct mt_sq_impl* sq_ctx_get(struct mtl_main_impl* impl,
                                            enum mtl_port port) {
  return impl->sq[port];
}

static int sq_init(struct mtl_main_impl* impl, struct mt_sq_impl* sq) {
  enum mtl_port port = sq->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_sq_queue* sq_queue;

  sq->sq_queues =
      mt_rte_zmalloc_socket(sizeof(*sq->sq_queues) * sq->max_sq_queues, soc_id);
  if (!sq->sq_queues) {
    err("%s(%d), sq_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < sq->max_sq_queues; q++) {
    sq_queue = &sq->sq_queues[q];
    sq_queue->queue_id = q;
    sq_queue->port_id = mt_port_id(impl, port);
    mt_pthread_mutex_init(&sq_queue->mutex, NULL);
    MT_TAILQ_INIT(&sq_queue->head);
  }

  return 0;
}

static int sq_uinit(struct mt_sq_impl* sq) {
  struct mt_sq_queue* sq_queue;
  struct mt_sq_entry* entry;

  if (sq->sq_queues) {
    for (uint16_t q = 0; q < sq->max_sq_queues; q++) {
      sq_queue = &sq->sq_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&sq_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry->flow.priv);
        MT_TAILQ_REMOVE(&sq_queue->head, entry, next);
        mt_rte_free(entry);
      }
      mt_pthread_mutex_destroy(&sq->sq_queues[q].mutex);
    }
    mt_rte_free(sq->sq_queues);
    sq->sq_queues = NULL;
  }

  return 0;
}

static uint32_t sq_flow_hash(struct mt_sq_flow* flow) {
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

struct mt_sq_entry* mt_sq_get(struct mtl_main_impl* impl, enum mtl_port port,
                              struct mt_sq_flow* flow) {
  if (!mt_shared_queue(impl, port)) {
    err("%s(%d), shared queue not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_sq_impl* sqm = sq_ctx_get(impl, port);
  uint32_t hash = sq_flow_hash(flow);
  uint16_t q = (hash % RTE_ETH_RETA_GROUP_SIZE) % sqm->max_sq_queues;
  struct mt_sq_queue* sq_queue = &sqm->sq_queues[q];
  struct mt_sq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%u), entry malloc fail\n", __func__, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->parnet = sqm;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));
  entry->hash = hash;

  mt_pthread_mutex_lock(&sq_queue->mutex);
  /* todo: insert sq entry by rbtree? */
  MT_TAILQ_INSERT_TAIL(&sq_queue->head, entry, next);
  mt_pthread_mutex_unlock(&sq_queue->mutex);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u\n", __func__, port, q, ip[0], ip[1],
       ip[2], ip[3], flow->dst_port, hash);
  return entry;
}

int mt_sq_put(struct mt_sq_entry* entry) {
  struct mt_sq_impl* sqm = entry->parnet;
  struct mt_sq_queue* sq_queue = &sqm->sq_queues[entry->queue_id];

  mt_pthread_mutex_lock(&sq_queue->mutex);
  MT_TAILQ_REMOVE(&sq_queue->head, entry, next);
  mt_pthread_mutex_unlock(&sq_queue->mutex);

  mt_rte_free(entry);
  return 0;
}

uint16_t mt_sq_rx_burst(struct mt_sq_entry* entry, uint16_t nb_pkts) {
  struct mt_sq_impl* sqm = entry->parnet;
  uint16_t q = entry->queue_id;
  struct mt_sq_queue* sq_queue = &sqm->sq_queues[q];
  struct rte_mbuf* pkts[nb_pkts];
  uint16_t rx;
  struct mt_sq_entry* sq_entry;
  uint32_t hash;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;

  mt_pthread_mutex_lock(&sq_queue->mutex);
  rx = rte_eth_rx_burst(sq_queue->port_id, q, pkts, nb_pkts);
  if (rx) dbg("%s(%u), rx pkts %u\n", __func__, q, rx);
  for (uint16_t i = 0; i < rx; i++) {
    hash = pkts[i]->hash.rss;
    hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    ipv4 = &hdr->ipv4;
    dbg("%s(%u), pkt %u sq %u\n", __func__, q, i, hash);
    MT_TAILQ_FOREACH(sq_entry, &sq_queue->head, next) {
      /* check if this is the matched hash or sys entry */
      /* todo: handle if two entries has same hash, and bulk mode */
      if ((hash == sq_entry->hash) ||
          (sq_entry->flow.sys_queue && (ipv4->next_proto_id != IPPROTO_UDP))) {
        sq_entry->flow.cb(sq_entry->flow.priv, &pkts[i], 1);
        break;
      }
    }
  }
  mt_pthread_mutex_unlock(&sq_queue->mutex);

  rte_pktmbuf_free_bulk(&pkts[0], rx);

#if 0
  /* debug */
  for (q = 1; q < sq->max_sq_queues; q++) {
    rx = rte_eth_rx_burst(sq_queue->port_id, q, pkts, nb_pkts);
    for (uint16_t i = 0; i < rx; i++) {
      info("%s(%u), pkt %u sq %u\n", __func__, q, i, pkts[i]->hash.sq);
      // mt_mbuf_dump_hdr(0, 0, "sq", pkts[i]);
    }
    rte_pktmbuf_free_bulk(&pkts[0], rx);
  }
#endif

  return rx;
}

int mt_sq_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_shared_queue(impl, i)) continue;
    impl->sq[i] = mt_rte_zmalloc_socket(sizeof(*impl->sq[i]), mt_socket_id(impl, i));
    if (!impl->sq[i]) {
      err("%s(%d), sq malloc fail\n", __func__, i);
      mt_sq_uinit(impl);
      return -ENOMEM;
    }
    impl->sq[i]->port = i;
    impl->sq[i]->max_sq_queues = mt_if(impl, i)->max_rx_queues;
    ret = sq_init(impl, impl->sq[i]);
    if (ret < 0) {
      err("%s(%d), sq init fail\n", __func__, i);
      mt_sq_uinit(impl);
      return ret;
    }
    info("%s(%d), succ with shared queue mode\n", __func__, i);
  }

  return 0;
}

int mt_sq_uinit(struct mtl_main_impl* impl) {
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (impl->sq[i]) {
      sq_uinit(impl->sq[i]);
      mt_rte_free(impl->sq[i]);
      impl->sq[i] = NULL;
    }
  }

  return 0;
}
