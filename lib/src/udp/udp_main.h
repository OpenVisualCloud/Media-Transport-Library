/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UDP_MAIN_H_
#define _MT_LIB_UDP_MAIN_H_

#include "../mt_dev.h"
#include "../mt_main.h"
#include "../mt_mcast.h"
#include "../mt_util.h"

/* if bind or not */
#define MUDP_BIND (MTL_BIT32(0))
/* if txq alloc or not */
#define MUDP_TXQ_ALLOC (MTL_BIT32(1))
/* if rxq alloc or not */
#define MUDP_RXQ_ALLOC (MTL_BIT32(2))
/* if mcast joined or not */
#define MUDP_RX_MCAST_JOINED (MTL_BIT32(3))

/* 50g */
#define MUDP_DEFAULT_RL_BPS (50ul * 1024 * 1024 * 1024)

struct mudp_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  int idx;

  enum mtl_port port;
  struct mt_udp_hdr hdr;
  uint16_t ipv4_packet_id;
  struct sockaddr_in bind_addr;

  uint64_t txq_bps; /* bit per sec for q */
  struct mt_tx_queue* txq;
  struct mt_rx_queue* rxq;
  struct rte_ring* rx_ring;
  uint16_t rx_burst_pkts;
  uint16_t rx_ring_thresh;
  struct rte_mempool* tx_pool;
  uint16_t element_size;
  unsigned int element_nb;

  int arp_timeout_ms;
  int tx_timeout_ms;
  int rx_timeout_ms;

  uint32_t flags;

  /* stat */
  /* do we need atomic here? atomic may impact the performance */
  int stat_pkt_build;
  int stat_pkt_tx;
  int stat_pkt_rx;
  int stat_pkt_deliver;
};

int mudp_verfiy_socket_args(int domain, int type, int protocol);

#endif
