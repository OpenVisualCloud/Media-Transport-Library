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

#include "st_arp.h"

#include "st_dev.h"
//#define DEBUG
#include "st_log.h"

static bool arp_is_valid_hdr(struct rte_arp_hdr* hdr) {
  if ((ntohs(hdr->arp_hardware) != RTE_ARP_HRD_ETHER) &&
      (ntohs(hdr->arp_protocol) != RTE_ETHER_TYPE_IPV4) &&
      (hdr->arp_hlen != RTE_ETHER_ADDR_LEN) && (hdr->arp_plen != 4)) {
    dbg("%s, not valid arp\n", __func__);
    return false;
  }

  return true;
}

static int arp_receive_request(struct st_main_impl* impl, struct rte_arp_hdr* request,
                               enum st_port port) {
  if (!arp_is_valid_hdr(request)) return -EINVAL;

  if (request->arp_data.arp_tip != *(uint32_t*)st_sip_addr(impl, port)) {
    dbg("%s(%d), not our arp\n", __func__, port);
    return -EINVAL;
  }

  struct rte_mbuf* rpl_pkt = rte_pktmbuf_alloc(st_get_mempool(impl, port));
  if (!rpl_pkt) {
    err("%s(%d), rpl_pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  rpl_pkt->pkt_len = rpl_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(rpl_pkt, struct rte_ether_hdr*);
  uint16_t port_id = st_port_id(impl, port);

  rte_eth_macaddr_get(port_id, st_eth_s_addr(eth));
  rte_ether_addr_copy(&request->arp_data.arp_sha, st_eth_d_addr(eth));
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
  arp->arp_data.arp_sip = *(uint32_t*)st_sip_addr(impl, port);

  /* send arp reply packet */
  uint16_t send = rte_eth_tx_burst(port_id, impl->arp.tx_q_id[port], &rpl_pkt, 1);
  if (send < 1) {
    err("%s(%d), rte_eth_tx_burst fail\n", __func__, port);
    rte_pktmbuf_free(rpl_pkt);
  }

  uint8_t* ip = (uint8_t*)&request->arp_data.arp_sip;
  info_once("%s(%d), send to %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

static int arp_receive_reply(struct st_main_impl* impl, struct rte_arp_hdr* reply,
                             enum st_port port) {
  if (!arp_is_valid_hdr(reply)) return -EINVAL;

  if (reply->arp_data.arp_tip != *(uint32_t*)st_sip_addr(impl, port)) {
    dbg("%s(%d), not our arp\n", __func__, port);
    return -EINVAL;
  }

  uint8_t* ip = (uint8_t*)&reply->arp_data.arp_sip;
  info_once("%s(%d), from %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);

  struct st_arp_impl* arp_impl = &impl->arp;

  /* save to arp impl */
  if (reply->arp_data.arp_sip == arp_impl->ip[port]) {
    memcpy(arp_impl->ea[port].addr_bytes, reply->arp_data.arp_sha.addr_bytes,
           RTE_ETHER_ADDR_LEN);
    rte_atomic32_set(&arp_impl->mac_ready[port], 1);
  }

  return 0;
}

static int arp_queues_uinit(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_arp_impl* arp = &impl->arp;

  for (int i = 0; i < num_ports; i++) {
    if (arp->tx_q_active[i]) {
      st_dev_free_tx_queue(impl, i, arp->tx_q_id[i]);
      arp->tx_q_active[i] = false;
    }
  }

  return 0;
}

static int arp_queues_init(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_arp_impl* arp = &impl->arp;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ret = st_dev_request_tx_queue(impl, i, &arp->tx_q_id[i], 0);
    if (ret < 0) {
      err("%s(%d), tx_q create fail\n", __func__, i);
      arp_queues_uinit(impl);
      return ret;
    }
    arp->tx_q_active[i] = true;
    info("%s(%d), tx q %d\n", __func__, i, arp->tx_q_id[i]);
  }

  return 0;
}

int st_arp_parse(struct st_main_impl* impl, struct rte_arp_hdr* hdr, enum st_port port) {
  switch (htons(hdr->arp_opcode)) {
    case RTE_ARP_OP_REQUEST:
      arp_receive_request(impl, hdr, port);
      break;
    case RTE_ARP_OP_REPLY:
      arp_receive_reply(impl, hdr, port);
      break;
    default:
      err("%s, st_arp_parse %04x uninplemented\n", __func__, ntohs(hdr->arp_opcode));
      return -EINVAL;
  }

  return 0;
}

int st_arp_cni_get_mac(struct st_main_impl* impl, struct rte_ether_addr* ea,
                       enum st_port port, uint32_t ip) {
  struct st_arp_impl* arp_impl = &impl->arp;
  uint16_t port_id = st_port_id(impl, port);
  uint16_t tx;
  int retry = 0;
  uint8_t* addr = (uint8_t*)&ip;

  arp_impl->ip[port] = ip;

  struct rte_mbuf* req_pkt = rte_pktmbuf_alloc(st_get_mempool(impl, port));
  if (!req_pkt) return -ENOMEM;

  req_pkt->pkt_len = req_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(req_pkt, struct rte_ether_hdr*);
  rte_eth_macaddr_get(port_id, st_eth_s_addr(eth));
  memset(st_eth_d_addr(eth), 0xFF, RTE_ETHER_ADDR_LEN);
  eth->ether_type = htons(RTE_ETHER_TYPE_ARP);  // ARP_PROTOCOL
  struct rte_arp_hdr* arp =
      rte_pktmbuf_mtod_offset(req_pkt, struct rte_arp_hdr*, sizeof(struct rte_ether_hdr));
  arp->arp_hardware = htons(RTE_ARP_HRD_ETHER);
  arp->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);  // IP protocol
  arp->arp_hlen = RTE_ETHER_ADDR_LEN;              // size of MAC
  arp->arp_plen = 4;                               // size of fo IP
  arp->arp_opcode = htons(RTE_ARP_OP_REQUEST);
  arp->arp_data.arp_tip = ip;
  arp->arp_data.arp_sip = *(uint32_t*)st_sip_addr(impl, port);
  rte_eth_macaddr_get(port_id, &arp->arp_data.arp_sha);
  memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);

  /* send arp request packet */
  while (rte_atomic32_read(&arp_impl->mac_ready[port]) == 0) {
    rte_mbuf_refcnt_update(req_pkt, 1);
    tx = rte_eth_tx_burst(port_id, arp_impl->tx_q_id[port], &req_pkt, 1);
    if (tx < 1) {
      err("%s, rte_eth_tx_burst fail\n", __func__);
      rte_mbuf_refcnt_update(req_pkt, -1);
    }

    if (rte_atomic32_read(&impl->request_exit)) return -EIO;

    st_sleep_ms(100);
    retry++;
    if (0 == (retry % 50))
      info("%s(%d), waiting arp from %d.%d.%d.%d\n", __func__, port, addr[0], addr[1],
           addr[2], addr[3]);
  }
  memcpy(ea->addr_bytes, arp_impl->ea[port].addr_bytes, RTE_ETHER_ADDR_LEN);
  rte_pktmbuf_free(req_pkt);

  return 0;
}

#ifdef ST_HAS_KNI
static int arp_get(int sfd, in_addr_t ip, struct rte_ether_addr* ea, const char* ifname) {
  struct arpreq arpreq;
  struct sockaddr_in* sin;
  struct in_addr ina;
  unsigned char* hw_addr;

  memset(&arpreq, 0, sizeof(struct arpreq));

  sin = (struct sockaddr_in*)&arpreq.arp_pa;
  memset(sin, 0, sizeof(struct sockaddr_in));
  sin->sin_family = AF_INET;
  ina.s_addr = ip;
  memcpy(&sin->sin_addr, (char*)&ina, sizeof(struct in_addr));

  strcpy(arpreq.arp_dev, ifname);
  int ret = ioctl(sfd, SIOCGARP, &arpreq);
  if (ret < 0) {
    err("%s, entry not available in cache...\n", __func__);
    return -EIO;
  }

  info("%s, entry has been successfully retreived\n", __func__);
  hw_addr = (unsigned char*)arpreq.arp_ha.sa_data;
  memcpy(ea->addr_bytes, hw_addr, RTE_ETHER_ADDR_LEN);
  dbg("%s, mac addr found : %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, hw_addr[0],
      hw_addr[1], hw_addr[2], hw_addr[3], hw_addr[4], hw_addr[5]);

  return 0;
}

int st_arp_socket_get_mac(struct st_main_impl* impl, struct rte_ether_addr* ea,
                          uint8_t dip[ST_IP_ADDR_LEN], const char* ifname) {
  int sockfd = 0;
  struct sockaddr_in addr;
  char dummy_buf[4];

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = *(uint32_t*)dip;
  addr.sin_port = htons(12345);
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    err("%s, failed to create socket\n", __func__);
    return -EIO;
  }

  while (arp_get(sockfd, *(uint32_t*)dip, ea, ifname) != 0) {
    sendto(sockfd, dummy_buf, 0, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    st_sleep_ms(100);
  }

  return 0;
}
#endif

int st_arp_init(struct st_main_impl* impl) {
  int ret;

  ret = arp_queues_init(impl);
  if (ret < 0) return ret;

  return 0;
}

int st_arp_uinit(struct st_main_impl* impl) {
  arp_queues_uinit(impl);
  return 0;
}
