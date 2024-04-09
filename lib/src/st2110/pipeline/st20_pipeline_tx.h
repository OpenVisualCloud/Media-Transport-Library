/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_PIPELINE_ST20_TX_HEAD_H_
#define _ST_LIB_PIPELINE_ST20_TX_HEAD_H_

#include "../st_main.h"
#include "st_plugin.h"

enum st20p_tx_frame_status {
  ST20P_TX_FRAME_FREE = 0,
  ST20P_TX_FRAME_READY,
  ST20P_TX_FRAME_IN_CONVERTING, /* for converting */
  ST20P_TX_FRAME_CONVERTED,
  ST20P_TX_FRAME_IN_USER,         /* in user */
  ST20P_TX_FRAME_IN_TRANSMITTING, /* for transport */
  ST20P_TX_FRAME_STATUS_MAX,
};

struct st20p_tx_frame {
  enum st20p_tx_frame_status stat;
  struct st_frame src; /* before converting */
  struct st_frame dst; /* converted */
  struct st20_convert_frame_meta convert_frame;
  uint16_t idx;
  void* user_meta; /* the meta data from user */
  size_t user_meta_buffer_size;
  size_t user_meta_data_size;
};

struct st20p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st20p_tx_ops ops;

  st20_tx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_convert_idx;
  uint16_t framebuff_consumer_idx;
  struct st20p_tx_frame* framebuffs;
  pthread_mutex_t lock;
  int usdt_frame_cnt;

  struct st20_convert_session_impl* convert_impl;
  struct st_frame_converter* internal_converter;
  bool ready;
  bool derive; /* input_fmt == transport_fmt */

  size_t src_size;

  bool second_field;

  /* for ST20P_TX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;

  rte_atomic32_t stat_convert_fail;
  rte_atomic32_t stat_busy;
  /* get frame stat */
  int stat_get_frame_try;
  int stat_get_frame_succ;
  int stat_put_frame;
};

#endif
