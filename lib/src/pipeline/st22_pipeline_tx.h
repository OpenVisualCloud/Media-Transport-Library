/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
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
  struct st_frame_meta src; /* before encoding */
  struct st_frame_meta dst; /* encoded */
  struct st22_encode_frame_meta encode_frame;
  uint16_t idx;
};

struct st22p_tx_ctx {
  struct st_main_impl* impl;
  int idx;
  enum st_session_type type; /* for sanity check */

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

  size_t src_size;

  rte_atomic32_t stat_encode_fail;
};

#endif
