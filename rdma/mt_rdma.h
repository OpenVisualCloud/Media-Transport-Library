/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _MT_RDMA_HEAD_H_
#define _MT_RDMA_HEAD_H_

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mtl_rdma_api.h"

#define MT_RDMA_MSG_MAGIC (0x494D544C) /* ASCII representation of "IMTL" */

#define MT_RDMA_MSG_MAX_SIZE (1024)

#define MT_SAFE_FREE(obj, free_fn)                                             \
  do {                                                                         \
    if (obj) {                                                                 \
      free_fn(obj);                                                            \
      obj = NULL;                                                              \
    }                                                                          \
  } while (0)

void mt_rdma_set_log_level(enum mtl_rdma_log_level level);
enum mtl_rdma_log_level mt_rdma_get_log_level(void);

/* log define */
#ifdef DEBUG
#define dbg(...)                                                               \
  do {                                                                         \
    if (mt_rdma_get_log_level() <= MTL_RDMA_LOG_LEVEL_DEBUG)                   \
      printf(__VA_ARGS__);                                                     \
  } while (0)
#else
#define dbg(...)                                                               \
  do {                                                                         \
  } while (0)
#endif
#define info(...)                                                              \
  do {                                                                         \
    if (mt_rdma_get_log_level() <= MTL_RDMA_LOG_LEVEL_INFO)                    \
      printf(__VA_ARGS__);                                                     \
  } while (0)
#define notce(...)                                                             \
  do {                                                                         \
    if (mt_rdma_get_log_level() <= MTL_RDMA_LOG_LEVEL_NOTICE)                  \
      printf(__VA_ARGS__);                                                     \
  } while (0)
#define warn(...)                                                              \
  do {                                                                         \
    if (mt_rdma_get_log_level() <= MTL_RDMA_LOG_LEVEL_WARNING)                 \
      printf(__VA_ARGS__);                                                     \
  } while (0)
#define err(...)                                                               \
  do {                                                                         \
    if (mt_rdma_get_log_level() <= MTL_RDMA_LOG_LEVEL_ERR)                     \
      printf(__VA_ARGS__);                                                     \
  } while (0)
#define critical(...)                                                          \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)

/* Remote buffer info for RDMA write */
struct mt_rdma_remote_buffer {
  uint32_t remote_key;
  uint64_t remote_addr;
  uint32_t remote_meta_key;
  uint64_t remote_meta_addr;
};

/* Private data for RDMA CM handshake */
struct mt_rdma_connect_priv {
  uint16_t buf_cnt;
  size_t buf_capacity;
  bool dual_qp;
};

enum mt_rdma_message_type {
  MT_RDMA_MSG_NONE = 0,
  MT_RDMA_MSG_BUFFER_DONE,
  MT_RDMA_MSG_MAX,
};

struct mt_rdma_message {
  uint32_t magic;
  enum mt_rdma_message_type type;
  union {
    struct {
      uint16_t buf_idx;
      uint32_t seq_num;
      struct mt_rdma_remote_buffer remote_buffer;
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
  struct ibv_mr *mr;
  void *meta;
  struct mt_rdma_remote_buffer remote_buffer;
  uint32_t ref_count;
  pthread_mutex_t lock;
};

struct mt_rdma_tx_ctx {
  char ops_name[32];
  struct mtl_rdma_tx_ops ops;
  /* RDMA context */
  struct rdma_event_channel *ec;
  struct ibv_cq *cq;
  struct ibv_comp_channel *cc;
  struct rdma_cm_id *id;
  struct ibv_pd *pd;
  struct ibv_qp *qp;
  struct ibv_mr *meta_mr;
  struct ibv_mr *recv_msgs_mr;
  struct rdma_cm_id *listen_id;

  uint32_t buffer_seq_num;
  void *meta_region; /* 1024 bytes * buf_cnt */
  struct mt_rdma_message *recv_msgs;
  struct mt_rdma_tx_buffer *tx_buffers;
  uint16_t buffer_cnt;
  pthread_t connect_thread;
  pthread_t cq_poll_thread;
  bool cq_poll_only;

  atomic_bool connected;
  atomic_bool connect_stop;
  atomic_bool cq_poll_stop;

  uint64_t stat_buffer_sent;
  uint64_t stat_buffer_acked;
  uint64_t stat_buffer_error;
  uint64_t stat_cq_poll_done;
  uint64_t stat_cq_poll_empty;
};

struct mt_rdma_rx_buffer {
  uint16_t idx;
  enum mt_rdma_buffer_status status;
  struct mtl_rdma_buffer buffer;
  struct ibv_mr *mr;
  pthread_mutex_t lock;
  uint8_t recv_mask;
};

struct mt_rdma_rx_ctx {
  char ops_name[32];
  struct mtl_rdma_rx_ops ops;
  /* RDMA context */
  struct rdma_event_channel *ec;
  struct ibv_cq *cq;
  struct ibv_comp_channel *cc;
  struct rdma_cm_id *id;
  struct ibv_pd *pd;
  struct ibv_qp *qp;
  struct ibv_mr *meta_mr;
  struct ibv_mr *recv_msgs_mr;

  void *meta_region; /* 1024 bytes * buf_cnt */
  struct mt_rdma_message *recv_msgs;
  struct mt_rdma_rx_buffer *rx_buffers;
  uint16_t buffer_cnt;
  pthread_t connect_thread;
  pthread_t cq_poll_thread;
  bool cq_poll_only;

  atomic_bool connected;
  atomic_bool connect_stop;
  atomic_bool cq_poll_stop;

  uint64_t stat_buffer_received;
  uint64_t stat_buffer_error;
  uint64_t stat_cq_poll_done;
  uint64_t stat_cq_poll_empty;
};

struct mt_rdma_impl {
  int init;
  struct mtl_rdma_init_params para;
};

static inline struct mtl_rdma_init_params *
mt_rdma_get_params(struct mt_rdma_impl *impl) {
  return &impl->para;
}

static inline bool mt_rdma_low_latency(struct mt_rdma_impl *impl) {
  if (mt_rdma_get_params(impl)->flags & MTL_RDMA_FLAG_LOW_LATENCY)
    return true;
  else
    return false;
}

static inline int mt_rdma_handle_cq_events(struct ibv_comp_channel *cc,
                                           struct ibv_cq *cq) {
  void *cq_ctx = NULL;
  int ret = ibv_get_cq_event(cc, &cq, &cq_ctx);
  if (ret) {
    err("%s, ibv_get_cq_event failed\n", __func__);
    return -EIO;
  }
  ibv_ack_cq_events(cq, 1);
  ret = ibv_req_notify_cq(cq, 0);
  if (ret) {
    err("%s, ibv_req_notify_cq failed\n", __func__);
    return -EIO;
  }
  return 0;
}

static inline int mt_rdma_post_write_imm(struct rdma_cm_id *id, void *context,
                                         void *addr, size_t length,
                                         struct ibv_mr *mr, int flags,
                                         uint64_t remote_addr, uint32_t rkey,
                                         uint32_t imm_data) {
  struct ibv_send_wr wr, *bad_wr;

  struct ibv_sge sge = {
      .addr = (uint64_t)addr,
      .length = length,
      .lkey = mr->lkey,
  };

  wr.wr_id = (uint64_t)context;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  wr.send_flags = flags;
  wr.imm_data = imm_data;
  wr.wr.rdma.remote_addr = remote_addr;
  wr.wr.rdma.rkey = rkey;

  return ibv_post_send(id->qp, &wr, &bad_wr);
}

#endif /* _MT_RDMA_HEAD_H_ */