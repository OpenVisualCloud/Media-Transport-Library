/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_PIPELINE_ST22_RX_HEAD_H_
#define _ST_LIB_PIPELINE_ST22_RX_HEAD_H_

#include "../st_main.h"
#include "st_plugin.h"

enum st22p_rx_frame_status {
  ST22P_RX_FRAME_FREE = 0,
  ST22P_RX_FRAME_READY,       /* get from transport */
  ST22P_RX_FRAME_IN_DECODING, /* for encoding */
  ST22P_RX_FRAME_DECODED,
  ST22P_RX_FRAME_IN_USER, /* in user */
  ST22P_RX_FRAME_STATUS_MAX,
};

struct st22p_rx_frame {
  enum st22p_rx_frame_status stat;
  struct st_frame src; /* before decoding */
  struct st_frame dst; /* decoded */
  struct st22_decode_frame_meta decode_frame;
  uint16_t idx;
};

struct st22p_rx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */
  enum st_frame_fmt codestream_fmt;

  char ops_name[ST_MAX_NAME_LEN];
  struct st22p_rx_ops ops;

  st22_rx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_decode_idx;
  uint16_t framebuff_consumer_idx;
  struct st22p_rx_frame* framebuffs;
  pthread_mutex_t lock;

  /* for ST22P_RX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;

  struct st22_decode_session_impl* decode_impl;
  /* for ST22_DECODER_RESP_FLAG_BLOCK_GET */
  bool decode_block_get;
  pthread_cond_t decode_block_wake_cond;
  pthread_mutex_t decode_block_wake_mutex;
  uint64_t decode_block_timeout_ns;

  bool ready;
  bool derive; /* output_fmt == transport_fmt */
  bool ext_frame;
  int usdt_frame_cnt;

  size_t dst_size;
  size_t max_codestream_size;

  mt_atomic32_t stat_decode_fail;
  mt_atomic32_t stat_busy;
  /* get frame stat */
  uint32_t stat_get_frame_try;
  uint32_t stat_get_frame_succ;
  uint32_t stat_put_frame;
  uint32_t stat_drop_frame;
  uint32_t stat_decode_get_frame_try;
  uint32_t stat_decode_get_frame_succ;
  uint32_t stat_decode_put_frame;
};

#endif
