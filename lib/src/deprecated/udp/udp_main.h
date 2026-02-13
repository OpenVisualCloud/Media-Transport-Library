/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UDP_MAIN_H_
#define _MT_LIB_UDP_MAIN_H_

#include "../../mt_mcast.h"
#include "udp_rxq.h"

// clang-format off
#ifdef WINDOWSENV
#include "deprecated/mudp_win.h"
#endif
#include "deprecated/mudp_api.h"
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
/* if mcast init or not */
#define MUDP_MCAST_INIT (MTL_BIT32(2))
/* if tx mac is defined by user */
#define MUDP_TX_USER_MAC (MTL_BIT32(3))
/* if check bind address for RX */
#define MUDP_BIND_ADDRESS_CHECK (MTL_BIT32(4))

/* 1g */
#define MUDP_DEFAULT_RL_BPS (1ul * 1024 * 1024 * 1024)

#define MUDP_PREFIX "MU_"

struct mudp_impl {
  struct mtl_main_impl* parent;
  enum mt_handle_type type;
  int idx;
  bool alive;
  int (*user_dump)(void* priv);
  void* user_dump_priv;

  enum mtl_port port;
  struct mt_udp_hdr hdr;
  uint16_t bind_port;

  int fallback_fd; /* for MTL_PMD_KERNEL_SOCKET */

  uint64_t txq_bps; /* bit per sec for q */
  struct mt_txq_entry* txq;
  struct mur_client* rxq;
  unsigned int rx_ring_count;
  unsigned int rx_poll_sleep_us;
  struct rte_mempool* tx_pool;
  bool tx_pool_by_queue;
  uint16_t element_size;
  unsigned int element_nb;

  /* lcore mode info */
  /* wakeup when rte_ring_count(s->rx_ring) reach this threshold */
  unsigned int wake_thresh_count;
  /* wakeup when timeout with last wakeup */
  unsigned int wake_timeout_us;

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
  /* gso segment */
  size_t gso_segment_sz;
  /* if port is reused */
  int reuse_port;
  /* if address is reused */
  int reuse_addr;

  /* stat */
  /* do we need atomic here? atomic may impact the performance */
  uint32_t stat_pkt_build;
  uint32_t stat_pkt_arp_fail;
  uint32_t stat_pkt_tx;
  uint32_t stat_tx_gso_count;
  uint32_t stat_tx_retry;

  uint32_t stat_pkt_dequeue;
  uint32_t stat_pkt_deliver;
  uint32_t stat_poll_cnt;
  uint32_t stat_poll_succ_cnt;
  uint32_t stat_poll_timeout_cnt;
  uint32_t stat_poll_zero_timeout_cnt;
  uint32_t stat_poll_query_ret_cnt;
  uint32_t stat_rx_msg_cnt;
  uint32_t stat_rx_msg_succ_cnt;
  uint32_t stat_rx_msg_timeout_cnt;
  uint32_t stat_rx_msg_again_cnt;
};

int mudp_verify_socket_args(int domain, int type, int protocol);

int mudp_poll_query(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv);

#endif
