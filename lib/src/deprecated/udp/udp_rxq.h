/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_UDP_RXQ_H_
#define _MT_LIB_UDP_RXQ_H_

#include "../../datapath/mt_queue.h"
#include "../../mt_main.h"
#include "../../mt_sch.h"
#include "../../mt_util.h"

#define MT_UDP_RXQ_PREFIX "UR_"

struct mur_queue;

struct mur_client {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  uint16_t dst_port;
  int idx;

  struct rte_ring* ring;
  struct mur_queue* q; /* the backend queue it attached */

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

  uint32_t stat_timedwait;
  uint32_t stat_timedwait_timeout;
  uint32_t stat_pkt_rx;
  uint32_t stat_pkt_rx_enq_fail;

  /* linked list for reuse port */
  MT_TAILQ_ENTRY(mur_client) next;
};

MT_TAILQ_HEAD(mur_client_list, mur_client);

/* support reuse port with load balancer */
struct mur_queue {
  struct mtl_main_impl* parent;
  enum mtl_port port;
  mt_atomic32_t refcnt;
  int client_idx;        /* incremental idx fort client */
  pthread_mutex_t mutex; /* clients lock */

  struct mt_rxq_entry* rxq;
  uint16_t rxq_id;
  uint16_t dst_port;
  uint16_t rx_burst_pkts;

  int reuse_port;
  struct mur_client_list client_head; /* client attached */
  int clients;                        /* how many clients connected */

  /* linked list */
  MT_TAILQ_ENTRY(mur_queue) next;
};

MT_TAILQ_HEAD(mur_queue_list, mur_queue);

struct mudp_rxq_mgr {
  struct mtl_main_impl* parent;
  enum mtl_port port;

  pthread_mutex_t mutex;
  struct mur_queue_list head;
};

struct mur_client_create {
  struct mtl_main_impl* impl;
  enum mtl_port port;
  uint16_t dst_port;
  unsigned int ring_count;
  unsigned int wake_thresh_count;
  unsigned int wake_timeout_us;
  int reuse_port;
};

int mur_client_put(struct mur_client* q);
struct mur_client* mur_client_get(struct mur_client_create* create);
uint16_t mur_client_rx(struct mur_client* q);
int mur_client_dump(struct mur_client* q);

int mur_client_timedwait(struct mur_client* cq, unsigned int timedwait_us,
                         unsigned int poll_sleep_us);

static inline struct rte_ring* mur_client_ring(struct mur_client* c) {
  return c->ring;
}

static inline int mur_client_set_wake_thresh(struct mur_client* c, unsigned int count) {
  c->wake_thresh_count = count;
  return 0;
}

static inline int mur_client_set_wake_timeout(struct mur_client* c, unsigned int us) {
  c->wake_timeout_us = us;
  return 0;
}

int mudp_rxq_init(struct mtl_main_impl* impl);
int mudp_rxq_uinit(struct mtl_main_impl* impl);

#endif
