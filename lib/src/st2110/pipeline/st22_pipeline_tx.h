/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_PIPELINE_ST22_TX_HEAD_H_
#define _ST_LIB_PIPELINE_ST22_TX_HEAD_H_

#include "../st_main.h"
#include "st_plugin.h"

enum st22p_tx_frame_status {
  ST22P_TX_FRAME_FREE = 0,
  ST22P_TX_FRAME_IN_USER,
  ST22P_TX_FRAME_READY,
  ST22P_TX_FRAME_IN_ENCODING, /* for encoding */
  ST22P_TX_FRAME_ENCODED,
  ST22P_TX_FRAME_IN_TRANSMITTING, /* for transport */
  ST22P_TX_FRAME_STATUS_MAX,
};

struct st22p_tx_frame {
  enum st22p_tx_frame_status stat;
  struct st_frame src; /* before encoding */
  struct st_frame dst; /* encoded */
  struct st22_encode_frame_meta encode_frame;
  uint16_t idx;
  uint32_t seq_number;
};

struct st22p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */
  enum st_frame_fmt codestream_fmt;

  char ops_name[ST_MAX_NAME_LEN];
  struct st22p_tx_ops ops;

  st22_tx_handle transport;
  uint16_t framebuff_cnt;
  uint32_t framebuff_sequence_number;
  struct st22p_tx_frame* framebuffs;
  pthread_mutex_t lock; /* protect framebuffs */

  /* for ST22P_TX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;

  struct st22_encode_session_impl* encode_impl;
  /* for ST22_ENCODER_RESP_FLAG_BLOCK_GET */
  bool encode_block_get;
  pthread_cond_t encode_block_wake_cond;
  pthread_mutex_t encode_block_wake_mutex;
  uint64_t encode_block_timeout_ns;

  bool ready;
  bool derive; /* input_fmt == transport_fmt */
  bool ext_frame;
  bool second_field;
  int usdt_frame_cnt;

  size_t src_size;

  rte_atomic32_t stat_encode_fail;
  /* get frame stat */
  uint32_t stat_get_frame_try;
  uint32_t stat_get_frame_succ;
  uint32_t stat_put_frame;
  uint32_t stat_drop_frame;
  /* get frame stat */
  uint32_t stat_encode_get_frame_try;
  uint32_t stat_encode_get_frame_succ;
  uint32_t stat_encode_put_frame;
};

#endif
