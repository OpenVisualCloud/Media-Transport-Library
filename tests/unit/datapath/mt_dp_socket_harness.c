/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Includes the production mt_dp_socket.c so the file-local
 * tx_socket_init_thread_data() can set up a TX socket entry without the stat
 * registration mt_tx_socket_get() needs a full mtl_main_impl for.
 */

#define _GNU_SOURCE /* as mt_main.h, before any system header */
#include "datapath/mt_dp_socket_harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_dp_socket.c"

#define UT_DPS_PAYLOAD_LEN 64
#define UT_DPS_MAX_PKTS 8

static int ut_dps_rx_socket(uint16_t* port) {
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t len = sizeof(addr);
  struct timeval timeout = {.tv_usec = 200 * 1000};
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (fd < 0) return -1;
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      getsockname(fd, (struct sockaddr*)&addr, &len) < 0 ||
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    close(fd);
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}

static int ut_dps_count_datagrams(int fd, int max) {
  char buf[UT_DPS_PAYLOAD_LEN * UT_DPS_MAX_PKTS];
  int count = 0;

  while (count < max && recv(fd, buf, sizeof(buf), 0) > 0) count++;
  return count;
}

int ut_dps_gso_burst(int pkt_count, int* flow_port_datagrams, int* hdr_port_datagrams) {
  static unsigned int pool_idx;
  const uint8_t loopback[MTL_IP_ADDR_LEN] = {127, 0, 0, 1};
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  struct mt_tx_socket_entry* entry = calloc(1, sizeof(*entry));
  struct rte_mbuf* pkts[UT_DPS_MAX_PKTS] = {0};
  struct rte_mempool* pool = NULL;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  uint16_t flow_port = 0, hdr_port = 0;
  int flow_fd = -1, hdr_fd = -1;
  int sent = -ENOMEM;

  if (!impl || !entry) {
    free(entry);
    free(impl);
    return -ENOMEM;
  }
  for (int i = 0; i < MT_DP_SOCKET_THREADS_MAX; i++) {
    entry->threads_data[i].parent = entry;
    entry->threads_data[i].fd = -1;
  }
  if (pkt_count > UT_DPS_MAX_PKTS || ut_eal_init() < 0) goto out;

  snprintf(impl->kport_info.kernel_if[MTL_PORT_P], MTL_PORT_MAX_LEN, "lo");
  entry->parent = impl;
  entry->port = MTL_PORT_P;
  entry->gso_sz = UT_DPS_PAYLOAD_LEN;
  entry->threads = 1;
  flow_fd = ut_dps_rx_socket(&flow_port);
  hdr_fd = ut_dps_rx_socket(&hdr_port);
  memcpy(entry->flow.dip_addr, loopback, MTL_IP_ADDR_LEN);
  entry->flow.dst_port = flow_port;
  if (flow_fd < 0 || hdr_fd < 0 ||
      tx_socket_init_thread_data(&entry->threads_data[0]) < 0) {
    sent = -ENODEV;
    goto out;
  }

  snprintf(pool_name, sizeof(pool_name), "ut_dps_pool_%u", pool_idx++);
  pool = rte_pktmbuf_pool_create(pool_name, 32, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                 rte_socket_id());
  if (!pool || rte_pktmbuf_alloc_bulk(pool, pkts, pkt_count) < 0) goto out;
  for (int i = 0; i < pkt_count; i++) {
    uint16_t len = sizeof(struct mt_udp_hdr) + UT_DPS_PAYLOAD_LEN;
    struct mt_udp_hdr* hdr = (struct mt_udp_hdr*)rte_pktmbuf_append(pkts[i], len);
    memset(hdr, 0, len);
    hdr->eth.ether_type = htons(RTE_ETHER_TYPE_IPV4);
    memcpy(&hdr->ipv4.dst_addr, loopback, MTL_IP_ADDR_LEN);
    hdr->udp.dst_port = htons(hdr_port);
  }

  sent = mt_tx_socket_burst(entry, pkts, pkt_count);
  for (int i = 0; i < sent; i++) pkts[i] = NULL; /* freed by the burst */
  *hdr_port_datagrams = ut_dps_count_datagrams(hdr_fd, pkt_count);
  *flow_port_datagrams = ut_dps_count_datagrams(flow_fd, pkt_count);

out:
  for (int i = 0; i < UT_DPS_MAX_PKTS; i++) rte_pktmbuf_free(pkts[i]);
  rte_mempool_free(pool);
  if (entry->threads_data[0].fd >= 0) close(entry->threads_data[0].fd);
  if (flow_fd >= 0) close(flow_fd);
  if (hdr_fd >= 0) close(hdr_fd);
  free(entry);
  free(impl);
  return sent;
}

int ut_dps_rx_source(uint16_t* sender_port, uint16_t* src_port, uint32_t* src_addr) {
  static unsigned int pool_idx;
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  struct mt_rx_socket_entry* entry = calloc(1, sizeof(*entry));
  struct mt_rx_socket_thread* t = entry ? &entry->threads_data[0] : NULL;
  struct sockaddr_in dst = {.sin_family = AF_INET,
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  char payload[UT_DPS_PAYLOAD_LEN] = {0};
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  struct rte_mbuf* pkt = NULL;
  struct mt_udp_hdr* hdr;
  uint16_t rx_port = 0;
  int tx_fd = -1, ret = -ENOMEM;

  if (entry) entry->fd = -1; /* before any goto out, which closes it */
  if (!impl || !entry) goto out;
  if (ut_eal_init() < 0) goto out;

  entry->parent = impl;
  entry->port = MTL_PORT_P;
  entry->pool_element_sz = UT_DPS_PAYLOAD_LEN;
  t->parent = entry;
  entry->fd = ut_dps_rx_socket(&rx_port);
  tx_fd = ut_dps_rx_socket(sender_port);
  if (entry->fd < 0 || tx_fd < 0) {
    ret = -ENODEV;
    goto out;
  }
  (void)snprintf(pool_name, sizeof(pool_name), "ut_dps_rx_pool_%u", pool_idx++);
  entry->pool = rte_pktmbuf_pool_create(pool_name, 32, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                        (int)rte_socket_id());
  if (!entry->pool) goto out;

  dst.sin_port = htons(rx_port);
  /* the receive timeout bounds the MSG_PEEK wait for the datagram, which
   * rx_socket_recv_mbuf() then reads with MSG_DONTWAIT */
  if (sendto(tx_fd, payload, sizeof(payload), 0, (struct sockaddr*)&dst, sizeof(dst)) !=
          sizeof(payload) ||
      recv(entry->fd, payload, sizeof(payload), MSG_PEEK) != sizeof(payload)) {
    ret = -EIO;
    goto out;
  }
  pkt = rx_socket_recv_mbuf(t);
  if (!pkt) {
    ret = -EIO;
    goto out;
  }
  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  *src_port = ntohs(hdr->udp.src_port);
  *src_addr = ntohl(hdr->ipv4.src_addr);
  ret = 0;

out:
  rte_pktmbuf_free(pkt);
  if (entry) rte_mempool_free(entry->pool);
  if (tx_fd >= 0) close(tx_fd);
  if (entry && entry->fd >= 0) close(entry->fd);
  free(entry);
  free(impl);
  return ret;
}
