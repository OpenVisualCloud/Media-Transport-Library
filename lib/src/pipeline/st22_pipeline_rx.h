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
  struct st_frame_meta src; /* before decoding */
  struct st_frame_meta dst; /* decoded */
  struct st22_decode_frame_meta decode_frame;
  uint16_t idx;
};

struct st22p_rx_ctx {
  struct st_main_impl* impl;
  int idx;
  enum st_session_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st22p_rx_ops ops;

  st22_rx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_decode_idx;
  uint16_t framebuff_consumer_idx;
  struct st22p_rx_frame* framebuffs;
  pthread_mutex_t lock;

  struct st22_decode_session_impl* decode_impl;
  bool ready;

  size_t dst_size;
  size_t max_codestream_size;

  rte_atomic32_t stat_decode_fail;
  rte_atomic32_t stat_busy;
};

#endif
