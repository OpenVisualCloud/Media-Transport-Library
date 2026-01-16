/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "udp_main.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"
#include "udp_rxq.h"

#ifndef UDP_SEGMENT
/* fix for centos build */
#define UDP_SEGMENT 103 /* Set GSO segmentation size */
#endif

#ifndef SO_COOKIE
/* fix for centos 7 build */
#define SO_COOKIE 57
#endif

static inline void udp_set_flag(struct mudp_impl* s, uint32_t flag) {
  s->flags |= flag;
}

static inline void udp_clear_flag(struct mudp_impl* s, uint32_t flag) {
  s->flags &= ~flag;
}

static inline bool udp_get_flag(struct mudp_impl* s, uint32_t flag) {
  if (s->flags & flag)
    return true;
  else
    return false;
}

static inline bool udp_alive(struct mudp_impl* s) {
  if (!mt_aborted(s->parent) && s->alive)
    return true;
  else
    return false;
}

static inline bool udp_is_fallback(struct mudp_impl* s) {
  if (s->fallback_fd >= 0)
    return true;
  else
    return false;
}

int mudp_verify_socket_args(int domain, int type, int protocol) {
  if (domain != AF_INET) {
    dbg("%s, invalid domain %d\n", __func__, domain);
    MUDP_ERR_RET(EINVAL);
  }
  if ((type != SOCK_DGRAM) && (type != (SOCK_DGRAM | SOCK_NONBLOCK))) {
    dbg("%s, invalid type %d\n", __func__, type);
    MUDP_ERR_RET(EINVAL);
  }
  if ((protocol != 0) && (protocol != IPPROTO_UDP)) {
    dbg("%s, invalid protocol %d\n", __func__, protocol);
    MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

static int udp_verify_addr(const struct sockaddr_in* addr, socklen_t addrlen) {
  if (addr->sin_family != AF_INET) {
    err("%s, invalid sa_family %d\n", __func__, addr->sin_family);
    MUDP_ERR_RET(EINVAL);
  }
  if (addrlen < sizeof(*addr)) {
    err("%s, invalid addrlen %d\n", __func__, addrlen);
    MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

static int udp_verify_bind_addr(struct mudp_impl* s, const struct sockaddr_in* addr,
                                socklen_t addrlen) {
  int idx = s->idx;
  int ret;

  ret = udp_verify_addr(addr, addrlen);
  if (ret < 0) return ret;

  if (!udp_get_flag(s, MUDP_BIND_ADDRESS_CHECK)) return 0;

  /* check if our IP or any IP */
  if (addr->sin_addr.s_addr == INADDR_ANY)
    return 0; /* kernel mcast bind use INADDR_ANY */
  /* should we support INADDR_LOOPBACK? */
  if (memcmp(&addr->sin_addr.s_addr, mt_sip_addr(s->parent, s->port), MTL_IP_ADDR_LEN)) {
    uint8_t* ip = (uint8_t*)&addr->sin_addr.s_addr;
    err("%s(%d), invalid bind ip %u.%u.%u.%u\n", __func__, idx, ip[0], ip[1], ip[2],
        ip[3]);
    MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

static int udp_verify_sendto_args(size_t len, int flags, const struct sockaddr_in* addr,
                                  socklen_t addrlen) {
  int ret = udp_verify_addr(addr, addrlen);
  if (ret < 0) return ret;

  if ((len <= 0) || (len > MUDP_MAX_GSO_BYTES)) {
    err("%s, invalid len %" PRIu64 "\n", __func__, len);
    MUDP_ERR_RET(EINVAL);
  }
  if (flags) {
    err("%s, invalid flags %d\n", __func__, flags);
    MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

static int udp_verify_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  MTL_MAY_UNUSED(timeout);

  if (!fds) {
    err("%s, NULL fds\n", __func__);
    MUDP_ERR_RET(EINVAL);
  }
  if (nfds <= 0) {
    err("%s, invalid nfds %d\n", __func__, (int)nfds);
    MUDP_ERR_RET(EINVAL);
  }
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    if (!(fds[i].events & POLLIN)) {
      err("%s(%d), invalid events 0x%x\n", __func__, (int)i, fds[i].events);
      MUDP_ERR_RET(EINVAL);
    }
    fds[i].revents = 0;
  }

  return 0;
}

static int udp_build_tx_pkt(struct mtl_main_impl* impl, struct mudp_impl* s,
                            struct rte_mbuf* pkt, const void* buf, size_t len,
                            const struct sockaddr_in* addr_in, int arp_timeout_ms) {
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  enum mtl_port port = s->port;
  int idx = s->idx;
  int ret;

  if (len > MUDP_MAX_BYTES) {
    err("%s(%d), invalid len %" PRId64 "\n", __func__, idx, len);
    MUDP_ERR_RET(EIO);
  }

  /* copy eth, ip, udp */
  rte_memcpy(hdr, &s->hdr, sizeof(*hdr));

  /* eth */
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);
  uint8_t* dip = (uint8_t*)&addr_in->sin_addr;
  if (udp_get_flag(s, MUDP_TX_USER_MAC)) {
    rte_memcpy(d_addr->addr_bytes, s->user_mac, RTE_ETHER_ADDR_LEN);
  } else {
    ret = mt_dst_ip_mac(impl, dip, d_addr, port, arp_timeout_ms);
    if (ret < 0) {
      if (arp_timeout_ms) /* log only if not zero timeout */
        err("%s(%d), mt_dst_ip_mac fail %d for %u.%u.%u.%u\n", __func__, idx, ret, dip[0],
            dip[1], dip[2], dip[3]);
      s->stat_pkt_arp_fail++;
      MUDP_ERR_RET(EIO);
    }
  }

  /* ip */
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

  /* udp */
  udp->dst_port = addr_in->sin_port;

  /* pkt mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP;
  pkt->data_len = len + sizeof(*hdr);
  pkt->pkt_len = pkt->data_len;

  /* copy payload */
  void* payload = (uint8_t*)udp + sizeof(struct rte_udp_hdr);
  mtl_memcpy(payload, buf, len);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!mt_if_has_offload_ipv4_cksum(impl, port)) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  s->stat_pkt_build++;
  return 0;
}

static ssize_t udp_msg_len(const struct msghdr* msg) {
  size_t len = 0;
  for (int i = 0; i < msg->msg_iovlen; i++) {
    len += msg->msg_iov[i].iov_len;
  }
  return len;
}

static int udp_cmsg_handle(struct mudp_impl* s, const struct msghdr* msg) {
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg);
  if (!cmsg) return 0;
  int idx = s->idx;

  switch (cmsg->cmsg_level) {
    case SOL_UDP:
      if (cmsg->cmsg_type == UDP_SEGMENT) {
        if (cmsg->cmsg_len == CMSG_LEN(sizeof(uint16_t))) {
          uint16_t* p_val = (uint16_t*)CMSG_DATA(cmsg);
          uint16_t val = *p_val;
          dbg("%s(%d), UDP_SEGMENT val %u\n", __func__, idx, val);
          s->gso_segment_sz = val;
        } else {
          err("%s(%d), unknow cmsg_len %" PRId64 " for UDP_SEGMENT\n", __func__, idx,
              cmsg->cmsg_len);
          MUDP_ERR_RET(EINVAL);
        }
      }
      break;
    default:
      break;
  }

  return 0;
}

static int udp_build_tx_msg_pkt(struct mtl_main_impl* impl, struct mudp_impl* s,
                                struct rte_mbuf** pkts, unsigned int pkts_nb,
                                const struct msghdr* msg,
                                const struct sockaddr_in* addr_in, int arp_timeout_ms,
                                size_t sz_per_pkt) {
  enum mtl_port port = s->port;
  int idx = s->idx;
  int ret;

  /* get the dst mac address */
  struct rte_ether_addr d_addr;
  uint8_t* dip = (uint8_t*)&addr_in->sin_addr;
  if (udp_get_flag(s, MUDP_TX_USER_MAC)) {
    rte_memcpy(&d_addr.addr_bytes, s->user_mac, RTE_ETHER_ADDR_LEN);
  } else {
    ret = mt_dst_ip_mac(impl, dip, &d_addr, port, arp_timeout_ms);
    if (ret < 0) {
      if (arp_timeout_ms) /* log only if not zero timeout */
        err("%s(%d), mt_dst_ip_mac fail %d for %u.%u.%u.%u\n", __func__, idx, ret, dip[0],
            dip[1], dip[2], dip[3]);
      s->stat_pkt_arp_fail++;
      MUDP_ERR_RET(EIO);
    }
  }

  void* payloads[pkts_nb];
  memset(payloads, 0, sizeof(payloads)); /* prvents maybe-uninitialized error */

  /* fill hdr info for all pkts */
  for (unsigned int i = 0; i < pkts_nb; i++) {
    struct rte_mbuf* pkt = pkts[i];
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
    struct rte_ether_hdr* eth = &hdr->eth;
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
    struct rte_udp_hdr* udp = &hdr->udp;

    /* copy eth, ip, udp */
    rte_memcpy(hdr, &s->hdr, sizeof(*hdr));
    /* update dst mac */
    rte_memcpy(mt_eth_d_addr(eth), &d_addr, sizeof(d_addr));
    /* ip */
    mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);
    /* udp */
    udp->dst_port = addr_in->sin_port;
    /* pkt mbuf */
    mt_mbuf_init_ipv4(pkt);
    pkt->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP;

    payloads[i] = &udp[1];
    s->stat_pkt_build++;
  }

  unsigned int pkt_idx = 0;
  void* pd = payloads[pkt_idx];
  size_t pd_len = sz_per_pkt;
  /* copy msg buffer to payload */
  for (int i = 0; i < msg->msg_iovlen; i++) {
    size_t iov_len = msg->msg_iov[i].iov_len;
    void* iov = msg->msg_iov[i].iov_base;
    while (iov_len > 0) {
      if (pd_len <= 0) {
        err("%s(%d), no available payload, pkts_nb %u\n", __func__, idx, pkts_nb);
        MUDP_ERR_RET(EIO);
      }
      size_t clen = RTE_MIN(pd_len, iov_len);
      rte_memcpy(pd, iov, clen);
      pd += clen;
      iov += clen;
      iov_len -= clen;
      pd_len -= clen;
      if (pd_len <= 0) {
        pkts[pkt_idx]->data_len = sz_per_pkt + sizeof(struct mt_udp_hdr);
        pkts[pkt_idx]->pkt_len = pkts[pkt_idx]->data_len;
        pkt_idx++;
        dbg("%s(%d), pd to idx %u\n", __func__, idx, pkt_idx);
        if (pkt_idx >= pkts_nb) {
          dbg("%s(%d), pd reach max %u\n", __func__, idx, pkts_nb);
          pd = NULL;
          pd_len = 0;
        } else {
          pd = payloads[pkt_idx];
          pd_len = sz_per_pkt;
        }
      }
    }
  }

  /* update data len for last pkt */
  if ((pd_len > 0) && (pd_len < sz_per_pkt)) {
    pkts[pkt_idx]->data_len = sz_per_pkt - pd_len + sizeof(struct mt_udp_hdr);
    pkts[pkt_idx]->pkt_len = pkts[pkt_idx]->data_len;
  }

  /* fill the info according to the payload */
  for (unsigned int i = 0; i < pkts_nb; i++) {
    struct rte_mbuf* pkt = pkts[i];
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
    struct rte_udp_hdr* udp = &hdr->udp;

    udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
    ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
    if (!mt_if_has_offload_ipv4_cksum(impl, port)) {
      /* generate cksum if no offload */
      ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
    }
  }

  return 0;
}

static unsigned int udp_tx_pkts(struct mtl_main_impl* impl, struct mudp_impl* s,
                                struct rte_mbuf** pkts, unsigned int count) {
  int idx = s->idx;
  unsigned int sent = 0;
  uint64_t start_ts = mt_get_tsc(impl);

  while (1) {
    unsigned int remaining = count - sent;
    sent = mt_txq_burst(s->txq, pkts, remaining);
    s->stat_pkt_tx += sent;
    if (sent >= count) { /* all tx succ */
      return sent;
    }

    /* check timeout */
    unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
    if (us > s->tx_timeout_us) {
      warn("%s(%d), fail as timeout %u us\n", __func__, idx, s->tx_timeout_us);
      return sent;
    }
    s->stat_tx_retry++;
    mt_sleep_us(1);
  }

  /* never reach here */
  err("%s(%d), never reach here\n", __func__, idx);
  return sent;
}

static int udp_bind_port(struct mudp_impl* s, uint16_t bind_port) {
  int idx = s->idx;

  if (!bind_port) {
    bind_port = mt_random_port(s->bind_port);
    info("%s(%d), random bind port number %u\n", __func__, idx, bind_port);
  }
  /* save bind port number */
  s->bind_port = bind_port;
  /* update src port for tx also */
  s->hdr.udp.src_port = htons(bind_port);
  info("%s(%d), bind port number %u\n", __func__, idx, bind_port);
  return 0;
}

static int udp_init_hdr(struct mtl_main_impl* impl, struct mudp_impl* s) {
  struct mt_udp_hdr* hdr = &s->hdr;
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  int idx = s->idx;
  enum mtl_port port = s->port;
  int ret;

  /* dst mac and ip should be filled in the pkt build */

  /* eth */
  memset(eth, 0x0, sizeof(*eth));
  ret = mt_macaddr_get(impl, port, mt_eth_s_addr(eth));
  if (ret < 0) {
    err("%s(%d), macaddr get fail %d for port %d\n", __func__, idx, ret, port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ip header */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0;
  ipv4->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ipv4->next_proto_id = IPPROTO_UDP;
  mtl_memcpy(&ipv4->src_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);

  /* udp */
  memset(udp, 0x0, sizeof(*udp));
  udp->dgram_cksum = 0;

  return 0;
}

static int udp_uinit_txq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  enum mtl_port port = s->port;

  if (s->txq) {
    /* flush all the pkts in the tx pool */
    mt_txq_flush(s->txq, mt_get_pad(impl, port));
    mt_txq_put(s->txq);
    s->txq = NULL;
  }
  if (s->tx_pool_by_queue) {
    /* tsq use same mempool for shared queue */
    if (s->tx_pool) {
      mt_mempool_free(s->tx_pool);
      s->tx_pool = NULL;
    }
  }

  udp_clear_flag(s, MUDP_TXQ_ALLOC);
  return 0;
}

static int udp_init_txq(struct mtl_main_impl* impl, struct mudp_impl* s,
                        const struct sockaddr_in* addr_in) {
  enum mtl_port port = s->port;
  int idx = s->idx;
  uint16_t queue_id;

  dbg("%s(%d), start\n", __func__, idx);

  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.bytes_per_sec = s->txq_bps / 8;
  mtl_memcpy(&flow.dip_addr, &addr_in->sin_addr, MTL_IP_ADDR_LEN);
  flow.dst_port = ntohs(addr_in->sin_port);

  s->txq = mt_txq_get(impl, port, &flow);
  if (!s->txq) {
    err("%s(%d), txq entry get fail\n", __func__, idx);
    udp_uinit_txq(impl, s);
    MUDP_ERR_RET(ENOMEM);
  }
  queue_id = mt_txq_queue_id(s->txq);
  /* shared txq use shared mempool */
  s->tx_pool = mt_txq_mempool(s->txq);
  if (!s->tx_pool) {
    char pool_name[32];
    snprintf(pool_name, 32, "%sP%dQ%uS%d_TX", MUDP_PREFIX, port, queue_id, idx);
    struct rte_mempool* pool = mt_mempool_create(impl, port, pool_name, s->element_nb,
                                                 MT_MBUF_CACHE_SIZE, 0, s->element_size);
    if (!pool) {
      err("%s(%d), mempool create fail\n", __func__, idx);
      udp_uinit_txq(impl, s);
      MUDP_ERR_RET(ENOMEM);
    }
    s->tx_pool = pool;
    s->tx_pool_by_queue = true;
  }

  udp_set_flag(s, MUDP_TXQ_ALLOC);
  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int udp_uinit_rxq(struct mudp_impl* s) {
  if (s->rxq) {
    mur_client_put(s->rxq);
    s->rxq = NULL;
  }
  return 0;
}

static int udp_init_rxq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  int idx = s->idx;

  if (s->rxq) {
    err("%s(%d), rxq already get\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }

  struct mur_client_create create;
  create.impl = impl;
  create.dst_port = s->bind_port;
  create.port = s->port;
  create.ring_count = s->rx_ring_count;
  create.wake_thresh_count = s->wake_thresh_count;
  create.wake_timeout_us = s->wake_timeout_us;
  create.reuse_port = s->reuse_port;
  s->rxq = mur_client_get(&create);
  if (!s->rxq) {
    err("%s(%d), rxq get fail\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }

  return 0;
}

static int udp_stat_dump(void* priv) {
  struct mudp_impl* s = priv;
  int idx = s->idx;
  enum mtl_port port = s->port;

  if (s->rxq) {
    notice("%s(%d,%d), rx ring cnt %u\n", __func__, port, idx,
           rte_ring_count(mur_client_ring(s->rxq)));
  }
  if (s->stat_rx_msg_cnt) {
    notice("%s(%d,%d), rx_msg %u succ %u timeout %u again %u\n", __func__, port, idx,
           s->stat_rx_msg_cnt, s->stat_rx_msg_succ_cnt, s->stat_rx_msg_timeout_cnt,
           s->stat_rx_msg_again_cnt);
    s->stat_rx_msg_cnt = 0;
    s->stat_rx_msg_succ_cnt = 0;
    s->stat_rx_msg_timeout_cnt = 0;
    s->stat_rx_msg_again_cnt = 0;
  }
  if (s->stat_poll_cnt) {
    notice("%s(%d,%d), poll %u succ %u timeout %u 0-timeout %u query_ret %u\n", __func__,
           port, idx, s->stat_poll_cnt, s->stat_poll_succ_cnt, s->stat_poll_timeout_cnt,
           s->stat_poll_zero_timeout_cnt, s->stat_poll_query_ret_cnt);
    s->stat_poll_cnt = 0;
    s->stat_poll_succ_cnt = 0;
    s->stat_poll_timeout_cnt = 0;
    s->stat_poll_zero_timeout_cnt = 0;
    s->stat_poll_query_ret_cnt = 0;
  }
  if (s->stat_pkt_dequeue) {
    notice("%s(%d,%d), pkt dequeue %u deliver %u\n", __func__, port, idx,
           s->stat_pkt_dequeue, s->stat_pkt_deliver);
    s->stat_pkt_dequeue = 0;
    s->stat_pkt_deliver = 0;
  }
  if (s->rxq) mur_client_dump(s->rxq);

  if (s->stat_pkt_build) {
    notice("%s(%d,%d), pkt build %u tx %u\n", __func__, port, idx, s->stat_pkt_build,
           s->stat_pkt_tx);
    s->stat_pkt_build = 0;
    s->stat_pkt_tx = 0;
  }
  if (s->stat_tx_gso_count) {
    notice("%s(%d,%d), tx gso count %u\n", __func__, port, idx, s->stat_tx_gso_count);
    s->stat_tx_gso_count = 0;
  }
  if (s->stat_pkt_arp_fail) {
    warn("%s(%d,%d), pkt %u arp fail\n", __func__, port, idx, s->stat_pkt_arp_fail);
    s->stat_pkt_arp_fail = 0;
  }
  if (s->stat_tx_retry) {
    warn("%s(%d,%d), pkt tx retry %u\n", __func__, port, idx, s->stat_tx_retry);
    s->stat_tx_retry = 0;
  }
  if (s->user_dump) {
    s->user_dump(s->user_dump_priv);
  }
  return 0;
}

static int udp_get_sndbuf(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  mtl_memcpy(optval, &s->sndbuf_sz, sz);
  return 0;
}

static int udp_get_rcvbuf(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  mtl_memcpy(optval, &s->rcvbuf_sz, sz);
  return 0;
}

static int udp_set_sndbuf(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);
  uint32_t sndbuf_sz;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  sndbuf_sz = *((uint32_t*)optval);
  info("%s(%d), sndbuf_sz %u\n", __func__, idx, sndbuf_sz);
  s->sndbuf_sz = sndbuf_sz;
  return 0;
}

static int udp_set_rcvbuf(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);
  uint32_t rcvbuf_sz;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  rcvbuf_sz = *((uint32_t*)optval);
  info("%s(%d), rcvbuf_sz %u\n", __func__, idx, rcvbuf_sz);
  s->rcvbuf_sz = rcvbuf_sz;
  return 0;
}

static int udp_get_rcvtimeo(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  struct timeval* tv;
  size_t sz = sizeof(*tv);
  unsigned int us;

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  us = mudp_get_rx_timeout(s);
  tv = (struct timeval*)optval;
  tv->tv_sec = us / US_PER_S;
  tv->tv_usec = us % US_PER_S;
  return 0;
}

static int udp_set_cookie(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint64_t);
  uint64_t cookie;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  cookie = *((uint64_t*)optval);
  info("%s(%d), cookie %" PRIu64 "\n", __func__, idx, cookie);
  s->cookie = cookie;
  return 0;
}

static int udp_get_cookie(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint64_t);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  mtl_memcpy(optval, &s->cookie, sz);
  return 0;
}

static int udp_set_rcvtimeo(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  const struct timeval* tv;
  size_t sz = sizeof(*tv);
  unsigned int us;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  tv = (const struct timeval*)optval;
  us = tv->tv_sec * 1000 * 1000 + tv->tv_usec;
  mudp_set_rx_timeout(s, us);
  return 0;
}

static int udp_set_reuse_port(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(int);
  int reuse_port;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  reuse_port = *((int*)optval);
  info("%s(%d), reuse_port %d\n", __func__, idx, reuse_port);
  s->reuse_port = reuse_port;
  return 0;
}

static int udp_get_reuse_port(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(int);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  mtl_memcpy(optval, &s->reuse_port, sz);
  return 0;
}

static int udp_set_reuse_addr(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(int);
  int reuse_addr;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  reuse_addr = *((int*)optval);
  info("%s(%d), reuse_addr %d\n", __func__, idx, reuse_addr);
  s->reuse_addr = reuse_addr;
  return 0;
}

static int udp_get_reuse_addr(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(int);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (*optlen));
    MUDP_ERR_RET(EINVAL);
  }

  mtl_memcpy(optval, &s->reuse_addr, sz);
  return 0;
}

static int udp_init_mcast(struct mtl_main_impl* impl, struct mudp_impl* s) {
  int idx = s->idx;
  enum mtl_port port = s->port;

  if (s->mcast_addrs) {
    err("%s(%d), mcast addrs already init\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }
  s->mcast_addrs = mt_rte_zmalloc_socket(sizeof(*s->mcast_addrs) * s->mcast_addrs_nb,
                                         mt_socket_id(impl, port));
  if (!s->mcast_addrs) {
    err("%s(%d), mcast addrs malloc fail\n", __func__, idx);
    MUDP_ERR_RET(ENOMEM);
  }

  udp_set_flag(s, MUDP_MCAST_INIT);
  return 0;
}

static int udp_uinit_mcast(struct mudp_impl* s) {
  int idx = s->idx;

  if (!s->mcast_addrs) {
    dbg("%s(%d), mcast addrs not init\n", __func__, idx);
    return 0;
  }

  for (int i = 0; i < s->mcast_addrs_nb; i++) {
    if (s->mcast_addrs[i]) {
      warn("%s(%d), mcast still active on %d\n", __func__, idx, i);
      break;
    }
  }

  mt_rte_free(s->mcast_addrs);
  s->mcast_addrs = NULL;
  udp_clear_flag(s, MUDP_MCAST_INIT);
  return 0;
}

static int udp_add_membership(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  struct mtl_main_impl* impl = s->parent;
  enum mtl_port port = s->port;
  const struct ip_mreq* mreq;
  size_t sz = sizeof(*mreq);
  uint8_t* ip;
  int ret;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }

  /* init mcast if not */
  if (!udp_get_flag(s, MUDP_MCAST_INIT)) {
    ret = udp_init_mcast(impl, s);
    if (ret < 0) {
      err("%s(%d), init mcast fail\n", __func__, idx);
      return ret;
    }
  }

  mreq = (const struct ip_mreq*)optval;
  ip = (uint8_t*)&mreq->imr_multiaddr.s_addr;
  uint32_t group_addr = mt_ip_to_u32(ip);
  ret = mt_mcast_join(s->parent, group_addr, 0, port);
  if (ret < 0) {
    err("%s(%d), join mcast fail\n", __func__, idx);
    return ret;
  }

  bool added = false;
  mt_pthread_mutex_lock(&s->mcast_addrs_mutex);
  for (int i = 0; i < s->mcast_addrs_nb; i++) {
    if (!s->mcast_addrs[i]) {
      s->mcast_addrs[i] = group_addr;
      added = true;
      info("%s(%d), add %d.%d.%d.%d on %d\n", __func__, port, ip[0], ip[1], ip[2], ip[3],
           i);
      break;
    }
  }
  mt_pthread_mutex_unlock(&s->mcast_addrs_mutex);
  if (!added) {
    err("%s(%d), record mcast fail\n", __func__, idx);
    mt_mcast_leave(s->parent, group_addr, 0, port);
    MUDP_ERR_RET(EIO);
  }

  return 0;
}

static int udp_drop_membership(struct mudp_impl* s, const void* optval,
                               socklen_t optlen) {
  int idx = s->idx;
  enum mtl_port port = s->port;
  const struct ip_mreq* mreq;
  size_t sz = sizeof(*mreq);
  uint8_t* ip;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, optlen);
    MUDP_ERR_RET(EINVAL);
  }
  if (!s->mcast_addrs) {
    err("%s(%d), mcast addrs not init\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }

  mreq = (const struct ip_mreq*)optval;
  ip = (uint8_t*)&mreq->imr_multiaddr.s_addr;
  uint32_t group_addr = mt_ip_to_u32(ip);

  bool found = false;
  mt_pthread_mutex_lock(&s->mcast_addrs_mutex);
  for (int i = 0; i < s->mcast_addrs_nb; i++) {
    if (s->mcast_addrs[i] == group_addr) {
      found = true;
      s->mcast_addrs[i] = 0;
      info("%s(%d), drop %d.%d.%d.%d on %d\n", __func__, port, ip[0], ip[1], ip[2], ip[3],
           i);
      break;
    }
  }
  mt_pthread_mutex_unlock(&s->mcast_addrs_mutex);
  if (!found) {
    err("%s(%d), record mcast not found\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }

  mt_mcast_leave(s->parent, group_addr, 0, port);
  return 0;
}

static ssize_t udp_rx_dequeue(struct mudp_impl* s, void* buf, size_t len, int flags,
                              struct sockaddr* src_addr, socklen_t* addrlen) {
  int idx = s->idx;
  int ret;
  ssize_t copied = 0;
  struct rte_mbuf* pkt = NULL;
  MTL_MAY_UNUSED(flags);

  /* dequeue pkt from rx ring */
  ret = rte_ring_sc_dequeue(mur_client_ring(s->rxq), (void**)&pkt);
  if (ret < 0) return ret;
  s->stat_pkt_dequeue++;

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  const size_t hdr_len = sizeof(*hdr);
  uint32_t pkt_len = rte_pktmbuf_pkt_len(pkt);

  if (pkt_len < hdr_len) {
    err("%s(%d), invalid packet len %u < header len %zu\n", __func__, idx, pkt_len,
        hdr_len);
    rte_pktmbuf_free(pkt);
    errno = EBADMSG;
    return -1;
  }

  struct rte_udp_hdr* udp = &hdr->udp;
  ssize_t payload_len = (ssize_t)ntohs(udp->dgram_len) - sizeof(*udp);
  ssize_t payload_cap = (ssize_t)pkt_len - hdr_len;

  if (payload_len < 0 || payload_len > payload_cap) {
    err("%s(%d), invalid payload len %" PRId64 " (cap %" PRId64 ")\n", __func__, idx,
        (int64_t)payload_len, (int64_t)payload_cap);
    rte_pktmbuf_free(pkt);
    errno = EBADMSG;
    return -1;
  }

  void* payload = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  dbg("%s(%d), payload_len %" PRId64 " bytes\n", __func__, idx, (int64_t)payload_len);

  if (payload_len <= len) {
    rte_memcpy(buf, payload, payload_len);
    copied = payload_len;
    s->stat_pkt_deliver++;

    if (src_addr) { /* only AF_INET now*/
      struct sockaddr_in addr_in;
      struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;

      memset(&addr_in, 0, sizeof(addr_in));
      addr_in.sin_family = AF_INET;
      addr_in.sin_port = udp->src_port;
      addr_in.sin_addr.s_addr = ipv4->src_addr;
      dbg("%s(%d), dst port %u src port %u\n", __func__, idx, ntohs(udp->dst_port),
          ntohs(udp->src_port));
      rte_memcpy((void*)src_addr, &addr_in, RTE_MIN(*addrlen, sizeof(addr_in)));
    }
  } else {
    err("%s(%d), payload len %" PRIu64 " buf len %" PRIu64 "\n", __func__, idx,
        payload_len, len);
  }
  rte_pktmbuf_free(pkt);
  dbg("%s(%d), copied %" PRIu64 " bytes, flags %d\n", __func__, idx, copied, flags);
  return copied;
}

static ssize_t udp_rx_ret_timeout(struct mudp_impl* s, int flags) {
  MTL_MAY_UNUSED(flags);
  if (s->rx_timeout_us) {
    dbg("%s(%d), timeout to %d ms, flags %d\n", __func__, s->idx, s->rx_timeout_us,
        flags);
    MUDP_ERR_RET(ETIMEDOUT);
  } else {
    MUDP_ERR_RET(EAGAIN);
  }
}

static ssize_t udp_recvfrom(struct mudp_impl* s, void* buf, size_t len, int flags,
                            struct sockaddr* src_addr, socklen_t* addrlen) {
  struct mtl_main_impl* impl = s->parent;
  ssize_t copied = 0;
  uint16_t rx;
  uint64_t start_ts = mt_get_tsc(impl);

dequeue:
  /* dequeue pkt from rx ring */
  copied = udp_rx_dequeue(s, buf, len, flags, src_addr, addrlen);
  if (copied > 0) return copied;

  rx = mur_client_rx(s->rxq);
  if (rx) { /* dequeue again as rx succ */
    goto dequeue;
  }

  /* return EAGAIN if MSG_DONTWAIT is set */
  if (flags & MSG_DONTWAIT) {
    MUDP_ERR_RET(EAGAIN);
  }

  unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
  unsigned int timeout = s->rx_timeout_us;
  if ((us < timeout) && udp_alive(s)) {
    if (s->rx_poll_sleep_us) {
      mur_client_timedwait(s->rxq, timeout - us, s->rx_poll_sleep_us);
    }
    goto dequeue;
  }

  return udp_rx_ret_timeout(s, flags);
}

static ssize_t udp_rx_msg_dequeue(struct mudp_impl* s, struct msghdr* msg, int flags) {
  int idx = s->idx;
  int ret;
  ssize_t copied = 0;
  struct rte_mbuf* pkt = NULL;
  MTL_MAY_UNUSED(flags);

  /* dequeue pkt from rx ring */
  ret = rte_ring_sc_dequeue(mur_client_ring(s->rxq), (void**)&pkt);
  if (ret < 0) return ret;
  s->stat_pkt_dequeue++;

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  const size_t hdr_len = sizeof(*hdr);
  uint32_t pkt_len = rte_pktmbuf_pkt_len(pkt);

  if (pkt_len < hdr_len) {
    err("%s(%d), invalid packet len %u < header len %zu\n", __func__, idx, pkt_len,
        hdr_len);
    rte_pktmbuf_free(pkt);
    errno = EBADMSG;
    return -1;
  }

  struct rte_udp_hdr* udp = &hdr->udp;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  ssize_t payload_len = (ssize_t)ntohs(udp->dgram_len) - sizeof(*udp);
  ssize_t payload_cap = (ssize_t)pkt_len - hdr_len;

  if (payload_len < 0 || payload_len > payload_cap) {
    err("%s(%d), invalid payload len %" PRId64 " (cap %" PRId64 ")\n", __func__, idx,
        (int64_t)payload_len, (int64_t)payload_cap);
    rte_pktmbuf_free(pkt);
    errno = EBADMSG;
    return -1;
  }

  uint8_t* payload = rte_pktmbuf_mtod_offset(pkt, uint8_t*, hdr_len);
  dbg("%s(%d), payload_len %" PRId64 " bytes\n", __func__, idx, (int64_t)payload_len);

  msg->msg_flags = 0;

  if (msg->msg_name) { /* address */
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = udp->src_port;
    addr_in.sin_addr.s_addr = ipv4->src_addr;
    dbg("%s(%d), dst port %u src port %u\n", __func__, idx, ntohs(udp->dst_port),
        ntohs(udp->src_port));
    rte_memcpy(msg->msg_name, &addr_in, RTE_MIN(msg->msg_namelen, sizeof(addr_in)));
  }

  if (msg->msg_control) { /* Ancillary data */
    struct cmsghdr chdr;
    memset(&chdr, 0, sizeof(chdr));
    chdr.cmsg_len = sizeof(chdr);
    chdr.cmsg_level = ipv4->next_proto_id;
    rte_memcpy(msg->msg_control, &chdr, RTE_MIN(msg->msg_controllen, sizeof(chdr)));
  }

  if (msg->msg_iov) { /* Vector of data */
    ssize_t remaining = payload_len;
    for (int i = 0; i < msg->msg_iovlen; i++) {
      size_t clen = RTE_MIN(msg->msg_iov[i].iov_len, (size_t)remaining);
      rte_memcpy(msg->msg_iov[i].iov_base, payload, clen);
      remaining -= (ssize_t)clen;
      payload += clen;
      copied += clen;
      if (remaining <= 0) break;
    }
    payload_len = remaining;
    s->stat_pkt_deliver++;
  }

  if (payload_len)
    warn("%s(%d), %" PRId64 " bytes not copied \n", __func__, idx, (int64_t)payload_len);

  rte_pktmbuf_free(pkt);
  dbg("%s(%d), copied %" PRId64 " bytes, flags %d\n", __func__, idx, copied, flags);
  return copied;
}

static ssize_t udp_recvmsg(struct mudp_impl* s, struct msghdr* msg, int flags) {
  struct mtl_main_impl* impl = s->parent;
  ssize_t copied = 0;
  uint16_t rx;
  uint64_t start_ts = mt_get_tsc(impl);

  s->stat_rx_msg_cnt++;

dequeue:
  /* msg dequeue pkt from rx ring */
  copied = udp_rx_msg_dequeue(s, msg, flags);
  if (copied > 0) {
    s->stat_rx_msg_succ_cnt++;
    return copied;
  }

  rx = mur_client_rx(s->rxq);
  if (rx) { /* dequeue again as rx succ */
    goto dequeue;
  }

  /* return EAGAIN if MSG_DONTWAIT is set */
  if (flags & MSG_DONTWAIT) {
    s->stat_rx_msg_again_cnt++;
    MUDP_ERR_RET(EAGAIN);
  }

  unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
  unsigned int timeout = s->rx_timeout_us;
  if ((us < timeout) && udp_alive(s)) {
    if (s->rx_poll_sleep_us) {
      mur_client_timedwait(s->rxq, timeout - us, s->rx_poll_sleep_us);
    }
    goto dequeue;
  }

  s->stat_rx_msg_timeout_cnt++;
  return udp_rx_ret_timeout(s, flags);
}

static int udp_fallback_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  struct mudp_impl* s;
  struct pollfd p_fds[nfds];
  int ret;

  if (nfds > 0) {
    s = fds[0].fd;
    dbg("%s(%d), nfds %d timeout %d\n", __func__, s->idx, (int)nfds, timeout);
  } else {
    dbg("%s, nfds %d timeout %d\n", __func__, (int)nfds, timeout);
  }

  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;

    if (!udp_is_fallback(s)) {
      err("%s(%d), it's not a fallback fd\n", __func__, s->idx);
      return -EIO;
    }
    p_fds[i].fd = s->fallback_fd;
    p_fds[i].events = fds[i].events;
    p_fds[i].revents = fds[i].revents;
  }

#ifdef WINDOWSENV
  MTL_MAY_UNUSED(timeout);
  ret = -EIO;
  err("%s(%d), not support on this platform\n", __func__, s->idx);
#else
  ret = poll(p_fds, nfds, timeout);
#endif

  for (mudp_nfds_t i = 0; i < nfds; i++) {
    fds[i].revents = p_fds[i].revents;
  }

  return ret;
}

static int udp_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv) {
  struct mudp_impl* s = fds[0].fd;
  struct mtl_main_impl* impl = s->parent;
  uint64_t start_ts = mt_get_tsc(impl);
  int rc, ret;

  dbg("%s(%d), nfds %d timeout %d\n", __func__, s->idx, (int)nfds, timeout);
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;

    if (udp_is_fallback(s)) {
      err("%s(%d), it's backed by a fallback fd\n", __func__, s->idx);
      return -EIO;
    }

    if (!s->rxq) {
      ret = udp_init_rxq(impl, s);
      if (ret < 0) {
        err("%s(%d), init rxq fail\n", __func__, s->idx);
        return ret;
      }
    }
    s->stat_poll_cnt++;
  }

  /* rx from nic firstly if no pending pkt for each fd */
rx_poll:
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    unsigned int count = rte_ring_count(mur_client_ring(s->rxq));
    if (!count) {
      mur_client_rx(s->rxq);
    }
  }

  /* check the ready fds */
  rc = 0;
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    unsigned int count = rte_ring_count(mur_client_ring(s->rxq));
    if (count > 0) {
      rc++;
      fds[i].revents = POLLIN;
      s->stat_poll_succ_cnt++;
      dbg("%s(%d), ring count %u\n", __func__, s->idx, count);
    }
  }
  if (rc > 0) {
    dbg("%s(%d), rc %d\n", __func__, s->idx, rc);
    return rc;
  }

  if (query) { /* check if any pending event on the user query callback */
    rc = query(priv);
    if (rc != 0) {
      dbg("%s(%d), query rc %d\n", __func__, s->idx, rc);
      for (mudp_nfds_t i = 0; i < nfds; i++) {
        s = fds[i].fd;
        s->stat_poll_query_ret_cnt++;
      }
      return rc;
    }
  }

  /* check if timeout */
  int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
  if (((ms < timeout) || (timeout < 0)) && udp_alive(s)) {
    if (s->rx_poll_sleep_us) {
      mur_client_timedwait(s->rxq, (timeout - ms) * US_PER_MS, s->rx_poll_sleep_us);
    }
    goto rx_poll;
  }

  dbg("%s(%d), timeout to %d ms\n", __func__, s->idx, timeout);
  if (timeout == 0)
    s->stat_poll_zero_timeout_cnt++;
  else
    s->stat_poll_timeout_cnt++;
  return 0;
}

mudp_handle mudp_socket_port(mtl_handle mt, int domain, int type, int protocol,
                             enum mtl_port port) {
  int ret;
  struct mtl_main_impl* impl = mt;
  struct mudp_impl* s;

  static int mudp_idx = 0;
  int idx = mudp_idx;
  mudp_idx++;

  ret = mudp_verify_socket_args(domain, type, protocol);
  if (ret < 0) return NULL;

  /* make sure tsc is ready, mudp_recvfrom will use tsc */
  mt_wait_tsc_stable(impl);

  s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, port));
  if (!s) {
    err("%s(%d), s malloc fail\n", __func__, idx);
    return NULL;
  }
  s->parent = impl;
  s->type = MT_HANDLE_UDP;
  s->idx = idx;
  s->port = port;
  s->element_nb = mt_if_nb_tx_desc(impl, port) + 512;
  s->element_size = MUDP_MAX_BYTES;
  /* No dependency to arp for kernel based udp stack */
  s->arp_timeout_us = MT_TIMEOUT_ZERO;
  s->msg_arp_timeout_us = MT_TIMEOUT_ZERO;
  s->tx_timeout_us = 10 * US_PER_MS;
  s->rx_timeout_us = 0;
  s->txq_bps = MUDP_DEFAULT_RL_BPS;
  s->rx_ring_count = 1024;
  s->rx_poll_sleep_us = 10;
  s->sndbuf_sz = 10 * 1024;
  s->rcvbuf_sz = 10 * 1024;
  s->wake_thresh_count = 32;
  s->wake_timeout_us = 1000;
  s->cookie = idx;
  s->mcast_addrs_nb = 16; /* max 16 mcast address */
  s->gso_segment_sz = MUDP_MAX_BYTES;
  s->fallback_fd = -1;
  mt_pthread_mutex_init(&s->mcast_addrs_mutex, NULL);

  if (mt_pmd_is_kernel_socket(impl, port)) {
    ret = socket(domain, type, protocol);
    if (ret < 0) {
      err("%s(%d), fall back to socket fail %d\n", __func__, idx, ret);
      mudp_close(s);
      return NULL;
    }
    s->fallback_fd = ret;
    info("%s(%d), fall back to socket fd %d\n", __func__, idx, s->fallback_fd);
    goto succ;
  }

  ret = udp_init_hdr(impl, s);
  if (ret < 0) {
    err("%s(%d), hdr init fail\n", __func__, idx);
    mudp_close(s);
    return NULL;
  }

  /* todo: use random port, now hardcode to 0xAAAA plus index */
  udp_bind_port(s, 43690 + idx);

  ret = mt_stat_register(impl, udp_stat_dump, s, "udp");
  if (ret < 0) {
    err("%s(%d), hdr init fail\n", __func__, idx);
    mudp_close(s);
    return NULL;
  }

succ:
  s->alive = true;
  info("%s(%d), succ, socket %p\n", __func__, idx, s);
  return s;
}

mudp_handle mudp_socket(mtl_handle mt, int domain, int type, int protocol) {
  return mudp_socket_port(mt, domain, type, protocol, MTL_PORT_P);
}

int mudp_close(mudp_handle ut) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->alive = false;

  if (s->fallback_fd >= 0) {
    close(s->fallback_fd);
    s->fallback_fd = -1;
  }

  mt_stat_unregister(impl, udp_stat_dump, s);
  udp_stat_dump(s);

  udp_uinit_txq(impl, s);
  udp_uinit_rxq(s);
  udp_uinit_mcast(s);

  mt_pthread_mutex_destroy(&s->mcast_addrs_mutex);
  mt_rte_free(s);
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int mudp_bind(mudp_handle ut, const struct sockaddr* addr, socklen_t addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
  int ret;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  if (udp_is_fallback(s)) {
    ret = bind(s->fallback_fd, addr, addrlen);
    uint8_t* ip = (uint8_t*)&addr_in->sin_addr.s_addr;
    info("%s(%d), fallback fd %d bind ip %u.%u.%u.%u port %u ret %d\n", __func__, idx,
         s->fallback_fd, ip[0], ip[1], ip[2], ip[3], htons(addr_in->sin_port), ret);
    return ret;
  }

  ret = udp_verify_bind_addr(s, addr_in, addrlen);
  if (ret < 0) return ret;

  /* uinit rx if any */
  udp_uinit_rxq(s);

  /* set bind port */
  udp_bind_port(s, htons(addr_in->sin_port));

  ret = udp_init_rxq(impl, s);
  if (ret < 0) {
    err("%s(%d), init rxq fail\n", __func__, idx);
    return ret;
  }

  /* init txq if not */
  if (!udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    ret = udp_init_txq(impl, s, addr_in);
    if (ret < 0) {
      err("%s(%d), init txq fail\n", __func__, idx);
      return ret;
    }
  }

  udp_set_flag(s, MUDP_BIND);
  return 0;
}

ssize_t mudp_sendto(mudp_handle ut, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  int arp_timeout_ms = s->arp_timeout_us / 1000;
  int ret;

  if (udp_is_fallback(s))
    return sendto(s->fallback_fd, buf, len, flags, dest_addr, addrlen);

  const struct sockaddr_in* addr_in = (struct sockaddr_in*)dest_addr;
  ret = udp_verify_sendto_args(len, flags, addr_in, addrlen);
  if (ret < 0) {
    err("%s(%d), invalid args\n", __func__, idx);
    return ret;
  }

  /* init txq if not */
  if (!udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    ret = udp_init_txq(impl, s, addr_in);
    if (ret < 0) {
      err("%s(%d), init txq fail\n", __func__, idx);
      return ret;
    }
  }

  size_t sz_per_pkt = s->gso_segment_sz;
  unsigned int pkts_nb = len / sz_per_pkt;
  if (len % sz_per_pkt) pkts_nb++;
  struct rte_mbuf* pkts[pkts_nb];
  dbg("%s(%d), pkts_nb %u\n", __func__, idx, pkts_nb);
  if (pkts_nb > 1) s->stat_tx_gso_count++;

  ret = rte_pktmbuf_alloc_bulk(s->tx_pool, pkts, pkts_nb);
  if (ret < 0) {
    err("%s(%d), pktmbuf alloc fail, pkts_nb %u\n", __func__, idx, pkts_nb);
    MUDP_ERR_RET(ENOMEM);
  }

  size_t offset = 0;
  for (unsigned int i = 0; i < pkts_nb; i++) {
    size_t cur_len = RTE_MIN(sz_per_pkt, len - offset);
    ret = udp_build_tx_pkt(impl, s, pkts[i], buf + offset, cur_len, addr_in,
                           arp_timeout_ms);
    if (ret < 0) {
      rte_pktmbuf_free_bulk(pkts, pkts_nb);
      if (arp_timeout_ms) {
        err("%s(%d), build pkt fail %d\n", __func__, idx, ret);
        return ret;
      } else {
        mt_sleep_us(1);
        /* align to kernel behavior which sendto succ even if arp not resolved */
        return len;
      }
    }
  }

  unsigned int sent = udp_tx_pkts(impl, s, pkts, pkts_nb);
  if (sent < pkts_nb) {
    rte_pktmbuf_free_bulk(pkts + sent, pkts_nb - sent);
    if (sent) {                 /* partially send */
      return sent * sz_per_pkt; /* the size is fixed for the sent packets */
    } else {
      MUDP_ERR_RET(ETIMEDOUT);
    }
  }

  return len;
}

ssize_t mudp_sendmsg(mudp_handle ut, const struct msghdr* msg, int flags) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  int arp_timeout_ms = s->msg_arp_timeout_us / 1000;
  int ret;

#ifndef WINDOWSENV
  if (udp_is_fallback(s)) return sendmsg(s->fallback_fd, msg, flags);
#endif

  const struct sockaddr_in* addr_in = (struct sockaddr_in*)msg->msg_name;
  /* len to 1 to let the verify happy */
  ret = udp_verify_sendto_args(1, flags, addr_in, msg->msg_namelen);
  if (ret < 0) {
    err("%s(%d), invalid args\n", __func__, idx);
    return ret;
  }

  /* init txq if not */
  if (!udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    ret = udp_init_txq(impl, s, addr_in);
    if (ret < 0) {
      err("%s(%d), init txq fail\n", __func__, idx);
      return ret;
    }
  }

  udp_cmsg_handle(s, msg);

  /* UDP_SEGMENT check */
  size_t sz_per_pkt = s->gso_segment_sz;
  size_t total_len = udp_msg_len(msg);
  unsigned int pkts_nb = total_len / sz_per_pkt;
  if (total_len % sz_per_pkt) pkts_nb++;
  /* Ensure pkts_nb is greater than 0 */
  if (pkts_nb == 0) {
    err("%s(%d): pkts_nb is 0\n", __func__, idx);
    return -EINVAL; /* Invalid argument */
  }

  struct rte_mbuf* pkts[pkts_nb];
  dbg("%s(%d), pkts_nb %u total_len %" PRId64 "\n", __func__, idx, pkts_nb, total_len);
  if (pkts_nb > 1) s->stat_tx_gso_count++;

  ret = rte_pktmbuf_alloc_bulk(s->tx_pool, pkts, pkts_nb);
  if (ret < 0) {
    err("%s(%d), pktmbuf alloc fail, pkts_nb %u\n", __func__, idx, pkts_nb);
    MUDP_ERR_RET(ENOMEM);
  }

  ret = udp_build_tx_msg_pkt(impl, s, pkts, pkts_nb, msg, addr_in, arp_timeout_ms,
                             sz_per_pkt);
  if (ret < 0) {
    rte_pktmbuf_free_bulk(pkts, pkts_nb);
    if (arp_timeout_ms) {
      err("%s(%d), build pkt fail %d\n", __func__, idx, ret);
      return ret;
    } else {
      mt_sleep_us(1);
      /* align to kernel behavior which sendmsg succ even if arp not resolved */
      return total_len;
    }
  }

  unsigned int sent = udp_tx_pkts(impl, s, pkts, pkts_nb);
  if (sent < pkts_nb) {
    rte_pktmbuf_free_bulk(pkts + sent, pkts_nb - sent);
    if (sent) {                 /* partially send */
      return sent * sz_per_pkt; /* the size is fixed for the sent packets */
    } else {
      MUDP_ERR_RET(ETIMEDOUT);
    }
  }

  return total_len;
}

int mudp_poll_query(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv) {
  int ret = udp_verify_poll(fds, nfds, timeout);
  if (ret < 0) return ret;

  struct mudp_impl* s = fds[0].fd;

  if (udp_is_fallback(s)) {
    if (query) {
      err("%s(%d), query not support for fallback pth\n", __func__, s->idx);
      return -EIO;
    }
    return udp_fallback_poll(fds, nfds, timeout);
  } else {
    return udp_poll(fds, nfds, timeout, query, priv);
  }
}

int mudp_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  return mudp_poll_query(fds, nfds, timeout, NULL, NULL);
}

ssize_t mudp_recvfrom(mudp_handle ut, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  int ret;

  if (udp_is_fallback(s))
    return recvfrom(s->fallback_fd, buf, len, flags, src_addr, addrlen);

  /* init rxq if not */
  if (!s->rxq) {
    ret = udp_init_rxq(impl, s);
    if (ret < 0) {
      err("%s(%d), init rxq fail\n", __func__, idx);
      return ret;
    }
  }

  return udp_recvfrom(s, buf, len, flags, src_addr, addrlen);
}

ssize_t mudp_recvmsg(mudp_handle ut, struct msghdr* msg, int flags) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  int ret;

#ifndef WINDOWSENV
  if (udp_is_fallback(s)) return recvmsg(s->fallback_fd, msg, flags);
#endif

  /* init rxq if not */
  if (!s->rxq) {
    ret = udp_init_rxq(impl, s);
    if (ret < 0) {
      err("%s(%d), init rxq fail\n", __func__, idx);
      return ret;
    }
  }

  return udp_recvmsg(s, msg, flags);
}

int mudp_getsockopt(mudp_handle ut, int level, int optname, void* optval,
                    socklen_t* optlen) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (udp_is_fallback(s))
    return getsockopt(s->fallback_fd, level, optname, optval, optlen);

  switch (level) {
    case SOL_SOCKET: {
      switch (optname) {
        case SO_SNDBUF:
        case SO_SNDBUFFORCE:
          return udp_get_sndbuf(s, optval, optlen);
        case SO_RCVBUF:
        case SO_RCVBUFFORCE:
          return udp_get_rcvbuf(s, optval, optlen);
        case SO_RCVTIMEO:
          return udp_get_rcvtimeo(s, optval, optlen);
        case SO_COOKIE:
          return udp_get_cookie(s, optval, optlen);
        case SO_REUSEPORT:
          return udp_get_reuse_port(s, optval, optlen);
        case SO_REUSEADDR:
          return udp_get_reuse_addr(s, optval, optlen);
        default:
          err("%s(%d), unknown optname %d for SOL_SOCKET\n", __func__, idx, optname);
          MUDP_ERR_RET(EINVAL);
      }
    }
    default:
      err("%s(%d), unknown level %d\n", __func__, idx, level);
      MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

int mudp_setsockopt(mudp_handle ut, int level, int optname, const void* optval,
                    socklen_t optlen) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (udp_is_fallback(s))
    return setsockopt(s->fallback_fd, level, optname, optval, optlen);

  switch (level) {
    case SOL_SOCKET: {
      switch (optname) {
        case SO_SNDBUF:
        case SO_SNDBUFFORCE:
          return udp_set_sndbuf(s, optval, optlen);
        case SO_RCVBUF:
        case SO_RCVBUFFORCE:
          return udp_set_rcvbuf(s, optval, optlen);
        case SO_RCVTIMEO:
          return udp_set_rcvtimeo(s, optval, optlen);
        case SO_COOKIE:
          return udp_set_cookie(s, optval, optlen);
        case SO_REUSEADDR: /* skip now */
          return udp_set_reuse_addr(s, optval, optlen);
        case SO_REUSEPORT:
          return udp_set_reuse_port(s, optval, optlen);
        default:
          err("%s(%d), unknown optname %d for SOL_SOCKET\n", __func__, idx, optname);
          MUDP_ERR_RET(EINVAL);
      }
    }
    case IPPROTO_IP: {
      switch (optname) {
        case IP_ADD_MEMBERSHIP:
          return udp_add_membership(s, optval, optlen);
        case IP_DROP_MEMBERSHIP:
          return udp_drop_membership(s, optval, optlen);
        case IP_PKTINFO:
          info("%s(%d), skip IP_PKTINFO\n", __func__, idx);
          return 0;
#ifdef IP_RECVTOS
        case IP_RECVTOS:
          info("%s(%d), skip IP_RECVTOS\n", __func__, idx);
          return 0;
#endif
        case IP_MTU_DISCOVER:
          info("%s(%d), skip IP_MTU_DISCOVER\n", __func__, idx);
          return 0;
        case IP_TOS:
          dbg("%s(%d), skip IP_TOS\n", __func__, idx);
          return 0;
        default:
          err("%s(%d), unknown optname %d for IPPROTO_IP\n", __func__, idx, optname);
          MUDP_ERR_RET(EINVAL);
      }
    }
    default:
      err("%s(%d), unknown level %d\n", __func__, idx, level);
      MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

int mudp_ioctl(mudp_handle ut, unsigned long cmd, va_list args) {
  struct mudp_impl* s = ut;
  int idx = s->idx;
  MTL_MAY_UNUSED(args);

#ifndef WINDOWSENV
  if (udp_is_fallback(s)) return ioctl(s->fallback_fd, cmd, args);
#endif

  switch (cmd) {
    case FIONBIO:
      info("%s(%d), skip FIONBIO now\n", __func__, idx);
      break;
    default:
      err("%s(%d), unknown cmd %d\n", __func__, idx, (int)cmd);
      MUDP_ERR_RET(EINVAL);
  }

  return 0;
}

int mudp_set_tx_mac(mudp_handle ut, uint8_t mac[MTL_MAC_ADDR_LEN]) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  rte_memcpy(s->user_mac, mac, MTL_MAC_ADDR_LEN);
  udp_set_flag(s, MUDP_TX_USER_MAC);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n", __func__, idx, mac[0],
       mac[1], mac[2], mac[3], mac[4], mac[5]);
  return 0;
}

int mudp_bind_address_check(mudp_handle ut, bool enable) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  if (enable)
    udp_set_flag(s, MUDP_BIND_ADDRESS_CHECK);
  else
    udp_clear_flag(s, MUDP_BIND_ADDRESS_CHECK);
  return 0;
}

int mudp_set_tx_rate(mudp_handle ut, uint64_t bps) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  if (udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    err("%s(%d), txq already alloced\n", __func__, idx);
    MUDP_ERR_RET(EINVAL);
  }

  if (!bps) { /* todo: add more bps check */
    err("%s(%d), invalid bps: %" PRIu64 "\n", __func__, idx, bps);
    MUDP_ERR_RET(EINVAL);
  }

  if (bps != s->txq_bps) {
    s->txq_bps = bps;
    info("%s(%d), new bps: %" PRIu64 "\n", __func__, idx, bps);
  }

  return 0;
}

uint64_t mudp_get_tx_rate(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  return s->txq_bps;
}

int mudp_set_tx_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->tx_timeout_us = us;
  info("%s(%d), new timeout: %u us\n", __func__, idx, us);
  return 0;
}

unsigned int mudp_get_tx_timeout(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  return s->tx_timeout_us;
}

int mudp_set_rx_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->rx_timeout_us = us;
  info("%s(%d), new timeout: %u us\n", __func__, idx, us);
  return 0;
}

unsigned int mudp_get_rx_timeout(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  return s->rx_timeout_us;
}

int mudp_set_arp_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->arp_timeout_us = us;
  info("%s(%d), new timeout: %u ms\n", __func__, idx, us);
  return 0;
}

unsigned int mudp_get_arp_timeout(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  return s->arp_timeout_us;
}

int mudp_set_rx_ring_count(mudp_handle ut, unsigned int count) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  if (s->rxq) {
    err("%s(%d), rxq already alloced\n", __func__, idx);
    MUDP_ERR_RET(EINVAL);
  }

  /* add value check? */
  s->rx_ring_count = count;
  return 0;
}

int mudp_set_wake_thresh_count(mudp_handle ut, unsigned int count) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->wake_thresh_count = count;
  if (s->rxq) mur_client_set_wake_thresh(s->rxq, count);
  return 0;
}

int mudp_set_wake_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  s->wake_timeout_us = us;
  if (s->rxq) mur_client_set_wake_timeout(s->rxq, us);
  return 0;
}

int mudp_set_rx_poll_sleep(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  /* add value check? */
  s->rx_poll_sleep_us = us;
  return 0;
}

int mudp_get_sip(mudp_handle ut, uint8_t ip[MTL_IP_ADDR_LEN]) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  mtl_memcpy(ip, mt_sip_addr(s->parent, s->port), MTL_IP_ADDR_LEN);
  return 0;
}

int mudp_tx_valid_ip(mudp_handle ut, uint8_t dip[MTL_IP_ADDR_LEN]) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  struct mtl_main_impl* impl = s->parent;
  enum mtl_port port = s->port;

  if (mt_is_multicast_ip(dip)) {
    return 0;
  } else if (mt_is_lan_ip(dip, mt_sip_addr(impl, port), mt_sip_netmask(impl, port))) {
    return 0;
  } else if (mt_ip_to_u32(mt_sip_gateway(impl, port))) {
    return 0;
  }

  MUDP_ERR_RET(EINVAL);
}

int mudp_register_stat_dump_cb(mudp_handle ut, int (*dump)(void* priv), void* priv) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    MUDP_ERR_RET(EIO);
  }

  if (s->user_dump) {
    err("%s(%d), %p already registered\n", __func__, idx, s->user_dump);
    MUDP_ERR_RET(EIO);
  }

  s->user_dump = dump;
  s->user_dump_priv = priv;
  return 0;
}

bool mudp_is_multicast(const struct sockaddr_in* saddr) {
  uint8_t* ip = (uint8_t*)&saddr->sin_addr;
  bool mcast = mt_is_multicast_ip(ip);
  dbg("%s, ip %u.%u.%u.%u\n", __func__, ip[0], ip[1], ip[2], ip[3]);
  return mcast;
}
