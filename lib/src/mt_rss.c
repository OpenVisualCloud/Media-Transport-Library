/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_rss.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_util.h"

static inline struct mt_rss_impl* rss_ctx_get(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->rss[port];
}

static int rss_init(struct mtl_main_impl* impl, struct mt_rss_impl* rss) {
  enum mtl_port port = rss->port;
  int soc_id = mt_socket_id(impl, port);
  struct mt_rss_queue* rss_queue;

  rss->rss_queues =
      mt_rte_zmalloc_socket(sizeof(*rss->rss_queues) * rss->max_rss_queues, soc_id);
  if (!rss->rss_queues) {
    err("%s(%d), rss_queues alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < rss->max_rss_queues; q++) {
    rss_queue = &rss->rss_queues[q];
    rss_queue->queue_id = q;
    rss_queue->port_id = mt_port_id(impl, port);
    mt_pthread_mutex_init(&rss_queue->mutex, NULL);
    MT_TAILQ_INIT(&rss_queue->head);
  }

  return 0;
}

static int rss_uinit(struct mt_rss_impl* rss) {
  struct mt_rss_queue* rss_queue;
  struct mt_rss_entry* entry;

  if (rss->rss_queues) {
    for (uint16_t q = 0; q < rss->max_rss_queues; q++) {
      rss_queue = &rss->rss_queues[q];

      /* check if any not free */
      while ((entry = MT_TAILQ_FIRST(&rss_queue->head))) {
        warn("%s(%u), entry %p not free\n", __func__, q, entry->flow.priv);
        MT_TAILQ_REMOVE(&rss_queue->head, entry, next);
        mt_rte_free(entry);
      }
      mt_pthread_mutex_destroy(&rss->rss_queues[q].mutex);
    }
    mt_rte_free(rss->rss_queues);
    rss->rss_queues = NULL;
  }

  return 0;
}

static uint32_t rss_flow_hash(struct mt_rss_flow* flow) {
  struct rte_ipv4_tuple tuple;

  if (flow->no_udp) return 0;

  rte_memcpy(&tuple.src_addr, flow->sip_addr, MTL_IP_ADDR_LEN);
  rte_memcpy(&tuple.dst_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
  tuple.sport = htons(flow->src_port);
  tuple.dport = htons(flow->dst_port);
  return mt_dev_softrss((uint32_t*)&tuple, RTE_THASH_V4_L4_LEN);
}

struct mt_rss_entry* mt_rss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rss_flow* flow) {
  if (!mt_has_rss(impl, port)) {
    err("%s(%d), rss not enabled\n", __func__, port);
    return NULL;
  }

  struct mt_rss_impl* rss = rss_ctx_get(impl, port);
  uint32_t hash = rss_flow_hash(flow);
  uint16_t q = (hash % RTE_ETH_RETA_GROUP_SIZE) % rss->max_rss_queues;
  struct mt_rss_queue* rss_queue = &rss->rss_queues[q];
  struct mt_rss_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%u), entry malloc fail\n", __func__, q);
    return NULL;
  }
  entry->queue_id = q;
  entry->rss = rss;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));
  entry->hash = hash;

  mt_pthread_mutex_lock(&rss_queue->mutex);
  MT_TAILQ_INSERT_TAIL(&rss_queue->head, entry, next);
  mt_pthread_mutex_unlock(&rss_queue->mutex);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), q %u ip %u.%u.%u.%u, port %u hash %u\n", __func__, port, q, ip[0], ip[1],
       ip[2], ip[3], flow->dst_port, hash);
  return entry;
}

int mt_rss_put(struct mt_rss_entry* entry) {
  struct mt_rss_impl* rss = entry->rss;
  struct mt_rss_queue* rss_queue = &rss->rss_queues[entry->queue_id];

  mt_pthread_mutex_lock(&rss_queue->mutex);
  MT_TAILQ_REMOVE(&rss_queue->head, entry, next);
  mt_pthread_mutex_unlock(&rss_queue->mutex);

  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rss_rx_burst(struct mt_rss_entry* entry, uint16_t nb_pkts) {
  struct mt_rss_impl* rss = entry->rss;
  uint16_t q = entry->queue_id;
  struct mt_rss_queue* rss_queue = &rss->rss_queues[q];
  struct rte_mbuf* pkts[nb_pkts];
  uint16_t rx;
  struct mt_rss_entry* rss_entry;
  uint32_t hash;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;

  mt_pthread_mutex_lock(&rss_queue->mutex);
  rx = rte_eth_rx_burst(rss_queue->port_id, q, pkts, nb_pkts);
  if (rx) dbg("%s(%u), rx pkts %u\n", __func__, q, rx);
  for (uint16_t i = 0; i < rx; i++) {
    hash = pkts[i]->hash.rss;
    hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    ipv4 = &hdr->ipv4;
    dbg("%s(%u), pkt %u rss %u\n", __func__, q, i, hash);
    MT_TAILQ_FOREACH(rss_entry, &rss_queue->head, next) {
      /* check if this is the matched hash or sys entry */
      if ((hash == rss_entry->hash) ||
          (rss_entry->flow.no_udp && (ipv4->next_proto_id != IPPROTO_UDP))) {
        rss_entry->flow.cb(rss_entry->flow.priv, &pkts[i], 1);
      }
    }
  }
  mt_pthread_mutex_unlock(&rss_queue->mutex);

  rte_pktmbuf_free_bulk(&pkts[0], rx);

#if 0
  /* debug */
  for (q = 1; q < rss->max_rss_queues; q++) {
    rx = rte_eth_rx_burst(rss_queue->port_id, q, pkts, nb_pkts);
    for (uint16_t i = 0; i < rx; i++) {
      info("%s(%u), pkt %u rss %u\n", __func__, q, i, pkts[i]->hash.rss);
      // mt_mbuf_dump_hdr(0, 0, "rss", pkts[i]);
    }
    rte_pktmbuf_free_bulk(&pkts[0], rx);
  }
#endif

  return rx;
}

int mt_rss_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    if (!mt_has_rss(impl, i)) continue;
    impl->rss[i] = mt_rte_zmalloc_socket(sizeof(*impl->rss[i]), mt_socket_id(impl, i));
    if (!impl->rss[i]) {
      err("%s(%d), rss malloc fail\n", __func__, i);
      mt_rss_uinit(impl);
      return -ENOMEM;
    }
    impl->rss[i]->port = i;
    impl->rss[i]->max_rss_queues = mt_if(impl, i)->max_rx_queues;
    ret = rss_init(impl, impl->rss[i]);
    if (ret < 0) {
      err("%s(%d), rss init fail\n", __func__, i);
      mt_rss_uinit(impl);
      return ret;
    }
    info("%s(%d), succ, rss mode %d\n", __func__, i, impl->rss_mode);
  }

  return 0;
}

int mt_rss_uinit(struct mtl_main_impl* impl) {
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (impl->rss[i]) {
      rss_uinit(impl->rss[i]);
      mt_rte_free(impl->rss[i]);
      impl->rss[i] = NULL;
    }
  }

  return 0;
}
