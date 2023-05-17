/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_UDP_RXQ_H_
#define _MT_LIB_UDP_RXQ_H_

#include "../mt_dev.h"
#include "../mt_main.h"
#include "../mt_rss.h"
#include "../mt_sch.h"
#include "../mt_shared_queue.h"
#include "../mt_util.h"

/* support reuse port with load balancer */
struct mudp_rxq {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  char name[64];

  struct mt_rx_queue* rxq;
  struct mt_rsq_entry* rsq;
  struct mt_rss_entry* rss;
  uint16_t rxq_id;
  uint16_t dst_port;

  uint16_t rx_burst_pkts;
  struct rte_ring* rx_ring;

  /* lcore mode */
  pthread_cond_t lcore_wake_cond;
  pthread_mutex_t lcore_wake_mutex;
  struct mt_sch_tasklet_impl* lcore_tasklet;
  /* bulk wakeup mode */
  /* wakeup when rte_ring_count(s->rx_ring) reach this threshold */
  unsigned int wake_thresh_count;
  /* wakeup when timeout with last wakeup */
  unsigned int wake_timeout_us;
  uint64_t wake_tsc_last;

  uint32_t stat_pkt_rx_enq_fail;
};

struct mudp_rxq_create {
  struct mtl_main_impl* impl;
  enum mtl_port port;
  uint16_t dst_port;
  unsigned int ring_count;
  unsigned int wake_thresh_count;
  unsigned int wake_timeout_us;
};

int mudp_put_rxq(struct mudp_rxq* q);
struct mudp_rxq* mudp_get_rxq(struct mudp_rxq_create* create);
uint16_t mudp_rxq_rx(struct mudp_rxq* q);
int mudp_rxq_dump(struct mudp_rxq* q);

int mudp_rxq_timedwait_lcore(struct mudp_rxq* q, unsigned int us);
char* mudp_rxq_mode(struct mudp_rxq* q);

static inline struct rte_ring* mudp_rxq_ring(struct mudp_rxq* q) { return q->rx_ring; }

static inline bool mudp_rxq_lcore_mode(struct mudp_rxq* q) {
  return q->lcore_tasklet ? true : false;
}

static inline int mudp_rxq_set_wake_thresh(struct mudp_rxq* q, unsigned int count) {
  q->wake_thresh_count = count;
  return 0;
}

static inline int mudp_rxq_set_wake_timeout(struct mudp_rxq* q, unsigned int us) {
  q->wake_timeout_us = us;
  return 0;
}

#endif
