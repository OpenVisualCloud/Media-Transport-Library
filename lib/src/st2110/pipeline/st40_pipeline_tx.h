/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef __ST40_PIPELINE_TX_H__
#define __ST40_PIPELINE_TX_H__

#include "../st_main.h"
#include "st40_pipeline_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum st40p_tx_frame_status {
  ST40P_TX_FRAME_FREE = 0,
  ST40P_TX_FRAME_IN_USER,         /* in user */
  ST40P_TX_FRAME_READY,           /* ready to transport */
  ST40P_TX_FRAME_IN_TRANSMITTING, /* for transport */
  ST40P_TX_FRAME_STATUS_MAX,
};

struct st40p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st40p_tx_ops ops;

  st40_tx_handle transport;
  uint16_t framebuff_cnt;
  uint32_t framebuff_seq_number;
  struct st40p_tx_frame* framebuffs;
  pthread_mutex_t lock;
  bool ready;

  int frames_per_sec;

  /* for ST40P_TX_FLAG_BLOCK_GET */
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

struct st40p_tx_frame {
  enum st40p_tx_frame_status stat;
  struct st40_frame_info frame_info;
  uint16_t idx;
  /** Pointer to the main ancillary frame buffer */
  struct st40_frame* anc_frame;
  uint32_t seq_number;
};

#if defined(__cplusplus)
}
#endif

#endif /* __ST40_PIPELINE_TX_H__ */
