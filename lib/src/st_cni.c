/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */
#include "st_cni.h"

#include "st_arp.h"
#include "st_dev.h"
#include "st_kni.h"
// #define DEBUG
#include "st_log.h"
#include "st_ptp.h"
#include "st_sch.h"
#include "st_util.h"

static int cni_rx_handle(struct st_main_impl* impl, struct rte_mbuf* m,
                         enum st_port port) {
  struct st_ptp_impl* ptp = st_get_ptp(impl, port);
  struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  uint16_t ether_type, src_port;
  struct rte_vlan_hdr* vlan_header;
  bool vlan = false;
  struct st_ptp_header* ptp_hdr;
  struct rte_arp_hdr* arp_hdr;
  struct st_ptp_ipv4_udp* ipv4_hdr;
  size_t hdr_offset = sizeof(struct rte_ether_hdr);

  // st_mbuf_dump(port, 0, "kni_rx", m);

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
      ptp_hdr = rte_pktmbuf_mtod_offset(m, struct st_ptp_header*, hdr_offset);
      st_ptp_parse(ptp, ptp_hdr, vlan, ST_PTP_L2, m->timesync, NULL);
      break;
    case RTE_ETHER_TYPE_ARP:
      arp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_arp_hdr*, hdr_offset);
      st_arp_parse(impl, arp_hdr, port);
      break;
    case RTE_ETHER_TYPE_IPV4:
      ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct st_ptp_ipv4_udp*, hdr_offset);
      src_port = ntohs(ipv4_hdr->udp.src_port);
      hdr_offset += sizeof(struct st_ptp_ipv4_udp);
      if ((src_port == ST_PTP_UDP_EVENT_PORT) || (src_port == ST_PTP_UDP_GEN_PORT)) {
        ptp_hdr = rte_pktmbuf_mtod_offset(m, struct st_ptp_header*, hdr_offset);
        st_ptp_parse(ptp, ptp_hdr, vlan, ST_PTP_L4, m->timesync, ipv4_hdr);
      }
      break;
    default:
      // dbg("%s(%d), unknown ether_type %d\n", __func__, port, ether_type);
      break;
  }

  return 0;
}

static int cni_traffic(struct st_main_impl* impl) {
  struct st_cni_impl* cni = st_get_cni(impl);
  int num_ports = st_num_ports(impl);
  uint16_t port_id;
  struct rte_mbuf* pkts_rx[ST_CNI_RX_BURST_SIZE];
  uint16_t rx;
  struct st_ptp_impl* ptp;

  for (int i = 0; i < num_ports; i++) {
    ptp = st_get_ptp(impl, i);
    port_id = st_port_id(impl, i);

    /* rx from ptp rx queue */
    if (ptp->rx_queue_active) {
      rx = rte_eth_rx_burst(port_id, ptp->rx_queue_id, pkts_rx, ST_CNI_RX_BURST_SIZE);
      if (rx > 0) {
        cni->eth_rx_cnt[i] += rx;
        for (uint16_t ri = 0; ri < rx; ri++) cni_rx_handle(impl, pkts_rx[ri], i);
        st_free_mbufs(&pkts_rx[0], rx);
      }
    }

    /* rx from cni rx queue */
    rx = rte_eth_rx_burst(port_id, cni->rx_q_id[i], pkts_rx, ST_CNI_RX_BURST_SIZE);
    if (rx > 0) {
      cni->eth_rx_cnt[i] += rx;
      for (uint16_t ri = 0; ri < rx; ri++) cni_rx_handle(impl, pkts_rx[ri], i);
      st_kni_handle(impl, i, pkts_rx, rx);
      st_free_mbufs(&pkts_rx[0], rx);
    }
  }

  return 0;
}

static void* cni_trafic_thread(void* arg) {
  struct st_main_impl* impl = arg;
  struct st_cni_impl* cni = st_get_cni(impl);

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&cni->stop_thread) == 0) {
    cni_traffic(impl);
    st_sleep_ms(1);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static int cni_trafic_thread_start(struct st_main_impl* impl, struct st_cni_impl* cni) {
  int ret;

  if (cni->tid) {
    err("%s, cni_trafic thread already start\n", __func__);
    return 0;
  }

  rte_atomic32_set(&cni->stop_thread, 0);
  ret = rte_ctrl_thread_create(&cni->tid, "cni_trafic", NULL, cni_trafic_thread, impl);
  if (ret < 0) {
    err("%s, cni_trafic thread create fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

static int cni_trafic_thread_stop(struct st_cni_impl* cni) {
  rte_atomic32_set(&cni->stop_thread, 1);
  if (cni->tid) {
    pthread_join(cni->tid, NULL);
    cni->tid = 0;
  }

  return 0;
}

static int cni_tasklet_start(void* priv) {
  struct st_main_impl* impl = priv;
  struct st_cni_impl* cni = st_get_cni(impl);

  /* tasklet will take over the cni thread */
  if (cni->lcore_tasklet) cni_trafic_thread_stop(cni);

  return 0;
}

static int cni_tasklet_stop(void* priv) {
  struct st_main_impl* impl = priv;
  struct st_cni_impl* cni = st_get_cni(impl);

  if (cni->lcore_tasklet) cni_trafic_thread_start(impl, cni);

  return 0;
}

static int cni_tasklet_handlder(void* priv) {
  struct st_main_impl* impl = priv;

  cni_traffic(impl);
  return 0;
}

static int cni_queues_uinit(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_cni_impl* cni = st_get_cni(impl);

  for (int i = 0; i < num_ports; i++) {
    if (cni->rx_q_active[i]) {
      st_dev_free_rx_queue(impl, i, cni->rx_q_id[i]);
      cni->rx_q_active[i] = false;
    }
  }

  return 0;
}

static int cni_queues_init(struct st_main_impl* impl, struct st_cni_impl* cni) {
  int num_ports = st_num_ports(impl);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ret = st_dev_request_rx_queue(impl, i, &cni->rx_q_id[i], NULL);
    if (ret < 0) {
      err("%s(%d), kni_rx_q create fail\n", __func__, i);
      cni_queues_uinit(impl);
      return ret;
    }
    cni->rx_q_active[i] = true;
    info("%s(%d), rx q %d\n", __func__, i, cni->rx_q_id[i]);
  }

  return 0;
}

void st_cni_stat(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_cni_impl* cni = st_get_cni(impl);

  for (int i = 0; i < num_ports; i++) {
    info("CNI(%d): eth_rx_cnt %d \n", i, cni->eth_rx_cnt[i]);
    cni->eth_rx_cnt[i] = 0;
  }
}

int st_cni_init(struct st_main_impl* impl) {
  int ret;
  struct st_cni_impl* cni = st_get_cni(impl);
  struct st_init_params* p = st_get_user_params(impl);

  cni->lcore_tasklet = (p->flags & ST_FLAG_CNI_THREAD) ? false : true;
  rte_atomic32_set(&cni->stop_thread, 0);

  ret = st_kni_init(impl);
  if (ret < 0) return ret;

  ret = cni_queues_init(impl, cni);
  if (ret < 0) {
    st_cni_uinit(impl);
    return ret;
  }

  if (cni->lcore_tasklet) {
    struct st_sch_tasklet_ops ops;

    memset(&ops, 0x0, sizeof(ops));
    ops.priv = impl;
    ops.name = "cni";
    ops.start = cni_tasklet_start;
    ops.stop = cni_tasklet_stop;
    ops.handler = cni_tasklet_handlder;

    ret = st_sch_register_tasklet(impl->main_sch, &ops);
    if (ret < 0) {
      info("%s, st_sch_register_tasklet fail %d\n", __func__, ret);
      st_cni_uinit(impl);
      return ret;
    }
  }

  ret = st_cni_start(impl);
  if (ret < 0) {
    info("%s, st_cni_start fail %d\n", __func__, ret);
    st_cni_uinit(impl);
    return ret;
  }

  return 0;
}

int st_cni_uinit(struct st_main_impl* impl) {
  st_cni_stop(impl);

  cni_queues_uinit(impl);

  st_kni_uinit(impl);

  info("%s, succ\n", __func__);
  return 0;
}

int st_cni_start(struct st_main_impl* impl) {
  struct st_cni_impl* cni = st_get_cni(impl);
  int ret;

  ret = cni_trafic_thread_start(impl, cni);
  if (ret < 0) return ret;

  return 0;
}

int st_cni_stop(struct st_main_impl* impl) {
  struct st_cni_impl* cni = st_get_cni(impl);

  cni_trafic_thread_stop(cni);

  return 0;
}
