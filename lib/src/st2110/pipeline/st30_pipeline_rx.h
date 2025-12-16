/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _ST_LIB_PIPELINE_ST30_RX_HEAD_H_
#define _ST_LIB_PIPELINE_ST30_RX_HEAD_H_

#include "../st_main.h"
#include "st30_pipeline_api.h"

enum st30p_rx_frame_status {
  ST30P_RX_FRAME_FREE = 0,
  ST30P_RX_FRAME_READY,   /* get from transport */
  ST30P_RX_FRAME_IN_USER, /* in user */
  ST30P_RX_FRAME_STATUS_MAX,
};

struct st30p_rx_frame {
  enum st30p_rx_frame_status stat;
  struct st30_frame frame;
  uint16_t idx;
};

struct st30p_rx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st30p_rx_ops ops;

  st30_rx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st30p_rx_frame* framebuffs;
  pthread_mutex_t lock;
  bool ready;

  /* usdt dump */
  int usdt_dump_fd;
  char usdt_dump_path[64];
  int usdt_dumped_frames;
  int frames_per_sec;

  /* for ST30P_RX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;
  bool block_wake_pending;

  /* get frame stat */
  uint32_t stat_get_frame_try;
  uint32_t stat_get_frame_succ;
  uint32_t stat_put_frame;
  uint32_t stat_busy;
};

#endif
