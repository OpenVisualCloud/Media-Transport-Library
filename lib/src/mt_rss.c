/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_rss.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_util.h"

static const char* rss_mode_names[MT_RSS_MODE_MAX] = {
    "none",
    "l3",
    "l3_l4",
    "l3_l4_dst_port_only",
    "l3_da_l4_dst_port_only",
    "l4_dst_port_only",
};

static const char* rss_mode_name(enum mt_rss_mode rss_mode) {
  return rss_mode_names[rss_mode];
}

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

static enum mt_rss_mode rss_flow_mode(struct mt_rx_flow* flow) {
  if (flow->no_port_flow)
    return MT_RSS_MODE_L3;
  else if (flow->no_ip_flow)
    return MT_RSS_MODE_L4_DP_ONLY;
  else if (mt_is_multicast_ip(flow->dip_addr))
    return MT_RSS_MODE_L3_DA_L4_DP_ONLY;
  else
    return MT_RSS_MODE_L3_L4_DP_ONLY;
}

static int rss_flow_check(struct mtl_main_impl* impl, enum mtl_port port,
                          struct mt_rx_flow* flow) {
  enum mt_rss_mode sys_rss_mode = mt_get_rss_mode(impl, port);
  enum mt_rss_mode flow_rss_mode = rss_flow_mode(flow);

  if (mt_if(impl, port)->drv_type == MT_DRV_ENA) return 0;

  if (flow->sys_queue) return 0;

  if (sys_rss_mode == flow_rss_mode) return 0;

  err("%s(%d), flow require rss %s but sys is set to %s\n", __func__, port,
      rss_mode_name(flow_rss_mode), rss_mode_name(sys_rss_mode));
  return -EIO;
}

static uint32_t rss_flow_hash(struct mt_rx_flow* flow, enum mt_rss_mode rss) {
  uint32_t tuple[4];
  uint32_t len = 0;

  if (flow->sys_queue) return 0;

  uint32_t src_addr = RTE_IPV4(flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2],
                               flow->dip_addr[3]);
  uint32_t dst_addr = RTE_IPV4(flow->sip_addr[0], flow->sip_addr[1], flow->sip_addr[2],
                               flow->sip_addr[3]);
  uint32_t port = flow->dst_port;

  if (rss == MT_RSS_MODE_L3) {
    tuple[0] = src_addr;
    tuple[1] = dst_addr;
    len = 2;
  } else if (rss == MT_RSS_MODE_L3_L4) {
    tuple[0] = src_addr;
    tuple[1] = dst_addr;
    /* temp use dst_port now */
    tuple[2] = (port << 16) | port;
    len = 3;
  } else if (rss == MT_RSS_MODE_L3_L4_DP_ONLY) {
    tuple[0] = src_addr;
    tuple[1] = dst_addr;
    tuple[2] = (port << 16) | 0;
    len = 3;
  } else if (rss == MT_RSS_MODE_L3_DA_L4_DP_ONLY) {
    tuple[0] = src_addr;
    tuple[1] = (port << 16) | 0;
    len = 2;
  } else if (rss == MT_RSS_MODE_L4_DP_ONLY) {
    tuple[0] = (port << 16) | 0;
    len = 1;
  } else {
    err("%s(%d), not support rss mode %d\n", __func__, port, rss);
    return 0;
  }

  return mt_dev_softrss(tuple, len);
}

struct mt_rss_entry* mt_rss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rx_flow* flow) {
  if (!mt_has_rss(impl, port)) {
    err("%s(%d), rss not enabled\n", __func__, port);
    return NULL;
  }
  if (rss_flow_check(impl, port, flow) < 0) return NULL;

  struct mt_rss_impl* rss = rss_ctx_get(impl, port);
  uint32_t hash = rss_flow_hash(flow, mt_get_rss_mode(impl, port));
  uint16_t q = mt_dev_rss_hash_queue(impl, port, hash);
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
  /* todo: insert rss entry by rbtree? */
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

uint16_t mt_rss_burst(struct mt_rss_entry* entry, uint16_t nb_pkts) {
  struct mt_rss_impl* rss = entry->rss;
  uint16_t q = entry->queue_id;
  struct mt_rss_queue* rss_queue = &rss->rss_queues[q];
  struct rte_mbuf* pkts[nb_pkts];
  struct rte_mbuf* rss_pkts[nb_pkts];
  uint16_t rx;
  struct mt_rss_entry* rss_entry;
  struct mt_rss_entry* last_rss_entry = NULL;
  uint32_t hash;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;

  mt_pthread_mutex_lock(&rss_queue->mutex);
  rx = rte_eth_rx_burst(rss_queue->port_id, q, pkts, nb_pkts);
  if (rx) dbg("%s(%u), rx pkts %u\n", __func__, q, rx);
  int rss_pkts_nb = 0;
  for (uint16_t i = 0; i < rx; i++) {
    hash = pkts[i]->hash.rss;
    hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    ipv4 = &hdr->ipv4;
    dbg("%s(%u), pkt %u rss %u\n", __func__, q, i, hash);
    MT_TAILQ_FOREACH(rss_entry, &rss_queue->head, next) {
      /* check if this is the matched hash or sys entry */
      if ((hash == rss_entry->hash) ||
          (rss_entry->flow.sys_queue && (ipv4->next_proto_id != IPPROTO_UDP))) {
        if (rss_entry != last_rss_entry) {
          if (rss_pkts_nb)
            last_rss_entry->flow.cb(last_rss_entry->flow.priv, &rss_pkts[0], rss_pkts_nb);
          last_rss_entry = rss_entry;
          rss_pkts_nb = 0;
        }
        rss_pkts[rss_pkts_nb++] = pkts[i];
        break;
      }
    }
  }
  if (rss_pkts_nb)
    last_rss_entry->flow.cb(last_rss_entry->flow.priv, &rss_pkts[0], rss_pkts_nb);
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
    info("%s(%d), rss mode %s\n", __func__, i, rss_mode_name(mt_get_rss_mode(impl, i)));
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
