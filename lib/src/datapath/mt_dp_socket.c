/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_dp_socket.h"

#include "../mt_log.h"
#include "../mt_socket.h"
#include "../mt_stat.h"
#include "../mt_util.h"
#ifndef WINDOWSENV
#include "deprecated/mudp_api.h"
#endif

#define MT_RX_DP_SOCKET_PREFIX "SR_"
#define MT_TX_DP_SOCKET_PREFIX "SR_"

#ifndef UDP_SEGMENT
/* fix for centos build */
#define UDP_SEGMENT 103 /* Set GSO segmentation size */
#endif

#ifndef WINDOWSENV

static inline int tx_socket_verify_mbuf(struct rte_mbuf* m) {
  if (m->nb_segs > 1) {
    err("%s, only support one nb_segs %u\n", __func__, m->nb_segs);
    return -ENOTSUP;
  }

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(m, struct mt_udp_hdr*);
  struct rte_ether_hdr* eth = &hdr->eth;
  uint16_t ether_type = ntohs(eth->ether_type);

  if (ether_type != RTE_ETHER_TYPE_IPV4) {
    err("%s, not ipv4, ether_type 0x%x\n", __func__, ether_type);
    return -ENOTSUP;
  }

  return 0;
}

static int tx_socket_send_mbuf(struct mt_tx_socket_thread* t, struct rte_mbuf* m) {
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  int fd = t->fd, ret;
  struct sockaddr_in send_addr;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;

  /* check if suppoted */
  ret = tx_socket_verify_mbuf(m);
  if (ret < 0) {
    err("%s(%d,%d), unsupported mbuf %p ret %d\n", __func__, port, fd, m, ret);
    return ret;
  }

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(m, struct mt_udp_hdr*);
  // mt_mbuf_dump(port, 0, "socket_tx", m);

  void* payload = rte_pktmbuf_mtod_offset(m, void*, sizeof(struct mt_udp_hdr));
  ssize_t payload_len = m->data_len - sizeof(struct mt_udp_hdr);

  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  mudp_init_sockaddr(&send_addr, (uint8_t*)&ipv4->dst_addr, ntohs(udp->dst_port));

  t->stat_tx_try++;
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
  t->stat_tx_pkt++;

  return 0;
}

static uint16_t tx_socket_send_mbuf_gso(struct mt_tx_socket_thread* t,
                                        struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  uint16_t tx = 0;
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  int fd = t->fd, ret;
  uint16_t gso_sz = entry->gso_sz;
  struct iovec iovs[nb_pkts];
  uint16_t gso_cnt = 0;
  struct msghdr* msg = &t->msg;
  ssize_t write;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;

  msg->msg_iov = iovs;

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    ret = tx_socket_verify_mbuf(m);
    if (ret < 0) {
      err("%s(%d,%d), unsupported mbuf %p ret %d\n", __func__, port, fd, m, ret);
      return tx;
    }

    t->stat_tx_try++;
    uint16_t payload_len = m->data_len - sizeof(struct mt_udp_hdr);
    void* payload = rte_pktmbuf_mtod_offset(m, void*, sizeof(struct mt_udp_hdr));
    dbg("%s(%d,%d), mbuf %u payload_len %u\n", __func__, port, fd, i, payload_len);

    if (payload_len == gso_sz) {
      iovs[gso_cnt].iov_base = payload;
      iovs[gso_cnt].iov_len = payload_len;
      gso_cnt++;
    } else {
      if (gso_cnt) {
        msg->msg_iovlen = gso_cnt;
        write = sendmsg(fd, msg, MSG_DONTWAIT);
        if (write != (gso_sz * gso_cnt)) {
          dbg("%s(%d,%d), sendmsg 1 fail, len %u send %" PRId64 "\n", __func__, port, fd,
              gso_sz * gso_cnt, write);
          return tx;
        }
        tx += gso_cnt;
        if (stats) {
          stats->tx_packets += gso_cnt;
          stats->tx_bytes += write;
        }
        t->stat_tx_pkt += gso_cnt;
        t->stat_tx_gso++;

        gso_cnt = 0;
      }
      write = sendto(fd, payload, payload_len, MSG_DONTWAIT, &t->send_addr,
                     sizeof(t->send_addr));
      if (write != payload_len) {
        dbg("%s(%d,%d), sendto fail, len %u send %" PRId64 "\n", __func__, port, fd,
            payload_len, write);
        return tx;
      }
      tx++;
      if (stats) {
        stats->tx_packets++;
        stats->tx_bytes += write;
      }
      t->stat_tx_pkt++;
    }
  }

  if (gso_cnt) {
    msg->msg_iovlen = gso_cnt;
    write = sendmsg(fd, msg, MSG_DONTWAIT);
    if (write != (gso_sz * gso_cnt)) {
      dbg("%s(%d,%d), sendmsg fail, len %u send %" PRId64 "\n", __func__, port, fd,
          gso_sz * gso_cnt, write);
      return tx;
    }
    dbg("%s(%d,%d), sendmsg succ, len %u send %" PRId64 "\n", __func__, port, fd,
        gso_sz * gso_cnt, write);
    tx += gso_cnt;
    if (stats) {
      stats->tx_packets += gso_cnt;
      stats->tx_bytes += write;
    }
    t->stat_tx_pkt += gso_cnt;
    t->stat_tx_gso++;
  }

  return tx;
}

static void* tx_socket_thread_loop(void* arg) {
  struct mt_tx_socket_thread* t = arg;
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  struct rte_mbuf* m = NULL;
  int ret;

  info("%s(%d,%d), start\n", __func__, port, t->fd);
  while (rte_atomic32_read(&t->stop_thread) == 0) {
    ret = rte_ring_mc_dequeue(entry->ring, (void**)&m);
    if (ret < 0) continue;
    do {
      ret = tx_socket_send_mbuf(t, m);
    } while ((ret < 0) && (rte_atomic32_read(&t->stop_thread) == 0));
    rte_pktmbuf_free(m);
  }
  info("%s(%d,%d), stop\n", __func__, port, t->fd);

  return NULL;
}

static int tx_socket_init_thread_data(struct mt_tx_socket_thread* t) {
  int ret;
  struct mt_tx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  int idx = t->idx;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d,%d), socket open fail %d\n", __func__, port, idx, fd);
    return fd;
  }
  t->fd = fd;
  info("%s(%d), fd %d for thread %d\n", __func__, idx, fd, idx);

  /* non-blocking */
  ret = mt_fd_set_nonbolck(fd);
  if (ret < 0) {
    err("%s(%d,%d), set nonbolck fail %d\n", __func__, port, idx, ret);
    return ret;
  }

  /* bind to device */
  const char* if_name = mt_kernel_if_name(entry->parent, port);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, idx, if_name, ret);
    return ret;
  }

  if (entry->gso_sz) {
    mudp_init_sockaddr(&t->send_addr, entry->flow.dip_addr, entry->flow.dst_port);
    t->msg.msg_namelen = sizeof(t->send_addr);
    t->msg.msg_name = &t->send_addr;

    /* gso size for sendmsg */
    t->msg.msg_control = t->msg_control;
    t->msg.msg_controllen = sizeof(t->msg_control);
    struct cmsghdr* cmsg;
    cmsg = CMSG_FIRSTHDR(&t->msg);
    cmsg->cmsg_level = SOL_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t* val_p;
    val_p = (uint16_t*)CMSG_DATA(cmsg);
    *val_p = entry->gso_sz;
  }

  return 0;
}

static int tx_socket_init_threads(struct mt_tx_socket_entry* entry) {
  int idx = entry->threads_data[0].fd;
  int ret;

  /* fds[0] already init */
  for (int i = 1; i < entry->threads; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];
    ret = tx_socket_init_thread_data(t);
    if (ret < 0) return ret;
  }

  /* create the ring, multi producer single consumer */
  char ring_name[64];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, sizeof(ring_name), "%sRP%dFD%d", MT_TX_DP_SOCKET_PREFIX,
           entry->port, idx);
  flags = RING_F_SC_DEQ;
  count = mt_if_nb_rx_desc(entry->parent, entry->port);
  ring =
      rte_ring_create(ring_name, count, mt_socket_id(entry->parent, entry->port), flags);
  if (!ring) {
    err("%s(%d), ring create fail\n", __func__, idx);
    return -EIO;
  }
  entry->ring = ring;

  /* create the threads */
  for (int i = 0; i < entry->threads; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];

    rte_atomic32_set(&t->stop_thread, 0);
    ret = pthread_create(&t->tid, NULL, tx_socket_thread_loop, t);
    if (ret < 0) {
      err("%s(%d), thread create fail %d for thread %d\n", __func__, idx, ret, i);
      return ret;
    }
  }

  return 0;
}

static int tx_socket_stat_dump(void* priv) {
  struct mt_tx_socket_entry* entry = priv;
  enum mtl_port port = entry->port;

  for (int i = 0; i < entry->threads; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];
    int fd = t->fd;

    info("%s(%d,%d), tx pkt %d gso %d try %d on thread %d\n", __func__, port, fd,
         t->stat_tx_pkt, t->stat_tx_gso, t->stat_tx_try, i);
    t->stat_tx_pkt = 0;
    t->stat_tx_gso = 0;
    t->stat_tx_try = 0;
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

  struct mt_tx_socket_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  /* 5g bit per second */
  entry->rate_limit_per_thread = (uint64_t)6 * 1000 * 1000 * 1000;
  entry->gso_sz = flow->gso_sz;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    struct mt_tx_socket_thread* t = &entry->threads_data[i];
    t->idx = i;
    t->fd = -1;
    t->parent = entry;
  }

  ret = tx_socket_init_thread_data(&entry->threads_data[0]);
  if (ret < 0) {
    mt_tx_socket_put(entry);
    return NULL;
  }

  uint64_t required = flow->bytes_per_sec * 8;
  entry->threads = required / entry->rate_limit_per_thread + 1;
  entry->threads = RTE_MIN(entry->threads, MT_DP_SOCKET_THREADS_MAX);
  if (entry->threads > 1) {
    ret = tx_socket_init_threads(entry);
    if (ret < 0) {
      err("%s(%d), init %d threads fail %d\n", __func__, port, entry->threads, ret);
      mt_tx_socket_put(entry);
      return NULL;
    }
  }

  ret = mt_stat_register(impl, tx_socket_stat_dump, entry, "tx_socket");
  if (ret < 0) {
    err("%s(%d), stat register fail %d\n", __func__, port, ret);
    mt_tx_socket_put(entry);
    return NULL;
  }
  entry->stat_registered = true;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), fd %d ip %u.%u.%u.%u, port %u, threads %u gso_sz %u\n", __func__, port,
       entry->threads_data[0].fd, ip[0], ip[1], ip[2], ip[3], flow->dst_port,
       entry->threads, entry->gso_sz);
  return entry;
}

int mt_tx_socket_put(struct mt_tx_socket_entry* entry) {
  int idx = entry->threads_data[0].fd;
  enum mtl_port port = entry->port;

  if (entry->stat_registered) {
    tx_socket_stat_dump(entry);
    mt_stat_unregister(entry->parent, tx_socket_stat_dump, entry);
    entry->stat_registered = false;
  }

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

  info("%s(%d,%d), succ\n", __func__, port, idx);
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

  if (entry->gso_sz) {
    tx = tx_socket_send_mbuf_gso(&entry->threads_data[0], tx_pkts, nb_pkts);
  } else {
    for (tx = 0; tx < nb_pkts; tx++) {
      struct rte_mbuf* m = tx_pkts[tx];
      ret = tx_socket_send_mbuf(&entry->threads_data[0], m);
      if (ret < 0) break;
    }
  }

  rte_pktmbuf_free_bulk(tx_pkts, tx);
  return tx;
}

static int rx_socket_init_fd(struct mt_rx_socket_entry* entry, int fd, bool reuse) {
  int ret;
  enum mtl_port port = entry->port;
  struct mtl_main_impl* impl = entry->parent;
  struct mt_rxq_flow* flow = &entry->flow;

  if (reuse) {
    int optval = 1;
    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    if (ret < 0) {
      err("%s(%d,%d), set reuse fail %d\n", __func__, port, fd, ret);
      return ret;
    }
  }

  /* non-blocking */
  ret = mt_fd_set_nonbolck(fd);
  if (ret < 0) {
    err("%s(%d,%d), set nonbolck fail %d\n", __func__, port, fd, ret);
    return ret;
  }

  /* bind to device */
  const char* if_name = mt_kernel_if_name(impl, port);
  info("%s(%d,%d), SO_BINDTODEVICE to %s\n", __func__, port, fd, if_name);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, fd, if_name, ret);
    return ret;
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
    return ret;
  }

  /* join multicast group, will drop automatically when socket fd closed */
  if (mt_is_multicast_ip(flow->dip_addr)) {
    ret = mt_socket_fd_join_multicast(impl, port, flow, fd);
    if (ret < 0) {
      err("%s(%d,%d), join multicast fail %d\n", __func__, port, fd, ret);
      return ret;
    }
  }

  return 0;
}

static struct rte_mbuf* rx_socket_recv_mbuf(struct mt_rx_socket_thread* t) {
  struct mt_rx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  struct mtl_port_status* stats = mt_if(entry->parent, port)->dev_stats_sw;
  int fd = entry->fd;
  struct rte_mbuf* pkt = t->mbuf;

  if (!pkt) {
    pkt = rte_pktmbuf_alloc(entry->pool);
    if (!pkt) {
      err("%s(%d), pkt alloc fail\n", __func__, port);
      return NULL;
    }
    t->mbuf = pkt;
  }

  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  void* payload = &hdr[1];
  struct rte_udp_hdr* udp = &hdr->udp;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct sockaddr_in addr_in;
  socklen_t addr_in_len = sizeof(addr_in);

  t->stat_rx_try++;
  ssize_t len =
      recvfrom(fd, payload, entry->pool_element_sz, MSG_DONTWAIT, &addr_in, &addr_in_len);
  if (len <= 0) {
    return NULL;
  }
  /* get one packet */
  dbg("%s(%d,%d), recv len %" PRId64 "\n", __func__, port, fd, len);
  pkt->pkt_len = len + sizeof(*hdr);
  pkt->data_len = pkt->pkt_len;
  udp->dgram_len = htons(len + sizeof(*udp));
  udp->src_port = addr_in.sin_port;
  ipv4->src_addr = addr_in.sin_addr.s_addr;
  ipv4->next_proto_id = IPPROTO_UDP;

  if (stats) {
    stats->rx_packets++;
    stats->rx_bytes += pkt->data_len;
  }
  t->stat_rx_pkt++;

  /* succ, deliver the pkt */
  t->mbuf = NULL;
  return pkt;
}

static void* rx_socket_thread_loop(void* arg) {
  struct mt_rx_socket_thread* t = arg;
  struct mt_rx_socket_entry* entry = t->parent;
  enum mtl_port port = entry->port;
  int idx = t->idx, fd = entry->fd;
  struct rte_mbuf* m;
  int ret;

  info("%s(%d,%d), start thread %d\n", __func__, port, fd, idx);
  while (rte_atomic32_read(&t->stop_thread) == 0) {
    m = rx_socket_recv_mbuf(t);
    if (!m) continue;
    while (rte_atomic32_read(&t->stop_thread) == 0) {
      ret = rte_ring_mp_enqueue(entry->ring, m);
      if (ret >= 0) break; /* succ */
    }
  }
  info("%s(%d,%d), stop thread %d\n", __func__, port, fd, idx);

  return NULL;
}

static int rx_socket_init_threads(struct mt_rx_socket_entry* entry) {
  int fd = entry->fd;
  enum mtl_port port = entry->port;
  int ret;

  /* create the ring, one producer multi consumer */
  char ring_name[64];
  struct rte_ring* ring;
  unsigned int flags, count;
  snprintf(ring_name, sizeof(ring_name), "%sRP%dFD%d", MT_RX_DP_SOCKET_PREFIX, port, fd);
  flags = RING_F_SP_ENQ;
  count = mt_if_nb_tx_desc(entry->parent, port);
  ring = rte_ring_create(ring_name, count, mt_socket_id(entry->parent, port), flags);
  if (!ring) {
    err("%s(%d,%d), ring create fail\n", __func__, port, fd);
    return -EIO;
  }
  entry->ring = ring;

  /* create the threads except the base fd */
  for (int i = 0; i < entry->threads; i++) {
    struct mt_rx_socket_thread* t = &entry->threads_data[i];

    rte_atomic32_set(&t->stop_thread, 0);
    ret = pthread_create(&t->tid, NULL, rx_socket_thread_loop, t);
    if (ret < 0) {
      err("%s(%d,%d), thread create fail %d for thread %d\n", __func__, port, fd, ret, i);
      return ret;
    }
  }

  return 0;
}

static int rx_socket_stat_dump(void* priv) {
  struct mt_rx_socket_entry* entry = priv;
  enum mtl_port port = entry->port;
  int fd = entry->fd;

  for (int i = 0; i < entry->threads; i++) {
    struct mt_rx_socket_thread* t = &entry->threads_data[i];

    info("%s(%d,%d), rx pkt %d try %d on thread %d\n", __func__, port, fd, t->stat_rx_pkt,
         t->stat_rx_try, i);
    t->stat_rx_pkt = 0;
    t->stat_rx_try = 0;
  }

  return 0;
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

  struct mt_rx_socket_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    close(fd);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  entry->pool_element_sz = 2048;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));
  /* 5g bit per second */
  entry->rate_limit_per_thread = (uint64_t)5 * 1000 * 1000 * 1000;

  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    struct mt_rx_socket_thread* t = &entry->threads_data[i];
    t->idx = i;
    t->parent = entry;
  }
  entry->fd = fd;

  uint64_t required = flow->bytes_per_sec * 8;
  entry->threads = required / entry->rate_limit_per_thread + 1;
  entry->threads = RTE_MIN(entry->threads, MT_DP_SOCKET_THREADS_MAX);
  ret = rx_socket_init_fd(entry, fd, false);
  if (ret < 0) {
    mt_rx_socket_put(entry);
    return NULL;
  }

  /* Create mempool to hold the rx queue mbufs. */
  unsigned int mbuf_elements = mt_if_nb_rx_desc(impl, port) + 1024;
  char pool_name[ST_MAX_NAME_LEN];
  snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%dF%d_MBUF", MT_RX_DP_SOCKET_PREFIX, port, fd);
  /* no priv */
  entry->pool = mt_mempool_create(impl, port, pool_name, mbuf_elements,
                                  MT_MBUF_CACHE_SIZE, 0, entry->pool_element_sz);
  if (!entry->pool) {
    err("%s(%d), mempool %s create fail\n", __func__, port, pool_name);
    mt_rx_socket_put(entry);
    return NULL;
  }

  if (entry->threads > 1) {
    ret = rx_socket_init_threads(entry);
    if (ret < 0) {
      err("%s(%d,%d), init %d threads fail %d\n", __func__, port, fd, entry->threads,
          ret);
      mt_rx_socket_put(entry);
      return NULL;
    }
  }

  ret = mt_stat_register(impl, rx_socket_stat_dump, entry, "rx_socket");
  if (ret < 0) {
    err("%s(%d), stat register fail %d\n", __func__, port, ret);
    mt_rx_socket_put(entry);
    return NULL;
  }
  entry->stat_registered = true;

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), fd %d ip %u.%u.%u.%u port %u threads %d\n", __func__, port, fd, ip[0],
       ip[1], ip[2], ip[3], flow->dst_port, entry->threads);
  return entry;
}

int mt_rx_socket_put(struct mt_rx_socket_entry* entry) {
  int fd = entry->fd;
  enum mtl_port port = entry->port;
  struct mt_rx_socket_thread* t;

  if (entry->stat_registered) {
    rx_socket_stat_dump(entry);
    mt_stat_unregister(entry->parent, rx_socket_stat_dump, entry);
    entry->stat_registered = false;
  }

  /* stop threads */
  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    t = &entry->threads_data[i];

    rte_atomic32_set(&t->stop_thread, 1);
    if (t->tid) {
      pthread_join(t->tid, NULL);
      t->tid = 0;
    }
    if (t->mbuf) {
      rte_pktmbuf_free(t->mbuf);
      t->mbuf = NULL;
    }
  }

  if (entry->ring) {
    mt_ring_dequeue_clean(entry->ring);
    rte_ring_free(entry->ring);
    entry->ring = NULL;
  }
  /* close fd */
  if (entry->fd >= 0) {
    close(entry->fd);
    entry->fd = -1;
  }
  if (entry->pool) {
    mt_mempool_free(entry->pool);
    entry->pool = NULL;
  }

  info("%s(%d,%d), succ\n", __func__, port, fd);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_socket_burst(struct mt_rx_socket_entry* entry, struct rte_mbuf** rx_pkts,
                            const uint16_t nb_pkts) {
  uint16_t rx = 0;
  struct mt_rx_socket_thread* t = &entry->threads_data[0];

  if (entry->ring) {
    return rte_ring_sc_dequeue_burst(entry->ring, (void**)rx_pkts, nb_pkts, NULL);
  }

  for (rx = 0; rx < nb_pkts; rx++) {
    struct rte_mbuf* pkt = rx_socket_recv_mbuf(t);
    if (!pkt) break;
    rx_pkts[rx] = pkt;
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
