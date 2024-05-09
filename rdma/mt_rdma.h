/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _MT_RDMA_HEAD_H_
#define _MT_RDMA_HEAD_H_

#include <infiniband/verbs.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mtl_rdma_api.h"

#define MT_RDMA_MSG_MAGIC (0x494D544C) /* ASCII representation of "IMTL" */

#define MT_RDMA_USER_META_MAX (1024 - sizeof(struct mt_rdma_message))

#define MT_SAFE_FREE(obj, free_fn) \
  do {                             \
    if (obj) {                     \
      free_fn(obj);                \
      obj = NULL;                  \
    }                              \
  } while (0)

enum mt_rdma_message_type {
  MT_RDMA_MSG_NONE = 0,
  MT_RDMA_MSG_BUFFER_READY,
  MT_RDMA_MSG_BUFFER_DONE,
  MT_RDMA_MSG_BYE,
  MT_RDMA_MSG_MAX,
};

struct mt_rdma_message {
  uint32_t magic;
  enum mt_rdma_message_type type;
  union {
    struct {
      uint16_t buf_idx;
      uint32_t seq_num;
    } buf_ready;
    struct {
      uint16_t buf_idx;
      uint64_t rx_buf_addr;
      uint32_t rx_buf_key;
      uint32_t seq_num;
    } buf_done;
  };
};

enum mt_rdma_buffer_status {
  MT_RDMA_BUFFER_STATUS_FREE, /* done */
  MT_RDMA_BUFFER_STATUS_IN_PRODUCTION,
  MT_RDMA_BUFFER_STATUS_IN_TRANSMISSION,
  MT_RDMA_BUFFER_STATUS_READY,
  MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION,
  MT_RDMA_BUFFER_STATUS_MAX,
};

struct mt_rdma_tx_buffer {
  uint16_t idx;
  enum mt_rdma_buffer_status status;
  struct mtl_rdma_buffer buffer;
  struct ibv_mr* mr;
  uint32_t remote_key;
  uint64_t remote_addr;
  uint32_t ref_count;
};

struct mt_rdma_tx_ctx {
  char ops_name[32];
  struct mtl_rdma_tx_ops ops;
  /* RDMA context */
  struct rdma_event_channel* ec;
  struct ibv_cq* cq;
  struct ibv_comp_channel* cc;
  struct rdma_cm_id* id;
  struct ibv_pd* pd;
  struct ibv_qp* qp;
  struct ibv_mr* message_mr;
  struct rdma_cm_id* listen_id;

  uint16_t buffer_producer_idx;
  uint32_t buffer_seq_num;
  void* message_region; /* 1024 bytes * buf_cnt */
  struct mt_rdma_tx_buffer* tx_buffers;
  uint16_t buffer_cnt;
  pthread_t connect_thread;
  pthread_t cq_poll_thread;

  atomic_bool connected;
  atomic_bool connect_stop;
  atomic_bool cq_poll_stop;

  uint64_t stat_buffer_sent;
  uint64_t stat_buffer_acked;
  uint64_t stat_buffer_error;
};

struct mt_rdma_rx_buffer {
  uint16_t idx;
  enum mt_rdma_buffer_status status;
  struct mtl_rdma_buffer buffer;
  struct ibv_mr* mr;
};

struct mt_rdma_rx_ctx {
  char ops_name[32];
  struct mtl_rdma_rx_ops ops;
  /* RDMA context */
  struct rdma_event_channel* ec;
  struct ibv_cq* cq;
  struct ibv_comp_channel* cc;
  struct rdma_cm_id* id;
  struct ibv_pd* pd;
  struct ibv_qp* qp;
  struct ibv_mr* message_mr;

  void* message_region; /* 1024 bytes * buf_cnt */
  struct mt_rdma_rx_buffer* rx_buffers;
  uint16_t buffer_cnt;
  pthread_t connect_thread;
  pthread_t cq_poll_thread;

  atomic_bool connected;
  atomic_bool connect_stop;
  atomic_bool cq_poll_stop;

  uint64_t stat_buffer_received;
  uint64_t stat_buffer_error;
};

struct mt_rdma_impl {
  int init;
};

#endif /* _MT_RDMA_HEAD_H_ */