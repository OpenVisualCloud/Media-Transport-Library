/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_arp.h"

#include "datapath/mt_queue.h"
// #define DEBUG
#include "mt_log.h"
#include "mt_socket.h"
#include "mt_util.h"

#define ARP_REQ_PERIOD_MS (500)
#define ARP_REQ_PERIOD_US (ARP_REQ_PERIOD_MS * 1000)

static int arp_start_arp_timer(struct mt_arp_impl* arp_impl);

static inline struct mt_arp_impl* get_arp(struct mtl_main_impl* impl,
                                          enum mtl_port port) {
  return impl->arp[port];
}

static void arp_reset(struct mt_arp_impl* arp) {
  struct mt_arp_entry* entry = NULL;

  for (int i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    entry = &arp->entries[i];

    mt_atomic32_set(&entry->mac_ready, 0);
    entry->ip = 0;
    memset(&entry->ea, 0, sizeof(entry->ea));
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

  struct rte_mbuf* rpl_pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!rpl_pkt) {
    err("%s(%d), rpl_pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  rpl_pkt->pkt_len = rpl_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(rpl_pkt, struct rte_ether_hdr*);
  mt_macaddr_get(impl, port, mt_eth_s_addr(eth));
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
  mt_macaddr_get(impl, port, &arp->arp_data.arp_sha);
  arp->arp_data.arp_sip = *(uint32_t*)mt_sip_addr(impl, port);

  /* send arp reply packet */
  uint16_t send = mt_sys_queue_tx_burst(impl, port, &rpl_pkt, 1);
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
  uint8_t* addr_bytes = reply->arp_data.arp_sha.addr_bytes;
  info_once("%s(%d), from %d.%d.%d.%d, mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
            __func__, port, ip[0], ip[1], ip[2], ip[3], addr_bytes[0], addr_bytes[1],
            addr_bytes[2], addr_bytes[3], addr_bytes[4], addr_bytes[5]);

  struct mt_arp_impl* arp_impl = get_arp(impl, port);
  struct mt_arp_entry* entry = NULL;

  /* check if our request */
  mt_pthread_mutex_lock(&arp_impl->mutex);
  int i;
  for (i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    entry = &arp_impl->entries[i];

    if (entry->ip == reply->arp_data.arp_sip) break;
  }
  if (i >= MT_ARP_ENTRY_MAX) {
    err_once("%s(%d), not our arp request, from %d.%d.%d.%d\n", __func__, port, ip[0],
             ip[1], ip[2], ip[3]);
    mt_pthread_mutex_unlock(&arp_impl->mutex);
    return -EINVAL;
  }

  /* save to arp table */
  memcpy(entry->ea.addr_bytes, reply->arp_data.arp_sha.addr_bytes, RTE_ETHER_ADDR_LEN);
  mt_atomic32_set_release(&entry->mac_ready, 1);
  mt_pthread_mutex_unlock(&arp_impl->mutex);

  return 0;
}

static int arp_send_req(struct mtl_main_impl* impl, enum mtl_port port, uint32_t ip) {
  struct rte_mbuf* req_pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!req_pkt) {
    err("%s(%d), req_pkt malloc fail\n", __func__, port);
    return -ENOMEM;
  }

  req_pkt->pkt_len = req_pkt->data_len =
      sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

  struct rte_ether_hdr* eth = rte_pktmbuf_mtod(req_pkt, struct rte_ether_hdr*);
  mt_macaddr_get(impl, port, mt_eth_s_addr(eth));
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
  mt_macaddr_get(impl, port, &arp->arp_data.arp_sha);
  memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);

  uint16_t send = mt_sys_queue_tx_burst(impl, port, &req_pkt, 1);
  if (send < 1) {
    err("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(req_pkt);
    return -EIO;
  }

  dbg("%s(%d), ip 0x%x\n", __func__, port, ip);
  return 0;
}

static int arp_get_result(struct mt_arp_impl* arp_impl, struct mt_arp_entry* entry,
                          int timeout_ms) {
  enum mtl_port port = arp_impl->port;
  int retry = 0;
  int max_retry = 0;
  int sleep_interval_ms = 500;

  if (timeout_ms) max_retry = (timeout_ms / sleep_interval_ms) + 1;

  /* wait the arp result */
  while (!mt_atomic32_read_acquire(&entry->mac_ready)) {
    if (mt_aborted(arp_impl->parent)) {
      err("%s(%d), cache fail as user aborted\n", __func__, port);
      return -EIO;
    }
    if (retry >= max_retry) {
      if (max_retry) /* log only if not zero timeout */
        err("%s(%d), cache fail as timeout to %d ms\n", __func__, port, timeout_ms);
      return -EIO;
    }
    mt_sleep_ms(sleep_interval_ms);
    retry++;
    if (0 == (retry % 10)) {
      uint8_t ip[MTL_IP_ADDR_LEN];
      mt_u32_to_ip(entry->ip, ip);
      info("%s(%d), cache waiting arp from %d.%d.%d.%d\n", __func__, port, ip[0], ip[1],
           ip[2], ip[3]);
    }
  }

  /* mac ready now */
  return 0;
}

static void arp_timer_cb(void* param) {
  struct mt_arp_impl* arp_impl = param;
  struct mt_arp_entry* entry = NULL;
  enum mtl_port port = arp_impl->port;
  struct mtl_main_impl* impl = arp_impl->parent;
  int pending = 0;

  dbg("%s(%d), start\n", __func__, port);
  mt_pthread_mutex_lock(&arp_impl->mutex);
  for (int i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    entry = &arp_impl->entries[i];

    if (entry->ip && !mt_atomic32_read_acquire(&entry->mac_ready)) {
      /* has request but not get arp reply */
      arp_send_req(impl, port, entry->ip);
      pending++;
    }
  }
  arp_impl->timer_active = false;
  mt_pthread_mutex_unlock(&arp_impl->mutex);

  if (pending > 0) {
    arp_start_arp_timer(arp_impl);
    dbg("%s(%d), start arp timer for %d req\n", __func__, port, pending);
  }
}

static int arp_start_arp_timer(struct mt_arp_impl* arp_impl) {
  int ret = 0;

  mt_pthread_mutex_lock(&arp_impl->mutex);
  if (!arp_impl->timer_active) {
    dbg("%s(%d), start arp timer\n", __func__, arp_impl->port);
    ret = rte_eal_alarm_set(ARP_REQ_PERIOD_US, arp_timer_cb, arp_impl);
    if (ret >= 0) {
      arp_impl->timer_active = true;
    } else {
      err("%s(%d), start arp timer fail %d\n", __func__, arp_impl->port, ret);
    }
  }
  mt_pthread_mutex_unlock(&arp_impl->mutex);

  return ret;
}

int mt_arp_parse(struct mtl_main_impl* impl, struct rte_arp_hdr* hdr,
                 enum mtl_port port) {
  switch (ntohs(hdr->arp_opcode)) {
    case RTE_ARP_OP_REQUEST:
      return arp_receive_request(impl, hdr, port);
    case RTE_ARP_OP_REPLY:
      return arp_receive_reply(impl, hdr, port);
    default:
      err("%s, mt_arp_parse %04x unimplemented\n", __func__, ntohs(hdr->arp_opcode));
      return -EINVAL;
  }
}

static int mt_arp_cni_get_mac(struct mtl_main_impl* impl, struct rte_ether_addr* ea,
                              enum mtl_port port, uint32_t ip, int timeout_ms) {
  struct mt_arp_impl* arp_impl = get_arp(impl, port);
  struct mt_arp_entry* entry = NULL;
  int ret;

  mt_pthread_mutex_lock(&arp_impl->mutex);
  int i;
  for (i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    entry = &arp_impl->entries[i];

    if (entry->ip == ip) { /* arp request sent already */
      mt_pthread_mutex_unlock(&arp_impl->mutex);
      goto get_result;
    }
  }

  /* not sent, try find one empty slot */
  for (i = 0; i < MT_ARP_ENTRY_MAX; i++) {
    entry = &arp_impl->entries[i];

    /* find a empty slot */
    if (entry->ip == 0) break;
  }
  if (i >= MT_ARP_ENTRY_MAX) {
    warn("%s(%d), reset arp\n", __func__, port);
    /* arp table full, flush it, do we need extra protect? */
    arp_reset(arp_impl);
    i = 0;
  }
  entry = &arp_impl->entries[i];
  entry->ip = ip;
  mt_atomic32_set(&entry->mac_ready, 0);
  mt_pthread_mutex_unlock(&arp_impl->mutex);

  uint8_t addr[MTL_IP_ADDR_LEN];
  mt_u32_to_ip(ip, addr);
  info("%s(%d), %d.%d.%d.%d alloc at %d\n", __func__, port, addr[0], addr[1], addr[2],
       addr[3], i);

  /* arp and wait the reply */
  arp_send_req(impl, port, ip);  /* send the first arp request packet */
  arp_start_arp_timer(arp_impl); /* start the timer to monitor if send arp again */

get_result:
  /* wait the arp result */
  ret = arp_get_result(arp_impl, entry, timeout_ms);
  if (ret >= 0) {
    /* ready now, copy the mac addr arp result */
    memcpy(ea->addr_bytes, entry->ea.addr_bytes, RTE_ETHER_ADDR_LEN);
  }
  return ret;
}

int mt_arp_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);

  for (int i = 0; i < num_ports; i++) {
    if (mt_has_virtio_user(impl, i)) continue; /* use kernel path */
    struct mt_arp_impl* arp = mt_rte_zmalloc_socket(sizeof(*arp), socket);
    if (!arp) {
      err("%s(%d), arp malloc fail\n", __func__, i);
      mt_arp_uinit(impl);
      return -ENOMEM;
    }

    mt_pthread_mutex_init(&arp->mutex, NULL);
    arp->port = i;
    arp->parent = impl;

    /* assign arp instance */
    impl->arp[i] = arp;
  }

  return 0;
}

int mt_arp_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_arp_impl* arp = get_arp(impl, i);
    if (!arp) continue;

    mt_pthread_mutex_destroy(&arp->mutex);

    /* free the memory */
    mt_rte_free(arp);
    impl->arp[i] = NULL;
  }

  return 0;
}

int mt_arp_get_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                   struct rte_ether_addr* ea, enum mtl_port port, int timeout_ms) {
  int ret;

  dbg("%s(%d), start to get mac for ip %d.%d.%d.%d\n", __func__, port, dip[0], dip[1],
      dip[2], dip[3]);
  if (mt_drv_use_kernel_ctl(impl, port) || mt_has_virtio_user(impl, port)) {
    ret = mt_socket_get_mac(impl, mt_kernel_if_name(impl, port), dip, ea, timeout_ms);
    if (ret < 0) {
      dbg("%s(%d), failed to get mac from socket %d\n", __func__, port, ret);
      return ret;
    }
  } else {
    ret = mt_arp_cni_get_mac(impl, ea, port, mt_ip_to_u32(dip), timeout_ms);
    if (ret < 0) {
      dbg("%s(%d), failed to get mac from cni %d\n", __func__, port, ret);
      return ret;
    }
  }

  return 0;
}
