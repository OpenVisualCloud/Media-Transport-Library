/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "udp_main.h"

#include <mudp_api.h>

#include "../mt_log.h"
#include "../mt_stat.h"

static inline void udp_set_flag(struct mudp_impl* s, uint32_t flag) { s->flags |= flag; }

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

int mudp_verify_socket_args(int domain, int type, int protocol) {
  if (domain != AF_INET) {
    dbg("%s, invalid domain %d\n", __func__, domain);
    return -EINVAL;
  }
  if (type != SOCK_DGRAM) {
    dbg("%s, invalid type %d\n", __func__, type);
    return -EINVAL;
  }
  if (protocol != 0) {
    dbg("%s, invalid protocol %d\n", __func__, protocol);
    return -EINVAL;
  }

  return 0;
}

static int udp_verify_addr(const struct sockaddr_in* addr, socklen_t addrlen) {
  if (addr->sin_family != AF_INET) {
    err("%s, invalid sa_family %d\n", __func__, addr->sin_family);
    return -EINVAL;
  }
  if (addrlen != sizeof(*addr)) {
    err("%s, invalid addrlen %d\n", __func__, (int)addrlen);
    return -EINVAL;
  }

  return 0;
}

static int udp_verify_bind_addr(struct mudp_impl* s, const struct sockaddr_in* addr,
                                socklen_t addrlen) {
  int idx = s->idx;
  int ret;

  ret = udp_verify_addr(addr, addrlen);
  if (ret < 0) return ret;

  /* check if our IP or any IP */
  if (addr->sin_addr.s_addr == INADDR_ANY)
    return 0; /* kernel mcast bind use INADDR_ANY */
  /* should we support INADDR_LOOPBACK? */
  if (memcmp(&addr->sin_addr.s_addr, mt_sip_addr(s->parent, s->port), MTL_IP_ADDR_LEN)) {
    uint8_t* ip = (uint8_t*)&addr->sin_addr.s_addr;
    err("%s(%d), invalid bind ip %u.%u.%u.%u\n", __func__, idx, ip[0], ip[1], ip[2],
        ip[3]);
    return -EINVAL;
  }

  return 0;
}

static int udp_verify_sendto_args(size_t len, int flags, const struct sockaddr_in* addr,
                                  socklen_t addrlen) {
  int ret = udp_verify_addr(addr, addrlen);
  if (ret < 0) return ret;

  if (len > MUDP_MAX_BYTES) {
    err("%s, invalid len %d\n", __func__, (int)len);
    return -EINVAL;
  }
  if (flags) {
    err("%s, invalid flags %d\n", __func__, flags);
    return -EINVAL;
  }

  return 0;
}

static int udp_verify_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  if (!fds) {
    err("%s, NULL fds\n", __func__);
    return -EINVAL;
  }
  if (nfds <= 0) {
    err("%s, invalid nfds %d\n", __func__, (int)nfds);
    return -EINVAL;
  }
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    if (!(fds[i].events & POLLIN)) {
      err("%s(%d), invalid events 0x%x\n", __func__, (int)i, fds[i].events);
      return -EINVAL;
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

  /* copy eth, ip, udp */
  rte_memcpy(hdr, &s->hdr, sizeof(*hdr));

  /* eth */
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);
  uint8_t* dip = (uint8_t*)&addr_in->sin_addr;
  if (udp_get_flag(s, MUDP_TX_USER_MAC)) {
    rte_memcpy(d_addr->addr_bytes, s->user_mac, RTE_ETHER_ADDR_LEN);
  } else {
    ret = mt_dev_dst_ip_mac(impl, dip, d_addr, port, arp_timeout_ms);
    if (ret < 0) {
      if (arp_timeout_ms) /* log only if not zero timeout */
        err("%s(%d), mt_dev_dst_ip_mac fail %d for %u.%u.%u.%u\n", __func__, idx, ret,
            dip[0], dip[1], dip[2], dip[3]);
      s->stat_pkt_arp_fail++;
      return ret;
    }
  }

  /* ip */
  ipv4->packet_id = htons(s->ipv4_packet_id);
  s->ipv4_packet_id++;
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

  /* udp */
  udp->dst_port = addr_in->sin_port;

  /* pkt mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP;
  pkt->data_len = len + sizeof(*hdr);
  pkt->pkt_len = pkt->data_len;

  /* copy payload */
  void* payload = &udp[1];
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

static int udp_bind_port(struct mudp_impl* s, uint16_t bind_port) {
  int idx = s->idx;

  /* save bind port number */
  s->bind_port = bind_port;
  /* update src port for tx also */
  s->hdr.udp.src_port = htons(bind_port);
  info("%s(%d), bind port %u\n", __func__, idx, bind_port);
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
  ret = rte_eth_macaddr_get(mt_port_id(impl, port), mt_eth_s_addr(eth));
  if (ret < 0) {
    err("%s(%d), rte_eth_macaddr_get fail %d for port %d\n", __func__, idx, ret, port);
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
    mt_dev_flush_tx_queue(impl, s->txq, mt_get_pad(impl, port));
    mt_dev_put_tx_queue(impl, s->txq);
    s->txq = NULL;
  }
  if (s->tsq) {
    /* flush all the pkts in the tx pool */
    mt_tsq_flush(impl, s->tsq, mt_get_pad(impl, port));
    mt_tsq_put(s->tsq);
    s->tsq = NULL;
  }
  if (!mt_shared_queue(impl, port)) {
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

  if (mt_shared_queue(impl, port)) {
    struct mt_tsq_flow flow;
    memset(&flow, 0, sizeof(flow));
    mtl_memcpy(&flow.dip_addr, &addr_in->sin_addr, MTL_IP_ADDR_LEN);
    flow.dst_port = ntohs(addr_in->sin_port);
    s->tsq = mt_tsq_get(impl, port, &flow);
    if (!s->tsq) {
      err("%s(%d), get tsq entry get fail\n", __func__, idx);
      udp_uinit_txq(impl, s);
      return -ENOMEM;
    }
    queue_id = mt_tsq_queue_id(s->tsq);
    mt_tsq_set_bps(impl, s->tsq, s->txq_bps / 8);
    s->tx_pool = mt_tsq_mempool(s->tsq);
  } else {
    s->txq = mt_dev_get_tx_queue(impl, port, s->txq_bps / 8);
    if (!s->txq) {
      err("%s(%d), get tx queue fail\n", __func__, idx);
      udp_uinit_txq(impl, s);
      return -EIO;
    }
    queue_id = mt_dev_tx_queue_id(s->txq);

    char pool_name[32];
    snprintf(pool_name, 32, "MUDP-TX-P%d-Q%u-%d", port, queue_id, idx);
    struct rte_mempool* pool = mt_mempool_create(impl, port, pool_name, s->element_nb,
                                                 MT_MBUF_CACHE_SIZE, 0, s->element_size);
    if (!pool) {
      err("%s(%d), mempool create fail\n", __func__, idx);
      udp_uinit_txq(impl, s);
      return -ENOMEM;
    }
    s->tx_pool = pool;
  }

  udp_set_flag(s, MUDP_TXQ_ALLOC);
  return 0;
}

static int udp_uinit_tasklet(struct mudp_impl* s) {
  if (s->lcore_tasklet) {
    mt_sch_unregister_tasklet(s->lcore_tasklet);
    s->lcore_tasklet = NULL;
  }

  return 0;
}

static int udp_uinit_rxq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  udp_uinit_tasklet(s);

  if (s->rxq) {
    mt_dev_put_rx_queue(impl, s->rxq);
    s->rxq = NULL;
  }
  if (s->rsq) {
    mt_rsq_put(s->rsq);
    s->rsq = NULL;
  }
  if (s->rss) {
    mt_rss_put(s->rss);
    s->rss = NULL;
  }

  if (s->rx_ring) {
    mt_ring_dequeue_clean(s->rx_ring);
    rte_ring_free(s->rx_ring);
    s->rx_ring = NULL;
  }

  udp_clear_flag(s, MUDP_RXQ_ALLOC);
  return 0;
}

static void udp_wakeup(struct mudp_impl* s) {
  mt_pthread_mutex_lock(&s->lcore_wake_mutex);
  mt_pthread_cond_signal(&s->lcore_wake_cond);
  mt_pthread_mutex_unlock(&s->lcore_wake_mutex);
}

static uint16_t udp_rx_handle(struct mudp_impl* s, struct rte_mbuf** pkts,
                              uint16_t nb_pkts) {
  int idx = s->idx;
  struct rte_mbuf* valid_mbuf[nb_pkts];
  uint16_t valid_mbuf_cnt = 0;
  uint16_t n = 0;

  s->stat_pkt_rx += nb_pkts;

  /* check if valid udp pkt */
  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkts[i], struct mt_udp_hdr*);
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;

    if (ipv4->next_proto_id == IPPROTO_UDP) {
      valid_mbuf[valid_mbuf_cnt] = pkts[i];
      valid_mbuf_cnt++;
      rte_mbuf_refcnt_update(pkts[i], 1);
    } else { /* invalid pkt */
      warn("%s(%d), not udp pkt %u\n", __func__, idx, ipv4->next_proto_id);
    }
  }

  /* enqueue the valid mbuf */
  if (valid_mbuf_cnt) {
    if (s->rx_ring) {
      n = rte_ring_sp_enqueue_bulk(s->rx_ring, (void**)&valid_mbuf[0], valid_mbuf_cnt,
                                   NULL);
    }
    if (!n) {
      dbg("%s(%d), %u pkts enqueue fail\n", __func__, idx, valid_mbuf_cnt);
      rte_pktmbuf_free_bulk(&valid_mbuf[0], valid_mbuf_cnt);
      s->stat_pkt_rx_enq_fail += valid_mbuf_cnt;
    }
  }

  return n;
}

static int udp_rsq_mbuf_cb(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct mudp_impl* s = priv;
  udp_rx_handle(s, mbuf, nb);
  return 0;
}

static uint16_t udp_rx(struct mtl_main_impl* impl, struct mudp_impl* s) {
  uint16_t rx_burst = s->rx_burst_pkts;
  struct rte_mbuf* pkts[rx_burst];

  if (s->rsq) return mt_rsq_burst(s->rsq, rx_burst);
  if (s->rss) return mt_rss_burst(s->rss, rx_burst);

  if (!s->rxq) return 0;
  uint16_t rx = mt_dev_rx_burst(s->rxq, pkts, rx_burst);
  if (!rx) return 0; /* no pkt */
  uint16_t n = udp_rx_handle(s, pkts, rx);
  rte_pktmbuf_free_bulk(&pkts[0], rx);
  return n;
}

static int udp_tasklet_handler(void* priv) {
  struct mudp_impl* s = priv;
  struct mtl_main_impl* impl = s->parent;

  udp_rx(impl, s);

  unsigned int count = rte_ring_count(s->rx_ring);
  if (count > 0) {
    uint64_t tsc = mt_get_tsc(impl);
    int us = (tsc - s->wake_tsc_last) / NS_PER_US;
    if ((count > s->wake_thresh_count) || (us > s->wake_timeout_us)) {
      udp_wakeup(s);
      s->wake_tsc_last = tsc;
    }
  }
  return 0;
}

static int udp_init_tasklet(struct mtl_main_impl* impl, struct mudp_impl* s) {
  if (!mt_udp_lcore(impl, s->port)) return 0;

  struct mt_sch_tasklet_ops ops;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = s;
  ops.name = s->name;
  ops.handler = udp_tasklet_handler;

  s->lcore_tasklet = mt_sch_register_tasklet(impl->main_sch, &ops);
  if (!s->lcore_tasklet) {
    err("%s, register lcore tasklet fail\n", __func__);
    mudp_close(s);
    return -EIO;
  }
  /* start mtl to start the sch */
  mtl_start(impl);
  return 0;
}

static int udp_init_rxq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  enum mtl_port port = s->port;
  int idx = s->idx;
  uint16_t queue_id;

  struct mt_rx_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.no_ip_flow = true;
  flow.dst_port = s->bind_port;
  flow.priv = s;
  flow.cb = udp_rsq_mbuf_cb;

  if (mt_has_rss(impl, port)) {
    s->rss = mt_rss_get(impl, port, &flow);
    if (!s->rss) {
      err("%s(%d), get rss fail\n", __func__, idx);
      udp_uinit_rxq(impl, s);
      return -EIO;
    }
    queue_id = mt_rss_queue_id(s->rss);
  } else if (mt_shared_queue(impl, port)) {
    s->rsq = mt_rsq_get(impl, port, &flow);
    if (!s->rsq) {
      err("%s(%d), get rsq fail\n", __func__, idx);
      udp_uinit_rxq(impl, s);
      return -EIO;
    }
    queue_id = mt_rsq_queue_id(s->rsq);
  } else {
    s->rxq = mt_dev_get_rx_queue(impl, port, &flow);
    if (!s->rxq) {
      err("%s(%d), get rx queue fail\n", __func__, idx);
      udp_uinit_rxq(impl, s);
      return -EIO;
    }
    queue_id = mt_dev_rx_queue_id(s->rxq);
  }
  s->rxq_id = queue_id;

  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, 32, "MUDP-RX-P%d-Q%u-%d", port, queue_id, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->rx_ring_count;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d), rx ring create fail\n", __func__, idx);
    udp_uinit_rxq(impl, s);
    return -ENOMEM;
  }
  s->rx_ring = ring;

  int ret = udp_init_tasklet(impl, s);
  if (ret < 0) {
    err("%s(%d), init tasklet fail %d\n", __func__, idx, ret);
    udp_uinit_rxq(impl, s);
    return -ENOMEM;
  }

  info("%s(%d), succ, port %u\n", __func__, idx, s->bind_port);
  udp_set_flag(s, MUDP_RXQ_ALLOC);
  return 0;
}

static char* udp_rxq_mode(struct mudp_impl* s) {
  if (s->rsq) return "shared";
  if (s->rss) return "rss";
  return "dedicated";
}

static int udp_stat_dump(void* priv) {
  struct mudp_impl* s = priv;
  int idx = s->idx;
  enum mtl_port port = s->port;

  if (s->stat_pkt_build) {
    notice("%s(%d,%d), pkt build %u tx %u\n", __func__, port, idx, s->stat_pkt_build,
           s->stat_pkt_tx);
    s->stat_pkt_build = 0;
    s->stat_pkt_tx = 0;
  }
  if (s->stat_pkt_rx) {
    notice("%s(%d,%d), pkt rx %u deliver %u, %s rxq %u\n", __func__, port, idx,
           s->stat_pkt_rx, s->stat_pkt_deliver, udp_rxq_mode(s), s->rxq_id);
    s->stat_pkt_rx = 0;
    s->stat_pkt_deliver = 0;
  }
  if (s->stat_pkt_rx_enq_fail) {
    warn("%s(%d,%d), pkt rx %u enqueue fail\n", __func__, port, idx,
         s->stat_pkt_rx_enq_fail);
    s->stat_pkt_rx_enq_fail = 0;
  }
  if (s->stat_pkt_arp_fail) {
    warn("%s(%d,%d), pkt %u arp fail\n", __func__, port, idx, s->stat_pkt_arp_fail);
    s->stat_pkt_arp_fail = 0;
  }
  if (s->stat_tx_retry) {
    warn("%s(%d,%d), pkt tx retry %u\n", __func__, port, idx, s->stat_tx_retry);
    s->stat_tx_retry = 0;
  }
  if (s->stat_timedwait) {
    notice("%s(%d,%d), timedwait %u timeout %u\n", __func__, port, idx, s->stat_timedwait,
           s->stat_timedwait_timeout);
    s->stat_timedwait = 0;
    s->stat_timedwait_timeout = 0;
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
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (int)(*optlen));
    return -EINVAL;
  }

  mtl_memcpy(optval, &s->sndbuf_sz, sz);
  return 0;
}

static int udp_get_rcvbuf(struct mudp_impl* s, void* optval, socklen_t* optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);

  if (*optlen != sz) {
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (int)(*optlen));
    return -EINVAL;
  }

  mtl_memcpy(optval, &s->rcvbuf_sz, sz);
  return 0;
}

static int udp_set_sndbuf(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  size_t sz = sizeof(uint32_t);
  uint32_t sndbuf_sz;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, (int)optlen);
    return -EINVAL;
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
    err("%s(%d), invalid optlen %d\n", __func__, idx, (int)optlen);
    return -EINVAL;
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
    err("%s(%d), invalid *optlen %d\n", __func__, idx, (int)(*optlen));
    return -EINVAL;
  }

  us = mudp_get_rx_timeout(s);
  tv = (struct timeval*)optval;
  tv->tv_sec = us / US_PER_S;
  tv->tv_usec = us % US_PER_S;
  return 0;
}

static int udp_set_rcvtimeo(struct mudp_impl* s, const void* optval, socklen_t optlen) {
  int idx = s->idx;
  const struct timeval* tv;
  size_t sz = sizeof(*tv);
  unsigned int us;

  if (optlen != sz) {
    err("%s(%d), invalid optlen %d\n", __func__, idx, (int)optlen);
    return -EINVAL;
  }

  tv = (const struct timeval*)optval;
  us = tv->tv_sec * 1000 * 1000 + tv->tv_usec;
  mudp_set_rx_timeout(s, us);
  return 0;
}

static int udp_init_mcast(struct mtl_main_impl* impl, struct mudp_impl* s) {
  int idx = s->idx;
  enum mtl_port port = s->port;

  if (s->mcast_addrs) {
    err("%s(%d), mcast addrs already init\n", __func__, idx);
    return -EIO;
  }
  s->mcast_addrs = mt_rte_zmalloc_socket(sizeof(*s->mcast_addrs) * s->mcast_addrs_nb,
                                         mt_socket_id(impl, port));
  if (!s->mcast_addrs) {
    err("%s(%d), mcast addrs malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  udp_set_flag(s, MUDP_MCAST_INIT);
  return 0;
}

static int udp_uinit_mcast(struct mtl_main_impl* impl, struct mudp_impl* s) {
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
    err("%s(%d), invalid optlen %d\n", __func__, idx, (int)optlen);
    return -EINVAL;
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
  ret = mt_mcast_join(s->parent, group_addr, port);
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
    mt_mcast_leave(s->parent, group_addr, port);
    return -EIO;
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
    err("%s(%d), invalid optlen %d\n", __func__, idx, (int)optlen);
    return -EINVAL;
  }
  if (!s->mcast_addrs) {
    err("%s(%d), mcast addrs not init\n", __func__, idx);
    return -EIO;
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
    return -EIO;
  }

  mt_mcast_leave(s->parent, group_addr, port);
  return 0;
}

static ssize_t udp_rx_dequeue(struct mudp_impl* s, void* buf, size_t len, int flags,
                              struct sockaddr* src_addr, socklen_t* addrlen) {
  int idx = s->idx;
  int ret;
  ssize_t copied = 0;
  struct rte_mbuf* pkt = NULL;

  /* dequeue pkt from rx ring */
  ret = rte_ring_sc_dequeue(s->rx_ring, (void**)&pkt);
  if (ret < 0) return ret;

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct rte_udp_hdr* udp = &hdr->udp;
  void* payload = &udp[1];
  ssize_t payload_len = ntohs(udp->dgram_len) - sizeof(*udp);
  dbg("%s(%d), payload_len %d bytes\n", __func__, idx, (int)payload_len);

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
      rte_memcpy((void*)src_addr, &addr_in, *addrlen);
    }
  } else {
    err("%s(%d), payload len %d buf len %d\n", __func__, idx, (int)payload_len, (int)len);
  }
  rte_pktmbuf_free(pkt);
  dbg("%s(%d), copied %d bytes, flags %d\n", __func__, idx, (int)copied, flags);
  return copied;
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

rx_pool:
  rx = udp_rx(impl, s);
  if (rx) { /* dequeue again as rx succ */
    goto dequeue;
  }

  /* return EAGAIN if MSG_DONTWAIT is set */
  if (flags & MSG_DONTWAIT) {
    errno = EAGAIN;
    return -EAGAIN;
  }

  unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
  if ((us < s->rx_timeout_us) && udp_alive(s)) {
    if (s->rx_poll_sleep_us) mt_sleep_us(s->rx_poll_sleep_us);
    goto rx_pool;
  }

  dbg("%s(%d), timeout to %d ms, flags %d\n", __func__, s->idx, s->rx_timeout_us, flags);
  return -ETIMEDOUT;
}

static int udp_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  struct mudp_impl* s = fds[0].fd;
  struct mtl_main_impl* impl = s->parent;
  uint64_t start_ts = mt_get_tsc(impl);
  int rc;

  dbg("%s(%d), nfds %d\n", __func__, s->idx, (int)nfds);

  /* rx from nic firstly if no pending pkt for each fd */
rx_poll:
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    unsigned int count = rte_ring_count(s->rx_ring);
    if (!count) udp_rx(impl, s);
  }

  /* check the ready fds */
  rc = 0;
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    unsigned int count = rte_ring_count(s->rx_ring);
    if (count > 0) {
      rc++;
      fds[i].revents = POLLIN;
      dbg("%s(%d), ring count %u\n", __func__, s->idx, count);
    }
  }
  if (rc > 0) return rc;

  /* check if timeout */
  int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
  if ((ms < timeout) && udp_alive(s)) {
    if (s->rx_poll_sleep_us) mt_sleep_us(s->rx_poll_sleep_us);
    goto rx_poll;
  }

  dbg("%s, timeout to %d ms\n", __func__, timeout);
  return 0;
}

static void udp_timedwait_lcore(struct mudp_impl* s, unsigned int us) {
  mt_pthread_mutex_lock(&s->lcore_wake_mutex);
  if (udp_alive(s)) {
    struct timespec time;
    clock_gettime(MT_THREAD_TIMEDWAIT_CLOCK_ID, &time);
    uint64_t ns = mt_timespec_to_ns(&time);
    ns += us * NS_PER_US;
    mt_ns_to_timespec(ns, &time);
    s->stat_timedwait++;
    int ret = mt_pthread_cond_timedwait(&s->lcore_wake_cond, &s->lcore_wake_mutex, &time);
    dbg("%s(%d), timedwait ret %d\n", __func__, s->idx, ret);
    if (ret == ETIMEDOUT) s->stat_timedwait_timeout++;
  }
  mt_pthread_mutex_unlock(&s->lcore_wake_mutex);
}

static ssize_t udp_recvfrom_lcore(struct mudp_impl* s, void* buf, size_t len, int flags,
                                  struct sockaddr* src_addr, socklen_t* addrlen) {
  struct mtl_main_impl* impl = s->parent;
  ssize_t copied = 0;
  uint64_t start_ts = mt_get_tsc(impl);

dequeue:
  /* dequeue pkt from rx ring */
  copied = udp_rx_dequeue(s, buf, len, flags, src_addr, addrlen);
  if (copied > 0) return copied;

  /* return EAGAIN if MSG_DONTWAIT is set */
  if (flags & MSG_DONTWAIT) {
    errno = EAGAIN;
    return -EAGAIN;
  }

  unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
  unsigned int timeout = s->rx_timeout_us;
  if (us < timeout) {
    udp_timedwait_lcore(s, timeout - us);
    goto dequeue;
  }

  dbg("%s(%d), timeout to %d ms, flags %d\n", __func__, s->idx, s->rx_timeout_ms, flags);
  return -ETIMEDOUT;
}

static int udp_poll_lcore(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  struct mudp_impl* s = fds[0].fd;
  struct mtl_main_impl* impl = s->parent;
  uint64_t start_ts = mt_get_tsc(impl);
  int rc;

  /* check if no pending pkt for each fd */
poll:
  /* check the ready fds */
  rc = 0;
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    unsigned int count = rte_ring_count(s->rx_ring);
    if (count > 0) {
      rc++;
      fds[i].revents = POLLIN;
      dbg("%s(%d), ring count %u\n", __func__, s->idx, count);
    }
  }
  if (rc > 0) return rc;

  /* check if timeout */
  int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
  if (ms < timeout) {
    udp_timedwait_lcore(s, timeout - ms);
    goto poll; /* check poll again */
  }

  dbg("%s, timeout to %d ms\n", __func__, timeout);
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
  snprintf(s->name, sizeof(s->name), "mudp_%d", idx);
  s->port = port;
  s->element_nb = mt_if_nb_tx_desc(impl, port) + 512;
  s->element_size = MUDP_MAX_BYTES;
  /* No dependency to arp for kernel based udp stack */
  s->arp_timeout_us = MT_DEV_TIMEOUT_ZERO;
  s->tx_timeout_us = 10 * US_PER_MS;
  s->rx_timeout_us = US_PER_S;
  s->txq_bps = MUDP_DEFAULT_RL_BPS;
  s->rx_burst_pkts = 128;
  s->rx_ring_count = 1024;
  s->rx_poll_sleep_us = 10;
  s->sndbuf_sz = 10 * 1024;
  s->rcvbuf_sz = 10 * 1024;
  s->mcast_addrs_nb = 16; /* max 16 mcast address */
  mt_pthread_mutex_init(&s->mcast_addrs_mutex, NULL);

  /* lcore related */
  mt_pthread_mutex_init(&s->lcore_wake_mutex, NULL);
#if MT_THREAD_TIMEDWAIT_CLOCK_ID != CLOCK_REALTIME
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, MT_THREAD_TIMEDWAIT_CLOCK_ID);
  mt_pthread_cond_init(&s->lcore_wake_cond, &attr);
#else
  mt_pthread_cond_init(&s->lcore_wake_cond, NULL);
#endif
  s->wake_thresh_count = 32;
  s->wake_timeout_us = 1000;
  s->wake_tsc_last = mt_get_tsc(impl);

  ret = udp_init_hdr(impl, s);
  if (ret < 0) {
    err("%s(%d), hdr init fail\n", __func__, idx);
    mudp_close(s);
    return NULL;
  }

  /* todo: use random port, now hardcode to 0xAAAA plus index */
  udp_bind_port(s, 43690 + idx);

  ret = mt_stat_register(impl, udp_stat_dump, s);
  if (ret < 0) {
    err("%s(%d), hdr init fail\n", __func__, idx);
    mudp_close(s);
    return NULL;
  }

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
    return -EIO;
  }

  s->alive = false;
  udp_wakeup(s); /* wake up any pending wait */

  mt_stat_unregister(impl, udp_stat_dump, s);
  udp_stat_dump(s);

  udp_uinit_txq(impl, s);
  udp_uinit_rxq(impl, s);
  udp_uinit_mcast(impl, s);

  mt_pthread_mutex_destroy(&s->mcast_addrs_mutex);
  mt_pthread_mutex_destroy(&s->lcore_wake_mutex);
  mt_pthread_cond_destroy(&s->lcore_wake_cond);

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
    return -EIO;
  }

  ret = udp_verify_bind_addr(s, addr_in, addrlen);
  if (ret < 0) return ret;

  /* uinit rx if any */
  udp_uinit_rxq(impl, s);

  /* set bind port */
  udp_bind_port(s, htons(addr_in->sin_port));

  ret = udp_init_rxq(impl, s);
  if (ret < 0) {
    err("%s(%d), init rxq fail\n", __func__, idx);
    return ret;
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
  struct rte_mbuf* m;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

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

  m = rte_pktmbuf_alloc(s->tx_pool);
  if (!m) {
    err("%s(%d), pktmbuf alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  ret = udp_build_tx_pkt(impl, s, m, buf, len, addr_in, arp_timeout_ms);
  if (ret < 0) {
    rte_pktmbuf_free(m);
    if (arp_timeout_ms) {
      err("%s(%d), build pkt fail %d\n", __func__, idx, ret);
      return ret;
    } else {
      mt_sleep_us(1);
      /* align to kernel behavior which sendto succ even if arp not resolved */
      return len;
    }
  }

  uint64_t start_ts = mt_get_tsc(impl);
  while (1) {
    uint16_t sent;

    if (s->tsq)
      sent = mt_tsq_burst(s->tsq, &m, 1);
    else
      sent = mt_dev_tx_burst(s->txq, &m, 1);
    if (sent >= 1) { /* burst succ */
      s->stat_pkt_tx++;
      break;
    }

    /* check timeout */
    unsigned int us = (mt_get_tsc(impl) - start_ts) / NS_PER_US;
    if (us > s->tx_timeout_us) {
      warn("%s(%d), fail as timeout %u us\n", __func__, idx, s->tx_timeout_us);
      rte_pktmbuf_free(m);
      return -ETIMEDOUT;
    }
    s->stat_tx_retry++;
    mt_sleep_us(1);
  }

  return len;
}

int mudp_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout) {
  int ret = udp_verify_poll(fds, nfds, timeout);
  if (ret < 0) return ret;

  struct mudp_impl* s = fds[0].fd;
  struct mtl_main_impl* impl = s->parent;

  /* init rxq if not */
  for (mudp_nfds_t i = 0; i < nfds; i++) {
    s = fds[i].fd;
    if (!udp_get_flag(s, MUDP_RXQ_ALLOC)) {
      ret = udp_init_rxq(impl, s);
      if (ret < 0) {
        err("%s(%d), init rxq fail\n", __func__, s->idx);
        return ret;
      }
    }
  }

  if (s->lcore_tasklet)
    return udp_poll_lcore(fds, nfds, timeout);
  else
    return udp_poll(fds, nfds, timeout);
}

ssize_t mudp_recvfrom(mudp_handle ut, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parent;
  int idx = s->idx;
  int ret;

  /* init rxq if not */
  if (!udp_get_flag(s, MUDP_RXQ_ALLOC)) {
    ret = udp_init_rxq(impl, s);
    if (ret < 0) {
      err("%s(%d), init rxq fail\n", __func__, idx);
      return ret;
    }
  }

  if (s->lcore_tasklet)
    return udp_recvfrom_lcore(s, buf, len, flags, src_addr, addrlen);
  else
    return udp_recvfrom(s, buf, len, flags, src_addr, addrlen);
}

int mudp_getsockopt(mudp_handle ut, int level, int optname, void* optval,
                    socklen_t* optlen) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  switch (level) {
    case SOL_SOCKET: {
      switch (optname) {
        case SO_SNDBUF:
#ifdef SO_SNDBUFFORCE
        case SO_SNDBUFFORCE:
#endif
          return udp_get_sndbuf(s, optval, optlen);
        case SO_RCVBUF:
#ifdef SO_RCVBUFFORCE
        case SO_RCVBUFFORCE:
#endif
          return udp_get_rcvbuf(s, optval, optlen);
        case SO_RCVTIMEO:
          return udp_get_rcvtimeo(s, optval, optlen);
        default:
          err("%s(%d), unknown optname %d for SOL_SOCKET\n", __func__, idx, optname);
          return -EINVAL;
      }
    }
    default:
      err("%s(%d), unknown level %d\n", __func__, idx, level);
      return -EINVAL;
  }

  return 0;
}

int mudp_setsockopt(mudp_handle ut, int level, int optname, const void* optval,
                    socklen_t optlen) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  switch (level) {
    case SOL_SOCKET: {
      switch (optname) {
        case SO_SNDBUF:
#ifdef SO_SNDBUFFORCE
        case SO_SNDBUFFORCE:
#endif
          return udp_set_sndbuf(s, optval, optlen);
        case SO_RCVBUF:
#ifdef SO_RCVBUFFORCE
        case SO_RCVBUFFORCE:
#endif
          return udp_set_rcvbuf(s, optval, optlen);
        case SO_RCVTIMEO:
          return udp_set_rcvtimeo(s, optval, optlen);
        case SO_REUSEADDR: /* skip now */
          info("%s(%d), skip SO_REUSEADDR\n", __func__, idx);
          return 0;
        default:
          err("%s(%d), unknown optname %d for SOL_SOCKET\n", __func__, idx, optname);
          return -EINVAL;
      }
    }
    case IPPROTO_IP: {
      switch (optname) {
        case IP_ADD_MEMBERSHIP:
          return udp_add_membership(s, optval, optlen);
        case IP_DROP_MEMBERSHIP:
          return udp_drop_membership(s, optval, optlen);
        default:
          err("%s(%d), unknown optname %d for IPPROTO_IP\n", __func__, idx, optname);
          return -EINVAL;
      }
    }
    default:
      err("%s(%d), unknown level %d\n", __func__, idx, level);
      return -EINVAL;
  }

  return 0;
}

int mudp_set_tx_mac(mudp_handle ut, uint8_t mac[MTL_MAC_ADDR_LEN]) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  rte_memcpy(s->user_mac, mac, MTL_MAC_ADDR_LEN);
  udp_set_flag(s, MUDP_TX_USER_MAC);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n", __func__, idx, mac[0],
       mac[1], mac[2], mac[3], mac[4], mac[5]);
  return 0;
}

int mudp_set_tx_rate(mudp_handle ut, uint64_t bps) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  if (udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    err("%s(%d), txq already alloced\n", __func__, idx);
    return -EINVAL;
  }

  if (!bps) { /* todo: add more bps check */
    err("%s(%d), invalid bps: %" PRIu64 "\n", __func__, idx, bps);
    return -EINVAL;
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
    return -EIO;
  }

  return s->txq_bps;
}

int mudp_set_tx_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
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
    return -EIO;
  }

  return s->tx_timeout_us;
}

int mudp_set_rx_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
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
    return -EIO;
  }

  return s->rx_timeout_us;
}

int mudp_set_arp_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
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
    return -EIO;
  }

  return s->arp_timeout_us;
}

int mudp_set_rx_ring_count(mudp_handle ut, unsigned int count) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  if (udp_get_flag(s, MUDP_RXQ_ALLOC)) {
    err("%s(%d), rxq already alloced\n", __func__, idx);
    return -EINVAL;
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
    return -EIO;
  }

  /* add value check? */
  s->wake_thresh_count = count;
  return 0;
}

int mudp_set_wake_timeout(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  /* add value check? */
  s->wake_timeout_us = us;
  return 0;
}

int mudp_set_rx_poll_sleep(mudp_handle ut, unsigned int us) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
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
    return -EIO;
  }

  mtl_memcpy(ip, mt_sip_addr(s->parent, s->port), MTL_IP_ADDR_LEN);
  return 0;
}

int mudp_tx_valid_ip(mudp_handle ut, uint8_t dip[MTL_IP_ADDR_LEN]) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
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

  return -EINVAL;
}

int mudp_register_stat_dump_cb(mudp_handle ut, int (*dump)(void* priv), void* priv) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  if (s->user_dump) {
    err("%s(%d), %p already registered\n", __func__, idx, s->user_dump);
    return -EIO;
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
