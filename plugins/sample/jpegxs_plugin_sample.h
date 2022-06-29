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

#ifndef _JPEGXS_PLUGIN_SAMPLE_H_
#define _JPEGXS_PLUGIN_SAMPLE_H_

#include <st_pipeline_api.h>

#define MAX_SAMPLE_ENCODER_SESSIONS (8)
#define MAX_SAMPLE_DECODER_SESSIONS (8)

struct jpegxs_encoder_session {
  int idx;

  struct st22_encoder_create_req req;
  st22p_encode_session session_p;
  bool stop;
  pthread_t encode_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int frame_cnt;
};

struct jpegxs_decoder_session {
  int idx;

  struct st22_decoder_create_req req;
  st22p_decode_session session_p;
  bool stop;
  pthread_t decode_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int frame_cnt;
};

struct jpegxs_sample_ctx {
  st22_encoder_dev_handle encoder_dev_handle;
  st22_decoder_dev_handle decoder_dev_handle;
  struct jpegxs_encoder_session* encoder_sessions[MAX_SAMPLE_ENCODER_SESSIONS];
  struct jpegxs_decoder_session* decoder_sessions[MAX_SAMPLE_DECODER_SESSIONS];
};

/* the APIs for plugin */
int st_plugin_get_meta(struct st_plugin_meta* meta);
st_plugin_priv st_plugin_create(st_handle st);
int st_plugin_free(st_plugin_priv handle);

#endif
