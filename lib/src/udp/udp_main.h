/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UDP_MAIN_H_
#define _MT_LIB_UDP_MAIN_H_

#include "../mt_dev.h"
#include "../mt_main.h"
#include "../mt_mcast.h"
#include "../mt_rss.h"
#include "../mt_sch.h"
#include "../mt_shared_queue.h"
#include "../mt_util.h"

// clang-format off
#ifdef WINDOWSENV
#include "mudp_win.h"
#endif
#include "mudp_api.h"
// clang-format on

/* On error, -1 is returned, and errno is set appropriately. */
#define MUDP_ERR_RET(code) \
  do {                     \
    errno = code;          \
    return -1;             \
  } while (0)

/* if bind or not */
#define MUDP_BIND (MTL_BIT32(0))
/* if txq alloc or not */
#define MUDP_TXQ_ALLOC (MTL_BIT32(1))
/* if rxq alloc or not */
#define MUDP_RXQ_ALLOC (MTL_BIT32(2))
/* if mcast init or not */
#define MUDP_MCAST_INIT (MTL_BIT32(3))
/* if tx mac is defined by user */
#define MUDP_TX_USER_MAC (MTL_BIT32(4))
/* if check bind address for RX */
#define MUDP_BIND_ADDRESS_CHECK (MTL_BIT32(5))

/* 1g */
#define MUDP_DEFAULT_RL_BPS (1ul * 1024 * 1024 * 1024)

struct mudp_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  int idx;
  char name[64];
  bool alive;
  int (*user_dump)(void* priv);
  void* user_dump_priv;

  enum mtl_port port;
  struct mt_udp_hdr hdr;
  uint16_t ipv4_packet_id;
  uint16_t bind_port;

  uint64_t txq_bps; /* bit per sec for q */
  struct mt_tx_queue* txq;
  struct mt_tsq_entry* tsq;
  struct mt_rx_queue* rxq;
  struct mt_rsq_entry* rsq;
  struct mt_rss_entry* rss;
  uint16_t rxq_id;
  struct rte_ring* rx_ring;
  unsigned int rx_ring_count;
  uint16_t rx_burst_pkts;
  unsigned int rx_poll_sleep_us;
  struct rte_mempool* tx_pool;
  uint16_t element_size;
  unsigned int element_nb;

  pthread_cond_t lcore_wake_cond;
  pthread_mutex_t lcore_wake_mutex;
  struct mt_sch_tasklet_impl* lcore_tasklet;
  /* bulk wakeup mode */
  /* wakeup when rte_ring_count(s->rx_ring) reach this threshold */
  unsigned int wake_thresh_count;
  /* wakeup when timeout with last wakeup */
  unsigned int wake_timeout_us;
  uint64_t wake_tsc_last;

  unsigned int arp_timeout_us;
  unsigned int msg_arp_timeout_us;
  unsigned int tx_timeout_us;
  unsigned int rx_timeout_us;
  uint8_t user_mac[MTL_MAC_ADDR_LEN];

  uint32_t* mcast_addrs;
  int mcast_addrs_nb;
  pthread_mutex_t mcast_addrs_mutex;

  uint32_t flags;

  /* send buffer size */
  uint32_t sndbuf_sz;
  /* receive buffer size */
  uint32_t rcvbuf_sz;
  /* cookie for SO_COOKIE */
  uint64_t cookie;

  /* stat */
  /* do we need atomic here? atomic may impact the performance */
  uint32_t stat_pkt_build;
  uint32_t stat_pkt_arp_fail;
  uint32_t stat_pkt_tx;
  uint32_t stat_tx_retry;
  uint32_t stat_pkt_rx;
  uint32_t stat_pkt_rx_enq_fail;
  uint32_t stat_pkt_deliver;
  uint32_t stat_timedwait;
  uint32_t stat_timedwait_timeout;
};

int mudp_verify_socket_args(int domain, int type, int protocol);

int mudp_poll_query(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv);

#endif
