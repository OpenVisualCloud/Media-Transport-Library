/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _CONVERT_PLUGIN_SAMPLE_H_
#define _CONVERT_PLUGIN_SAMPLE_H_

#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>

#define MAX_COLOR_CONVERT_SESSIONS (8)

struct converter_session {
  int idx;

  struct st20_converter_create_req req;
  st20p_convert_session session_p;
  bool stop;
  pthread_t convert_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int frame_cnt;
};

struct convert_ctx {
  st20_converter_dev_handle converter_dev_handle;
  struct converter_session *converter_sessions[MAX_COLOR_CONVERT_SESSIONS];
};

/* the APIs for plugin */
int st_plugin_get_meta(struct st_plugin_meta *meta);
st_plugin_priv st_plugin_create(mtl_handle st);
int st_plugin_free(st_plugin_priv handle);

#endif
