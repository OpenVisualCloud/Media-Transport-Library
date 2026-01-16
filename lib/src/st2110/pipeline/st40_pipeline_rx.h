/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef __ST40_PIPELINE_RX_H__
#define __ST40_PIPELINE_RX_H__

#include "../st_main.h"
#include "st40_pipeline_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum st40p_rx_frame_status {
  ST40P_RX_FRAME_FREE = 0,
  ST40P_RX_FRAME_RECEIVING,
  ST40P_RX_FRAME_READY,
  ST40P_RX_FRAME_IN_USER,
  ST40P_RX_FRAME_STATUS_MAX,
};

struct st40p_rx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st40p_rx_ops ops;

  st40_rx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st40p_rx_frame* framebuffs;
  struct st40p_rx_frame* inflight_frame;
  uint32_t inflight_rtp_timestamp;
  bool last_seq_valid[MTL_SESSION_PORT_MAX];
  uint16_t last_seq[MTL_SESSION_PORT_MAX];
  pthread_mutex_t lock;
  bool ready;

  /* for ST40P_RX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;
  bool block_wake_pending;

  /* usdt dump */
  uint32_t usdt_dump_frame_cnt;

  /* stat */
  uint32_t stat_get_frame_try;
  uint32_t stat_get_frame_succ;
  uint32_t stat_put_frame;
  uint32_t stat_busy;
  uint32_t stat_drop_frame;
};

struct st40p_rx_frame {
  enum st40p_rx_frame_status stat;
  struct st40_frame_info frame_info;
  struct st40_meta meta[ST40_MAX_META];
  uint16_t idx;
};

#if defined(__cplusplus)
}
#endif

#endif /* __ST40_PIPELINE_RX_H__ */
