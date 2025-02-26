/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * st2110-20 combined redundant transport, experimental feature only.
 */

#ifndef _ST_LIB_REDUNDANT_COMBINED_ST20_RX_HEAD_H_
#define _ST_LIB_REDUNDANT_COMBINED_ST20_RX_HEAD_H_

#include <experimental/st20_combined_api.h>

#include "../st_main.h"

struct st20rc_rx_ctx;

struct st20rc_rx_transport {
  st20_rx_handle handle;
  enum mtl_session_port port; /* port this handle attached */
  struct st20rc_rx_ctx* parent;
};

struct st20rc_rx_frame {
  void* frame;
  enum mtl_session_port port;
  struct st20_rx_frame_meta meta;
};

struct st20rc_rx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  enum mt_handle_type type; /* for sanity check, must be MT_HANDLE_RX_VIDEO_R */

  char ops_name[ST_MAX_NAME_LEN];
  struct st20rc_rx_ops ops;

  pthread_mutex_t lock;
  bool ready;
  struct st20rc_rx_transport* transport[MTL_SESSION_PORT_MAX];

  /* global status for current frame */
  void* cur_frame;
  uint64_t cur_timestamp;
  bool cur_frame_complete;
  /* the frames passed to user */
  struct st20rc_rx_frame* frames;
  int frames_cnt;
};

#endif
