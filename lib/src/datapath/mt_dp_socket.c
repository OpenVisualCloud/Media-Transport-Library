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

#define MT_RX_DP_SOCKET_MEMPOOL_PREFIX "SR_"

#ifndef WINDOWSENV

struct mt_tx_socket_entry* mt_tx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_txq_flow* flow) {
  int ret;

  if (!mt_pmd_is_kernel_socket(impl, port)) {
    err("%s(%d), this pmd is not kernel socket\n", __func__, port);
    return NULL;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), socket open fail %d\n", __func__, port, fd);
    return NULL;
  }

  /* non-blocking */
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  ret = fcntl(fd, F_SETFL, flags);
  if (ret < 0) {
    err("%s(%d,%d), O_NONBLOCK fail %d\n", __func__, port, fd, ret);
    close(fd);
    return NULL;
  }
#if 0
  /* bind to device */
  const char* if_name = mt_kernel_if_name(impl, port);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, fd, if_name, ret);
    close(fd);
    return NULL;
  }
#endif

  struct mt_tx_socket_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    close(fd);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  entry->fd = fd;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));
  mudp_init_sockaddr(&entry->send_addr, flow->dip_addr, flow->dst_port);

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), fd %d ip %u.%u.%u.%u, port %u\n", __func__, port, fd, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port);
  return entry;
}

int mt_tx_socket_put(struct mt_tx_socket_entry* entry) {
  if (entry->fd > 0) {
    close(entry->fd);
  }
  info("%s(%d,%d), succ\n", __func__, entry->port, entry->fd);
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_socket_burst(struct mt_tx_socket_entry* entry, struct rte_mbuf** tx_pkts,
                            uint16_t nb_pkts) {
  uint16_t tx = 0;
  enum mtl_port port = entry->port;
  int fd = entry->fd;

  for (tx = 0; tx < nb_pkts; tx++) {
    struct rte_mbuf* m = tx_pkts[tx];

    /* check if suppoted */
    if (m->nb_segs > 1) {
      err("%s(%d,%d), only support one nb_segs %u\n", __func__, port, fd, m->nb_segs);
      goto done;
    }
    struct rte_ether_hdr* eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
    if (eth->ether_type != htons(RTE_ETHER_TYPE_IPV4)) {
      err("%s(%d,%d), not ipv4\n", __func__, port, fd);
      goto done;
    }

    void* payload = rte_pktmbuf_mtod_offset(m, void*, sizeof(struct mt_udp_hdr));
    ssize_t payload_len = m->data_len - sizeof(struct mt_udp_hdr);
    /* nonblocking */
    ssize_t send =
        sendto(entry->fd, payload, payload_len, MSG_DONTWAIT,
               (const struct sockaddr*)&entry->send_addr, sizeof(entry->send_addr));
    dbg("%s(%d,%d), len %" PRId64 " send %" PRId64 "\n", __func__, port, fd, payload_len,
        send);
    if (send != payload_len) {
      err("%s(%d,%d), sendto fail, len %" PRId64 " send %" PRId64 "\n", __func__, port,
          fd, payload_len, send);
      goto done;
    }
  }

done:
  rte_pktmbuf_free_bulk(tx_pkts, tx);
  return tx;
}

struct mt_rx_socket_entry* mt_rx_socket_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_rxq_flow* flow) {
  int ret;

  if (!mt_pmd_is_kernel_socket(impl, port)) {
    err("%s(%d), this pmd is not kernel socket\n", __func__, port);
    return NULL;
  }

  if (flow->sys_queue) {
    err("%s(%d), sys_queue not supported\n", __func__, port);
    return NULL;
  }
  if (flow->no_port_flow) {
    err("%s(%d), no_port_flow not supported\n", __func__, port);
    return NULL;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("%s(%d), socket open fail %d\n", __func__, port, fd);
    return NULL;
  }
  /* non-blocking */
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  ret = fcntl(fd, F_SETFL, flags);
  if (ret < 0) {
    err("%s(%d,%d), O_NONBLOCK fail %d\n", __func__, port, fd, ret);
    close(fd);
    return NULL;
  }
#if 0
  /* bind to device */
  const char* if_name = mt_kernel_if_name(impl, port);
  info("%s(%d,%d), SO_BINDTODEVICE to %s\n", __func__, port, fd, if_name);
  ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name));
  if (ret < 0) {
    err("%s(%d,%d), SO_BINDTODEVICE to %s fail %d\n", __func__, port, fd, if_name, ret);
    close(fd);
    return NULL;
  }
#endif
  /* bind to port */
  struct sockaddr_in bind_addr;
  mudp_init_sockaddr(&bind_addr, mt_sip_addr(impl, port), flow->dst_port);
  ret = bind(fd, (const struct sockaddr*)&bind_addr, sizeof(bind_addr));
  if (ret < 0) {
    err("%s(%d,%d), bind to port %u fail %d\n", __func__, port, fd, flow->dst_port, ret);
    close(fd);
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
  entry->fd = fd;
  entry->pool_element_sz = 2048;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  /* Create mempool to hold the rx queue mbufs. */
  unsigned int mbuf_elements = mt_if_nb_rx_desc(impl, port) + 1024;
  char pool_name[ST_MAX_NAME_LEN];
  snprintf(pool_name, ST_MAX_NAME_LEN, "%sP%dF%d_MBUF", MT_RX_DP_SOCKET_MEMPOOL_PREFIX,
           port, fd);
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
    addr_in.sin_port = udp->src_port;
    addr_in.sin_addr.s_addr = ipv4->src_addr;

    rx_pkts[rx] = pkt;
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
