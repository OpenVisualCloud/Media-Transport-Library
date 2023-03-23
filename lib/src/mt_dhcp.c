/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_dhcp.h"

#include "mt_dev.h"
#include "mt_log.h"

#define DHCP_OP_BOOTREQUEST (1)
#define DHCP_OP_BOOTREPLY (2)
#define DHCP_HTYPE_ETHERNET (1)
#define DHCP_HLEN_ETHERNET (6)
#define DHCP_MAGIC_COOKIE (0x63825363)

#define DHCP_OPTION_END (255)
#define DHCP_OPTION_SUBNET_MASK (1)
#define DHCP_OPTION_ROUTER (3)
#define DHCP_OPTION_DNS_SERVER (6)
#define DHCP_OPTION_REQUESTED_IP_ADDRESS (50)
#define DHCP_OPTION_LEASE_TIME (51)
#define DHCP_OPTION_MESSAGE_TYPE (53)
#define DHCP_OPTION_SERVER_IDENTIFIER (54)
#define DHCP_OPTION_PARAMETER_REQUEST_LIST (55)

#define DHCP_MESSAGE_TYPE_DISCOVER (1)
#define DHCP_MESSAGE_TYPE_OFFER (2)
#define DHCP_MESSAGE_TYPE_REQUEST (3)
#define DHCP_MESSAGE_TYPE_ACK (5)
#define DHCP_MESSAGE_TYPE_NAK (6)
#define DHCP_MESSAGE_TYPE_RELEASE (7)

static inline struct mt_dhcp_impl* get_dhcp(struct mtl_main_impl* impl,
                                            enum mtl_port port) {
  return impl->dhcp[port];
}

static inline void dhcp_set_status(struct mt_dhcp_impl* dhcp,
                                   enum mt_dhcp_status status) {
  mt_pthread_mutex_lock(&dhcp->mutex);
  dhcp->status = status;
  mt_pthread_mutex_unlock(&dhcp->mutex);
}

static int dhcp_send_discover(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  struct rte_mbuf* pkt;
  struct mt_dhcp_hdr* dhcp;
  struct rte_ether_hdr* eth;
  struct rte_ipv4_hdr* ip;
  struct rte_udp_hdr* udp;
  uint8_t* options;
  size_t hdr_offset = 0;

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr*);
  uint16_t port_id = mt_port_id(impl, port);
  rte_eth_macaddr_get(port_id, mt_eth_s_addr(eth));
  memset(mt_eth_d_addr(eth), 0xFF, RTE_ETHER_ADDR_LEN); /* send to broadcast */
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth);

  ip = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip->time_to_live = 128;
  ip->type_of_service = 0;
  ip->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip->hdr_checksum = 0;
  ip->next_proto_id = IPPROTO_UDP;
  ip->src_addr = 0;
  ip->dst_addr = 0xFFFFFFFF; /* send to broadcast */
  hdr_offset += sizeof(*ip);

  udp = rte_pktmbuf_mtod_offset(pkt, struct rte_udp_hdr*, hdr_offset);
  udp->src_port = htons(MT_DHCP_UDP_CLIENT_PORT);
  udp->dst_port = htons(MT_DHCP_UDP_SERVER_PORT);
  hdr_offset += sizeof(*udp);

  dhcp = rte_pktmbuf_mtod_offset(pkt, struct mt_dhcp_hdr*, hdr_offset);
  memset(dhcp, 0, sizeof(*dhcp));
  dhcp->op = DHCP_OP_BOOTREQUEST;
  dhcp->htype = DHCP_HTYPE_ETHERNET;
  dhcp->hlen = DHCP_HLEN_ETHERNET;
  dhcp->xid = htonl(dhcp_impl->xid);
  dhcp->magic_cookie = htonl(DHCP_MAGIC_COOKIE);
  rte_memcpy(dhcp->chaddr, eth->src_addr.addr_bytes, sizeof(eth->src_addr.addr_bytes));
  options = dhcp->options;
  *options++ = DHCP_OPTION_MESSAGE_TYPE;
  *options++ = 1;
  *options++ = DHCP_MESSAGE_TYPE_DISCOVER;
  *options++ = DHCP_OPTION_PARAMETER_REQUEST_LIST;
  *options++ = 3;
  *options++ = DHCP_OPTION_SUBNET_MASK;
  *options++ = DHCP_OPTION_ROUTER;
  *options++ = DHCP_OPTION_DNS_SERVER;
  *options++ = DHCP_OPTION_END;
  hdr_offset += sizeof(*dhcp) + RTE_PTR_DIFF(options, dhcp->options);

  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->data_len = hdr_offset;

  /* update length */
  ip->total_length = htons(pkt->pkt_len - sizeof(*eth));
  udp->dgram_len = htons(pkt->pkt_len - sizeof(*eth) - sizeof(*ip));

  /* send dhcp discover packet */
  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (send < 1) {
    err_once("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  dhcp_set_status(dhcp_impl, MT_DHCP_STATUS_DISCOVERING);

  info("%s(%d), dhcp discover sent\n", __func__, port);

  return 0;
}

static int dhcp_send_request(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  struct rte_mbuf* pkt;
  struct mt_dhcp_hdr* dhcp;
  struct rte_ether_hdr* eth;
  struct rte_ipv4_hdr* ip;
  struct rte_udp_hdr* udp;
  uint8_t* options;
  size_t hdr_offset = 0;

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr*);
  uint16_t port_id = mt_port_id(impl, port);
  rte_eth_macaddr_get(port_id, mt_eth_s_addr(eth));
  memset(mt_eth_d_addr(eth), 0xFF, RTE_ETHER_ADDR_LEN);
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth);

  ip = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip->time_to_live = 128;
  ip->type_of_service = 0;
  ip->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip->hdr_checksum = 0;
  ip->next_proto_id = IPPROTO_UDP;
  ip->src_addr = 0;
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  if (dhcp_impl->status == MT_DHCP_STATUS_RENEWING)
    ip->dst_addr = htonl(*(uint32_t*)dhcp_impl->server_ip);
  else
    ip->dst_addr = 0xFFFFFFFF;
  mt_pthread_mutex_unlock(&dhcp_impl->mutex);
  hdr_offset += sizeof(*ip);

  udp = rte_pktmbuf_mtod_offset(pkt, struct rte_udp_hdr*, hdr_offset);
  udp->src_port = htons(MT_DHCP_UDP_CLIENT_PORT);
  udp->dst_port = htons(MT_DHCP_UDP_SERVER_PORT);
  hdr_offset += sizeof(*udp);

  dhcp = rte_pktmbuf_mtod_offset(pkt, struct mt_dhcp_hdr*, hdr_offset);
  memset(dhcp, 0, sizeof(*dhcp));
  dhcp->op = DHCP_OP_BOOTREQUEST;
  dhcp->htype = DHCP_HTYPE_ETHERNET;
  dhcp->hlen = DHCP_HLEN_ETHERNET;
  dhcp->xid = htonl(dhcp_impl->xid);
  dhcp->magic_cookie = htonl(DHCP_MAGIC_COOKIE);
  if (dhcp_impl->status == MT_DHCP_STATUS_RENEWING ||
      dhcp_impl->status == MT_DHCP_STATUS_REBINDING)
    dhcp->ciaddr = htonl(*(uint32_t*)dhcp_impl->ip);
  rte_memcpy(dhcp->chaddr, eth->src_addr.addr_bytes, sizeof(eth->src_addr.addr_bytes));
  options = dhcp->options;
  *options++ = DHCP_OPTION_MESSAGE_TYPE;
  *options++ = 1;
  *options++ = DHCP_MESSAGE_TYPE_REQUEST;
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  if (dhcp_impl->status != MT_DHCP_STATUS_RENEWING) {
    *options++ = DHCP_OPTION_REQUESTED_IP_ADDRESS;
    *options++ = 4;
    *options++ = dhcp_impl->ip[0];
    *options++ = dhcp_impl->ip[1];
    *options++ = dhcp_impl->ip[2];
    *options++ = dhcp_impl->ip[3];
  }
  if (dhcp_impl->status != MT_DHCP_STATUS_RENEWING &&
      dhcp_impl->status != MT_DHCP_STATUS_REBINDING) {
    *options++ = DHCP_OPTION_SERVER_IDENTIFIER;
    *options++ = 4;
    *options++ = dhcp_impl->server_ip[0];
    *options++ = dhcp_impl->server_ip[1];
    *options++ = dhcp_impl->server_ip[2];
    *options++ = dhcp_impl->server_ip[3];
  }
  mt_pthread_mutex_unlock(&dhcp_impl->mutex);
  *options++ = DHCP_OPTION_PARAMETER_REQUEST_LIST;
  *options++ = 3;
  *options++ = DHCP_OPTION_SUBNET_MASK;
  *options++ = DHCP_OPTION_ROUTER;
  *options++ = DHCP_OPTION_DNS_SERVER;
  *options++ = DHCP_OPTION_END;
  hdr_offset += sizeof(*dhcp) + RTE_PTR_DIFF(options, dhcp->options);

  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->data_len = hdr_offset;

  /* update length */
  ip->total_length = htons(pkt->pkt_len - sizeof(*eth));
  udp->dgram_len = htons(pkt->pkt_len - sizeof(*eth) - sizeof(*ip));

  /* send dhcp request packet */
  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (send < 1) {
    err_once("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  info("%s(%d), dhcp request sent\n", __func__, port);

  return 0;
}

static int dhcp_recv_offer(struct mtl_main_impl* impl, struct mt_dhcp_hdr* offer,
                           enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  if (dhcp_impl->status != MT_DHCP_STATUS_DISCOVERING) {
    dbg("%s(%d), not in discovering status\n", __func__, port);
    mt_pthread_mutex_unlock(&dhcp_impl->mutex);
    return -EIO;
  }
  rte_memcpy(dhcp_impl->ip, &offer->yiaddr, MTL_IP_ADDR_LEN);
  mt_pthread_mutex_unlock(&dhcp_impl->mutex);
  info("%s(%d), received dhcp offer\n", __func__, port);
  info("%s(%d), ip address: %s\n", __func__, dhcp_impl->port,
       inet_ntoa(*(struct in_addr*)dhcp_impl->ip));
  uint8_t* options = offer->options;
  while (*options != DHCP_OPTION_END) {
    if (*options == DHCP_OPTION_SUBNET_MASK)
      dbg("%s(%d), subnet mask: %s\n", __func__, port,
          inet_ntoa(*(struct in_addr*)(options + 2)));
    if (*options == DHCP_OPTION_ROUTER)
      dbg("%s(%d), default gateway: %s\n", __func__, port,
          inet_ntoa(*(struct in_addr*)(options + 2)));
    if (*options == DHCP_OPTION_DNS_SERVER) {
      for (int i = 0; i < options[1] / 4; ++i)
        dbg("%s(%d), dns server %d: %s\n", __func__, port, i,
            inet_ntoa(*(struct in_addr*)(options + 2 + i * 4)));
    }
    if (*options == DHCP_OPTION_SERVER_IDENTIFIER)
      rte_memcpy(dhcp_impl->server_ip, &options[2], MTL_IP_ADDR_LEN);
    options += options[1] + 2;
  }

  dhcp_set_status(dhcp_impl, MT_DHCP_STATUS_REQUESTING);
  dhcp_send_request(impl, port);

  return 0;
}

/* renew at t1 after ack */
static void dhcp_renew_handler(void* param) {
  struct mt_dhcp_impl* dhcp_impl = param;
  dhcp_set_status(dhcp_impl, MT_DHCP_STATUS_RENEWING);
  dhcp_send_request(dhcp_impl->parent, dhcp_impl->port);
}

/* rebind at t2 after ack if not renewed */
static void dhcp_rebind_handler(void* param) {
  struct mt_dhcp_impl* dhcp_impl = param;
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  if (dhcp_impl->status != MT_DHCP_STATUS_BOUND) {
    dhcp_impl->status = MT_DHCP_STATUS_REBINDING;
    mt_pthread_mutex_unlock(&dhcp_impl->mutex);
    dhcp_send_request(dhcp_impl->parent, dhcp_impl->port);
  } else
    mt_pthread_mutex_unlock(&dhcp_impl->mutex);
}

/* exceeds lease time */
static void dhcp_lease_handler(void* param) {
  struct mt_dhcp_impl* dhcp_impl = param;
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  if (dhcp_impl->status != MT_DHCP_STATUS_BOUND) {
    dhcp_impl->status = MT_DHCP_STATUS_INIT;
    mt_pthread_mutex_unlock(&dhcp_impl->mutex);
    dhcp_send_discover(dhcp_impl->parent, dhcp_impl->port);
  } else
    mt_pthread_mutex_unlock(&dhcp_impl->mutex);
}

static int dhcp_recv_ack(struct mtl_main_impl* impl, struct mt_dhcp_hdr* ack,
                         enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  int ret;
  double t = 0.0, t1 = 0.0, t2 = 0.0;
  uint8_t* options = ack->options;
  mt_pthread_mutex_lock(&dhcp_impl->mutex);
  rte_memcpy(dhcp_impl->ip, &ack->yiaddr, MTL_IP_ADDR_LEN);
  while (*options != DHCP_OPTION_END) {
    if (*options == DHCP_OPTION_SUBNET_MASK)
      rte_memcpy(dhcp_impl->netmask, &options[2], MTL_IP_ADDR_LEN);
    if (*options == DHCP_OPTION_ROUTER)
      rte_memcpy(dhcp_impl->gateway, &options[2], MTL_IP_ADDR_LEN);
    if (*options == DHCP_OPTION_LEASE_TIME) {
      t = ntohl(*(uint32_t*)(options + 2));
      t1 = t * 0.5;
      t2 = t * 0.875;
    }
    if (*options == DHCP_OPTION_SERVER_IDENTIFIER)
      rte_memcpy(dhcp_impl->server_ip, &options[2], MTL_IP_ADDR_LEN);
    options += options[1] + 2;
  }
  mt_pthread_mutex_unlock(&dhcp_impl->mutex);

  ret = rte_eal_alarm_set(t1 * US_PER_S, dhcp_renew_handler, dhcp_impl);
  if (ret < 0) {
    err("%s(%d), start renew timer fail %d, t1 %lf\n", __func__, dhcp_impl->port, ret,
        t1);
    return ret;
  }

  ret = rte_eal_alarm_set(t2 * US_PER_S, dhcp_rebind_handler, dhcp_impl);
  if (ret < 0) {
    err("%s(%d), start rebind timer fail %d, t2 %lf\n", __func__, dhcp_impl->port, ret,
        t2);
    return ret;
  }

  ret = rte_eal_alarm_set(t * US_PER_S, dhcp_lease_handler, dhcp_impl);
  if (ret < 0) {
    err("%s(%d), start lease timer fail %d, t %lf\n", __func__, dhcp_impl->port, ret, t);
    return ret;
  }

  dhcp_set_status(dhcp_impl, MT_DHCP_STATUS_BOUND);

  info("%s(%d), dhcp configuration done\n", __func__, dhcp_impl->port);
  info("%s(%d), ip address: %s\n", __func__, dhcp_impl->port,
       inet_ntoa(*(struct in_addr*)dhcp_impl->ip));
  info("%s(%d), subnet mask: %s\n", __func__, dhcp_impl->port,
       inet_ntoa(*(struct in_addr*)dhcp_impl->netmask));
  info("%s(%d), default gateway: %s\n", __func__, dhcp_impl->port,
       inet_ntoa(*(struct in_addr*)dhcp_impl->gateway));
  info("%s(%d), lease time: %u s\n", __func__, dhcp_impl->port, (uint32_t)t);

  return 0;
}

static int dhcp_send_release(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  struct rte_mbuf* pkt;
  struct mt_dhcp_hdr* dhcp;
  struct rte_ether_hdr* eth;
  struct rte_ipv4_hdr* ip;
  struct rte_udp_hdr* udp;
  uint8_t* options;
  size_t hdr_offset = 0;

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr*);
  uint16_t port_id = mt_port_id(impl, port);
  rte_eth_macaddr_get(port_id, mt_eth_s_addr(eth));
  memset(mt_eth_d_addr(eth), 0xFF, RTE_ETHER_ADDR_LEN);
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth);

  ip = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip->time_to_live = 128;
  ip->type_of_service = 0;
  ip->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip->hdr_checksum = 0;
  ip->next_proto_id = IPPROTO_UDP;
  ip->src_addr = 0;
  ip->dst_addr = htonl(*(uint32_t*)dhcp_impl->server_ip);
  hdr_offset += sizeof(*ip);

  udp = rte_pktmbuf_mtod_offset(pkt, struct rte_udp_hdr*, hdr_offset);
  udp->src_port = htons(MT_DHCP_UDP_CLIENT_PORT);
  udp->dst_port = htons(MT_DHCP_UDP_SERVER_PORT);
  hdr_offset += sizeof(*udp);

  dhcp = rte_pktmbuf_mtod_offset(pkt, struct mt_dhcp_hdr*, hdr_offset);
  memset(dhcp, 0, sizeof(*dhcp));
  dhcp->op = DHCP_OP_BOOTREQUEST;
  dhcp->htype = DHCP_HTYPE_ETHERNET;
  dhcp->hlen = DHCP_HLEN_ETHERNET;
  dhcp->xid = rand();
  dhcp->magic_cookie = htonl(DHCP_MAGIC_COOKIE);
  dhcp->ciaddr = htonl(*(uint32_t*)dhcp_impl->ip);
  rte_memcpy(dhcp->chaddr, eth->src_addr.addr_bytes, sizeof(eth->src_addr.addr_bytes));
  options = dhcp->options;
  *options++ = DHCP_OPTION_MESSAGE_TYPE;
  *options++ = 1;
  *options++ = DHCP_MESSAGE_TYPE_RELEASE;
  *options++ = DHCP_OPTION_REQUESTED_IP_ADDRESS;
  *options++ = 4;
  *options++ = dhcp_impl->ip[0];
  *options++ = dhcp_impl->ip[1];
  *options++ = dhcp_impl->ip[2];
  *options++ = dhcp_impl->ip[3];
  *options++ = DHCP_OPTION_SERVER_IDENTIFIER;
  *options++ = 4;
  *options++ = dhcp_impl->server_ip[0];
  *options++ = dhcp_impl->server_ip[1];
  *options++ = dhcp_impl->server_ip[2];
  *options++ = dhcp_impl->server_ip[3];
  *options++ = DHCP_OPTION_END;
  hdr_offset += sizeof(*dhcp) + RTE_PTR_DIFF(options, dhcp->options);

  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->data_len = hdr_offset;

  /* update length */
  ip->total_length = htons(pkt->pkt_len - sizeof(*eth));
  udp->dgram_len = htons(pkt->pkt_len - sizeof(*eth) - sizeof(*ip));

  /* send dhcp release packet */
  uint16_t send = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (send < 1) {
    err_once("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  dhcp_set_status(dhcp_impl, MT_DHCP_STATUS_INIT);

  return 0;
}

int mt_dhcp_parse(struct mtl_main_impl* impl, struct mt_dhcp_hdr* hdr,
                  enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);
  if (ntohl(hdr->magic_cookie) != DHCP_MAGIC_COOKIE) {
    err("%s(%d), invalid magic cookie 0x%" PRIx32 "\n", __func__, port,
        ntohl(hdr->magic_cookie));
    return -EINVAL;
  }

  if (hdr->op != DHCP_OP_BOOTREPLY) {
    err("%s(%d), invalid op %u\n", __func__, port, hdr->op);
    return -EINVAL;
  }

  if (ntohl(hdr->xid) != dhcp_impl->xid) {
    err("%s(%d), xid mismatch 0x%" PRIx32 " : 0x%" PRIx32 "\n", __func__, port,
        ntohl(hdr->xid), dhcp_impl->xid);
    return -EINVAL;
  }

  uint8_t* options = hdr->options;
  if (*options != DHCP_OPTION_MESSAGE_TYPE) {
    err("%s(%d), invalid option field %u\n", __func__, port, *options);
    return -EINVAL;
  }

  switch (options[2]) {
    case DHCP_MESSAGE_TYPE_OFFER:
      dhcp_recv_offer(impl, hdr, port);
      break;
    case DHCP_MESSAGE_TYPE_ACK:
      dhcp_recv_ack(impl, hdr, port);
      break;
    case DHCP_MESSAGE_TYPE_NAK:
      /* restart the cycle */
      dhcp_send_discover(impl, port);
      break;
    default:
      err("%s(%d), invalid dhcp message type %u\n", __func__, port, options[2]);
      return -EINVAL;
  }

  return 0;
}

uint8_t* mt_dhcp_get_ip(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);

  if (dhcp_impl->status != MT_DHCP_STATUS_BOUND &&
      dhcp_impl->status != MT_DHCP_STATUS_RENEWING &&
      dhcp_impl->status != MT_DHCP_STATUS_REBINDING) {
    dbg("%s(%d), ip may not be usable\n", __func__, port);
  }

  return dhcp_impl->ip;
}

uint8_t* mt_dhcp_get_netmask(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);

  if (dhcp_impl->status != MT_DHCP_STATUS_BOUND &&
      dhcp_impl->status != MT_DHCP_STATUS_RENEWING &&
      dhcp_impl->status != MT_DHCP_STATUS_REBINDING) {
    dbg("%s(%d), netmask may not be usable\n", __func__, port);
  }

  return dhcp_impl->netmask;
}

uint8_t* mt_dhcp_get_gateway(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_dhcp_impl* dhcp_impl = get_dhcp(impl, port);

  if (dhcp_impl->status != MT_DHCP_STATUS_BOUND &&
      dhcp_impl->status != MT_DHCP_STATUS_RENEWING &&
      dhcp_impl->status != MT_DHCP_STATUS_REBINDING) {
    dbg("%s(%d), gateway may not be usable\n", __func__, port);
  }

  return dhcp_impl->gateway;
}

int mt_dhcp_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);

  for (int i = 0; i < num_ports; i++) {
    if (mt_if(impl, i)->net_proto != MTL_PROTO_DHCP) continue;
    struct mt_dhcp_impl* dhcp = mt_rte_zmalloc_socket(sizeof(*dhcp), socket);
    if (!dhcp) {
      err("%s(%d), dhcp malloc fail\n", __func__, i);
      mt_dhcp_uinit(impl);
      return -ENOMEM;
    }

    mt_pthread_mutex_init(&dhcp->mutex, NULL);
    dhcp->port = i;
    dhcp->parent = impl;
    dhcp->status = MT_DHCP_STATUS_INIT;
    dhcp->xid = rand();

    /* assign dhcp instance */
    impl->dhcp[i] = dhcp;

    /* trigger discover at init */
    dhcp_send_discover(impl, i);
  }

  int done, max_retry = 50;
  while (--max_retry) {
    done = 0;
    for (int i = 0; i < num_ports; i++)
      if (impl->dhcp[i]->status == MT_DHCP_STATUS_BOUND) done++;
    if (done == num_ports) break;
    mt_sleep_ms(100);
  }
  if (done != num_ports) {
    err("%s, dhcp init fail\n", __func__);
    mt_dhcp_uinit(impl);
    return -ETIME;
  }

  return 0;
}

int mt_dhcp_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_dhcp_impl* dhcp = get_dhcp(impl, i);
    if (!dhcp) continue;

    /* send release to server */
    dhcp_send_release(impl, i);

    mt_pthread_mutex_destroy(&dhcp->mutex);

    /* free the memory */
    mt_rte_free(dhcp);
    impl->dhcp[i] = NULL;
  }

  return 0;
}
