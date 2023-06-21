/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_cni.h"

#include "mt_arp.h"
#include "mt_dhcp.h"
#include "mt_kni.h"
#include "mt_queue.h"
// #define DEBUG
#include "mt_dhcp.h"
#include "mt_log.h"
#include "mt_ptp.h"
#include "mt_sch.h"
#include "mt_stat.h"
#include "mt_tap.h"
#include "mt_util.h"

static int cni_rx_handle(struct mtl_main_impl* impl, struct rte_mbuf* m,
                         enum mtl_port port) {
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  struct mt_dhcp_impl* dhcp = mt_get_dhcp(impl, port);
  struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  uint16_t ether_type, src_port;
  struct rte_vlan_hdr* vlan_header;
  bool vlan = false;
  struct mt_ptp_header* ptp_hdr;
  struct rte_arp_hdr* arp_hdr;
  struct mt_dhcp_hdr* dhcp_hdr;
  struct mt_ptp_ipv4_udp* ipv4_hdr;
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
      ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ptp_ipv4_udp*, hdr_offset);
      src_port = ntohs(ipv4_hdr->udp.src_port);
      hdr_offset += sizeof(struct mt_ptp_ipv4_udp);
      if (ptp && (src_port == MT_PTP_UDP_EVENT_PORT ||
                  src_port == MT_PTP_UDP_GEN_PORT)) { /* ptp pkt*/
        ptp_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ptp_header*, hdr_offset);
        mt_ptp_parse(ptp, ptp_hdr, vlan, MT_PTP_L4, m->timesync, ipv4_hdr);
      } else if (dhcp && src_port == MT_DHCP_UDP_SERVER_PORT) { /* dhcp pkt */
        dhcp_hdr = rte_pktmbuf_mtod_offset(m, struct mt_dhcp_hdr*, hdr_offset);
        mt_dhcp_parse(impl, dhcp_hdr, port);
      }
      break;
    default:
      // dbg("%s(%d), unknown ether_type %d\n", __func__, port, ether_type);
      break;
  }
  cni->eth_rx_bytes[port] += m->pkt_len;

  return 0;
}

static int cni_traffic(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);
  int num_ports = mt_num_ports(impl);
  struct rte_mbuf* pkts_rx[ST_CNI_RX_BURST_SIZE];
  uint16_t rx;
  struct mt_ptp_impl* ptp;
  bool done = true;

  for (int i = 0; i < num_ports; i++) {
    ptp = mt_get_ptp(impl, i);

    /* rx from ptp rx queue */
    if (ptp && ptp->rxq) {
      rx = mt_rxq_burst(ptp->rxq, pkts_rx, ST_CNI_RX_BURST_SIZE);
      if (rx > 0) {
        cni->eth_rx_cnt[i] += rx;
        for (uint16_t ri = 0; ri < rx; ri++) cni_rx_handle(impl, pkts_rx[ri], i);
        mt_free_mbufs(&pkts_rx[0], rx);
        done = false;
      }
    }
    mt_tap_handle(impl, i);
    /* rx from cni rx queue */
    if (cni->rxq[i]) {
      rx = mt_rxq_burst(cni->rxq[i], pkts_rx, ST_CNI_RX_BURST_SIZE);
      if (rx > 0) {
        cni->eth_rx_cnt[i] += rx;
        for (uint16_t ri = 0; ri < rx; ri++) cni_rx_handle(impl, pkts_rx[ri], i);
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

static int cni_rsq_mbuf_cb(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct mt_cni_priv* cni_priv = priv;
  struct mtl_main_impl* impl = cni_priv->impl;
  enum mtl_port port = cni_priv->port;
  struct mt_cni_impl* cni = mt_get_cni(impl);

  cni->eth_rx_cnt[port] += nb;
  for (uint16_t ri = 0; ri < nb; ri++) cni_rx_handle(impl, mbuf[ri], port);
  mt_kni_handle(impl, port, mbuf, nb);

  return 0;
}

static int cni_queues_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_cni_impl* cni = mt_get_cni(impl);

  for (int i = 0; i < num_ports; i++) {
    if (cni->rxq[i]) {
      mt_rxq_put(cni->rxq[i]);
      cni->rxq[i] = NULL;
    }
  }

  return 0;
}

static int cni_queues_init(struct mtl_main_impl* impl, struct mt_cni_impl* cni) {
  int num_ports = mt_num_ports(impl);

  if (mt_no_system_rxq(impl)) {
    warn("%s, disabled as no system rx queues\n", __func__);
    return 0;
  }

  for (int i = 0; i < num_ports; i++) {
    /* no cni for kernel based pmd */
    if (mt_pmd_is_kernel(impl, i)) continue;

    cni->cni_priv[i].impl = impl;
    cni->cni_priv[i].port = i;

    struct mt_rxq_flow flow;
    memset(&flow, 0, sizeof(flow));
    flow.sys_queue = true;
    flow.priv = &cni->cni_priv[i];
    flow.cb = cni_rsq_mbuf_cb;

    cni->rxq[i] = mt_rxq_get(impl, i, &flow);
    info("%s(%d), rxq %d\n", __func__, i, mt_rxq_queue_id(cni->rxq[i]));
    if (!cni->rxq[i]) {
      err("%s(%d), rx queue get fail\n", __func__, i);
      cni_queues_uinit(impl);
      return -EIO;
    }
  }

  return 0;
}

static bool cni_if_need(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    if (!mt_pmd_is_kernel(impl, i)) return true;
  }

  return false;
}

static int cni_stat(void* priv) {
  struct mt_cni_impl* cni = priv;
  int num_ports = cni->num_ports;

  if (!cni->used) return 0;

  for (int i = 0; i < num_ports; i++) {
    notice("CNI(%d): eth_rx_rate %" PRIu64 " Mb/s, eth_rx_cnt %u\n", i,
           cni->eth_rx_bytes[i] * 8 / MT_DEV_STAT_INTERVAL_S / MT_DEV_STAT_M_UNIT,
           cni->eth_rx_cnt[i]);
    cni->eth_rx_cnt[i] = 0;
    cni->eth_rx_bytes[i] = 0;
  }

  return 0;
}

int mt_cni_init(struct mtl_main_impl* impl) {
  int ret;
  struct mt_cni_impl* cni = mt_get_cni(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);

  cni->num_ports = mt_num_ports(impl);
  cni->used = cni_if_need(impl);
  if (!cni->used) return 0;

  cni->lcore_tasklet = (p->flags & MTL_FLAG_CNI_THREAD) ? false : true;
  rte_atomic32_set(&cni->stop_thread, 0);

  ret = mt_kni_init(impl);
  if (ret < 0) return ret;

  ret = cni_queues_init(impl, cni);
  if (ret < 0) {
    mt_cni_uinit(impl);
    return ret;
  }

  ret = mt_tap_init(impl);
  if (ret < 0) return ret;

  if (mt_has_srss(impl, MTL_PORT_P)) {
    goto exit_with_stat_register;
  }

  if (cni->lcore_tasklet) {
    struct mt_sch_tasklet_ops ops;

    memset(&ops, 0x0, sizeof(ops));
    ops.priv = impl;
    ops.name = "cni";
    ops.start = cni_tasklet_start;
    ops.stop = cni_tasklet_stop;
    ops.handler = cni_tasklet_handler;

    cni->tasklet = mt_sch_register_tasklet(impl->main_sch, &ops);
    if (!cni->tasklet) {
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

exit_with_stat_register:
  mt_stat_register(impl, cni_stat, cni, "cni");

  return 0;
}

int mt_cni_uinit(struct mtl_main_impl* impl) {
  struct mt_cni_impl* cni = mt_get_cni(impl);

  if (cni->tasklet) {
    mt_sch_unregister_tasklet(cni->tasklet);
    cni->tasklet = NULL;
  }

  mt_stat_unregister(impl, cni_stat, cni);

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
