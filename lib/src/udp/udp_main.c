/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "udp_main.h"

#include <mudp_api.h>

#include "../mt_log.h"

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

static int udp_verfiy_socket_args(int domain, int type, int protocol) {
  if (domain != AF_INET) {
    err("%s, invalid domain %d\n", __func__, domain);
    return -EINVAL;
  }
  if (type != SOCK_DGRAM) {
    err("%s, invalid type %d\n", __func__, type);
    return -EINVAL;
  }
  if (protocol != 0) {
    err("%s, invalid protocol %d\n", __func__, protocol);
    return -EINVAL;
  }

  return 0;
}

static int udp_verfiy_addr(const struct sockaddr_in* addr, socklen_t addrlen) {
  if (addr->sin_family != AF_INET) {
    err("%s, invalid sa_family %d\n", __func__, addr->sin_family);
    return -1;
  }
  if (addrlen != sizeof(*addr)) {
    err("%s, invalid addrlen %d\n", __func__, (int)addrlen);
    return -1;
  }

  return 0;
}

static int udp_verfiy_sendto_args(size_t len, int flags, const struct sockaddr_in* addr,
                                  socklen_t addrlen) {
  int ret = udp_verfiy_addr(addr, addrlen);
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

static int udp_build_tx_pkt(struct mtl_main_impl* impl, struct mudp_impl* s,
                            struct rte_mbuf* pkt, const void* buf, size_t len,
                            const struct sockaddr_in* addr_in) {
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
  ret = mt_dev_dst_ip_mac(impl, dip, d_addr, port, s->arp_timeout_ms);
  if (ret < 0) {
    err("%s(%d), mt_dev_dst_ip_mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0],
        dip[1], dip[2], dip[3]);
    return ret;
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
  void* payload = (void*)(udp + 1);
  mtl_memcpy(payload, buf, len);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!mt_if_has_offload_ipv4_cksum(impl, port)) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

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
  if (s->txq) {
    /* flush all the pkts in the tx ring desc */
    mt_dev_flush_tx_queue(impl, s->txq, mt_get_pad(impl, s->port));
    mt_dev_put_tx_queue(impl, s->txq);
    s->txq = NULL;
  }
  if (s->tx_pool) {
    mt_mempool_free(s->tx_pool);
    s->tx_pool = NULL;
  }

  udp_clear_flag(s, MUDP_TXQ_ALLOC);
  return 0;
}

static int udp_init_txq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  enum mtl_port port = s->port;
  int idx = s->idx;

  /* queue, alloc rxq in the bind */
  s->txq = mt_dev_get_tx_queue(impl, port, s->txq_bps / 8);
  if (!s->txq) {
    err("%s(%d), get tx queue fail\n", __func__, idx);
    udp_uinit_txq(impl, s);
    return -EIO;
  }

  char pool_name[32];
  snprintf(pool_name, 32, "MUDP-TX-P%d-Q%u", port, mt_dev_tx_queue_id(s->txq));
  struct rte_mempool* pool = mt_mempool_create(impl, port, pool_name, s->element_nb,
                                               MT_MBUF_CACHE_SIZE, 0, s->element_size);
  if (!pool) {
    err("%s(%d), mempool create fail\n", __func__, idx);
    udp_uinit_txq(impl, s);
    return -ENOMEM;
  }
  s->tx_pool = pool;

  udp_set_flag(s, MUDP_TXQ_ALLOC);
  return 0;
}

static int udp_uinit_rxq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  if (udp_get_flag(s, MUDP_RX_MCAST_JOINED)) {
    struct sockaddr_in* addr_in = &s->bind_addr;
    uint8_t* ip = (uint8_t*)&addr_in->sin_addr;
    mt_mcast_leave(impl, mt_ip_to_u32(ip), s->port);
    udp_clear_flag(s, MUDP_RX_MCAST_JOINED);
  }

  if (s->rxq) {
    mt_dev_put_rx_queue(impl, s->rxq);
    s->rxq = NULL;
  }

  if (s->rx_ring) {
    mt_ring_dequeue_clean(s->rx_ring);
    rte_ring_free(s->rx_ring);
    s->rx_ring = NULL;
  }

  udp_clear_flag(s, MUDP_RXQ_ALLOC);
  return 0;
}

static int udp_init_rxq(struct mtl_main_impl* impl, struct mudp_impl* s) {
  enum mtl_port port = s->port;
  int idx = s->idx;
  struct sockaddr_in* addr_in = &s->bind_addr;
  uint8_t* ip = (uint8_t*)&addr_in->sin_addr;

  struct mt_rx_flow flow;
  memset(&flow, 0, sizeof(flow));
  rte_memcpy(flow.dip_addr, ip, MTL_IP_ADDR_LEN);
  rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
  flow.port_flow = true;
  flow.dst_port = ntohs(addr_in->sin_port);
  s->rxq = mt_dev_get_rx_queue(impl, port, &flow);
  if (!s->rxq) {
    err("%s(%d), get rx queue fail\n", __func__, idx);
    udp_uinit_rxq(impl, s);
    return -EIO;
  }

  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, 32, "MUDP-RX-P%d-Q%u", port, mt_dev_rx_queue_id(s->rxq));
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->rx_burst_pkts * 2;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d), rx ring create fail\n", __func__, idx);
    udp_uinit_rxq(impl, s);
    return -ENOMEM;
  }
  s->rx_ring = ring;

  if (mt_is_multicast_ip(ip)) {
    int ret = mt_mcast_join(impl, mt_ip_to_u32(ip), port);
    if (ret < 0) {
      err("%s(%d), mcast join fail\n", __func__, idx);
      udp_uinit_rxq(impl, s);
      return ret;
    }
    udp_set_flag(s, MUDP_RX_MCAST_JOINED);
  }

  info("%s(%d), succ, ip %u.%u.%u.%u port %u\n", __func__, idx, ip[0], ip[1], ip[2],
       ip[3], flow.dst_port);
  udp_set_flag(s, MUDP_RXQ_ALLOC);
  return 0;
}

static uint16_t udp_rx(struct mtl_main_impl* impl, struct mudp_impl* s) {
  int idx = s->idx;
  uint16_t rx_burst = s->rx_burst_pkts;
  struct rte_mbuf* pkt[rx_burst];
  uint16_t rx = mt_dev_rx_burst(s->rxq, pkt, rx_burst);
  uint16_t n = 0;

  if (!rx) return 0; /* no pkt */

  struct rte_mbuf* free_mbuf[rx];
  uint16_t free_mbuf_cnt = 0;
  struct rte_mbuf* valid_mbuf[rx];
  uint16_t valid_mbuf_cnt = 0;

  /* check if valid udp pkt */
  for (uint16_t i = 0; i < rx; i++) {
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt[i], struct mt_udp_hdr*);
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;

    if (ipv4->next_proto_id == IPPROTO_UDP) {
      valid_mbuf[valid_mbuf_cnt] = pkt[i];
      valid_mbuf_cnt++;
    } else { /* invalid pkt, free directly */
      warn("%s(%d), not udp pkt %u\n", __func__, idx, ipv4->next_proto_id);
      free_mbuf[free_mbuf_cnt] = pkt[i];
      free_mbuf_cnt++;
    }
  }

  /* enqueue the valid mbuf */
  if (valid_mbuf_cnt) {
    n = rte_ring_sp_enqueue_bulk(s->rx_ring, (void**)&valid_mbuf[0], valid_mbuf_cnt,
                                 NULL);
    if (!n) {
      warn("%s(%d), %u pkts enqueue fail\n", __func__, idx, valid_mbuf_cnt);
      rte_pktmbuf_free_bulk(&valid_mbuf[0], valid_mbuf_cnt);
    }
  }
  /* free the invalid mbuf */
  if (free_mbuf_cnt) {
    rte_pktmbuf_free_bulk(&free_mbuf[0], free_mbuf_cnt);
  }

  return n;
}

mudp_handle mudp_socket(mtl_handle mt, int domain, int type, int protocol) {
  int ret;
  struct mtl_main_impl* impl = mt;
  struct mudp_impl* s;
  enum mtl_port port = MTL_PORT_P;

  static int mudp_idx = 0;
  int idx = mudp_idx;
  mudp_idx++;

  ret = udp_verfiy_socket_args(domain, type, protocol);
  if (ret < 0) return NULL;

  /* make sure tsc is ready, mudp_recvfrom will use tsc */
  mt_wait_tsc_stable(impl);

  s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, port));
  if (!s) {
    err("%s(%d), s malloc fail\n", __func__, idx);
    return NULL;
  }
  s->parnet = impl;
  s->type = MT_HANDLE_UDP;
  s->idx = idx;
  s->port = port;
  s->element_nb = mt_if_nb_tx_desc(impl, port) + 512;
  s->element_size = MUDP_MAX_BYTES;
  s->arp_timeout_ms = 0;
  s->tx_timeout_ms = 10;
  s->rx_timeout_ms = 10;
  s->txq_bps = MUDP_DEFAULT_RL_BPS;
  s->rx_burst_pkts = 128;
  s->rx_ring_thresh = s->rx_burst_pkts / 2;

  ret = udp_init_hdr(impl, s);
  if (ret < 0) {
    err("%s(%d), hdr init fail\n", __func__, idx);
    mudp_close(s);
    return NULL;
  }

  info("%s(%d), succ\n", __func__, idx);
  return s;
}

int mudp_close(mudp_handle ut) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parnet;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  udp_uinit_txq(impl, s);
  udp_uinit_rxq(impl, s);

  mt_rte_free(s);
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int mudp_bind(mudp_handle ut, const struct sockaddr* addr, socklen_t addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parnet;
  int idx = s->idx;
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
  int ret;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  ret = udp_verfiy_addr(addr_in, addrlen);
  if (ret < 0) return ret;

  /* uinit rx if any */
  udp_uinit_rxq(impl, s);

  /* save bind addr */
  s->bind_addr = *addr_in;
  /* update hdr */
  s->hdr.udp.src_port = addr_in->sin_port;
  udp_set_flag(s, MUDP_BIND);
  return 0;
}

ssize_t mudp_sendto(mudp_handle ut, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parnet;
  int idx = s->idx;
  int ret;
  struct rte_mbuf* m;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  const struct sockaddr_in* addr_in = (struct sockaddr_in*)dest_addr;
  ret = udp_verfiy_sendto_args(len, flags, addr_in, addrlen);
  if (ret < 0) {
    err("%s(%d), invalid args\n", __func__, idx);
    return ret;
  }

  /* init txq if not */
  if (!udp_get_flag(s, MUDP_TXQ_ALLOC)) {
    ret = udp_init_txq(impl, s);
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

  ret = udp_build_tx_pkt(impl, s, m, buf, len, addr_in);
  if (ret < 0) {
    err("%s(%d), build pkt fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free(m);
    return ret;
  }

  uint16_t tx = mt_dev_tx_burst_busy(impl, s->txq, &m, 1, s->tx_timeout_ms);
  if (tx < 1) {
    err("%s(%d), tx pkt fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free(m);
    return -EIO;
  }

  return len;
}

ssize_t mudp_recvfrom(mudp_handle ut, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  struct mudp_impl* s = ut;
  struct mtl_main_impl* impl = s->parnet;
  int idx = s->idx;
  int ret;
  ssize_t copied = 0;

  /* init rxq if not */
  if (!udp_get_flag(s, MUDP_RXQ_ALLOC)) {
    ret = udp_init_rxq(impl, s);
    if (ret < 0) {
      err("%s(%d), init rxq fail\n", __func__, idx);
      return ret;
    }
  }

  uint64_t start_ts = mt_get_tsc(impl);
  struct rte_mbuf* pkt = NULL;
dequeue:
  /* dequeue pkt from rx ring */
  ret = rte_ring_sc_dequeue(s->rx_ring, (void**)&pkt);
  if (ret >= 0) {
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
    struct rte_udp_hdr* udp = &hdr->udp;
    void* payload = (void*)(udp + 1);
    ssize_t payload_len = ntohs(udp->dgram_len) - sizeof(*udp);
    dbg("%s(%d), payload_len %d bytes\n", __func__, idx, (int)payload_len);

    if (payload_len <= len) {
      rte_memcpy(buf, payload, payload_len);
      copied = payload_len;
    } else {
      err("%s(%d), payload len %d buf len %d\n", __func__, idx, (int)payload_len,
          (int)len);
    }
    rte_pktmbuf_free(pkt);
    dbg("%s(%d), copied %d bytes\n", __func__, idx, (int)copied);
    return copied;
  }

  uint16_t rx;
rx_pool:
  rx = udp_rx(impl, s);
  if (rx) { /* dequeue again as rx succ */
    goto dequeue;
  }

  int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
  if ((ms < s->rx_timeout_ms) && !mt_aborted(impl)) {
    goto rx_pool;
  }

  dbg("%s(%d), timeout to %d ms\n", __func__, idx, s->rx_timeout_ms);
  return -ETIMEDOUT;
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

  s->txq_bps = bps;
  info("%s(%d), new bps: %" PRIu64 "\n", __func__, idx, bps);
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

int mudp_set_tx_timeout_ms(mudp_handle ut, int ms) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  s->tx_timeout_ms = ms;
  info("%s(%d), new timeout: %u ms\n", __func__, idx, ms);
  return 0;
}

int mudp_get_tx_timeout_ms(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  return s->tx_timeout_ms;
}

int mudp_set_rx_timeout_ms(mudp_handle ut, int ms) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  s->rx_timeout_ms = ms;
  info("%s(%d), new timeout: %u ms\n", __func__, idx, ms);
  return 0;
}

int mudp_get_rx_timeout_ms(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  return s->rx_timeout_ms;
}

int mudp_set_arp_timeout_ms(mudp_handle ut, int ms) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  s->arp_timeout_ms = ms;
  info("%s(%d), new timeout: %u ms\n", __func__, idx, ms);
  return 0;
}

int mudp_get_arp_timeout_ms(mudp_handle ut) {
  struct mudp_impl* s = ut;
  int idx = s->idx;

  if (s->type != MT_HANDLE_UDP) {
    err("%s(%d), invalid type %d\n", __func__, idx, s->type);
    return -EIO;
  }

  return s->arp_timeout_ms;
}
