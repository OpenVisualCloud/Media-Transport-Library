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
  int stat_get_frame_try;
  int stat_get_frame_succ;
  int stat_put_frame;
  int stat_busy;
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
