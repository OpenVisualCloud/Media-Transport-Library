/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_cni.h"

#include "datapath/mt_queue.h"
#include "mt_arp.h"
#include "mt_dhcp.h"
#include "mt_kni.h"
// #define DEBUG
#include "mt_dhcp.h"
#include "mt_log.h"
#include "mt_ptp.h"
#include "mt_sch.h"
#include "mt_stat.h"
#include "mt_tap.h"
#include "mt_util.h"

#define MT_CSQ_RING_PREFIX "CSQ_"

static inline struct mt_cni_entry* cni_get_entry(struct mtl_main_impl* impl,
                                                 enum mtl_port port) {
  return &mt_get_cni(impl)->entries[port];
}

/* return true if try lock succ */
static inline bool csq_try_lock(struct mt_cni_entry* cni) {
  int ret = rte_spinlock_trylock(&cni->csq_lock);
  return ret ? true : false;
}

static inline void csq_lock(struct mt_cni_entry* cni) {
  rte_spinlock_lock(&cni->csq_lock);
}

static inline void csq_unlock(struct mt_cni_entry* cni) {
  rte_spinlock_unlock(&cni->csq_lock);
}

static int csq_entry_free(struct mt_csq_entry* entry) {
  if (entry->ring) {
    mt_ring_dequeue_clean(entry->ring);
    rte_ring_free(entry->ring);
  }
  info("%s(%d), succ on idx %d\n", __func__, entry->parent->port, entry->idx);
  mt_rte_free(entry);
  return 0;
}

static void cni_udp_detect_dump(struct mt_cni_entry* cni,
                                struct mt_cni_udp_detect_entry* entry) {
  uint8_t* sip = (uint8_t*)&entry->tuple[0];
  uint8_t* dip = (uint8_t*)&entry->tuple[1];
  uint32_t udp_port = entry->tuple[2];
  uint16_t src_port = ntohs((uint16_t)udp_port);
  uint16_t dst_port = ntohs((uint16_t)(udp_port >> 16));
  info("%s(%d), sip: %d.%d.%d.%d, dip: %d.%d.%d.%d, src_port %u dst_port %d, pkt %d\n",
       __func__, cni->port, sip[0], sip[1], sip[2], sip[3], dip[0], dip[1], dip[2],
       dip[3], src_port, dst_port, entry->pkt_cnt);
}

static int cni_udp_detect_analyses(struct mt_cni_entry* cni, struct mt_udp_hdr* hdr) {
  enum mtl_port port = cni->port;
  uint32_t tuple[3];
  struct mt_cni_udp_detect_entry* entry;
  struct mt_cni_udp_detect_list* list = &cni->udp_detect;
  uint8_t* dip = (uint8_t*)&hdr->ipv4.dst_addr;

  if (!mt_is_multicast_ip(dip) &&
      memcmp(mt_sip_addr(cni->impl, port), dip, MTL_IP_ADDR_LEN)) {
    dbg("%s(%d), not our ip %u.%u.%u.%u\n", __func__, port, dip[0], dip[1], dip[2],
        dip[3]);
    return -EINVAL;
  }

  rte_memcpy(tuple, &hdr->ipv4.src_addr, sizeof(tuple));

  /* search if it's a known udp stream */
  MT_TAILQ_FOREACH(entry, list, next) {
    if (0 == memcmp(tuple, entry->tuple, sizeof(tuple))) {
      entry->pkt_cnt++;
      return 0; /* found */
    }
  }

  entry = mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(cni->impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return -ENOMEM;
  }
  rte_memcpy(entry->tuple, tuple, sizeof(tuple));
  /* add to list */
  MT_TAILQ_INSERT_TAIL(list, entry, next);
  info("%s(%d), new udp stream:\n", __func__, port);
  cni_udp_detect_dump(cni, entry);

  return 0;
}

static int csq_stat(struct mt_cni_entry* cni) {
  enum mtl_port port = cni->port;
  struct mt_csq_entry* csq = NULL;
  int idx;

  if (!csq_try_lock(cni)) {
    notice("%s(%d), get lock fail\n", __func__, port);
    return 0;
  }
  MT_TAILQ_FOREACH(csq, &cni->csq_queues, next) {
    idx = csq->idx;
    notice("%s(%d,%d), enqueue %u dequeue %u\n", __func__, port, idx,
           csq->stat_enqueue_cnt, csq->stat_dequeue_cnt);
    csq->stat_enqueue_cnt = 0;
    csq->stat_dequeue_cnt = 0;
    if (csq->stat_enqueue_fail_cnt) {
      warn("%s(%d,%d), enqueue fail %u\n", __func__, port, idx,
           csq->stat_enqueue_fail_cnt);
      csq->stat_enqueue_fail_cnt = 0;
    }
  }
  csq_unlock(cni);

  return 0;
}

static int cni_burst_to_kernel(struct mt_cni_entry* cni, struct rte_mbuf* m) {
  struct mtl_main_impl* impl = cni->impl;
  enum mtl_port port = cni->port;
  struct mt_interface* inf = mt_if(impl, port);
  if (!inf->virtio_port_active) return 0;

  int ret = rte_eth_tx_burst(inf->virtio_port_id, 0, &m, 1);
  if (ret < 1) {
    warn("%s(%d), forward packet to kernel fail\n", __func__, port);
    return -EIO;
  }
  cni->virtio_rx_cnt++;

  return 0;
}

static int cni_udp_handle(struct mt_cni_entry* cni, struct rte_mbuf* m) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct mt_csq_entry* csq = NULL;
  int ret;

  hdr = rte_pktmbuf_mtod(m, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  csq_lock(cni);
  MT_TAILQ_FOREACH(csq, &cni->csq_queues, next) {
    bool ip_matched;
    if (csq->flow.no_ip_flow) {
      ip_matched = true;
    } else {
      ip_matched = mt_is_multicast_ip(csq->flow.dip_addr)
                       ? (ipv4->dst_addr == *(uint32_t*)csq->flow.dip_addr)
                       : (ipv4->src_addr == *(uint32_t*)csq->flow.dip_addr);
    }
    bool port_matched;
    if (csq->flow.no_port_flow) {
      port_matched = true;
    } else {
      port_matched = ntohs(udp->dst_port) == csq->flow.dst_port;
    }
    if (ip_matched && port_matched) { /* match dst ip:port */
      ret = rte_ring_sp_enqueue(csq->ring, m);
      if (ret < 0) {
        csq->stat_enqueue_fail_cnt++;
      } else {
        rte_mbuf_refcnt_update(m, 1);
        csq->stat_enqueue_cnt++;
      }
      csq_unlock(cni);
      return 0;
    }
  }
  csq_unlock(cni);

  /* unmatched UDP packets fallback to kernel */
  cni_burst_to_kernel(cni, m);

  /* analyses if it's a UDP stream, for debug usage */
  cni_udp_detect_analyses(cni, hdr);
  return 0;
}

static int cni_rx_handle(struct mt_cni_entry* cni, struct rte_mbuf* m) {
  struct mtl_main_impl* impl = cni->impl;
  enum mtl_port port = cni->port;
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  struct mt_dhcp_impl* dhcp = mt_get_dhcp(impl, port);
  struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  uint16_t ether_type, src_port;
  struct rte_vlan_hdr* vlan_header;
  bool vlan = false;
  struct mt_ptp_header* ptp_hdr;
  struct rte_arp_hdr* arp_hdr;
  struct mt_dhcp_hdr* dhcp_hdr;
  struct mt_ipv4_udp* ipv4_hdr;
  size_t hdr_offset = sizeof(struct rte_ether_hdr);

  // mt_mbuf_dump_hdr(port, 0, "cni_rx", m);

  /* vlan check */
  ether_type = ntohs(eth_hdr->ether_type);
  if (ether_type == RTE_ETHER_TYPE_VLAN) {
    vlan_header = (struct rte_vlan_hdr*)((void*)&eth_hdr->ether_type + 2);
    ether_type = ntohs(vlan_header->eth_proto);
    vlan = true;
    hdr_offset += sizeof(struct rte_vlan_hdr);
    dbg("%s(%d), vlan mbuf %d\n", __func__, port, vlan);
  }

  dbg("%s(%d), ether_type 0x%x\n", __func__, port, ether_type);
  switch (ether_type) {
    case RTE_ETHER_TYPE_1588:
      ptp_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ptp_header*, hdr_offset);
      mt_ptp_parse(ptp, ptp_hdr, vlan, MT_PTP_L2, m->timesync, NULL);
      break;
    case RTE_ETHER_TYPE_ARP:
      arp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_arp_hdr*, hdr_offset);
      mt_arp_parse(impl, arp_hdr, port);
      break;
    case RTE_ETHER_TYPE_IPV4:
      ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ipv4_udp*, hdr_offset);
      if (ipv4_hdr->ip.next_proto_id == IPPROTO_UDP) {
        src_port = ntohs(ipv4_hdr->udp.src_port);
        hdr_offset += sizeof(struct mt_ipv4_udp);
        if (ptp && (src_port == MT_PTP_UDP_EVENT_PORT ||
                    src_port == MT_PTP_UDP_GEN_PORT)) { /* ptp pkt*/
          ptp_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ptp_header*, hdr_offset);
          mt_ptp_parse(ptp, ptp_hdr, vlan, MT_PTP_L4, m->timesync, ipv4_hdr);
        } else if (dhcp && src_port == MT_DHCP_UDP_SERVER_PORT) { /* dhcp pkt */
          dhcp_hdr = rte_pktmbuf_mtod_offset(m, struct mt_dhcp_hdr*, hdr_offset);
          mt_dhcp_parse(impl, dhcp_hdr, port);
        } else {
          cni_udp_handle(cni, m);
        }
      } else {
        /* ipv4 packets other than UDP fallback to kernel */
        cni_burst_to_kernel(cni, m);
      }
      break;
    default:
      // dbg("%s(%d), unknown ether_type %d\n", __func__, port, ether_type);
      /* unknown eth packets fallback to kernel */
      cni_burst_to_kernel(cni, m);
      break;
  }
  cni->eth_rx_bytes += m->pkt_len;

  return 0;
}

static int cni_traffic(struct mtl_main_impl* impl) {
  struct mt_cni_entry* cni;
  int num_ports = mt_num_ports(impl);
  struct rte_mbuf* pkts_rx[ST_CNI_RX_BURST_SIZE];
  uint16_t rx;
  bool done = true;

  for (int i = 0; i < num_ports; i++) {
    cni = cni_get_entry(impl, i);

    mt_tap_handle(impl, i);

    /* rx from cni rx queue */
    if (cni->rxq) {
      rx = mt_rxq_burst(cni->rxq, pkts_rx, ST_CNI_RX_BURST_SIZE);
      if (rx > 0) {
        cni->eth_rx_cnt += rx;
        for (uint16_t ri = 0; ri < rx; ri++) cni_rx_handle(cni, pkts_rx[ri]);
        mt_kni_handle(impl, i, pkts_rx, rx);
        mt_free_mbufs(&pkts_rx[0], rx);
        done = false;
      }
    }
  }

  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static void* cni_traffic_thread(void* arg) {
  struct mtl_main_impl* impl = arg;
  struct mt_cni_impl* cni = mt_get_cni(impl);

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&cni->stop_thread) == 0) {
    cni_traffic(impl);
    mt_sleep_ms(1);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static int cni_traffic_thread_start(struct mtl_main_impl* impl, struct mt_cni_impl* cni) {
  int ret;

  if (cni->tid) {
    err("%s, cni_traffic thread already start\n", __func__);
    return 0;
  }

  rte_atomic32_set(&cni->stop_thread, 0);
  ret = pthread_create(&cni->tid, NULL, cni_traffic_thread, impl);
  if (ret < 0) {
    err("%s, cni_traffic thread create fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

static int cni_traffic_thread_stop(struct mt_cni_impl* cni) {
  rte_atomic32_set(&cni->stop_thread, 1);
  if (cni->tid) {
    pthread_join(cni->tid, NULL);
    cni->tid = 0;
  }

  return 0;
}

static int cni_tasklet_start(void* priv) {
  struct mtl_main_impl* impl = priv;
  struct mt_cni_impl* cni = mt_get_cni(impl);

  /* tasklet will take over the cni thread */
  if (cni->lcore_tasklet) cni_traffic_thread_stop(cni);

  return 0;
}

static int cni_tasklet_stop(void* priv) {
  struct mtl_main_impl* impl = priv;
  struct mt_cni_impl* cni = mt_get_cni(impl);

  if (cni->lcore_tasklet) cni_traffic_thread_start(impl, cni);

  return 0;
}

static int cni_tasklet_handler(void* priv) {
  struct mtl_main_impl* impl = priv;

  return cni_traffic(impl);
}

static int cni_queues_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_cni_entry* cni;

  for (int i = 0; i < num_ports; i++) {
    cni = cni_get_entry(impl, i);

    if (cni->rxq) {
      mt_rxq_put(cni->rxq);
      cni->rxq = NULL;
    }
  }

  return 0;
}

static int cni_queues_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_cni_entry* cni;
  struct mt_interface* inf;

  if (mt_no_system_rxq(impl)) {
    warn("%s, disabled as no system rx queues\n", __func__);
    return 0;
  }

  for (int i = 0; i < num_ports; i++) {
    cni = cni_get_entry(impl, i);
    inf = mt_if(impl, i);

    /* continue if no cni */
    if (inf->drv_info.flags & MT_DRV_F_NO_CNI) continue;

    struct mt_rxq_flow flow;
    memset(&flow, 0, sizeof(flow));
    flow.sys_queue = true;
    cni->rxq = mt_rxq_get(impl, i, &flow);
    if (!cni->rxq) {
      err("%s(%d), rx queue get fail\n", __func__, i);
      cni_queues_uinit(impl);
      return -EIO;
    }
    info("%s(%d), rxq %d\n", __func__, i, mt_rxq_queue_id(cni->rxq));
  }

  return 0;
}

static bool cni_if_need(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_interface* inf;

  for (int i = 0; i < num_ports; i++) {
    inf = mt_if(impl, i);

    if (!(inf->drv_info.flags & MT_DRV_F_NO_CNI)) return true;
  }

  return false;
}

static int cni_stat(void* priv) {
  struct mt_cni_impl* cni_impl = priv;
  struct mt_cni_entry* cni;
  int num_ports = cni_impl->num_ports;
  double dump_period_s = mt_stat_dump_period_s(cni_impl->parent);

  if (!cni_impl->used) return 0;

  for (int i = 0; i < num_ports; i++) {
    cni = &cni_impl->entries[i];

    notice("CNI(%d): eth_rx_rate %f Mb/s, eth_rx_cnt %u\n", i,
           (double)cni->eth_rx_bytes * 8 / dump_period_s / MTL_STAT_M_UNIT,
           cni->eth_rx_cnt);
    cni->eth_rx_cnt = 0;
    cni->eth_rx_bytes = 0;

    if (cni->virtio_rx_cnt) {
      notice("CNI(%d): virtio_rx_cnt %u\n", i, cni->virtio_rx_cnt);
      cni->virtio_rx_cnt = 0;
    }

    csq_stat(cni);
  }

  return 0;
}

int mt_cni_init(struct mtl_main_impl* impl) {
  int ret;
  struct mt_cni_impl* cni_impl = mt_get_cni(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);

  cni_impl->parent = impl;

  cni_impl->used = cni_if_need(impl);
  if (!cni_impl->used) return 0;

  cni_impl->num_ports = mt_num_ports(impl);
  cni_impl->lcore_tasklet = (p->flags & MTL_FLAG_CNI_THREAD) ? false : true;
  rte_atomic32_set(&cni_impl->stop_thread, 0);

  for (int i = 0; i < cni_impl->num_ports; i++) {
    struct mt_cni_entry* cni = cni_get_entry(impl, i);
    cni->port = i;
    cni->impl = impl;
    MT_TAILQ_INIT(&cni->csq_queues);
    rte_spinlock_init(&cni->csq_lock);
    MT_TAILQ_INIT(&cni->udp_detect);
  }

  ret = mt_kni_init(impl);
  if (ret < 0) return ret;

  ret = cni_queues_init(impl);
  if (ret < 0) {
    mt_cni_uinit(impl);
    return ret;
  }

  ret = mt_tap_init(impl);
  if (ret < 0) return ret;

  if (cni_impl->lcore_tasklet) {
    struct mt_sch_tasklet_ops ops;

    memset(&ops, 0x0, sizeof(ops));
    ops.priv = impl;
    ops.name = "cni";
    ops.start = cni_tasklet_start;
    ops.stop = cni_tasklet_stop;
    ops.handler = cni_tasklet_handler;

    cni_impl->tasklet = mt_sch_register_tasklet(impl->main_sch, &ops);
    if (!cni_impl->tasklet) {
      err("%s, mt_sch_register_tasklet fail\n", __func__);
      mt_cni_uinit(impl);
      return -EIO;
    }
  }

  ret = mt_cni_start(impl);
  if (ret < 0) {
    err("%s, mt_cni_start fail %d\n", __func__, ret);
    mt_cni_uinit(impl);
    return ret;
  }

  mt_stat_register(impl, cni_stat, cni_impl, "cni");
  return 0;
}

int mt_cni_uinit(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni_impl = mt_get_cni(impl);
  struct mt_cni_udp_detect_entry* udp_detect;
  struct mt_csq_entry* csq;

  if (!cni_impl->used) return 0;

  for (int i = 0; i < cni_impl->num_ports; i++) {
    struct mt_cni_entry* cni = cni_get_entry(impl, i);

    /* free all udp queue entry */
    while ((csq = MT_TAILQ_FIRST(&cni->csq_queues))) {
      MT_TAILQ_REMOVE(&cni->csq_queues, csq, next);
      warn("%s(%d,%d), entry %p not free\n", __func__, i, csq->idx, csq);
      mt_rte_free(csq);
    }
    /* free all udp detected entry */
    while ((udp_detect = MT_TAILQ_FIRST(&cni->udp_detect))) {
      MT_TAILQ_REMOVE(&cni->udp_detect, udp_detect, next);
      cni_udp_detect_dump(cni, udp_detect);
      mt_rte_free(udp_detect);
    }
  }

  if (cni_impl->tasklet) {
    mt_sch_unregister_tasklet(cni_impl->tasklet);
    cni_impl->tasklet = NULL;
  }

  mt_stat_unregister(impl, cni_stat, cni_impl);

  mt_cni_stop(impl);

  cni_queues_uinit(impl);

  mt_kni_uinit(impl);

  mt_tap_uinit(impl);

  info("%s, succ\n", __func__);
  return 0;
}

int mt_cni_start(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);
  int ret;

  if (!cni->used) return 0;

  ret = cni_traffic_thread_start(impl, cni);
  if (ret < 0) return ret;

  return 0;
}

int mt_cni_stop(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);

  if (!cni->used) return 0;

  cni_traffic_thread_stop(cni);

  return 0;
}

struct mt_csq_entry* mt_csq_get(struct mtl_main_impl* impl, enum mtl_port port,
                                struct mt_rxq_flow* flow) {
  struct mt_cni_entry* cni = cni_get_entry(impl, port);
  int idx = cni->csq_idx;

  if (flow->sys_queue) {
    err("%s(%d,%d), not support sys queue\n", __func__, port, idx);
    return NULL;
  }

  struct mt_csq_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d,%d), entry malloc fail\n", __func__, port, idx);
    return NULL;
  }
  entry->idx = idx;
  entry->parent = cni;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  /* ring create */
  char ring_name[32];
  snprintf(ring_name, 32, "%sP%d_%d", MT_CSQ_RING_PREFIX, port, idx);
  entry->ring = rte_ring_create(ring_name, 512, mt_socket_id(impl, MTL_PORT_P),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!entry->ring) {
    err("%s(%d,%d), ring %s create fail\n", __func__, port, idx, ring_name);
    mt_rte_free(entry);
    return NULL;
  }

  csq_lock(cni);
  MT_TAILQ_INSERT_HEAD(&cni->csq_queues, entry, next);
  cni->csq_idx++;
  csq_unlock(cni);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u port %u on %d\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, idx);
  return entry;
}

int mt_csq_put(struct mt_csq_entry* entry) {
  struct mt_cni_entry* cni = entry->parent;

  csq_lock(cni);
  MT_TAILQ_REMOVE(&cni->csq_queues, entry, next);
  csq_unlock(cni);

  csq_entry_free(entry);
  return 0;
}

uint16_t mt_csq_burst(struct mt_csq_entry* entry, struct rte_mbuf** rx_pkts,
                      uint16_t nb_pkts) {
  uint16_t n = rte_ring_sc_dequeue_burst(entry->ring, (void**)rx_pkts, nb_pkts, NULL);
  entry->stat_dequeue_cnt += n;
  return n;
}
