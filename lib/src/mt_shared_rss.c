/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_shared_rss.h"

#include "mt_log.h"
#include "mt_sch.h"
#include "mt_util.h"

#define MT_SRSS_BURST_SIZE (128)

#define UPDATE_ENTRY()                                                       \
  do {                                                                       \
    if (matched_pkts_nb)                                                     \
      last_srss_entry->flow.cb(last_srss_entry->flow.priv, &matched_pkts[0], \
                               matched_pkts_nb);                             \
    last_srss_entry = srss_entry;                                            \
    matched_pkts_nb = 0;                                                     \
  } while (0)

static int srss_tasklet_handler(void* priv) {
  struct mt_srss_impl* srss = priv;
  struct mtl_main_impl* impl = srss->parent;
  struct mt_interface* inf = mt_if(impl, srss->port);
  struct rte_mbuf *pkts[MT_SRSS_BURST_SIZE], *matched_pkts[MT_SRSS_BURST_SIZE];
  struct mt_srss_entry *srss_entry, *last_srss_entry;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;

  for (uint16_t queue = 0; queue < inf->max_rx_queues; queue++) {
    uint16_t matched_pkts_nb = 0;
    pthread_mutex_lock(&srss->mutex);
    uint16_t rx =
        rte_eth_rx_burst(mt_port_id(impl, srss->port), queue, pkts, MT_SRSS_BURST_SIZE);
    if (rx) {
      last_srss_entry = NULL;
      for (uint16_t i = 0; i < rx; i++) {
        srss_entry = NULL;
        hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
        if (hdr->eth.ether_type !=
            htons(RTE_ETHER_TYPE_IPV4)) { /* non ip, redirect to cni */
          UPDATE_ENTRY();
          if (srss->cni_entry)
            srss->cni_entry->flow.cb(srss->cni_entry->flow.priv, &pkts[i], 1);
          continue;
        }
        ipv4 = &hdr->ipv4;
        if (ipv4->next_proto_id != IPPROTO_UDP) { /* non udp, redirect to cni */
          UPDATE_ENTRY();
          if (srss->cni_entry)
            srss->cni_entry->flow.cb(srss->cni_entry->flow.priv, &pkts[i], 1);
          continue;
        }
        udp = &hdr->udp;
        MT_TAILQ_FOREACH(srss_entry, &srss->head, next) {
          bool ip_matched =
              mt_is_multicast_ip(srss_entry->flow.dip_addr)
                  ? (ipv4->dst_addr == *(uint32_t*)srss_entry->flow.dip_addr)
                  : (ipv4->src_addr == *(uint32_t*)srss_entry->flow.dip_addr);
          bool port_matched = ntohs(udp->dst_port) == srss_entry->flow.dst_port;
          if (ip_matched && port_matched) { /* match dst ip:port */
            if (srss_entry != last_srss_entry) UPDATE_ENTRY();
            matched_pkts[matched_pkts_nb++] = pkts[i];
            break;
          }
        }
        if (!srss_entry) { /* no match, redirect to cni */
          UPDATE_ENTRY();
          if (srss->cni_entry)
            srss->cni_entry->flow.cb(srss->cni_entry->flow.priv, &pkts[i], 1);
        }
      }
      if (matched_pkts_nb)
        last_srss_entry->flow.cb(last_srss_entry->flow.priv, &matched_pkts[0],
                                 matched_pkts_nb);
      rte_pktmbuf_free_bulk(&pkts[0], rx);
    }
    pthread_mutex_unlock(&srss->mutex);
  }

  return 0;
}

static int srss_tasklet_start(void* priv) { return 0; }
static int srss_tasklet_stop(void* priv) { return 0; }

struct mt_srss_entry* mt_srss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                  struct mt_rx_flow* flow) {
  struct mt_srss_impl* srss = impl->srss[port];
  struct mt_srss_entry* entry;
  MT_TAILQ_FOREACH(entry, &srss->head, next) {
    if (entry->flow.dst_port == flow->dst_port &&
        *(uint32_t*)entry->flow.dip_addr == *(uint32_t*)flow->dip_addr) {
      err("%s(%d), already has entry %u.%u.%u.%u:%u\n", __func__, port, flow->dip_addr[0],
          flow->dip_addr[1], flow->dip_addr[2], flow->dip_addr[3], flow->dst_port);
      return NULL;
    }
  }
  entry = mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), malloc fail\n", __func__, port);
    return NULL;
  }
  entry->flow = *flow;
  entry->srss = srss;
  pthread_mutex_lock(&srss->mutex);
  MT_TAILQ_INSERT_TAIL(&srss->head, entry, next);
  if (flow->sys_queue) srss->cni_entry = entry;
  pthread_mutex_unlock(&srss->mutex);

  info("%s(%d), entry %u.%u.%u.%u:(dst)%u succ\n", __func__, port, flow->dip_addr[0],
       flow->dip_addr[1], flow->dip_addr[2], flow->dip_addr[3], flow->dst_port);
  return entry;
}

int mt_srss_put(struct mt_srss_entry* entry) {
  struct mt_srss_impl* srss = entry->srss;

  pthread_mutex_lock(&srss->mutex);
  MT_TAILQ_REMOVE(&srss->head, entry, next);
  pthread_mutex_unlock(&srss->mutex);

  mt_rte_free(entry);
  return 0;
}

int mt_srss_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    if (!mt_has_srss(impl, i)) continue;
    impl->srss[i] = mt_rte_zmalloc_socket(sizeof(*impl->srss[i]), mt_socket_id(impl, i));
    if (!impl->srss[i]) {
      err("%s(%d), srss malloc fail\n", __func__, i);
      mt_srss_uinit(impl);
      return -ENOMEM;
    }
    struct mt_srss_impl* srss = impl->srss[i];

    if (pthread_mutex_init(&srss->mutex, NULL) != 0) {
      err("%s(%d), mutex init fail\n", __func__, i);
      mt_srss_uinit(impl);
      return -EIO;
    }

    struct mt_sch_impl* sch = mt_sch_get(impl, mt_if(impl, i)->link_speed,
                                         MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL);
    if (!sch) {
      err("%s(%d), get sch fail\n", __func__, i);
      mt_srss_uinit(impl);
      return -EIO;
    }

    struct mt_sch_tasklet_ops ops;
    memset(&ops, 0x0, sizeof(ops));
    ops.priv = srss;
    ops.name = "shared_rss";
    ops.start = srss_tasklet_start;
    ops.stop = srss_tasklet_stop;
    ops.handler = srss_tasklet_handler;

    srss->sch = sch;
    srss->port = i;
    srss->parent = impl;
    MT_TAILQ_INIT(&srss->head);

    srss->tasklet = mt_sch_register_tasklet(sch, &ops);
    if (!srss->tasklet) {
      err("%s, mt_sch_register_tasklet fail\n", __func__);
      mt_srss_uinit(impl);
      return -EIO;
    }

    info("%s(%d), succ with shared rss mode\n", __func__, i);
  }

  return 0;
}

int mt_srss_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_srss_impl* srss = impl->srss[i];
    if (srss) {
      if (srss->tasklet) {
        mt_sch_unregister_tasklet(srss->tasklet);
        srss->tasklet = NULL;
      }
      if (srss->sch) {
        mt_sch_put(srss->sch, mt_if(impl, i)->link_speed);
        srss->sch = NULL;
      }
      struct mt_srss_entry* entry;
      while ((entry = MT_TAILQ_FIRST(&srss->head))) {
        warn("%s, still has entry %p\n", __func__, entry);
        MT_TAILQ_REMOVE(&srss->head, entry, next);
        mt_rte_free(entry);
      }
      pthread_mutex_destroy(&srss->mutex);
      mt_rte_free(srss);
      impl->srss[i] = NULL;
    }
  }
  return 0;
}