/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#undef MTL_HAS_USDT
#include "pipeline/st_plugin_harness.h"

#include <dlfcn.h>

#include "common/ut_common.h"

static int ut_dlopen_calls;
static int ut_dl_lib;

static void* ut_dlopen(const char* file, int mode) {
  (void)file;
  (void)mode;
  ut_dlopen_calls++;
  return &ut_dl_lib;
}

static void* ut_dlsym(void* handle, const char* symbol);

static int ut_dlclose(void* handle) {
  (void)handle;
  return 0;
}

#define dlopen ut_dlopen
#define dlsym ut_dlsym
#define dlclose ut_dlclose
#include "st2110/pipeline/st_plugin.c"
#undef dlclose
#undef dlsym
#undef dlopen

static struct st22_encode_dev_impl ut_encoder;
static struct st22_decode_dev_impl ut_decoder;
static struct st20_convert_dev_impl ut_converter;
static bool ut_fail_create;
static void* ut_frame_in_create;

static void* ut_create_session(enum ut_plugin_kind kind, void* session_p) {
  ut_frame_in_create = ut_plugin_get_frame(kind, session_p);
  return ut_fail_create ? NULL : session_p;
}

static st22_encode_priv ut_encoder_create_session(void* priv,
                                                  st22p_encode_session session_p,
                                                  struct st22_encoder_create_req* req) {
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_create_session(UT_PLUGIN_ENCODER, session_p);
}

static st22_decode_priv ut_decoder_create_session(void* priv,
                                                  st22p_decode_session session_p,
                                                  struct st22_decoder_create_req* req) {
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_create_session(UT_PLUGIN_DECODER, session_p);
}

static st20_convert_priv ut_converter_create_session(
    void* priv, st20p_convert_session session_p, struct st20_converter_create_req* req) {
  return ut_create_session(UT_PLUGIN_CONVERTER, session_p);
}

static struct st22_encode_frame_meta* ut_encode_get_frame(void* priv) {
  return priv;
}

static struct st22_decode_frame_meta* ut_decode_get_frame(void* priv) {
  return priv;
}

static struct st20_convert_frame_meta* ut_convert_get_frame(void* priv) {
  return priv;
}

void ut_plugin_reset(void) {
  memset(&ut_encoder, 0, sizeof(ut_encoder));
  memset(&ut_decoder, 0, sizeof(ut_decoder));
  memset(&ut_converter, 0, sizeof(ut_converter));
  ut_encoder.dev.name = "ut_encoder";
  ut_encoder.dev.create_session = ut_encoder_create_session;
  ut_decoder.dev.name = "ut_decoder";
  ut_decoder.dev.create_session = ut_decoder_create_session;
  ut_converter.dev.name = "ut_converter";
  ut_converter.dev.create_session = ut_converter_create_session;
  ut_fail_create = false;
  ut_frame_in_create = NULL;
}

void ut_plugin_fail_create(void) {
  ut_fail_create = true;
}

void* ut_plugin_get(enum ut_plugin_kind kind, void* frame) {
  switch (kind) {
    case UT_PLUGIN_ENCODER: {
      struct st22_get_encoder_request req = {.priv = frame,
                                             .get_frame = ut_encode_get_frame};
      return st22_get_encoder_session(&ut_encoder, &req);
    }
    case UT_PLUGIN_DECODER: {
      struct st22_get_decoder_request req = {.priv = frame,
                                             .get_frame = ut_decode_get_frame};
      return st22_get_decoder_session(&ut_decoder, &req);
    }
    case UT_PLUGIN_CONVERTER: {
      struct st20_get_converter_request req = {.priv = frame,
                                               .get_frame = ut_convert_get_frame};
      return st20_get_converter_session(&ut_converter, &req);
    }
  }
  return NULL;
}

void ut_plugin_put(enum ut_plugin_kind kind, void* session) {
  switch (kind) {
    case UT_PLUGIN_ENCODER:
      ((struct st22_encode_session_impl*)session)->session = NULL;
      break;
    case UT_PLUGIN_DECODER:
      ((struct st22_decode_session_impl*)session)->session = NULL;
      break;
    case UT_PLUGIN_CONVERTER:
      ((struct st20_convert_session_impl*)session)->session = NULL;
      break;
  }
}

void* ut_plugin_get_frame(enum ut_plugin_kind kind, void* session) {
  switch (kind) {
    case UT_PLUGIN_ENCODER:
      return st22_encoder_get_frame(session);
    case UT_PLUGIN_DECODER:
      return st22_decoder_get_frame(session);
    case UT_PLUGIN_CONVERTER:
      return st20_converter_get_frame(session);
  }
  return NULL;
}

void* ut_plugin_frame_in_create(void) {
  return ut_frame_in_create;
}

unsigned int ut_plugin_resp_flag(enum ut_plugin_kind kind, void* session) {
  if (kind == UT_PLUGIN_ENCODER)
    return ((struct st22_encode_session_impl*)session)->req.req.resp_flag;
  return ((struct st22_decode_session_impl*)session)->req.req.resp_flag;
}

static int ut_dl_get_meta(struct st_plugin_meta* meta) {
  meta->version = ST_PLUGIN_VERSION_V1;
  meta->magic = ST_PLUGIN_VERSION_V1_MAGIC;
  return 0;
}

static st_plugin_priv ut_dl_create(mtl_handle mt) {
  (void)mt;
  return &ut_dl_lib;
}

static int ut_dl_free(st_plugin_priv handle) {
  (void)handle;
  return 0;
}

static void* ut_dlsym(void* handle, const char* symbol) {
  (void)handle;
  if (!strcmp(symbol, ST_PLUGIN_GET_META_API)) return (void*)ut_dl_get_meta;
  if (!strcmp(symbol, ST_PLUGIN_CREATE_API)) return (void*)ut_dl_create;
  if (!strcmp(symbol, ST_PLUGIN_FREE_API)) return (void*)ut_dl_free;
  return NULL;
}

static struct mtl_main_impl ut_dl_impl;

mtl_handle ut_plugin_dl_reset(void) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(&ut_dl_impl);
  static bool locks_ready;

  if (ut_eal_init() < 0) return NULL;
  if (!locks_ready) {
    mt_pthread_mutex_init(&mgr->plugins_lock, NULL);
    locks_ready = true;
  }
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    if (mgr->plugins[i]) st_plugin_free(mgr->plugins[i]);
    mgr->plugins[i] = NULL;
  }
  mgr->plugins_nb = 0;
  ut_dl_impl.type = MT_HANDLE_MAIN;
  ut_dl_impl.inf[MTL_PORT_P].socket_id = (int)rte_socket_id();
  ut_dlopen_calls = 0;
  return &ut_dl_impl;
}

int ut_plugin_dlopen_calls(void) {
  return ut_dlopen_calls;
}
