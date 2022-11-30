/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UDP_MAIN_H_
#define _MT_LIB_UDP_MAIN_H_

#include "../mt_dev.h"
#include "../mt_main.h"
#include "../mt_util.h"

#define MUDP_BIND (MTL_BIT32(0))      /* if bind or not */
#define MUDP_TXQ_ALLOC (MTL_BIT32(1)) /* if txq alloc or not */
#define MUDP_RXQ_ALLOC (MTL_BIT32(2)) /* if rxq alloc or not */

/* 50g */
#define MUDP_DEFAULT_RL_BPS (50ul * 1024 * 1024 * 1024 / 8)

struct mudp_impl {
  struct mtl_main_impl* parnet;
  enum mt_handle_type type;
  int idx;

  enum mtl_port port;
  struct mt_udp_hdr hdr;
  uint16_t ipv4_packet_id;
  struct sockaddr_in bind_addr;

  uint64_t txq_bps; /* bytes per sec for q */
  struct mt_tx_queue* txq;
  struct mt_rx_queue* rxq;
  uint16_t rx_burst_pkts;
  struct rte_mempool* tx_pool;
  uint16_t element_size;
  unsigned int element_nb;

  int arp_timeout_ms;
  int tx_timeout_ms;

  uint32_t flags;
};

#endif
