/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_shared_rss.h"

#include "../dev/mt_af_xdp.h"
#include "../mt_log.h"
#include "../mt_sch.h"
#include "../mt_stat.h"
#include "../mt_util.h"

#define MT_SRSS_BURST_SIZE (128)
#define MT_SRSS_RING_PREFIX "SR_"

static inline struct mt_srss_list* srss_list_by_udp_port(struct mt_srss_impl* srss,
                                                         uint16_t port) {
  int l_idx = port % srss->lists_sz;
  return &srss->lists[l_idx];
}

static inline void srss_list_lock(struct mt_srss_list* list) {
  rte_spinlock_lock(&list->mutex);
}

/* return true if try lock succ */
static inline bool srss_list_try_lock(struct mt_srss_list* list) {
  int ret = rte_spinlock_trylock(&list->mutex);
  return ret ? true : false;
}

static inline void srss_list_unlock(struct mt_srss_list* list) {
  rte_spinlock_unlock(&list->mutex);
}

static inline void srss_entry_pkts_enqueue(struct mt_srss_entry* entry,
                                           struct rte_mbuf** pkts,
                                           const uint16_t nb_pkts) {
  /* use bulk version */
  unsigned int n = rte_ring_mp_enqueue_bulk(entry->ring, (void**)pkts, nb_pkts, NULL);
  entry->stat_enqueue_cnt += n;
  if (n == 0) {
    rte_pktmbuf_free_bulk(pkts, nb_pkts);
    entry->stat_enqueue_fail_cnt += nb_pkts;
  }
}

#define UPDATE_ENTRY()                                                             \
  do {                                                                             \
    if (matched_pkts_nb)                                                           \
      srss_entry_pkts_enqueue(last_srss_entry, &matched_pkts[0], matched_pkts_nb); \
    last_srss_entry = srss_entry;                                                  \
    matched_pkts_nb = 0;                                                           \
  } while (0)

#define CNI_ENQUEUE()                                        \
  do {                                                       \
    if (srss->cni_entry)                                     \
      srss_entry_pkts_enqueue(srss->cni_entry, &pkts[i], 1); \
    else                                                     \
      rte_pktmbuf_free(pkts[i]);                             \
  } while (0)

#define UPDATE_LIST()                           \
  do {                                          \
    if (last_list) srss_list_unlock(last_list); \
    srss_list_lock(list);                       \
    last_list = list;                           \
  } while (0)

static int srss_sch_tasklet_handler(void* priv) {
  struct mt_srss_sch* srss_sch = priv;
  struct mt_srss_impl* srss = srss_sch->parent;
  struct mtl_main_impl* impl = srss->parent;
  struct rte_mbuf *pkts[MT_SRSS_BURST_SIZE], *matched_pkts[MT_SRSS_BURST_SIZE];
  struct mt_srss_entry *srss_entry, *last_srss_entry;
  struct mt_srss_list *list = NULL, *last_list = NULL;
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;

  for (uint16_t queue = srss_sch->q_start; queue < srss_sch->q_end; queue++) {
    uint16_t matched_pkts_nb = 0;

    uint16_t rx;
    if (srss->xdps) {
      rx = mt_rx_xdp_burst(srss->xdps[queue], pkts, MT_SRSS_BURST_SIZE);
    } else {
      rx =
          rte_eth_rx_burst(mt_port_id(impl, srss->port), queue, pkts, MT_SRSS_BURST_SIZE);
    }
    if (!rx) continue;
    srss_sch->stat_pkts_rx += rx;

    last_srss_entry = NULL;
    for (uint16_t i = 0; i < rx; i++) {
      srss_entry = NULL;
      hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
      if (hdr->eth.ether_type !=
          htons(RTE_ETHER_TYPE_IPV4)) { /* non ip, redirect to cni */
        UPDATE_ENTRY();
        CNI_ENQUEUE();
        continue;
      }
      ipv4 = &hdr->ipv4;
      if (ipv4->next_proto_id != IPPROTO_UDP) { /* non udp, redirect to cni */
        UPDATE_ENTRY();
        CNI_ENQUEUE();
        continue;
      }

      /* get the list, lock if it's a list */
      list = srss_list_by_udp_port(srss, ntohs(hdr->udp.dst_port));
      if (list != last_list) {
        UPDATE_LIST();
      }
      /* check if match any entry in current list */
      struct mt_srss_entrys_list* head = &list->entrys_list;
      MT_TAILQ_FOREACH(srss_entry, head, next) {
        bool matched = mt_udp_matched(&srss_entry->flow, hdr);
        if (matched) {
          if (srss_entry != last_srss_entry) UPDATE_ENTRY();
          matched_pkts[matched_pkts_nb++] = pkts[i];
          break;
        }
      }

      if (!srss_entry) { /* no match, redirect to cni */
        UPDATE_ENTRY();
        CNI_ENQUEUE();
      }
    }
    if (matched_pkts_nb)
      srss_entry_pkts_enqueue(last_srss_entry, &matched_pkts[0], matched_pkts_nb);
  }

  if (last_list) srss_list_unlock(last_list);

  return 0;
}

static void* srss_traffic_thread(void* arg) {
  struct mt_srss_impl* srss = arg;

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&srss->stop_thread) == 0) {
    for (int s_idx = 0; s_idx < srss->schs_cnt; s_idx++) {
      struct mt_srss_sch* srss_sch = &srss->schs[s_idx];

      srss_sch_tasklet_handler(srss_sch);
    }
    mt_sleep_ms(1);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static int srss_traffic_thread_start(struct mt_srss_impl* srss) {
  int ret;

  if (srss->tid) {
    err("%s, srss_traffic thread already start\n", __func__);
    return 0;
  }

  rte_atomic32_set(&srss->stop_thread, 0);
  ret = pthread_create(&srss->tid, NULL, srss_traffic_thread, srss);
  if (ret < 0) {
    err("%s, srss_traffic thread create fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

static int srss_traffic_thread_stop(struct mt_srss_impl* srss) {
  rte_atomic32_set(&srss->stop_thread, 1);
  if (srss->tid) {
    pthread_join(srss->tid, NULL);
    srss->tid = 0;
  }

  return 0;
}

static int srss_sch_tasklet_start(void* priv) {
  struct mt_srss_sch* srss_sch = priv;

  if (srss_sch->idx == 0) {
    /* tasklet will take over the srss thread */
    srss_traffic_thread_stop(srss_sch->parent);
  }

  return 0;
}

static int srss_sch_tasklet_stop(void* priv) {
  struct mt_srss_sch* srss_sch = priv;

  if (srss_sch->idx == 0) {
    srss_traffic_thread_start(srss_sch->parent);
  }

  return 0;
}

static int srss_stat(void* priv) {
  struct mt_srss_impl* srss = priv;
  enum mtl_port port = srss->port;
  struct mt_srss_entry* entry;
  int idx;

  for (int l_idx = 0; l_idx < srss->lists_sz; l_idx++) {
    struct mt_srss_list* list = &srss->lists[l_idx];
    if (!srss_list_try_lock(list)) {
      continue;
    }

    struct mt_srss_entrys_list* head = &list->entrys_list;
    MT_TAILQ_FOREACH(entry, head, next) {
      idx = entry->idx;
      notice("%s(%d,%d,%d), enqueue %u dequeue %u\n", __func__, port, l_idx, idx,
             entry->stat_enqueue_cnt, entry->stat_dequeue_cnt);
      entry->stat_enqueue_cnt = 0;
      entry->stat_dequeue_cnt = 0;
      if (entry->stat_enqueue_fail_cnt) {
        warn("%s(%d,%d,%d), enqueue fail %u\n", __func__, port, l_idx, idx,
             entry->stat_enqueue_fail_cnt);
        entry->stat_enqueue_fail_cnt = 0;
      }
    }
    srss_list_unlock(list);
  }

  for (int s_idx = 0; s_idx < srss->schs_cnt; s_idx++) {
    struct mt_srss_sch* srss_sch = &srss->schs[s_idx];

    notice("%s(%d,%d), pkts rx %u\n", __func__, port, s_idx, srss_sch->stat_pkts_rx);
    srss_sch->stat_pkts_rx = 0;
  }

  return 0;
}

static int srss_uinit_xdp(struct mt_srss_impl* srss) {
  struct mt_rx_xdp_entry** xdps = srss->xdps;
  if (!xdps) return 0;

  for (uint16_t queue = 0; queue < srss->nb_rx_q; queue++) {
    if (xdps[queue]) {
      mt_rx_xdp_put(xdps[queue]);
      xdps[queue] = NULL;
    }
  }

  mt_rte_free(xdps);
  srss->xdps = NULL;
  return 0;
}

static int srss_init_xdp(struct mt_srss_impl* srss) {
  struct mtl_main_impl* impl = srss->parent;
  enum mtl_port port = srss->port;

  srss->xdps = mt_rte_zmalloc_socket(sizeof(*srss->xdps) * srss->nb_rx_q,
                                     mt_socket_id(impl, port));
  if (!srss->xdps) {
    err("%s(%d), xdps malloc fail\n", __func__, port);
    return -ENOMEM;
  }

  struct mt_rxq_flow flow;
  memset(&flow, 0, sizeof(flow));
  struct mt_rx_xdp_get_args args;
  memset(&args, 0, sizeof(args));
  args.queue_match = true;
  args.skip_flow = true;
  args.skip_udp_port_check = true;

  for (uint16_t queue = 0; queue < srss->nb_rx_q; queue++) {
    /* get a 1:1 mapped queue */
    args.queue_id = queue;
    srss->xdps[queue] = mt_rx_xdp_get(impl, port, &flow, &args);
    if (!srss->xdps[queue]) {
      err("%s(%d), xdp queue %u get fail\n", __func__, port, queue);
      srss_uinit_xdp(srss);
      return -ENOMEM;
    }
  }

  return 0;
}

struct mt_srss_entry* mt_srss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                  struct mt_rxq_flow* flow) {
  struct mt_srss_impl* srss = impl->srss[port];
  int idx = srss->entry_idx;
  struct mt_srss_entry* entry;
  struct mt_srss_list* list;
  struct mt_srss_entrys_list* head;

  if (!mt_has_srss(impl, port)) {
    err("%s(%d,%d), shared rss not enabled\n", __func__, port, idx);
    return NULL;
  }

  list = srss_list_by_udp_port(srss, flow->dst_port);
  head = &list->entrys_list;

  srss_list_lock(list);
  MT_TAILQ_FOREACH(entry, head, next) {
    /* todo: check with flow flags */
    if (entry->flow.dst_port == flow->dst_port &&
        *(uint32_t*)entry->flow.dip_addr == *(uint32_t*)flow->dip_addr) {
      err("%s(%d,%d), already has entry %u.%u.%u.%u:%u\n", __func__, port, idx,
          flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2], flow->dip_addr[3],
          flow->dst_port);
      srss_list_unlock(list);
      return NULL;
    }
  }
  srss_list_unlock(list);

  entry = mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d,%d), malloc fail\n", __func__, port, idx);
    return NULL;
  }

  /* ring create */
  char ring_name[32];
  snprintf(ring_name, 32, "%sP%d_%d", MT_SRSS_RING_PREFIX, port, idx);
  entry->ring =
      rte_ring_create(ring_name, 512, mt_socket_id(impl, MTL_PORT_P), RING_F_SC_DEQ);
  if (!entry->ring) {
    err("%s(%d,%d), ring create fail\n", __func__, port, idx);
    mt_rte_free(entry);
    return NULL;
  }

  entry->flow = *flow;
  entry->srss = srss;
  entry->idx = idx;

  srss_list_lock(list);
  MT_TAILQ_INSERT_TAIL(head, entry, next);
  if (flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE) srss->cni_entry = entry;
  srss->entry_idx++;
  srss_list_unlock(list);

  info("%s(%d), entry %u.%u.%u.%u:(dst)%u on %d of list %d\n", __func__, port,
       flow->dip_addr[0], flow->dip_addr[1], flow->dip_addr[2], flow->dip_addr[3],
       flow->dst_port, idx, list->idx);
  return entry;
}

int mt_srss_put(struct mt_srss_entry* entry) {
  struct mt_srss_impl* srss = entry->srss;
  enum mtl_port port = srss->port;
  struct mt_srss_list* list = srss_list_by_udp_port(srss, entry->flow.dst_port);
  struct mt_srss_entrys_list* head = &list->entrys_list;

  /* check if it's a known entry in the list */
  struct mt_srss_entry* temp_entry;
  bool found = false;
  srss_list_lock(list);
  MT_TAILQ_FOREACH(temp_entry, head, next) {
    if (entry == temp_entry) {
      found = true;
      break;
    }
  }
  srss_list_unlock(list);
  if (!found) {
    info("%s(%d), unknow entry %p on %d\n", __func__, port, entry, entry->idx);
    return -EIO;
  }

  if (srss->cni_entry == entry) {
    info("%s(%d), delete cni_entry %d\n", __func__, port, entry->idx);
    srss->cni_entry = NULL;
  }

  srss_list_lock(list);
  MT_TAILQ_REMOVE(head, entry, next);
  srss_list_unlock(list);

  if (entry->ring) {
    mt_ring_dequeue_clean(entry->ring);
    rte_ring_free(entry->ring);
    entry->ring = NULL;
  }

  info("%s(%d), succ on %d\n", __func__, port, entry->idx);
  mt_rte_free(entry);
  return 0;
}

int mt_srss_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);
  int ret;

  for (int port = 0; port < num_ports; port++) {
    if (!mt_has_srss(impl, port)) continue;

    impl->srss[port] =
        mt_rte_zmalloc_socket(sizeof(*impl->srss[port]), mt_socket_id(impl, port));
    if (!impl->srss[port]) {
      err("%s(%d), srss malloc fail\n", __func__, port);
      mt_srss_uinit(impl);
      return -ENOMEM;
    }
    struct mt_srss_impl* srss = impl->srss[port];

    srss->port = port;
    srss->parent = impl;
    srss->queue_mode =
        mt_pmd_is_native_af_xdp(impl, port) ? MT_QUEUE_MODE_XDP : MT_QUEUE_MODE_DPDK;
    srss->nb_rx_q = mt_if(impl, port)->nb_rx_q;

    srss->lists_sz = 64 - 1; /* use odd count for better distribution */
    srss->lists = mt_rte_zmalloc_socket(sizeof(*srss->lists) * srss->lists_sz,
                                        mt_socket_id(impl, port));
    if (!srss->lists) {
      err("%s(%d), lists malloc fail\n", __func__, port);
      mt_srss_uinit(impl);
      return -ENOMEM;
    }
    for (int l_idx = 0; l_idx < srss->lists_sz; l_idx++) {
      struct mt_srss_list* list = &srss->lists[l_idx];

      list->idx = l_idx;
      MT_TAILQ_INIT(&list->entrys_list);
      rte_spinlock_init(&list->mutex);
    }

    if (srss->queue_mode == MT_QUEUE_MODE_XDP) {
      ret = srss_init_xdp(srss);
      if (ret < 0) {
        err("%s(%d), init xdp fail\n", __func__, port);
        mt_srss_uinit(impl);
        return ret;
      }
    }

    srss->schs_cnt = p->rss_sch_nb[port];
    if (!srss->schs_cnt) srss->schs_cnt = 1;
    if (srss->schs_cnt > srss->nb_rx_q) srss->schs_cnt = srss->nb_rx_q;
    srss->schs = mt_rte_zmalloc_socket(sizeof(*srss->schs) * srss->schs_cnt,
                                       mt_socket_id(impl, port));
    if (!srss->schs) {
      err("%s(%d), schs malloc fail\n", __func__, port);
      mt_srss_uinit(impl);
      return -ENOMEM;
    }

    /* making sure schs_cnt is not zero to prevent divide-by-zero */
    if (!srss->schs_cnt) {
      err("%s(%d), schs_cnt is zero\n", __func__, port);
      mt_srss_uinit(impl);
      return -EINVAL;
    }
    mt_sch_mask_t sch_mask = MT_SCH_MASK_ALL;
    uint16_t q_idx = 0;
    uint16_t q_per_sch = srss->nb_rx_q / srss->schs_cnt;
    uint16_t q_remaining = srss->nb_rx_q % srss->schs_cnt;
    for (int s_idx = 0; s_idx < srss->schs_cnt; s_idx++) {
      struct mt_srss_sch* srss_sch = &srss->schs[s_idx];
      srss_sch->parent = srss;
      srss_sch->idx = s_idx;
      srss_sch->quota_mps = 0;
      srss_sch->q_start = q_idx;
      uint16_t q_end = srss_sch->q_start + q_per_sch;
      if (s_idx < q_remaining) q_end++;
      srss_sch->q_end = q_end;
      q_idx = q_end;

      struct mtl_sch_impl* sch =
          mt_sch_get(impl, srss_sch->quota_mps, MT_SCH_TYPE_DEFAULT, sch_mask);
      if (!sch) {
        err("%s(%d), get sch fail on %d\n", __func__, port, s_idx);
        mt_srss_uinit(impl);
        return -EIO;
      }
      srss_sch->sch = sch;

      struct mtl_tasklet_ops ops;
      memset(&ops, 0x0, sizeof(ops));
      ops.priv = srss_sch;
      ops.name = "shared_rss";
      ops.start = srss_sch_tasklet_start;
      ops.stop = srss_sch_tasklet_stop;
      ops.handler = srss_sch_tasklet_handler;
      srss_sch->tasklet = mtl_sch_register_tasklet(sch, &ops);
      if (!srss_sch->tasklet) {
        err("%s(%d), register tasklet fail on %d\n", __func__, port, s_idx);
        mt_srss_uinit(impl);
        return -EIO;
      }

      sch_mask &= ~(MTL_BIT64(sch->idx));
      info("%s(%d), sch %d with queues start %u end %u\n", __func__, port, s_idx,
           srss_sch->q_start, srss_sch->q_end);
    }

    rte_atomic32_set(&srss->stop_thread, 0);
    ret = srss_traffic_thread_start(srss);
    if (ret < 0) {
      err("%s(%d), traffic thread start fail\n", __func__, port);
      mt_srss_uinit(impl);
      return ret;
    }

    mt_stat_register(impl, srss_stat, srss, "srss");

    info("%s(%d), succ with shared rss mode\n", __func__, port);
  }

  return 0;
}

int mt_srss_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_srss_impl* srss = impl->srss[i];
    if (!srss) continue;

    mt_stat_unregister(impl, srss_stat, srss);
    srss_traffic_thread_stop(srss);

    if (srss->schs) {
      for (int s_idx = 0; s_idx < srss->schs_cnt; s_idx++) {
        struct mt_srss_sch* srss_sch = &srss->schs[s_idx];
        if (srss_sch->tasklet) {
          mtl_sch_unregister_tasklet(srss_sch->tasklet);
          srss_sch->tasklet = NULL;
        }
        if (srss_sch->sch) {
          mt_sch_put(srss_sch->sch, srss_sch->quota_mps);
          srss_sch->sch = NULL;
        }
      }

      mt_rte_free(srss->schs);
      srss->schs = NULL;
    }

    if (srss->lists) {
      for (int l_idx = 0; l_idx < srss->lists_sz; l_idx++) {
        struct mt_srss_list* list = &srss->lists[l_idx];

        struct mt_srss_entrys_list* head = &list->entrys_list;
        struct mt_srss_entry* entry;
        while ((entry = MT_TAILQ_FIRST(head))) {
          warn("%s(%d), still has entry %p on list_heads %d\n", __func__, i, entry,
               l_idx);
          MT_TAILQ_REMOVE(head, entry, next);
          mt_rte_free(entry);
        }
      }

      mt_rte_free(srss->lists);
      srss->lists = NULL;
    }

    srss_uinit_xdp(srss);

    mt_rte_free(srss);
    impl->srss[i] = NULL;
  }

  return 0;
}