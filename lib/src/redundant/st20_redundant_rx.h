/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_REDUNDANT_ST20_RX_HEAD_H_
#define _ST_LIB_REDUNDANT_ST20_RX_HEAD_H_

#include <st20_redundant_api.h>

#include "../st_main.h"

struct st20r_rx_ctx;

struct st20r_rx_transport {
  st20_rx_handle handle;
  enum st_port port; /* port this handle attached */
  struct st20r_rx_ctx* parnet;
};

struct st20r_rx_frame {
  void* frame;
  enum st_port port;
  struct st20_rx_frame_meta meta;
};

struct st20r_rx_ctx {
  struct st_main_impl* impl;
  int idx;
  enum st_session_type type; /* for sanity check, must be ST_SESSION_TYPE_RX_VIDEO_R */

  char ops_name[ST_MAX_NAME_LEN];
  struct st20r_rx_ops ops;

  pthread_mutex_t lock;
  bool ready;
  struct st20r_rx_transport* transport[ST_PORT_MAX];

  /* global status for current frame */
  void* cur_frame;
  uint64_t cur_timestamp;
  bool cur_frame_complete;
  /* the frames passed to user */
  struct st20r_rx_frame* frames;
  int frames_cnt;
};

#endif
