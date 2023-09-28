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
};

struct st22p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  enum mt_handle_type type; /* for sanity check */
  enum st_frame_fmt codestream_fmt;

  char ops_name[ST_MAX_NAME_LEN];
  struct st22p_tx_ops ops;

  st22_tx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_encode_idx;
  uint16_t framebuff_consumer_idx;
  struct st22p_tx_frame* framebuffs;
  pthread_mutex_t lock; /* protect framebuffs */

  struct st22_encode_session_impl* encode_impl;
  bool ready;
  bool ext_frame;

  size_t src_size;

  rte_atomic32_t stat_encode_fail;
};

#endif
