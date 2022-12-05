/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_arp.h"

#include "mt_dev.h"
//#define DEBUG
#include "mt_log.h"

static inline struct mt_arp_impl* get_arp(struct mtl_main_impl* impl,
                                          enum mtl_port port) {
  return &impl->arp[port];
}

static void arp_reset(struct mt_arp_impl* arp) {
  for (int i = 0; i > MT_ARP_ENTRY_MAX; i++) {
    rte_atomic32_set(&arp->mac_ready[i], 0);
    arp->ip[i] = 0;
    memset(&arp->ea[i], 0, sizeof(arp->ea[i]));
  }
}

static bool arp_is_valid_hdr(struct rte_arp_hdr* hdr) {
  if ((ntohs(hdr->arp_hardware) != RTE_ARP_HRD_ETHER) &&
      (ntohs(hdr->arp_protocol) != RTE_ETHER_TYPE_IPV4) &&
      (hdr->arp_hlen != RTE_ETHER_ADDR_LEN) && (hdr->arp_plen != 4)) {
    dbg("%s, not valid arp\n", __func__);
    return false;
  }

  return true;
}

static int arp_receive_request(struct mtl_main_impl* impl, struct rte_arp_hdr* request,
                               enum mtl_port port) {
  if (!arp_is_valid_hdr(request)) return -EINVAL;

  if (request->arp_data.arp_tip != *(uint32_t*)mt_sip_addr(impl, port)) {
    dbg("%s(%d), not our arp\n", __func__, port);
    return -EINVAL;
  }

  struct rte_mbuf* rpl_pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!rpl_pkt) {
    err("%s(%d), rpl_pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  rpl_pkt->pkt_len = rpl_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(rpl_pkt, struct rte_ether_hdr*);
  uint16_t port_id = mt_port_id(impl, port);

  rte_eth_macaddr_get(port_id, mt_eth_s_addr(eth));
  rte_ether_addr_copy(&request->arp_data.arp_sha, mt_eth_d_addr(eth));
  eth->ether_type = htons(RTE_ETHER_TYPE_ARP);  // ARP_PROTOCOL

  struct rte_arp_hdr* arp =
      rte_pktmbuf_mtod_offset(rpl_pkt, struct rte_arp_hdr*, sizeof(struct rte_ether_hdr));
  arp->arp_hardware = htons(RTE_ARP_HRD_ETHER);
  arp->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);  // IP protocol
  arp->arp_hlen = RTE_ETHER_ADDR_LEN;              // size of MAC
  arp->arp_plen = 4;                               // size of fo IP
  arp->arp_opcode = htons(RTE_ARP_OP_REPLY);
  rte_ether_addr_copy(&request->arp_data.arp_sha, &arp->arp_data.arp_tha);
  arp->arp_data.arp_tip = request->arp_data.arp_sip;
  rte_eth_macaddr_get(port_id, &arp->arp_data.arp_sha);
  arp->arp_data.arp_sip = *(uint32_t*)mt_sip_addr(impl, port);

  /* send arp reply packet */
  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &rpl_pkt, 1);
  if (send < 1) {
    err_once("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(rpl_pkt);
  }

  uint8_t* ip = (uint8_t*)&request->arp_data.arp_sip;
  info_once("%s(%d), send to %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

static int arp_receive_reply(struct mtl_main_impl* impl, struct rte_arp_hdr* reply,
                             enum mtl_port port) {
  if (!arp_is_valid_hdr(reply)) return -EINVAL;

  if (reply->arp_data.arp_tip != *(uint32_t*)mt_sip_addr(impl, port)) {
    dbg("%s(%d), not our arp\n", __func__, port);
    return -EINVAL;
  }

  uint8_t* ip = (uint8_t*)&reply->arp_data.arp_sip;
  info_once("%s(%d), from %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);

  struct mt_arp_impl* arp_impl = get_arp(impl, port);

  /* save to arp table */
  int i;
  for (i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    if (arp_impl->ip[i] == reply->arp_data.arp_sip) break;
  }
  if (i >= MT_ARP_ENTRY_MAX) {
    dbg("%s(%d), not our arp\n", __func__, port);
    return -EINVAL;
  }
  memcpy(arp_impl->ea[i].addr_bytes, reply->arp_data.arp_sha.addr_bytes,
         RTE_ETHER_ADDR_LEN);
  rte_atomic32_set(&arp_impl->mac_ready[i], 1);

  return 0;
}

int mt_arp_parse(struct mtl_main_impl* impl, struct rte_arp_hdr* hdr,
                 enum mtl_port port) {
  switch (htons(hdr->arp_opcode)) {
    case RTE_ARP_OP_REQUEST:
      arp_receive_request(impl, hdr, port);
      break;
    case RTE_ARP_OP_REPLY:
      arp_receive_reply(impl, hdr, port);
      break;
    default:
      err("%s, mt_arp_parse %04x uninplemented\n", __func__, ntohs(hdr->arp_opcode));
      return -EINVAL;
  }

  return 0;
}

int mt_arp_cni_get_mac(struct mtl_main_impl* impl, struct rte_ether_addr* ea,
                       enum mtl_port port, uint32_t ip, int timeout_ms) {
  struct mt_arp_impl* arp_impl = get_arp(impl, port);
  uint16_t port_id = mt_port_id(impl, port);
  uint16_t tx;
  int retry = 0;
  uint8_t* addr = (uint8_t*)&ip;
  int max_retry = 0;
  int sleep_interval_ms = 500;

  if (timeout_ms) max_retry = (timeout_ms / sleep_interval_ms) + 1;

  int i;
  for (i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    if (arp_impl->ip[i] == ip && rte_atomic32_read(&arp_impl->mac_ready[i]) == 1) {
      /* get cached mac addr */
      memcpy(ea->addr_bytes, arp_impl->ea[i].addr_bytes, RTE_ETHER_ADDR_LEN);
      return 0;
    }
    if (arp_impl->ip[i] == 0) break;
  }
  if (i >= MT_ARP_ENTRY_MAX) {
    /* arp table full, flush it */
    arp_reset(arp_impl);
    i = 0;
  }
  arp_impl->ip[i] = ip;
  rte_atomic32_set(&arp_impl->mac_ready[i], 0);

  struct rte_mbuf* req_pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!req_pkt) return -ENOMEM;

  req_pkt->pkt_len = req_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(req_pkt, struct rte_ether_hdr*);
  rte_eth_macaddr_get(port_id, mt_eth_s_addr(eth));
  memset(mt_eth_d_addr(eth), 0xFF, RTE_ETHER_ADDR_LEN);
  eth->ether_type = htons(RTE_ETHER_TYPE_ARP);  // ARP_PROTOCOL
  struct rte_arp_hdr* arp =
      rte_pktmbuf_mtod_offset(req_pkt, struct rte_arp_hdr*, sizeof(struct rte_ether_hdr));
  arp->arp_hardware = htons(RTE_ARP_HRD_ETHER);
  arp->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);  // IP protocol
  arp->arp_hlen = RTE_ETHER_ADDR_LEN;              // size of MAC
  arp->arp_plen = 4;                               // size of fo IP
  arp->arp_opcode = htons(RTE_ARP_OP_REQUEST);
  arp->arp_data.arp_tip = ip;
  arp->arp_data.arp_sip = *(uint32_t*)mt_sip_addr(impl, port);
  rte_eth_macaddr_get(port_id, &arp->arp_data.arp_sha);
  memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);

  /* send arp request packet */
  while (rte_atomic32_read(&arp_impl->mac_ready[i]) == 0) {
    if (mt_aborted(impl)) {
      err("%s(%d), fail as user aborted\n", __func__, port);
      return -EIO;
    }
    if ((max_retry > 0) && (retry > max_retry)) {
      err("%s(%d), fail as timeout to %d ms\n", __func__, port, timeout_ms);
      return -EIO;
    }

    rte_mbuf_refcnt_update(req_pkt, 1);
    tx = mt_dev_tx_sys_queue_burst(impl, port, &req_pkt, 1);
    if (tx < 1) {
      err("%s(%d), tx fail\n", __func__, port);
      rte_mbuf_refcnt_update(req_pkt, -1);
    }

    mt_sleep_ms(sleep_interval_ms);
    retry++;
    if (0 == (retry % 10))
      info("%s(%d), waiting arp from %d.%d.%d.%d\n", __func__, port, addr[0], addr[1],
           addr[2], addr[3]);
  }
  memcpy(ea->addr_bytes, arp_impl->ea[i].addr_bytes, RTE_ETHER_ADDR_LEN);
  rte_pktmbuf_free(req_pkt);

  return 0;
}

int mt_arp_init(struct mtl_main_impl* impl) { return 0; }

int mt_arp_uinit(struct mtl_main_impl* impl) { return 0; }
