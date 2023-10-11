/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_dp_socket.h"

#include "../mt_log.h"
#include "../mt_util.h"
#ifndef WINDOWSENV
#include "mudp_api.h"
#endif

#define MT_RX_DP_SOCKET_PREFIX "SR_"

#ifndef WINDOWSENV

static int tx_socket_send_mbuf(struct mt_tx_socket_thread* t, struct rte_mbuf* m) {
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  int fd = t->fd;
  struct sockaddr_in send_addr;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;

  /* check if suppoted */
  if (m->nb_segs > 1) {
    err("%s(%d,%d), only support one nb_segs %u\n", __func__, port, fd, m->nb_segs);
    return -ENOTSUP;
  }
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(m, struct mt_udp_hdr*);
  struct rte_ether_hdr* eth = &hdr->eth;

  if (eth->ether_type != htons(RTE_ETHER_TYPE_IPV4)) {
    err("%s(%d,%d), not ipv4\n", __func__, port, fd);
    return -ENOTSUP;
  }
  // mt_mbuf_dump(port, 0, "socket_tx", m);

  void* payload = rte_pktmbuf_mtod_offset(m, void*, sizeof(struct mt_udp_hdr));
  ssize_t payload_len = m->data_len - sizeof(struct mt_udp_hdr);

  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  mudp_init_sockaddr(&send_addr, (uint8_t*)&ipv4->dst_addr, ntohs(udp->dst_port));

  /* nonblocking */
  ssize_t send = sendto(fd, payload, payload_len, MSG_DONTWAIT,
                        (const struct sockaddr*)&send_addr, sizeof(send_addr));
  dbg("%s(%d,%d), len %" PRId64 " send %" PRId64 "\n", __func__, port, fd, payload_len,
      send);
  if (send != payload_len) {
    dbg("%s(%d,%d), sendto fail, len %" PRId64 " send %" PRId64 "\n", __func__, port, fd,
        payload_len, send);
    return -EBUSY;
  }
  if (stats) {
    stats->tx_packets++;
    stats->tx_bytes += m->data_len;
  }

  return 0;
}

static void* tx_socket_thread(void* arg) {
  struct mt_tx_socket_thread* t = arg;
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  struct rte_mbuf* m = NULL;
  int ret;

  info("%s(%d), start, fd %d\n", __func__, port, t->fd);
  while (rte_atomic32_read(&t->stop_thread) == 0) {
    ret = rte_ring_mc_dequeue(entry->ring, (void**)&m);
    if (ret < 0) continue;
    do {
      ret = tx_socket_send_mbuf(t, m);
    } while ((ret < 0) && (rte_atomic32_read(&t->stop_thread) == 0));
    rte_pktmbuf_free(m);
  }
  info("%s(%d), stop, fd %d\n", __func__, port, t->fd);

  return NULL;
}

static int tx_socket_init_threads(struct mt_tx_socket_entry* entry) {
  int idx = entry->threads_data[0].fd;
  int fd, ret;

  /* fds[0] already init */
  for (int i = 1; i < entry->threads; i++) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      mt_tx_socket_put(entry);
      err("%s(%d), socket open fail %d\n", __func__, idx, fd);
      return fd;
    }
    entry->threads_data[i].fd = fd;
    /* non-blocking */
    ret = mt_fd_set_nonbolck(fd);
    if (ret < 0) return ret;
    info("%s(%d), fd %d for thread %d\n", __func__, idx, fd, i);
  }

  /* create the ring, one producer multi consumer */
  char ring_name[64];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, sizeof(ring_name), "%sP%dFD%d", MT_RX_DP_SOCKET_PREFIX, entry->port,
           idx);
  flags = RING_F_SP_ENQ;
  count = 1024 * 4;
  ring =
      rte_ring_create(ring_name, count, mt_socket_id(entry->parent, entry->port), flags);
  if (!ring) {
    err("%s(%d), ring create fail\n", __func__, idx);
    return -EIO;
  }
  entry->ring = ring;

  /* create the threads except the base fd */
  for (int i = 0; i < entry->threads; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];

    rte_atomic32_set(&t->stop_thread, 0);
    ret = pthread_create(&t->tid, NULL, tx_socket_thread, t);
    if (ret < 0) {
      err("%s(%d), thread create fail %d for thread %d\n", __func__, idx, ret, i);
      return ret;
    }
  }

  return 0;
}

struct mt_tx_socket_entry* mt_tx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_txq_flow* flow) {
  int ret;

  if (!mt_drv_kernel_based(impl, port)) {
    err("%s(%d), this pmd is not kernel based\n", __func__, port);
    return NULL;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), socket open fail %d\n", __func__, port, fd);
    return NULL;
  }

  /* non-blocking */
  ret = mt_fd_set_nonbolck(fd);
  if (ret < 0) {
    err("%s(%d,%d), set nonbolck fail %d\n", __func__, port, fd, ret);
    close(fd);
    return NULL;
  }

  /* bind to device */
  const char* if_name = mt_kernel_if_name(impl, port);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, fd, if_name, ret);
    close(fd);
    return NULL;
  }

  struct mt_tx_socket_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    close(fd);
    return NULL;
  }
  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];
    t->fd = -1;
    t->parent = entry;
  }
  entry->parent = impl;
  entry->port = port;
  /* 4g bit per second */
  entry->rate_limit_per_thread = (uint64_t)4 * 1000 * 1000 * 1000;
  entry->threads_data[0].fd = fd;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  uint64_t required = flow->bytes_per_sec * 8;
  entry->threads = required / entry->rate_limit_per_thread + 1;
  if (entry->threads > 1) {
    ret = tx_socket_init_threads(entry);
    if (ret < 0) {
      err("%s(%d,%d), init %d threads fail %d\n", __func__, port, fd, entry->threads,
          ret);
      mt_tx_socket_put(entry);
      return NULL;
    }
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), fd %d ip %u.%u.%u.%u, port %u, threads %u\n", __func__, port,
       entry->threads_data[0].fd, ip[0], ip[1], ip[2], ip[3], flow->dst_port,
       entry->threads);
  return entry;
}

int mt_tx_socket_put(struct mt_tx_socket_entry* entry) {
  int idx = entry->threads_data[0].fd;

  /* stop threads */
  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];

    rte_atomic32_set(&t->stop_thread, 1);
    if (t->tid) {
      pthread_join(t->tid, NULL);
      t->tid = 0;
    }
  }

  if (entry->ring) {
    mt_ring_dequeue_clean(entry->ring);
    rte_ring_free(entry->ring);
    entry->ring = NULL;
  }

  /* close fd */
  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];

    if (t->fd >= 0) {
      close(t->fd);
      t->fd = -1;
    }
  }

  info("%s(%d,%d), succ\n", __func__, entry->port, idx);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_socket_burst(struct mt_tx_socket_entry* entry, struct rte_mbuf** tx_pkts,
                            uint16_t nb_pkts) {
  uint16_t tx = 0;
  int ret;

  if (entry->ring) {
    unsigned int n =
        rte_ring_sp_enqueue_bulk(entry->ring, (void**)&tx_pkts[0], nb_pkts, NULL);
    // tx_socket_dequeue(&entry->threads_data[0]);
    return n;
  }

  for (tx = 0; tx < nb_pkts; tx++) {
    struct rte_mbuf* m = tx_pkts[tx];
    ret = tx_socket_send_mbuf(&entry->threads_data[0], m);
    if (ret < 0) break;
  }

  rte_pktmbuf_free_bulk(tx_pkts, tx);
  return tx;
}

struct mt_rx_socket_entry* mt_rx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_rxq_flow* flow) {
  int ret;

  if (!mt_drv_kernel_based(impl, port)) {
    err("%s(%d), this pmd is not kernel based\n", __func__, port);
    return NULL;
  }

  if (flow->flags & MT_RXQ_FLOW_F_SYS_QUEUE) {
    err("%s(%d), sys_queue not supported\n", __func__, port);
    return NULL;
  }
  if (flow->flags & MT_RXQ_FLOW_F_NO_PORT) {
    err("%s(%d), no_port_flow not supported\n", __func__, port);
    return NULL;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), socket open fail %d\n", __func__, port, fd);
    return NULL;
  }
  /* non-blocking */
  ret = mt_fd_set_nonbolck(fd);
  if (ret < 0) {
    err("%s(%d,%d), set nonbolck fail %d\n", __func__, port, fd, ret);
    close(fd);
    return NULL;
  }

  /* bind to device */
  const char* if_name = mt_kernel_if_name(impl, port);
  info("%s(%d,%d), SO_BINDTODEVICE to %s\n", __func__, port, fd, if_name);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, fd, if_name, ret);
    close(fd);
    return NULL;
  }

  /* bind to port */
  struct sockaddr_in bind_addr;
  if (mt_is_multicast_ip(flow->dip_addr))
    mudp_init_sockaddr(&bind_addr, flow->dip_addr, flow->dst_port);
  else
    mudp_init_sockaddr(&bind_addr, mt_sip_addr(impl, port), flow->dst_port);
  ret = bind(fd, (const struct sockaddr*)&bind_addr, sizeof(bind_addr));
  if (ret < 0) {
    err("%s(%d,%d), bind to port %u fail %d\n", __func__, port, fd, flow->dst_port, ret);
    close(fd);
    return NULL;
  }

  if (mt_is_multicast_ip(flow->dip_addr)) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    /* multicast addr */
    memcpy(&mreq.imr_multiaddr.s_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
    /* local nic src ip */
    memcpy(&mreq.imr_interface.s_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    ret = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (ret < 0) {
      err("%s(%d), join multicast fail %d\n", __func__, fd, ret);
      close(fd);
      return NULL;
    }
    info("%s(%d), join multicast succ\n", __func__, fd);
  }

  struct mt_rx_socket_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    close(fd);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  entry->fd = fd;
  entry->pool_element_sz = 2048;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  /* Create mempool to hold the rx queue mbufs. */
  unsigned int mbuf_elements = mt_if_nb_rx_desc(impl, port) + 1024;
  char pool_name[ST_MAX_NAME_LEN];
  snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%dF%d_MBUF", MT_RX_DP_SOCKET_PREFIX, port, fd);
  /* no priv */
  entry->pool =
      mt_mempool_create_by_ops(impl, port, pool_name, mbuf_elements, MT_MBUF_CACHE_SIZE,
                               0, entry->pool_element_sz, NULL);
  if (!entry->pool) {
    err("%s(%d), mempool %s create fail\n", __func__, port, pool_name);
    mt_rx_socket_put(entry);
    return NULL;
  }
  entry->pkt = rte_pktmbuf_alloc(entry->pool);
  if (!entry->pkt) {
    err("%s(%d), pkt create fail\n", __func__, port);
    mt_rx_socket_put(entry);
    return NULL;
  }

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), fd %d ip %u.%u.%u.%u, port %u\n", __func__, port, fd, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port);
  return entry;
}

int mt_rx_socket_put(struct mt_rx_socket_entry* entry) {
  if (entry->fd > 0) {
    close(entry->fd);
  }
  if (entry->pkt) {
    rte_pktmbuf_free(entry->pkt);
    entry->pkt = NULL;
  }
  if (entry->pool) {
    mt_mempool_free(entry->pool);
    entry->pool = NULL;
  }
  info("%s(%d,%d), succ\n", __func__, entry->port, entry->fd);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_socket_burst(struct mt_rx_socket_entry* entry, struct rte_mbuf** rx_pkts,
                            const uint16_t nb_pkts) {
  enum mtl_port port = entry->port;
  int fd = entry->fd;
  uint16_t rx = 0;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;

  if (!entry->pkt) {
    entry->pkt = rte_pktmbuf_alloc(entry->pool);
    if (!entry->pkt) {
      err("%s(%d), pkt create fail\n", __func__, port);
      return 0;
    }
  }

  for (rx = 0; rx < nb_pkts; rx++) {
    struct rte_mbuf* pkt = entry->pkt;
    struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
    void* payload = &hdr[1];
    struct rte_udp_hdr* udp = &hdr->udp;
    struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;

    struct sockaddr_in addr_in;
    socklen_t addr_in_len = sizeof(addr_in);

    ssize_t len = recvfrom(fd, payload, entry->pool_element_sz, MSG_DONTWAIT, &addr_in,
                           &addr_in_len);
    if (len <= 0) {
      return rx;
    }
    /* get one packet */
    dbg("%s(%d,%d), recv len %" PRId64 "\n", __func__, port, fd, len);
    pkt->pkt_len = len + sizeof(*hdr);
    pkt->data_len = pkt->pkt_len;
    udp->dgram_len = htons(len + sizeof(*udp));
    udp->src_port = addr_in.sin_port;
    ipv4->src_addr = addr_in.sin_addr.s_addr;
    ipv4->next_proto_id = IPPROTO_UDP;

    rx_pkts[rx] = pkt;
    if (stats) {
      stats->rx_packets++;
      stats->rx_bytes += pkt->data_len;
    }

    /* allocate a new pkt for next iteration */
    entry->pkt = rte_pktmbuf_alloc(entry->pool);
    if (!entry->pkt) {
      err("%s(%d), pkt create fail at %u\n", __func__, port, rx);
      return rx;
    }
  }

  return rx;
}

#else
struct mt_tx_socket_entry* mt_tx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_txq_flow* flow) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(flow);
  err("%s(%d), not support on this platform\n", __func__, port);
  return NULL;
}

int mt_tx_socket_put(struct mt_tx_socket_entry* entry) {
  err("%s(%d), not support on this platform\n", __func__, entry->port);
  return 0;
}

uint16_t mt_tx_socket_burst(struct mt_tx_socket_entry* entry, struct rte_mbuf** tx_pkts,
                            uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  err("%s(%d), not support on this platform\n", __func__, entry->port);
  rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
  return 0;
}

struct mt_rx_socket_entry* mt_rx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_rxq_flow* flow) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(flow);
  err("%s(%d), not support on this platform\n", __func__, port);
  return NULL;
}

int mt_rx_socket_put(struct mt_rx_socket_entry* entry) {
  err("%s(%d), not support on this platform\n", __func__, entry->port);
  return 0;
}

uint16_t mt_rx_socket_burst(struct mt_rx_socket_entry* entry, struct rte_mbuf** rx_pkts,
                            const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}
#endif
