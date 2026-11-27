/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Plugin .so for the unit tests: registers one encoder dev and unregisters it on free.
 * UT_PLUGIN_KEEP_DEV leaves the dev registered on free, UT_PLUGIN_FAIL_CREATE fails
 * create after registering it.
 */

#include <st_pipeline_api.h>
#include <stdlib.h>

static st22_encoder_dev_handle ut_dev;
static int ut_ctx;

static st22_encode_priv ut_create_session(void* priv, st22p_encode_session session_p,
                                          struct st22_encoder_create_req* req) {
  (void)priv;
  (void)req;
  return session_p;
}

static int ut_free_session(void* priv, st22_encode_priv session) {
  (void)priv;
  (void)session;
  return 0;
}

st_plugin_priv st_plugin_create(mtl_handle mt) {
  struct st22_encoder_dev dev = {
      .name = "ut_st22_plugin",
      .target_device = ST_PLUGIN_DEVICE_CPU,
      .input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE,
      .output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM,
      .create_session = ut_create_session,
      .free_session = ut_free_session,
  };

  ut_dev = st22_encoder_register(mt, &dev);
#ifdef UT_PLUGIN_FAIL_CREATE
  return NULL;
#endif
  return ut_dev ? &ut_ctx : NULL;
}

int st_plugin_free(st_plugin_priv handle) {
  (void)handle;
#ifndef UT_PLUGIN_KEEP_DEV
  st22_encoder_unregister(ut_dev);
#endif
  return 0;
}

int st_plugin_get_meta(struct st_plugin_meta* meta) {
  meta->version = ST_PLUGIN_VERSION_V1;
  meta->magic = ST_PLUGIN_VERSION_V1_MAGIC;
  return 0;
}
