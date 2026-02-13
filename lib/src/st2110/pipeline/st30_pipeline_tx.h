/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _ST_LIB_PIPELINE_ST30_TX_HEAD_H_
#define _ST_LIB_PIPELINE_ST30_TX_HEAD_H_

#include "../st_main.h"
#include "st30_pipeline_api.h"

enum st30p_tx_frame_status {
  ST30P_TX_FRAME_FREE = 0,
  ST30P_TX_FRAME_IN_USER,         /* in user */
  ST30P_TX_FRAME_READY,           /* ready to transport */
  ST30P_TX_FRAME_IN_TRANSMITTING, /* for transport */
  ST30P_TX_FRAME_STATUS_MAX,
};

struct st30p_tx_frame {
  enum st30p_tx_frame_status stat;
  struct st30_frame frame;
  uint16_t idx;
  uint32_t seq_number;
};

struct st30p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st30p_tx_ops ops;

  st30_tx_handle transport;
  uint16_t framebuff_cnt;
  uint32_t framebuff_seq_number;
  struct st30p_tx_frame* framebuffs;
  pthread_mutex_t lock;
  bool ready;

  /* usdt dump */
  int usdt_dump_fd;
  char usdt_dump_path[64];
  int usdt_dumped_frames;
  int frames_per_sec;

  /* for ST30P_TX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;

  /* get frame stat */
  uint32_t stat_get_frame_try;
  uint32_t stat_get_frame_succ;
  uint32_t stat_put_frame;
  uint32_t stat_drop_frame;
};

#endif
