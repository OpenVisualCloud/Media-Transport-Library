/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_shared_rss.h"

#include <rte_config.h>
#include <rte_ether.h>

#include "mt_cni.h"
#include "mt_log.h"
#include "mt_sch.h"
#include "src/mt_rss.h"

#define MT_SRSS_BURST_SIZE (128)

static int srss_tasklet_handler(void* priv) {
  struct mt_srss_impl* srss = priv;
  struct mtl_main_impl* impl = srss->parent;
  struct mt_interface* inf = mt_if(impl, srss->port);

  for (int queue = 0; queue < inf->max_rx_queues; queue++) {
    struct rte_mbuf *pkts[MT_SRSS_BURST_SIZE], *rss_pkts[MT_SRSS_BURST_SIZE];
    struct mt_srss_entry *srss_entry, *last_srss_entry = NULL;
    int rss_pkts_nb = 0;
    pthread_mutex_lock(&srss->mutex);
    uint16_t rx =
        rte_eth_rx_burst(mt_port_id(impl, srss->port), queue, pkts, MT_SRSS_BURST_SIZE);
    if (rx) {
      dbg("%s(%d), rx pkts %u\n", __func__, queue, rx);
      for (uint16_t i = 0; i < rx; i++) {
        struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
        if (hdr->eth.ether_type != htons(RTE_ETHER_TYPE_IPV4)) continue;
        struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
        if (ipv4->next_proto_id != IPPROTO_UDP) continue;
        struct rte_udp_hdr* udp = &hdr->udp;
        MT_TAILQ_FOREACH(srss_entry, &srss->head, next) {
          if (ipv4->dst_addr == *(uint32_t*)srss_entry->flow.dip_addr &&
              ntohs(udp->dst_port) == srss_entry->flow.dst_port) {
            if (srss_entry != last_srss_entry) {
              if (rss_pkts_nb)
                last_srss_entry->flow.cb(last_srss_entry->flow.priv, &rss_pkts[0],
                                         rss_pkts_nb);
              last_srss_entry = srss_entry;
              rss_pkts_nb = 0;
            }
            rss_pkts[rss_pkts_nb++] = pkts[i];
            break;
          }
        }
      }
      if (rss_pkts_nb)
        last_srss_entry->flow.cb(last_srss_entry->flow.priv, &rss_pkts[0], rss_pkts_nb);
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
      return entry;
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
  pthread_mutex_unlock(&srss->mutex);
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
    if (!mt_has_rss(impl, i)) continue;
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

    struct mt_sch_impl* sch =
        mt_sch_get(impl, 100000 /*nic cap*/, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL);
    if (!sch) {
      err("%s, get sch fail\n", __func__);
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

    srss->tasklet = mt_sch_register_tasklet(sch, &ops);
    if (!srss->tasklet) {
      err("%s, mt_sch_register_tasklet fail\n", __func__);
      mt_srss_uinit(impl);
      return -EIO;
    }
    srss->sch = sch;
    srss->port = i;
    srss->parent = impl;
    MT_TAILQ_INIT(&srss->head);
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
      if (mt_sch_put(srss->sch, 1000000) < 0)
        err("%s(%d), mt_sch_put fail\n", __func__, i);
      struct mt_srss_entry* entry;
      while ((entry = MT_TAILQ_FIRST(&srss->head))) {
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